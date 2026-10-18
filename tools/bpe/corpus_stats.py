"""Session 11: calls vs unique inputs of the game's BPE decoder (sub_82C41918), from the per-call CSVs that
--wwe2k14_bpe_corpus=true writes (out/harness/bpe/calls/calls_NNN.csv). Answers: is the same input decoded many
times (a cache would help), and how much of the decode time and of the worst thread-frames are repeats.

Usage: py tools/bpe/corpus_stats.py NNN [NNN ...] [--top 10]
Output is printed (game-derived numbers; paste summaries only, never the corpus itself).
"""
import argparse
import collections
import csv
import pathlib
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
CALLS = REPO / "out" / "harness" / "bpe" / "calls"


def load(run):
    path = CALLS / f"calls_{int(run):03d}.csv"
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def report(name, rows, top):
    n = len(rows)
    if not n:
        print(f"== {name}: no calls")
        return
    by_in = collections.defaultdict(list)
    for r in rows:
        by_in[r["in_hash"]].append(r)
    total_bytes = sum(int(r["expanded"]) for r in rows)
    total_us = sum(float(r["guest_us"]) for r in rows)
    rep_rows = [r for rs in by_in.values() for r in rs[1:]]
    rep_bytes = sum(int(r["expanded"]) for r in rep_rows)
    rep_us = sum(float(r["guest_us"]) for r in rep_rows)
    print(f"== {name}")
    print(f"calls={n} unique_inputs={len(by_in)} repeat_calls={len(rep_rows)} ({100.0 * len(rep_rows) / n:.1f}%)")
    print(f"expanded_MB={total_bytes / 1e6:.1f} repeat_MB={rep_bytes / 1e6:.1f} ({100.0 * rep_bytes / max(total_bytes, 1):.1f}%)")
    print(f"guest_ms={total_us / 1000:.1f} repeat_guest_ms={rep_us / 1000:.1f} ({100.0 * rep_us / max(total_us, 1):.1f}%)"
          " (timed around the guest body only; the recorder's own work is outside it)")
    conflicts = sum(1 for rs in by_in.values() if len({r["out_hash"] for r in rs}) > 1)
    print(f"inputs with more than one output: {conflicts}")
    most = sorted(by_in.items(), key=lambda kv: -sum(float(r["guest_us"]) for r in kv[1][1:]))[:top]
    print(f"top {top} repeated inputs by repeat time: count, expanded, repeat_ms, callers (lr2)")
    for h, rs in most:
        if len(rs) < 2:
            break
        callers = collections.Counter(r["lr2"] for r in rs).most_common(3)
        print(f"  {h} x{len(rs)} {rs[0]['expanded']} B {sum(float(r['guest_us']) for r in rs[1:]) / 1000:.2f} ms"
              f" {callers}")
    # Worst thread-frames: how much of each is a repeat of an input seen earlier in the run?
    seen = set()
    tf = collections.defaultdict(lambda: [0.0, 0.0, 0, 0])
    for r in rows:  # rows are in call order per thread; across threads close to it (one mutex)
        key = (int(r["frame"]), r["htid"])
        us = float(r["guest_us"])
        tf[key][0] += us
        tf[key][2] += 1
        if r["in_hash"] in seen:
            tf[key][1] += us
            tf[key][3] += 1
        seen.add(r["in_hash"])
    print(f"top {top} thread-frames by guest decode time: frame, tid, ms, calls, repeat_ms, repeat_calls")
    for (frame, tid), (us, rus, c, rc) in sorted(tf.items(), key=lambda kv: -kv[1][0])[:top]:
        print(f"  {frame} {tid} {us / 1000:.2f} ms {c} calls; repeats {rus / 1000:.2f} ms {rc} calls")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--top", type=int, default=10)
    a = ap.parse_args()
    allrows = []
    for run in a.runs:
        rows = load(run)
        report(f"run {run}", rows, a.top)
        allrows += rows
    if len(a.runs) > 1:
        by_in = {}
        for r in allrows:
            by_in.setdefault(r["in_hash"], r)
        print(f"== all runs: calls={len(allrows)} unique_inputs={len(by_in)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
