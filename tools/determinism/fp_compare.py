"""Compare two runs' distance-probe files (Session 8, task A). Read-only; never launches the game.

    py tools/determinism/fp_compare.py A B [--pair I-J] [--context N] [--exe-dir PRESET]

A and B are run numbers recorded with --wwe2k14_fp_probe=true (src/fp_probe.h), which writes
out/harness/determinism/fp_NNN.tsv: one row per pairwise character distance the game computes in sub_82666310
(frame, host/guest thread, slot pair i-j, the two object pointers, the four input floats ax az bx bz, the result,
two recomputes, MXCSR, the cached guest csr, flags).

Prints:
  1. Per run: rows, flag counts (A = inputs changed between the call and the hook, C = the result differs from a
     round-to-nearest recompute, S = cached csr disagrees with MXCSR, N = MXCSR not round-to-nearest / flush on),
     the host and guest threads that ran the computation (how many rows each).
  2. Per slot pair: the two runs' value sequences (consecutive repeats of the same inputs + result removed) compared
     element by element: the first element where they differ, with both rows, and which of inputs / result differ.
     When only the result differs (same inputs), the cause is the computation itself (environment); when the inputs
     differ, the cause is upstream (whoever writes the positions).
Game-derived values (positions, distances): the output stays on the console / under out/.
"""
import argparse
import os
import struct
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
DET = os.path.join(REPO, 'out', 'harness', 'determinism')


def fp_path(run, exe_dir):
    folder = os.path.basename(os.path.normpath(exe_dir)) if exe_dir else 'win-amd64-release'
    stem = 'fp_%03d' % run if folder == 'win-amd64-release' else 'fp_%s_%03d' % (folder, run)
    return os.path.join(DET, stem + '.tsv')


def read(run, exe_dir):
    path = fp_path(run, exe_dir)
    if not os.path.exists(path):
        sys.exit('fp_compare: no probe file for run %d: %s' % (run, path))
    rows = []
    with open(path, encoding='utf-8', errors='replace') as f:
        header = f.readline().rstrip('\n').split('\t')
        for line in f:
            parts = line.rstrip('\n').split('\t')
            if len(parts) != len(header):
                continue  # last line cut off when the process quit
            rows.append(dict(zip(header, parts)))
    return rows


def fl(h):
    return struct.unpack('>f', bytes.fromhex(h))[0]


def key(r):
    return (r['ax'], r['az'], r['bx'], r['bz'], r['result'])


def seq(rows, pair):
    out = []
    for r in rows:
        if '%s-%s' % (r['i'], r['j']) != pair:
            continue
        if out and key(out[-1]) == key(r):
            continue
        out.append(r)
    return out


def show(tag, r):
    return ('%s frame %s htid %s gtid %s a %s b %s inputs ax %s (%.7g) az %s (%.7g) bx %s (%.7g) bz %s (%.7g) '
            '-> %s (%.9g) mxcsr %s cached %s flags %s' % (
                tag, r['frame'], r['htid'], r['gtid'], r['a'], r['b'], r['ax'], fl(r['ax']), r['az'], fl(r['az']),
                r['bx'], fl(r['bx']), r['bz'], fl(r['bz']), r['result'], fl(r['result']), r['mxcsr'], r['cached'],
                r['flags']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a', type=int)
    ap.add_argument('b', type=int)
    ap.add_argument('--pair', default=None, help='only this slot pair, e.g. 0-1')
    ap.add_argument('--context', type=int, default=2, help='equal elements to show before the first difference')
    ap.add_argument('--exe-dir', default=None)
    args = ap.parse_args()
    runs = {args.a: read(args.a, args.exe_dir), args.b: read(args.b, args.exe_dir)}
    for run, rows in runs.items():
        flags = Counter()
        for r in rows:
            for c in r['flags']:
                if c != '-':
                    flags[c] += 1
        threads = Counter((r['htid'], r['gtid']) for r in rows)
        mx = Counter(r['mxcsr'] for r in rows)
        print('run %d: %d rows, frames %s-%s; flags %s; threads (host, guest): %s; mxcsr values: %s' % (
            run, len(rows), rows[0]['frame'] if rows else '-', rows[-1]['frame'] if rows else '-',
            dict(flags) or 'none', dict(threads.most_common(8)), dict(mx.most_common(6))))
    pairs = sorted({'%s-%s' % (r['i'], r['j']) for rows in runs.values() for r in rows})
    if args.pair:
        pairs = [args.pair]
    for pair in pairs:
        sa, sb = seq(runs[args.a], pair), seq(runs[args.b], pair)
        n = min(len(sa), len(sb))
        first = next((k for k in range(n) if key(sa[k]) != key(sb[k])), None)
        if first is None:
            print('pair %s: %d / %d distinct values, the common %d identical in order%s' % (
                pair, len(sa), len(sb), n, '' if len(sa) == len(sb) else ' (one run has more)'))
            continue
        ra, rb = sa[first], sb[first]
        same_inputs = key(ra)[:4] == key(rb)[:4]
        print('pair %s: %d / %d distinct values; first difference at element %d: %s' % (
            pair, len(sa), len(sb), first,
            'SAME inputs, different result (the computation differs)' if same_inputs else
            'inputs differ (%s)' % ', '.join(nm for nm, x, y in zip(('ax', 'az', 'bx', 'bz'), key(ra), key(rb))
                                             if x != y)))
        for k in range(max(0, first - args.context), min(n, first + 2)):
            print('  [%d] %s' % (k, show('A', sa[k])))
            print('  [%d] %s' % (k, show('B', sb[k])))


if __name__ == '__main__':
    main()
