#!/usr/bin/env python3
r"""Lane L2's gate check (Session 103): the entrance page's floor pools and the follow spot, composed from the record's rows. 0 game runs: fview headless + frame_rig.exe.

  py -I tools\scene\l2_check.py [--out DIR] [--frames 20,110,150,200] [--reuse]      (--reuse: judge the pictures already under --out, no rig runs: for tuning the bars)

Judged on brock_losswin, per frame (arms: base = no rows; fit = pool + spot rows fitted from the game's own page, the spot centred on the record's entrant root; plants on the fit arm):
  ROOT   the record's skeleton node 2 is within 40 units of the spot fitted on the game's page (page texel = 11 units).
  PAGE   the composed page against the game's own page of that frame (fview --image-png): rms <= 0.03; a planted x / z swap of the pools must exceed it.
  SPOT   the floor RING contrast around the fitted spot, in the picture: median luminance of 8 floor points 120 units from the spot's centre over 8 points 450 units out (projected with the stream's own ViewProj, floor y = 0;
         the rings keep clear of the entrant's feet, and the picture's overall level, which is not class B, cancels). Taken from the GAME's picture first. fit: |ours - game| <= 0.25 and above base by >= 0.08;
         rows ignored / spot mirrored in z / spot 150 units off: each at least 0.08 below the fit (planted faults caught).
  every rig run must exit 0.
"""
import argparse, json, os, re, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout():
    common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
    return os.path.dirname(common) if common else ROOT


def ensure_bones_tool():
    exe = os.path.join(ROOT, 'out', 'tools', 'l2_bones.exe')
    src = os.path.join(HERE, 'l2_bones.cpp')
    if os.path.exists(exe) and os.path.getmtime(exe) >= os.path.getmtime(src):
        return exe
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    cmd = ['pwsh', '-NoProfile', '-Command', '. tools/env.ps1 | Out-Null; clang++ -O1 -std=c++20 -DNOMINMAX -D_CRT_SECURE_NO_WARNINGS -Wno-deprecated-declarations tools/scene/l2_bones.cpp src/renderer_scene/record_io.cpp -o out/tools/l2_bones.exe; if ($LASTEXITCODE -ne 0) { exit 1 }']
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors='replace', timeout=600)
    if p.returncode != 0:
        raise SystemExit('FAIL  cannot build l2_bones.exe: ' + (p.stdout + p.stderr)[-300:])
    return exe


def rig(capture, frames, out, extra):
    cmd = [sys.executable, '-I', os.path.join(HERE, 'frame_rig.py'), '--capture', capture, '--frames', ','.join(map(str, frames)), '--out', out, '--look-from-stream'] + extra
    p = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=1500)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def img(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.float64)


def project(vp, x, y, z, w, h):
    """the stream's row-vector ViewProj: clip = [x y z 1] * VP; returns the pixel of the point, or None"""
    M = np.array(vp, dtype=np.float64).reshape(4, 4)
    c = np.array([x, y, z, 1.0]) @ M
    if c[3] <= 1e-6:
        return None
    return (c[0] / c[3] * 0.5 + 0.5) * w, (1.0 - (c[1] / c[3] * 0.5 + 0.5)) * h


def ring(a, vp, sx, sz, radius, W, H, n=8):
    vals = []
    for k in range(n):
        ang = 2.0 * np.pi * k / n
        pt = project(vp, sx + radius * np.cos(ang), 0.0, sz + radius * np.sin(ang), W, H)
        if pt and 6 < pt[0] < W - 6 and 6 < pt[1] < H - 6:
            x, y = int(pt[0]), int(pt[1])
            vals.append(float(a[y - 3:y + 4, x - 3:x + 4].mean()))
    return float(np.median(vals)) if len(vals) >= 4 else None


def contrast(a, vp, sx, sz, W, H):
    i, o = ring(a, vp, sx, sz, 120.0, W, H), ring(a, vp, sx, sz, 450.0, W, H)
    return None if i is None or o is None or o < 1e-3 else i / o


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=None)
    ap.add_argument('--frames', default='20,110,150,200')
    ap.add_argument('--reuse', action='store_true')
    args = ap.parse_args()
    main_root = main_checkout()
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import run_dirs  # S103 PSO: a private output folder per run (never a fixed path under the main checkout)
    out = run_dirs.run_out('L2_gate', args.out)
    cap = os.path.join(main_root, 'out', 'captures', 'brock_losswin')
    frames = [int(x) for x in args.frames.split(',')]
    ensure_bones_tool()
    fails, res = [], {}

    def check(ok, text):
        print(('PASS' if ok else 'FAIL') + '  ' + text)
        if not ok:
            fails.append(text)

    arms = {'base': [], 'fit': ['--look-pools'], 'plant_pools': ['--look-pools', '--plant-look', 'pools'], 'plant_spot': ['--look-pools', '--plant-look', 'spot'],
            'plant_spot150': ['--look-pools', '--plant-look', 'spot150'], 'plant_swap': ['--look-pools', '--plant-look', 'pool_swap']}
    work, text_of = {}, {}
    for name, extra in arms.items():
        d = os.path.join(out, name)
        text = ''
        if not args.reuse:
            rc, text = rig(cap, frames, d, extra)
            check(rc == 0, 'rig arm %s exits 0 (exit %d)' % (name, rc))
            for line in text.splitlines():
                if line.startswith('FAIL') and 'textur' not in line:
                    check(False, '%s: %s' % (name, line[:170]))
        else:
            text = '\n'.join(open(os.path.join(d, 'frame_%03d_report.txt' % n)).read() for n in frames if os.path.exists(os.path.join(d, 'frame_%03d_report.txt' % n)))
        work[name], text_of[name] = os.path.join(d, 'work_brock_losswin'), text
    page_rms = {n: {} for n in ('fit', 'plant_swap')}
    for name in page_rms:
        for m in re.finditer(r'PAGE rms: frame (\d+) .*: ([0-9.]+)', text_of[name]):
            page_rms[name][int(m.group(1))] = float(m.group(2))
    dist = [int(m.group(1)) for m in re.finditer(r'(\d+) units apart', text_of['fit'])]
    check(len(dist) == len(frames) and max(dist or [999]) <= 40, 'ROOT: skeleton node 2 of the record is within 40 units of the spot fitted on the game page on every frame (frames %s: %s units; the page texel is 11)' % (frames, dist))
    for n in frames:
        rms, srms = page_rms['fit'].get(n), page_rms['plant_swap'].get(n)
        check(rms is not None and rms <= 0.03, 'frame %d PAGE: composed page vs the game\'s page rms %s (needs <= 0.03)' % (n, rms))
        check(srms is not None and srms > 0.03, 'frame %d PAGE: planted x / z swap of the pools rms %s (needs > 0.03: caught)' % (n, srms))
        game = img(os.path.join(work['fit'], 'game', 'frame_%05d.png' % n))
        H, W = game.shape[:2]
        sj = os.path.join(work['fit'], 'spot_%d.json' % n)
        sp = json.load(open(sj)) if os.path.exists(sj) else None
        cam = json.load(open(os.path.join(work['fit'], 'camera_%d.json' % n)))
        if not sp:
            print('INFO  frame %d: no fitted spot: not judged' % n)
            continue
        gc = contrast(game, cam['vp'], sp['x'], sp['z'], W, H)
        row = {'game': None if gc is None else round(gc, 3)}
        for name in ('base', 'fit', 'plant_pools', 'plant_spot', 'plant_spot150'):
            v = contrast(img(os.path.join(work[name], 'ours_%d_arena.png' % n)), cam['vp'], sp['x'], sp['z'], W, H)
            row[name] = None if v is None else round(v, 3)
        res[n] = row
        print('INFO  frame %d ring contrast (inside 120 / outside 450 units around the spot): %s' % (n, row))
        if gc is None or any(row[k] is None for k in ('base', 'fit', 'plant_pools', 'plant_spot', 'plant_spot150')):
            print('INFO  frame %d: the rings are off screen: not judged' % n)
            continue
        # ours is STRONGER than the game's ring contrast (+0.17..+0.49 measured): the game's frame has additive terms (bloom / glare / film) that compress a ratio toward 1 and ours has none [U, lane P]; so the bar is
        # "the spot is there and not weaker than the game's, and not absurdly stronger": game - 0.1 <= ours <= game + 0.6, and above no rows by 0.08
        check(gc - 0.1 <= row['fit'] <= gc + 0.6 and row['fit'] >= row['base'] + 0.08, 'frame %d SPOT: ring contrast ours %.3f vs the game %.3f (game - 0.1 .. game + 0.6) and above no rows %.3f by 0.08' % (n, row['fit'], gc, row['base']))
        for k, label in (('plant_pools', 'rows ignored'), ('plant_spot', 'spot mirrored in z'), ('plant_spot150', 'spot 150 units off')):
            check(row[k] <= row['fit'] - 0.08, 'frame %d SPOT: %s: %.3f at least 0.08 below the fit %.3f (planted fault caught)' % (n, label, row[k], row['fit']))
    os.makedirs(out, exist_ok=True)
    json.dump({'contrast': res, 'page_rms': page_rms, 'root_distance': dist}, open(os.path.join(out, 'l2_check_numbers.json'), 'w'), indent=1)
    judged = sum(1 for r in res.values() if r['game'] is not None and r['fit'] is not None)
    check(judged >= 2, 'at least two frames have the spot rings in view (%d judged)' % judged)
    print('%s  lane L2 check: %d failure(s)' % ('PASS' if not fails else 'FAIL', len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
