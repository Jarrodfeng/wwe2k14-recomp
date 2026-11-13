#!/usr/bin/env python3
"""Lane 70 offline proof (0 game runs): our ring drawn in place through the REAL seam (fview --ring-ours), Check B's capture (run 7051, arena 4, 341 frames, scene records).

  py -I tools\\ring\\ring_ours_check.py [--root DIR] [--step N] [--no-build]

Bar (each line PASS / FAIL; a missing input FAILS, never skips):
  1. clean arm on every STEP-th frame: the picture is BYTE-EQUAL to the stream's (fview, switch off): 0 differing pixels (RING_PROMOTION_MAP.md 5.1; the wrestlers' bar). The census of the last
     frame's run: fallback <= 2, replaced > 0, mismatch 0, unjoined 0, variant 0, texture 0, validation errors 0 (8-frame --validation 1 replay).
  2. the planted faults through the seam (--replace-fault): each must move the picture away from the clean arm OR change the census (fewer replaced / more kept) at a plant frame; the `,no_verify`
     twins (byte compare off) must move the PICTURE (F4: our geometry is what is drawn).
  Texture slots: fview feeds the image-content table (lane 66), so the file textures are ours where the content check passes (census `content: checks N ok K`: K > 0 is required) and the clean picture
  stays byte-equal WITH them; the `skirt_tie` plant (the wrong skirt texture forced in) must move the picture.
Bryan's entrance and wm30 carry no scene records (no arena id for the hook): their proof is the unit test (ring_ours_selftest.py), which runs the same shipped core on them.
Frames are one fview process each (the viewer replays frames 0..N); every process has a timeout."""
import argparse
import os
import re
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
PLANT_FRAMES = (100, 200, 300)
PLANTS = ('colour_order', 'uv_halves_swapped', 'uv_sign', 'bone_zero', 'weight_word', 'skirt_tie', 'part_only_join', 'lod_pin', 'uv_halves_swapped,no_verify', 'colour_order,no_verify')


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=300):
    # The GPU is shared with other lanes: retry ONLY the frame-ring memory refusal (a few attempts, said), never another failure.
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
    ap.add_argument('--step', type=int, default=8)
    ap.add_argument('--no-build', action='store_true')
    args = ap.parse_args()
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        return fail('numpy / Pillow are needed to read the pictures: %s' % e)
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261111_215201_s100_owner_s89_brock')
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    if not zips:
        return fail("input missing: Check B's case zip under " + runs)
    if not os.path.isdir(game):
        return fail('input missing: the game files ' + game)
    out = os.path.join(main_co, 'out', 'lane70')
    cap = os.path.join(main_co, 'out', 'lane40', 'brock', 'capture')   # the folder lane 40 unpacked
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        cap = os.path.join(out, 'brock', 'capture')
        if not os.path.isfile(os.path.join(cap, 'images.bin')):
            os.makedirs(os.path.join(out, 'brock'), exist_ok=True)
            zipfile.ZipFile(zips[0]).extractall(os.path.join(out, 'brock'))
    if not os.path.isdir(os.path.join(cap, 'scene')):
        return fail('the capture has no scene\\ records: ' + cap)
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 1800)
        if rc:
            print(text[-3000:])
            return fail('fview build (exit %d)' % rc)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        return fail('no fview.exe at ' + exe)
    work = os.path.join(root, 'out', 'l70check')
    os.makedirs(work, exist_ok=True)
    cache = {}

    def arm(tag, frame, extra, capdir=None):
        key = (tag, frame, capdir)
        if key in cache:
            return cache[key]
        d = os.path.join(work, 'arm_%s_%d%s' % (tag, frame, '_h1' if capdir else ''))
        rc, text = run([exe, '--capture', capdir or cap, '--frame', str(frame), '--judge-out', d] + extra, 600)
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2000:])
            raise RuntimeError('arm %s frame %d failed (exit %d)' % (tag, frame, rc))
        cache[key] = (np.asarray(Image.open(png).convert('RGB'), dtype=np.int16), text)
        return cache[key]

    census = re.compile(r'\[rdr\]\[ring\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
    diff = lambda a, b: int((np.abs(a - b).max(2) > 0).sum())
    clean_args = ['--ring-ours', '--game', game]
    fails = 0
    try:
        frames = sorted(set(list(range(0, 341, args.step)) + list(PLANT_FRAMES) + [340]))
        worst, worst_f, total = 0, -1, 0
        for f in frames:
            s, _ = arm('stream', f, [])
            o, _ = arm('clean', f, clean_args)
            n = diff(o, s)
            total += n
            if n > worst:
                worst, worst_f = n, f
            cache.pop(('stream', f), None)
            if f not in PLANT_FRAMES + (340,):
                cache.pop(('clean', f), None)
        ok = worst == 0
        print(('PASS' if ok else 'FAIL') + '  clean vs stream, %d frames (every %d + the plant frames): differing pixels max %d (frame %d), total %d (bar: byte-equal, 0)' % (len(frames), args.step, worst, worst_f, total))
        fails += not ok
        _, text = arm('clean', 340, clean_args)
        m = census.search(text)
        if not m:
            print(text[-1500:])
            return fail('no census line in the clean arm')
        nf, fb, rep, kept, nr, unj, mis, var, tex, pal, notours = map(int, m.groups())
        cm = re.search(r'content: checks (\d+) ok (\d+) differs (\d+) no feed (\d+)', text)
        ok = nf > 0 and fb <= 2 and rep > 0 and mis == 0 and unj == 0 and var == 0 and tex == 0 and cm is not None and int(cm.group(2)) > 0 and int(cm.group(4)) == 0
        print(('PASS' if ok else 'FAIL') + '  texture slots ours: ' + (cm.group(0) if cm else 'no content census (the feed is off?)'))
        print(('PASS' if ok else 'FAIL') + '  census (frame 340 run): ' + m.group(0))
        fails += not ok
        print('INFO  ' + [l for l in text.splitlines() if 'objects joined' in l][-1][:600])
        rc, vt = run([exe, '--capture', cap, '--ring-ours', '--game', game, '--frames', '8', '--validation', '1', '--replay'], 600)
        ok = rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt
        print(('PASS' if ok else 'FAIL') + '  validation: 8-frame replay with the layer on, errors 0 (exit %d)' % rc)
        if not ok:
            print(vt[-1500:])
        fails += not ok
        base = {f: int(census.search(arm('clean', f, clean_args)[1]).group(3)) for f in PLANT_FRAMES}
        for name in PLANTS:
            ds, reps = [], []
            for f in PLANT_FRAMES:
                img, t = arm('f_' + name, f, ['--ring-ours', '--replace-fault', name, '--game', game])
                ds.append(diff(img, arm('clean', f, clean_args)[0]))
                mm = census.search(t)
                reps.append(int(mm.group(3)) if mm else -1)
            moved_pic = max(ds) > 0
            moved_census = any(r != base[f] for r, f in zip(reps, PLANT_FRAMES))
            ok = moved_pic if 'no_verify' in name else (moved_pic if name == 'skirt_tie' else (moved_pic or moved_census))   # skirt_tie acts on a texture slot: it must move the picture now that the feed reaches fview
            ok = ok  # (F4: with the compare off the picture itself must move: our geometry reaches it)   # F4: with the compare off the picture itself must move: our geometry reaches it
            print(('PASS' if ok else 'FAIL') + '  planted %s: pixels from the clean arm at frames %s = %s; replaced %s vs clean %s -> %s' % (
                name, PLANT_FRAMES, ds, reps, [base[f] for f in PLANT_FRAMES], ('picture' if moved_pic else '') + (' + ' if moved_pic and moved_census else '') + ('census' if moved_census else '') or 'nothing'))
            fails += not ok
        # Run 7074 (post SDK patch 109: the stream holds SIGNED u; posts and ropes were refused live with the |u| rule): the clean arm must replace them, mismatch 0, picture equal.
        h1 = os.path.join(out, 'h1', 'capture')
        h1runs = os.path.join(main_co, 'out', 's101', 'runs', '20261112_121740_s100_owner_s89_brock')
        if not os.path.isfile(os.path.join(h1, 'images.bin')):
            hz = [os.path.join(h1runs, f) for f in os.listdir(h1runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(h1runs) else []
            if not hz:
                return fail("input missing: run 7074's case zip under " + h1runs)
            os.makedirs(os.path.join(out, 'h1'), exist_ok=True)
            zipfile.ZipFile(hz[0]).extractall(os.path.join(out, 'h1'))
        dh, repl_h = 0, 0
        for f in (20, 100, 200, 300):
            s1, _ = arm('stream', f, [], h1)
            o1, t1 = arm('clean', f, clean_args, h1)
            dh += diff(o1, s1)
            mh = census.search(t1)
            repl_h = max(repl_h, int(mh.group(3)) if mh else 0)
            inst = re.search(r'instances per entry \[([^\]]*)\]', t1)
            mism = int(mh.group(7)) if mh else -1
            ok_f = mism == 0 and inst is not None and '951x4' in inst.group(1) and ('960x' in inst.group(1) or '980x' in inst.group(1)) and 'u sign rule: signed' in t1
            print(('PASS' if ok_f else 'FAIL') + '  run 7074 frame %d: mismatch %d, instances [%s], u sign rule signed: %s' % (f, mism, inst.group(1) if inst else '?', 'u sign rule: signed' in t1))
            fails += not ok_f
        ok = dh == 0
        print(('PASS' if ok else 'FAIL') + '  run 7074 clean vs stream, frames 20 / 100 / 200 / 300: differing pixels total %d (posts + ropes replaced, replaced %d)' % (dh, repl_h))
        fails += not ok
    except RuntimeError as e:
        return fail(str(e))
    print('ring_ours_check: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
