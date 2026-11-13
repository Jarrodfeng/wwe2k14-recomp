"""Session 101 lane 166: shadow-term arms for the carry darkening. Same method as carry_sheet.py (fview headless, 0 game runs).
 A stream, B ps:170.w=1, E ps:168.w=0 (shadow-map strength 0: no map shadow at all), F ps:170.w=0 (pure near layer), G ps:170.w=0.5, H ps:170.w=0.25
 py -I tools\\fview\\carry_arms2.py --fview EXE --capture DIR --out DIR --frames 488,480
"""
import argparse, json, pathlib, sys
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from carry_sheet import run, luma, MASKS, THRESH
ARMS = {'A': [], 'B': ['--const-override', 'ps:170.w=1'], 'E': ['--const-override', 'ps:168.w=0'], 'F': ['--const-override', 'ps:170.w=0'],
        'G': ['--const-override', 'ps:170.w=0.5'], 'H': ['--const-override', 'ps:170.w=0.25']}
ap = argparse.ArgumentParser(); ap.add_argument('--fview'); ap.add_argument('--capture'); ap.add_argument('--out'); ap.add_argument('--frames')
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import fview_exe as fe   # lane 218: one locator, refuses a stale exe
fe.add_allow_stale(ap)
a = ap.parse_args(); a.fview = fe.find_or_exit(prefer=a.fview, allow_stale=a.allow_stale, kind='dev'); out = pathlib.Path(a.out); res = {}
for f in [int(x) for x in a.frames.split(',')]:
    imgs = {}
    for k, e in ARMS.items(): imgs[k], _ = run(a.fview, a.capture, f, e, out / 'arms2' / ('f%d_%s' % (f, k)))
    for k, e in MASKS.items(): imgs[k], _ = run(a.fview, a.capture, f, e, out / 'arms2' / ('f%d_%s' % (f, k)))
    r = {}
    for g, mk in ((0, 'M0'), (1, 'M1')):
        m = np.abs(imgs[mk] - imgs['A']).max(axis=2) > THRESH
        r[g] = {'px': int(m.sum()), **{k: round(float(luma(imgs[k])[m].mean()), 1) for k in ARMS}}
    res[f] = r; print(f, json.dumps(r))
json.dump(res, open(out / 'arms2.json', 'w'), indent=1)
