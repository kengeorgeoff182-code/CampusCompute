/*
 * worker.h - FR6: worker processes (fork) and failure detection (waitpid).
 *
 * The parent never assumes how a worker ended; it reports exactly what
 * waitpid() returned, decoded with the W* macros:
 *   WIFEXITED   -> normal termination, WEXITSTATUS gives the exit code
 *                  (0 = success, non-zero = failure reported by the worker)
 *   WIFSIGNALED -> killed by a signal, WTERMSIG gives the signal number
 *
 * Failures here are SIMULATED: the worker deliberately exits non-zero or
 * raises a signal on itself. They are labelled as such in the output and are
 * not real operating-system faults.
 */
#ifndef CC_WORKER_H
#define CC_WORKER_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
    WORKER_NORMAL = 0,      /* does some work and exits 0                  */
    WORKER_FAIL_EXIT,       /* SIMULATED failure: exits with status 3      */
    WORKER_FAIL_SIGNAL      /* SIMULATED failure: raises SIGTERM on itself */
} WorkerScenario;

typedef enum {
    WOUT_EXIT_OK = 0,       /* exited, status 0        */
    WOUT_EXIT_NONZERO,      /* exited, status != 0     */
    WOUT_SIGNALED,          /* terminated by a signal  */
    WOUT_OTHER              /* stopped/continued/unknown */
} WorkerOutcomeKind;

typedef struct {
    WorkerOutcomeKind kind;
    int exit_code;          /* valid for WOUT_EXIT_* */
    int signal;             /* valid for WOUT_SIGNALED */
    bool core_dumped;
} WorkerOutcome;

#define WORKER_SIM_EXIT_CODE 3

/* Decode a raw waitpid() status. */
WorkerOutcome worker_classify(int wait_status);

/* One-line human-readable description of a raw waitpid() status. */
void worker_describe_status(int wait_status, char *buf, size_t len);

const char *worker_scenario_name(WorkerScenario s);

/*
 * Fork one worker for 'scenario', wait for it, print and check the observed
 * status against what the scenario should produce.
 * Returns 0 if the parent detected exactly the expected outcome, else -1.
 */
int worker_run_scenario(WorkerScenario scenario);

/*
 * Start three workers at once (normal, simulated exit failure, simulated
 * signal failure) and reap them with waitpid(-1,...) as they finish. Prints a
 * summary table. Returns 0 if all three outcomes were detected correctly and
 * no child remains un-reaped.
 */
int worker_run_pool(void);

#endif /* CC_WORKER_H */
