#!/usr/bin/env python3
r"""S103 lane P1: the arena's padded barricade (ar_fence / ar_fence_b: slots 35 / 36 / 38 / 39, meshes 0 and 1) drawn from the scene record must be OPAQUE where the game draws it.

  py -I tools\scene\pad_check.py --panel PATH_ours_arena.png --camera PATH_camera_N.json [--arena 4] [--expect-fail TOKEN] [--min-px N] [--dark T]

The game draw (run 26 stream, op 522 of frame 600, `fview --save-case`): ps yBG_Default_specular_alpha_spec_fragment (the diffuse alpha is the highlight mask, NOT the output alpha), alpha test
GEQUAL 1/255, blend SRC_ALPHA (output alpha 1). So the game keeps every pad texel with alpha >= 1/255; the padding texture's alpha is 13 % on average (99 % of its texels are < 0.5), so a 0.5 test
(what we drew) keeps only the seams and shows the crowd / stands through the rest.
ORACLE (independent of the drawer): the fence meshes are read from the player's own pac (tools\arena\arena_files.py), rasterised with the frame's own ViewProj (the stream's c56-c59, the rig's
camera_N.json), the texture alpha sampled per pixel, the nearest pad triangle per pixel. KEEP = pixels the GAME keeps (alpha >= 1/255); CUT = KEEP minus the pixels a 0.5 test also keeps.
CHECK: over the KEEP pixels our ARENA-family panel must BE the pad: its luminance follows the pad texture's (Pearson r >= 0.5; the lit pad = albedo x a smooth light, ~2) and the light factor is even
(80 % within x0.5..x2 of the median). A cut-out pad shows what is behind it (r 0.17, the factor spreads 0.08..2.8; measured on frame 600 of run 26's case).
The game's own pixels are not reproducible offline here (fview's replay of the case leaves the arena out), so this checks the game's per-draw RULE (coverage), not its colour:
[C] rule, [U] colour (lane L4 owns the light). Exit 1 on any FAIL. Numbers only; nothing from the game is written."""
import argparse
import io
import json
import os
import subprocess
import sys
import zipfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
import arena_files as af  # noqa: E402

PAD_SLOTS = (35, 36, 38, 39)   # the barricade's four sides in arena 4; mesh 0 = ar_fence_b, mesh 1 = ar_fence (the padding), mesh 2 = ar_fence2 (the truss)


def tex_rgba(arena, name):
    t = arena.textures[name]
    im = Image.open(io.BytesIO(t.blob))
    im.load()
    return np.asarray(im.convert('RGBA'))


def strips_to_tris(idx):
    tris, run = [], []
    for v in list(idx) + [0xFFFF]:
        if v == 0xFFFF:
            for i in range(2, len(run)):
                a, b, c = run[i - 2], run[i - 1], run[i]
                if a != b and b != c and a != c:
                    tris.append((a, b, c))
            run = []
        else:
            run.append(int(v))
    return tris


def oracle(arena, vp, w, h):
    """-> (keep_game, keep_half, covered, albedo): per pixel arrays (albedo = the texel luminance 0..1); the nearest pad triangle wins (by the clip w)."""
    zbuf = np.full((h, w), np.inf)
    alpha = np.full((h, w), -1.0)
    albedo = np.zeros((h, w))
    for s in PAD_SLOTS:
        for mi in (0, 1):
            m = arena.slots[s][mi]
            if m.samplers.get('sampDiffusePrm') not in ('ar_fence_b', 'ar_fence'):
                continue
            tx = tex_rgba(arena, m.samplers['sampDiffusePrm'])
            th, tw = tx.shape[:2]
            P = np.c_[m.positions.astype(np.float64), np.ones(len(m.positions))] @ vp
            uv = m.uv1   # the diffuse UV is S2 for every mesh (the game, sub_82BD4BE0; BAN3); arena_files.py keeps the file-stream names (uv0 = S1 bytes, uv1 = S2)
            for (a, b, c) in strips_to_tris(m.indices):
                q = P[[a, b, c]]
                if (q[:, 3] <= 0).any():
                    continue
                sx = (q[:, 0] / q[:, 3] * 0.5 + 0.5) * w
                sy = (0.5 - q[:, 1] / q[:, 3] * 0.5) * h
                x0, x1 = int(max(np.floor(sx.min()), 0)), int(min(np.ceil(sx.max()), w - 1))
                y0, y1 = int(max(np.floor(sy.min()), 0)), int(min(np.ceil(sy.max()), h - 1))
                if x1 < x0 or y1 < y0:
                    continue
                X, Y = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
                d = (sy[1] - sy[2]) * (sx[0] - sx[2]) + (sx[2] - sx[1]) * (sy[0] - sy[2])
                if abs(d) < 1e-9:
                    continue
                l0 = ((sy[1] - sy[2]) * (X - sx[2]) + (sx[2] - sx[1]) * (Y - sy[2])) / d
                l1 = ((sy[2] - sy[0]) * (X - sx[2]) + (sx[0] - sx[2]) * (Y - sy[2])) / d
                l2 = 1 - l0 - l1
                inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
                if not inside.any():
                    continue
                z = l0 * q[0, 3] + l1 * q[1, 3] + l2 * q[2, 3]
                u = l0 * uv[a, 0] + l1 * uv[b, 0] + l2 * uv[c, 0]
                v = l0 * uv[a, 1] + l1 * uv[b, 1] + l2 * uv[c, 1]
                tu = np.floor(u * tw).astype(int) % tw
                tv = np.floor(v * th).astype(int) % th
                al = tx[tv, tu, 3] / 255.0
                lu = (tx[tv, tu, :3].astype(np.float64) @ np.array([0.2126, 0.7152, 0.0722])) / 255.0
                sub_z = zbuf[y0:y1 + 1, x0:x1 + 1]
                sub_a = alpha[y0:y1 + 1, x0:x1 + 1]
                sub_l = albedo[y0:y1 + 1, x0:x1 + 1]
                win = inside & (z < sub_z)
                sub_z[win] = z[win]
                sub_a[win] = al[win]
                sub_l[win] = lu[win]
    covered = alpha >= 0
    return covered & (alpha >= 1 / 255.0), covered & (alpha >= 0.5), covered, albedo


def rig_from_case(zip_path, frame, out, plant):
    """Unpack the frames 0..N of the stream, the record of frame N (+ the three shared files) of the case zip under OUT, run the offline rig on it (0 game runs), -> (arena panel, camera json).
    plant = the old behaviour: WWE_RDR_P1_PLANT=old_alpha makes the loader skip Material::alpha_ref_low (a 0.5 test on the pad)."""
    cap = os.path.join(out, 'capture')
    want = ['capture/images.bin', 'capture/geometry.bin', 'capture/textures.bin', 'capture/scene/frame_%06d.rsc' % frame] + ['capture/frames/frame_%06d.bin' % k for k in range(frame + 1)]  # the replay runs the stream from frame 0
    with zipfile.ZipFile(zip_path) as z:
        for nm in want:
            dst = os.path.join(out, nm)
            if not os.path.exists(dst) or os.path.getsize(dst) != z.getinfo(nm).file_size:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with z.open(nm) as src, open(dst, 'wb') as f:
                    while True:
                        b = src.read(1 << 22)
                        if not b:
                            break
                        f.write(b)
    env = dict(os.environ)
    env.setdefault('WWE_FVIEW_ALLOW_STALE', '1')
    if plant:
        env['WWE_RDR_P1_PLANT'] = 'old_alpha'
        env['WWE_RDR_D1_OLD'] = '1'   # S103 PLT: the read rule (D1, front_end.cpp RuleFor) now draws every stage-slot mesh, so the old-heuristic faults planted here (WWE_RDR_*_PLANT) are never reached on it; the twin runs the heuristics (WWE_RDR_D1_OLD=1 = the shipped fallback for no table / unsupported pair). docs/research/s103/PLT_plant_twins.md
    else:
        env.pop('WWE_RDR_P1_PLANT', None)
    rig = os.path.join(out, 'rig_plant' if plant else 'rig')
    p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'frame_rig.py'), '--capture', cap, '--frames', str(frame), '--out', rig], capture_output=True, text=True, errors='replace', env=env, timeout=900)
    if p.returncode != 0:
        print((p.stdout + p.stderr)[-1500:])
        raise SystemExit('FAIL  frame_rig failed (exit %d)' % p.returncode)
    work = os.path.join(rig, 'work_capture')
    return os.path.join(work, 'ours_%d_arena.png' % frame), os.path.join(work, 'camera_%d.json' % frame)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--panel')
    ap.add_argument('--camera')
    ap.add_argument('--case', help='run 26 case zip: run the rig on --frame, then check (instead of --panel / --camera)')
    ap.add_argument('--frame', type=int, default=300, help='a frame whose camera sees the barricade (300 / 500 / 600 of run 26 do; 300 replays the least)')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 's103', 'P1', 'gate'))
    ap.add_argument('--plant', action='store_true', help='the old behaviour (a 0.5 test on the pad): the check must FAIL')
    ap.add_argument('--arena', type=int, default=4)
    ap.add_argument('--min-px', type=int, default=3000, help='the CUT set must hold at least this many pixels (the barricade must be in view)')
    ap.add_argument('--corr', type=float, default=0.5, help='the least correlation of our panel luminance with the pad texture luminance')
    ap.add_argument('--share', type=float, default=0.80, help='the least share of pad pixels whose light factor (ours / texture) is within x0.5..x2 of the median')
    ap.add_argument('--expect-fail', default=None, metavar='TOKEN')
    a = ap.parse_args()
    if a.case:
        if not os.path.exists(a.case):   # a worktree has no out\ of its own: the main checkout's (the check's needs are relative to it)
            main = os.environ.get('RDR_MAIN_CHECKOUT') or os.path.join('C:\\', 'Users', 'Jarrod', 'wwe2k14-recomp')
            a.case = os.path.join(main, a.case)
        a.panel, a.camera = rig_from_case(a.case, a.frame, a.out, a.plant)
    if not a.panel or not a.camera:
        ap.error('--panel and --camera, or --case')
    vp = np.array(json.load(open(a.camera))['vp'], dtype=np.float64).reshape(4, 4)
    arena = af.load_arena(a.arena)
    img = np.asarray(Image.open(a.panel).convert('RGB'), dtype=np.float64) / 255.0
    h, w = img.shape[:2]
    keep, half, covered, albedo = oracle(arena, vp, w, h)
    cut = keep & ~half
    lum = img @ np.array([0.2126, 0.7152, 0.0722])
    n = int(keep.sum())
    lines, fail = [], []
    lines.append('pad oracle: covered %d px, the game keeps %d (alpha >= 1/255), a 0.5 test would keep %d (the old drawer dropped %d of them)' % (covered.sum(), n, half.sum(), cut.sum()))
    if cut.sum() < a.min_px:
        fail.append('PAD NOT IN VIEW: %d pixels the old test drops (< %d)' % (cut.sum(), a.min_px))
    else:
        alb = np.maximum(albedo[keep], 1e-3)
        r = float(np.corrcoef(lum[keep], albedo[keep])[0, 1])
        ratio = lum[keep] / alb
        med = float(np.median(ratio))
        within = float(((ratio >= 0.5 * med) & (ratio <= 2.0 * med)).mean())
        lines.append('our arena panel over the pad pixels of the game: correlation with the pad texture %.3f (need >= %.2f), light factor median %.2f, within x0.5..x2 of it %.3f (need >= %.2f)' % (r, a.corr, med, within, a.share))
        if r < a.corr or within < a.share:
            fail.append('PAD MISSING: our frame shows the pad on only part of the pixels the game fills with padding (correlation %.2f, even-light share %.2f): the stands / crowd show through' % (r, within))
    for line in lines:
        print(line)
    for f in fail:
        print('FAIL  ' + f)
    if not fail:
        print('PASS  pad_check')
    bad = bool(fail)
    if a.expect_fail:
        ok = bad and any(a.expect_fail in f for f in fail)
        print(('PASS' if ok else 'FAIL') + '  planted fault %s %s' % (a.expect_fail, 'caught' if ok else 'NOT caught'))
        return 0 if ok else 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
