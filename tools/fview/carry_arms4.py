"""Session 101 lane 210: which input makes the w = 0 layer of the wrestler shadow term dark in the carry (fview headless, 0 game runs).
Same method as carry_sheet.py / carry_arms2.py, with the thin match promotions on every arm (a thin capture draws almost no wrestler without them).
Arms: A stream; B ps:170.w=1; E ps:168.w=0 (no map shadow); F ps:170.w=0; C vs:170-171.w=0 (sphere strength 0); D vs:168-169.w=0 (sphere radius 0);
G = F + C; H = F + E; I = A + C (sphere off at the game's own w); K = ps:170.w=0.5.
Luma = mean over each wrestler's skin mask (the pixels that change when its group's draws are dropped, > THRESH levels). Group 0 = the carried one in the K2 capture.
  py -I tools\\fview\\carry_arms4.py --fview EXE --capture DIR/capture --out DIR --frames 724,764,824
"""
import argparse, json, pathlib, sys
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from carry_sheet import run, luma, MASKS, THRESH

import os
ROOT = os.environ.get('WWE_GAME_ROOT', str(pathlib.Path(__file__).resolve().parents[2]))  # the checkout that holds game_data (set WWE_GAME_ROOT when this is a worktree without it)
BASE = ['--root', ROOT, '--thin', 'arena,ring,wrestlers', '--crowd-ours', '--arena-ours', '--wrestlers-ours', '--ring-ours', '--flash-ours', '--props-ours', '--post-ours']
CO = '--const-override'
ARMS = {'A': [], 'B': [CO, 'ps:170.w=1'], 'E': [CO, 'ps:168.w=0'], 'F': [CO, 'ps:170.w=0'], 'C': [CO, 'vs:170-171.w=0'], 'D': [CO, 'vs:168-169.w=0'],
        'G': [CO, 'ps:170.w=0', CO, 'vs:170-171.w=0'], 'H': [CO, 'ps:170.w=0', CO, 'ps:168.w=0'], 'K': [CO, 'ps:170.w=0.5']}

if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('--fview'); ap.add_argument('--capture'); ap.add_argument('--out'); ap.add_argument('--frames')
    ap.add_argument('--arms', default=','.join(ARMS)); ap.add_argument('--extra', action='append', default=[], help='NAME=--flag value ... extra arm (spaces split)')
    import os, sys; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import fview_exe as fe   # lane 218: one locator, refuses a stale exe
    fe.add_allow_stale(ap)
    a = ap.parse_args(); out = pathlib.Path(a.out); res = {}
    # lane 210 refused a stale exe here (lane 208 measured the K2 carry 16-43 % on an fview.exe built before lane 170's fix); lane 218 moved that check into tools\fview\fview_exe.py.
    a.fview = fe.find_or_exit(prefer=a.fview, allow_stale=a.allow_stale, kind='dev')
    arms = {k: ARMS[k] for k in a.arms.split(',')}
    for e in a.extra:
        n, v = e.split('=', 1); arms[n] = v.split()
    for f in [int(x) for x in a.frames.split(',')]:
        imgs = {}
        for k, e in arms.items(): imgs[k], _ = run(a.fview, a.capture, f, BASE + e, out / 'arms4' / ('f%d_%s' % (f, k)))
        for k, e in MASKS.items(): imgs[k], _ = run(a.fview, a.capture, f, BASE + e, out / 'arms4' / ('f%d_%s' % (f, k)))
        r = {}
        for g, mk in ((0, 'M0'), (1, 'M1')):
            m = np.abs(imgs[mk] - imgs['A']).max(axis=2) > THRESH
            r[g] = {'px': int(m.sum()), **{k: round(float(luma(imgs[k])[m].mean()), 1) for k in arms}}
        res[f] = r; print(f, json.dumps(r), flush=True)
    json.dump(res, open(out / 'arms4.json', 'w'), indent=1)
