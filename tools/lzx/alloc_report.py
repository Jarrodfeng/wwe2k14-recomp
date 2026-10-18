"""Session 15 (A3): the loader's pool-setup jobs and ReXGlue's failed-allocation log lines, per run.

For each run log (needs --wwe2k14_load_probe=true for the job lines):
  jobs     "[wwe2k14][load] job ... fn=0x82521770 ... dur_us=N": the pool-setup jobs (count, ms each, total)
  heap     "BaseHeap::Alloc ..." / "PhysicalHeap::Alloc ..." error lines (what wwe2k14_phys_alloc_fast_fail removes)
  mismatch "[mem] fast_fail MISMATCH" lines (--wwe2k14_phys_alloc_fast_fail_verify; expected 0)
  fast     whether the run had --wwe2k14_phys_alloc_fast_fail on (from the "modified:" line)
With --by-window it also splits the jobs by load-turbo window (ingame = pre-entrance load, match = match load).

Usage: py tools\\lzx\\alloc_report.py 1125 1126 ... [--by-window]
"""
import re
import statistics
import sys
from pathlib import Path

LOGS = Path(__file__).resolve().parents[2] / "out" / "build" / "win-amd64-release" / "logs"
JOB = re.compile(r"\[wwe2k14\]\[load\] job .*? frame=(\d+) .*?fn=0x82521770 .*?dur_us=(\d+)")
ON = re.compile(r"\[wwe2k14\]\[turbo\] (\w+) on at frame (\d+)")
OFF = re.compile(r"\[wwe2k14\]\[turbo\] (\w+) off at frame (\d+)")


def scan(run):
    jobs, heap, mismatch, fast, windows, cur = [], 0, 0, False, [], None
    with open(LOGS / f"wwe2k14_{run:03d}.log", encoding="utf-8", errors="replace") as f:
        for line in f:
            if "Heap::Alloc" in line:
                heap += 1
            elif "fast_fail MISMATCH" in line:
                mismatch += 1
            elif "modified: wwe2k14_phys_alloc_fast_fail = 'true'" in line:
                fast = True
            m = JOB.search(line)
            if m:
                jobs.append((int(m.group(1)), int(m.group(2)) / 1000.0))
            m = ON.search(line)
            if m:
                cur = (m.group(1), int(m.group(2)))
            m = OFF.search(line)
            if m and cur:
                windows.append((cur[0], cur[1], int(m.group(2))))
                cur = None
    return jobs, heap, mismatch, fast, windows


def main():
    by_window = "--by-window" in sys.argv
    for arg in [a for a in sys.argv[1:] if not a.startswith("--")]:
        run = int(arg)
        jobs, heap, mismatch, fast, windows = scan(run)
        ms = [d for _, d in jobs]
        print(f"run {run}: fast_fail {'ON ' if fast else 'off'}  heap error lines {heap:5d}  mismatches {mismatch}  "
              f"jobs {len(ms)} total {sum(ms):7.1f} ms  median {statistics.median(ms) if ms else 0:6.1f}  "
              f"each {' '.join(f'{d:.1f}' for d in ms)}")
        if by_window:
            for name, f0, f1 in windows:
                if name in ("ingame", "match"):
                    w = [d for fr, d in jobs if f0 <= fr <= f1]
                    print(f"    {name:6s} frames {f0}-{f1}: {len(w)} jobs, {sum(w):.1f} ms")


if __name__ == "__main__":
    main()
