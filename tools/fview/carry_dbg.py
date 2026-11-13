"""Session 101 lane 210: reads the shadow-term internals (carry_dbg_fx.py's scratch fx folders) over each wrestler's skin mask.
Per frame: renders the stream (arm A) and the masks (draw-drop per group), then each debug variant with --apply-reload yCh_skin,yCh_skin_flex --fx-dir;
prints mean / median / 10th / 90th percentile of the three output channels (0..255) inside each wrestler's mask.
  py -I tools\\fview\\carry_dbg.py --fview EXE --capture DIR/capture --fxroot DIR --out DIR --frames 824 --variants 1,2,3
"""
import argparse, json, pathlib, sys
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from carry_sheet import run, MASKS, THRESH
from carry_arms4 import BASE
ap = argparse.ArgumentParser(); ap.add_argument('--fview'); ap.add_argument('--capture'); ap.add_argument('--fxroot'); ap.add_argument('--out'); ap.add_argument('--frames')
ap.add_argument('--variants', default='1,2,3'); ap.add_argument('--const', action='append', default=[], help='extra const-override spec applied to every debug arm')
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import fview_exe as fe   # lane 218: one locator, refuses a stale exe
fe.add_allow_stale(ap)
a = ap.parse_args(); a.fview = fe.find_or_exit(prefer=a.fview, allow_stale=a.allow_stale, kind='dev'); out = pathlib.Path(a.out); res = {}
for f in [int(x) for x in a.frames.split(',')]:
    base, _ = run(a.fview, a.capture, f, BASE, out / 'dbg' / ('f%d_A' % f))
    ms = {k: run(a.fview, a.capture, f, BASE + e, out / 'dbg' / ('f%d_%s' % (f, k)))[0] for k, e in MASKS.items() if k != 'MALL'}
    masks = {g: np.abs(ms[mk] - base).max(axis=2) > THRESH for g, mk in ((0, 'M0'), (1, 'M1'))}
    r = {}
    for v in a.variants.split(','):
        extra = ['--apply-reload', 'yCh_skin,yCh_skin_flex', '--fx-dir', str((pathlib.Path(a.fxroot) / ('fxd' + v)).resolve())]
        for c in a.const: extra += ['--const-override', c]
        img, _ = run(a.fview, a.capture, f, BASE + extra, out / 'dbg' / ('f%d_v%s' % (f, v)))
        for g, m in masks.items():
            px = img[m]
            r['v%s_g%d' % (v, g)] = {'n': int(m.sum()), 'mean': [round(float(x), 1) for x in px.mean(axis=0)], 'p10': [round(float(x), 1) for x in np.percentile(px, 10, axis=0)],
                                    'p50': [round(float(x), 1) for x in np.percentile(px, 50, axis=0)], 'p90': [round(float(x), 1) for x in np.percentile(px, 90, axis=0)]}
        print(f, 'v' + v, json.dumps({k: r[k] for k in r if k.startswith('v%s_' % v)}), flush=True)
    res[f] = r
json.dump(res, open(out / ('dbg_%s.json' % a.frames.replace(',', '_')), 'w'), indent=1)
