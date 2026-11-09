/*
 * resource_limits.c - FR7: demonstrating setrlimit().
 *
 * Every demonstration runs in a forked CHILD. The limit is applied only to
 * that child, the effect is provoked in a controlled way, and the observation
 * is checked against what the limit should produce. The parent prints its own
 * limit before and after, which proves it was never affected (so there is
 * nothing to restore).
 *
 *   FSIZE  soft limit 1024 bytes, SIGXFSZ ignored -> write() must fail with
 *          EFBIG after exactly 1024 bytes were stored in a temp file.
 *   NOFILE soft limit 16 -> open() must fail with EMFILE after exactly
 *          (16 - descriptors already open) successful opens.
 *   CPU    soft limit 1 s -> a busy loop must be killed by SIGXCPU.
 *
 * Only files this module creates are touched: one mkstemp() file under /tmp
 * for FSIZE, which is deleted by the child and whose removal the parent then
 * double-checks.
 */
#include "resource_limits.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"
#include "worker.h"

#define FSIZE_LIMIT_BYTES  1024
#define NOFILE_LIMIT       16
#define CPU_LIMIT_SECONDS  1
#define CPU_SAFETY_ALARM_S 10       /* the child can never spin forever */

static const char *limit_name(LimitKind k)
{
    switch (k) {
    case LIMIT_FSIZE:  return "RLIMIT_FSIZE (maximum file size)";
    case LIMIT_NOFILE: return "RLIMIT_NOFILE (maximum open files)";
    case LIMIT_CPU:    return "RLIMIT_CPU (maximum CPU seconds)";
    }
    return "?";
}

static int limit_resource(LimitKind k)
{
    switch (k) {
    case LIMIT_FSIZE:  return RLIMIT_FSIZE;
    case LIMIT_NOFILE: return RLIMIT_NOFILE;
    case LIMIT_CPU:    return RLIMIT_CPU;
    }
    return RLIMIT_CPU;
}

static void rlim_text(rlim_t v, char *buf, size_t len)
{
    if (v == RLIM_INFINITY)
        snprintf(buf, len, "unlimited");
    else
        snprintf(buf, len, "%llu", (unsigned long long)v);
}

/* "<who> <resource>: soft=... hard=..." */
static void print_limit(const char *who, LimitKind k)
{
    struct rlimit rl;
    char soft[32], hard[32];

    if (getrlimit(limit_resource(k), &rl) != 0) {
        printf("[%s] getrlimit failed: %s\n", who, strerror(errno));
        return;
    }
    rlim_text(rl.rlim_cur, soft, sizeof soft);
    rlim_text(rl.rlim_max, hard, sizeof hard);
    printf("[%s] limit now: soft=%s hard=%s\n", who, soft, hard);
}

/*
 * Lower the SOFT limit only (the hard limit is left alone, so the child could
 * even raise it again; it does not need to). Returns 0 / -1.
 */
static int apply_soft_limit(LimitKind k, rlim_t value)
{
    struct rlimit rl;

    if (getrlimit(limit_resource(k), &rl) != 0)
        return -1;
    if (rl.rlim_max != RLIM_INFINITY && value > rl.rlim_max)
        value = rl.rlim_max;
    rl.rlim_cur = value;
    return setrlimit(limit_resource(k), &rl);
}

/* ------------------------------------------------------------------ */
/* child bodies - each returns the child's exit status                  */
/* ------------------------------------------------------------------ */

/* Exit 0: exactly FSIZE_LIMIT_BYTES stored, then write() failed with EFBIG. */
static int child_fsize(void)
{
    char path[] = "/tmp/campuscompute_fsize_XXXXXX";
    char block[512];
    long total = 0;
    int fd, saved_errno = 0, failed = 0;

    signal(SIGXFSZ, SIG_IGN);   /* default would kill us; we want the errno */
    if (apply_soft_limit(LIMIT_FSIZE, FSIZE_LIMIT_BYTES) != 0) {
        printf("[child] setrlimit failed: %s\n", strerror(errno));
        return 1;
    }
    print_limit("child", LIMIT_FSIZE);

    fd = mkstemp(path);
    if (fd < 0) {
        printf("[child] mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    printf("[child] created temporary file %s\n", path);
    memset(block, 'A', sizeof block);

    printf("[child] trying to write 4096 bytes in 512-byte blocks...\n");
    for (int i = 0; i < 8; i++) {
        ssize_t w = write(fd, block, sizeof block);

        if (w < 0) {
            saved_errno = errno;
            failed = 1;
            printf("[child] write #%d FAILED: errno=%d (%s)\n", i + 1,
                   saved_errno, strerror(saved_errno));
            break;
        }
        total += w;
        printf("[child] write #%d stored %zd bytes (file size now %ld)\n",
               i + 1, w, total);
    }
    close(fd);
    if (unlink(path) != 0)
        printf("[child] warning: could not delete %s: %s\n", path,
               strerror(errno));
    else
        printf("[child] temporary file deleted\n");

    printf("[child] observed: %ld bytes stored, write %s\n", total,
           failed ? "refused" : "never refused");
    return (failed && saved_errno == EFBIG && total == FSIZE_LIMIT_BYTES)
               ? 0 : 1;
}

/* Number of descriptors in [0, upto) that are currently open. */
static int count_open_fds(int upto)
{
    int n = 0;

    for (int fd = 0; fd < upto; fd++)
        if (fcntl(fd, F_GETFD) != -1)
            n++;
    return n;
}

/* Exit 0: opens stopped with EMFILE after exactly the expected number. */
static int child_nofile(void)
{
    int fds[NOFILE_LIMIT + 8];
    int opened = 0, expected, already, saved_errno = 0;

    if (apply_soft_limit(LIMIT_NOFILE, NOFILE_LIMIT) != 0) {
        printf("[child] setrlimit failed: %s\n", strerror(errno));
        return 1;
    }
    print_limit("child", LIMIT_NOFILE);

    already = count_open_fds(NOFILE_LIMIT);
    expected = NOFILE_LIMIT - already;
    printf("[child] %d descriptors already open (stdin/stdout/stderr...), "
           "so %d more opens should succeed\n", already, expected);

    for (;;) {
        int fd = open("/dev/null", O_RDONLY);

        if (fd < 0) {
            saved_errno = errno;
            printf("[child] open #%d FAILED: errno=%d (%s)\n", opened + 1,
                   saved_errno, strerror(saved_errno));
            break;
        }
        if (opened >= (int)(sizeof fds / sizeof fds[0])) {
            close(fd);
            printf("[child] limit did not take effect\n");
            break;
        }
        fds[opened++] = fd;
    }
    printf("[child] observed: %d successful opens, then EMFILE=%s\n", opened,
           saved_errno == EMFILE ? "yes" : "no");
    for (int i = 0; i < opened; i++)
        close(fds[i]);
    printf("[child] all %d test descriptors closed\n", opened);
    return (saved_errno == EMFILE && opened == expected) ? 0 : 1;
}

/* Never returns normally: SIGXCPU is expected to end this process. */
static void child_cpu(void)
{
    volatile unsigned long spin = 0;
    struct rlimit core = { 0, 0 };

    setrlimit(RLIMIT_CORE, &core);              /* no core file, please */
    alarm(CPU_SAFETY_ALARM_S);                  /* safety net only */
    if (apply_soft_limit(LIMIT_CPU, CPU_LIMIT_SECONDS) != 0) {
        printf("[child] setrlimit failed: %s\n", strerror(errno));
        fflush(stdout);
        _exit(1);
    }
    print_limit("child", LIMIT_CPU);
    printf("[child] spinning in a busy loop; the kernel should send SIGXCPU "
           "after %d CPU second(s)\n", CPU_LIMIT_SECONDS);
    fflush(stdout);
    for (;;)
        spin++;
}

/* ------------------------------------------------------------------ */
/* parent side                                                         */
/* ------------------------------------------------------------------ */

static int wait_retry(pid_t pid, int *st)
{
    pid_t r;

    while ((r = waitpid(pid, st, 0)) < 0 && errno == EINTR)
        ;
    return r == pid ? 0 : -1;
}

static void leftover_tmp_check(void)
{
    /* The FSIZE child deletes its own mkstemp() file; verify none remain. */
    glob_t g;
    size_t n = 0;

    if (glob("/tmp/campuscompute_fsize_*", 0, NULL, &g) == 0) {
        n = g.gl_pathc;
        globfree(&g);
    }
    printf("[parent] temporary test files left in /tmp: %zu\n", n);
}

int limits_run(LimitKind kind)
{
    pid_t pid;
    int st = 0, ok = 0;
    struct rlimit before, after;
    int res = limit_resource(kind);

    cc_banner("FR7 - Resource Limit (setrlimit)");
    printf("Resource: %s\n", limit_name(kind));
    switch (kind) {
    case LIMIT_FSIZE:
        printf("Applied limit: soft %d bytes. Operation: write 4096 bytes to "
               "a temp file.\nExpected: only %d bytes stored, then EFBIG.\n",
               FSIZE_LIMIT_BYTES, FSIZE_LIMIT_BYTES);
        break;
    case LIMIT_NOFILE:
        printf("Applied limit: soft %d descriptors. Operation: open "
               "/dev/null repeatedly.\nExpected: failure with EMFILE once "
               "the limit is reached.\n", NOFILE_LIMIT);
        break;
    case LIMIT_CPU:
        printf("Applied limit: soft %d CPU second. Operation: busy loop.\n"
               "Expected: the child is killed by SIGXCPU.\n",
               CPU_LIMIT_SECONDS);
        break;
    }

    if (getrlimit(res, &before) != 0) {
        cc_errf("getrlimit: %s", strerror(errno));
        return -1;
    }
    print_limit("parent", kind);
    fflush(stdout);

    pid = fork();
    if (pid < 0) {
        cc_errf("fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int code = 0;

        printf("[child pid=%d] started; limit applies to this process only\n",
               (int)getpid());
        switch (kind) {
        case LIMIT_FSIZE:  code = child_fsize();  break;
        case LIMIT_NOFILE: code = child_nofile(); break;
        case LIMIT_CPU:    child_cpu();           break;  /* no return */
        }
        fflush(stdout);
        _exit(code);
    }

    if (wait_retry(pid, &st) != 0) {
        cc_errf("waitpid: %s", strerror(errno));
        return -1;
    }
    {
        char desc[160];

        worker_describe_status(st, desc, sizeof desc);
        printf("[parent] child %d: %s\n", (int)pid, desc);
    }

    if (kind == LIMIT_CPU) {
        ok = WIFSIGNALED(st) && WTERMSIG(st) == SIGXCPU;
        printf("[parent] observed effect: %s\n",
               ok ? "child was terminated by SIGXCPU (CPU limit enforced)"
                  : "child did NOT end with SIGXCPU");
    } else {
        ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
        printf("[parent] observed effect: %s\n",
               ok ? "limit enforced exactly as expected (child verified it)"
                  : "limit effect did NOT match the expectation");
    }
    if (kind == LIMIT_FSIZE)
        leftover_tmp_check();

    if (getrlimit(res, &after) != 0 || after.rlim_cur != before.rlim_cur ||
        after.rlim_max != before.rlim_max) {
        printf("[parent] parent limit CHANGED unexpectedly\n");
        ok = 0;
    } else {
        print_limit("parent", kind);
        printf("[parent] parent limit unchanged: the limit applied only to "
               "the child, nothing to restore\n");
    }
    printf("[parent] %s: %s\n", limit_name(kind), ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

int limits_run_all(void)
{
    static const LimitKind kinds[3] = { LIMIT_FSIZE, LIMIT_NOFILE, LIMIT_CPU };
    int passed = 0;

    for (int i = 0; i < 3; i++)
        if (limits_run(kinds[i]) == 0)
            passed++;
    printf("\n[parent] resource-limit demonstrations passed: %d/3\n", passed);
    return passed == 3 ? 0 : -1;
}
