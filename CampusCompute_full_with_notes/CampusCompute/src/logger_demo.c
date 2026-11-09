/*
 * logger_demo.c - FR3: a self-checking demonstration of the producer-consumer
 * logger. Several producer threads log events through a deliberately small
 * queue while a slow consumer writes them to a file, which forces both the
 * queue-full and the queue-empty conditions to occur for real. The run then
 * VERIFIES the outcome (counts, per-producer ordering, lines in the file).
 */
#include "logger.h"
#include "util.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEMO_MAX_PRODUCERS 32

typedef struct {
    Logger *lg;
    int id;                 /* 1-based producer number */
    int messages;
    int delay_ms;
    bool verbose;
    int sent;               /* result: messages accepted by the logger */
} ProducerArg;

void logger_demo_default_opts(LoggerDemoOpts *o)
{
    o->capacity = 3;                /* small on purpose: forces "full"     */
    o->producers = 3;
    o->messages = 5;
    o->producer_delay_ms = 2;
    o->consumer_delay_ms = 25;      /* slow consumer: forces "full"        */
    o->path = LOGGER_DEFAULT_PATH;
    o->trace = true;
}

static void *producer_main(void *p)
{
    ProducerArg *a = p;
    char src[LOG_SRC_MAX];

    snprintf(src, sizeof src, "producer-%d", a->id);
    if (a->verbose)
        printf("  [%-10s] thread started\n", src);
    for (int k = 1; k <= a->messages; k++) {
        if (logger_log(a->lg, LOG_INFO, src, "submitted job J%d (msg %d/%d)",
                       a->id * 100 + k, k, a->messages) != 0)
            break;
        a->sent++;
        cc_sleep_ms(a->delay_ms);
    }
    if (a->verbose)
        printf("  [%-10s] finished, %d/%d messages accepted\n", src, a->sent,
               a->messages);
    return NULL;
}

/*
 * Re-read this session's part of the log file and check it independently of
 * the logger's own counters: number of entry lines and, for every producer,
 * that its messages appear in order (per-producer FIFO).
 */
static int verify_log_file(const char *path, long offset, int producers,
                           int per_producer, int *lines, int *order_errors)
{
    FILE *fp = fopen(path, "r");
    char line[512];
    int next[DEMO_MAX_PRODUCERS + 1];
    int seen[DEMO_MAX_PRODUCERS + 1];

    *lines = 0;
    *order_errors = 0;
    if (fp == NULL)
        return -1;
    if (fseek(fp, offset, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    for (int i = 0; i <= DEMO_MAX_PRODUCERS; i++) {
        next[i] = 1;
        seen[i] = 0;
    }
    while (fgets(line, sizeof line, fp) != NULL) {
        const char *src, *msg;
        int pid, k;

        if (line[0] == '#')
            continue;
        (*lines)++;
        src = strstr(line, "[producer-");
        msg = strstr(line, "(msg ");
        if (src == NULL || msg == NULL) {
            (*order_errors)++;
            continue;
        }
        pid = atoi(src + strlen("[producer-"));
        k = atoi(msg + strlen("(msg "));
        if (pid < 1 || pid > producers || k != next[pid])
            (*order_errors)++;
        else
            next[pid]++;
        if (pid >= 1 && pid <= producers)
            seen[pid]++;
    }
    fclose(fp);
    for (int p = 1; p <= producers; p++)
        if (seen[p] != per_producer)
            (*order_errors)++;
    return 0;
}

static int check(const char *what, int ok)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    return ok ? 0 : 1;
}

int logger_run_demo(const LoggerDemoOpts *o)
{
    Logger lg;
    pthread_t th[DEMO_MAX_PRODUCERS];
    ProducerArg args[DEMO_MAX_PRODUCERS];
    int created = 0, failures = 0, lines = 0, order_errors = 0;
    unsigned long expected;
    LoggerStats st;
    bool ok_threads = true;

    if (o->producers < 1 || o->producers > DEMO_MAX_PRODUCERS ||
        o->messages < 1 || o->messages > 1000) {
        cc_errf("producers must be 1..%d and messages 1..1000",
                DEMO_MAX_PRODUCERS);
        return -1;
    }
    expected = (unsigned long)o->producers * (unsigned long)o->messages;

    cc_banner("FR3 - Concurrent Activity Logger (bounded producer-consumer)");
    printf("Queue capacity: %zu   Producers: %d x %d messages = %lu events\n"
           "Producer delay: %d ms   Consumer delay (simulated slow I/O): "
           "%d ms\nLog file: %s (append mode)\n\n",
           o->capacity, o->producers, o->messages, expected,
           o->producer_delay_ms, o->consumer_delay_ms, o->path);

    if (logger_init(&lg, o->path, o->capacity, o->consumer_delay_ms,
                    o->trace) != 0)
        return -1;
    if (logger_start(&lg) != 0) {
        logger_destroy(&lg);
        return -1;
    }
    if (o->trace)
        puts("  consumer thread running; giving it a moment so it blocks on "
             "an EMPTY queue...");
    cc_sleep_ms(60);

    for (int i = 0; i < o->producers; i++) {
        args[i] = (ProducerArg){ .lg = &lg, .id = i + 1,
                                 .messages = o->messages,
                                 .delay_ms = o->producer_delay_ms,
                                 .verbose = o->trace, .sent = 0 };
        int rc = pthread_create(&th[i], NULL, producer_main, &args[i]);

        if (rc != 0) {
            cc_errf("pthread_create (producer %d): %s", i + 1, strerror(rc));
            ok_threads = false;
            break;
        }
        created++;
    }
    for (int i = 0; i < created; i++)
        pthread_join(th[i], NULL);              /* orderly producer exit */
    if (o->trace)
        puts("  all producer threads joined; shutting the logger down "
             "(consumer drains remaining entries)...");

    if (logger_stop(&lg) != 0)
        failures++;

    /* After shutdown the logger must refuse new work, not hang. */
    int late = logger_log(&lg, LOG_INFO, "late-producer", "after shutdown");
    st = logger_get_stats(&lg);

    puts("\nLogger statistics:");
    printf("  enqueued=%lu dequeued=%lu written=%lu write_errors=%lu\n",
           st.enqueued, st.dequeued, st.written, st.write_errors);
    printf("  queue-FULL events (a producer had to wait)  : %lu\n",
           st.full_waits);
    printf("  queue-EMPTY events (consumer had to wait)   : %lu\n",
           st.empty_waits);
    printf("  maximum queue depth reached                 : %lu / %zu\n",
           st.max_depth, o->capacity);

    if (verify_log_file(lg.path, lg.start_offset, o->producers, o->messages,
                        &lines, &order_errors) != 0) {
        cc_errf("cannot re-read log file '%s' for verification", lg.path);
        failures++;
    }

    puts("\nVerification:");
    failures += check("all producer threads started and joined", ok_threads);
    failures += check("every event was accepted (enqueued == producers x "
                      "messages)", st.enqueued == expected);
    failures += check("consumer dequeued and wrote every event",
                      st.dequeued == st.enqueued && st.written == st.enqueued
                      && st.write_errors == 0);
    failures += check("queue never exceeded its capacity",
                      st.max_depth <= o->capacity);
    failures += check("log file holds exactly the expected number of lines",
                      (unsigned long)lines == expected);
    failures += check("each producer's messages are in order in the file",
                      order_errors == 0);
    failures += check("logging after shutdown is rejected (no hang)",
                      late == -1 && st.rejected == 1);
    printf("  [INFO] queue-full condition %s; queue-empty condition %s\n",
           st.full_waits ? "OBSERVED" : "not triggered with these settings",
           st.empty_waits ? "OBSERVED" : "not triggered with these settings");

    logger_destroy(&lg);                        /* mutex, condvars, file */
    printf("\nLog file written: %s\n", o->path);
    printf("FR3 RESULT: %s\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : -1;
}
