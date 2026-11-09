/*
 * scheduler.h - FR2: FCFS, Round Robin and non-preemptive Priority scheduling.
 *
 * Semantics (also printed by the program):
 *   FCFS      Order of arrival; tie on arrival -> lower job ID. Non-preemptive.
 *   RR        FIFO ready queue with a fixed time quantum. Jobs that arrive at
 *             or before the instant a slice ends are queued BEFORE the
 *             pre-empted job is re-queued. Simultaneous arrivals are queued by
 *             lower job ID.
 *   PRIORITY  Non-preemptive. LOWER number = HIGHER priority (1 is most
 *             urgent). Tie -> earlier arrival, then lower job ID.
 *
 *   Turnaround = Completion - Arrival      Waiting = Turnaround - Burst
 *
 * When no job is ready the CPU is idle until the next arrival; idle time is
 * recorded in the Gantt chart as segment job_id == SEG_IDLE.
 */
#ifndef CC_SCHEDULER_H
#define CC_SCHEDULER_H

#include <stddef.h>
#include "job_manager.h"

#define SEG_IDLE        0       /* job_id value for an idle Gantt segment */
#define RR_MIN_QUANTUM  1
#define RR_MAX_QUANTUM  1000
#define RR_DEFAULT_QUANTUM 3

typedef enum { ALGO_FCFS = 0, ALGO_RR, ALGO_PRIORITY, ALGO_COUNT } SchedAlgo;

typedef struct {
    int job_id;             /* SEG_IDLE for CPU idle */
    int start;
    int end;                /* exclusive */
} GanttSeg;

typedef struct {
    SchedAlgo algo;
    int quantum;                    /* RR only, otherwise 0 */
    JobSet jobs;                    /* private working copy with results */
    GanttSeg *segs;                 /* heap array, freed by sched_result_free */
    int nsegs;
    int cap_segs;
    int completion_order[JM_MAX_JOBS];
    int ncompleted;
    int finish_time;                /* time the last job completed */
    int idle_time;                  /* CPU idle time before finish_time */
    int sum_turnaround;
    int sum_waiting;
    double avg_turnaround;
    double avg_waiting;
    double cpu_utilization;         /* percent, busy / finish_time */
} ScheduleResult;

const char *sched_algo_name(SchedAlgo a);   /* "FCFS", "Round Robin", ... */

/*
 * Run one algorithm on a COPY of 'input' (input is never modified).
 * 'quantum' is only used (and validated) for ALGO_RR.
 * Returns 0 on success, -1 with a message in 'err'.
 * The result must be released with sched_result_free().
 */
int sched_run(SchedAlgo algo, int quantum, const JobSet *input,
              ScheduleResult *out, char *err, size_t errlen);

void sched_result_free(ScheduleResult *r);

/*
 * Independent invariant checker: every job completed, Gantt segments contiguous
 * and non-overlapping, per-job CPU time in the Gantt equals its burst, no job
 * ran before it arrived, and the TAT/WT formulas hold. Returns 0 if all hold,
 * else -1 with the first violated invariant in 'err'.
 */
int sched_verify_result(const ScheduleResult *r, char *err, size_t errlen);

/* ---- reporting (sched_report.c) ---- */

/* Policy rule text, Gantt chart, per-job table, averages. */
void sched_print_result(const ScheduleResult *r);

/* Gantt chart only. */
void sched_print_gantt(const ScheduleResult *r);

/*
 * Run all three algorithms on the same job set and print a comparison.
 * Also confirms the original job data was left untouched.
 * Returns 0 on success, -1 on error (message printed to stderr).
 */
int sched_compare(const JobSet *jobs, int quantum);

/* Run one algorithm, verify and print it. Returns 0 / -1. */
int sched_run_and_print(SchedAlgo algo, int quantum, const JobSet *jobs);

#endif /* CC_SCHEDULER_H */
