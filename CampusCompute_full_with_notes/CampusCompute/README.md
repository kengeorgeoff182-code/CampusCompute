# CampusCompute

Command-line prototype of a shared computing-lab job scheduler, written in C
for Linux (Ubuntu / WSL Ubuntu). It demonstrates job scheduling, a
producer-consumer logger with POSIX threads, shared-memory IPC with named
semaphores, the Banker's safety algorithm, worker failure detection with
`fork()`/`waitpid()`, and `setrlimit()` resource limits, with clean-up of
everything the program creates.

Every result is computed by the program at run time; nothing is hard-coded.
Worker failures are **simulated** and are labelled as such in the output.

## Prerequisites

* Ubuntu or WSL Ubuntu (developed and tested on Ubuntu 24.04, GCC 13.3, GNU Make 4.3), `gcc`, GNU `make`
* `python3` (optional: only for the independent reference checks in `tests/`)
* `zip` / `unzip` (optional: only for `make dist`)

```
sudo apt update && sudo apt install -y build-essential python3 zip unzip
```

No third-party libraries are used; only POSIX threads, POSIX shared memory
(`shm_open`), POSIX named semaphores (`sem_open`) and the C library.

## Build

```
make            # builds ./campuscompute
make clean      # removes build products
```

Compiler flags: `-std=c11 -Wall -Wextra -Wpedantic -Wshadow ...`, linked with
`-pthread -lrt`. The build produces no warnings.

## Run

```
./campuscompute help                 # all commands and options
make run                             # same as: ./campuscompute demo
```

| Command | Feature |
|---|---|
| `schedule fcfs\|rr\|priority\|compare [job options] [--quantum N]` | FR1, FR2 |
| `logger [--capacity N] [--producers N] [--messages N] [--producer-delay MS] [--consumer-delay MS] [--log PATH] [--quiet]` | FR3 |
| `ipc [job options] [--timeout S] [--simulate-consumer-crash]` | FR4 |
| `banker [--demo safe\|unsafe \| --file PATH]` | FR5 |
| `worker normal\|fail-exit\|fail-signal\|pool` | FR6 |
| `limits fsize\|nofile\|cpu\|all` | FR7 |
| `demo` | integrated run of everything, with a PASS/FAIL summary |
| `cleanup` | removes this program's leftover shm/semaphore objects |

Job options (for `schedule` and `ipc`): `--sample` (default), `--file PATH`
(one `ID ARRIVAL BURST PRIORITY` per line, `#` comments), and repeatable
`--job ID,ARRIVAL,BURST,PRIORITY`.

Examples:

```
./campuscompute schedule rr --quantum 2 --job 1,0,5,1 --job 2,1,3,2
./campuscompute schedule compare --file tests/data/jobs_ok.txt
./campuscompute logger --capacity 2 --producers 3 --messages 4
./campuscompute banker --file tests/data/banker_unsafe.txt
./campuscompute worker fail-signal
```

Exit status: `0` success, `1` a check failed, `2` bad command line or malformed
input (with a message on stderr). The program never crashes on bad input.

## Design decisions

**Scheduling (FR1/FR2).** Jobs have ID, arrival, CPU burst, priority and a
lifecycle status (`NEW -> READY -> RUNNING -> COMPLETED`). Each algorithm runs
on a private copy, so the input is never modified (checked by the program and
by a unit test). `Turnaround = Completion - Arrival`,
`Waiting = Turnaround - Burst`. When no job is ready the CPU idles until the
next arrival and the idle time is shown in the Gantt chart.

* **FCFS**: by arrival; tie on arrival -> lower job ID.
* **Round Robin**: FIFO ready queue; default quantum **3** (`--quantum`,
  1..1000). A job that arrives at or before the instant a slice ends is queued
  *before* the pre-empted job is re-queued; simultaneous arrivals are queued by
  lower ID.
* **Priority** (non-preemptive): **lower number = higher priority** (1 is most
  urgent). Tie -> earlier arrival -> lower job ID.

Input ranges: id 1..9999 (unique), arrival 0..10000, burst 1..1000,
priority 1..100, at most 64 jobs.

**Logger (FR3).** A circular buffer protected by one mutex. Two condition
variables (`not_full`, `not_empty`) block producers when the queue is full and
the consumer when it is empty; every wait sits in a `while` loop, so there is no
busy waiting. Shutdown sets a `closing` flag and broadcasts; the consumer
drains the queue, exits, and is joined. A producer blocked on a full queue is
released instead of hanging. File I/O happens outside the lock. The program
verifies per-producer ordering and the number of lines written.

**IPC (FR4).** A dispatcher process creates a POSIX shared-memory object holding
a 4-slot ring buffer and two named semaphores (free slots / filled slots). A
forked consumer re-opens them *by name*, so it has a genuinely separate
mapping, reads the job records (with a checksum), and writes a summary back
into the shared block. All waits use `sem_timedwait`, so a crashed peer cannot
hang the program. Object names include the user ID. Cleanup (`munmap`,
`sem_unlink`, `shm_unlink`) runs on success and on every error path and is
verified afterwards.

**Banker's algorithm (FR5).** Input file format (see `tests/data/`):
`processes`, `resources`, `available`, then `max` and `allocation` matrices.
**Need is always computed** (`Max - Allocation`), never entered. Inputs are
rejected when malformed, negative, out of range, when Allocation > Max, or when
a process's Max exceeds the total instances that exist. The safe sequence is
produced by the algorithm and then re-checked by a separate verifier.

**Workers (FR6).** `fork()` creates workers; the parent decodes the real
`waitpid()` status (`WIFEXITED`/`WEXITSTATUS` vs `WIFSIGNALED`/`WTERMSIG`) and
shows the raw value. Simulated failures: exit status 3, and `SIGTERM` raised by
the worker on itself. The pool mode reaps three concurrent workers with
`waitpid(-1)` and confirms no zombie remains (`ECHILD`).

**Resource limits (FR7).** Each limit is applied inside a forked child, so the
parent is unaffected (it prints its own limit before and after):

| Limit | Applied | Provoked | Observed |
|---|---|---|---|
| `RLIMIT_FSIZE` | 1024 bytes | write 4096 bytes to a temp file | exactly 1024 stored, then `EFBIG` |
| `RLIMIT_NOFILE` | 16 | open `/dev/null` repeatedly | `EMFILE` after the expected number of opens |
| `RLIMIT_CPU` | 1 s | busy loop | child killed by `SIGXCPU` |

Only a `mkstemp()` file under `/tmp` is created (and deleted) by these tests.

**Clean-up.** The program removes only objects it created: its own shm/semaphore
names (e.g. `/campuscompute_shm_<uid>`), its temp files, and the log files it
writes. It never touches unrelated files or other IPC objects.

## Testing

```
make test        # unit tests + integration tests
make unit        # C unit tests only            (tests/unit_tests.c)
make integration # CLI tests only               (tests/run_tests.sh)
make asan        # all tests under AddressSanitizer + UBSan
make tsan        # logger stress under ThreadSanitizer
python3 tests/ref_sched.py ./campuscompute   # independent scheduling reference
```

* Unit tests use hand-worked textbook values (FCFS 24/3/3, Round Robin q=4,
  priority example), tie-break cases, idle periods, the RR boundary rule, invalid
  inputs, status decoding, logger statistics and IPC clean-up.
* `tests/ref_sched.py` re-implements the three policies in Python from the
  written rules and compares per-job completion/turnaround/waiting and both
  averages for fixed and 150 random job sets.
* `tests/run_tests.sh` drives every command, checks exit codes and key output,
  re-verifies the printed Banker's safe sequence independently, and confirms no
  IPC object, temp file or process is left behind.

## Clean-up commands

```
make clean-ipc     # ./campuscompute cleanup: only this program's shm/semaphores
make clean-logs    # deletes logs/*.log (this project's logs only)
make distclean     # build products + logs
ls /dev/shm | grep campuscompute || echo "no CampusCompute IPC objects"
```

## Packaging

```
make dist           # CampusCompute_submission.zip (sources, headers, Makefile,
                    # README, tests; no build products or logs)
make verify-dist    # list the archive and run an integrity check (unzip -t)
```

## Project layout

```
include/   header files, one per module
src/       main.c (CLI)  job_manager.c  scheduler.c  sched_report.c  logger.c
           logger_demo.c  ipc.c  banker.c  worker.c  resource_limits.c  util.c
tests/     unit_tests.c  run_tests.sh  ref_sched.py  data/
logs/      log files written by the program
Makefile   README.md
```

## Known limits

* Linux only (uses `/dev/shm`-style POSIX shm, `sem_timedwait`, `/proc`).
* `RLIMIT` behaviour was verified on Ubuntu 24.04 (GCC 13). WSL 1 emulates
  parts of the kernel; WSL 2 behaves like native Linux.
* The programs are single-user tools; two copies run at the same time by the
  same user would share the same named IPC objects.
