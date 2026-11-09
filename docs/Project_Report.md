# CampusCompute
## Intelligent Job Scheduling and Resource Management

**Course:** Operating Systems — Level 3 Mini Project  
**Implementation Language:** C (C11)  
**Platform:** Linux / Ubuntu / WSL Ubuntu  
**Interface:** Command-Line Application

---

# 1. Project Overview

CampusCompute is a C-based command-line application that demonstrates fundamental Operating Systems concepts through an integrated job scheduling and resource management system.

The project brings together eight functional requirements (FR1–FR8):

- Job representation and lifecycle management.
- CPU scheduling using FCFS, Round Robin, and non-preemptive Priority.
- Concurrent producer–consumer logging using POSIX threads.
- Interprocess communication using POSIX shared memory and named semaphores.
- Deadlock avoidance using the Banker's safety algorithm.
- Process creation and termination-status detection using `fork()` and `waitpid()`.
- Process resource-limit demonstrations using `setrlimit()`.
- Command-line argument validation, integrated demonstrations, and cleanup.

CampusCompute is an educational simulator and demonstration program. It does not replace or modify the Linux kernel's actual CPU scheduler. It computes scheduling results in user space and uses Linux/POSIX facilities to demonstrate concurrency, IPC, process management, and resource limits.

# 2. Requirements and Feature Mapping

| Requirement | Feature | Main implementation files |
|---|---|---|
| FR1 | Job management and lifecycle | `job_manager.c`, `job_manager.h` |
| FR2 | FCFS, Round Robin, Priority scheduling and metrics | `scheduler.c`, `sched_report.c`, `scheduler.h` |
| FR3 | Producer–consumer logger with pthread synchronization | `logger.c`, `logger_demo.c`, `logger.h` |
| FR4 | Shared-memory IPC and named semaphore synchronization | `ipc.c`, `ipc.h` |
| FR5 | Banker's safety algorithm | `banker.c`, `banker.h` |
| FR6 | Worker processes, `fork()`, `waitpid()`, failure detection | `worker.c`, `worker.h` |
| FR7 | Process resource limits using `setrlimit()` | `resource_limits.c`, `resource_limits.h` |
| FR8 | CLI, argument parsing, command dispatch and integrated demo | `main.c`, `util.c`, `util.h` |

The headers in `include/` define the data structures, constants, enums, and function interfaces shared by the implementation files in `src/`.

# 3. High-Level Architecture

```text
                         USER
                           |
                           v
                 Command-Line Interface
                           |
                           v
                        main.c
                 Argument validation
                  and command dispatch
                           |
        +------------------+-------------------+
        |          |        |        |          |
        v          v        v        v          v
    Job Manager Scheduler Logger   IPC      Banker's
       FR1        FR2      FR3     FR4        FR5
        |          |        |        |          |
        |          |     pthreads  fork +      |
        |          |     mutexes   shared      |
        |          |     condvars  memory      |
        |          |               + sems      |
        |          |                            |
        +----------+----------------------------+
                           |
                  Additional OS features
                     /             \
                    v               v
              Worker Processes   Resource Limits
                   FR6                FR7
                    \                 /
                     \               /
                      v             v
                       Results and checks
                              |
                              v
                         CLI output
```

The `main.c` module is the application entry point. It parses the command and its options, constructs the required input data, invokes the relevant module, and converts the result into an application exit status.

The implementation is split into modules so that scheduling, IPC, logging, Banker's algorithm, worker processes, and resource limits can be developed and tested independently.

# 4. How the Application Runs

## 4.1 Build

From the project root:

```bash
make
```

The Makefile compiles every `src/*.c` file into an object file under `build/`, then links those objects into the `campuscompute` executable.

The build uses C11, POSIX feature macros, strict compiler warnings, pthread support, and the required system libraries.

## 4.2 Start the Application

```bash
./campuscompute help
```

The command-line interface displays the available commands and options.

## 4.3 Command Dispatch

The first command-line argument identifies the requested operation. `main.c` dispatches it to the corresponding command handler.

| Command | Handler / implementation |
|---|---|
| `schedule` | `cmd_schedule()` → job manager and scheduler |
| `logger` | `cmd_logger()` → logger demonstration |
| `ipc` | `cmd_ipc()` → shared-memory IPC demonstration |
| `banker` | `cmd_banker()` → Banker's algorithm |
| `worker` | `cmd_worker()` → worker-process module |
| `limits` | `cmd_limits()` → resource-limit module |
| `cleanup` | `cmd_cleanup()` → IPC cleanup |
| `demo` | `cmd_demo()` → integrated demonstration |

The program validates command names, required options, numeric values, and input-file data before executing the requested operation.

## 4.4 Exit Statuses

The application uses three main exit statuses:

| Code | Meaning |
|---:|---|
| `0` | Command completed successfully |
| `1` | Command ran, but an operation or verification failed |
| `2` | Invalid command-line arguments or malformed input |

These values are defined in `include/util.h`.

# 5. FR1 — Job Management and Lifecycle

A job represents a unit of work submitted to the scheduling system.

Each job contains:

- A unique job ID.
- An arrival time.
- A CPU burst time.
- A priority.
- A runtime lifecycle status.
- Runtime scheduling results such as completion, turnaround, and waiting time.

The permitted input ranges and maximum job count are defined in `include/job_manager.h`.

The job manager supports three ways of providing jobs:

1. Use the built-in sample job set.
2. Load jobs from a text file.
3. Supply jobs through repeatable `--job` command-line options.

A job-file record uses this format:

```text
ID ARRIVAL BURST PRIORITY
```

Blank lines and lines beginning with `#` are treated as comments.

The job manager validates job fields and rejects invalid values, duplicate IDs, and job sets that exceed the supported capacity.

The lifecycle is represented by the statuses:

```text
NEW -> READY -> RUNNING -> COMPLETED
```

The scheduler updates runtime state on its working copy of the job set. The original input is preserved so that different scheduling algorithms can be run against the same initial jobs.

# 6. FR2 — CPU Scheduling

The scheduler supports three algorithms.

## 6.1 First Come First Served (FCFS)

FCFS selects jobs in arrival order. If arrival times are equal, the lower job ID is selected first.

It is non-preemptive: once a job starts, it runs until its burst is complete.

## 6.2 Round Robin (RR)

Round Robin uses a FIFO ready queue and a fixed time quantum. The default quantum is 3, and the accepted range is 1 through 1000.

A running job receives up to one quantum of CPU time. If it has remaining burst time, it is returned to the ready queue.

The implementation has an explicit boundary rule: jobs arriving at or before the end of a time slice are queued before the pre-empted job is re-queued. Simultaneous arrivals are ordered by lower job ID.

## 6.3 Non-preemptive Priority

The scheduler chooses the highest-priority job among those that have arrived and are ready.

A lower numeric priority value represents a higher priority; priority 1 is the most urgent. Ties are resolved by earlier arrival time and then lower job ID.

This policy is non-preemptive.

## 6.4 Scheduling Metrics

For each job:

```text
Turnaround Time = Completion Time - Arrival Time
Waiting Time    = Turnaround Time - Burst Time
```

The scheduler calculates average turnaround time and average waiting time across the job set. It also records the Gantt-chart segments, completion order, finish time, idle time, and CPU utilization.

When no job is ready, the scheduler records an idle segment until the next job arrives.

## 6.5 Verification

The scheduler has an invariant checker that verifies, among other properties, that:

- Every job completes.
- Gantt segments are contiguous and non-overlapping.
- The total CPU time assigned to a job equals its burst time.
- A job never runs before its arrival time.
- Turnaround and waiting-time formulas hold.

The comparison operation runs all three policies on copies of the same input and checks that the original job data remains unchanged.

Example commands:

```bash
./campuscompute schedule fcfs
./campuscompute schedule rr --quantum 3
./campuscompute schedule priority
./campuscompute schedule compare
```

# 7. FR3 — Producer–Consumer Logger

The logger demonstrates concurrent threads communicating through a bounded circular queue.

There are multiple producer threads and one consumer thread:

```text
Producer 1 ----\
Producer 2 -----+----> Bounded Circular Queue ----> Consumer ----> Log file
Producer N ----/
```

Producers create log entries and submit them to the queue. The consumer removes queued entries and writes them to the configured log file.

The queue has a fixed capacity. When it is full, producers wait. When it is empty, the consumer waits.

The logger uses:

- A POSIX mutex to protect shared queue state.
- A `not_full` condition variable to block producers when the queue is full.
- A `not_empty` condition variable to block the consumer when the queue is empty.

Condition-variable waits are performed in `while` loops so that the queue condition is checked again after a thread wakes.

During shutdown, the logger marks itself as closing and broadcasts to waiting threads. The consumer drains queued messages, exits, and is joined. File I/O is performed outside the queue lock.

The demonstration verifies message counts, per-producer FIFO ordering, and the number of lines written to the log file.

Example:

```bash
./campuscompute logger --capacity 2 --producers 3 --messages 4
```

# 8. FR4 — Interprocess Communication

FR4 demonstrates communication between separate processes using POSIX shared memory and named semaphores.

The dispatcher creates:

- A shared-memory object.
- A four-slot ring buffer inside the shared region.
- A named semaphore counting free slots.
- A named semaphore counting filled slots.

The dispatcher then creates a consumer process using `fork()`. The consumer opens the shared-memory object and semaphores by name, giving it a separate mapping of the shared region.

The two semaphores coordinate access:

| Semaphore | Initial value | Meaning |
|---|---:|---|
| Empty-slot semaphore | 4 | Number of free ring-buffer slots |
| Filled-slot semaphore | 0 | Number of messages ready to consume |

The dispatcher waits for a free slot, writes a job record, and signals that an item is available. The consumer waits for an available item, reads it, and signals that a slot is free.

Each message contains a checksum. The consumer verifies the record and writes a summary back into shared memory. The parent reads that summary and compares it with the expected message count and burst-time total.

The waits use timed semaphore operations so that an unresponsive peer does not cause an indefinite wait. A simulated consumer-crash path tests failure detection and cleanup.

Cleanup releases the mapping and unlinks the named semaphores and shared-memory object. The program verifies that the named objects are no longer present.

Example:

```bash
./campuscompute ipc
./campuscompute ipc --simulate-consumer-crash
```

# 9. FR5 — Banker's Safety Algorithm

The Banker's algorithm checks whether a resource-allocation state is safe.

The input contains:

- Number of processes.
- Number of resource types.
- Available resource vector.
- Maximum demand matrix.
- Current allocation matrix.

The Need matrix is computed by the program:

```text
Need = Max - Allocation
```

It is not entered separately.

The safety check repeatedly searches for an unfinished process whose remaining Need is less than or equal to the currently available resources. It assumes that process can finish, then adds its allocated resources back to the available vector. The process repeats until all processes finish or no remaining process can proceed.

If all processes can finish in some order, the state is safe and the algorithm returns a safe sequence. Otherwise, the state is unsafe.

The program validates input dimensions and values, rejects negative or malformed data, rejects allocation values greater than maximum claims, and checks that a process's maximum claim does not exceed the total instances that exist.

After finding a safe sequence, the implementation verifies the sequence separately.

Example commands:

```bash
./campuscompute banker --demo safe
./campuscompute banker --demo unsafe
./campuscompute banker --file tests/data/banker_safe.txt
```

An unsafe state does not necessarily mean that deadlock has already occurred. It means that the algorithm cannot establish a sequence guaranteeing that all processes can finish.

# 10. FR6 — Worker Processes and Failure Detection

The worker module demonstrates process creation and parent-side termination-status handling.

The parent calls `fork()` to create a child process. The parent then calls `waitpid()` to collect the child's termination status.

The implementation demonstrates three scenarios:

1. Normal completion with exit status 0.
2. A simulated failure in which the child exits with status 3.
3. A simulated signal failure in which the child sends `SIGTERM` to itself.

The parent decodes the status using the `waitpid()` status macros:

- `WIFEXITED(status)` checks whether the child exited normally.
- `WEXITSTATUS(status)` obtains the normal exit code.
- `WIFSIGNALED(status)` checks whether a signal terminated the child.
- `WTERMSIG(status)` obtains the terminating signal number.

The worker-pool mode starts three workers and reaps whichever child finishes first using `waitpid(-1, ...)`. It checks that every child is collected and that no child remains unreaped.

The failures are intentionally simulated by the worker program. They are not real operating-system faults.

Example:

```bash
./campuscompute worker normal
./campuscompute worker fail-exit
./campuscompute worker fail-signal
./campuscompute worker pool
```

# 11. FR7 — Process Resource Limits

The resource-limit module demonstrates three POSIX resource limits using `setrlimit()`.

Each demonstration runs in a forked child. This isolates the resource limit from the main parent process.

## 11.1 `RLIMIT_FSIZE`

Limits the maximum file size the process may create. The demonstration applies a 1024-byte limit and attempts to write 4096 bytes. It observes the resulting file-size limit behaviour and the `EFBIG` error.

## 11.2 `RLIMIT_NOFILE`

Limits the number of file descriptors the process may open. The demonstration applies a limit of 16 and repeatedly opens `/dev/null` until the process reaches the descriptor limit and observes `EMFILE`.

## 11.3 `RLIMIT_CPU`

Limits CPU time. The demonstration applies a one-second CPU limit and runs a busy loop so that the CPU-time limit is exercised. The child is expected to receive `SIGXCPU`.

The module checks the observed effects and confirms that the parent's resource limit is unchanged.

Example:

```bash
./campuscompute limits fsize
./campuscompute limits nofile
./campuscompute limits cpu
./campuscompute limits all
```

# 12. FR8 — Command-Line Interface and Integrated Demonstration

The CLI is implemented in `src/main.c`.

It provides command handlers for scheduling, logging, IPC, Banker's algorithm, worker processes, resource limits, cleanup, and the integrated demo.

The CLI validates:

- Required command and subcommand names.
- Numeric option values and their ranges.
- Required option values.
- Scheduling policies and Round Robin quantum.
- Job specifications and job-file paths.
- Banker's algorithm input files and demo names.
- Unexpected or unsupported options.

The integrated command:

```bash
./campuscompute demo
```

runs the project's feature demonstrations and produces a PASS/FAIL summary. It also checks cleanup-related conditions such as remaining IPC objects and processes.

# 13. File-by-File Explanation

This section describes the files in the supplied repository archive. It distinguishes interface/header files, implementation files, tests, and project-support files.

## 13.1 Root-Level Files

### `.gitattributes`

Defines Git's handling of repository files, such as text-file treatment and line-ending behaviour. It helps keep file handling consistent across development environments.

### `.gitignore`

Lists generated or local files that should not be committed to Git. In this project, it excludes build products, executables, generated logs, archives, and editor/operating-system files while preserving the `logs/.keep` placeholder.

### `Makefile`

Defines the project's build, test, cleanup, sanitizer, and packaging operations.

Important targets:

| Target | Purpose |
|---|---|
| `make` / `make all` | Compile and link `campuscompute` |
| `make run` | Run the integrated demo |
| `make unit` | Build and run C unit tests |
| `make integration` | Run CLI/integration tests |
| `make test` | Run unit and integration tests |
| `make asan` | Build and test with AddressSanitizer and UBSan |
| `make tsan` | Build and run logger stress checks with ThreadSanitizer |
| `make clean` | Remove generated executables and build directories |
| `make clean-ipc` | Invoke the application's IPC cleanup |
| `make clean-logs` | Remove generated project log files |
| `make distclean` | Clean build products and generated logs |
| `make dist` | Create `CampusCompute_submission.zip` from selected project files |
| `make verify-dist` | List the archive and check ZIP integrity |
| `make help` | Display selected Makefile help text |

The Makefile compiles source files into object files under `build/`, then links them. It also creates a separate unit-test executable. The distribution target stages only the Makefile, README, source, headers, and tests, and creates the `logs/.keep` placeholder. It deliberately excludes build products, generated logs, private working notes, and old archives.

### `README.md`

The main project guide. It documents prerequisites, build and run commands, CLI commands, design decisions, algorithm semantics, testing, cleanup, packaging, and the project directory layout.

## 13.2 Header Files — `include/`

Header files declare the public interfaces used by the implementation modules. They define structures, enums, constants, and function prototypes.

### `include/banker.h`

Defines the Banker's algorithm data model and interface.

It declares the resource/process limits, the state and result structures, parsing and file-loading functions, Need computation, input validation, safe/unsafe demo setup, safety checking, sequence verification, and result printing.

### `include/ipc.h`

Defines the IPC demonstration interface and options.

It declares the ring-buffer slot count, default and maximum timeout values, IPC option structure, default-option initializer, IPC demonstration function, named-object cleanup function, and resource-presence check.

### `include/job_manager.h`

Defines the job model and job-set interface.

It declares the maximum job count and valid ranges for IDs, arrival times, burst times, and priorities. It also defines job lifecycle states, job and job-set structures, validation and insertion functions, command-line job-spec parsing, file loading, sample-job loading, runtime reset, status naming, and job printing.

### `include/logger.h`

Defines logger data structures and interfaces.

It declares log levels, queue/message limits, logger state and statistics structures, logger initialization/start/log/stop/destroy functions, statistics retrieval, and the options and entry point for the producer–consumer demonstration.

### `include/resource_limits.h`

Defines the resource-limit types and functions.

It declares the supported limit kinds—file size, open file descriptors, and CPU time—and the functions to run one or all demonstrations.

### `include/scheduler.h`

Defines the scheduling algorithms, result structures, constants, and functions.

It declares FCFS, Round Robin, and Priority algorithm identifiers; Gantt segments; scheduling results; the default and allowed Round Robin quantum; algorithm execution; result cleanup; invariant verification; Gantt and result printing; comparison; and run-and-print functions.

### `include/util.h`

Defines shared utility functions and application-wide constants.

It declares the application version and exit codes, strict integer parsing, formatted error reporting, millisecond sleep, timestamp formatting, output banners and rules, parent-directory creation, and a small write helper.

### `include/worker.h`

Defines worker scenarios and worker-result types.

It declares normal, simulated non-zero-exit, and simulated signal-termination scenarios; decoded worker outcome types; status-classification and description functions; single-worker execution; and the three-worker pool demonstration.

## 13.3 C Implementation Files — `src/`

### `src/main.c`

The application entry point and command-line controller.

It prints usage information, validates options, parses job arguments, and dispatches commands to their respective modules. It contains handlers for scheduling, logger, IPC, Banker's algorithm, worker processes, resource limits, cleanup, and the integrated demo.

It also coordinates the integrated demonstration and its PASS/FAIL summary. This file connects the independently implemented modules into one CLI application.

### `src/job_manager.c`

Implements FR1.

It initializes job sets, validates job attributes, adds jobs, parses comma-separated job specifications, loads jobs from files, loads the built-in sample set, resets runtime state, converts lifecycle states to display names, and prints job sets.

### `src/scheduler.c`

Implements the scheduling algorithms for FR2.

It contains the FCFS, Round Robin, and non-preemptive Priority scheduling logic. It manages ready-queue behaviour, Gantt segments, job completion calculations, result allocation and cleanup, and scheduling invariant verification.

### `src/sched_report.c`

Implements scheduling output and comparison reporting.

It prints the policy rules, Gantt charts, completion order, per-job scheduling results, and averages. It also provides the run-and-print and compare operations used by the CLI.

### `src/logger.c`

Implements the core thread-safe logger for FR3.

It manages the bounded circular queue, mutex, condition variables, consumer thread, blocking dequeue, log-file writing, graceful shutdown, cleanup, and logger statistics.

### `src/logger_demo.c`

Implements the producer–consumer demonstration around the logger.

It creates producer threads, configures demonstration options, generates messages, runs the consumer/logger, and checks the results. Its verification includes message counts, per-producer ordering, and the number of lines written.

### `src/ipc.c`

Implements FR4.

It creates and manages POSIX shared memory and named semaphores, constructs per-user IPC object names, coordinates the dispatcher and forked consumer, calculates and checks message checksums, uses timed semaphore waits, handles the simulated consumer failure, verifies returned summary data, and cleans up IPC resources.

### `src/banker.c`

Implements FR5.

It parses Banker's algorithm input, computes the Need matrix, validates resource configurations, provides safe and unsafe demonstration states, executes the safety algorithm, verifies a proposed safe sequence, and prints the state and result.

### `src/worker.c`

Implements FR6.

It creates child processes with `fork()`, runs the selected worker scenario, waits for child termination, decodes `waitpid()` status, checks whether the observed result matches the expected scenario, and implements the concurrent worker-pool demonstration.

### `src/resource_limits.c`

Implements FR7.

It applies `RLIMIT_FSIZE`, `RLIMIT_NOFILE`, and `RLIMIT_CPU` inside child processes. It provokes and observes each limit's effect, waits for child completion, checks temporary-file cleanup, and provides the combined all-limits demonstration.

### `src/util.c`

Implements shared helper functions used by multiple modules.

It provides strict integer parsing, formatted error output, interrupt-aware sleeping, timestamp formatting, consistent banners and horizontal rules, parent-directory creation, and a low-level string-writing helper.

## 13.4 Test Files — `tests/`

### `tests/unit_tests.c`

Contains C unit tests for individual functions and module behaviour.

The tests cover utility parsing, job validation, scheduling examples and edge cases, Banker's algorithm, worker status decoding, logger behaviour, and IPC-related behaviour and cleanup.

### `tests/run_tests.sh`

Runs command-line and integration tests against the compiled executable.

It checks CLI usage and invalid input, scheduling output, independent scheduling-reference agreement, logger behaviour, IPC success and failure cleanup, Banker's algorithm safe and unsafe cases, worker scenarios, resource limits, and the integrated demo.

The script also checks for leftover IPC objects, temporary files, and processes where applicable.

### `tests/ref_sched.py`

Provides an independent Python reference implementation of the scheduling policies. It compares the executable's scheduling results with the reference calculations for fixed and generated job sets. This is separate from the C scheduler implementation and helps detect algorithmic mismatches.

### `tests/data/`

Contains test input files used by the integration tests.

#### Banker's Algorithm Data

| File | Purpose |
|---|---|
| `banker_safe.txt` | Valid safe resource-allocation state |
| `banker_unsafe.txt` | Valid but unsafe resource-allocation state |
| `banker_invalid_alloc_gt_max.txt` | Tests rejection when allocation exceeds maximum claim |
| `banker_invalid_counts.txt` | Tests invalid process/resource counts |
| `banker_invalid_max_gt_total.txt` | Tests a maximum claim greater than total existing instances |
| `banker_invalid_negative.txt` | Tests rejection of negative values |
| `banker_malformed.txt` | Tests malformed Banker's input |

#### Job-Scheduling Data

| File | Purpose |
|---|---|
| `jobs_ok.txt` | Valid job input for scheduling |
| `jobs_bad_line.txt` | Tests malformed job-file records |
| `jobs_duplicate.txt` | Tests duplicate job IDs |

## 13.5 Documentation and Log Directories

### `docs/TRACEABILITY.md`

Maps project requirements to implementation and verification evidence. It is a project-support document and is not needed to compile or run the program.

### `docs/captions.txt`

Contains captions for the screenshot evidence sequence. It supports manual assembly of the screenshot PDF and is not required by the executable.

### `logs/.keep`

An empty placeholder that keeps the `logs/` directory present in the repository/archive. Runtime log files are generated when the logger or integrated demo runs. The `.keep` file is not a log file and does not contain application output.

### Generated `build/` Directory

The `build/` directory is created by `make`. It contains compiled object files, dependency files, and the unit-test executable. It is generated output rather than hand-written source and is removed by `make clean`. It is not included in the source submission archive.

# 14. How the Modules Work Together

The main data flow for scheduling is:

```text
CLI arguments / sample jobs / job file
                 |
                 v
          job_manager.c
        Validate and create
             JobSet
                 |
                 v
           scheduler.c
        Run selected policy
                 |
                 v
          ScheduleResult
                 |
                 v
          sched_report.c
       Gantt chart and metrics
                 |
                 v
             CLI output
```

The logger flow is:

```text
Producer threads
       |
       v
logger_log()
       |
       v
Mutex-protected bounded queue
       |
       v
Consumer thread
       |
       v
Log file
```

The IPC flow is:

```text
Dispatcher parent
       |
       +---- creates shared memory and semaphores
       |
       +---- fork()
                 |
                 v
           Consumer child
                 |
       Opens IPC objects by name
                 |
       Reads records and verifies checksums
                 |
       Writes summary into shared memory
                 |
                 v
          Parent verifies summary
                 |
                 v
           IPC cleanup
```

The Banker's, worker, and resource-limit modules are invoked independently through the CLI and are also exercised by the integrated demonstration.

# 15. Build, Run, Test, and Package

From the repository root:

```bash
# Build the application
make

# Display available commands
./campuscompute help

# Run the integrated demonstration
./campuscompute demo

# Run unit and integration tests
make test

# Remove generated build products
make clean

# Build the submission archive
make dist

# List and integrity-check the archive
make verify-dist
```

Useful individual commands:

```bash
./campuscompute schedule compare
./campuscompute logger
./campuscompute ipc
./campuscompute banker --demo safe
./campuscompute banker --demo unsafe
./campuscompute worker pool
./campuscompute limits all
./campuscompute cleanup
```

# 16. Verification Evidence

The project verification results reported from the user's WSL run were:

| Verification | Reported result |
|---|---:|
| C unit tests | 277 checks, 0 failed |
| Integration tests | 129 passed, 0 failed |
| Independent scheduling reference | 180 job sets × 3 policies, 0 mismatches |
| Integrated demonstration | 14 of 14 stages passed |
| IPC cleanup | No CampusCompute IPC objects left |
| Worker cleanup | Child processes reaped; no remaining child processes |

These figures are recorded from the WSL test output supplied during the project conversation. They are not a new test execution performed while generating this report.

# 17. Important Concepts to Understand

For a project demonstration or viva, be prepared to explain:

1. The difference between a process and a thread.
2. The difference between preemptive and non-preemptive scheduling.
3. How FCFS, Round Robin, and Priority scheduling select jobs.
4. How completion, turnaround, and waiting times are calculated.
5. Why the producer–consumer queue requires a mutex and condition variables.
6. How condition variables prevent busy waiting.
7. Why shared memory is used for communication between separate processes.
8. How the empty-slot and filled-slot semaphores coordinate the ring buffer.
9. What a safe sequence means in the Banker's algorithm.
10. How `fork()` creates a child and how `waitpid()` reports its termination.
11. The difference between normal exit and signal termination.
12. What `setrlimit()` controls and why the demonstrations run in child processes.
13. How the program validates inputs and cleans up IPC objects and child processes.

A useful explanation pattern for every functional requirement is:

- What operating-system problem does it demonstrate?
- Which algorithm, data structure, or system call is used?
- How does the implementation work?
- How does the program verify the result?