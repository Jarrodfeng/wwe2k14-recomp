"""Find guest words whose values follow a pattern across memory snapshots (Session 27 item 2, the menu map).

The snapshots are `--wwe2k14_snapshot_trigger` dumps of physical memory (logs/snap_RUN_NAME_H.bin = guest
0xA0000000..0xC0000000, big-endian). Local only: never commit them.

  py tools/menus/snap_find.py RUN NAME=VALUE [NAME=VALUE ...] [--width 1|2|4] [--offset-ok] [--same NAME,NAME]
                                  [--diff NAME,NAME] [--limit N]

  NAME=VALUE   the value the word must hold in snapshot NAME (decimal or 0x hex). With --offset-ok the values may all
               be shifted by the same constant (e.g. 1-based instead of 0-based).
  --same A,B   the word must also be equal in A and B (and every listed name) - e.g. unchanged screens
  --diff A,B   the word must differ between A and B

Prints guest addresses (0x...) and the values in every snapshot given.
"""
import argparse
import os
import sys

import numpy as np

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..'))
LOGS = os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs')
BASE = 0xA0000000
CHUNK = 16 << 20


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('pairs', nargs='+')
    ap.add_argument('--width', type=int, default=4, choices=(1, 2, 4))
    ap.add_argument('--offset-ok', action='store_true')
    ap.add_argument('--same', action='append', default=[])
    ap.add_argument('--diff', action='append', default=[])
    ap.add_argument('--limit', type=int, default=40)
    a = ap.parse_args()
    want = []
    for p in a.pairs:
        n, v = p.split('=')
        want.append((n, int(v, 0)))
    same = [s.split(',') for s in a.same]
    diff = [s.split(',') for s in a.diff]
    names = sorted({n for n, _ in want} | {n for g in same + diff for n in g})
    paths = {n: os.path.join(LOGS, f'snap_{a.run}_{n}_H.bin') for n in names}
    for n, p in paths.items():
        if not os.path.exists(p):
            sys.exit(f'missing {p}')
    dt = {1: '>u1', 2: '>u2', 4: '>u4'}[a.width]
    maps = {n: np.memmap(p, dtype=dt, mode='r') for n, p in paths.items()}
    total = len(next(iter(maps.values())))
    step = CHUNK // a.width
    hits = []
    for lo in range(0, total, step):
        hi = min(total, lo + step)
        blk = {n: np.asarray(m[lo:hi]).astype(np.int64) for n, m in maps.items()}
        n0, v0 = want[0]
        shift = blk[n0] - v0 if a.offset_ok else np.zeros(hi - lo, dtype=np.int64)
        ok = (blk[n0] == v0 + shift)
        for n, v in want[1:]:
            ok &= blk[n] == v + shift
        if a.offset_ok:
            ok &= np.abs(shift) <= 16
        for g in same:
            for n in g[1:]:
                ok &= blk[g[0]] == blk[n]
        for g in diff:
            ok &= blk[g[0]] != blk[g[1]]
        for i in np.nonzero(ok)[0]:
            hits.append(lo + int(i))
    print(f'{len(hits)} match(es), width {a.width}')
    for i in hits[:a.limit]:
        vals = ' '.join(f'{n}={int(maps[n][i])}' for n in names)
        print(f'  0x{BASE + i * a.width:08X}  {vals}')


if __name__ == '__main__':
    main()
