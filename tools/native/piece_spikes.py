r"""Which pieces spike (Session 82): over the frames of a pass-logged run (route s82_slow_frames.txt or s80_match_log.txt,
--vulkan_gpu_timestamps=true --gpu_native_gpu_timers=true), per piece (its place in the frame, label, size) the minimum,
median and maximum GPU ms (own time + the gap before it) and how many frames it is more than 3x its median. A slow frame
whose extra time lands in ONE piece every time is that piece's work; extra time that lands in a different piece from frame
to frame (the PC: a 0.003 ms clear pass taking 1.7 ms) is the desktop compositor or another GPU client taking the GPU
during our timestamps (Session 80: dwm ~3.3 ms chunks, `bench.ps1 -Front`).

    py -I tools\native\piece_spikes.py RUN [--dir LOGDIR] [--from FRAME] [--top N]
"""
import argparse
import os
import re
import statistics
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PASS = re.compile(r'\[native\]\[pass\] frame (\d+) #(\d+) kind (\d+) (\d+)x(\d+) .*?: ([0-9.]+) ms \(before it ([0-9.]+) ms\)'
                  r'(?: owned (\S+))?')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--dir', default=os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs'))
    ap.add_argument('--from', dest='first', type=int, default=0)
    ap.add_argument('--top', type=int, default=12)
    a = ap.parse_args()
    pieces = defaultdict(dict)
    with open(os.path.join(a.dir, f'wwe2k14_{int(a.run):03d}.log'), encoding='utf-8', errors='replace') as f:
        for line in f:
            m = PASS.search(line)
            if not m or int(m.group(1)) < a.first:
                continue
            key = (int(m.group(2)), m.group(8) or 'truth', f'{m.group(4)}x{m.group(5)}')
            pieces[key][int(m.group(1))] = float(m.group(6)) + float(m.group(7))
    rows = []
    for key, v in pieces.items():
        xs = list(v.values())
        if len(xs) < 5:
            continue
        med = statistics.median(xs)
        hit = sum(1 for x in xs if x > 3 * max(med, 0.002))
        rows.append((max(xs) / max(med, 0.002), key, min(xs), med, max(xs), hit, len(xs)))
    rows.sort(reverse=True)
    print('piece (slot, label, size)            min / median / max ms   frames > 3x median')
    for r in rows[:a.top]:
        print(f'  {r[1][0]:3d} {r[1][1]:10s} {r[1][2]:9s}  {r[2]:7.3f} {r[3]:7.3f} {r[4]:7.3f}   {r[5]} of {r[6]}')
    big = [r for r in rows if r[5] >= 1]
    print(f'{len(big)} of {len(rows)} pieces spiked at least once; spikes in many different pieces = the desktop / '
          f'another GPU client, in one piece every time = that piece')


if __name__ == '__main__':
    main()
