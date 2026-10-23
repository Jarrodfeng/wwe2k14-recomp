"""Compare what every resolve wrote in two runs (Session 40): the [resolve_stats] lines with "hash H, frame F"
(gpu_resolve_log with vulkan_readback_resolve full; route short\\s40_rename_hash.txt).

Resolves are matched by (frame, order within the frame, destination). Per kind of resolve (surface_info): how many
matched, and how many have the same hash. Numbers only.

    py tools\\native\\resolve_hash_compare.py RUN_A RUN_B
"""
import os
import re
import sys
from collections import defaultdict

LOGS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out', 'build', 'win-amd64-release', 'logs')
LINE = re.compile(r'\[resolve_stats\] resolve \d+ dest 0x([0-9A-F]+) (\d+) bytes: .*color_info 0x([0-9A-F]+) '
                  r'surface_info 0x([0-9A-F]+), hash ([0-9A-F]+), frame (\d+)')


def load(run):
    out = {}
    order = defaultdict(int)
    with open(os.path.join(LOGS, f'wwe2k14_{int(run):03d}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = LINE.search(line)
            if not m:
                continue
            frame = int(m.group(6))
            k = order[frame]
            order[frame] += 1
            out[(frame, k, m.group(1))] = (m.group(4), m.group(5), int(m.group(2)))
    return out


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    stats = defaultdict(lambda: [0, 0])
    for key, (surface, h, _) in a.items():
        if key in b:
            stats[surface][0] += 1
            stats[surface][1] += b[key][1] == h
    print(f'run {sys.argv[1]}: {len(a)} resolves, run {sys.argv[2]}: {len(b)}; matched by frame / order / dest: '
          f'{sum(s[0] for s in stats.values())}')
    print(f'{"surface_info":<12} {"matched":>8} {"same hash":>10}')
    for surface, (n, same) in sorted(stats.items(), key=lambda x: -x[1][0]):
        print(f'0x{surface:<10} {n:>8} {same:>10}  {100.0 * same / n:.1f}%')


if __name__ == '__main__':
    main()
