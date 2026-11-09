# Screenshot capture checklist S01-S24 (private working notes - not in the ZIP)

You must capture these yourself in your own terminal. They are not, and cannot
be, produced for you: the assignment requires genuine screenshots with your
real SRN in the prompt and forbids edited or fabricated output.

## Before you start (every new terminal window)

```
export SRN='YOUR_FULL_SRN'        # replace with your real, complete SRN
export PS1='[$SRN] \w\$ '
cd ~/CampusCompute                # wherever you unzipped the project
clear
```

* Use a terminal at least 110 columns x 45 rows, large readable font.
* Capture the whole window so the prompt line **and** the command are visible.
* `clear` before each capture. Do not crop out the prompt. Do not edit anything.
* Do the captures **in order**: S22 needs the logs created by S10/S11/S21, and
  S24 (`make dist`) deletes the executable and logs, so it must be last.
* Save as `S01.png` ... `S24.png` in one folder (e.g. `~/shots`).
* Piping to `head`/`tail` below is safe: the program ignores SIGPIPE and still
  finishes and cleans up.

## Commands, expected output, prerequisites

| # | Command(s) | You should see | Prerequisite |
|---|---|---|---|
| S01 | `pwd; uname -sr; head -2 /etc/os-release` | Ubuntu/WSL info, `[SRN]` prompt, project directory | prompt set |
| S02 | `gcc --version \| head -1; make --version \| head -1` | GCC and Make versions | - |
| S03 | `ls -l; ls src include tests` | Makefile, README, source and header files | - |
| S04 | `make clean && make` | every `.c` compiled, linked, no warnings/errors | - |
| S05 | `./campuscompute help` | commands, options, exit codes | S04 |
| S06 | `./campuscompute schedule fcfs` | order, Gantt, per-job table, averages | S04 |
| S07 | `./campuscompute schedule rr --quantum 3` | quantum 3, order, metrics | S04 |
| S08 | `./campuscompute schedule priority` | priorities, tie-break rule, metrics | S04 |
| S09 | `./campuscompute schedule compare` | three policies on one job set, comparison table | S04 |
| S10 | `./campuscompute logger --capacity 2 --producers 2 --messages 3 --consumer-delay 5 \| head -30; tail -6 logs/campuscompute.log` | ENQUEUE/DEQUEUE/BLOCKED lines, then real log lines | S04 |
| S11 | `./campuscompute logger --capacity 2 --producers 2 --messages 3 --consumer-delay 5 \| tail -18` | producers finished/joined, consumer exiting, statistics, PASS | S04 |
| S12 | `./campuscompute ipc \| head -26` | shm + semaphore names, dispatcher/consumer exchange, checksum OK | S04 |
| S13 | `./campuscompute ipc \| tail -16; ls /dev/shm \| grep campuscompute \|\| echo "no CampusCompute IPC objects left"` | data verified, sem_unlink/shm_unlink, objects gone | S04 |
| S14 | `./campuscompute banker --demo safe` | Need matrix, steps, RESULT: SAFE, sequence | S04 |
| S15 | `./campuscompute banker --demo unsafe` | RESULT: UNSAFE, explanation | S04 |
| S16 | `./campuscompute worker normal` | fork, waitpid, exit status 0, no zombie | S04 |
| S17 | `./campuscompute worker fail-exit; ./campuscompute worker fail-signal` | SIMULATED labels, status 3 and signal 15 detected | S04 |
| S18 | `./campuscompute limits fsize` | limit 1024, write refused with errno 27, parent unchanged, PASS | S04 |
| S19 | `./campuscompute schedule rr --quantum 0; echo "exit status: $?"; ./campuscompute frobnicate; echo "exit status: $?"` | error messages, exit status 2 | S04 |
| S20 | `./campuscompute cleanup; ls /dev/shm \| grep campuscompute \|\| echo "no IPC objects"; pgrep -x campuscompute \|\| echo "no campuscompute process running"` | nothing left | S04 |
| S21 | `./campuscompute demo \| tail -30` | cleanup verification and the 14-stage summary, OVERALL: PASS | S04 |
| S22 | `ls -l logs; head -12 logs/campuscompute.log; head -8 logs/demo_producer_consumer.log` | log files with timestamped entries | S10, S11, S21 done |
| S23 | `ls -l src include tests Makefile README.md campuscompute` | final files and executable | S04 (executable present) |
| S24 | `make dist > /dev/null; unzip -l CampusCompute_submission.zip \| tail -15; make verify-dist \| tail -2` | archive listing, "No errors detected" | **last**; needs zip |

Optional extra evidence you may add *inside* a numbered screenshot (never as a
new number): `make test 2>&1 | tail -5` (all tests pass).

## Build the single PDF

```
python3 -m pip install --user reportlab pillow      # once, if missing
python3 docs/build_pdf.py --srn YOUR_FULL_SRN --shots ~/shots
```

This creates `YOUR_FULL_SRN.pdf` (the filename is your SRN only) with one
screenshot per page followed by its one-line explanation from
`docs/captions.txt`. It stops with an error if any of S01-S24 is missing, if
the SRN looks like the assignment's placeholder, or if a caption is absent.
Edit `docs/captions.txt` if a sentence does not match what your screenshot
shows. Open the PDF and check every page, and that the SRN is readable in each
screenshot, before submitting.

## Build and check the ZIP (S24)

```
make dist           # CampusCompute_submission.zip
make verify-dist    # lists contents and runs: unzip -t
```

Rename the ZIP only if your portal requires a specific name. The ZIP contains
the sources, headers, Makefile, README and tests; it does not contain the
PDF, build products, logs or these notes (the assignment forbids a separate
report). Submit the PDF as a separate file unless your portal says otherwise.
