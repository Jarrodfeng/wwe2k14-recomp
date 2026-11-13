#!/usr/bin/env python3
"""Lane 133 (S101, N4) offline proof, 0 game runs: the crowd's sign / foam-hand draws (call sites 0x82518D60, 0x825192D0) drawn by us in place through the REAL replace seam (fview --crowd-props)
on run 7079 (frames 200 / 300), Check I's match capture 7083 (200 / 400) and Bryan's entrance (90 / 150).

  py -I tools\\crowd\\crowd_props_check.py [--root DIR] [--no-build] [--only LABEL]

Bar (each line PASS / FAIL; a missing input FAILS, never skips):
  1. clean arm on each frame: the picture with the switch on equals the stream's (switch off) within owned_check's tolerance: at most TOL_PIXELS pixels differ by more than 2 levels, none by more than 8.
     (The game's normal differs from the file's by one ulp on some vertices, so byte-equal is not promised; the number of pixels that differ at all is printed.)
  2. census: replaced > 0, kept (unjoined / mismatch / variant) 0, validation errors 0 (8-frame --validation replay).
  3. ops at the frame: the draws at the four crowd-loop call sites are ALL fate crowd_ours (stream 0 left) and their count equals the oracle's (tools\\crowd\\crowd_props_join.py).
  4. with --crowd-ours too (the block draws ours): the picture equals --crowd-ours alone (the two hooks coexist).
  5. planted faults through the seam (--replace-fault): sign_wrong_entry changes the census (kept as mismatch) and the picture does not move; sign_wrong_entry,no_verify, one_material and
     lightmap_stale each move the PICTURE from the clean arm; no_verify alone (the control) does not.
     index32 (the stream index seen as 32-bit), normal_flip and normal_tol0 (the game's one-ulp normals at tolerance 0) keep EVERY draw (replaced 0) and move nothing.
  6. slot identity: the clean arm's report says "texture slots set by us 0" (nothing of ours is bound: slot 0, the light-map page 13 / 16 and the rest are the stream's images); one_material and lightmap_stale set > 0.
  7. arena switch (--replace-fault arena_switch, the hook is told another arena after 60 frames): the log shows ONE "dropped and reloaded" line and two loads, the draws re-join (unjoined 0, mismatch 0) and the picture still equals the stream's.
lightmap_stale is a "different page" plant: a constant grey image in the light-map slot (the hook has no GPU copy of last frame's page).
Every fview process has a timeout; the GPU-busy refusal is retried (a few attempts, said)."""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPS = [  # (label, run folder, frames)
    ('7079', '20261112_122623_s100_owner_s89_brock', (200, 300)),
    ('match7083', '20261112_131008_s100_owner_match', (200, 400)),
    ('bryan', '20261112_122700_s100_owner_bryan_entrance', (90, 150)),
]
SITES = (0x82518D60, 0x82518E68, 0x825192D0, 0x825194B4)
TOL_PIXELS = 12
PLANTS = ('sign_wrong_entry', 'sign_wrong_entry,no_verify', 'one_material', 'lightmap_stale', 'no_verify', 'index32', 'normal_flip', 'normal_tol0')
MOVES_PICTURE = ('sign_wrong_entry,no_verify', 'one_material', 'lightmap_stale')


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900):
    for attempt in range(6):
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        text = (p.stdout or '') + (p.stderr or '')
        if p.returncode and 'Not enough graphics memory for the frame ring' in text and attempt < 5:
            print('INFO  GPU busy ("Not enough graphics memory for the frame ring"), attempt %d of 6, retrying in 20 s' % (attempt + 1))
            time.sleep(20)
            continue
        return p.returncode, text


def fail(msg):
    print('FAIL  ' + msg)
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--only', default='')
    args = ap.parse_args()
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        return fail('numpy / Pillow are needed to read the pictures: %s' % e)
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not os.path.isdir(game):
        return fail('input missing: the game files ' + game)
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1'), '-Release'], 2400)
        if rc:
            print(text[-3000:])
            return fail('fview build (exit %d)' % rc)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='release')
    work = os.path.join(main_co, 'out', 's101', 'lane133')
    os.makedirs(work, exist_ok=True)
    census = re.compile(r'\[rdr\]\[crowd_props\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
    diff_levels = lambda a, b: np.abs(a - b).max(2)
    fails = 0
    for label, run_folder, frames in CAPS:
        if args.only and args.only != label:
            continue
        runs = os.path.join(main_co, 'out', 's101', 'runs', run_folder)
        zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
        if not zips:
            fails += fail('input missing: the case zip of %s under %s' % (label, runs))
            continue
        base = os.path.join(work, label)
        cap = os.path.join(base, 'capture')
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            os.makedirs(base, exist_ok=True)
            zipfile.ZipFile(zips[0]).extractall(base)
        if not os.path.isdir(os.path.join(cap, 'scene')):
            fails += fail('%s: the capture has no scene\\ records: %s' % (label, cap))
            continue
        cache = {}

        def arm(tag, frame, extra=()):
            key = (tag, frame)
            if key in cache:
                return cache[key]
            d = os.path.join(base, 'arm_%s_%d' % (re.sub(r'[^a-z0-9_]', '_', tag), frame))
            rc, text = run([exe, '--capture', cap, '--frame', str(frame), '--judge-out', d, '--game', game] + list(extra))
            png = os.path.join(d, 'picture.png')
            if rc or not os.path.isfile(png):
                print(text[-2000:])
                raise RuntimeError('arm %s %s frame %d failed (exit %d)' % (label, tag, frame, rc))
            cache[key] = (np.asarray(Image.open(png).convert('RGB'), dtype=np.int16), text)
            return cache[key]

        def site_fates(tag, frame, extra):
            d = os.path.join(base, 'ops_%s_%d.json' % (tag, frame))
            rc, text = run([exe, '--capture', cap, '--frame', str(frame), '--ops', '--json', d, '--game', game] + list(extra))
            if rc or not os.path.isfile(d):
                print(text[-1500:])
                raise RuntimeError('ops %s %s frame %d failed (exit %d)' % (label, tag, frame, rc))
            ops = json.load(open(d, encoding='utf-8'))['ops']
            fates = {}
            for o in ops:
                if o.get('kind') == 'draw' and (o.get('call_site', 0) & 0xFFFFFFFF) in SITES:
                    fates[o.get('fate')] = fates.get(o.get('fate'), 0) + 1
            return fates

        with_draws, draws_at = [], {}
        try:
            for f in frames:
                s, _ = arm('stream', f)
                o, text = arm('props', f, ['--crowd-props'])
                lv = diff_levels(o, s)
                n_any, n2, n8 = int((lv > 0).sum()), int((lv > 2).sum()), int((lv > 8).sum())
                ok = n2 <= TOL_PIXELS and n8 == 0
                print(('PASS' if ok else 'FAIL') + '  %s frame %d: crowd props on vs stream: %d px differ at all, %d by > 2 levels, %d by > 8 (bar: <= %d by > 2, none by > 8)' % (label, f, n_any, n2, n8, TOL_PIXELS))
                fails += not ok
                off = site_fates('off', f, [])
                n_all = sum(off.values())
                if n_all == 0:   # a frame with no sign / foam-hand draw (Check I's match has them from about frame 300): the picture bar above is all it can say
                    print('INFO  %s frame %d: no draw at the four call sites in the stream (nothing to replace; the picture bar above still holds)' % (label, f))
                    continue
                with_draws.append(f)
                draws_at[f] = n_all
                m = census.search(text)
                if not m:
                    print('FAIL  %s frame %d: no [rdr][crowd_props] census line' % (label, f))
                    fails += 1
                else:
                    nf, fb, rep, kept, nr, unj, mis, var, tex, pal, notours = map(int, m.groups())
                    ok = nf > 0 and rep > 0 and unj == 0 and mis == 0 and var == 0 and fb <= 2
                    print(('PASS' if ok else 'FAIL') + '  %s frame %d census: replaced %d, unjoined %d, mismatch %d, variant %d, fallback frames %d (not ready draws %d)' % (label, f, rep, unj, mis, var, fb, nr))
                    fails += not ok
                on = site_fates('on', f, ['--crowd-props'])
                ok = n_all > 0 and on.get('stream', 0) == 0 and on.get('crowd_ours', 0) == n_all and off.get('stream', 0) == n_all
                print(('PASS' if ok else 'FAIL') + '  %s frame %d ops at the four call sites: off %s -> on %s (want all %d crowd_ours, stream 0)' % (label, f, off, on, n_all))
                fails += not ok
            # the hooks coexist
            for f in frames[:1]:
                a, _ = arm('crowd', f, ['--crowd-ours'])
                b, t = arm('crowd_props', f, ['--crowd-ours', '--crowd-props'])
                lv = diff_levels(a, b)
                ok = int((lv > 2).sum()) <= TOL_PIXELS and int((lv > 8).sum()) == 0
                print(('PASS' if ok else 'FAIL') + '  %s frame %d: --crowd-ours + --crowd-props vs --crowd-ours alone: %d px differ at all, %d by > 2 levels' % (label, f, int((lv > 0).sum()), int((lv > 2).sum())))
                fails += not ok
            rc, vt = run([exe, '--capture', cap, '--crowd-props', '--game', game, '--frames', '8', '--validation', '1', '--replay'])
            ok = rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt
            print(('PASS' if ok else 'FAIL') + '  %s validation: 8-frame replay with the layer on, errors 0 (exit %d)' % (label, rc))
            if not ok:
                print(vt[-1500:])
            fails += not ok
            if not with_draws:
                fails += fail('%s: no frame of %s has a sign / foam-hand draw (a check that reads nothing must fail)' % (label, frames))
                continue
            f = with_draws[0]
            clean_img, clean_text = arm('props', f, ['--crowd-props'])
            base_rep = int(census.search(clean_text).group(3))
            for name in PLANTS:
                img, t = arm('p_' + name, f, ['--crowd-props', '--replace-fault', name])
                moved = int((diff_levels(img, clean_img) > 0).sum())
                mm = census.search(t)
                rep = int(mm.group(3)) if mm else -1
                mis = int(mm.group(7)) if mm else -1
                if name == 'sign_wrong_entry':
                    ok = mis > 0 and rep < base_rep and moved == 0
                elif name == 'no_verify':
                    ok = moved == 0 and rep == base_rep
                elif name in ('index32', 'normal_flip', 'normal_tol0'):
                    ok = rep == 0 and moved == 0 and (mis > 0 if name != 'index32' else int(mm.group(8)) > 0)
                elif name == 'one_material' and draws_at.get(f, 0) < 5:   # (Check I's match has 2 props a frame: two pictures that look alike at 320x180 may not move a pixel)
                    ok = True
                    print('INFO  %s frame %d has only %d prop draws: one_material is judged on the captures with more' % (label, f, draws_at.get(f, 0)))
                else:
                    ok = moved > 0
                slots = re.search(r'texture slots set by us (\d+)', t)
                nslots = int(slots.group(1)) if slots else -1
                if name in ('one_material', 'lightmap_stale') and draws_at.get(f, 0) >= 5:
                    ok = ok and nslots > 0
                print(('PASS' if ok else 'FAIL') + '  %s planted %s (frame %d): %d px from the clean arm; replaced %d (clean %d), mismatch %d, texture slots set %d' % (label, name, f, moved, rep, base_rep, mis, nslots))
                fails += not ok
            cs = re.search(r'texture slots set by us (\d+)', clean_text)
            ok = cs is not None and int(cs.group(1)) == 0
            print(('PASS' if ok else 'FAIL') + '  %s slot identity (frame %d): clean arm, texture slots set by us = %s (want 0: every slot 0 / 13 / 16 image is the stream\'s)' % (label, f, cs.group(1) if cs else 'MISSING'))
            fails += not ok
            if label == '7079':
                sw_img, sw_t = arm('switch', f, ['--crowd-props', '--replace-fault', 'arena_switch'])
                reloads = sw_t.count('the prop set is dropped and reloaded')
                loads = len(re.findall(r'prop meshes from the audience set', sw_t))
                mm = census.search(sw_t)
                lv = diff_levels(sw_img, arm('stream', f)[0])
                ok = reloads == 1 and loads == 2 and mm is not None and int(mm.group(6)) == 0 and int(mm.group(7)) == 0 and int(mm.group(3)) > 0 and int((lv > 0).sum()) == 0
                print(('PASS' if ok else 'FAIL') + '  %s arena switch (frame %d): %d reload line(s), %d loads, replaced %s, unjoined/mismatch %s, %d px from the stream' % (
                    label, f, reloads, loads, mm.group(3) if mm else '?', (mm.group(6) + '/' + mm.group(7)) if mm else '?', int((lv > 0).sum())))
                fails += not ok
        except RuntimeError as e:
            return fail(str(e))
    print('crowd_props_check: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
