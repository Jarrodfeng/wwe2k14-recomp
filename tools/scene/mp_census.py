#!/usr/bin/env python3
"""S103 lane MP offline census (0 game runs): what the STREAM draws on the flat 2D corpus, read with fview --ops --json, against what the recorder is built to read.
For each capture and a spread of frames: particle-class draws, the particle GROUPS (maximal runs of particle draws, separated by any other draw), the 2D rows (class sprite, technique T_Rect / T_Sprite /
T_SpriteYUVAlpha) before each group (= the row_after_p1 - 1 the recorder stamps at the node call), and the verdict the flat frame would read: no particle draw = Whole (an empty pass), else Held
(screen_cam). The recorded side (the node calls themselves) only exists in a live run: this census is the stream side of the PF-style comparison and the prediction the first live census must meet.
  py -I tools/scene/mp_census.py [--root DIR] [--fview EXE] [--captures DIR] [--frames N] [--json OUT]"""
import argparse
import json
import os
import subprocess
import sys

NAMES = ['c2d_title', 'c2d_menu', 'c2d_options', 'c2d_play', 'c2d_select', 'c2d_load']
ROW_TECH = ('T_Rect', 'T_Sprite', 'T_SpriteYUVAlpha', 'T_RectRGradation')


def census(ops):
    draws = [o for o in ops if o['kind'] == 'draw']
    groups, rows, cur, n_part = [], 0, 0, 0
    for o in draws:
        if o.get('class') == 'particle':
            n_part += 1
            cur += 1
        else:
            if cur:
                groups.append({'draws': cur, 'rows_before': rows_at_start})
                cur = 0
            if o.get('class') == 'sprite' and o.get('technique') in ROW_TECH:
                rows += 1
            rows_at_start = rows
        if cur == 1 and o.get('class') == 'particle':
            rows_at_start = rows
    if cur:
        groups.append({'draws': cur, 'rows_before': rows_at_start})
    return {'particle_draws': n_part, 'groups': groups, 'rows': rows, 'sites': sorted({o['call_site'] for o in draws if o.get('class') == 'particle'})}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--fview', default=None)
    ap.add_argument('--captures', default=None)
    ap.add_argument('--frames', type=int, default=4)
    ap.add_argument('--json', default=None)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_root = os.path.normpath(os.path.join(root, '..', '..', '..')) if os.path.basename(os.path.dirname(os.path.dirname(root))) == '.claude' else root
    fview = a.fview or os.path.join(main_root, 'out', 'renderer', 'dev', 'fview.exe')
    caps = a.captures or os.path.join(main_root, 'out', 'captures')
    game = os.path.join(main_root, 'game_data', 'extracted')
    tmp = os.path.join(root, 'out', 's103', 'mp')
    os.makedirs(tmp, exist_ok=True)
    report = {}
    for name in NAMES:
        cap = os.path.join(caps, name)
        fdir = os.path.join(cap, 'frames')
        if not os.path.isdir(fdir):
            print('%-12s capture missing' % name)
            continue
        n = len([f for f in os.listdir(fdir) if f.endswith('.bin')])
        picks = sorted({int(i * (n - 1) / max(1, a.frames - 1)) for i in range(a.frames)})
        res = []
        for fr in picks:
            out = os.path.join(tmp, '%s_%d.json' % (name, fr))
            p = subprocess.run([fview, '--capture', cap, '--frame', str(fr), '--ops', '--json', out, '--root', main_root, '--game', game], capture_output=True, text=True, errors='replace')
            if not os.path.exists(out):
                print('%-12s frame %d: fview failed: %s' % (name, fr, (p.stdout + p.stderr)[-200:]))
                continue
            with open(out) as f:
                res.append(dict(frame=fr, **census(json.load(f)['ops'])))
        report[name] = {'frames_in_capture': n, 'sampled': res}
        whole = sum(1 for r in res if r['particle_draws'] == 0)
        print('%-12s frames %3d sampled %d: particle draws %s; groups per frame %s; rows before each group %s; verdict Whole(empty) %d / Held(screen_cam) %d' % (
            name, n, len(res), [r['particle_draws'] for r in res], [len(r['groups']) for r in res], [[g['rows_before'] for g in r['groups']] for r in res], whole, len(res) - whole))
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(report, f, indent=1)
    return 0


if __name__ == '__main__':
    sys.exit(main())
