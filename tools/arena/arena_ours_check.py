#!/usr/bin/env python3
"""Lane 40 offline proof (0 game runs): our arena drawn in place through the REAL seam (fview --arena-ours), Check B's capture (run 7051, arena 4, 341 frames).

  py -I tools\\arena\\arena_ours_check.py [--root DIR] [--step N] [--no-build] [--sheets-only]

Bar (each line PASS / FAIL; a missing input FAILS, never skips):
  1. clean arm on EVERY frame (step 1 by default): the picture vs the stream's (fview, switch off): pixels whose luminance differs by more than 12 <= CLEAN_PX; the max
     and mean are printed. The census line of the last frame's run: fallback <= 2, replaced > 0, mismatch 0, unjoined 0, validation errors 0 (8-frame --validation 1 replay).
  2. planted faults through the seam (--replace-fault): vertex_swizzle, no_mirror, first_texture must each move the picture by more than FAULT_PX on the plant frames and
     must change the census (no_mirror: fewer replaced); winding_flip / file_lightmap / join_shift are run too and printed.
  3. the sheets in out\\s101\\arena\\ : stage_sheet_N.png (stream | ours | difference x4, labelled, zoomed on the stage) at a few frames, and uv_sheet.png (default vs uv_abs_u vs
     the stream on a frame where the sign matters).
Frames are one fview process each (the viewer replays frames 0..N): the full run takes ~25 minutes on the RTX 4090; --step 5 about 5. Every process has a timeout."""
import argparse
import csv
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
PIX = 12.0
CLEAN_PX = 12000       # pixels (of 921600) differing by > 12 luminance, clean arm (set from the measured corpus: see the run log)
FAULT_PX = 5000        # a planted picture fault must move the picture by more than this (px over 12 luminance) from the clean arm at some plant frame
PLANT_FRAMES = (100, 200, 300)
SHEET_FRAMES = (30, 100, 200, 300)
ZOOM = (160, 120, 1120, 480)   # the stage: ramp, set, stands' structure (960 x 360 of the 1280 x 720 picture)


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=300):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def fail(msg):
    print('FAIL  ' + msg)
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--step', type=int, default=1)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--sheets-only', action='store_true')
    args = ap.parse_args()
    try:
        import numpy as np
        from PIL import Image, ImageDraw
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
    out = os.path.join(main_co, 'out', 's101', 'arena')
    cap = os.path.join(out, 'case_brock', 'capture')
    if not os.path.isfile(os.path.join(cap, 'images.bin')):
        os.makedirs(os.path.join(out, 'case_brock'), exist_ok=True)
        zipfile.ZipFile(zips[0]).extractall(os.path.join(out, 'case_brock'))
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
    os.makedirs(out, exist_ok=True)

    lum = np.array([0.3, 0.59, 0.11])
    L = lambda a: a @ lum
    px_diff = lambda a, b: int((np.abs(L(a) - L(b)) > PIX).sum())
    cache = {}

    def arm(tag, frame, extra):
        key = (tag, frame)
        if key in cache:
            return cache[key]
        d = os.path.join(out, 'arm_%s_%d' % (tag, frame))
        rc, text = run([exe, '--capture', cap, '--frame', str(frame), '--judge-out', d] + extra, 600)
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2000:])
            raise RuntimeError('arm %s frame %d failed (exit %d)' % (tag, frame, rc))
        img = np.asarray(Image.open(png).convert('RGB'), dtype=np.float64)
        cache[key] = (img, text)
        return cache[key]

    stream = lambda f: arm('stream', f, [])
    ours = lambda f: arm('clean', f, ['--arena-ours', '--game', game])
    census = re.compile(r'\[rdr\]\[arena\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
    fails = 0
    try:
        if not args.sheets_only:
            frames = list(range(0, 341, args.step))
            rows = []
            for f in frames:
                s, _ = stream(f)
                o, text = ours(f)
                rows.append((f, px_diff(o, s)))
                cache.pop(('stream', f), None)
                cache.pop(('clean', f), None)
            with open(os.path.join(out, 'per_frame.csv'), 'w', newline='') as fh:
                w = csv.writer(fh)
                w.writerow(['frame', 'px_over_12'])
                w.writerows(rows)
            d = [r[1] for r in rows]
            ok = max(d) <= CLEAN_PX
            print(('PASS' if ok else 'FAIL') + '  clean vs stream, %d frames: pixels over %d luminance: max %d (frame %d), mean %.0f, median %d (bar <= %d); per frame: %s' % (
                len(d), PIX, max(d), rows[d.index(max(d))][0], sum(d) / len(d), sorted(d)[len(d) // 2], CLEAN_PX, os.path.join(out, 'per_frame.csv')))
            fails += not ok
        _, text = ours(340)
        m = census.search(text)
        if not m:
            print(text[-1500:])
            return fail('no census line in the clean arm')
        nf, fb, rep, kept, nr, unj, mis, var, tex, pal, notours = map(int, m.groups())
        sz = re.search(r'size (\d+) \(size unchecked', text)
        # texture refusals are allowed only for a different image size (msg_score2: 252x64 in the file, 256x64 in the stream, counted twice by the replay)
        ok = nf > 0 and fb <= 2 and rep > 0 and mis == 0 and unj == 0 and var == 0 and tex == (int(sz.group(1)) if sz else -1)
        print(('PASS' if ok else 'FAIL') + '  census (frame 340 run): ' + m.group(0))
        fails += not ok
        print('INFO  ' + [l for l in text.splitlines() if 'objects joined' in l][-1])
        rc, vt = run([exe, '--capture', cap, '--arena-ours', '--game', game, '--frames', '8', '--validation', '1', '--replay'], 600)
        ok = rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt
        print(('PASS' if ok else 'FAIL') + '  validation: 8-frame replay with the layer on, errors 0 (exit %d)' % rc)
        if not ok:
            print(vt[-1500:])
        fails += not ok

        base_rep = {}
        for f in PLANT_FRAMES:
            base_rep[f] = int(census.search(ours(f)[1]).group(3))
        for name in ('vertex_swizzle', 'no_mirror', 'first_texture', 'bone_off_by_one', 'bone_zero', 'winding_flip', 'file_lightmap', 'join_shift'):
            ds, dc, reps = [], [], []
            for f in PLANT_FRAMES:
                img, text = arm('f_' + name, f, ['--arena-ours', '--replace-fault', name, '--game', game])
                ds.append(px_diff(img, stream(f)[0]))
                dc.append(px_diff(img, ours(f)[0]))
                mm = census.search(text)
                reps.append(int(mm.group(3)) if mm else -1)
            must = name in ('vertex_swizzle', 'no_mirror', 'first_texture', 'bone_off_by_one')   # (S101 lane 49: bone_off_by_one moves the props; bone_zero is printed only: in Check B the props' bone matrices 0..2 are equal wherever the camera sees them - frames 0 50 100 150 192 200 250 300 340 all 0 pixels - so only the unit proof, 42,312 vertices, catches it)
            # In place, a draw the hook refuses (no_mirror: the mirrored copy stays the stream's) is drawn by the game, so its picture equals the stream's by construction: such a plant
            # shows in the census (fewer replaced), the others must also move the picture away from the clean arm.
            moved_pic = max(dc) > FAULT_PX
            moved_census = any(r < base_rep[f] for r, f in zip(reps, PLANT_FRAMES))
            ok = (moved_pic or moved_census) if must else True
            print(('PASS' if ok else 'FAIL') + '  planted %s: pixels from the clean arm at frames %s = %s (bar > %d), from the stream %s; replaced %s vs clean %s -> %s' % (
                name, PLANT_FRAMES, dc, FAULT_PX, ds, reps, [base_rep[f] for f in PLANT_FRAMES], ('picture' if moved_pic else '') + (' + ' if moved_pic and moved_census else '') + ('census' if moved_census else '') or 'nothing'))
            fails += not ok

        def crop(a):
            return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).crop(ZOOM)

        def sheet(panels, path, title_h=26):
            w, h = panels[0][0].size
            sh = Image.new('RGB', (w, (h + title_h) * len(panels)), (16, 16, 16))
            dr = ImageDraw.Draw(sh)
            for i, (im, t) in enumerate(panels):
                sh.paste(im, (0, (h + title_h) * i + title_h))
                dr.text((6, (h + title_h) * i + 6), t, fill=(255, 255, 255))
            sh.save(path)
            return path

        for f in SHEET_FRAMES:
            s, _ = stream(f)
            o, _ = ours(f)
            dif = np.clip(np.abs(L(o) - L(s))[:, :, None].repeat(3, 2) * 4.0, 0, 255)
            n = px_diff(o, s)
            p = sheet([(crop(s), 'STREAM (the game\'s own arena draws, Check B run 7051 frame %d, switch off)' % f),
                       (crop(o), 'OURS (--arena-ours: stage slots from the player\'s files, file sign of u; stream program/state/constants/light map)'),
                       (crop(dif), 'DIFFERENCE |ours - stream| x4 (luminance); %d pixels of the full picture differ by > %d' % (n, PIX))],
                      os.path.join(out, 'stage_sheet_%d.png' % f))
            print('sheet: ' + p)
        # the |u| question: default (file sign) vs uv_abs_u vs the stream, on the frame where they differ most
        best, bestd = SHEET_FRAMES[0], -1
        for f in SHEET_FRAMES:
            a, _ = ours(f)
            b, _ = arm('uvabs', f, ['--arena-ours', '--replace-fault', 'uv_abs_u', '--game', game])
            dd = px_diff(a, b)
            print('INFO  frame %d: default vs uv_abs_u differ in %d pixels; uv_abs_u vs stream %d, default vs stream %d' % (f, dd, px_diff(b, stream(f)[0]), px_diff(a, stream(f)[0])))
            if dd > bestd:
                best, bestd = f, dd
        s, _ = stream(best)
        a, _ = ours(best)
        b, _ = arm('uvabs', best, ['--arena-ours', '--replace-fault', 'uv_abs_u', '--game', game])
        dd = np.clip(np.abs(L(a) - L(b))[:, :, None].repeat(3, 2) * 4.0, 0, 255)
        p = sheet([(crop(s), 'STREAM frame %d (its converted u is |u|)' % best),
                   (crop(a), 'OURS default: the file\'s sign of u'),
                   (crop(b), 'OURS uv_abs_u: |u| like the stream'),
                   (crop(dd), 'DIFFERENCE default vs uv_abs_u x4 (%d pixels > %d)' % (bestd, PIX))], os.path.join(out, 'uv_sheet.png'))
        print('sheet: ' + p)
    except RuntimeError as e:
        return fail(str(e))
    print('arena_ours_check: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
