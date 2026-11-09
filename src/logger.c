/*
 * logger.c - FR3: bounded producer-consumer activity logger (see logger.h for
 * the design summary).
 *
 * Lock order / ownership
 *   - lg->lock guards: buf, head, tail, count, closing, stats.
 *   - lg->fp is only touched by the consumer thread while it runs; the
 *     session header (init) and footer (stop, after join) are written by the
 *     calling thread when no consumer is running, so no extra lock is needed.
 *   - stdout trace lines are printed while holding lg->lock so that the order
 *     of trace lines is exactly the order of queue operations.
 */
#include "logger.h"
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static const char *level_name(LogLevel l)
{
    switch (l) {
    case LOG_DEBUG: return "DEBUG";
    case LOG_INFO:  return "INFO";
    case LOG_WARN:  return "WARN";
    case LOG_ERROR: return "ERROR";
    }
    return "?";
}

void logger_destroy(Logger *lg)
{
    if (lg->fp != NULL) {
        fclose(lg->fp);
        lg->fp = NULL;
    }
    if (lg->not_empty_ok) {
        pthread_cond_destroy(&lg->not_empty);
        lg->not_empty_ok = false;
    }
    if (lg->not_full_ok) {
        pthread_cond_destroy(&lg->not_full);
        lg->not_full_ok = false;
    }
    if (lg->mutex_ok) {
        pthread_mutex_destroy(&lg->lock);
        lg->mutex_ok = false;
    }
    free(lg->buf);
    lg->buf = NULL;
}

int logger_init(Logger *lg, const char *path, size_t capacity,
                int consumer_delay_ms, bool trace)
{
    char err[200];
    int rc;

    memset(lg, 0, sizeof *lg);
    if (capacity < 1 || capacity > LOGGER_MAX_CAPACITY) {
        cc_errf("logger queue capacity %zu out of range (1..%d)", capacity,
                LOGGER_MAX_CAPACITY);
        return -1;
    }
    if (path == NULL || *path == '\0' || strlen(path) >= sizeof lg->path) {
        cc_errf("invalid log file path");
        return -1;
    }
    snprintf(lg->path, sizeof lg->path, "%s", path);
    lg->capacity = capacity;
    lg->consumer_delay_ms = consumer_delay_ms;
    lg->trace = trace;

    lg->buf = calloc(capacity, sizeof *lg->buf);
    if (lg->buf == NULL) {
        cc_errf("out of memory allocating log queue");
        return -1;
    }
    if ((rc = pthread_mutex_init(&lg->lock, NULL)) != 0) {
        cc_errf("pthread_mutex_init: %s", strerror(rc));
        goto fail;
    }
    lg->mutex_ok = true;
    if ((rc = pthread_cond_init(&lg->not_full, NULL)) != 0) {
        cc_errf("pthread_cond_init: %s", strerror(rc));
        goto fail;
    }
    lg->not_full_ok = true;
    if ((rc = pthread_cond_init(&lg->not_empty, NULL)) != 0) {
        cc_errf("pthread_cond_init: %s", strerror(rc));
        goto fail;
    }
    lg->not_empty_ok = true;

    if (cc_ensure_parent_dir(path, err, sizeof err) != 0) {
        cc_errf("%s", err);
        goto fail;
    }
    lg->fp = fopen(path, "a");              /* append: never destroys data */
    if (lg->fp == NULL) {
        cc_errf("cannot open log file '%s': %s", path, strerror(errno));
        goto fail;
    }
    fseek(lg->fp, 0, SEEK_END);
    lg->start_offset = ftell(lg->fp);
    if (lg->start_offset < 0)
        lg->start_offset = 0;
    fprintf(lg->fp, "# --- CampusCompute log session start (pid %d, queue "
            "capacity %zu) ---\n", (int)getpid(), capacity);
    fflush(lg->fp);
    return 0;

fail:
    logger_destroy(lg);
    return -1;
}

/*
 * Take one entry out of the queue. Blocks while the queue is empty and the
 * logger is still open. Returns 1 with *out filled, or 0 once the logger is
 * closed AND drained (the consumer's signal to exit).
 */
static int dequeue_blocking(Logger *lg, LogEntry *out)
{
    bool waited = false;
    int have;

    pthread_mutex_lock(&lg->lock);
    while (lg->count == 0 && !lg->closing) {
        if (!waited) {
            lg->stats.empty_waits++;
            waited = true;
            if (lg->trace)
                printf("  [consumer  ] BLOCKED  queue EMPTY (0/%zu) -> "
                       "waiting on not_empty\n", lg->capacity);
        }
        pthread_cond_wait(&lg->not_empty, &lg->lock);
    }
    if (lg->count == 0) {                   /* closing and nothing left */
        have = 0;
    } else {
        *out = lg->buf[lg->head];
        lg->head = (lg->head + 1) % lg->capacity;
        lg->count--;
        lg->stats.dequeued++;
        if (lg->trace)
            printf("  [consumer  ] DEQUEUE  depth=%zu/%zu  <- [%s] %s\n",
                   lg->count, lg->capacity, out->source, out->message);
        pthread_cond_signal(&lg->not_full); /* a slot just became free */
        have = 1;
    }
    pthread_mutex_unlock(&lg->lock);
    return have;
}

static void write_entry(Logger *lg, const LogEntry *e)
{
    char ts[40];
    int ok;

    cc_format_timestamp(&e->ts, ts, sizeof ts);
    ok = fprintf(lg->fp, "%s [%-5s] [%s] %s\n", ts, level_name(e->level),
                 e->source, e->message) >= 0 && fflush(lg->fp) == 0;
    pthread_mutex_lock(&lg->lock);
    if (ok)
        lg->stats.written++;
    else
        lg->stats.write_errors++;
    pthread_mutex_unlock(&lg->lock);
    if (!ok)
        clearerr(lg->fp);
}

static void *consumer_main(void *arg)
{
    Logger *lg = arg;
    LogEntry e;

    while (dequeue_blocking(lg, &e)) {
        write_entry(lg, &e);
        cc_sleep_ms(lg->consumer_delay_ms);     /* simulated slow I/O */
    }
    pthread_mutex_lock(&lg->lock);
    if (lg->trace)
        printf("  [consumer  ] queue closed and drained -> thread exiting\n");
    pthread_mutex_unlock(&lg->lock);
    return NULL;
}

int logger_start(Logger *lg)
{
    int rc = pthread_create(&lg->consumer, NULL, consumer_main, lg);

    if (rc != 0) {
        cc_errf("pthread_create (logger consumer): %s", strerror(rc));
        return -1;
    }
    lg->consumer_started = true;
    return 0;
}

int logger_log(Logger *lg, LogLevel level, const char *source,
               const char *fmt, ...)
{
    LogEntry e;
    va_list ap;
    bool waited = false;

    /* Format outside the lock: keeps the critical section tiny. */
    memset(&e, 0, sizeof e);
    clock_gettime(CLOCK_REALTIME, &e.ts);
    e.level = level;
    snprintf(e.source, sizeof e.source, "%s", source ? source : "?");
    va_start(ap, fmt);
    vsnprintf(e.message, sizeof e.message, fmt, ap);
    va_end(ap);
    for (char *p = e.message; *p; p++)          /* one entry == one line */
        if (*p == '\n' || *p == '\r')
            *p = ' ';

    pthread_mutex_lock(&lg->lock);
    while (lg->count == lg->capacity && !lg->closing) {
        if (!waited) {
            lg->stats.full_waits++;
            waited = true;
            if (lg->trace)
                printf("  [%-10s] BLOCKED  queue FULL (%zu/%zu) -> waiting "
                       "on not_full\n", e.source, lg->count, lg->capacity);
        }
        pthread_cond_wait(&lg->not_full, &lg->lock);
    }
    if (lg->closing) {
        lg->stats.rejected++;
        pthread_mutex_unlock(&lg->lock);
        return -1;
    }
    lg->buf[lg->tail] = e;
    lg->tail = (lg->tail + 1) % lg->capacity;
    lg->count++;
    lg->stats.enqueued++;
    if (lg->count > lg->stats.max_depth)
        lg->stats.max_depth = lg->count;
    if (lg->trace)
        printf("  [%-10s] ENQUEUE  depth=%zu/%zu  -> %s\n", e.source,
               lg->count, lg->capacity, e.message);
    pthread_cond_signal(&lg->not_empty);        /* wake the consumer */
    pthread_mutex_unlock(&lg->lock);
    return 0;
}

int logger_stop(Logger *lg)
{
    int rc = 0;

    if (lg->stopped)
        return 0;
    lg->stopped = true;

    pthread_mutex_lock(&lg->lock);
    lg->closing = true;
    pthread_cond_broadcast(&lg->not_empty);     /* release the consumer   */
    pthread_cond_broadcast(&lg->not_full);      /* release any producers  */
    pthread_mutex_unlock(&lg->lock);

    if (lg->consumer_started) {
        int jr = pthread_join(lg->consumer, NULL);

        if (jr != 0) {
            cc_errf("pthread_join (logger consumer): %s", strerror(jr));
            rc = -1;
        }
        lg->consumer_started = false;
    } else {
        /* No consumer thread was running: drain synchronously. */
        LogEntry e;

        while (dequeue_blocking(lg, &e))
            write_entry(lg, &e);
    }

    if (lg->fp != NULL) {
        fprintf(lg->fp, "# --- session end: enqueued=%lu written=%lu "
                "full_waits=%lu empty_waits=%lu ---\n",
                lg->stats.enqueued, lg->stats.written, lg->stats.full_waits,
                lg->stats.empty_waits);
        if (fflush(lg->fp) != 0)
            rc = -1;
    }
    return rc;
}

LoggerStats logger_get_stats(Logger *lg)
{
    LoggerStats s;

    pthread_mutex_lock(&lg->lock);
    s = lg->stats;
    pthread_mutex_unlock(&lg->lock);
    return s;
}
