/*
 * resource_limits.h - FR7: demonstrating process resource limits with
 * setrlimit().
 *
 * Every demonstration runs in a forked CHILD so the limit only ever applies to
 * that child; the parent prints its own limit before and after to show it was
 * not affected, so there is nothing to restore. Only files created by the
 * demonstration itself (a temporary file that it deletes) are touched.
 */
#ifndef CC_RESOURCE_LIMITS_H
#define CC_RESOURCE_LIMITS_H

typedef enum {
    LIMIT_FSIZE = 0,    /* RLIMIT_FSIZE: max file size a process may create */
    LIMIT_NOFILE,       /* RLIMIT_NOFILE: max open file descriptors         */
    LIMIT_CPU           /* RLIMIT_CPU: max CPU seconds (kills the child)    */
} LimitKind;

/* Run one demonstration. Returns 0 if the limit was applied AND its effect
 * was observed exactly as expected, else -1. */
int limits_run(LimitKind kind);

/* Run all three. Returns 0 only if all three succeeded. */
int limits_run_all(void);

#endif /* CC_RESOURCE_LIMITS_H */
