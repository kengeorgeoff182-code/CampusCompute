/*
 * scheduler.c - FR2: FCFS, Round Robin and non-preemptive Priority scheduling.
 *
 * Every algorithm is a discrete-time simulation over a private copy of the
 * input JobSet. Time only advances by running a job or by jumping over an
 * idle gap to the next arrival.
 */
#include "scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *sched_algo_name(SchedAlgo a)
{
    switch (a) {
    case ALGO_FCFS:     return "FCFS";
    case ALGO_RR:       return "Round Robin";
    case ALGO_PRIORITY: return "Priority (non-preemptive)";
    default:            return "?";
    }
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* Fill idx[] with job indices ordered by (arrival, id). Insertion sort:
 * n <= JM_MAX_JOBS and the sort must be stable and deterministic. */
static void sort_by_arrival(const JobSet *js, int idx[])
{
    for (int i = 0; i < js->count; i++) {
        int k = i;

        while (k > 0) {
            const Job *a = &js->jobs[idx[k - 1]];
            const Job *b = &js->jobs[i];

            if (a->arrival < b->arrival ||
                (a->arrival == b->arrival && a->id < b->id))
                break;
            idx[k] = idx[k - 1];
            k--;
        }
        idx[k] = i;
    }
}

/* Append (or extend) a Gantt segment. Adjacent segments of the same job are
 * merged so a job that keeps the CPU across quanta shows as one block. */
static int add_segment(ScheduleResult *r, int job_id, int start, int end)
{
    if (end <= start)
        return 0;
    if (r->nsegs > 0) {
        GanttSeg *last = &r->segs[r->nsegs - 1];

        if (last->job_id == job_id && last->end == start) {
            last->end = end;
            return 0;
        }
    }
    if (r->nsegs == r->cap_segs) {
        int ncap = r->cap_segs ? r->cap_segs * 2 : 32;
        GanttSeg *p = realloc(r->segs, (size_t)ncap * sizeof *p);

        if (p == NULL)
            return -1;
        r->segs = p;
        r->cap_segs = ncap;
    }
    r->segs[r->nsegs].job_id = job_id;
    r->segs[r->nsegs].start = start;
    r->segs[r->nsegs].end = end;
    r->nsegs++;
    return 0;
}

static void finish_job(ScheduleResult *r, Job *j, int t)
{
    j->status = JOB_COMPLETED;
    j->remaining = 0;
    j->completion_time = t;
    j->turnaround = t - j->arrival;          /* TAT = CT - AT  */
    j->waiting = j->turnaround - j->burst;   /* WT  = TAT - BT */
    r->completion_order[r->ncompleted++] = j->id;
}

/* ------------------------------------------------------------------ */
/* FCFS                                                                */
/* ------------------------------------------------------------------ */

static int run_fcfs(ScheduleResult *r)
{
    JobSet *js = &r->jobs;
    int idx[JM_MAX_JOBS];
    int t = 0;

    sort_by_arrival(js, idx);
    for (int k = 0; k < js->count; k++) {
        Job *j = &js->jobs[idx[k]];

        if (t < j->arrival) {                       /* CPU idle gap */
            if (add_segment(r, SEG_IDLE, t, j->arrival) != 0)
                return -1;
            t = j->arrival;
        }
        j->status = JOB_RUNNING;
        j->start_time = t;
        if (add_segment(r, j->id, t, t + j->burst) != 0)
            return -1;
        t += j->burst;
        finish_job(r, j, t);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Non-preemptive priority                                             */
/* ------------------------------------------------------------------ */

/* Priority key: lower priority number, then earlier arrival, then lower ID. */
static int priority_better(const Job *a, const Job *b)
{
    if (a->priority != b->priority)
        return a->priority < b->priority;
    if (a->arrival != b->arrival)
        return a->arrival < b->arrival;
    return a->id < b->id;
}

static int run_priority(ScheduleResult *r)
{
    JobSet *js = &r->jobs;
    int t = 0, done = 0;

    while (done < js->count) {
        Job *pick = NULL;
        int next_arrival = -1;

        for (int i = 0; i < js->count; i++) {
            Job *j = &js->jobs[i];

            if (j->status == JOB_COMPLETED)
                continue;
            if (j->arrival <= t) {
                j->status = JOB_READY;
                if (pick == NULL || priority_better(j, pick))
                    pick = j;
            } else if (next_arrival < 0 || j->arrival < next_arrival) {
                next_arrival = j->arrival;
            }
        }
        if (pick == NULL) {                         /* nobody ready: idle */
            if (next_arrival < 0)
                return -1;                          /* cannot happen */
            if (add_segment(r, SEG_IDLE, t, next_arrival) != 0)
                return -1;
            t = next_arrival;
            continue;
        }
        pick->status = JOB_RUNNING;
        pick->start_time = t;
        if (add_segment(r, pick->id, t, t + pick->burst) != 0)
            return -1;
        t += pick->burst;
        finish_job(r, pick, t);
        done++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Round Robin                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int q[JM_MAX_JOBS];     /* a job is in the queue at most once */
    int head, count;
} ReadyQueue;

static void rq_push(ReadyQueue *rq, int i)
{
    rq->q[(rq->head + rq->count) % JM_MAX_JOBS] = i;
    rq->count++;
}

static int rq_pop(ReadyQueue *rq)
{
    int i = rq->q[rq->head];

    rq->head = (rq->head + 1) % JM_MAX_JOBS;
    rq->count--;
    return i;
}

/* Move every job with arrival <= t from the arrival-sorted list to the
 * ready queue. */
static void admit(JobSet *js, const int idx[], int *next, ReadyQueue *rq,
                  int t)
{
    while (*next < js->count && js->jobs[idx[*next]].arrival <= t) {
        js->jobs[idx[*next]].status = JOB_READY;
        rq_push(rq, idx[*next]);
        (*next)++;
    }
}

static int run_rr(ScheduleResult *r)
{
    JobSet *js = &r->jobs;
    int idx[JM_MAX_JOBS];
    ReadyQueue rq = { .head = 0, .count = 0 };
    int next = 0, t = 0, done = 0;

    sort_by_arrival(js, idx);
    admit(js, idx, &next, &rq, t);

    while (done < js->count) {
        Job *j;
        int run, i;

        if (rq.count == 0) {                        /* idle until next job */
            int na;

            if (next >= js->count)
                return -1;                          /* cannot happen */
            na = js->jobs[idx[next]].arrival;
            if (add_segment(r, SEG_IDLE, t, na) != 0)
                return -1;
            t = na;
            admit(js, idx, &next, &rq, t);
            continue;
        }
        i = rq_pop(&rq);
        j = &js->jobs[i];
        run = j->remaining < r->quantum ? j->remaining : r->quantum;

        j->status = JOB_RUNNING;
        if (j->start_time < 0)
            j->start_time = t;
        if (add_segment(r, j->id, t, t + run) != 0)
            return -1;
        t += run;
        j->remaining -= run;

        /* Arrivals up to and including 't' queue BEFORE the pre-empted job. */
        admit(js, idx, &next, &rq, t);
        if (j->remaining == 0) {
            finish_job(r, j, t);
            done++;
        } else {
            j->status = JOB_READY;
            rq_push(&rq, i);
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                          */
/* ------------------------------------------------------------------ */

int sched_run(SchedAlgo algo, int quantum, const JobSet *input,
              ScheduleResult *out, char *err, size_t errlen)
{
    int rc = -1;

    memset(out, 0, sizeof *out);
    if (input == NULL || input->count <= 0) {
        snprintf(err, errlen, "no jobs to schedule (add jobs first)");
        return -1;
    }
    if (algo < 0 || algo >= ALGO_COUNT) {
        snprintf(err, errlen, "unknown scheduling algorithm");
        return -1;
    }
    if (algo == ALGO_RR && (quantum < RR_MIN_QUANTUM ||
                            quantum > RR_MAX_QUANTUM)) {
        snprintf(err, errlen,
                 "invalid time quantum %d (expected an integer %d..%d)",
                 quantum, RR_MIN_QUANTUM, RR_MAX_QUANTUM);
        return -1;
    }

    out->algo = algo;
    out->quantum = (algo == ALGO_RR) ? quantum : 0;
    out->jobs = *input;                     /* private copy */
    jobset_reset_runtime(&out->jobs);

    switch (algo) {
    case ALGO_FCFS:     rc = run_fcfs(out);     break;
    case ALGO_RR:       rc = run_rr(out);       break;
    case ALGO_PRIORITY: rc = run_priority(out); break;
    default:            break;
    }
    if (rc != 0) {
        snprintf(err, errlen, "scheduler failed (out of memory or internal "
                              "error)");
        sched_result_free(out);
        return -1;
    }

    /* aggregate metrics */
    for (int i = 0; i < out->jobs.count; i++) {
        const Job *j = &out->jobs.jobs[i];

        out->sum_turnaround += j->turnaround;
        out->sum_waiting += j->waiting;
        if (j->completion_time > out->finish_time)
            out->finish_time = j->completion_time;
    }
    for (int i = 0; i < out->nsegs; i++)
        if (out->segs[i].job_id == SEG_IDLE)
            out->idle_time += out->segs[i].end - out->segs[i].start;
    out->avg_turnaround = (double)out->sum_turnaround / out->jobs.count;
    out->avg_waiting = (double)out->sum_waiting / out->jobs.count;
    out->cpu_utilization = out->finish_time > 0
        ? 100.0 * (out->finish_time - out->idle_time) / out->finish_time
        : 0.0;
    return 0;
}

void sched_result_free(ScheduleResult *r)
{
    free(r->segs);
    r->segs = NULL;
    r->nsegs = r->cap_segs = 0;
}

/* ------------------------------------------------------------------ */
/* independent invariant checker                                       */
/* ------------------------------------------------------------------ */

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)

/* Does job a outrank job b under the non-preemptive policies? */
static int outranks(SchedAlgo algo, const Job *a, const Job *b)
{
    if (algo == ALGO_PRIORITY)
        return priority_better(a, b);
    if (a->arrival != b->arrival)           /* FCFS */
        return a->arrival < b->arrival;
    return a->id < b->id;
}

int sched_verify_result(const ScheduleResult *r, char *err, size_t errlen)
{
    const JobSet *js = &r->jobs;

    if (r->nsegs == 0 || r->segs[0].start != 0)
        FAIL("Gantt chart must start at time 0");

    for (int s = 0; s < r->nsegs; s++) {
        const GanttSeg *g = &r->segs[s];

        if (g->end <= g->start)
            FAIL("empty or inverted Gantt segment %d", s);
        if (s > 0 && g->start != r->segs[s - 1].end)
            FAIL("Gantt gap/overlap before segment %d", s);
    }

    for (int i = 0; i < js->count; i++) {
        const Job *j = &js->jobs[i];
        int cpu = 0, nseg = 0, first = -1, last_end = -1;

        if (j->status != JOB_COMPLETED)
            FAIL("job %d not COMPLETED", j->id);
        for (int s = 0; s < r->nsegs; s++) {
            const GanttSeg *g = &r->segs[s];

            if (g->job_id != j->id)
                continue;
            if (g->start < j->arrival)
                FAIL("job %d ran at %d before arrival %d", j->id, g->start,
                     j->arrival);
            if (first < 0)
                first = g->start;
            cpu += g->end - g->start;
            last_end = g->end;
            nseg++;
        }
        if (cpu != j->burst)
            FAIL("job %d received %d CPU units, burst is %d", j->id, cpu,
                 j->burst);
        if (j->completion_time != last_end)
            FAIL("job %d completion %d != end of last segment %d", j->id,
                 j->completion_time, last_end);
        if (j->start_time != first)
            FAIL("job %d start %d != first segment %d", j->id, j->start_time,
                 first);
        if (j->turnaround != j->completion_time - j->arrival)
            FAIL("job %d turnaround formula violated", j->id);
        if (j->waiting != j->turnaround - j->burst || j->waiting < 0)
            FAIL("job %d waiting formula violated", j->id);
        if (r->algo != ALGO_RR && nseg != 1)
            FAIL("job %d was split under a non-preemptive policy", j->id);
    }

    /* Work conservation: never idle while a job has arrived and is unfinished. */
    for (int s = 0; s < r->nsegs; s++) {
        const GanttSeg *g = &r->segs[s];

        if (g->job_id != SEG_IDLE)
            continue;
        for (int i = 0; i < js->count; i++)
            if (js->jobs[i].arrival < g->end &&
                js->jobs[i].completion_time > g->start)
                FAIL("CPU idle [%d,%d) while job %d was ready", g->start,
                     g->end, js->jobs[i].id);
    }

    /* Non-preemptive policies: at every dispatch no ready job outranks the
     * chosen one. */
    if (r->algo != ALGO_RR) {
        for (int s = 0; s < r->nsegs; s++) {
            const GanttSeg *g = &r->segs[s];
            const Job *chosen = NULL;

            if (g->job_id == SEG_IDLE)
                continue;
            for (int i = 0; i < js->count; i++)
                if (js->jobs[i].id == g->job_id)
                    chosen = &js->jobs[i];
            for (int i = 0; i < js->count; i++) {
                const Job *k = &js->jobs[i];

                if (k == chosen || k->arrival > g->start ||
                    k->completion_time <= g->start)
                    continue;
                if (outranks(r->algo, k, chosen))
                    FAIL("job %d should have run before job %d at time %d",
                         k->id, chosen->id, g->start);
            }
        }
    }
    return 0;
}
