/*
 * unit_tests.c - unit tests for CampusCompute modules (no framework needed).
 *
 * Expected values are worked out BY HAND from the textbook examples and the
 * documented rules, never copied from the program's output. Build and run
 * with:  make unit
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "banker.h"
#include "ipc.h"
#include "job_manager.h"
#include "logger.h"
#include "scheduler.h"
#include "util.h"
#include "worker.h"

static int g_checks, g_failed;

#define CHECK(cond, ...) do {                                              \
        g_checks++;                                                        \
        if (!(cond)) {                                                     \
            g_failed++;                                                    \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                  \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while (0)

static void section(const char *name)
{
    printf("[unit] %s\n", name);
}

/* ---- helpers ---------------------------------------------------------- */

static JobSet make_jobs(const int (*rows)[4], int n)
{
    JobSet js;
    char err[128];

    jobset_init(&js);
    for (int i = 0; i < n; i++)
        if (jobset_add(&js, rows[i][0], rows[i][1], rows[i][2], rows[i][3],
                       err, sizeof err) != 0)
            printf("  setup error: %s\n", err);
    return js;
}

static const Job *job_by_id(const ScheduleResult *r, int id)
{
    for (int i = 0; i < r->jobs.count; i++)
        if (r->jobs.jobs[i].id == id)
            return &r->jobs.jobs[i];
    return NULL;
}

/* Check completion/turnaround/waiting of one job against hand-worked values. */
static void expect_job(const ScheduleResult *r, int id, int completion,
                       int turnaround, int waiting, const char *tag)
{
    const Job *j = job_by_id(r, id);

    CHECK(j != NULL, "%s: job %d missing", tag, id);
    if (j == NULL)
        return;
    CHECK(j->completion_time == completion,
          "%s: J%d completion %d, expected %d", tag, id, j->completion_time,
          completion);
    CHECK(j->turnaround == turnaround, "%s: J%d turnaround %d, expected %d",
          tag, id, j->turnaround, turnaround);
    CHECK(j->waiting == waiting, "%s: J%d waiting %d, expected %d", tag, id,
          j->waiting, waiting);
}

static int run(SchedAlgo a, int q, const JobSet *js, ScheduleResult *r)
{
    char err[160];
    int rc = sched_run(a, q, js, r, err, sizeof err);

    if (rc != 0)
        printf("  sched_run error: %s\n", err);
    return rc;
}

/* ---- util / job manager ----------------------------------------------- */

static void test_util(void)
{
    long v = 0;

    section("util: strict integer parsing");
    CHECK(cc_parse_long("42", 0, 100, &v) == 0 && v == 42, "42");
    CHECK(cc_parse_long("-3", -10, 10, &v) == 0 && v == -3, "-3");
    CHECK(cc_parse_long("", 0, 10, &v) == -1, "empty string");
    CHECK(cc_parse_long("12abc", 0, 100, &v) == -1, "trailing junk");
    CHECK(cc_parse_long(" 5", 0, 10, &v) == -1, "leading space");
    CHECK(cc_parse_long("5 ", 0, 10, &v) == -1, "trailing space");
    CHECK(cc_parse_long("101", 0, 100, &v) == -2, "above max");
    CHECK(cc_parse_long("99999999999999999999", 0, 100, &v) == -2,
          "overflow");
    CHECK(cc_parse_long(NULL, 0, 10, &v) == -1, "NULL");
}

static void test_job_manager(void)
{
    JobSet js;
    char err[160];

    section("job manager (FR1): validation");
    jobset_init(&js);
    CHECK(jobset_add(&js, 1, 0, 5, 2, err, sizeof err) == 0, "valid job");
    CHECK(jobset_add(&js, 1, 0, 5, 2, err, sizeof err) != 0,
          "duplicate id rejected");
    CHECK(jobset_add(&js, 0, 0, 5, 2, err, sizeof err) != 0, "id 0");
    CHECK(jobset_add(&js, 2, -1, 5, 2, err, sizeof err) != 0,
          "negative arrival");
    CHECK(jobset_add(&js, 2, 0, 0, 2, err, sizeof err) != 0, "zero burst");
    CHECK(jobset_add(&js, 2, 0, JM_MAX_BURST + 1, 2, err, sizeof err) != 0,
          "burst too large");
    CHECK(jobset_add(&js, 2, 0, 5, 0, err, sizeof err) != 0, "priority 0");
    CHECK(jobset_add(&js, 2, 0, 5, JM_MAX_PRIORITY + 1, err, sizeof err) != 0,
          "priority too large");
    CHECK(js.count == 1, "rejected jobs must not be added (count=%d)",
          js.count);
    CHECK(jobset_add_spec(&js, "3,1,4,2", err, sizeof err) == 0,
          "comma spec");
    CHECK(jobset_add_spec(&js, "4 1 4 2", err, sizeof err) == 0,
          "space spec");
    CHECK(jobset_add_spec(&js, "5,1,4", err, sizeof err) != 0,
          "too few fields");
    CHECK(jobset_add_spec(&js, "5,1,4,2,9", err, sizeof err) != 0,
          "too many fields");
    CHECK(jobset_add_spec(&js, "x,1,4,2", err, sizeof err) != 0,
          "non-numeric field");
    CHECK(js.count == 3, "count after specs = %d", js.count);

    jobset_init(&js);
    for (int i = 1; i <= JM_MAX_JOBS; i++)
        CHECK(jobset_add(&js, i, 0, 1, 1, err, sizeof err) == 0, "fill %d", i);
    CHECK(jobset_add(&js, JM_MAX_JOBS + 1, 0, 1, 1, err, sizeof err) != 0,
          "capacity overflow rejected");
}

/* ---- scheduler -------------------------------------------------------- */

static void test_fcfs(void)
{
    /* Silberschatz: bursts 24, 3, 3 at time 0 -> waits 0, 24, 27 (avg 17). */
    static const int rows[][4] = { {1,0,24,1}, {2,0,3,1}, {3,0,3,1} };
    JobSet js = make_jobs(rows, 3);
    ScheduleResult r;
    char err[160];

    section("scheduler: FCFS (textbook 24/3/3)");
    CHECK(run(ALGO_FCFS, 0, &js, &r) == 0, "run");
    expect_job(&r, 1, 24, 24, 0, "FCFS");
    expect_job(&r, 2, 27, 27, 24, "FCFS");
    expect_job(&r, 3, 30, 30, 27, "FCFS");
    CHECK(r.sum_waiting == 51 && r.avg_waiting > 16.99 && r.avg_waiting < 17.01,
          "avg waiting %.2f, expected 17.00", r.avg_waiting);
    CHECK(r.avg_turnaround > 26.99 && r.avg_turnaround < 27.01,
          "avg turnaround %.2f, expected 27.00", r.avg_turnaround);
    CHECK(sched_verify_result(&r, err, sizeof err) == 0, "invariants: %s", err);
    sched_result_free(&r);
}

static void test_fcfs_idle(void)
{
    /* One job arrives at 5: CPU idle 0-5, J1 5-8, J2 (arr 6) 8-10. */
    static const int rows[][4] = { {1,5,3,1}, {2,6,2,1} };
    JobSet js = make_jobs(rows, 2);
    ScheduleResult r;

    section("scheduler: CPU idle period at the start");
    CHECK(run(ALGO_FCFS, 0, &js, &r) == 0, "run");
    expect_job(&r, 1, 8, 3, 0, "idle");
    expect_job(&r, 2, 10, 4, 2, "idle");
    CHECK(r.idle_time == 5, "idle time %d, expected 5", r.idle_time);
    CHECK(r.finish_time == 10, "finish %d, expected 10", r.finish_time);
    CHECK(r.nsegs >= 1 && r.segs[0].job_id == SEG_IDLE &&
              r.segs[0].start == 0 && r.segs[0].end == 5,
          "first Gantt segment must be idle 0-5");
    sched_result_free(&r);
}

static void test_rr(void)
{
    /* Textbook 24/3/3 with quantum 4: P2 ends 7, P3 ends 10, P1 ends 30. */
    static const int rows[][4] = { {1,0,24,1}, {2,0,3,1}, {3,0,3,1} };
    JobSet js = make_jobs(rows, 3);
    ScheduleResult r;
    char err[160];

    section("scheduler: Round Robin (textbook, q=4) and quantum validation");
    CHECK(run(ALGO_RR, 4, &js, &r) == 0, "run");
    expect_job(&r, 2, 7, 7, 4, "RR");
    expect_job(&r, 3, 10, 10, 7, "RR");
    expect_job(&r, 1, 30, 30, 6, "RR");
    CHECK(r.avg_waiting > 5.66 && r.avg_waiting < 5.67,
          "avg waiting %.3f, expected 17/3", r.avg_waiting);
    CHECK(r.quantum == 4, "quantum recorded as %d", r.quantum);
    CHECK(sched_verify_result(&r, err, sizeof err) == 0, "invariants: %s", err);
    sched_result_free(&r);

    CHECK(sched_run(ALGO_RR, 0, &js, &r, err, sizeof err) != 0,
          "quantum 0 rejected");
    CHECK(sched_run(ALGO_RR, -2, &js, &r, err, sizeof err) != 0,
          "negative quantum rejected");
    CHECK(sched_run(ALGO_RR, RR_MAX_QUANTUM + 1, &js, &r, err, sizeof err) != 0,
          "quantum above maximum rejected");
}

static void test_rr_arrival_order(void)
{
    /*
     * q=2. J1 (arrival 0, burst 4), J2 (arrival 2, burst 1).
     * J1 runs 0-2 and is pre-empted at t=2, the SAME instant J2 arrives.
     * Documented rule: the arrival is queued BEFORE the pre-empted job, so
     * the queue is [J2, J1]: J2 runs 2-3 (done 3), J1 runs 3-5 (done 5).
     * With the opposite rule the queue would be [J1, J2]: J1 would finish at
     * 4 and J2 at 5, so this case tells the two rules apart.
     */
    static const int rows[][4] = { {1,0,4,1}, {2,2,1,1} };
    JobSet js = make_jobs(rows, 2);
    ScheduleResult r;

    section("scheduler: RR queues a simultaneous arrival before the pre-empted job");
    CHECK(run(ALGO_RR, 2, &js, &r) == 0, "run");
    expect_job(&r, 2, 3, 1, 0, "RR-arrival");
    expect_job(&r, 1, 5, 5, 1, "RR-arrival");
    sched_result_free(&r);
}

static void test_priority(void)
{
    /* Silberschatz priority example (lower number = higher priority):
     * order P2, P5, P1, P3, P4 -> waits 0, 1, 6, 16, 18 (avg 8.2). */
    static const int rows[][4] = {
        {1,0,10,3}, {2,0,1,1}, {3,0,2,4}, {4,0,1,5}, {5,0,5,2}
    };
    JobSet js = make_jobs(rows, 5);
    ScheduleResult r;

    section("scheduler: non-preemptive Priority (textbook)");
    CHECK(run(ALGO_PRIORITY, 0, &js, &r) == 0, "run");
    expect_job(&r, 2, 1, 1, 0, "PRIO");
    expect_job(&r, 5, 6, 6, 1, "PRIO");
    expect_job(&r, 1, 16, 16, 6, "PRIO");
    expect_job(&r, 3, 18, 18, 16, "PRIO");
    expect_job(&r, 4, 19, 19, 18, "PRIO");
    CHECK(r.avg_waiting > 8.19 && r.avg_waiting < 8.21,
          "avg waiting %.2f, expected 8.20", r.avg_waiting);
    sched_result_free(&r);
}

static void test_priority_tiebreak(void)
{
    /*
     * J9 (prio 5) runs 0-4 because it is the only job at t=0. At t=4 J5
     * (arrived 1) and J4 (arrived 2) are ready with EQUAL priority 1: the
     * EARLIER arrival (J5) wins even though its id is larger. J5 4-6, J4 6-8.
     * Equal priority AND equal arrival -> lower id: J2 before J3 (arr 0).
     */
    static const int rows[][4] = { {9,0,4,5}, {5,1,2,1}, {4,2,2,1} };
    static const int rows2[][4] = { {3,0,2,1}, {2,0,2,1} };
    JobSet js = make_jobs(rows, 3), js2 = make_jobs(rows2, 2);
    ScheduleResult r;

    section("scheduler: Priority tie-breaking (arrival, then id)");
    CHECK(run(ALGO_PRIORITY, 0, &js, &r) == 0, "run");
    expect_job(&r, 9, 4, 4, 0, "tie-arrival");
    expect_job(&r, 5, 6, 5, 3, "tie-arrival");
    expect_job(&r, 4, 8, 6, 4, "tie-arrival");
    sched_result_free(&r);

    CHECK(run(ALGO_PRIORITY, 0, &js2, &r) == 0, "run2");
    expect_job(&r, 2, 2, 2, 0, "tie-id");
    expect_job(&r, 3, 4, 4, 2, "tie-id");
    sched_result_free(&r);
}

static void test_inputs_not_modified(void)
{
    static const int rows[][4] = {
        {1,0,5,2}, {2,1,3,1}, {3,2,8,3}, {4,2,2,1}, {5,3,4,2}, {6,25,3,1}
    };
    JobSet js = make_jobs(rows, 6), before;
    ScheduleResult r;

    section("scheduler: algorithms work on copies of the input");
    before = js;
    for (int a = 0; a < ALGO_COUNT; a++) {
        CHECK(run((SchedAlgo)a, 3, &js, &r) == 0, "run algo %d", a);
        sched_result_free(&r);
    }
    CHECK(memcmp(&before, &js, sizeof js) == 0,
          "input job set changed after running all algorithms");
    CHECK(js.jobs[0].status == JOB_NEW && js.jobs[0].completion_time == -1,
          "input lifecycle fields must stay untouched");
}

static void test_empty_jobset(void)
{
    JobSet js;
    ScheduleResult r;
    char err[160];

    section("scheduler: empty job set is rejected");
    jobset_init(&js);
    CHECK(sched_run(ALGO_FCFS, 0, &js, &r, err, sizeof err) != 0,
          "empty set rejected");
}

/* ---- banker ----------------------------------------------------------- */

static int parse_text(BankerState *bs, const char *text, char *err,
                      size_t errlen)
{
    size_t len = strlen(text);
    char *copy = malloc(len + 1);       /* fmemopen wants a mutable buffer */
    FILE *fp;
    int rc;

    if (copy == NULL)
        return -1;
    memcpy(copy, text, len + 1);
    fp = fmemopen(copy, len, "r");
    if (fp == NULL) {
        free(copy);
        return -1;
    }
    rc = banker_parse(bs, fp, "memory", err, errlen);
    fclose(fp);
    free(copy);
    return rc;
}

static void test_banker(void)
{
    static const char safe_text[] =
        "processes 5\nresources 3\navailable 3 3 2\n"
        "max\n7 5 3\n3 2 2\n9 0 2\n2 2 2\n4 3 3\n"
        "allocation\n0 1 0\n2 0 0\n3 0 2\n2 1 1\n0 0 2\n";
    BankerState bs;
    BankerResult res;
    char err[200];
    /* Hand-worked textbook sequences (the standard answer <1,3,4,0,2> is
     * also valid; the program's index-order scan gives <1,3,0,2,4>). */
    static const int seq_textbook[5] = { 1, 3, 4, 0, 2 };
    static const int seq_bad[5] = { 0, 1, 2, 3, 4 };

    section("banker (FR5): Need, safe state, sequence verification");
    CHECK(parse_text(&bs, safe_text, err, sizeof err) == 0, "parse: %s", err);
    CHECK(bs.need[0][0] == 7 && bs.need[0][1] == 4 && bs.need[0][2] == 3,
          "Need[P0] must be Max-Alloc = 7 4 3");
    CHECK(bs.need[2][0] == 6 && bs.need[2][1] == 0 && bs.need[2][2] == 0,
          "Need[P2] must be 6 0 0");
    CHECK(banker_validate(&bs, err, sizeof err) == 0, "validate: %s", err);
    banker_check_safety(&bs, &res, false);
    CHECK(res.safe && res.seq_len == 5, "state must be SAFE");
    CHECK(banker_verify_sequence(&bs, res.sequence, res.seq_len),
          "program's own sequence fails independent verification");
    CHECK(banker_verify_sequence(&bs, seq_textbook, 5),
          "textbook sequence <1,3,4,0,2> must verify");
    CHECK(!banker_verify_sequence(&bs, seq_bad, 5),
          "<0,1,2,3,4> must NOT verify (P0 cannot run first)");

    banker_load_demo_unsafe(&bs);
    banker_check_safety(&bs, &res, false);
    CHECK(!res.safe, "demo unsafe state must be UNSAFE");

    banker_load_demo_safe(&bs);
    banker_check_safety(&bs, &res, false);
    CHECK(res.safe, "demo safe state must be SAFE");
}

static void test_banker_unsafe_by_hand(void)
{
    /* 12 instances; Needs 5, 6, 7 all exceed Available 3 -> unsafe. */
    static const char text[] =
        "processes 3\nresources 1\navailable 3\nmax\n10\n8\n9\n"
        "allocation\n5\n2\n2\n";
    BankerState bs;
    BankerResult res;
    char err[200];

    section("banker: hand-worked unsafe state");
    CHECK(parse_text(&bs, text, err, sizeof err) == 0, "parse: %s", err);
    banker_check_safety(&bs, &res, false);
    CHECK(!res.safe && res.seq_len == 0, "must be unsafe with no process able "
          "to finish (seq_len=%d)", res.seq_len);
}

static void test_banker_invalid(void)
{
    static const char *const bad[] = {
        /* allocation > max */
        "processes 2\nresources 1\navailable 5\nmax\n6\n3\nallocation\n1\n4\n",
        /* max larger than total instances */
        "processes 2\nresources 1\navailable 5\nmax\n20\n3\nallocation\n2\n1\n",
        /* negative available */
        "processes 2\nresources 1\navailable -1\nmax\n3\n3\nallocation\n1\n1\n",
        /* too few numbers (truncated) */
        "processes 2\nresources 2\navailable 1 1\nmax\n3 3\n3\n",
        /* non-numeric */
        "processes two\nresources 1\navailable 1\n",
        /* process count out of range */
        "processes 0\nresources 1\navailable 1\n",
        "processes 99\nresources 1\navailable 1\n",
        /* wrong keyword order */
        "resources 1\nprocesses 2\n",
        /* empty file */
        "",
        /* trailing garbage after allocation matrix */
        "processes 1\nresources 1\navailable 1\nmax\n2\nallocation\n1\nextra\n",
    };
    BankerState bs;
    char err[200];

    section("banker: invalid configurations are rejected");
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = parse_text(&bs, bad[i], err, sizeof err);

        if (rc == 0)
            rc = banker_validate(&bs, err, sizeof err);
        CHECK(rc != 0, "invalid case #%zu was accepted", i);
        if (rc != 0)
            CHECK(err[0] != '\0', "invalid case #%zu gave no message", i);
    }
}

/* ---- worker status decoding ------------------------------------------- */

static void test_worker_classify(void)
{
    WorkerOutcome o;

    section("worker (FR6): waitpid status decoding");
    o = worker_classify(0);
    CHECK(o.kind == WOUT_EXIT_OK && o.exit_code == 0, "exit 0");
    o = worker_classify(3 << 8);
    CHECK(o.kind == WOUT_EXIT_NONZERO && o.exit_code == 3, "exit 3");
    o = worker_classify(255 << 8);
    CHECK(o.kind == WOUT_EXIT_NONZERO && o.exit_code == 255, "exit 255");
    o = worker_classify(SIGTERM);
    CHECK(o.kind == WOUT_SIGNALED && o.signal == SIGTERM, "SIGTERM");
    o = worker_classify(SIGKILL);
    CHECK(o.kind == WOUT_SIGNALED && o.signal == SIGKILL, "SIGKILL");
}

static void test_worker_real_processes(void)
{
    section("worker: real fork()/waitpid() scenarios");
    CHECK(worker_run_scenario(WORKER_NORMAL) == 0, "normal worker");
    CHECK(worker_run_scenario(WORKER_FAIL_EXIT) == 0, "simulated exit(3)");
    CHECK(worker_run_scenario(WORKER_FAIL_SIGNAL) == 0, "simulated SIGTERM");
    CHECK(worker_run_pool() == 0, "worker pool");
}

/* ---- logger ----------------------------------------------------------- */

static int count_lines(const char *path)
{
    FILE *fp = fopen(path, "r");
    int n = 0, c;

    if (fp == NULL)
        return -1;
    while ((c = fgetc(fp)) != EOF)
        if (c == '\n')
            n++;
    fclose(fp);
    return n;
}

static void test_logger(void)
{
    Logger lg;
    LoggerStats st;
    char path[] = "/tmp/campuscompute_unit_XXXXXX";
    int fd = mkstemp(path), before;

    section("logger (FR3): bounded queue, full/empty, shutdown");
    CHECK(fd >= 0, "mkstemp");
    if (fd < 0)
        return;
    close(fd);

    CHECK(logger_init(&lg, path, 1, 3, false) == 0, "init");
    CHECK(logger_start(&lg) == 0, "start");
    before = count_lines(path);
    for (int i = 0; i < 20; i++)
        CHECK(logger_log(&lg, LOG_INFO, "unit", "message %d", i) == 0,
              "log %d", i);
    CHECK(logger_stop(&lg) == 0, "stop");
    st = logger_get_stats(&lg);
    CHECK(st.enqueued == 20, "enqueued %lu", st.enqueued);
    CHECK(st.dequeued == 20, "dequeued %lu", st.dequeued);
    CHECK(st.written == 20, "written %lu", st.written);
    CHECK(st.write_errors == 0, "write errors %lu", st.write_errors);
    CHECK(st.full_waits > 0, "capacity-1 queue must have blocked producers");
    CHECK(st.max_depth <= 1, "queue exceeded its capacity (max_depth=%lu)",
          st.max_depth);
    CHECK(logger_log(&lg, LOG_INFO, "unit", "late") == -1,
          "logging after stop must be rejected, not hang");
    CHECK(logger_get_stats(&lg).rejected == 1, "rejected counter");
    logger_destroy(&lg);
    CHECK(count_lines(path) >= before + 20, "log file must hold 20+ new lines "
          "(has %d)", count_lines(path));
    unlink(path);

    CHECK(logger_init(&lg, "/nonexistent_dir_cc/x/y.log", 4, 0, false) != 0,
          "unwritable path must fail cleanly");
    CHECK(logger_init(&lg, path, 0, 0, false) != 0, "capacity 0 rejected");
    unlink(path);
}

/* ---- IPC -------------------------------------------------------------- */

static void test_ipc(void)
{
    static const int rows[][4] = { {1,0,5,2}, {2,1,3,1}, {3,2,8,3} };
    JobSet js = make_jobs(rows, 3);
    IpcOptions opt;

    section("ipc (FR4): shared memory + semaphores, fault path, cleanup");
    ipc_cleanup_named(false);
    ipc_default_options(&opt);
    CHECK(ipc_run_demo(&js, &opt) == 0, "normal exchange");
    CHECK(!ipc_resources_present(), "objects left after a normal run");

    opt.fault = IPC_FAULT_CONSUMER_CRASH;
    opt.timeout_s = 1;
    CHECK(ipc_run_demo(&js, &opt) == -1, "simulated consumer crash must be "
          "reported as a failure");
    CHECK(!ipc_resources_present(), "objects left after the failure path");
    CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD,
          "no child may be left un-reaped");

    CHECK(ipc_run_demo(NULL, &opt) == -1, "NULL job set rejected");
}

/* ---------------------------------------------------------------------- */

int main(void)
{
    /* The demos print a lot; keep the unit-test summary readable. */
    test_util();
    test_job_manager();
    test_fcfs();
    test_fcfs_idle();
    test_rr();
    test_rr_arrival_order();
    test_priority();
    test_priority_tiebreak();
    test_inputs_not_modified();
    test_empty_jobset();
    test_banker();
    test_banker_unsafe_by_hand();
    test_banker_invalid();
    test_worker_classify();
    test_logger();
    test_ipc();
    test_worker_real_processes();

    printf("\n[unit] %d checks, %d failed -> %s\n", g_checks, g_failed,
           g_failed ? "FAIL" : "PASS");
    return g_failed ? 1 : 0;
}
