/*
 * main.c - FR8: command-line interface for CampusCompute.
 *
 * Every feature is a separate sub-command so it can be tested on its own:
 *
 *   campuscompute schedule fcfs|rr|priority|compare [job options] [--quantum N]
 *   campuscompute logger  [--capacity N] [--producers N] [--messages N] ...
 *   campuscompute ipc     [job options] [--timeout S] [--simulate-consumer-crash]
 *   campuscompute banker  [--demo safe|unsafe | --file PATH]
 *   campuscompute worker  normal|fail-exit|fail-signal|pool
 *   campuscompute limits  fsize|nofile|cpu|all
 *   campuscompute demo | cleanup | help | version
 *
 * Exit status: 0 = success, 1 = the command ran but a check failed,
 *              2 = bad command line or malformed input.
 */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "banker.h"
#include "ipc.h"
#include "job_manager.h"
#include "logger.h"
#include "resource_limits.h"
#include "scheduler.h"
#include "util.h"
#include "worker.h"

#define PROG "campuscompute"
#define DEMO_LOGGER_PATH "logs/demo_producer_consumer.log"
#define MAX_STAGES 16

/* ------------------------------------------------------------------ */
/* usage                                                               */
/* ------------------------------------------------------------------ */

static void print_usage(FILE *out)
{
    fprintf(out,
"CampusCompute %s - job scheduling and resource management prototype\n"
"\n"
"Usage: " PROG " <command> [options]\n"
"\n"
"Commands:\n"
"  schedule <fcfs|rr|priority|compare> [job options] [--quantum N]\n"
"        CPU scheduling (FR1/FR2). 'compare' runs all three on the same jobs.\n"
"        --quantum N   Round Robin time quantum, %d..%d (default %d)\n"
"  logger [--capacity N] [--producers N] [--messages N]\n"
"         [--producer-delay MS] [--consumer-delay MS] [--log PATH] [--quiet]\n"
"        Bounded producer-consumer logger with POSIX threads (FR3).\n"
"  ipc [job options] [--timeout S] [--simulate-consumer-crash]\n"
"        Shared memory + named semaphores between two processes (FR4).\n"
"  banker [--demo safe|unsafe | --file PATH]\n"
"        Banker's safety algorithm (FR5). No option: run both demo states.\n"
"  worker <normal|fail-exit|fail-signal|pool>\n"
"        fork()/waitpid() failure detection (FR6). Failures are SIMULATED.\n"
"  limits <fsize|nofile|cpu|all>\n"
"        setrlimit() demonstrations (FR7).\n"
"  demo      Run every feature in turn and print a summary (integrated run).\n"
"  cleanup   Remove this program's leftover POSIX shm/semaphore objects.\n"
"  help      Show this text.     version   Show the version.\n"
"\n"
"Job options (schedule, ipc):\n"
"  --sample              built-in 6-job demonstration set (default)\n"
"  --file PATH           load jobs: one \"ID ARRIVAL BURST PRIORITY\" per line\n"
"  --job ID,ARR,BURST,PRIO   add one job (repeatable)\n"
"  Ranges: id %d..%d, arrival 0..%d, burst %d..%d, priority %d..%d\n"
"  (1 = most urgent), at most %d jobs.\n"
"\n"
"Exit status: 0 success, 1 a check failed, 2 usage / malformed input.\n"
"Examples:\n"
"  " PROG " schedule rr --quantum 2 --job 1,0,5,1 --job 2,1,3,2\n"
"  " PROG " banker --file tests/data/banker_unsafe.txt\n",
            CC_VERSION, RR_MIN_QUANTUM, RR_MAX_QUANTUM, RR_DEFAULT_QUANTUM,
            JM_MIN_ID, JM_MAX_ID, JM_MAX_ARRIVAL, JM_MIN_BURST, JM_MAX_BURST,
            JM_MIN_PRIORITY, JM_MAX_PRIORITY, JM_MAX_JOBS);
}

static int usage_error(const char *cmd, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static int usage_error(const char *cmd, const char *fmt, ...)
{
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    cc_errf("%s", msg);
    fprintf(stderr, "Run '" PROG " help' to see the usage%s%s%s.\n",
            cmd ? " of '" : "", cmd ? cmd : "", cmd ? "'" : "");
    return CC_EXIT_USAGE;
}

/* ------------------------------------------------------------------ */
/* option helpers                                                      */
/* ------------------------------------------------------------------ */

/*
 * Read the value of option argv[*i] ("--name VALUE"), advancing *i. Returns 0
 * or -1 after printing an error.
 */
static int opt_value(int argc, char **argv, int *i, const char **val)
{
    if (*i + 1 >= argc) {
        cc_errf("option %s needs a value", argv[*i]);
        return -1;
    }
    *val = argv[++*i];
    return 0;
}

static int opt_long(int argc, char **argv, int *i, long min, long max,
                    long *out)
{
    const char *name = argv[*i], *val;
    int rc;

    if (opt_value(argc, argv, i, &val) != 0)
        return -1;
    rc = cc_parse_long(val, min, max, out);
    if (rc == -1) {
        cc_errf("option %s: '%s' is not a whole number", name, val);
        return -1;
    }
    if (rc == -2) {
        cc_errf("option %s: %s is out of range (%ld..%ld)", name, val, min,
                max);
        return -1;
    }
    return 0;
}

/*
 * Common job-set options. Returns 1 if argv[*i] was a job option (consumed),
 * 0 if it is not one, -1 on error (message printed).
 */
typedef struct {
    JobSet jobs;
    bool explicit_jobs;     /* --file / --job given: do not load the sample */
    bool want_sample;
} JobArgs;

static int job_option(int argc, char **argv, int *i, JobArgs *ja)
{
    const char *val;
    char err[160];

    if (strcmp(argv[*i], "--sample") == 0) {
        ja->want_sample = true;
        return 1;
    }
    if (strcmp(argv[*i], "--file") == 0) {
        if (opt_value(argc, argv, i, &val) != 0)
            return -1;
        if (jobset_load_file(&ja->jobs, val, err, sizeof err) != 0) {
            cc_errf("%s", err);
            return -1;
        }
        ja->explicit_jobs = true;
        return 1;
    }
    if (strcmp(argv[*i], "--job") == 0) {
        if (opt_value(argc, argv, i, &val) != 0)
            return -1;
        if (jobset_add_spec(&ja->jobs, val, err, sizeof err) != 0) {
            cc_errf("--job '%s': %s", val, err);
            return -1;
        }
        ja->explicit_jobs = true;
        return 1;
    }
    return 0;
}

static void job_args_init(JobArgs *ja)
{
    jobset_init(&ja->jobs);
    ja->explicit_jobs = false;
    ja->want_sample = false;
}

/* After option parsing: fall back to the sample set when nothing was given. */
static int job_args_finish(JobArgs *ja)
{
    if (ja->explicit_jobs && ja->want_sample) {
        cc_errf("--sample cannot be combined with --file/--job");
        return -1;
    }
    if (!ja->explicit_jobs)
        jobset_load_sample(&ja->jobs);
    return 0;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static int cmd_schedule(int argc, char **argv)
{
    JobArgs ja;
    long quantum = RR_DEFAULT_QUANTUM;
    const char *which;
    int rc = CC_EXIT_OK;

    if (argc < 3)
        return usage_error("schedule", "schedule needs one of: fcfs, rr, "
                           "priority, compare");
    which = argv[2];
    if (strcmp(which, "fcfs") && strcmp(which, "rr") &&
        strcmp(which, "priority") && strcmp(which, "compare"))
        return usage_error("schedule", "unknown scheduling policy '%s' "
                           "(use fcfs, rr, priority or compare)", which);

    job_args_init(&ja);
    for (int i = 3; i < argc; i++) {
        int r = job_option(argc, argv, &i, &ja);

        if (r < 0)
            return CC_EXIT_USAGE;
        if (r > 0)
            continue;
        if (strcmp(argv[i], "--quantum") == 0) {
            if (opt_long(argc, argv, &i, RR_MIN_QUANTUM, RR_MAX_QUANTUM,
                         &quantum) != 0)
                return CC_EXIT_USAGE;
        } else {
            return usage_error("schedule", "unknown option '%s'", argv[i]);
        }
    }
    if (job_args_finish(&ja) != 0)
        return CC_EXIT_USAGE;

    jobset_print(&ja.jobs, "Input jobs");
    if (strcmp(which, "fcfs") == 0)
        rc = sched_run_and_print(ALGO_FCFS, 0, &ja.jobs);
    else if (strcmp(which, "rr") == 0)
        rc = sched_run_and_print(ALGO_RR, (int)quantum, &ja.jobs);
    else if (strcmp(which, "priority") == 0)
        rc = sched_run_and_print(ALGO_PRIORITY, 0, &ja.jobs);
    else
        rc = sched_compare(&ja.jobs, (int)quantum);
    return rc == 0 ? CC_EXIT_OK : CC_EXIT_FAIL;
}

static int cmd_logger(int argc, char **argv)
{
    LoggerDemoOpts o;
    long v;

    logger_demo_default_opts(&o);
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--capacity") == 0) {
            if (opt_long(argc, argv, &i, 1, LOGGER_MAX_CAPACITY, &v) != 0)
                return CC_EXIT_USAGE;
            o.capacity = (size_t)v;
        } else if (strcmp(a, "--producers") == 0) {
            if (opt_long(argc, argv, &i, 1, 16, &v) != 0)
                return CC_EXIT_USAGE;
            o.producers = (int)v;
        } else if (strcmp(a, "--messages") == 0) {
            if (opt_long(argc, argv, &i, 1, 10000, &v) != 0)
                return CC_EXIT_USAGE;
            o.messages = (int)v;
        } else if (strcmp(a, "--producer-delay") == 0) {
            if (opt_long(argc, argv, &i, 0, 1000, &v) != 0)
                return CC_EXIT_USAGE;
            o.producer_delay_ms = (int)v;
        } else if (strcmp(a, "--consumer-delay") == 0) {
            if (opt_long(argc, argv, &i, 0, 1000, &v) != 0)
                return CC_EXIT_USAGE;
            o.consumer_delay_ms = (int)v;
        } else if (strcmp(a, "--log") == 0) {
            if (opt_value(argc, argv, &i, &o.path) != 0)
                return CC_EXIT_USAGE;
            if (o.path[0] == '\0')
                return usage_error("logger", "--log needs a non-empty path");
        } else if (strcmp(a, "--quiet") == 0) {
            o.trace = false;
        } else {
            return usage_error("logger", "unknown option '%s'", a);
        }
    }
    return logger_run_demo(&o) == 0 ? CC_EXIT_OK : CC_EXIT_FAIL;
}

static int cmd_ipc(int argc, char **argv)
{
    JobArgs ja;
    IpcOptions opt;
    long v;

    ipc_default_options(&opt);
    job_args_init(&ja);
    for (int i = 2; i < argc; i++) {
        int r = job_option(argc, argv, &i, &ja);

        if (r < 0)
            return CC_EXIT_USAGE;
        if (r > 0)
            continue;
        if (strcmp(argv[i], "--timeout") == 0) {
            if (opt_long(argc, argv, &i, 1, IPC_MAX_TIMEOUT_S, &v) != 0)
                return CC_EXIT_USAGE;
            opt.timeout_s = (int)v;
        } else if (strcmp(argv[i], "--simulate-consumer-crash") == 0) {
            opt.fault = IPC_FAULT_CONSUMER_CRASH;
        } else {
            return usage_error("ipc", "unknown option '%s'", argv[i]);
        }
    }
    if (job_args_finish(&ja) != 0)
        return CC_EXIT_USAGE;
    return ipc_run_demo(&ja.jobs, &opt) == 0 ? CC_EXIT_OK : CC_EXIT_FAIL;
}

/*
 * Run one Banker's state and check the program's verdict against what the
 * state is known to be (used by the demo states only).
 */
static int banker_demo_state(bool expect_safe)
{
    BankerState bs;
    BankerResult res;

    if (expect_safe)
        banker_load_demo_safe(&bs);
    else
        banker_load_demo_unsafe(&bs);
    if (banker_run(&bs) != 0)
        return -1;
    banker_check_safety(&bs, &res, false);
    if (res.safe != expect_safe) {
        cc_errf("verdict differs from the expected %s state",
                expect_safe ? "safe" : "unsafe");
        return -1;
    }
    if (res.safe && !banker_verify_sequence(&bs, res.sequence, res.seq_len)) {
        cc_errf("safe sequence failed independent verification");
        return -1;
    }
    return 0;
}

static int cmd_banker(int argc, char **argv)
{
    const char *demo = NULL, *file = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--demo") == 0) {
            if (opt_value(argc, argv, &i, &demo) != 0)
                return CC_EXIT_USAGE;
            if (strcmp(demo, "safe") && strcmp(demo, "unsafe"))
                return usage_error("banker", "--demo must be 'safe' or "
                                   "'unsafe', not '%s'", demo);
        } else if (strcmp(argv[i], "--file") == 0) {
            if (opt_value(argc, argv, &i, &file) != 0)
                return CC_EXIT_USAGE;
        } else {
            return usage_error("banker", "unknown option '%s'", argv[i]);
        }
    }
    if (demo != NULL && file != NULL)
        return usage_error("banker", "--demo and --file cannot be combined");

    if (file != NULL) {
        BankerState bs;
        char err[200];

        if (banker_load_file(&bs, file, err, sizeof err) != 0) {
            cc_errf("%s", err);
            return CC_EXIT_USAGE;
        }
        return banker_run(&bs) == 0 ? CC_EXIT_OK : CC_EXIT_FAIL;
    }
    if (demo != NULL)
        return banker_demo_state(strcmp(demo, "safe") == 0) == 0
                   ? CC_EXIT_OK : CC_EXIT_FAIL;
    {
        int a = banker_demo_state(true);
        int b = banker_demo_state(false);

        return (a == 0 && b == 0) ? CC_EXIT_OK : CC_EXIT_FAIL;
    }
}

static int cmd_worker(int argc, char **argv)
{
    int rc;

    if (argc != 3)
        return usage_error("worker", "worker needs exactly one of: normal, "
                           "fail-exit, fail-signal, pool");
    if (strcmp(argv[2], "normal") == 0)
        rc = worker_run_scenario(WORKER_NORMAL);
    else if (strcmp(argv[2], "fail-exit") == 0)
        rc = worker_run_scenario(WORKER_FAIL_EXIT);
    else if (strcmp(argv[2], "fail-signal") == 0)
        rc = worker_run_scenario(WORKER_FAIL_SIGNAL);
    else if (strcmp(argv[2], "pool") == 0)
        rc = worker_run_pool();
    else
        return usage_error("worker", "unknown worker scenario '%s'", argv[2]);
    return rc == 0 ? CC_EXIT_OK : CC_EXIT_FAIL;
}

static int cmd_limits(int argc, char **argv)
{
    int rc;

    if (argc != 3)
        return usage_error("limits", "limits needs exactly one of: fsize, "
                           "nofile, cpu, all");
    if (strcmp(argv[2], "fsize") == 0)
        rc = limits_run(LIMIT_FSIZE);
    else if (strcmp(argv[2], "nofile") == 0)
        rc = limits_run(LIMIT_NOFILE);
    else if (strcmp(argv[2], "cpu") == 0)
        rc = limits_run(LIMIT_CPU);
    else if (strcmp(argv[2], "all") == 0)
        rc = limits_run_all();
    else
        return usage_error("limits", "unknown limit '%s'", argv[2]);
    return rc == 0 ? CC_EXIT_OK : CC_EXIT_FAIL;
}

static int cmd_cleanup(int argc, char **argv)
{
    int n;

    (void)argv;
    if (argc != 2)
        return usage_error("cleanup", "cleanup takes no arguments");
    cc_banner("Cleanup of this program's IPC objects");
    n = ipc_cleanup_named(true);
    printf("removed %d leftover object(s); this program's IPC objects now "
           "present: %s\n", n, ipc_resources_present() ? "YES" : "none");
    return ipc_resources_present() ? CC_EXIT_FAIL : CC_EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* integrated demonstration                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    const char *requirement;
    bool passed;
} Stage;

typedef struct {
    Stage stages[MAX_STAGES];
    int n;
} StageList;

static void stage_record(StageList *sl, const char *name, const char *req,
                         int rc)
{
    if (sl->n < MAX_STAGES) {
        sl->stages[sl->n].name = name;
        sl->stages[sl->n].requirement = req;
        sl->stages[sl->n].passed = (rc == 0);
        sl->n++;
    }
}

/* Final check: no child processes or IPC resources remain. */
static int verify_cleanup(void)
{
    int st;
    int ok = 1;

    bool children_left =
        !(waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD);

    bool ipc_left = ipc_resources_present();

    cc_banner("FR7 - Cleanup verification");

    printf("child processes still running or un-reaped : %s\n",
           children_left ? "YES (problem)" : "none (waitpid -> ECHILD)");

    printf("POSIX shm/semaphores of this program       : %s\n",
           ipc_left ? "STILL PRESENT (problem)" : "none (released)");

    if (children_left || ipc_left)
        ok = 0;

    printf("cleanup verification: %s\n", ok ? "PASS" : "FAIL");

    return ok ? 0 : -1;
}

/* Write the stage results through the concurrent logger (FR3 + log file). */
static int log_demo_results(const StageList *sl)
{
    Logger lg;
    int fails = 0;

    if (logger_init(&lg, LOGGER_DEFAULT_PATH, 8, 0, false) != 0)
        return -1;
    if (logger_start(&lg) != 0) {
        logger_destroy(&lg);
        return -1;
    }
    logger_log(&lg, LOG_INFO, "demo", "integrated demonstration finished; "
               "%d stages", sl->n);
    for (int i = 0; i < sl->n; i++) {
        logger_log(&lg, sl->stages[i].passed ? LOG_INFO : LOG_ERROR, "demo",
                   "%-4s %-34s %s", sl->stages[i].requirement,
                   sl->stages[i].name,
                   sl->stages[i].passed ? "PASS" : "FAIL");
        if (!sl->stages[i].passed)
            fails++;
    }
    logger_log(&lg, fails ? LOG_ERROR : LOG_INFO, "demo",
               "overall result: %s", fails ? "FAIL" : "PASS");
    logger_stop(&lg);
    logger_destroy(&lg);
    return 0;
}

static int cmd_demo(int argc, char **argv)
{
    StageList sl = { .n = 0 };
    JobSet jobs;
    LoggerDemoOpts lo;
    IpcOptions io;
    int failed = 0;

    (void)argv;
    if (argc != 2)
        return usage_error("demo", "demo takes no arguments");

    cc_banner("CampusCompute integrated demonstration");
    printf("Version %s. Every result below is computed by the program; the "
           "worker\nfailures are SIMULATED, not real operating-system "
           "failures.\n", CC_VERSION);

    jobset_load_sample(&jobs);
    jobset_print(&jobs, "FR1 - Sample job set");

    stage_record(&sl, "FCFS scheduling", "FR2",
                 sched_run_and_print(ALGO_FCFS, 0, &jobs));
    stage_record(&sl, "Round Robin scheduling", "FR2",
                 sched_run_and_print(ALGO_RR, RR_DEFAULT_QUANTUM, &jobs));
    stage_record(&sl, "Priority scheduling", "FR2",
                 sched_run_and_print(ALGO_PRIORITY, 0, &jobs));
    stage_record(&sl, "Scheduling comparison", "FR2",
                 sched_compare(&jobs, RR_DEFAULT_QUANTUM));

    logger_demo_default_opts(&lo);
    lo.path = DEMO_LOGGER_PATH;
    stage_record(&sl, "Producer-consumer logger", "FR3",
                 logger_run_demo(&lo));

    ipc_default_options(&io);
    stage_record(&sl, "Shared memory + semaphore IPC", "FR4",
                 ipc_run_demo(&jobs, &io));

    stage_record(&sl, "Banker's safe state", "FR5", banker_demo_state(true));
    stage_record(&sl, "Banker's unsafe state", "FR5",
                 banker_demo_state(false));

    stage_record(&sl, "Worker: normal completion", "FR6",
                 worker_run_scenario(WORKER_NORMAL));
    stage_record(&sl, "Worker: simulated exit failure", "FR6",
                 worker_run_scenario(WORKER_FAIL_EXIT));
    stage_record(&sl, "Worker: simulated signal failure", "FR6",
                 worker_run_scenario(WORKER_FAIL_SIGNAL));
    stage_record(&sl, "Worker pool (waitpid -1)", "FR6", worker_run_pool());

    stage_record(&sl, "Resource limits (3 kinds)", "FR7", limits_run_all());
    stage_record(&sl, "Cleanup verification", "FR7", verify_cleanup());

    if (log_demo_results(&sl) != 0)
        cc_errf("could not write the demonstration summary log");

    cc_banner("Integrated demonstration summary");
    printf("  %-5s %-36s %s\n", "Req", "Stage", "Result");
    cc_rule('-', 56);
    for (int i = 0; i < sl.n; i++) {
        printf("  %-5s %-36s %s\n", sl.stages[i].requirement,
               sl.stages[i].name, sl.stages[i].passed ? "PASS" : "FAIL");
        if (!sl.stages[i].passed)
            failed++;
    }
    cc_rule('-', 56);
    printf("  %d of %d stages passed.  Logs: %s, %s\n", sl.n - failed, sl.n,
           LOGGER_DEFAULT_PATH, DEMO_LOGGER_PATH);
    printf("  OVERALL: %s\n", failed ? "FAIL" : "PASS");
    return failed ? CC_EXIT_FAIL : CC_EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *cmd;

    setvbuf(stdout, NULL, _IOLBF, 0);       /* keep stdout/stderr in order */
    /*
     * "campuscompute ... | head" closes the pipe early. Ignore SIGPIPE so such
     * a pipeline can never kill the program half-way and skip its cleanup of
     * threads, shared memory and semaphores (writes just fail with EPIPE).
     */
    signal(SIGPIPE, SIG_IGN);
    if (argc < 2) {
        print_usage(stderr);
        return CC_EXIT_USAGE;
    }
    cmd = argv[1];

    if (!strcmp(cmd, "help") || !strcmp(cmd, "--help") || !strcmp(cmd, "-h")) {
        print_usage(stdout);
        return CC_EXIT_OK;
    }
    if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) {
        printf("CampusCompute %s\n", CC_VERSION);
        return CC_EXIT_OK;
    }
    if (!strcmp(cmd, "schedule"))
        return cmd_schedule(argc, argv);
    if (!strcmp(cmd, "logger"))
        return cmd_logger(argc, argv);
    if (!strcmp(cmd, "ipc"))
        return cmd_ipc(argc, argv);
    if (!strcmp(cmd, "banker"))
        return cmd_banker(argc, argv);
    if (!strcmp(cmd, "worker"))
        return cmd_worker(argc, argv);
    if (!strcmp(cmd, "limits"))
        return cmd_limits(argc, argv);
    if (!strcmp(cmd, "demo"))
        return cmd_demo(argc, argv);
    if (!strcmp(cmd, "cleanup"))
        return cmd_cleanup(argc, argv);

    return usage_error(NULL, "unknown command '%s'", cmd);
}
