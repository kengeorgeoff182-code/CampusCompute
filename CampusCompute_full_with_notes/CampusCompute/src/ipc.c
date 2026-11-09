/*
 * ipc.c - FR4: two processes exchanging data through POSIX shared memory,
 * coordinated by named POSIX semaphores. See ipc.h for the protocol.
 *
 * Resource ownership
 *   The PARENT creates (O_CREAT|O_EXCL) and finally unlinks the shared-memory
 *   object and both semaphores. The CHILD only opens and closes them. The
 *   parent's cleanup runs through a single exit label so it executes on the
 *   success path and on every error path (including SIGINT/SIGTERM, which are
 *   turned into a flag that interrupts the blocking waits).
 */
#include "ipc.h"
#include "util.h"
#include "worker.h"

#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define IPC_MAGIC        0x43434D50u        /* "CCMP" */
#define IPC_TEXT_MAX     48
#define IPC_NAME_MAX     64
#define CONSUMER_WORK_MS 20                 /* simulated per-message work */
#define FAULT_CRASH_CODE 4

/* ---- layout of the shared block ------------------------------------ */

typedef struct {
    uint32_t seq;                   /* 1-based message number */
    uint32_t job_id;
    uint32_t arrival;
    uint32_t burst;
    uint32_t priority;
    uint32_t checksum;              /* FNV-1a over the fields above + text */
    char text[IPC_TEXT_MAX];
} IpcMessage;

typedef struct {
    uint32_t magic;
    uint32_t total_messages;        /* announced by the dispatcher          */
    uint32_t head;                  /* next slot to read  (consumer-owned)  */
    uint32_t tail;                  /* next slot to write (dispatcher-owned)*/
    IpcMessage slots[IPC_SLOTS];
    /* reply area: written by the consumer, read by the parent after waitpid */
    uint32_t reply_valid;
    uint32_t reply_received;
    uint32_t reply_bad_checksums;
    uint32_t reply_consumer_blocked;
    uint64_t reply_sum_burst;
} IpcShared;

typedef struct {
    char shm_name[IPC_NAME_MAX];
    char empty_name[IPC_NAME_MAX];
    char full_name[IPC_NAME_MAX];
    bool shm_created, empty_created, full_created;
    IpcShared *shm;                 /* NULL if not mapped  */
    sem_t *sem_empty;               /* NULL if not open    */
    sem_t *sem_full;
} IpcCtx;

static volatile sig_atomic_t g_stop;

static void on_stop_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

void ipc_default_options(IpcOptions *o)
{
    o->fault = IPC_FAULT_NONE;
    o->timeout_s = IPC_DEFAULT_TIMEOUT_S;
}

static void make_names(IpcCtx *c)
{
    unsigned uid = (unsigned)getuid();

    snprintf(c->shm_name, sizeof c->shm_name, "/campuscompute_shm_%u", uid);
    snprintf(c->empty_name, sizeof c->empty_name,
             "/campuscompute_sem_empty_%u", uid);
    snprintf(c->full_name, sizeof c->full_name,
             "/campuscompute_sem_full_%u", uid);
}

static uint32_t checksum_of(const IpcMessage *m)
{
    uint32_t h = 2166136261u;
    const uint32_t f[5] = { m->seq, m->job_id, m->arrival, m->burst,
                            m->priority };

    for (int i = 0; i < 5; i++)
        for (int b = 0; b < 4; b++) {
            h ^= (f[i] >> (8 * b)) & 0xffu;
            h *= 16777619u;
        }
    for (const char *p = m->text; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    return h;
}

/* Wait on a semaphore with a timeout; -1 with errno ETIMEDOUT / EINTR. */
static int sem_wait_timeout(sem_t *s, int timeout_s)
{
    struct timespec dl;

    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += timeout_s;
    for (;;) {
        if (g_stop) {
            errno = EINTR;
            return -1;
        }
        if (sem_timedwait(s, &dl) == 0)
            return 0;
        if (errno != EINTR)
            return -1;
    }
}

static int semval(sem_t *s)
{
    int v = -1;

    sem_getvalue(s, &v);
    return v;
}

/* ------------------------------------------------------------------ */
/* named-object housekeeping                                           */
/* ------------------------------------------------------------------ */

bool ipc_resources_present(void)
{
    IpcCtx c;
    bool present = false;
    int fd;
    sem_t *s;

    make_names(&c);
    fd = shm_open(c.shm_name, O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        present = true;
    }
    s = sem_open(c.empty_name, 0);
    if (s != SEM_FAILED) {
        sem_close(s);
        present = true;
    }
    s = sem_open(c.full_name, 0);
    if (s != SEM_FAILED) {
        sem_close(s);
        present = true;
    }
    return present;
}

int ipc_cleanup_named(bool verbose)
{
    IpcCtx c;
    int removed = 0;

    make_names(&c);
    if (sem_unlink(c.empty_name) == 0) {
        removed++;
        if (verbose) printf("  removed semaphore %s\n", c.empty_name);
    } else if (errno != ENOENT) {
        cc_errf("sem_unlink(%s): %s", c.empty_name, strerror(errno));
    }
    if (sem_unlink(c.full_name) == 0) {
        removed++;
        if (verbose) printf("  removed semaphore %s\n", c.full_name);
    } else if (errno != ENOENT) {
        cc_errf("sem_unlink(%s): %s", c.full_name, strerror(errno));
    }
    if (shm_unlink(c.shm_name) == 0) {
        removed++;
        if (verbose) printf("  removed shared memory %s\n", c.shm_name);
    } else if (errno != ENOENT) {
        cc_errf("shm_unlink(%s): %s", c.shm_name, strerror(errno));
    }
    if (verbose && removed == 0)
        puts("  nothing to remove (no CampusCompute IPC objects exist)");
    return removed;
}

/* ------------------------------------------------------------------ */
/* consumer process (child)                                            */
/* ------------------------------------------------------------------ */

/*
 * Runs in the forked child. It first drops the handles inherited from the
 * parent and then opens everything again BY NAME, exactly as an unrelated
 * process would. Returns the child's exit status.
 */
static int consumer_process(IpcCtx *inherited, const IpcOptions *opt,
                            uint32_t expected_total)
{
    IpcShared *shm = NULL;
    sem_t *sem_empty = SEM_FAILED, *sem_full = SEM_FAILED;
    struct stat st;
    uint64_t sum = 0;
    uint32_t received = 0, bad = 0, blocked = 0;
    int fd, code = 0;

    /* forget the inherited mapping/handles; reopen by name */
    munmap(inherited->shm, sizeof(IpcShared));
    sem_close(inherited->sem_empty);
    sem_close(inherited->sem_full);

    fd = shm_open(inherited->shm_name, O_RDWR, 0);
    if (fd < 0) {
        cc_errf("[consumer] shm_open(%s): %s", inherited->shm_name,
                strerror(errno));
        return 2;
    }
    if (fstat(fd, &st) != 0 || (size_t)st.st_size < sizeof(IpcShared)) {
        cc_errf("[consumer] shared-memory object has unexpected size");
        close(fd);
        return 2;
    }
    shm = mmap(NULL, sizeof *shm, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);                                  /* mapping stays valid */
    if (shm == MAP_FAILED) {
        cc_errf("[consumer] mmap: %s", strerror(errno));
        return 2;
    }
    sem_empty = sem_open(inherited->empty_name, 0);
    sem_full = sem_open(inherited->full_name, 0);
    if (sem_empty == SEM_FAILED || sem_full == SEM_FAILED) {
        cc_errf("[consumer] sem_open: %s", strerror(errno));
        code = 2;
        goto out;
    }
    if (shm->magic != IPC_MAGIC || shm->total_messages != expected_total) {
        cc_errf("[consumer] shared block failed validation");
        code = 2;
        goto out;
    }
    printf("  [consumer   pid=%d] opened %s by name; mapped at %p\n",
           (int)getpid(), inherited->shm_name, (void *)shm);

    for (uint32_t k = 0; k < expected_total; k++) {
        const IpcMessage *m;
        uint32_t slot;

        if (sem_trywait(sem_full) != 0) {
            if (errno != EAGAIN && errno != EINTR) {
                cc_errf("[consumer] sem_trywait: %s", strerror(errno));
                code = 2;
                goto out;
            }
            blocked++;
            printf("  [consumer   pid=%d] BLOCKED: no data (sem_full=%d) - "
                   "waiting\n", (int)getpid(), semval(sem_full));
            if (sem_wait_timeout(sem_full, opt->timeout_s) != 0) {
                cc_errf("[consumer] gave up waiting for data: %s",
                        errno == ETIMEDOUT ? "timed out" : strerror(errno));
                code = 3;
                goto out;
            }
        }
        slot = shm->head;
        m = &shm->slots[slot];
        if (checksum_of(m) != m->checksum)
            bad++;
        printf("  [consumer   pid=%d] <- seq %u: J%u (arrival=%u burst=%u "
               "prio=%u) from slot %u, checksum %s\n", (int)getpid(), m->seq,
               m->job_id, m->arrival, m->burst, m->priority, slot,
               checksum_of(m) == m->checksum ? "OK" : "BAD");
        sum += m->burst;
        received++;
        shm->head = (slot + 1) % IPC_SLOTS;
        sem_post(sem_empty);                    /* slot is free again */

        if (opt->fault == IPC_FAULT_CONSUMER_CRASH && received == 1) {
            printf("  [consumer   pid=%d] *** SIMULATED FAILURE: consumer "
                   "exits abruptly with status %d (injected by CampusCompute,"
                   " not a real fault) ***\n", (int)getpid(),
                   FAULT_CRASH_CODE);
            fflush(stdout);
            _exit(FAULT_CRASH_CODE);            /* skips normal cleanup */
        }
        cc_sleep_ms(CONSUMER_WORK_MS);          /* simulated processing */
    }

    /* reply to the dispatcher through the same shared block */
    shm->reply_received = received;
    shm->reply_bad_checksums = bad;
    shm->reply_consumer_blocked = blocked;
    shm->reply_sum_burst = sum;
    shm->reply_valid = 1;
    printf("  [consumer   pid=%d] all %u messages read; summary written back "
           "to shared memory\n", (int)getpid(), received);

out:
    if (sem_full != SEM_FAILED)
        sem_close(sem_full);
    if (sem_empty != SEM_FAILED)
        sem_close(sem_empty);
    munmap(shm, sizeof *shm);
    return code;
}

/* ------------------------------------------------------------------ */
/* dispatcher (parent)                                                 */
/* ------------------------------------------------------------------ */

static int dispatch_messages(IpcCtx *c, const JobSet *jobs, uint32_t total,
                             const IpcOptions *opt, uint32_t *blocked_out)
{
    uint32_t blocked = 0;

    for (uint32_t k = 0; k < total; k++) {
        const Job *j = &jobs->jobs[k % (uint32_t)jobs->count];
        IpcMessage *m;
        uint32_t slot;

        if (sem_trywait(c->sem_empty) != 0) {
            if (errno != EAGAIN && errno != EINTR) {
                cc_errf("sem_trywait: %s", strerror(errno));
                return -1;
            }
            blocked++;
            printf("  [dispatcher pid=%d] BLOCKED: ring full "
                   "(sem_empty=%d) - waiting for the consumer\n",
                   (int)getpid(), semval(c->sem_empty));
            if (sem_wait_timeout(c->sem_empty, opt->timeout_s) != 0) {
                if (errno == ETIMEDOUT)
                    cc_errf("dispatcher timed out after %d s waiting for a "
                            "free slot - the consumer is not responding",
                            opt->timeout_s);
                else
                    cc_errf("dispatcher interrupted");
                *blocked_out = blocked;
                return -1;
            }
        }
        slot = c->shm->tail;
        m = &c->shm->slots[slot];
        memset(m, 0, sizeof *m);
        m->seq = k + 1;
        m->job_id = (uint32_t)j->id;
        m->arrival = (uint32_t)j->arrival;
        m->burst = (uint32_t)j->burst;
        m->priority = (uint32_t)j->priority;
        snprintf(m->text, sizeof m->text, "dispatch J%d to compute node",
                 j->id);
        m->checksum = checksum_of(m);
        c->shm->tail = (slot + 1) % IPC_SLOTS;
        sem_post(c->sem_full);                  /* publish the slot */
        printf("  [dispatcher pid=%d] -> seq %u: J%u (arrival=%u burst=%u "
               "prio=%u) into slot %u   [sem_empty=%d sem_full=%d]\n",
               (int)getpid(), m->seq, m->job_id, m->arrival, m->burst,
               m->priority, slot, semval(c->sem_empty), semval(c->sem_full));
    }
    *blocked_out = blocked;
    return 0;
}

int ipc_run_demo(const JobSet *jobs, const IpcOptions *opt)
{
    IpcCtx c;
    struct sigaction sa, old_int, old_term;
    pid_t child = -1;
    uint32_t total, producer_blocked = 0;
    uint64_t expected_sum = 0;
    bool reaped = false, send_ok = false;
    int rc = -1, fd = -1, st = 0;

    if (jobs == NULL || jobs->count <= 0) {
        cc_errf("no jobs to send (add jobs first)");
        return -1;
    }
    memset(&c, 0, sizeof c);
    make_names(&c);
    total = (uint32_t)jobs->count;
    if (opt->fault == IPC_FAULT_CONSUMER_CRASH)
        total += IPC_SLOTS + 2;     /* enough traffic to overflow the ring */
    for (uint32_t k = 0; k < total; k++)
        expected_sum += (uint64_t)jobs->jobs[k % (uint32_t)jobs->count].burst;

    cc_banner("FR4 - Inter-Process Communication (POSIX shared memory + "
              "named semaphores)");
    printf("Shared memory : %s  (%zu bytes, %d-slot ring buffer)\n"
           "Semaphores    : %s (free slots, initial %d)\n"
           "                %s (filled slots, initial 0)\n"
           "Messages      : %u job records, per-wait timeout %d s%s\n\n",
           c.shm_name, sizeof(IpcShared), IPC_SLOTS, c.empty_name, IPC_SLOTS,
           c.full_name, total, opt->timeout_s,
           opt->fault == IPC_FAULT_CONSUMER_CRASH
               ? "\nFault injection: SIMULATED consumer crash after 1 message"
               : "");

    /* Turn SIGINT/SIGTERM into a flag so the cleanup below always runs. */
    g_stop = 0;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);                   /* no SA_RESTART: EINTR */
    sigaction(SIGINT, &sa, &old_int);
    sigaction(SIGTERM, &sa, &old_term);

    /* 1. create the shared-memory object (fails if one already exists) */
    fd = shm_open(c.shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        if (errno == EEXIST)
            cc_errf("%s already exists (left over from an interrupted run?). "
                    "Run './campuscompute cleanup' to remove it.",
                    c.shm_name);
        else
            cc_errf("shm_open(%s): %s", c.shm_name, strerror(errno));
        goto out;
    }
    c.shm_created = true;
    if (ftruncate(fd, (off_t)sizeof(IpcShared)) != 0) {
        cc_errf("ftruncate: %s", strerror(errno));
        goto out;
    }
    c.shm = mmap(NULL, sizeof *c.shm, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                 0);
    close(fd);
    fd = -1;
    if (c.shm == MAP_FAILED) {
        c.shm = NULL;
        cc_errf("mmap: %s", strerror(errno));
        goto out;
    }
    memset(c.shm, 0, sizeof *c.shm);
    c.shm->magic = IPC_MAGIC;
    c.shm->total_messages = total;

    /* 2. create the named semaphores */
    c.sem_empty = sem_open(c.empty_name, O_CREAT | O_EXCL, 0600, IPC_SLOTS);
    if (c.sem_empty == SEM_FAILED) {
        c.sem_empty = NULL;
        cc_errf("sem_open(%s): %s", c.empty_name,
                errno == EEXIST ? "already exists - run './campuscompute "
                                  "cleanup'" : strerror(errno));
        goto out;
    }
    c.empty_created = true;
    c.sem_full = sem_open(c.full_name, O_CREAT | O_EXCL, 0600, 0);
    if (c.sem_full == SEM_FAILED) {
        c.sem_full = NULL;
        cc_errf("sem_open(%s): %s", c.full_name,
                errno == EEXIST ? "already exists - run './campuscompute "
                                  "cleanup'" : strerror(errno));
        goto out;
    }
    c.full_created = true;
    printf("Created and initialised shared memory + 2 semaphores. Parent "
           "mapping at %p.\n", (void *)c.shm);

    /* 3. second process */
    fflush(stdout);                             /* no duplicated buffers */
    child = fork();
    if (child < 0) {
        cc_errf("fork: %s", strerror(errno));
        goto out;
    }
    if (child == 0) {
        int code = consumer_process(&c, opt, total);

        fflush(stdout);
        _exit(code);        /* never run the parent's atexit/cleanup code */
    }

    /* 4. exchange the data */
    send_ok = dispatch_messages(&c, jobs, total, opt, &producer_blocked) == 0;
    if (!send_ok && child > 0)
        kill(child, SIGTERM);       /* do not wait out the child's timeout */

    /* 5. reap the consumer and report exactly what waitpid returned */
    while (waitpid(child, &st, 0) < 0) {
        if (errno == EINTR) {
            if (g_stop)
                kill(child, SIGTERM);
            continue;
        }
        cc_errf("waitpid: %s", strerror(errno));
        break;
    }
    reaped = true;
    {
        char desc[128];

        worker_describe_status(st, desc, sizeof desc);
        printf("\nConsumer process %d: %s\n", (int)child, desc);
    }
    if (!send_ok)
        goto out;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        cc_errf("consumer did not finish successfully");
        goto out;
    }

    /* 6. read the consumer's reply from shared memory and verify it */
    printf("Reply read back from shared memory: received=%u, sum of bursts="
           "%llu, bad checksums=%u\n", c.shm->reply_received,
           (unsigned long long)c.shm->reply_sum_burst,
           c.shm->reply_bad_checksums);
    printf("Expected by dispatcher:             sent=%u,     sum of bursts="
           "%llu\n", total, (unsigned long long)expected_sum);
    printf("Blocking events: dispatcher waited %u time(s) on a full ring, "
           "consumer waited %u time(s) on an empty ring.\n",
           producer_blocked, c.shm->reply_consumer_blocked);
    if (c.shm->reply_valid && c.shm->reply_received == total &&
        c.shm->reply_sum_burst == expected_sum &&
        c.shm->reply_bad_checksums == 0) {
        puts("DATA EXCHANGE VERIFIED: every record arrived intact through "
             "shared memory.");
        rc = 0;
    } else {
        cc_errf("data exchange verification FAILED");
    }

out:
    /* ---- cleanup: runs on success and on every error path ---- */
    if (child > 0 && !reaped) {                 /* never leave a zombie */
        kill(child, SIGTERM);
        while (waitpid(child, &st, 0) < 0 && errno == EINTR)
            ;
    }
    if (fd >= 0)
        close(fd);
    puts("\nCleanup of IPC resources created by this run:");
    if (c.shm != NULL) {
        if (munmap(c.shm, sizeof *c.shm) == 0)
            puts("  munmap: shared-memory mapping released");
        c.shm = NULL;
    }
    if (c.sem_empty != NULL) {
        sem_close(c.sem_empty);
        c.sem_empty = NULL;
    }
    if (c.sem_full != NULL) {
        sem_close(c.sem_full);
        c.sem_full = NULL;
    }
    if (c.empty_created && sem_unlink(c.empty_name) == 0)
        printf("  sem_unlink: %s removed\n", c.empty_name);
    if (c.full_created && sem_unlink(c.full_name) == 0)
        printf("  sem_unlink: %s removed\n", c.full_name);
    if (c.shm_created && shm_unlink(c.shm_name) == 0)
        printf("  shm_unlink: %s removed\n", c.shm_name);

    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    if (g_stop)
        puts("  (interrupted by signal - resources released anyway)");

    /* Verification: re-open each name; ENOENT proves it is gone. Only
     * meaningful for objects this run created. */
    if (c.shm_created || c.empty_created || c.full_created) {
        if (ipc_resources_present()) {
            cc_errf("IPC objects still present after cleanup");
            rc = -1;
        } else {
            puts("  verified: shm_open/sem_open on all three names now "
                 "fail with ENOENT (objects are gone)");
        }
    }
    return rc;
}
