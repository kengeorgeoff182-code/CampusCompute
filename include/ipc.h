/*
 * ipc.h - FR4: inter-process communication with POSIX shared memory and
 * named POSIX semaphores.
 *
 * Two cooperating processes:
 *   dispatcher (parent)  creates the shared-memory object and the semaphores,
 *                        writes job records into a 4-slot ring buffer;
 *   consumer   (child)   created with fork(); it RE-OPENS the objects by name
 *                        (shm_open / sem_open) so it is a genuinely separate
 *                        mapping, reads the records, and writes a summary
 *                        (count, sum of bursts, checksum failures) back into
 *                        the shared block for the parent to read.
 *
 * Synchronisation (classic bounded buffer, one producer / one consumer):
 *   sem_empty (initial IPC_SLOTS) counts free slots,
 *   sem_full  (initial 0)         counts filled slots.
 * All waits use sem_timedwait so a crashed peer cannot hang the program.
 *
 * Object names contain the user id, e.g. /campuscompute_shm_1000, so they can
 * never collide with another user's objects and ipc_cleanup_named() can only
 * ever remove objects belonging to this program.
 */
#ifndef CC_IPC_H
#define CC_IPC_H

#include <stdbool.h>
#include "job_manager.h"

#define IPC_SLOTS             4
#define IPC_DEFAULT_TIMEOUT_S 5
#define IPC_MAX_TIMEOUT_S     60

typedef enum {
    IPC_FAULT_NONE = 0,
    IPC_FAULT_CONSUMER_CRASH    /* SIMULATED: consumer exits(4) after 1 msg */
} IpcFault;

typedef struct {
    IpcFault fault;
    int timeout_s;              /* per-wait timeout */
} IpcOptions;

void ipc_default_options(IpcOptions *o);

/*
 * Run the two-process shared-memory demonstration using 'jobs' as the data to
 * exchange. Always releases every object it created, on success and on every
 * error path, and verifies the release. Returns 0 on success, -1 on failure.
 */
int ipc_run_demo(const JobSet *jobs, const IpcOptions *opt);

/*
 * Remove this program's named IPC objects (shm + 2 semaphores for the
 * current user) if they exist -- e.g. leftovers from a crashed run. Nothing
 * else is touched. Returns the number of objects removed.
 */
int ipc_cleanup_named(bool verbose);

/* True if any of this program's named IPC objects currently exists. */
bool ipc_resources_present(void);

#endif /* CC_IPC_H */
