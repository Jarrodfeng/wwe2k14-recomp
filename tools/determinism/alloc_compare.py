"""Compare two boots' allocation traces (Session 8, task A). Read-only; never launches the game.

    py tools/determinism/alloc_compare.py A B [--context N] [--exe-dir PRESET]

A and B are run numbers recorded with --wwe2k14_alloc_trace_frames=N (src/fp_probe.h), which writes
out/harness/determinism/alloc_NNN.tsv (frame, host thread, guest thread, allocator path 1/2, size, second argument,
caller return address) for every request to sub_82C2DCD8 / sub_82C2F5A8 during the first N guest frames.

Prints, per run, the boot heap pointers from the log's "[alloctrace] stopped" line (0x836F3AAC is the layout marker
of docs/systems/determinism.md), row counts per guest thread; then
  1. per guest thread: whether its own request sequence (path, size, arg, caller) is the same in both runs, and the
     first difference if not (a thread asking for something else = a content/timing input on that thread);
  2. the merged sequence (the order the requests reached the allocator): the first index where it differs and the
     rows around it (same per-thread sequences but a different merge = two threads racing for the same heap).
Guest thread IDs are not stable across boots (worker thread 25 in one boot can be 24 in the next), so threads are
paired before comparing: same ID and same sequence first, then any unpaired thread with an identical sequence under
another ID (printed as "A thread X = B thread Y"), then the rest by ID. The merged order is compared with B's IDs
mapped to A's, and the first difference ignoring thread IDs entirely is printed too.
Row order is the order requests entered the hook (the second instruction of the allocator entry point) under the
trace's own lock, before the allocator's critical section: a proxy for the allocation order, and the trace lock
itself serializes the race it measures.
Callers are guest return addresses; `py tools/q/q.py addr 0x...` names the calling function.
"""
import argparse
import os
import re
import sys
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DET = os.path.join(REPO, 'out', 'harness', 'determinism')
STOP_RE = re.compile(r'\[alloctrace\] stopped at frame (\d+) after (\d+) rows; boot heap pointers (.*)$')


def read(run, exe_dir):
    folder = os.path.basename(os.path.normpath(exe_dir)) if exe_dir else 'win-amd64-release'
    stem = 'alloc_%03d' % run if folder == 'win-amd64-release' else 'alloc_%s_%03d' % (folder, run)
    path = os.path.join(DET, stem + '.tsv')
    if not os.path.exists(path):
        sys.exit('alloc_compare: no trace for run %d: %s' % (run, path))
    rows = []
    with open(path, encoding='utf-8', errors='replace') as f:
        header = f.readline().rstrip('\n').split('\t')
        for line in f:
            parts = line.rstrip('\n').split('\t')
            if len(parts) == len(header):
                rows.append(dict(zip(header, parts)))
    log = os.path.join(REPO, 'out', 'build', folder, 'logs', 'wwe2k14_%03d.log' % run)
    stop = None
    if os.path.exists(log):
        with open(log, encoding='utf-8', errors='replace') as f:
            for line in f:
                m = STOP_RE.search(line)
                if m:
                    stop = m.group(3).strip()
    return rows, stop


def req(r):
    return (r['path'], r['size'], r['arg'], r['caller'])


def fmt(r):
    return 'frame %s gtid %s htid %s path %s size %s arg %s caller %s' % (
        r['frame'], r['gtid'], r['htid'], r['path'], r['size'], r['arg'], r['caller'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a', type=int)
    ap.add_argument('b', type=int)
    ap.add_argument('--context', type=int, default=4)
    ap.add_argument('--exe-dir', default=None)
    args = ap.parse_args()
    data = {}
    for run in (args.a, args.b):
        rows, stop = read(run, args.exe_dir)
        data[run] = rows
        per = Counter(r['gtid'] for r in rows)
        print('run %d: %d rows; %s; per guest thread: %s' % (run, len(rows), stop or 'no stop line',
                                                            dict(sorted(per.items(), key=lambda kv: int(kv[0])))))
    ra, rb = data[args.a], data[args.b]
    ta, tb = defaultdict(list), defaultdict(list)
    for r in ra:
        ta[r['gtid']].append(r)
    for r in rb:
        tb[r['gtid']].append(r)
    seq_a = {g: tuple(req(r) for r in rs) for g, rs in ta.items()}
    seq_b = {g: tuple(req(r) for r in rs) for g, rs in tb.items()}
    # Pair A threads with B threads: (1) same ID, same sequence; (2) same sequence under another ID; (3) same ID.
    pair = {}  # A gtid -> B gtid
    free_b = set(tb)
    for g in sorted(ta, key=int):
        if g in free_b and seq_a[g] == seq_b[g]:
            pair[g] = g
            free_b.discard(g)
    renamed = []
    for g in sorted(set(ta) - set(pair), key=int):
        match = next((h for h in sorted(free_b, key=int) if seq_b[h] == seq_a[g]), None)
        if match is not None:
            pair[g] = match
            free_b.discard(match)
            renamed.append((g, match))
    for g in sorted(set(ta) - set(pair), key=int):
        if g in free_b:
            pair[g] = g
            free_b.discard(g)
    unpaired_a = sorted(set(ta) - set(pair), key=int)
    unpaired_b = sorted(free_b, key=int)
    print()
    print('per guest thread (A ID; B ID when it differs):')
    for g in sorted(pair, key=int):
        h = pair[g]
        sa, sb = ta[g], tb[h]
        label = 'guest thread %s' % g if g == h else 'A thread %s = B thread %s' % (g, h)
        n = min(len(sa), len(sb))
        first = next((k for k in range(n) if req(sa[k]) != req(sb[k])), None)
        if first is None:
            note = 'same %d requests' % n if len(sa) == len(sb) else 'common %d same, then %d vs %d' % (
                n, len(sa), len(sb))
            print('  %s: %s' % (label, note))
        else:
            print('  %s: first difference at its request %d of %d / %d' % (label, first, len(sa), len(sb)))
            print('    A ' + fmt(sa[first]))
            print('    B ' + fmt(sb[first]))
    for g in unpaired_a:
        print('  guest thread %s: only in A (%d requests, first: %s)' % (g, len(ta[g]), fmt(ta[g][0])))
    for h in unpaired_b:
        print('  guest thread %s: only in B (%d requests, first: %s)' % (h, len(tb[h]), fmt(tb[h][0])))
    if renamed:
        print('  note: %d thread(s) did the same requests under a different guest thread ID; the per-ID view would '
              'report them as different' % len(renamed))
    to_a = {h: g for g, h in pair.items()}
    n = min(len(ra), len(rb))
    first = next((k for k in range(n) if (ra[k]['gtid'],) + req(ra[k]) !=
                  (to_a.get(rb[k]['gtid'], 'B' + rb[k]['gtid']),) + req(rb[k])), None)
    first_any = next((k for k in range(n) if req(ra[k]) != req(rb[k])), None)
    print()
    print('merged order (B thread IDs mapped to A):')
    if first is None:
        print('  identical for the common %d requests' % n)
    else:
        print('  first difference at request %d' % first)
        for k in range(max(0, first - args.context), min(n, first + args.context + 1)):
            mark = '>' if k == first else ' '
            print('  %s[%d] A %s' % (mark, k, fmt(ra[k])))
            print('  %s[%d] B %s' % (mark, k, fmt(rb[k])))
    print('  ignoring thread IDs: %s' % ('identical for the common %d requests' % n if first_any is None
                                         else 'first difference at request %d' % first_any))

if __name__ == '__main__':
    main()
