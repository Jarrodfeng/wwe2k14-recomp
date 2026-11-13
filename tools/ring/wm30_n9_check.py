#!/usr/bin/env python3
"""Lane 137 offline proof (S101, 0 game runs; docs\\renderer\\WHATS_LEFT.md N9): the wm30-only draws that were still the game's, drawn by us through the REAL seam
(fview --ring-ours --props-ours) on Check I's run 7084 (30 Years, arena 29, 668 frames, scene records from frame 131; frame 131 itself is the hooks' start-up frame, the pictures are the stream's there).

  py -I tools\\ring\\wm30_n9_check.py [--root DIR] [--frames 132,200,300,450,600,667] [--no-plants]

Bar (each line PASS / FAIL; a missing input FAILS, never skips):
  1. per frame, with the ring and props classes on: the picture is BYTE-EQUAL to the stream's (0 differing pixels); the census has replaced > 0, mismatch 0, unjoined 0, variant 0.
  2. the ops table (fview --ops --json) at each frame: the 24 T_Lambert draws at 0x82BF80A4 are fate ring_ours (stream 0), the cage / commentary-desk draws at 0x82C0A950 (14) are fate
     props_ours, and none of the N9 objects (0xAAF36580, 0xA4B321F0, 0xA4B317C0, 0xA4B32C20, 0xAA1113F0, 0xAA112C50, 0xAA110900) is left as stream.
  3. validation: an 8-frame replay with the layers on, errors 0.
  4. planted faults through the seam (--replace-fault proxy_wrong for the ring, scene_wrong for the props): each must change the census (draws kept) at frame 300.
Every fview process has a timeout; the GPU-busy refusal is retried (a few attempts, said)."""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
RUN = '20261112_131029_s100_owner_s90_wm30'
LAMBERT = 0x82BF80A4
COLOUR = 0x82C0A950
N9_OBJECTS = {0xAAF36580: 'proxy', 0xA4B321F0: 'cage', 0xA4B317C0: 'cage', 0xA4B32C20: 'cage', 0xAA1113F0: 'desk', 0xAA112C50: 'monitors', 0xAA110900: 'k_color'}


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
    ap.add_argument('--frames', default='132,200,300,450,600,667')
    ap.add_argument('--no-plants', action='store_true')
    ap.add_argument('--fault', default='', help='tests only: a real re-find (lambert_off / scene_off) handed to fview as --replace-fault; the check must then FAIL naming the class that lost its draws')
    args = ap.parse_args()
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        return fail('numpy / Pillow are needed to read the pictures: %s' % e)
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', RUN)
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    if not zips:
        return fail("input missing: run 7084's case zip under " + runs)
    if not os.path.isdir(game):
        return fail('input missing: the game files ' + game)
    out = os.path.join(main_co, 'out', 'lane70', 'wm30b')
    cap = os.path.join(out, 'capture')
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        import zipfile
        os.makedirs(out, exist_ok=True)
        zipfile.ZipFile(zips[0]).extractall(out)
    if not os.path.isdir(os.path.join(cap, 'scene')):
        return fail('the capture has no scene\\ records: ' + cap)
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    if not os.path.isfile(exe):
        return fail('no fview.exe at ' + exe + ' (tools\\fview\\build.ps1)')
    work = os.path.join(root, 'out', 'l137check')
    os.makedirs(work, exist_ok=True)
    cache = {}
    ring_re = re.compile(r'\[rdr\]\[ring\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
    props_re = re.compile(r'\[rdr\]\[props\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
    on = ['--ring-ours', '--props-ours', '--game', game] + (['--replace-fault', args.fault] if args.fault else [])

    def arm(tag, frame, extra=()):
        key = (tag, frame)
        if key in cache:
            return cache[key]
        d = os.path.join(work, 'arm_%s_%d' % (tag.replace(',', '_'), frame))
        rc, text = run([exe, '--capture', cap, '--frame', str(frame), '--judge-out', d] + list(extra), 900)
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2000:])
            raise RuntimeError('arm %s frame %d failed (exit %d)' % (tag, frame, rc))
        cache[key] = (np.asarray(Image.open(png).convert('RGB'), dtype=np.int16), text)
        return cache[key]

    def ops(tag, frame, extra):
        d = os.path.join(work, 'ops_%s_%d.json' % (tag, frame))
        rc, text = run([exe, '--capture', cap, '--frame', str(frame), '--ops', '--json', d] + extra, 900)
        if rc or not os.path.isfile(d):
            print(text[-1500:])
            raise RuntimeError('ops %s frame %d failed (exit %d)' % (tag, frame, rc))
        return [o for o in json.load(open(d, encoding='utf-8'))['ops'] if o.get('kind') == 'draw']

    diff = lambda a, b: int((np.abs(a - b).max(2) > 0).sum())
    frames = [int(x) for x in args.frames.split(',')]
    fails = 0
    try:
        for f in frames:
            s, _ = arm('stream', f)
            o, text = arm('ours', f, on)
            n = diff(o, s)
            r, p = ring_re.search(text), props_re.search(text)
            ok = n == 0 and r is not None and p is not None
            print(('PASS' if ok else 'FAIL') + '  frame %d: ring + props vs stream differing pixels %d (bar: byte-equal, 0); census %s' % (f, n, 'present' if (r and p) else 'MISSING'))
            fails += not ok
            for tag, m in (('ring', r), ('props', p)):
                if not m:
                    continue
                nf, fb, rep, kept, nr, unj, mis, var, tex, pal, notours = map(int, m.groups())
                ok = rep > 0 and mis == 0 and unj == 0 and var == 0
                print(('PASS' if ok else 'FAIL') + '  frame %d %s census: replaced %d, mismatch %d, unjoined %d, variant %d, fallback %d, not ready %d' % (f, tag, rep, mis, unj, var, fb, nr))
                fails += not ok
            draws = ops('ours', f, on)
            lam = [d for d in draws if (d.get('pass_site', 0) & 0xFFFFFFFF) == LAMBERT]
            left = [d for d in draws if (d.get('object', 0) & 0xFFFFFFFF) in N9_OBJECTS and d.get('fate') == 'stream' and (d.get('pass_site', 0) & 0xFFFFFFFF) in (LAMBERT, COLOUR)]
            lam_ours = sum(1 for d in lam if d.get('fate') == 'ring_ours')
            scene_ours = sum(1 for d in draws if (d.get('object', 0) & 0xFFFFFFFF) in N9_OBJECTS and (d.get('pass_site', 0) & 0xFFFFFFFF) == COLOUR and d.get('fate') == 'props_ours')
            ok = lam_ours == len(lam) == 24 and scene_ours == 14 and not left
            print(('PASS' if ok else 'FAIL') + '  frame %d ops: T_Lambert ring_ours %d/24 (T_Lambert draws %d), props_ours %d/14 (cage + desk + monitors + k_color), N9 objects left as stream %d (want 0)' % (f, lam_ours, len(lam), scene_ours, len(left)))
            fails += not ok
        rc, vt = run([exe, '--capture', cap, '--frame', '300', '--frames', '8', '--validation', '1', '--replay'] + on, 900)
        ok = rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt
        print(('PASS' if ok else 'FAIL') + '  validation: 8-frame replay with the layers on, errors 0 (exit %d)' % rc)
        if not ok:
            print(vt[-1500:])
        fails += not ok
        if not args.no_plants and not args.fault:
            for name, flag, which in (('proxy_wrong', '--ring-ours', 'ring'), ('scene_wrong', '--props-ours', 'props')):
                one = [flag, '--game', game]   # one class at a time: --replace-fault names are parsed by every class that is on
                clean = (ring_re if which == 'ring' else props_re).search(arm('one_' + which, 300, one)[1])
                _, t = arm('f_' + name, 300, one + ['--replace-fault', name])
                m = (ring_re if which == 'ring' else props_re).search(t)
                if not (m and clean):
                    print('FAIL  planted %s: census missing' % name)
                    fails += 1
                    continue
                rep0, rep1, mis1 = int(clean.group(3)), int(m.group(3)), int(m.group(7))
                ok = rep1 < rep0 and (mis1 > 0 or which == 'ring')   # ring: the wrong entry is refused by the Lambert guard (NotOurs), not counted as mismatch
                print(('PASS' if ok else 'FAIL') + '  planted %s (%s) at frame 300: replaced %d -> %d, mismatch 0 -> %d' % (name, which, rep0, rep1, mis1))
                fails += not ok
    except RuntimeError as e:
        return fail(str(e))
    print('wm30_n9_check: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
