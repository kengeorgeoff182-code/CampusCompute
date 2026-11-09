/*
 * worker.c - FR6: fork() worker processes and detect how they ended with
 * waitpid().
 *
 * fork() safety note: the program may have logger threads running when it
 * forks (integrated demo). In the child only the forking thread survives, so
 * the child must not touch the logger or any lock another thread might have
 * held. The worker body therefore uses only write(2), nanosleep and _exit.
 */
#include "worker.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

const char *worker_scenario_name(WorkerScenario s)
{
    switch (s) {
    case WORKER_NORMAL:      return "normal";
    case WORKER_FAIL_EXIT:   return "simulated failure: non-zero exit";
    case WORKER_FAIL_SIGNAL: return "simulated failure: killed by signal";
    }
    return "?";
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGTERM: return "SIGTERM";
    case SIGKILL: return "SIGKILL";
    case SIGINT:  return "SIGINT";
    case SIGABRT: return "SIGABRT";
    case SIGSEGV: return "SIGSEGV";
    case SIGXCPU: return "SIGXCPU";
    case SIGXFSZ: return "SIGXFSZ";
    case SIGUSR1: return "SIGUSR1";
    case SIGHUP:  return "SIGHUP";
    default:      return "signal";
    }
}

WorkerOutcome worker_classify(int st)
{
    WorkerOutcome o;

    memset(&o, 0, sizeof o);
    if (WIFEXITED(st)) {
        o.exit_code = WEXITSTATUS(st);
        o.kind = o.exit_code == 0 ? WOUT_EXIT_OK : WOUT_EXIT_NONZERO;
    } else if (WIFSIGNALED(st)) {
        o.kind = WOUT_SIGNALED;
        o.signal = WTERMSIG(st);
        o.core_dumped = WCOREDUMP(st) != 0;
    } else {
        o.kind = WOUT_OTHER;
    }
    return o;
}

void worker_describe_status(int st, char *buf, size_t len)
{
    WorkerOutcome o = worker_classify(st);

    switch (o.kind) {
    case WOUT_EXIT_OK:
        snprintf(buf, len, "exited normally with status 0 (success)");
        break;
    case WOUT_EXIT_NONZERO:
        snprintf(buf, len, "exited with NON-ZERO status %d (reported "
                 "failure)", o.exit_code);
        break;
    case WOUT_SIGNALED:
        snprintf(buf, len, "TERMINATED BY SIGNAL %d (%s: %s)%s", o.signal,
                 signal_name(o.signal), strsignal(o.signal),
                 o.core_dumped ? ", core dumped" : "");
        break;
    default:
        snprintf(buf, len, "stopped or unknown wait status 0x%x", st);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* child side                                                          */
/* ------------------------------------------------------------------ */

static void child_sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

    nanosleep(&ts, NULL);       /* interruption is harmless here */
}

/* Never returns. Async-signal-safe calls only (see file header). */
static void worker_child(WorkerScenario sc, long work_ms)
{
    char buf[200];
    sigset_t none;
    volatile unsigned long acc = 0;

    signal(SIGTERM, SIG_DFL);           /* make sure default actions apply */
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);

    snprintf(buf, sizeof buf, "  [worker pid=%d] started (%s)\n",
             (int)getpid(), worker_scenario_name(sc));
    cc_write_str(STDOUT_FILENO, buf);

    for (unsigned long i = 0; i < 2000000UL; i++)   /* pretend to compute */
        acc += i % 7;
    child_sleep_ms(work_ms);

    switch (sc) {
    case WORKER_NORMAL:
        snprintf(buf, sizeof buf, "  [worker pid=%d] work finished, "
                 "exiting with status 0\n", (int)getpid());
        cc_write_str(STDOUT_FILENO, buf);
        _exit(0);
    case WORKER_FAIL_EXIT:
        snprintf(buf, sizeof buf, "  [worker pid=%d] *** SIMULATED FAILURE: "
                 "exiting with status %d (injected by CampusCompute, not a "
                 "real fault) ***\n", (int)getpid(), WORKER_SIM_EXIT_CODE);
        cc_write_str(STDOUT_FILENO, buf);
        _exit(WORKER_SIM_EXIT_CODE);
    case WORKER_FAIL_SIGNAL:
        snprintf(buf, sizeof buf, "  [worker pid=%d] *** SIMULATED FAILURE: "
                 "sending SIGTERM to itself (injected by CampusCompute, not "
                 "a real fault) ***\n", (int)getpid());
        cc_write_str(STDOUT_FILENO, buf);
        raise(SIGTERM);                 /* default action: terminate */
        break;
    }
    _exit(99);                          /* not reached */
}

/* ------------------------------------------------------------------ */
/* parent side                                                         */
/* ------------------------------------------------------------------ */

static int verify_outcome(WorkerScenario sc, const WorkerOutcome *o)
{
    switch (sc) {
    case WORKER_NORMAL:
        return o->kind == WOUT_EXIT_OK;
    case WORKER_FAIL_EXIT:
        return o->kind == WOUT_EXIT_NONZERO &&
               o->exit_code == WORKER_SIM_EXIT_CODE;
    case WORKER_FAIL_SIGNAL:
        return o->kind == WOUT_SIGNALED && o->signal == SIGTERM;
    }
    return 0;
}

static const char *expected_text(WorkerScenario sc)
{
    switch (sc) {
    case WORKER_NORMAL:      return "exit status 0";
    case WORKER_FAIL_EXIT:   return "exit status 3";
    case WORKER_FAIL_SIGNAL: return "termination by SIGTERM";
    }
    return "?";
}

/* waitpid() retried on EINTR. Returns pid, or -1 (errno set). */
static pid_t wait_retry(pid_t pid, int *st)
{
    pid_t r;

    while ((r = waitpid(pid, st, 0)) < 0 && errno == EINTR)
        ;
    return r;
}

/* After reaping, there must be no children left (no zombies). */
static int no_children_left(void)
{
    int st;

    return waitpid(-1, &st, WNOHANG) == -1 && errno == ECHILD;
}

int worker_run_scenario(WorkerScenario sc)
{
    pid_t pid;
    int st = 0, ok;
    char desc[160];
    WorkerOutcome o;

    cc_banner("FR6 - Worker Process and Failure Detection");
    printf("Scenario: %s\n", worker_scenario_name(sc));
    if (sc != WORKER_NORMAL)
        puts("NOTE: this failure is SIMULATED by the worker itself; it is "
             "not a real operating-system failure.");
    printf("[parent pid=%d] calling fork() to create the worker\n",
           (int)getpid());
    fflush(stdout);
    pid = fork();
    if (pid < 0) {
        cc_errf("fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0)
        worker_child(sc, 30);
    printf("[parent pid=%d] worker created: pid=%d; calling waitpid()\n",
           (int)getpid(), (int)pid);

    if (wait_retry(pid, &st) != pid) {
        cc_errf("waitpid: %s", strerror(errno));
        return -1;
    }
    o = worker_classify(st);
    worker_describe_status(st, desc, sizeof desc);
    printf("[parent] waitpid() returned pid=%d, raw status=0x%04x\n",
           (int)pid, (unsigned)st);
    printf("[parent] worker %d: %s\n", (int)pid, desc);
    printf("[parent] WIFEXITED=%d WIFSIGNALED=%d\n", WIFEXITED(st) ? 1 : 0,
           WIFSIGNALED(st) ? 1 : 0);

    ok = verify_outcome(sc, &o);
    printf("[parent] expected %s -> detection %s\n", expected_text(sc),
           ok ? "CORRECT" : "WRONG");
    if (!no_children_left()) {
        cc_errf("a child process was left un-reaped");
        ok = 0;
    } else {
        puts("[parent] worker reaped; no zombie or remaining child "
             "(waitpid -> ECHILD)");
    }
    return ok ? 0 : -1;
}

int worker_run_pool(void)
{
    static const WorkerScenario scenarios[3] = {
        WORKER_NORMAL, WORKER_FAIL_EXIT, WORKER_FAIL_SIGNAL
    };
    static const long work_ms[3] = { 120, 20, 60 };   /* finish out of order */
    pid_t pids[3] = { -1, -1, -1 };
    int started = 0, reaped = 0, correct = 0, ok = 1;

    cc_banner("FR6 - Worker Pool (3 concurrent workers, reaped with "
              "waitpid(-1))");
    puts("Two of the three failures below are SIMULATED by the workers "
         "themselves; none is a real OS failure.");
    fflush(stdout);
    for (int i = 0; i < 3; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            cc_errf("fork: %s", strerror(errno));
            ok = 0;
            break;
        }
        if (pid == 0)
            worker_child(scenarios[i], work_ms[i]);
        pids[i] = pid;
        started++;
        printf("[parent] started worker %d: pid=%d (%s)\n", i + 1, (int)pid,
               worker_scenario_name(scenarios[i]));
    }

    printf("\n  %-8s %-36s %-8s %s\n", "PID", "Scenario", "Raw", "Detected "
           "outcome (in order of completion)");
    while (reaped < started) {
        int st = 0, idx = -1;
        pid_t pid = wait_retry(-1, &st);

        if (pid < 0) {
            cc_errf("waitpid(-1): %s", strerror(errno));
            ok = 0;
            break;
        }
        reaped++;
        for (int i = 0; i < 3; i++)
            if (pids[i] == pid)
                idx = i;
        if (idx < 0) {
            printf("  %-8d (unexpected child)\n", (int)pid);
            ok = 0;
            continue;
        }
        {
            char desc[160];
            WorkerOutcome o = worker_classify(st);
            int good = verify_outcome(scenarios[idx], &o);

            worker_describe_status(st, desc, sizeof desc);
            printf("  %-8d %-36s 0x%04x   %s  [%s]\n", (int)pid,
                   worker_scenario_name(scenarios[idx]), (unsigned)st, desc,
                   good ? "as expected" : "UNEXPECTED");
            correct += good;
        }
    }
    printf("\n[parent] %d/%d workers reaped, %d/%d outcomes detected "
           "correctly\n", reaped, started, correct, started);
    if (!no_children_left()) {
        cc_errf("a child process was left un-reaped");
        ok = 0;
    } else {
        puts("[parent] no remaining children (waitpid -> ECHILD): all "
             "workers reaped, no zombies");
    }
    return (ok && started == 3 && correct == 3) ? 0 : -1;
}
