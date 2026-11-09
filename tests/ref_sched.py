#!/usr/bin/env python3
"""Independent reference checker for CampusCompute scheduling (FR2).

Re-implements FCFS, Round Robin and non-preemptive Priority from the written
rules only (no C code shared), runs ./campuscompute on the same job sets and
compares completion / turnaround / waiting per job plus both averages.

Rules (as documented by the program):
  FCFS     arrival order, tie -> lower id
  RR       FIFO queue; jobs arriving at or before a slice end are queued
           BEFORE the pre-empted job; simultaneous arrivals by lower id
  PRIORITY non-preemptive, lower number = higher priority,
           tie -> earlier arrival -> lower id
Usage: ref_sched.py [path-to-binary]   (exit 0 = all cases agree)
"""
import random
import re
import subprocess
import sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "./campuscompute"


def fcfs(jobs):
    t, done = 0, {}
    for j in sorted(jobs, key=lambda j: (j[1], j[0])):
        t = max(t, j[1]) + j[2]
        done[j[0]] = t
    return done


def priority(jobs):
    t, done, left = 0, {}, list(jobs)
    while left:
        ready = [j for j in left if j[1] <= t]
        if not ready:
            t = min(j[1] for j in left)
            continue
        j = min(ready, key=lambda j: (j[3], j[1], j[0]))
        t += j[2]
        done[j[0]] = t
        left.remove(j)
    return done


def rr(jobs, q):
    jobs = sorted(jobs, key=lambda j: (j[1], j[0]))
    rem = {j[0]: j[2] for j in jobs}
    arr = {j[0]: j[1] for j in jobs}
    pending = [j[0] for j in jobs]          # not yet queued, by arrival, id
    queue, done, t = [], {}, 0

    def admit(upto):
        while pending and arr[pending[0]] <= upto:
            queue.append(pending.pop(0))

    admit(0)
    while len(done) < len(jobs):
        if not queue:
            t = arr[pending[0]]
            admit(t)
            continue
        jid = queue.pop(0)
        run = min(q, rem[jid])
        t += run
        rem[jid] -= run
        admit(t)                            # arrivals first ...
        if rem[jid] == 0:
            done[jid] = t
        else:
            queue.append(jid)               # ... then the pre-empted job
    return done


def metrics(jobs, done):
    rows = {}
    for jid, arrival, burst, _ in jobs:
        tat = done[jid] - arrival
        rows[jid] = (done[jid], tat, tat - burst)
    n = len(jobs)
    return (rows, sum(r[1] for r in rows.values()) / n,
            sum(r[2] for r in rows.values()) / n)


ROW = re.compile(r"^\s+J(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)"
                 r"\s+(\d+)\s+COMPLETED")


def run_c(policy, jobs, q):
    cmd = [BIN, "schedule", policy]
    for j in jobs:
        cmd += ["--job", "%d,%d,%d,%d" % j]
    if policy == "rr":
        cmd += ["--quantum", str(q)]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    rows, avgs = {}, {}
    section = out.split("Gantt chart", 1)[-1]
    for line in section.splitlines():
        m = ROW.match(line)
        if m:
            g = [int(x) for x in m.groups()]
            rows[g[0]] = (g[5], g[6], g[7])   # completion, turnaround, waiting
    m1 = re.search(r"Average turnaround time\s*=\s*([\d.]+)", out)
    m2 = re.search(r"Average waiting time\s*=\s*([\d.]+)", out)
    if m1 and m2:
        avgs = (float(m1.group(1)), float(m2.group(1)))
    return rows, avgs


def check(name, jobs, q=3):
    bad = 0
    for policy in ("fcfs", "rr", "priority"):
        ref = {"fcfs": fcfs, "priority": priority}.get(policy)
        done = rr(jobs, q) if policy == "rr" else ref(jobs)
        rrows, ravg_t, ravg_w = metrics(jobs, done)
        crows, cavg = run_c(policy, jobs, q)
        ok = crows == rrows and cavg and \
            abs(cavg[0] - ravg_t) < 0.006 and abs(cavg[1] - ravg_w) < 0.006
        if not ok:
            bad += 1
            print("MISMATCH %s/%s q=%d jobs=%s\n  ref=%s avg=(%.2f,%.2f)\n"
                  "  c  =%s avg=%s" % (name, policy, q, jobs, rrows, ravg_t,
                                       ravg_w, crows, cavg))
    return bad


FIXED = {
    "sample": [(1, 0, 5, 2), (2, 1, 3, 1), (3, 2, 8, 3), (4, 2, 2, 1),
               (5, 3, 4, 2), (6, 25, 3, 1)],
    "same-arrival": [(1, 0, 4, 2), (2, 0, 3, 2), (3, 0, 5, 1)],
    "equal-priority": [(1, 0, 4, 1), (2, 1, 4, 1), (3, 2, 4, 1)],
    "idle-start": [(1, 5, 3, 1), (2, 6, 2, 2)],
    "single": [(7, 0, 1, 1)],
    "textbook": [(1, 0, 24, 1), (2, 0, 3, 1), (3, 0, 3, 1)],
}


def main():
    bad = total = 0
    for name, jobs in FIXED.items():
        for q in (1, 2, 3, 4, 7):
            bad += check(name, jobs, q)
            total += 1
    rnd = random.Random(2026)
    for case in range(150):
        n = rnd.randint(1, 8)
        ids = rnd.sample(range(1, 50), n)
        jobs = [(i, rnd.randint(0, 12), rnd.randint(1, 9), rnd.randint(1, 4))
                for i in ids]
        bad += check("random%d" % case, jobs, rnd.randint(1, 5))
        total += 1
    print("reference check: %d job sets x 3 policies, %d mismatch(es)"
          % (total, bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
