#!/usr/bin/env python3
"""Lane 35 acceptance test (offline, 0 game runs): the crowd drawn by us inside the game's frame, through the REAL binder path (fview --crowd-ours).

  py -I tools\\crowd\\crowd_ours_check.py [--root DIR] [--frame 200] [--no-build]

Material: Check B's run 7051 case zip (out\\s101\\runs\\20261111_215201_s100_owner_s89_brock\\case_*.zip of the MAIN checkout: it carries capture\\scene\\; held, never tidied).
A missing input FAILS (it never skips). Arms (each one fview process, one frame picture): stream (switch off), clean (--crowd-ours), and planted faults.
What stays on the stream in this slice: the far cards (the game's own 64x64 picture of the person on a quad, its own program, alpha test and light constants) and the floor / sign people;
ours replaces the stands members' blocks.
Bar:
  1. clean: ours frames > 0, fallback <= 2 (the first frame has no previous record), stream draws dropped > 0, members drawn > 0, validation errors 0 (8-frame --validation 1 replay).
  2. pairing (record j-1 for frame j): every member's world equals a stream crowd draw's c76-c78 bit for bit, AND the members drawn equal the stream crowd draws dropped (the count
     equality: an off-by-one still matches ~90% of worlds, it does not keep the counts equal), and no frame was counted as badly paired.
  3. geometry + pose: every (class, LOD) group's triangle count (non-degenerate, strip normalised) and vertex count EQUAL the stream's, and the posed positions equal the stream's captured blob
     (lane 36's stride-28 blob). Planted double_mesh and lod0_palette (LOD 1/2 members posed as the LOD 0 body with the LOD 0 palette: the poses that differ) must FAIL it.
  4a. offset and no_cards must be CAUGHT by the exact pairing + count check (a bad frame returns 0 from Inject: it falls back to the stream, counted), not by the picture.
  4. the picture: pixels differing from the stream by more than 12 (luminance) <= CLEAN_PX; every picture fault (bind_pose, identity_vp, cards_as_bodies: the first design,
     the "several outfits overlaid" look and darker cards) must exceed FAULT_PX (so each is a visible, caught fault, and its distance from the stream is above the clean bar).
  5. switch off: the stream arm's picture md5 equals BASE_STREAM_MD5, the md5 of the picture the base (before this lane) drew for this capture and frame on the machine it was recorded on
     (RTX 4090, this fview). A different GPU or driver may draw other bits: then re-record it from a build of the base and say so; the check FAILS on a mismatch, it never skips.
  6. the sheet out\\s101\\crowd\\ours_vs_stream.png (stream | ours | difference x4), every panel labelled, the reference named, and a 2x zoom crop out\\s101\\crowd\\crop_zoom_ours.png.
A check that cannot read its input fails loudly."""
import argparse
import hashlib
import os
import re
import subprocess
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))

# The md5 of the picture the BASE (the s101-renderer-arc commit 9627608e, before this lane) draws for frame 200 of Check B's 7051 capture with the switch off, on the machine it was recorded on:
# RTX 4090, NVIDIA driver 93D28000, Vulkan 1.4.325. Another GPU or driver can draw other bits. To re-record: make a worktree at the base commit, tools\\fview\\build.ps1, then
#   out\\renderer\\dev\\fview.exe --capture <out\\s101\\crowd\\case_brock\\capture> --frame 200 --judge-out D   and md5 D\\picture.png; put it here and say so in the hand-back.
# lane 170 (2026-11-13): re-pinned. The 16-bit target rule (CARRY_DARKENING.md section 12) changed the wrestlers' shadow-map shading on this frame: the old pin
# 8415eafec496acc3b0a518ec54fe94ab is reproduced to the bit by the old rule (FX_T16_DIVISOR=64 + --apply-reload yShadowMap); the new picture differs in 1541 pixels, all on or
# within 8 px of a wrestler (115 over 10/255), mean luma change in the wrestler mask -0.12; nothing else moved.
BASE_STREAM_MD5 = '6111aa4ea5d22ebdcfb0d17c72133ee2'
CLEAN_PX = 6000       # pixels (of 921600) whose luminance differs from the stream by more than 12, clean
FAULT_PX = 9000       # a planted picture fault must exceed this
PIX = 12.0


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=900):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def fail(msg):
    print('FAIL  ' + msg)
    return 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--frame', type=int, default=200)
    ap.add_argument('--no-build', action='store_true')
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
        return fail('input missing: Check B\'s case zip under ' + runs)
    if not os.path.isdir(game):
        return fail('input missing: the game files ' + game)
    out = os.path.join(root, 'out', 's101', 'crowd')
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

    def arm(name, extra):
        d = os.path.join(out, 'arm_' + name)
        rc, text = run([exe, '--capture', cap, '--frame', str(args.frame), '--judge-out', d] + extra)
        png = os.path.join(d, 'picture.png')
        if rc or not os.path.isfile(png):
            print(text[-2000:])
            raise RuntimeError('arm %s failed (exit %d)' % (name, rc))
        return np.asarray(Image.open(png).convert('RGB'), dtype=np.float64), text, png

    lum = np.array([0.3, 0.59, 0.11])
    L = lambda a: a @ lum
    px_diff = lambda a, b: int((np.abs(L(a) - L(b)) > PIX).sum())
    fails = 0
    kept_re = re.compile(r'kept for members ours could not draw: (\d+)')
    census = re.compile(r'ours frames (\d+), fallback (\d+), stream draws dropped (\d+), members drawn (\d+)')
    pairing = re.compile(r'pairing (\d+) of (\d+) worlds match the stream \((\d+) stream crowd draws dropped\), (\d+) frames with a bad pairing, (\d+) of (\d+) \(class, LOD\) checks: triangles \+ vertices equal the stream.s, and (\d+) posed vertices match')
    try:
        stream, _, stream_png = arm('stream', [])
        clean, text, clean_png = arm('clean', ['--crowd-ours', '--game', game])
        faults = {n: arm(n, ['--crowd-ours', '--crowd-fault', n, '--game', game]) for n in ('bind_pose', 'identity_vp', 'no_cards', 'offset', 'double_mesh', 'cards_as_bodies', 'lod0_palette', 'undrawable_type')}
    except RuntimeError as e:
        return fail(str(e))

    m, p = census.search(text), pairing.search(text)
    if not m or not p:
        print(text[-1500:])
        return fail('no census / pairing line in the clean arm')
    ours, fb, dropped, members = map(int, m.groups())
    pm, pt, sd, bad, tm, tt, tp = map(int, p.groups())
    ok = ours > 0 and fb <= 2 and dropped > 0 and members > 0
    print(('PASS' if ok else 'FAIL') + '  clean: ours frames %d, fallback %d, stream draws dropped %d, members drawn %d' % (ours, fb, dropped, members))
    fails += not ok
    ok = pt > 0 and pm == pt and pt == sd and bad == 0
    kept = int(kept_re.search(text).group(1)) if kept_re.search(text) else -1
    print(('PASS' if ok else 'FAIL') + '  pairing: %d of %d member worlds equal a stream crowd draw\'s c76-c78 bit for bit; members %d == stream crowd draws dropped %d; bad-pairing frames %d' % (pm, pt, pt, sd, bad))
    fails += not ok
    ok = kept >= 0 and dropped + kept == sd and dropped == members
    print(('PASS' if ok else 'FAIL') + '  draw accounting (binder side): stream crowd draws dropped %d + kept %d == the %d stream block draws seen by the hook; dropped == members drawn %d (density 100)' % (dropped, kept, sd, members))
    fails += not ok
    ok = tt > 0 and tm == tt and tp == tt
    print(('PASS' if ok else 'FAIL') + '  geometry + pose: %d of %d (class, LOD) groups have exactly the stream triangle count (strip normalised) and vertex count; %d have posed vertices equal to the stream blob (mean residual <= 1e-3, max <= 1e-2 units)' % (tm, tt, tp))
    fails += not ok
    d_clean = px_diff(clean, stream)
    ok = d_clean <= CLEAN_PX
    print(('PASS' if ok else 'FAIL') + '  clean vs stream: %d pixels differ by > %d luminance (bar <= %d)' % (d_clean, PIX, CLEAN_PX))
    fails += not ok

    for n in ('bind_pose', 'identity_vp', 'cards_as_bodies'):
        d = px_diff(faults[n][0], stream)
        ok = d > FAULT_PX
        print(('PASS' if ok else 'FAIL') + '  planted %s: %d pixels differ from the stream (must exceed %d, above the clean bar %d)' % (n, d, FAULT_PX, CLEAN_PX))
        fails += not ok
    for n, what in (('offset', 'record j, not j-1'), ('no_cards', 'the stream cards dropped and not drawn')):
        po, cn = pairing.search(faults[n][1]), census.search(faults[n][1])
        ok = bool(po) and bool(cn) and int(po.group(4)) > 0 and int(cn.group(2)) > fb and (int(po.group(1)) < int(po.group(2)) or int(po.group(2)) != int(po.group(3)))
        print(('PASS' if ok else 'FAIL') + '  planted %s (%s): ' % (n, what) + (('worlds %s of %s match, members %s vs stream draws %s, bad-pairing frames %s, fallback frames %s' % (po.group(1), po.group(2), po.group(2), po.group(3), po.group(4), cn.group(2))) if po and cn else 'unreadable') + ' (must be caught: those frames fall back to the stream)')
        fails += not ok
    pb = pairing.search(faults['bind_pose'][1])
    ok = bool(pb) and int(pb.group(5)) == int(pb.group(6)) and int(pb.group(7)) < int(pb.group(6))
    print(('PASS' if ok else 'FAIL') + '  planted bind_pose: geometry %s of %s passes but pose %s of %s fails (only the pose check sees it)' % (pb.group(5) if pb else '?', pb.group(6) if pb else '?', pb.group(7) if pb else '?', pb.group(6) if pb else '?'))
    fails += not ok
    cu, ku, pu = census.search(faults['undrawable_type'][1]), kept_re.search(faults['undrawable_type'][1]), pairing.search(faults['undrawable_type'][1])
    ok = bool(cu) and bool(ku) and bool(pu) and int(ku.group(1)) > 0 and int(cu.group(3)) + int(ku.group(1)) == int(pu.group(3)) and int(pu.group(4)) == 0 and px_diff(faults['undrawable_type'][0], stream) <= CLEAN_PX
    print(('PASS' if ok else 'FAIL') + '  planted undrawable_type (the first draw\'s type has no body): ' + (('kept %s stream draws, dropped %s, hook saw %s, bad-pairing frames %s, picture %d px from the stream (no hole)' % (ku.group(1), cu.group(3), pu.group(3), pu.group(4), px_diff(faults['undrawable_type'][0], stream))) if cu and ku and pu else 'unreadable') + ' (kept draws are drawn the stream way, the accounting holds)')
    fails += not ok
    for n, what in (('double_mesh', 'every mesh drawn twice'), ('lod0_palette', 'LOD 1/2 members as the LOD 0 body with the LOD 0 palette: poses that differ')):
        pd = pairing.search(faults[n][1])
        ok = bool(pd) and (int(pd.group(5)) < int(pd.group(6)) or int(pd.group(7)) < int(pd.group(6)))
        print(('PASS' if ok else 'FAIL') + '  planted %s (%s): geometry %s of %s, pose %s (must fail)' % (n, what, pd.group(5) if pd else '?', pd.group(6) if pd else '?', pd.group(7) if pd else '?'))
        fails += not ok
    md5 = hashlib.md5(open(stream_png, 'rb').read()).hexdigest()
    ok = md5 == BASE_STREAM_MD5
    print(('PASS' if ok else 'FAIL') + '  switch off: the stream arm\'s picture md5 %s %s the base\'s (%s)' % (md5, '==' if ok else '!=', BASE_STREAM_MD5))
    fails += not ok

    rc, vt = run([exe, '--capture', cap, '--crowd-ours', '--game', game, '--frames', '8', '--validation', '1', '--replay'])
    ok = rc == 0 and 'validation errors 0' in vt and 'validation active: yes' in vt
    print(('PASS' if ok else 'FAIL') + '  validation: 8-frame replay with the layer on, errors 0 (exit %d)' % rc)
    if not ok:
        print(vt[-1500:])
    fails += not ok

    def panel(a, title):
        im = Image.fromarray(np.clip(a, 0, 255).astype(np.uint8)).crop((0, 0, 1280, 460))
        out_im = Image.new('RGB', (1280, 490), (20, 20, 20))
        out_im.paste(im, (0, 30))
        ImageDraw.Draw(out_im).text((8, 8), title, fill=(255, 255, 255))
        return out_im
    diff = np.clip(np.abs(L(clean) - L(stream))[:, :, None].repeat(3, 2) * 4.0, 0, 255)
    sheet = Image.new('RGB', (1280, 490 * 3), (0, 0, 0))
    for i, (a, t) in enumerate([(stream, 'STREAM (reference: the game\'s own block / card draws, Check B run 7051 frame %d, switch off)' % args.frame),
                                (clean, 'OURS (--crowd-ours: stands members drawn from scene record %d, the stream\'s block draws dropped; far cards stay the stream\'s)' % (args.frame - 1)),
                                (diff, 'DIFFERENCE |ours - stream| x4 (luminance); %d pixels differ by > %d' % (d_clean, PIX))]):
        sheet.paste(panel(a, t), (0, 490 * i))
    sheet.save(os.path.join(out, 'ours_vs_stream.png'))
    box = (300, 200, 620, 380)
    zoom = Image.new('RGB', (1280, 2 * 360 + 40), (0, 0, 0))
    first = Image.fromarray(np.clip(stream, 0, 255).astype(np.uint8)).crop(box).resize((640, 360), Image.LANCZOS)
    second = Image.fromarray(np.clip(clean, 0, 255).astype(np.uint8)).crop(box).resize((640, 360), Image.LANCZOS)
    old = Image.fromarray(np.clip(faults['cards_as_bodies'][0], 0, 255).astype(np.uint8)).crop(box).resize((640, 360), Image.LANCZOS)
    zoom = Image.new('RGB', (1920, 390), (0, 0, 0))
    for i, (im, t) in enumerate([(first, 'STREAM (reference), 2x'), (second, 'OURS (clean), 2x'), (old, 'OLD DESIGN (cards as bodies: the ghosting fault), 2x')]):
        zoom.paste(im, (640 * i, 30))
        ImageDraw.Draw(zoom).text((640 * i + 8, 8), t, fill=(255, 255, 255))
    zoom.save(os.path.join(out, 'crop_zoom_ours.png'))
    print('sheet: ' + os.path.join(out, 'ours_vs_stream.png') + '  zoom: ' + os.path.join(out, 'crop_zoom_ours.png'))
    print('crowd_ours: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
