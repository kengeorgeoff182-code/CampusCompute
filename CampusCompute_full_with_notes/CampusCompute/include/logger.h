/*
 * logger.h - FR3: concurrent activity logger (bounded producer-consumer).
 *
 * Design
 *   * A circular buffer of 'capacity' LogEntry slots shared by any number of
 *     producer threads (callers of logger_log) and ONE consumer thread that
 *     appends entries to a log file.
 *   * One mutex protects every field that is shared (buffer indices, count,
 *     'closing', statistics).
 *   * Two condition variables:  not_full  - producers wait here when the
 *     buffer is full;  not_empty - the consumer waits here when it is empty.
 *     Every wait is inside a while-loop that re-checks its predicate, so
 *     spurious wake-ups are harmless and there is NO busy waiting.
 *   * Shutdown: logger_stop() sets 'closing' and broadcasts both condition
 *     variables. The consumer keeps draining until the buffer is empty AND
 *     closing is set, then exits and is joined. Producers blocked on a full
 *     buffer are released and get -1 ("logger closed") instead of hanging.
 *   * File I/O and artificial delays happen OUTSIDE the lock, so a slow disk
 *     never blocks producers that merely want to enqueue.
 */
#ifndef CC_LOGGER_H
#define CC_LOGGER_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

#define LOG_MSG_MAX          160
#define LOG_SRC_MAX          24
#define LOGGER_MAX_CAPACITY  4096
#define LOGGER_DEFAULT_PATH  "logs/campuscompute.log"

typedef enum { LOG_DEBUG = 0, LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;

typedef struct {
    struct timespec ts;             /* time the event was produced */
    LogLevel level;
    char source[LOG_SRC_MAX];
    char message[LOG_MSG_MAX];
} LogEntry;

typedef struct {
    unsigned long enqueued;         /* entries accepted into the queue   */
    unsigned long dequeued;         /* entries taken by the consumer     */
    unsigned long written;          /* entries successfully written      */
    unsigned long write_errors;
    unsigned long full_waits;       /* times a producer blocked: FULL    */
    unsigned long empty_waits;      /* times the consumer blocked: EMPTY */
    unsigned long rejected;         /* logger_log() after shutdown       */
    unsigned long max_depth;        /* high-water mark of the queue      */
} LoggerStats;

typedef struct {
    /* shared state, protected by 'lock' */
    LogEntry *buf;
    size_t capacity, head, tail, count;
    bool closing;
    LoggerStats stats;
    pthread_mutex_t lock;
    pthread_cond_t not_full;
    pthread_cond_t not_empty;
    /* owned by the consumer thread / set up before it starts */
    FILE *fp;
    char path[256];
    long start_offset;              /* file size when this session began */
    int consumer_delay_ms;          /* simulated slow I/O (0 = none)     */
    bool trace;                     /* print queue activity to stdout    */
    pthread_t consumer;
    bool consumer_started;
    bool stopped;                   /* logger_stop() already completed   */
    /* init progress, so logger_destroy can unwind any partial init */
    bool mutex_ok, not_full_ok, not_empty_ok;
} Logger;

/*
 * Allocate the queue, open 'path' for appending (creating its parent
 * directory if needed) and write a session header. 0 on success; -1 on
 * failure after printing a message (nothing is leaked).
 */
int logger_init(Logger *lg, const char *path, size_t capacity,
                int consumer_delay_ms, bool trace);

/* Start the consumer thread. 0 on success, -1 on failure. */
int logger_start(Logger *lg);

/*
 * Producer API (thread-safe, printf-style message). Blocks while the queue is
 * full. Returns 0 if the entry was queued, -1 if the logger is closed.
 */
int logger_log(Logger *lg, LogLevel level, const char *source,
               const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/*
 * Graceful shutdown: stop accepting entries, let the consumer drain what is
 * queued, join it, write a session footer. Safe to call once after init
 * (with or without logger_start). Returns 0 on success.
 */
int logger_stop(Logger *lg);

/* Free everything. Call after logger_stop (or after a failed logger_init). */
void logger_destroy(Logger *lg);

/* Copy of the statistics, taken under the lock. */
LoggerStats logger_get_stats(Logger *lg);

/* ---- demonstration / verification (logger_demo.c) ---- */

typedef struct {
    size_t capacity;
    int producers;
    int messages;               /* per producer */
    int producer_delay_ms;
    int consumer_delay_ms;
    const char *path;
    bool trace;
} LoggerDemoOpts;

void logger_demo_default_opts(LoggerDemoOpts *o);

/*
 * Run a producer-consumer demonstration and VERIFY it: counts, per-producer
 * FIFO order and the number of lines actually written to the log file.
 * Returns 0 if every check passed, -1 otherwise.
 */
int logger_run_demo(const LoggerDemoOpts *o);

#endif /* CC_LOGGER_H */
