#!/usr/bin/env python3
"""S103 lane C4 gate check: the ring DRAWN from a record's per-call rows (rf_ring.cpp posed path, ring_pose.h) on run 16's real record (frame 399: Lesnar vs Bryan, mode 1; the game's own rows for covers / turnbuckles /
posts, our recomputed ropes): built and drawn headless with lane C's driver (tools\\scene\\c_driver.cpp, no window, no game run). The checks, on one 960x540 view of the ring:
  1. the report says the classes were drawn from rows (rope 12, cover 12, turnbuckle 12, post 4) and nothing was skipped;
  2. the posed ring is the rest ring to within 2 % of the pixels (the ropes barely move in this match: the pose must not move the pieces: a wrong slot -> node map, a wrong space or a wrong mirror would);
  3. the planted twin "ring_nopose" (every node the identity) must be visibly different (> 1.2 % of the pixels: the four posts pile up at one corner);
  4. the planted twin "ring_skirts" (the game's three skirt entries taken as ours) draws the apron from its 66-node rows and stays within 4 % of the rest ring;
  5. the validation layer and magenta counts are zero.
Reads the case zip READ ONLY (one scene record extracted to out\\s103\\c4\\gate); the game files from the main checkout READ ONLY. 0 game runs.
  py -I tools\\scene\\ring_posed_check.py [--root DIR] [--game DIR] [--zip PATH] [--frame N] [--no-build]"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile

ZIP = r'C:\Users\Jarrod\wwe2k14-recomp\out\build\win-amd64-release\out\cases\case_20261116_023239_766_0_pc_run_exit.zip'


def diff_fraction(pa, pb):
    import numpy as np
    from PIL import Image
    a = np.asarray(Image.open(pa).convert('RGB')).astype(int)
    b = np.asarray(Image.open(pb).convert('RGB')).astype(int)
    d = np.abs(a - b).sum(axis=2)
    return float((d > 30).sum()) / d.size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--game', default=r'C:\Users\Jarrod\wwe2k14-recomp\game_data\extracted')
    ap.add_argument('--zip', default=ZIP)
    ap.add_argument('--frame', type=int, default=399)
    ap.add_argument('--no-build', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if not a.no_build:
        r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'scene', 'build_c_driver.ps1')], cwd=root, capture_output=True, text=True)
        if r.returncode:
            print('FAIL: c_driver did not build\n' + r.stdout[-2000:] + r.stderr[-2000:])
            return 1
    if not os.path.isfile(a.zip):
        print('FAIL: the run 16 case zip is missing: ' + a.zip)
        return 1
    out = os.path.join(root, 'out', 's103', 'c4', 'gate')
    cap = os.path.join(out, 'cap')
    os.makedirs(os.path.join(cap, 'scene'), exist_ok=True)
    name = 'frame_%06d.rsc' % a.frame
    with zipfile.ZipFile(a.zip) as z, open(os.path.join(cap, 'scene', name), 'wb') as fh:
        fh.write(z.read('capture/scene/' + name))
    env = dict(os.environ, WWE_RDR_VALIDATION='1')
    p = subprocess.run([os.path.join(root, 'out', 'tools', 'c_driver.exe'), '--game', a.game, '--capture', cap, '--frame', str(a.frame), '--out', out, '--tag', 'rp', '--cam', '1100,500,0,45'],
                       capture_output=True, text=True, env=env, timeout=600)
    log = p.stdout + p.stderr
    with open(os.path.join(out, 'rp_log.txt'), 'w', encoding='utf-8') as fh:
        fh.write(log)
    if p.returncode:
        print('FAIL: the driver exited %d\n%s' % (p.returncode, log[-1500:]))
        return 1
    bad = 0
    # the "ring" panel's report line (the report follows its panel line)
    m = re.search(r'panel ring crc.*?\n\[c\]\s+Ring: objects (\d+) drawn (\d+) skipped (\d+) why posed, classes rope=(\d+)\(\w+\) cover=(\d+)\(\w+\) turnbuckle=(\d+)\(\w+\)\s*post=(\d+)\(\w+\)', log, re.S)
    if not m:
        print('FAIL: the ring panel did not report a posed ring\n' + '\n'.join(l for l in log.splitlines() if 'Ring' in l)[:800])
        return 1
    drawn, skipped, rope, cover, tb, post = int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)), int(m.group(7))
    print('posed ring: rope %d cover %d turnbuckle %d post %d, drawn %d skipped %d' % (rope, cover, tb, post, drawn, skipped))
    if (rope, cover, tb, post) != (12, 12, 12, 4) or skipped:
        print('FAIL: not every class was drawn from its rows')
        bad += 1
    png = lambda n: os.path.join(out, 'rp_%s.png' % n)
    posed_rest = diff_fraction(png('ring'), png('ring_rest'))
    nopose = diff_fraction(png('ring_nopose'), png('ring'))
    skirts = diff_fraction(png('ring_skirts'), png('ring_rest'))
    print('posed vs rest %.3f %% of pixels differ; planted no_pose vs posed %.3f %%; skirts-from-rows vs rest %.3f %%' % (posed_rest * 100, nopose * 100, skirts * 100))
    if posed_rest > 0.02:
        print('FAIL: the posed ring is not the rest ring (the pose moved the pieces)')
        bad += 1
    if nopose < 0.012:
        print('FAIL: the planted twin (identity palette) is not caught')
        bad += 1
    if skirts > 0.04:
        print('FAIL: the apron posed from its rows is not the rest apron')
        bad += 1
    if not re.search(r'skirt=3\(ours\)', log):
        print('FAIL: the skirt twin did not draw the skirts from rows')
        bad += 1
    for mm in re.finditer(r'panel (\w+) crc \w+ drawn_px \d+ magenta_px (\d+) validation (\d+)', log):
        if mm.group(1).startswith('ring') and (int(mm.group(2)) or int(mm.group(3))):
            print('FAIL: panel %s has magenta %s / validation errors %s' % (mm.group(1), mm.group(2), mm.group(3)))
            bad += 1
    # S103 FOB: the family order on the POSED ring (the game's: canvas, skirts, arena, screens, posts, turnbuckles, covers, ropes, props); the four planted old orders must fail it; the price of each difference is printed
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import fo_order
    import numpy as np
    from PIL import Image
    px = lambda n: np.asarray(Image.open(png(n)).convert('RGB')).astype(int)
    if 'post' not in (fo_order.tokens_of(log, 'whole_plain') or []):
        print('FAIL: the record\'s ring is not posed in whole_plain (the order check needs the posed pieces)')
        bad += 1
    if not fo_order.run(log, lambda n: (int((np.abs(px('whole_plain') - px(n)).max(axis=2) > 12).sum()), int((np.abs(px('whole_plain') - px(n)).max(axis=2) > 0).sum()))):
        bad += 1
    print('RESULT', 'OK' if bad == 0 else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
