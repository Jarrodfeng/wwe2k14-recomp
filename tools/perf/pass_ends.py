"""Session 56: who ends the truth passes. Reads a run's "[native][seg] frame F truth pass end: base B stack ..." lines
(SDK, with gpu_native_segment_log on) and counts the call stacks, symbolized against the staged rexgpu-xenos.dll / .pdb
like tools\\perf\\barmap.py.

    py tools\\perf\\pass_ends.py RUN [--frame F] [--depth 5]

Numbers only; our own logs.
"""
import argparse
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import barmap  # noqa: E402

LINE = re.compile(r'\[native\]\[seg\] frame (\d+) truth pass end: base ([0-9A-F]+) stack ((?:[0-9A-F]+ ?){1,8})')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run', type=int)
    ap.add_argument('--frame', type=int)
    ap.add_argument('--depth', type=int, default=5)
    a = ap.parse_args()
    log = os.path.join(barmap.EXE_DIR, 'logs', f'wwe2k14_{a.run:03d}.log')
    stacks = collections.Counter()
    frames = set()
    base = 0
    for line in open(log, encoding='utf-8', errors='replace'):
        m = LINE.search(line)
        if not m or (a.frame is not None and int(m.group(1)) != a.frame):
            continue
        frames.add(int(m.group(1)))
        base = int(m.group(2), 16)
        stacks[tuple(int(x, 16) for x in m.group(3).split()[:a.depth])] += 1
    if not stacks:
        sys.exit('no pass-end lines')
    addrs = sorted({x for s in stacks for x in s})
    names = barmap.symbolize(addrs, base, os.path.join(barmap.EXE_DIR, 'rexgpu-xenos.dll'))
    print(f'run {a.run}: {sum(stacks.values())} pass ends in {len(frames)} frame(s) {sorted(frames)}')
    for s, n in stacks.most_common():
        print(f'{n / len(frames):6.1f}/frame  ' + '  <  '.join(names.get(x, hex(x)).split(' ')[0] for x in s))


if __name__ == '__main__':
    main()
