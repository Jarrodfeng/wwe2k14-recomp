"""Frame-rate check at any present rate (Session 19 item 10: 120 / 144 / 165 fps and slow motion).

    py tools/bench/rate_check.py RUN [--hz 120] [--step-hz 120]

Scenes are anchored on the file opens like fps60.py: Brock (343.bik2), the second entrance (324.bik2 / 103.bik2) and
the match (M3.PAC) to the end of the frame log. Per scene: frames, wall s, presents per second, game speed
(= presents/s / step_hz: 1.0 = real time, 0.5 = half-speed slow motion), frame-time p50 / p99 / worst against the
budget 1000 / hz ms, and how many frames took more than 1.5x / 2x the budget. With a fixed step per update a late
frame slows the game down instead of dropping a frame, so the worst frames decide whether a rate is usable.
Reads logs/wwe2k14_NNN.log and logs/frames_NNN.csv only (our own run output)."""
import argparse
import csv
import re

L = r'C:/Users/Jarrod/wwe2k14-recomp/out/build/win-amd64-release/logs'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('run')
    ap.add_argument('--hz', type=float, default=120.0, help='target present rate (budget = 1000 / hz ms)')
    ap.add_argument('--step-hz', type=float, default=0.0, help='the run\'s wwe2k14_step_hz (default: --hz)')
    a = ap.parse_args()
    n = a.run.zfill(3)
    step = a.step_hz or a.hz
    anch = {}
    for line in open(f'{L}/wwe2k14_{n}.log', encoding='utf-8', errors='replace'):
        m = re.search(r"\[file\] \S+ '([^']+)'.*frame=(\d+)", line)
        if m:
            p = m.group(1).lower()
            for k in ('343.bik2', '103.bik2', '324.bik2', 'm3.pac'):
                if k in p and k not in anch:
                    anch[k] = int(m.group(2))
    frames = []
    for r in csv.DictReader(open(f'{L}/frames_{n}.csv')):
        frames.append((int(r['frame']), float(r['t_seconds']), float(r['frame_ms'])))
    if not frames:
        print('no frames'); return
    last = frames[-1][0]
    second = anch.get('324.bik2') or anch.get('103.bik2')
    scenes = []
    if '343.bik2' in anch and second:
        scenes.append(('Brock', anch['343.bik2'], second))
    if second and 'm3.pac' in anch:
        scenes.append(('second', second, anch['m3.pac']))
        # Session 21: Bryan's wide ramp pan (the second titantron +380 .. +500 frames, as bench_report's tron_324_pan).
        scenes.append(('pan', second + 380, second + 500))
    if 'm3.pac' in anch:
        scenes.append(('match', anch['m3.pac'], last))
    budget = 1000.0 / a.hz
    print(f'run {n}: target {a.hz:g} fps (budget {budget:.2f} ms), step {step:g} Hz; anchors {anch}')
    print(f'{"scene":8} {"frames":>7} {"wall s":>7} {"fps":>7} {"speed":>6} {"p50":>6} {"p99":>6} {"worst":>6} '
          f'{">1.5x":>6} {">2x":>5}')
    for name, f0, f1 in scenes:
        sel = [f for f in frames if f0 < f[0] <= f1]
        if len(sel) < 2:
            continue
        wall = sel[-1][1] - sel[0][1]
        fps = (len(sel) - 1) / wall if wall > 0 else 0
        ms = sorted(f[2] for f in sel)
        p = lambda q: ms[min(len(ms) - 1, int(len(ms) * q))]
        over15 = sum(1 for v in ms if v > 1.5 * budget)
        over2 = sum(1 for v in ms if v > 2 * budget)
        print(f'{name:8} {len(sel):7d} {wall:7.1f} {fps:7.1f} {fps / step:6.2f} {p(0.5):6.2f} {p(0.99):6.2f} '
              f'{ms[-1]:6.1f} {over15:6d} {over2:5d}')


if __name__ == '__main__':
    main()
