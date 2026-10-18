"""Dropped frames per arm of an in-run A/B (Session 8 task B: --wwe2k14_precise_sleep).

Usage: py tools/analysis/ab_drops.py RUN [RUN...] --cvar NAME [--ms 20] [--logs DIR]

For each run: the arm of every guest frame from the timeline's 'cvar' events (a = fnv1a32(NAME), b = value,
c = guest frame; the value holds from that frame on; any non-zero value = arm B), the scene anchors as in
tools/bench/fps60.py, then per scene and arm: frames, frames > 20 / 25 / 35 ms, and the frames over --ms with their
ms, seconds after the scene anchor and arm. With logs/barrier_NNN.csv (--wwe2k14_barrier_probe=true) also the
update-thread work of each listed frame: the slot whose barrier call returns into --update-fn (default sub_82260588 =
db/frame_pacing.json game_main_thread_loop; the function of each slot's return address comes from tools/q `addr`),
wake -> arrival from the row of frame-1; "did not run" when that slot has no wake/arrival in that row (-1).
Frames before the first flip are arm '-' (not counted). Our own run output only.
"""
import argparse
import csv
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
LOGS = os.path.join(ROOT, 'out', 'build', 'win-amd64-release', 'logs')
KEYS = ('343.bik2', '324.bik2', '103.bik2', 'm3.pac')
_FN = {}


def func_of(lr_hex):
    """Guest function containing this return address, from tools/q (cached per address)."""
    if lr_hex not in _FN:
        try:
            out = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'q', 'q.py'), 'addr', f'0x{lr_hex}'],
                                 capture_output=True, text=True, timeout=60).stdout
            m = re.search(r'function:\s*(\S+)', out)
            _FN[lr_hex] = m.group(1).upper().replace('SUB_', 'sub_') if m else '?'
        except Exception:
            _FN[lr_hex] = '?'
    return _FN[lr_hex]


def fnv1a32(s):
    h = 0x811C9DC5
    for ch in s.encode():
        h = ((h ^ ch) * 0x01000193) & 0xFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+')
    ap.add_argument('--cvar', required=True)
    ap.add_argument('--ms', type=float, default=20.0)
    ap.add_argument('--logs', default=LOGS)
    ap.add_argument('--update-fn', default='sub_82260588',
                    help='guest function of the update thread\'s barrier call (default: game_main_thread_loop)')
    a = ap.parse_args()
    want_fn = a.update_fn.upper().replace('SUB_', 'sub_')
    h = fnv1a32(a.cvar)
    total = {}
    for run in a.runs:
        n = re.sub(r'\D', '', os.path.basename(run)).zfill(3)
        flips = []
        with open(os.path.join(a.logs, f'timeline_{n}.csv'), newline='') as f:
            for r in csv.DictReader(f):
                if r.get('event') == 'cvar':
                    try:
                        if int(r['a'], 16) == h:
                            flips.append((int(r['c'], 16), int(r['b'], 16) != 0))
                    except (ValueError, TypeError):
                        pass
        flips.sort()
        anch = {}
        with open(os.path.join(a.logs, f'wwe2k14_{n}.log'), encoding='utf-8', errors='replace') as f:
            for line in f:
                m = re.search(r"\[file\] \S+ '([^']+)'.*frame=(\d+)", line)
                if m:
                    p = m.group(1).lower()
                    for k in KEYS:
                        if k in p and k not in anch:
                            anch[k] = int(m.group(2))
        order = sorted(anch.items(), key=lambda kv: kv[1])
        frames = []
        with open(os.path.join(a.logs, f'frames_{n}.csv'), newline='') as f:
            for r in csv.DictReader(f):
                frames.append((int(r['frame']), float(r['t_seconds']), float(r['frame_ms'])))
        t_of = {fr: t for fr, t, _ in frames}
        work = {}  # frame -> update-thread work in ms, or None = that slot did not run in the frame's row
        bpath = os.path.join(a.logs, f'barrier_{n}.csv')
        if os.path.exists(bpath):
            with open(bpath) as f:
                next(f, None)
                for line in f:
                    p = line.strip().split(',')
                    try:
                        for s in p[5:]:
                            lr, tid, wk, ar = s.split(';')
                            if func_of(lr.upper()) == want_fn:
                                wk, ar = float(wk), float(ar)
                                work[int(p[0]) + 1] = (ar - wk) if wk >= 0 and ar >= 0 else None
                    except ValueError:
                        pass  # header-only file or a truncated last line
            if not work:
                print(f'run {n}: barrier_{n}.csv has no slot in {want_fn} (update-thread work not shown)')

        def arm(fr):
            v = None
            for f0, on in flips:
                if f0 <= fr:
                    v = on
                else:
                    break
            return '-' if v is None else ('B' if v else 'A')

        def scene(fr):
            s, t0 = 'pre', None
            for k, v in order:
                if v <= fr:
                    s, t0 = k, t_of.get(v)
            return s, t0

        print(f'run {n}: {len(flips)} flips of {a.cvar} (first at frame {flips[0][0] if flips else "-"}); anchors '
              + ', '.join(f'{k}@{v}' for k, v in order))
        for fr, t, ms in frames:
            if ms >= 400:
                continue
            ar = arm(fr)
            sc, t0 = scene(fr)
            key = (sc, ar)
            c = total.setdefault(key, [0, 0, 0, 0])
            c[0] += 1
            c[1] += ms > 20
            c[2] += ms > 25
            c[3] += ms > 35
            if ms > a.ms and ar != '-':
                dt = f'+{t - t0:7.2f}s' if t0 is not None else '        -'
                if fr not in work:
                    wk = ''
                elif work[fr] is None:
                    wk = '  update thread did not run in this row'
                else:
                    wk = f'  update-thread work {work[fr]:6.2f} ms'
                print(f'  frame {fr:6d} {ms:6.1f} ms  {sc:9s} {dt}  arm {ar}{wk}')
    print('\n# all runs together: scene, arm: frames, > 20 / > 25 / > 35 ms')
    for (sc, ar), c in sorted(total.items(), key=lambda kv: (kv[0][0], kv[0][1])):
        if ar == '-':
            continue
        print(f'  {sc:9s} {ar}: {c[0]:6d} frames, > 20 ms {c[1]:3d}, > 25 ms {c[2]:3d}, > 35 ms {c[3]:3d}')


if __name__ == '__main__':
    main()
