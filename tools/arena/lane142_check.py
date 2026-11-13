#!/usr/bin/env python3
r"""Lane 142 offline proof (0 game runs): the arena's picture slots and the wrestlers' model loads, through fview.

  py -I tools\\arena\\lane142_check.py [--root DIR] [--no-build]

Arena (Check B's Brock case zip, frame 340, --arena-ours):
  A1 default rule: texture refusals 0, `picture stays the stream's ... size N` > 0 (the msg_score2 kind), mismatch 0, validation has no errors
  A2 planted `--replace-fault strict_texture` (the old rule): texture refusals > 0, replaced lower than A1 by the same count
  A3 the two pictures differ by at most PIX_OK (50) pixels over 12 luminance (the picture is the stream's image either way); A1 asserts the frame has draws on the new paths, so a 0 is not vacuous
  A5 planted `--replace-fault uv_shift` (the stream's picture kept with shifted uv): "kept for uv" > 0 and replaced lower; the default has "kept for uv" 0
  R1-R4 (C++, out\tools\wrestler_ours_check.exe): --trim-test passes, its planted twin fails; --loader-test (2 pac opens for A B A B A) passes, its arrival-order twin fails
Wrestlers (run 7088's J2 zip, frame 554, --wrestlers-ours, --replace-fault slow_load = a worker's latency simulated, 3 asks per model):
  W1 default: `loading` draws counted; W2 planted `serial_load` (the old one-read-at-a-time start): MORE loading draws than W1
  W3 planted `no_pac_keep` (the old cost: every model load unpacks its pac again): the wrestler hook's `ms` at least 2x W4 (default, no slow_load)
  W4 default, no slow_load: mismatch 0, models read > 0
Missing inputs FAIL, never skip."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
PIX = 12.0
PIX_OK = 50   # (review: the judged frame must hold draws on the new paths - A1 asserts it - and the two pictures must agree to within 50 pixels)


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    fails = 0

    def verdict(ok, text):
        nonlocal fails
        print(('PASS  ' if ok else 'FAIL  ') + text)
        fails += not ok

    brock = os.path.join(main_co, 'out', 's101', 'runs', '20261111_215201_s100_owner_s89_brock')
    j2 = os.path.join(main_co, 'out', 's101', 'runs', '20261112_172550_s100_owner_s90_wm30')
    game = os.path.join(main_co, 'game_data', 'extracted')
    zips_b = [os.path.join(brock, f) for f in os.listdir(brock) if f.endswith('.zip')] if os.path.isdir(brock) else []
    zips_j = [os.path.join(j2, f) for f in os.listdir(j2) if f.endswith('.zip')] if os.path.isdir(j2) else []
    if not zips_b or not zips_j or not os.path.isdir(game):
        print('FAIL  missing input (Check B zip %s, J2 zip %s, game files %s)' % (bool(zips_b), bool(zips_j), os.path.isdir(game)))
        return 1
    try:
        import numpy as np
        from PIL import Image
    except Exception as e:
        print('FAIL  numpy / Pillow: %s' % e)
        return 1
    if not args.no_build:
        rc, text = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1'), '-Release'], 2400)
        if rc:
            print(text[-2000:])
            print('FAIL  fview build')
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='release')
    if not os.path.isfile(exe):
        print('FAIL  no fview.exe at ' + exe)
        return 1
    out = os.path.join(main_co, 'out', 's101', 'lane142')
    os.makedirs(out, exist_ok=True)

    def fv(tag, zip_path, frame, extra):
        d = os.path.join(out, 'chk_' + tag)
        os.makedirs(d, exist_ok=True)
        rc, text = run([exe, '--case', zip_path, '--frame', str(frame), '--judge-out', d] + extra + ['--game', game, '--root', main_co])
        if rc:
            print(text[-1500:])
            raise RuntimeError('fview %s exit %d' % (tag, rc))
        png = os.path.join(d, 'picture.png')
        img = np.asarray(Image.open(png).convert('RGB'), dtype=np.float64) if os.path.isfile(png) else None
        return text, img

    def census(text, cls):
        m = re.search(r'\[rdr\]\[%s\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\).*?ms ([0-9.]+)' % cls, text)
        return m

    lum = np.array([0.3, 0.59, 0.11])
    try:
        zb = zips_b[0]
        ta, ia = fv('a1', zb, 340, ['--arena-ours'])
        tb, ib = fv('a2', zb, 340, ['--arena-ours', '--replace-fault', 'strict_texture'])
        ca, cb = census(ta, 'arena'), census(tb, 'arena')
        if not ca or not cb:
            print('FAIL  no arena census line')
            return 1
        stays = re.search(r'size (\d+), put (\d+)', ta)
        verdict(int(ca.group(9)) == 0 and int(ca.group(7)) == 0 and stays and int(stays.group(1)) > 0,
                'A1 default rule: texture %s, mismatch %s, picture stays the stream\'s (size) %s' % (ca.group(9), ca.group(7), stays and stays.group(1)))
        verdict(int(cb.group(9)) > 0 and int(ca.group(3)) - int(cb.group(3)) == int(cb.group(9)),
                'A2 planted strict_texture (the old rule): texture %s, replaced %s vs %s (the difference is the texture count)' % (cb.group(9), cb.group(3), ca.group(3)))
        d = int((np.abs(ia @ lum - ib @ lum) > PIX).sum())
        verdict(d <= PIX_OK, 'A3 the two pictures differ by %d pixels over %d luminance (bar <= %d)' % (d, PIX, PIX_OK))


        tu, _ = fv('a5', zb, 340, ['--arena-ours', '--replace-fault', 'uv_shift'])
        cu = census(tu, 'arena')
        kd = re.search(r'kept for uv (\d+)', ta)
        ku = re.search(r'kept for uv (\d+)', tu)
        verdict(kd and int(kd.group(1)) == 0 and ku and int(ku.group(1)) > 0 and int(cu.group(3)) < int(ca.group(3)),
                'A5 uv: default kept-for-uv %s; planted uv_shift kept-for-uv %s, replaced %s vs %s' % (kd and kd.group(1), ku and ku.group(1), cu.group(3), ca.group(3)))
        verdict('validation' not in ta.lower() or 'error' not in ta.lower().split('validation')[-1][:200], 'A4 no validation error text in the arena run')

        rc_, tb_ = run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'wrestler', 'build_wrestler_ours_check.ps1')], 1200)
        wo = os.path.join(root, 'out', 'tools', 'wrestler_ours_check.exe')
        if rc_ or not os.path.isfile(wo):
            print(tb_[-1500:])
            print('FAIL  wrestler_ours_check build')
            return 1
        for name, args_, want in (('R1 trim-test', ['--trim-test'], 0), ('R2 trim-test plant (final TrimIdle skipped)', ['--trim-test', '--trim-test-fault'], 1),
                                  ('R3 loader-test (A B A B A B queued behind a held mutex: 2 pac opens)', ['--loader-test'], 0), ('R4 loader-test plant (arrival order)', ['--loader-test-fault'], 1)):
            rcx, tx = run([wo, '--game', game] + args_, 600)
            verdict(rcx == want, '%s: exit %d (want %d): %s' % (name, rcx, want, ' | '.join(l for l in tx.splitlines() if 'test:' in l)[:260]))

        zj = zips_j[0]
        t1, _ = fv('w1', zj, 554, ['--wrestlers-ours', '--replace-fault', 'slow_load'])
        t2, _ = fv('w2', zj, 554, ['--wrestlers-ours', '--replace-fault', 'slow_load,serial_load'])
        t3, _ = fv('w3', zj, 554, ['--wrestlers-ours', '--replace-fault', 'no_pac_keep'])
        t4, _ = fv('w4', zj, 554, ['--wrestlers-ours'])
        l = lambda t: int(re.search(r'no link \d+, loading (\d+)', t).group(1))
        c1, c3, c4 = census(t1, 'wrestlers'), census(t3, 'wrestlers'), census(t4, 'wrestlers')
        verdict(l(t1) > 0 and int(c1.group(7)) == 0, 'W1 slow_load, all candidates started together: loading %d draws, mismatch %s' % (l(t1), c1.group(7)))
        verdict(l(t2) > l(t1), 'W2 planted serial_load (one read per frame): loading %d > %d' % (l(t2), l(t1)))
        ms3, ms4 = float(c3.group(12)), float(c4.group(12))
        verdict(ms3 >= 2.0 * ms4, 'W3 planted no_pac_keep: hook ms %.0f vs %.0f with the pac kept (bar >= 2x)' % (ms3, ms4))
        verdict(int(c4.group(7)) == 0 and 'models read 0,' not in t4, 'W4 default: mismatch %s, loading %d' % (c4.group(7), l(t4)))
    except Exception as e:
        print('FAIL  %s' % e)
        return 1
    print('lane142_check: %s' % ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
