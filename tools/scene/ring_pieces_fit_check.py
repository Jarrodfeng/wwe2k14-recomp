#!/usr/bin/env python3
"""S103 lane C4 check: the SHIPPED cover and post rules (src\\scene_live\\ring_recompute_core.h CoverNodes / PostNodes) against the GAME's own RingRows in run 16's records (Lesnar vs Bryan, mode 1,
--wwe2k14_rdr_ring_d3=1). Covers: the grid records are derived from the game's rope rows of the same record, the cover model's inverse bind from the first record only (tools\\scene\\ring_pieces_fit.cpp):
every cover instance of every sampled record must be within 2e-3 of the game's rows. Posts: every instance within 1e-5 (no input but the image constants). Posts and canvas rows identical in every record;
skirt rows differ record to record. Planted twins (side + 1, angle base pi/2, post step 0) must be caught. Reads the case zip READ ONLY; 0 game runs.
  py -I tools\\scene\\ring_pieces_fit_check.py [--root DIR] [--zip PATH] [--every N]"""
import argparse
import os
import re
import subprocess
import sys
import zipfile

CLANG = r'C:\Program Files\LLVM\bin\clang++.exe'
ZIP = r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\out\cases\case_20261116_023239_766_0_pc_run_exit.zip'
RX = re.compile(r'records (\d+): covers (\d+) instances, (\d+) off \(worst ([0-9.e+-]+)\); posts (\d+) instances, (\d+) off \(worst ([0-9.e+-]+)\); posts changed in (\d+) records, canvas changed in (\d+) records, skirts changed in (\d+) of (\d+)')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--zip', default=ZIP)
    ap.add_argument('--every', type=int, default=8)
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    out = os.path.join(root, 'out', 's103', 'c4', 'gate')
    os.makedirs(out, exist_ok=True)
    exe = os.path.join(out, 'ring_pieces_fit.exe')
    b = subprocess.run([CLANG, '-std=c++20', '-O1', '-D_CRT_SECURE_NO_WARNINGS', '-fuse-ld=lld', os.path.join(root, 'tools', 'scene', 'ring_pieces_fit.cpp'),
                        os.path.join(root, 'src', 'renderer_scene', 'record_io.cpp'), os.path.join(root, 'src', 'renderer_scene', 'live_scene.cpp'), '-o', exe], capture_output=True, text=True)
    if b.returncode:
        print('FAIL: did not compile\n' + b.stdout + b.stderr)
        return 1
    if not os.path.isfile(a.zip):
        print('FAIL: the run 16 case zip is missing: ' + a.zip)
        return 1
    z = zipfile.ZipFile(a.zip)
    names = sorted(n for n in z.namelist() if n.endswith('.rsc'))[::a.every]
    files = []
    for n in names:
        p = os.path.join(out, os.path.basename(n))
        if not os.path.isfile(p):
            with open(p, 'wb') as fh:
                fh.write(z.read(n))
        files.append(p)

    def run(plant=None):
        args = [exe] + (['--plant', plant] if plant else []) + files
        r = subprocess.run(args, capture_output=True, text=True)
        m = RX.search(r.stdout)
        if not m:
            print('FAIL: no summary' + (' (plant %s)' % plant if plant else '') + '\n' + r.stdout + r.stderr)
            return None
        return dict(zip(('recs', 'cv', 'cv_off', 'cv_worst', 'po', 'po_off', 'po_worst', 'po_chg', 'canvas_chg', 'sk_chg', 'sk_recs'), (int(m.group(i)) if i not in (4, 7) else float(m.group(i)) for i in range(1, 12))))
    bad = 0
    base = run()
    if base is None:
        return 1
    print('records %d: covers %d instances, %d off (worst %.6g); posts %d instances, %d off (worst %.3g); posts changed %d, canvas changed %d, skirts changed in %d of %d consecutive records' %
          (base['recs'], base['cv'], base['cv_off'], base['cv_worst'], base['po'], base['po_off'], base['po_worst'], base['po_chg'], base['canvas_chg'], base['sk_chg'], base['sk_recs']))
    if base['recs'] < 20 or base['cv_off'] or base['cv_worst'] > 2e-3:
        print('FAIL: the cover rule does not reproduce the game\'s cover rows')
        bad += 1
    if base['po_off'] or base['po_worst'] > 1e-5:
        print('FAIL: the post rule does not reproduce the game\'s post rows')
        bad += 1
    if base['po_chg'] or base['canvas_chg']:
        print('FAIL: posts or canvas changed between records (they are meant to be static)')
        bad += 1
    if base['sk_chg'] < base['sk_recs'] * 0.9:
        print('FAIL: the skirts do not move record to record (the reader-not-static claim)')
        bad += 1
    for plant, key in (('side_off', 'cv_off'), ('ang_base', 'cv_off'), ('post_step', 'po_off')):
        r = run(plant)
        ok = r is not None and r[key] > 0
        print('twin %-10s %s (%s %s)' % (plant, 'caught' if ok else 'NOT CAUGHT', key, r[key] if r else '?'))
        bad += 0 if ok else 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
