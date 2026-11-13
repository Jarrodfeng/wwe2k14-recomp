"""Session 101 lane 166: shadow-map fetch scale arms. The skin shaders' slot-12 sample is multiplied by S (a scratch copy of the fx folder, fview --apply-reload),
luma of each wrestler's skin mask (as carry_arms2.py). Scratch fx folders: out\\s101\\carry\\fx32 / fx64 (copies of src\\renderer\\shaders\\fx with line 391 of fx_skin.glsl edited).
 py -I tools\\fview\\carry_arms3.py --fview EXE --capture DIR --out DIR --frames 488,480,113 --scales 32,64
"""
import argparse, json, pathlib, sys
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from carry_sheet import run, luma, MASKS, THRESH
ap = argparse.ArgumentParser(); ap.add_argument('--fview'); ap.add_argument('--capture'); ap.add_argument('--out'); ap.add_argument('--frames'); ap.add_argument('--scales')
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import fview_exe as fe   # lane 218: one locator, refuses a stale exe
fe.add_allow_stale(ap)
a = ap.parse_args(); a.fview = fe.find_or_exit(prefer=a.fview, allow_stale=a.allow_stale, kind='dev'); out = pathlib.Path(a.out); res = {}
for f in [int(x) for x in a.frames.split(',')]:
    imgs = {}
    imgs['A'], _ = run(a.fview, a.capture, f, [], out / 'arms3' / ('f%d_A' % f))
    imgs['B'], _ = run(a.fview, a.capture, f, ['--const-override', 'ps:170.w=1'], out / 'arms3' / ('f%d_B' % f))
    for s in a.scales.split(','):
        fx = str((out / ('fx' + s)).resolve())
        imgs['S' + s], _ = run(a.fview, a.capture, f, ['--apply-reload', 'yCh_skin,yCh_skin_flex', '--fx-dir', fx], out / 'arms3' / ('f%d_S%s' % (f, s)))
        imgs['S%sB' % s], _ = run(a.fview, a.capture, f, ['--apply-reload', 'yCh_skin,yCh_skin_flex', '--fx-dir', fx, '--const-override', 'ps:170.w=1'], out / 'arms3' / ('f%d_S%sB' % (f, s)))
    for k, e in MASKS.items(): imgs[k], _ = run(a.fview, a.capture, f, e, out / 'arms3' / ('f%d_%s' % (f, k)))
    r = {}
    for g, mk in ((0, 'M0'), (1, 'M1')):
        m = np.abs(imgs[mk] - imgs['A']).max(axis=2) > THRESH
        r[g] = {'px': int(m.sum()), **{k: round(float(luma(imgs[k])[m].mean()), 1) for k in imgs if not k.startswith('M')}}
    res[f] = r; print(f, json.dumps(r))
json.dump(res, open(out / 'arms3.json', 'w'), indent=1)
