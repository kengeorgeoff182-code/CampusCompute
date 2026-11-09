# Requirements traceability matrix (private working notes - not part of the ZIP)

Spec: *Level 3 Mini-Project - CampusCompute* (UE25CS242A). "Tests" names the
unit-test section (U: `tests/unit_tests.c`) or integration group (I:
`tests/run_tests.sh`). "Shot" is the screenshot that demonstrates it.

| Req | Module (src/) | Key functions | Tests | Shot |
|---|---|---|---|---|
| FR1 job fields, validation, lifecycle | `job_manager.c` | `job_validate`, `jobset_add`, `jobset_add_spec`, `jobset_load_file`, `jobset_reset_runtime` | U "job manager (FR1)"; I "schedule: bad --job / bad job file / duplicate id" | S06-S08, S19 |
| FR2 FCFS | `scheduler.c`, `sched_report.c` | `sched_run(ALGO_FCFS)`, `sched_print_result` | U FCFS textbook, idle period; I FCFS; `ref_sched.py` | S06 |
| FR2 Round Robin, quantum stated | `scheduler.c` | `sched_run(ALGO_RR)`, quantum check 1..1000 | U RR q=4, RR boundary rule, quantum validation; I RR q=3; `ref_sched.py` | S07 |
| FR2 Priority, convention + tie-break | `scheduler.c` | `sched_run(ALGO_PRIORITY)` | U Priority textbook, tie-breaking; I Priority; `ref_sched.py` | S08 |
| FR2 comparison, inputs not corrupted | `sched_report.c` | `sched_compare`, `sched_verify_result` | U "work on copies"; I compare | S09 |
| FR3 bounded queue, mutex, condvars, clean exit | `logger.c`, `logger_demo.c` | `logger_init/start/log/stop/destroy`, `logger_run_demo` | U logger (capacity 1, stats, reject after stop); I logger x3; `make tsan` | S10, S11, S22 |
| FR4 shm + named semaphores, error handling, cleanup | `ipc.c` | `ipc_run_demo`, `ipc_cleanup_named`, `ipc_resources_present` | U ipc (normal, crash path, NULL); I ipc, ipc crash, leftover checks | S12, S13 |
| FR5 Banker (Need = Max - Alloc), safe + unsafe | `banker.c` | `banker_parse`, `banker_validate`, `banker_check_safety`, `banker_verify_sequence`, `banker_run` | U banker x4 sections; I banker group + Python re-verification | S14, S15 |
| FR6 fork + waitpid, normal and simulated failure | `worker.c` | `worker_run_scenario`, `worker_run_pool`, `worker_classify` | U status decoding + real scenarios; I worker x4 | S16, S17 |
| FR7 setrlimit + observed effect | `resource_limits.c` | `limits_run`, `limits_run_all` | I limits x4 | S18 |
| FR7 cleanup (threads, children, files, shm, semaphores) | `ipc.c`, `logger.c`, `main.c` | `verify_cleanup`, `ipc_cleanup_named` | I leftover checks, `/dev/shm`, `pgrep -x`; ASan | S20 |
| FR8 CLI, usage, errors, independent testability | `main.c` | `cmd_*`, `usage_error`, `opt_long` | I "FR8" group (31 invalid-input cases) | S05, S19 |
| Integrated execution | `main.c` | `cmd_demo` | I demo | S21 |
| Generated logs | `logger.c` | log file in `logs/` | I demo log checks | S22 |
| Build from source, Makefile, README | `Makefile`, `README.md` | targets all/clean/run/test/dist | clean rebuild + extracted-ZIP rebuild | S02-S04, S23 |
| Submission ZIP + integrity | `Makefile` | `dist`, `verify-dist` | `unzip -t` | S24 |

## Rule checklist (spec section 3 and 5)

| Rule | Status |
|---|---|
| C on Linux/Ubuntu/WSL; builds from source with Makefile | verified on Ubuntu 24.04 / GCC 13.3 and WSL Ubuntu 26.04 / GCC 15.2 |
| No hard-coded output; repeatable test inputs | results computed at run time; fixed data in `tests/data/`; independent Python reference |
| Simulated failures clearly identified | every simulated failure prints "SIMULATED ... not a real fault" |
| Clean up only own resources | per-user shm/semaphore names; own temp files; own logs |
| Git/GitHub not required | not used |
| Real SRN in every terminal prompt | **you must do this** (see SCREENSHOTS.md) |
| Genuine screenshots S01-S24, one line each, no edits | **you must capture these** |
| One PDF named `<SRN>.pdf`, no separate report | `docs/build_pdf.py` assembles it from your real screenshots |
| ZIP of source + build files | `make dist` (README included, docs/ excluded) |

## Decisions where the spec leaves a choice

* Priority: lower number = higher priority; ties by arrival then ID.
* Round Robin: default quantum 3; arrivals at a slice boundary queue first.
* Banker: a state whose Max exceeds the total instances is rejected as invalid.
* IPC primitive: named POSIX semaphores (counting, bounded buffer), not a mutex.
* Limits: three demonstrated (FSIZE, NOFILE, CPU), each in a forked child.
* A simulated IPC fault (consumer exits) is reported as a failed run (exit 1).

## Unverified in this build environment

* WSL Ubuntu (tested on a native Ubuntu 24.04 container).
* Screenshots, the final PDF and the submission ZIP *as you will capture them*;
  only the tooling was tested, using throw-away dummy images that are not
  included anywhere.
