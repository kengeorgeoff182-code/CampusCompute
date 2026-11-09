/*
 * job_manager.h - FR1: job representation, validation and lifecycle state.
 *
 * A JobSet is a plain value type (fixed-size array, no heap). Schedulers
 * receive a const pointer and work on a private copy, so one algorithm can
 * never corrupt the data used by another.
 */
#ifndef CC_JOB_MANAGER_H
#define CC_JOB_MANAGER_H

#include <stddef.h>

#define JM_MAX_JOBS     64
#define JM_MIN_ID       1
#define JM_MAX_ID       9999
#define JM_MAX_ARRIVAL  10000
#define JM_MIN_BURST    1
#define JM_MAX_BURST    1000
#define JM_MIN_PRIORITY 1       /* 1 = most urgent (lower number wins) */
#define JM_MAX_PRIORITY 100

/* Process lifecycle: NEW -> READY -> RUNNING -> (READY ...) -> COMPLETED */
typedef enum {
    JOB_NEW = 0,        /* submitted, not yet arrived / not yet queued */
    JOB_READY,          /* arrived and waiting for the CPU             */
    JOB_RUNNING,        /* currently on the CPU                        */
    JOB_COMPLETED       /* finished                                    */
} JobStatus;

typedef struct {
    /* user-supplied attributes */
    int id;
    int arrival;
    int burst;
    int priority;
    /* lifecycle / results, filled in by a scheduler run */
    JobStatus status;
    int remaining;          /* CPU time still needed                      */
    int start_time;         /* first time on CPU, -1 if never ran         */
    int completion_time;    /* -1 until completed                         */
    int turnaround;         /* completion - arrival                       */
    int waiting;            /* turnaround - burst                         */
} Job;

typedef struct {
    Job jobs[JM_MAX_JOBS];
    int count;
} JobSet;

void jobset_init(JobSet *js);

/* Check attribute ranges. Returns 0 if valid, else -1 and fills 'err'. */
int job_validate(int id, int arrival, int burst, int priority,
                 char *err, size_t errlen);

/* Validate and append a job; rejects duplicates and overflow. */
int jobset_add(JobSet *js, int id, int arrival, int burst, int priority,
               char *err, size_t errlen);

/*
 * Parse "ID,ARRIVAL,BURST,PRIORITY" (commas and/or spaces) and add it.
 * Used for --job on the command line and for menu input.
 */
int jobset_add_spec(JobSet *js, const char *spec, char *err, size_t errlen);

/*
 * Load a job file: one job per line "ID ARRIVAL BURST PRIORITY", '#' starts
 * a comment. All-or-nothing: on error 'js' is left unchanged and 'err'
 * names the offending line.
 */
int jobset_load_file(JobSet *js, const char *path, char *err, size_t errlen);

/* Built-in demonstration job set (6 jobs, ties and an idle gap). */
void jobset_load_sample(JobSet *js);

/* Put every job back to NEW with remaining = burst. */
void jobset_reset_runtime(JobSet *js);

const char *job_status_name(JobStatus s);

/* Print the input attributes (and current status) as a table. */
void jobset_print(const JobSet *js, const char *title);

#endif /* CC_JOB_MANAGER_H */
