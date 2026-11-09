/*
 * job_manager.c - FR1: job representation, validation and lifecycle state.
 */
#include "job_manager.h"
#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

void jobset_init(JobSet *js)
{
    memset(js, 0, sizeof *js);
}

int job_validate(int id, int arrival, int burst, int priority,
                 char *err, size_t errlen)
{
    if (id < JM_MIN_ID || id > JM_MAX_ID) {
        snprintf(err, errlen, "job ID %d out of range (expected %d..%d)",
                 id, JM_MIN_ID, JM_MAX_ID);
        return -1;
    }
    if (arrival < 0 || arrival > JM_MAX_ARRIVAL) {
        snprintf(err, errlen,
                 "job %d: arrival time %d out of range (expected 0..%d)",
                 id, arrival, JM_MAX_ARRIVAL);
        return -1;
    }
    if (burst < JM_MIN_BURST || burst > JM_MAX_BURST) {
        snprintf(err, errlen,
                 "job %d: CPU burst %d out of range (expected %d..%d)",
                 id, burst, JM_MIN_BURST, JM_MAX_BURST);
        return -1;
    }
    if (priority < JM_MIN_PRIORITY || priority > JM_MAX_PRIORITY) {
        snprintf(err, errlen,
                 "job %d: priority %d out of range (expected %d..%d, "
                 "1 = most urgent)",
                 id, priority, JM_MIN_PRIORITY, JM_MAX_PRIORITY);
        return -1;
    }
    return 0;
}

int jobset_add(JobSet *js, int id, int arrival, int burst, int priority,
               char *err, size_t errlen)
{
    Job *j;

    if (job_validate(id, arrival, burst, priority, err, errlen) != 0)
        return -1;
    if (js->count >= JM_MAX_JOBS) {
        snprintf(err, errlen, "too many jobs (maximum is %d)", JM_MAX_JOBS);
        return -1;
    }
    for (int i = 0; i < js->count; i++) {
        if (js->jobs[i].id == id) {
            snprintf(err, errlen, "duplicate job ID %d", id);
            return -1;
        }
    }
    j = &js->jobs[js->count++];
    memset(j, 0, sizeof *j);
    j->id = id;
    j->arrival = arrival;
    j->burst = burst;
    j->priority = priority;
    j->status = JOB_NEW;
    j->remaining = burst;
    j->start_time = -1;
    j->completion_time = -1;
    return 0;
}

/*
 * Split a job line into exactly four integers. Commas count as spaces and
 * '#' starts a comment. Returns 1 if a job was parsed, 0 for a blank/comment
 * line, -1 on a syntax error (message in err).
 */
static int parse_job_line(const char *line, int vals[4], char *err,
                          size_t errlen)
{
    static const char *const names[4] = { "job ID", "arrival time",
                                          "CPU burst", "priority" };
    char buf[256];
    char *tok, *save = NULL;
    int n = 0;

    snprintf(buf, sizeof buf, "%s", line);
    for (char *p = buf; *p; p++) {
        if (*p == '#' || *p == '\n' || *p == '\r') {
            *p = '\0';
            break;
        }
        if (*p == ',' || *p == '\t')
            *p = ' ';
    }
    for (tok = strtok_r(buf, " ", &save); tok != NULL;
         tok = strtok_r(NULL, " ", &save)) {
        long v;
        int rc;

        if (n == 4) {
            snprintf(err, errlen,
                     "too many fields (expected: ID ARRIVAL BURST PRIORITY)");
            return -1;
        }
        rc = cc_parse_long(tok, INT_MIN, INT_MAX, &v);
        if (rc != 0) {
            snprintf(err, errlen, "%s '%s' is not a valid integer",
                     names[n], tok);
            return -1;
        }
        vals[n++] = (int)v;
    }
    if (n == 0)
        return 0;
    if (n != 4) {
        snprintf(err, errlen,
                 "expected 4 fields (ID ARRIVAL BURST PRIORITY), found %d", n);
        return -1;
    }
    return 1;
}

int jobset_add_spec(JobSet *js, const char *spec, char *err, size_t errlen)
{
    int v[4];
    int rc = parse_job_line(spec, v, err, errlen);

    if (rc < 0)
        return -1;
    if (rc == 0) {
        snprintf(err, errlen, "empty job specification");
        return -1;
    }
    return jobset_add(js, v[0], v[1], v[2], v[3], err, errlen);
}

int jobset_load_file(JobSet *js, const char *path, char *err, size_t errlen)
{
    FILE *fp = fopen(path, "r");
    JobSet tmp;
    char line[256], why[160];
    int lineno = 0;

    if (fp == NULL) {
        snprintf(err, errlen, "cannot open job file '%s': %s", path,
                 strerror(errno));
        return -1;
    }
    jobset_init(&tmp);
    while (fgets(line, sizeof line, fp) != NULL) {
        int v[4], rc;

        lineno++;
        if (strchr(line, '\n') == NULL && !feof(fp)) {
            snprintf(err, errlen, "%s: line %d is too long", path, lineno);
            fclose(fp);
            return -1;
        }
        rc = parse_job_line(line, v, why, sizeof why);
        if (rc == 0)
            continue;
        if (rc < 0 || jobset_add(&tmp, v[0], v[1], v[2], v[3], why,
                                 sizeof why) != 0) {
            snprintf(err, errlen, "%s: line %d: %s", path, lineno, why);
            fclose(fp);
            return -1;
        }
    }
    if (ferror(fp)) {
        snprintf(err, errlen, "cannot read job file '%s': %s", path,
                 strerror(errno));
        fclose(fp);
        return -1;
    }
    fclose(fp);
    if (tmp.count == 0) {
        snprintf(err, errlen, "%s: no jobs found", path);
        return -1;
    }
    *js = tmp;
    return 0;
}

void jobset_load_sample(JobSet *js)
{
    /* id arrival burst priority -- includes same-arrival jobs (J3,J4),
     * equal priorities (J2,J4 / J1,J5) and a CPU-idle gap before J6. */
    static const int sample[][4] = {
        { 1,  0, 5, 2 },
        { 2,  1, 3, 1 },
        { 3,  2, 8, 3 },
        { 4,  2, 2, 1 },
        { 5,  3, 4, 2 },
        { 6, 25, 3, 1 },
    };
    char err[128];

    jobset_init(js);
    for (size_t i = 0; i < sizeof sample / sizeof sample[0]; i++)
        (void)jobset_add(js, sample[i][0], sample[i][1], sample[i][2],
                         sample[i][3], err, sizeof err);
}

void jobset_reset_runtime(JobSet *js)
{
    for (int i = 0; i < js->count; i++) {
        Job *j = &js->jobs[i];

        j->status = JOB_NEW;
        j->remaining = j->burst;
        j->start_time = -1;
        j->completion_time = -1;
        j->turnaround = 0;
        j->waiting = 0;
    }
}

const char *job_status_name(JobStatus s)
{
    switch (s) {
    case JOB_NEW:       return "NEW";
    case JOB_READY:     return "READY";
    case JOB_RUNNING:   return "RUNNING";
    case JOB_COMPLETED: return "COMPLETED";
    }
    return "UNKNOWN";
}

void jobset_print(const JobSet *js, const char *title)
{
    printf("%s (%d job%s)\n", title, js->count, js->count == 1 ? "" : "s");
    printf("  %-5s %8s %6s %9s  %-9s\n", "Job", "Arrival", "Burst",
           "Priority", "Status");
    for (int i = 0; i < js->count; i++) {
        const Job *j = &js->jobs[i];

        printf("  J%-4d %8d %6d %9d  %-9s\n", j->id, j->arrival, j->burst,
               j->priority, job_status_name(j->status));
    }
}
