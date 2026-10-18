"""Reverse pointer scan in a --wwe2k14_snapshot_trigger snapshot (Session 27 item 2): chains from a static address to
a heap word, in the states.toml chain form `0xSTATIC>OFF>OFF...`.

  py tools/menus/pointer_scan.py RUN NAME 0xTARGET [--depth 4] [--max-off 0x1000] [--check NAME,NAME ...]

Each hop: a word whose value V lies in [A - max_off, A] (A = the address reached so far) gives OFF = A - V. A chain ends
at a word in the static data (0x835C0000..0x83A60000). --check re-reads each chain in other snapshots of the same run
and prints the value it reaches there (a good chain gives the expected cursor values everywhere). Local only.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
from snap_read import D_HI, D_LO, H_HI, H_LO, Snap, chain  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('name')
    ap.add_argument('target')
    ap.add_argument('--depth', type=int, default=4)
    ap.add_argument('--max-off', default='0x1000')
    ap.add_argument('--check', default='')
    ap.add_argument('--limit', type=int, default=40)
    a = ap.parse_args()
    target = int(a.target, 0)
    max_off = int(a.max_off, 0)
    s = Snap(a.run, a.name)
    # (value, address) sorted by value, heap and static together
    hw = np.frombuffer(s.h[:len(s.h) // 4 * 4], dtype='>u4').astype(np.uint32)
    dw = np.frombuffer(s.d[:len(s.d) // 4 * 4], dtype='>u4').astype(np.uint32)
    vals = np.concatenate([hw, dw])
    addrs = np.concatenate([H_LO + 4 * np.arange(len(hw), dtype=np.uint64),
                            D_LO + 4 * np.arange(len(dw), dtype=np.uint64)])
    keep = (vals >= H_LO) & (vals < H_HI)  # only heap pointers lead to heap objects
    vals, addrs = vals[keep], addrs[keep]
    order = np.argsort(vals, kind='stable')
    vals, addrs = vals[order], addrs[order]
    print(f'{len(vals)} heap pointers in the snapshot', file=sys.stderr)

    def holders(addr):
        lo = np.searchsorted(vals, max(H_LO, addr - max_off), 'left')
        hi = np.searchsorted(vals, addr, 'right')
        return [(int(addrs[i]), addr - int(vals[i])) for i in range(lo, hi)]

    results = []
    frontier = [(target, [])]
    seen = set()
    for depth in range(a.depth):
        nxt = []
        for addr, offs in frontier:
            for h, off in holders(addr):
                if (h, depth) in seen:
                    continue
                seen.add((h, depth))
                path = [off] + offs
                if D_LO <= h < D_HI:
                    results.append((h, path))
                elif len(nxt) < 20000:
                    nxt.append((h, path))
        frontier = nxt
        print(f'depth {depth + 1}: {len(results)} static chain(s), {len(frontier)} open', file=sys.stderr)
    results.sort(key=lambda r: (len(r[1]), sum(r[1])))
    checks = [Snap(a.run, n) for n in a.check.split(',') if n] if a.check else []
    for h, path in results[:a.limit]:
        text = f'0x{h:08X}' + ''.join(f'>{o}' for o in path)
        line = text
        for n, cs in zip(a.check.split(','), checks):
            r = chain(cs, text)
            line += f'  {n}=' + ('?' if r is None or r[0] is None else str(r[0]))
        print(line)


if __name__ == '__main__':
    main()
