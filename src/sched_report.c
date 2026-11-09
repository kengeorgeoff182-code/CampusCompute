/*
 * sched_report.c - FR2: printing of schedules (Gantt chart, per-job metrics)
 * and the side-by-side comparison of all three policies.
 */
#include "scheduler.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#define GANTT_SEGS_PER_ROW 8

static void print_rule_text(const ScheduleResult *r)
{
    switch (r->algo) {
    case ALGO_FCFS:
        puts("Rule: jobs run in order of arrival time, non-preemptive.\n"
             "Tie-break: equal arrival time -> lower job ID first.");
        break;
    case ALGO_RR:
        printf("Rule: preemptive, FIFO ready queue, TIME QUANTUM = %d.\n"
               "Tie-break: simultaneous arrivals are queued by lower job ID; "
               "a job arriving at or\nbefore the end of a slice is queued "
               "BEFORE the pre-empted job is re-queued.\n", r->quantum);
        break;
    case ALGO_PRIORITY:
        puts("Rule: non-preemptive. LOWER priority number = HIGHER priority "
             "(1 is most urgent).\n"
             "Tie-break: equal priority -> earlier arrival time; still equal "
             "-> lower job ID.");
        break;
    default:
        break;
    }
}

static int digits(int v)
{
    int n = 1;

    while (v >= 10) {
        v /= 10;
        n++;
    }
    return n;
}

void sched_print_gantt(const ScheduleResult *r)
{
    puts("Gantt chart (cells not drawn to scale):");
    for (int base = 0; base < r->nsegs; base += GANTT_SEGS_PER_ROW) {
        int end = base + GANTT_SEGS_PER_ROW;

        if (end > r->nsegs)
            end = r->nsegs;

        /* row 1: job labels between '|' separators */
        printf("  |");
        for (int s = base; s < end; s++) {
            char label[16];
            int w, len, pad_l, pad_r;

            if (r->segs[s].job_id == SEG_IDLE)
                snprintf(label, sizeof label, "idle");
            else
                snprintf(label, sizeof label, "J%d", r->segs[s].job_id);
            len = (int)strlen(label);
            w = len + 2;
            if (w < digits(r->segs[s].start) + 1)
                w = digits(r->segs[s].start) + 1;
            pad_l = (w - len) / 2;
            pad_r = w - len - pad_l;
            printf("%*s%s%*s|", pad_l, "", label, pad_r, "");
        }
        putchar('\n');

        /* row 2: start time of each cell, aligned under its left '|' */
        printf("  ");
        for (int s = base; s < end; s++) {
            char label[16];
            int len, w;

            if (r->segs[s].job_id == SEG_IDLE)
                len = 4;
            else {
                snprintf(label, sizeof label, "J%d", r->segs[s].job_id);
                len = (int)strlen(label);
            }
            w = len + 2;
            if (w < digits(r->segs[s].start) + 1)
                w = digits(r->segs[s].start) + 1;
            printf("%-*d", w + 1, r->segs[s].start);
        }
        printf("%d\n", r->segs[end - 1].end);
    }
}

static void print_order(const char *label, const int *ids, int n)
{
    printf("%s", label);
    for (int i = 0; i < n; i++) {
        if (i > 0 && i % 12 == 0)
            printf("\n%*s", (int)strlen(label), "");
        printf("%sJ%d", i ? " -> " : "", ids[i]);
    }
    putchar('\n');
}

void sched_print_result(const ScheduleResult *r)
{
    int dispatch[1 << 12];
    int nd = 0;
    char title[96];

    if (r->algo == ALGO_RR)
        snprintf(title, sizeof title, "%s Scheduling (quantum = %d)",
                 sched_algo_name(r->algo), r->quantum);
    else
        snprintf(title, sizeof title, "%s Scheduling",
                 sched_algo_name(r->algo));
    cc_banner(title);
    print_rule_text(r);
    puts("Formulas: Turnaround = Completion - Arrival;  "
         "Waiting = Turnaround - Burst\n");

    for (int i = 0; i < r->nsegs && nd < (int)(sizeof dispatch / sizeof *dispatch);
         i++)
        if (r->segs[i].job_id != SEG_IDLE)
            dispatch[nd++] = r->segs[i].job_id;
    print_order("Execution order (CPU dispatch sequence): ", dispatch, nd);
    print_order("Completion order:                        ",
                r->completion_order, r->ncompleted);
    putchar('\n');
    sched_print_gantt(r);

    printf("\n  %-5s %7s %6s %5s %6s %11s %11s %8s  %-9s\n", "Job",
           "Arrival", "Burst", "Prio", "Start", "Completion", "Turnaround",
           "Waiting", "Status");
    for (int i = 0; i < r->jobs.count; i++) {
        const Job *j = &r->jobs.jobs[i];

        printf("  J%-4d %7d %6d %5d %6d %11d %11d %8d  %-9s\n", j->id,
               j->arrival, j->burst, j->priority, j->start_time,
               j->completion_time, j->turnaround, j->waiting,
               job_status_name(j->status));
    }
    printf("\n  Average turnaround time = %.2f\n", r->avg_turnaround);
    printf("  Average waiting time    = %.2f\n", r->avg_waiting);
    printf("  Finish time = %d, CPU idle time = %d, CPU utilisation = "
           "%.1f%%\n", r->finish_time, r->idle_time, r->cpu_utilization);
}

int sched_run_and_print(SchedAlgo algo, int quantum, const JobSet *jobs)
{
    ScheduleResult r;
    char err[200];

    if (sched_run(algo, quantum, jobs, &r, err, sizeof err) != 0) {
        cc_errf("%s", err);
        return -1;
    }
    if (sched_verify_result(&r, err, sizeof err) != 0) {
        cc_errf("internal consistency check failed: %s", err);
        sched_result_free(&r);
        return -1;
    }
    sched_print_result(&r);
    printf("  Consistency check (Gantt vs. metrics): PASSED\n");
    sched_result_free(&r);
    return 0;
}

int sched_compare(const JobSet *jobs, int quantum)
{
    ScheduleResult res[ALGO_COUNT];
    JobSet before = *jobs;
    char err[200], rrname[48];
    int ok = 0, rc = -1;

    memset(res, 0, sizeof res);
    for (int a = 0; a < ALGO_COUNT; a++) {
        if (sched_run((SchedAlgo)a, quantum, jobs, &res[a], err,
                      sizeof err) != 0) {
            cc_errf("%s", err);
            goto out;
        }
        ok++;
        if (sched_verify_result(&res[a], err, sizeof err) != 0) {
            cc_errf("%s consistency check failed: %s",
                    sched_algo_name((SchedAlgo)a), err);
            goto out;
        }
    }

    cc_banner("Scheduling Comparison (same job set, three policies)");
    jobset_print(jobs, "Input job set");
    puts("\nPolicies: FCFS (arrival order); Round Robin; Priority "
         "(non-preemptive, lower number = higher priority,\n"
         "tie -> earlier arrival -> lower job ID).");
    printf("Round Robin time quantum = %d\n\n", quantum);

    for (int a = 0; a < ALGO_COUNT; a++) {
        printf("%s:\n", a == ALGO_RR ? "Round Robin" :
               sched_algo_name((SchedAlgo)a));
        sched_print_gantt(&res[a]);
        putchar('\n');
    }

    snprintf(rrname, sizeof rrname, "Round Robin (q=%d)", quantum);
    printf("%-27s %15s %12s %7s %5s %9s\n", "Algorithm", "Avg Turnaround",
           "Avg Waiting", "Finish", "Idle", "CPU Util");
    for (int a = 0; a < ALGO_COUNT; a++) {
        printf("%-27s %15.2f %12.2f %7d %5d %8.1f%%\n",
               a == ALGO_RR ? rrname : sched_algo_name((SchedAlgo)a),
               res[a].avg_turnaround, res[a].avg_waiting, res[a].finish_time,
               res[a].idle_time, res[a].cpu_utilization);
    }

    printf("\nPer-job Waiting / Turnaround:\n  %-5s", "Job");
    printf(" %12s %12s %12s\n", "FCFS", "RR", "Priority");
    for (int i = 0; i < jobs->count; i++) {
        printf("  J%-4d", jobs->jobs[i].id);
        for (int a = 0; a < ALGO_COUNT; a++) {
            char cell[24];

            snprintf(cell, sizeof cell, "%d / %d", res[a].jobs.jobs[i].waiting,
                     res[a].jobs.jobs[i].turnaround);
            printf(" %12s", cell);
        }
        putchar('\n');
    }

    /* Best policy per metric (integer sums avoid floating-point ties). */
    for (int metric = 0; metric < 2; metric++) {
        int best = -1;

        for (int a = 0; a < ALGO_COUNT; a++) {
            int v = metric ? res[a].sum_turnaround : res[a].sum_waiting;
            int b = best < 0 ? 0 : (metric ? res[best].sum_turnaround
                                           : res[best].sum_waiting);
            if (best < 0 || v < b)
                best = a;
        }
        printf("%s: ", metric ? "Lowest average turnaround" :
                                "Lowest average waiting   ");
        for (int a = 0; a < ALGO_COUNT; a++) {
            int v = metric ? res[a].sum_turnaround : res[a].sum_waiting;
            int b = metric ? res[best].sum_turnaround : res[best].sum_waiting;

            if (v == b)
                printf("%s  ", a == ALGO_RR ? "Round Robin" :
                       sched_algo_name((SchedAlgo)a));
        }
        putchar('\n');
    }

    printf("\nOriginal job data unchanged after all three runs: %s\n",
           memcmp(&before, jobs, sizeof before) == 0 ? "YES" : "NO (BUG)");
    printf("Consistency checks (Gantt vs. metrics) for all policies: "
           "PASSED\n");
    rc = memcmp(&before, jobs, sizeof before) == 0 ? 0 : -1;

out:
    for (int a = 0; a < ok + (rc != 0 && ok < ALGO_COUNT ? 1 : 0) &&
                    a < ALGO_COUNT; a++)
        sched_result_free(&res[a]);
    return rc;
}
