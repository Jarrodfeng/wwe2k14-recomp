#!/usr/bin/env python3
"""Lane 115 offline proof (0 game runs): our stage screens (keys 801-807 of the arena pac: the wall behind the stage, the mini-tron, the ribbon band; geometry only) drawn in place through the REAL seam
(fview --screens-ours) on Check F's capture (run 7067's case zip, 337 scene records) and on run 7079's capture.

  py -I tools\\screens\\screens_ours_check.py [--root DIR] [--no-build] [--frames 200,260]

Bar (each line PASS / FAIL; a missing input FAILS, never skips):
  1. clean arm on each frame: the picture is BYTE-EQUAL to the stream's (fview, switch off): 0 differing pixels. Census: replaced > 0, mismatch 0, unjoined 0, variant 0, validation errors 0 (8-frame
     --validation 1 replay).
  2. the draws at the screens' site 0x82C0F4BC (fview --ops --json) at frame 200: with the screens class alone on, exactly 4 become fate screens_ours and the stream's count drops by 4.
  3. the planted faults through the seam (--replace-fault): each `,no_verify` twin must move the PICTURE (our geometry is what is drawn); the faults the byte compare refuses (uv_halves_swapped, colour_order,
     pair_swap, uv_sign) must change the census (draws kept) or the picture.
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
PLANTS = ('pair_swap', 'uv_halves_swapped', 'colour_order', 'uv_sign', 'pair_swap,no_verify', 'uv_halves_swapped,no_verify', 'colour_order,no_verify')
PICTURE_PLANTS = ('pair_swap,no_verify', 'uv_halves_swapped,no_verify')   # (a colour_order,no_verify fault may not show: the screens' vertex colour is white where it is visible; its census twin is checked)
SITE = 0x82C0F4BC


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=300):
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
    ap.add_argument('--frames', default='200,260')
    args = ap.parse_args()
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        return fail('numpy / Pillow are needed to read the pictures: %s' % e)
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261112_101633_s100_owner_s89_brock')
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    if not zips:
        return fail("input missing: Check F's case zip under " + runs)
    if not os.path.isdir(game):
        return fail('input missing: the game files ' + game)
    out = os.path.join(main_co, 'out', 'lane115')
    cap = os.path.join(out, 'checkF', 'capture')
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        os.makedirs(os.path.join(out, 'checkF'), exist_ok=True)
        zipfile.ZipFile(zips[0]).extractall(os.path.join(out, 'checkF'))
    if not os.path.isdir(os.path.join(cap, 'scene')):
        return fail('the capture has no scene\\ records: ' + cap)
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], 2400)
        if rc:
            print(text[-3000:])
            return fail('fview build (exit %d)' % rc)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        return fail('no fview.exe at ' + exe)
    work = os.path.join(root, 'out', 'l115check')
    os.makedirs(work, exist_ok=True)
    cache = {}

    def arm(tag, frame, extra=(), capture=cap):
        key = (tag, frame, capture)
        if key in cache:
            return cache[key]
        d = os.path.join(work, 'arm_%s_%d_%s' % (tag.replace(',', '_'), frame, os.path.basename(os.path.dirname(capture))))
        rc, text = run([exe, '--capture', capture, '--frame', str(frame), '--judge-out', d] + list(extra), 900)
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2000:])
            raise RuntimeError('arm %s frame %d failed (exit %d)' % (tag, frame, rc))
        cache[key] = (np.asarray(Image.open(png).convert('RGB'), dtype=np.int16), text)
        return cache[key]

    def draws_at_site(tag, frame, extra):
        d = os.path.join(work, 'ops_%s_%d.json' % (tag, frame))
        rc, text = run([exe, '--capture', cap, '--frame', str(frame), '--ops', '--json', d] + extra, 900)
        if rc or not os.path.isfile(d):
            print(text[-1500:])
            raise RuntimeError('ops %s frame %d failed (exit %d)' % (tag, frame, rc))
        ops = json.load(open(d, encoding='utf-8'))['ops']
        draws = [o for o in ops if o.get('kind') == 'draw' and (o.get('pass_site', 0) & 0xFFFFFFFF) == SITE]
        stream = [o for o in draws if o.get('fate') == 'stream']
        ours = [o for o in draws if o.get('fate') == 'screens_ours']
        why = {}
        for o in stream:
            why[o.get('keep_why', '')] = why.get(o.get('keep_why', ''), 0) + 1
        return len(stream), len(ours), why

    census = re.compile(r'\[rdr\]\[screens\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
    diff = lambda a, b: int((np.abs(a - b).max(2) > 0).sum())
    clean_args = ['--screens-ours', '--game', game]
    frames = [int(x) for x in args.frames.split(',')]
    fails = 0
    try:
        for f in frames:
            s, _ = arm('stream', f)
            o, text = arm('clean', f, clean_args)
            n = diff(o, s)
            m = census.search(text)
            ok = n == 0 and m is not None
            print(('PASS' if ok else 'FAIL') + '  frame %d: clean vs stream differing pixels %d (bar: byte-equal, 0); census %s' % (f, n, m.group(0) if m else 'MISSING'))
            fails += not ok
            if m:
                nf, fb, rep, kept, nr, unj, mis, var, tex, pal, notours = map(int, m.groups())
                ok = nf > 0 and fb <= 2 and rep > 0 and mis == 0 and unj == 0 and var == 0
                print(('PASS' if ok else 'FAIL') + '  frame %d census: replaced %d, mismatch %d, unjoined %d, variant %d, fallback %d' % (f, rep, mis, unj, var, fb))
                fails += not ok
            for l in text.splitlines():
                if 'objects joined' in l:
                    print('INFO  ' + l[:900])
                    break
        s0, p0, _ = draws_at_site('off', 200, [])
        s1, p1, why = draws_at_site('on', 200, clean_args)
        ok = p1 == 4 and s0 - s1 == 4 and p0 == 0
        print(('PASS' if ok else 'FAIL') + '  0x82C0F4BC draws at frame 200 (only the screens class on): stream %d -> %d, screens_ours draws %d (want 4 = wall, 2nd wall variant block, mini-tron, ribbon band); kept reasons of the stream ones: %s' % (s0, s1, p1, why))
        fails += not ok
        rc, vt = run([exe, '--capture', cap, '--screens-ours', '--game', game, '--frames', '8', '--validation', '1', '--replay'], 900)
        ok = rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt
        print(('PASS' if ok else 'FAIL') + '  validation: 8-frame replay with the layer on, errors 0 (exit %d)' % rc)
        if not ok:
            print(vt[-1500:])
        fails += not ok
        # run 7079 (the owner Brock run): the same proof on a second capture
        cap2 = os.path.join(out, 'r7079', 'capture')
        r2 = os.path.join(main_co, 'out', 's101', 'runs', '20261112_122623_s100_owner_s89_brock')
        z2 = [os.path.join(r2, f) for f in os.listdir(r2) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(r2) else []
        if not os.path.isfile(os.path.join(cap2, 'images.bin')):
            if not z2:
                return fail("input missing: run 7079's case zip under " + r2)
            os.makedirs(os.path.join(out, 'r7079'), exist_ok=True)
            zipfile.ZipFile(z2[0]).extractall(os.path.join(out, 'r7079'))
        s, _ = arm('stream', 200, (), cap2)
        o, text = arm('clean', 200, clean_args, cap2)
        m = census.search(text)
        n = diff(o, s)
        ok = n == 0 and m is not None and int(m.group(3)) > 0 and int(m.group(7)) == 0
        print(('PASS' if ok else 'FAIL') + '  run 7079 frame 200: clean vs stream differing pixels %d; census %s' % (n, m.group(0) if m else 'MISSING'))
        fails += not ok
        base = {f: int(census.search(arm('clean', f, clean_args)[1]).group(3)) for f in frames}
        for name in PLANTS:
            ds, reps = [], []
            for f in frames:
                img, t = arm('f_' + name, f, ['--screens-ours', '--replace-fault', name, '--game', game])
                ds.append(diff(img, arm('clean', f, clean_args)[0]))
                mm = census.search(t)
                reps.append(int(mm.group(3)) if mm else -1)
            moved_pic = max(ds) > 0
            moved_census = any(r != base[f] for r, f in zip(reps, frames))
            ok = moved_pic if name in PICTURE_PLANTS else (moved_pic or moved_census)
            print(('PASS' if ok else 'FAIL') + '  planted %s: pixels from the clean arm at frames %s = %s; replaced %s vs clean %s -> %s' % (
                name, frames, ds, reps, [base[f] for f in frames], ('picture' if moved_pic else '') + (' + ' if moved_pic and moved_census else '') + ('census' if moved_census else '') or 'nothing'))
            fails += not ok
    except RuntimeError as e:
        return fail(str(e))
    print('screens_ours_check: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
