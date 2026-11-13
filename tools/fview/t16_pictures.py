"""Session 101 lane 170: what the 16-bit target rule changes in the picture (CARRY_DARKENING.md section 12). fview headless, 0 game runs. For a capture and frames:
  A  the stream as captured          B  ps:170.w=1 (the far weight forced to 1)       E  ps:168.w=0 (no wrestler-map shadow at all)
  N  --draw-drop @class=shadow (no shadow pass drawn: no maps, no plane mask, no blobs)      M0 / M1 / MALL  the wrestler masks (carry_sheet.py)
Per wrestler (mask = pixels that change when its skin draws go): luma of A, B, E, and the map's own contribution at w = 1, (E - B) / E ("normal-distance wrestler-on-wrestler shadow").
Mat shadow: region = pixels outside every wrestler mask where the shadow passes change the picture (|A - N| > threshold); mean luma drop there, the drop's peak and the areas above
10 / 50 / 90 % of the peak (softness = A10 / A90: a hard edge is ~1, a wide blurred one is large), and the area in 1/1000 of the frame.
  py -I tools\\fview\\t16_pictures.py --fview EXE --capture DIR --out DIR --frames 488,113 [--tag before|after]
"""
import argparse, json, pathlib, sys
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from carry_sheet import run, luma, MASKS, THRESH

ARMS = {'A': [], 'B': ['--const-override', 'ps:170.w=1'], 'E': ['--const-override', 'ps:168.w=0'], 'N': ['--draw-drop', '@class=shadow']}
ap = argparse.ArgumentParser(); ap.add_argument('--fview'); ap.add_argument('--capture'); ap.add_argument('--out'); ap.add_argument('--frames'); ap.add_argument('--tag', default='')
ap.add_argument('--extra', default='', help='extra fview flags, space separated (e.g. --wrestlers-ours)')
import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import fview_exe as fe   # lane 218: one locator, refuses a stale exe
fe.add_allow_stale(ap)
a = ap.parse_args(); a.fview = fe.find_or_exit(prefer=a.fview, allow_stale=a.allow_stale, kind='dev'); out = pathlib.Path(a.out); extra = a.extra.split(); res = {}
for f in [int(x) for x in a.frames.split(',')]:
    imgs = {}
    for k, e in {**ARMS, **MASKS}.items(): imgs[k], _ = run(a.fview, a.capture, f, e + extra, out / 'pics' / ('f%d_%s_%s' % (f, a.tag, k)))
    r = {'wrestlers': {}}
    wr = np.zeros(imgs['A'].shape[:2], bool)
    for g, mk in ((0, 'M0'), (1, 'M1')):
        m = np.abs(imgs[mk] - imgs['A']).max(axis=2) > THRESH
        wr |= m
        la, lb, le = (float(luma(imgs[k])[m].mean()) for k in 'ABE')
        r['wrestlers'][g] = {'px': int(m.sum()), 'A': round(la, 2), 'B_w1': round(lb, 2), 'E_nomap': round(le, 2), 'map_darkening_at_w1_pct': round(100 * (le - lb) / le, 2) if le else 0.0,
                             'lowered_vs_w1_pct': round(100 * (lb - la) / lb, 2) if lb else 0.0}
    wr |= np.abs(imgs['MALL'] - imgs['A']).max(axis=2) > THRESH
    d = luma(imgs['N']) - luma(imgs['A'])            # >0: the shadow passes made it darker
    reg = (np.abs(imgs['A'] - imgs['N']).max(axis=2) > THRESH) & ~wr
    if reg.sum():
        dd = d[reg]; peak = float(np.percentile(dd, 99.5)) or 1e-6
        area = lambda t: int(((d > t * peak) & reg).sum())
        r['mat'] = {'px': int(reg.sum()), 'per_mille': round(1000 * reg.sum() / reg.size, 2), 'mean_drop': round(float(dd.mean()), 2), 'peak_drop': round(peak, 2),
                    'A10': area(0.1), 'A50': area(0.5), 'A90': area(0.9), 'softness_A10_over_A90': round(area(0.1) / max(area(0.9), 1), 2)}
    else:
        r['mat'] = {'px': 0}
    res[f] = r; print(a.tag, f, json.dumps(r))
json.dump(res, open(out / ('t16_%s.json' % a.tag), 'w'), indent=1)
