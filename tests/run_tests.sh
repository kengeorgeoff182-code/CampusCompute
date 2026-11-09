#!/usr/bin/env bash
# CampusCompute integration tests: drives the real binary through the CLI.
# Usage: bash tests/run_tests.sh [path-to-binary]      (run from project root)
# Exit status 0 only if every check passed.

BIN="${1:-./campuscompute}"
DATA="tests/data"

OUT="$(mktemp /tmp/cc_test_out.XXXXXX)"
ERR="$(mktemp /tmp/cc_test_err.XXXXXX)"
BANKER_OUT="$(mktemp /tmp/cc_banker_safe.XXXXXX)"

pass=0; fail=0

cleanup() {
    rm -f "$OUT" "$ERR" "$BANKER_OUT" "${LOGF:-}"
}

trap cleanup EXIT

if [ ! -x "$BIN" ]; then
    echo "error: binary '$BIN' not found - run 'make' first" >&2
    exit 2
fi

ok()   { pass=$((pass + 1)); printf '  PASS  %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; if [ -n "$2" ]; then printf '        %s\n' "$2"; fi; return 0; }

# run NAME EXPECTED_EXIT -- command args...   (stdout -> $OUT, stderr -> $ERR)
run() {
    local name="$1" want="$2"; shift 3
    timeout 60 "$@" >"$OUT" 2>"$ERR"
    local got=$?
    if [ "$got" -eq "$want" ]; then ok "$name (exit $got)"
    else bad "$name" "expected exit $want, got $got"; fi
}
# has_out PATTERN / has_err PATTERN : fixed-string match on captured output
has_out() { grep -qF -- "$1" "$OUT" && ok "  output contains: $1" || bad "  output missing: $1"; }
has_err() { grep -qF -- "$1" "$ERR" && ok "  stderr contains: $1" || bad "  stderr missing: $1"; }

leftover_check() {
    local shm; shm=$(ls /dev/shm 2>/dev/null | grep -c campuscompute)
    if [ "$shm" -eq 0 ]; then ok "no CampusCompute objects left in /dev/shm ($1)"
    else bad "leftover IPC objects in /dev/shm ($1)"; fi
}

mkdir -p logs
echo "== FR8: CLI usage and error handling =="
run "no arguments"            2 -- "$BIN";                          has_err "Usage:"
run "help"                    0 -- "$BIN" help;                     has_out "Usage:"
run "version"                 0 -- "$BIN" version;                  has_out "CampusCompute"
run "unknown command"         2 -- "$BIN" frobnicate;               has_err "unknown command"
run "schedule: no policy"     2 -- "$BIN" schedule;                 has_err "needs one of"
run "schedule: bad policy"    2 -- "$BIN" schedule sjf;             has_err "unknown scheduling policy"
run "schedule: quantum 0"     2 -- "$BIN" schedule rr --quantum 0;  has_err "out of range"
run "schedule: quantum abc"   2 -- "$BIN" schedule rr --quantum abc; has_err "not a whole number"
run "schedule: quantum missing" 2 -- "$BIN" schedule rr --quantum;  has_err "needs a value"
run "schedule: bad option"    2 -- "$BIN" schedule fcfs --turbo;    has_err "unknown option"
run "schedule: bad --job"     2 -- "$BIN" schedule fcfs --job 1,0,0,1; has_err "burst"
run "schedule: --job fields"  2 -- "$BIN" schedule fcfs --job 1,2,3;
run "schedule: missing file"  2 -- "$BIN" schedule fcfs --file nope.txt; has_err "nope.txt"
run "schedule: bad job file"  2 -- "$BIN" schedule fcfs --file $DATA/jobs_bad_line.txt; has_err "line 2"
run "schedule: duplicate id"  2 -- "$BIN" schedule fcfs --file $DATA/jobs_duplicate.txt
run "schedule: --sample + --job" 2 -- "$BIN" schedule fcfs --sample --job 1,0,1,1
run "logger: capacity 0"      2 -- "$BIN" logger --capacity 0;      has_err "out of range"
run "logger: producers 99"    2 -- "$BIN" logger --producers 99
run "logger: bad option"      2 -- "$BIN" logger --nope
run "ipc: timeout 0"          2 -- "$BIN" ipc --timeout 0
run "ipc: bad option"         2 -- "$BIN" ipc --nope
run "banker: bad demo name"   2 -- "$BIN" banker --demo maybe
run "banker: demo + file"     2 -- "$BIN" banker --demo safe --file $DATA/banker_safe.txt
run "banker: missing file"    2 -- "$BIN" banker --file nope.txt
for f in invalid_alloc_gt_max invalid_max_gt_total invalid_negative malformed invalid_counts; do
    run "banker: reject $f"   2 -- "$BIN" banker --file $DATA/banker_$f.txt; has_err "error:"
done
run "worker: no scenario"     2 -- "$BIN" worker
run "worker: bad scenario"    2 -- "$BIN" worker explode
run "limits: bad limit"       2 -- "$BIN" limits memory
run "cleanup: extra argument" 2 -- "$BIN" cleanup now

echo "== FR1/FR2: scheduling (sample job set, hand-checked averages) =="
run "FCFS"      0 -- "$BIN" schedule fcfs;     has_out "Average turnaround time = 10.67"; has_out "Average waiting time    = 6.50"
run "RR q=3"    0 -- "$BIN" schedule rr --quantum 3; has_out "TIME QUANTUM = 3"; has_out "Average turnaround time = 11.67"; has_out "Average waiting time    = 7.50"
run "Priority"  0 -- "$BIN" schedule priority; has_out "Average turnaround time = 9.00"; has_out "Average waiting time    = 4.83"; has_out "Tie-break"
run "compare"   0 -- "$BIN" schedule compare; has_out "Original job data unchanged after all three runs: YES"; has_out "Lowest average waiting"
run "job file"  0 -- "$BIN" schedule compare --file $DATA/jobs_ok.txt
run "--job list" 0 -- "$BIN" schedule rr --quantum 2 --job 1,0,5,1 --job 2,1,3,2 --job 3,1,3,2
if command -v python3 >/dev/null 2>&1; then
    if python3 tests/ref_sched.py "$BIN" >"$OUT" 2>&1; then ok "independent Python reference agrees: $(tail -1 "$OUT")"
    else bad "independent Python reference MISMATCH" "$(tail -3 "$OUT")"; fi
else
    echo "  SKIP  python3 not installed: independent scheduling reference not run"
fi

echo "== FR3: producer-consumer logger =="
LOGF="$(mktemp /tmp/cc_test_log.XXXXXX)"
run "logger default" 0 -- "$BIN" logger --log "$LOGF"; has_out "FR3 RESULT: PASS"; has_out "queue-full condition OBSERVED"
[ -s "$LOGF" ] && ok "log file written and non-empty" || bad "log file empty"
run "logger capacity 1, 8 producers" 0 -- "$BIN" logger --capacity 1 --producers 8 --messages 50 --producer-delay 0 --consumer-delay 0 --log "$LOGF" --quiet; has_out "FR3 RESULT: PASS"
run "logger single producer" 0 -- "$BIN" logger --producers 1 --messages 1 --capacity 4 --log "$LOGF" --quiet
rm -f "$LOGF"

echo "== FR4: shared memory + semaphores =="
run "ipc normal" 0 -- "$BIN" ipc;  has_out "checksum OK"; has_out "objects are gone"
leftover_check "after ipc"
run "ipc simulated consumer crash (must be detected, no hang)" 1 -- "$BIN" ipc --simulate-consumer-crash --timeout 2; has_out "SIMULATED FAILURE"; has_out "objects are gone"
leftover_check "after ipc failure path"

echo "== FR5: Banker's algorithm =="
run "banker safe file"   0 -- "$BIN" banker --file $DATA/banker_safe.txt; has_out "RESULT: SAFE"; has_out "re-verification of the sequence: PASSED"
cp "$OUT" "$BANKER_OUT"
if command -v python3 >/dev/null 2>&1; then
    if python3 - "$DATA/banker_safe.txt" "$BANKER_OUT" <<'PY'
import re, sys
toks = [t for l in open(sys.argv[1]) for t in l.split('#')[0].split()]
n, m = int(toks[1]), int(toks[3]); i = toks.index('available') + 1
avail = list(map(int, toks[i:i+m])); i = toks.index('max') + 1
mx = [list(map(int, toks[i+r*m:i+r*m+m])) for r in range(n)]
i = toks.index('allocation') + 1
al = [list(map(int, toks[i+r*m:i+r*m+m])) for r in range(n)]
seq = [int(x) for x in re.search(r'Safe sequence: <(.*?)>', open(sys.argv[2]).read()).group(1).replace('P','').split(',')]
work = avail[:]
assert sorted(seq) == list(range(n)), "not a permutation"
for p in seq:
    need = [mx[p][j] - al[p][j] for j in range(m)]
    assert all(need[j] <= work[j] for j in range(m)), "P%d cannot run" % p
    work = [work[j] + al[p][j] for j in range(m)]
PY
    then ok "independent Python verification of the printed safe sequence"
    else bad "printed safe sequence FAILED independent verification"; fi
fi
run "banker unsafe file" 0 -- "$BIN" banker --file $DATA/banker_unsafe.txt; has_out "RESULT: UNSAFE"; has_out "No safe sequence exists"
run "banker demo safe"   0 -- "$BIN" banker --demo safe;   has_out "RESULT: SAFE"
run "banker demo unsafe" 0 -- "$BIN" banker --demo unsafe; has_out "RESULT: UNSAFE"
run "banker both demos"  0 -- "$BIN" banker

echo "== FR6: worker failure detection =="
run "worker normal"      0 -- "$BIN" worker normal;      has_out "exit status 0"; has_out "detection CORRECT"; has_out "ECHILD"
run "worker fail-exit"   0 -- "$BIN" worker fail-exit;   has_out "NON-ZERO status 3"; has_out "SIMULATED"; has_out "detection CORRECT"
run "worker fail-signal" 0 -- "$BIN" worker fail-signal; has_out "SIGNAL 15"; has_out "SIMULATED"; has_out "detection CORRECT"
run "worker pool"        0 -- "$BIN" worker pool;        has_out "3/3 workers reaped, 3/3 outcomes detected correctly"

echo "== FR7: resource limits and cleanup =="
run "limits fsize"  0 -- "$BIN" limits fsize;  has_out "write #3 FAILED: errno=27"; has_out "parent limit unchanged"
run "limits nofile" 0 -- "$BIN" limits nofile; has_out "EMFILE=yes"
run "limits cpu"    0 -- "$BIN" limits cpu;    has_out "SIGXCPU"
run "limits all"    0 -- "$BIN" limits all;    has_out "passed: 3/3"
[ -z "$(ls /tmp/campuscompute_fsize_* 2>/dev/null)" ] && ok "no temporary test files left in /tmp" || bad "temporary files left in /tmp"
run "cleanup command" 0 -- "$BIN" cleanup;    has_out "none"

echo "== Integration: full demonstration =="
run "demo" 0 -- "$BIN" demo; has_out "OVERALL: PASS"; has_out "14 of 14 stages passed"
[ -s logs/campuscompute.log ] && ok "logs/campuscompute.log written" || bad "demo log missing"
grep -q "overall result: PASS" logs/campuscompute.log && ok "log records the overall result" || bad "log lacks overall result"
leftover_check "after demo"
if pgrep -x "$(basename "$BIN")" >/dev/null 2>&1; then bad "campuscompute processes still running"; else ok "no campuscompute processes left running"; fi

echo
echo "integration tests: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
