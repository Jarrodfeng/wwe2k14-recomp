r"""S102 lane L1b: the GPU cost of the compute dispatch against the cube bake, offline on the PC (a ranking, not a Surface number: 0 game runs, 0 Surface runs).
  py -I tools\l1\l1b_gpu.py [--root DIR] [--reps 3] [--frames 91] [--scales 1,3] [--cap bryan]
rplay --arms "off=6:0|on=6:1": frame f is drawn under arm f % 2 with the compute switch off / on (arm kind 6, src\renderer\player\rplay.cpp), GPU stamps on; each stamped frame's GPU ms is kept under its arm
(colour = the scene colour pass, where the skin / gear / trans pixel work lands; copies = the copies role; rest = everything else, which holds the pre-light role; total). The arm order is rotated
between repetitions and the 4090 is shared, so read it as a ranking: the saving is the pre-light and copies rows, the cost is the rest row (the pre-light role: the strip passes the dispatch replaces, and the dispatch itself, both named pre-light) and the copies row (the 30 copies). The colour row must not move (the readers are untouched).
`--scales 3` draws 3840x2160 to show that nothing the lever does scales with the picture."""
import argparse
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
BUNDLE = ['--shadows-ours', '--plane-shadow-ours', '--copy-batch', '1']   # (--shadow-casters-ours needs --game in rplay)
ARM_RE = re.compile(r'\[rplay\]\[arm\] (\S+): frames (\d+), mean/median ms: colour ([\d.]+)/([\d.]+) work ([\d.]+)/([\d.]+) total ([\d.]+)/([\d.]+) copies ([\d.]+)/([\d.]+) shadows ([\d.]+)/([\d.]+) rest ([\d.]+)/([\d.]+)')
DRAW_RE = re.compile(r'\[rplay\]\[draws\] ([\d.]+) draws (\d+) tris (\d+) px (\d+) vs (\d+) fs a frame: (.*)$')
COLS = ('colour', 'work', 'total', 'copies', 'shadows', 'rest')


def main_checkout(root):
    try:
        common = subprocess.run(['git', '-C', root, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return root


def run(rplay, cap, frames, spec, scale, extra, skip=6, timeout=1500):
    cmd = [rplay, '--capture', cap, '--frames', str(frames), '--gpu-stamps', '1', '--gpu-log', os.path.join(os.environ.get('TEMP', '.'), 'l1b_arms_%d.csv' % os.getpid()), '--arms', spec, '--arm-skip', str(skip), '--scale', str(scale), '--small-passes-ours'] + BUNDLE + extra
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, errors='replace')
    text = (p.stdout or '') + (p.stderr or '')
    arms = {}
    for m in ARM_RE.finditer(text):
        v = [float(x) for x in m.groups()[2:]]
        arms[m.group(1)] = {c: (v[2 * i], v[2 * i + 1]) for i, c in enumerate(COLS)}   # (mean, median)
    return p.returncode, arms, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--reps', type=int, default=3)
    ap.add_argument('--frames', type=int, default=91)
    ap.add_argument('--scales', default='1,3')
    ap.add_argument('--cap', default='bryan')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    rplay = os.path.join(root, 'out', 'renderer', 'dev', 'rplay.exe')
    cap = os.path.join(main_checkout(root), 'out', 'lane86', a.cap, 'capture')
    if not os.path.exists(rplay) or not os.path.isdir(cap):
        print('FAIL no rplay.exe or capture (%s, %s)' % (rplay, cap))
        return 1
    bad = 0
    for scale in [float(s) for s in a.scales.split(',')]:
        rows = {'off': [], 'on': []}
        for rep in range(a.reps):
            spec = 'off=6:0|on=6:1' if rep % 2 == 0 else 'on=6:1|off=6:0'
            rc, arms, text = run(rplay, cap, a.frames, spec, scale, [])
            if rc != 0 or set(arms) != {'off', 'on'}:
                print('FAIL scale %g rep %d: rplay exit %d, arms %s' % (scale, rep, rc, sorted(arms)))
                print(text[-600:])
                bad += 1
                continue
            for k in rows:
                rows[k].append(arms[k])
            d = {c: arms['on'][c][1] - arms['off'][c][1] for c in COLS}
            print('scale %g rep %d (%s first): median ms off | on | delta   ' % (scale, rep, spec.split('=')[0]) + '   '.join('%s %.3f | %.3f | %+.3f' % (c, arms['off'][c][1], arms['on'][c][1], d[c]) for c in ('colour', 'copies', 'rest', 'total')))
        if rows['off'] and rows['on']:
            med = lambda k, c: statistics.median(r[c][1] for r in rows[k])
            print('scale %g MEDIAN of %d reps: ' % (scale, len(rows['off'])) + '   '.join('%s %.3f -> %.3f (%+.3f)' % (c, med('off', c), med('on', c), med('on', c) - med('off', c)) for c in COLS))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
