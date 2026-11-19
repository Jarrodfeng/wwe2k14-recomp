"""S103 CMB: the offline proof of the host attire composer (src\\assets\\attire_compose.*), 0 game runs.

    py -I tools\\assets\\attire_compose_check.py [--no-build] [--keep]

Reads the player's own files read-only (tools\\assets\\attire_compose_dump.py -> out\\cmb\\) and the main checkout's capture out\\captures\\s100_heenan_build.
Checks (each prints PASS / FAIL with its number), then plants each fault from the CMP adversary's "Exact checks for the builder" 4 and requires that at
least the named check FAILS:
  U  unit: the hook read on a fake guest memory (every refusal once, mode 0 / 2 stay the base, set 0 vs 1 bytes, the tag, the registry)
  A  Heenan ch249 PACH 24902 + slot 1 colours composed with the captured replay's nibble order vs the capture's own atlas (image 1648 -> 1640,
     fview --surface 1640): mean abs difference <= 1 / 255 (the replay draws the file's masks with the nibbles rotated: R 0xF000 G 0x000F B 0x00F0
     A 0x0F00; everything else in the build is compared: base, colours, k, value / 15, 255, alpha)
  B  the same inputs with the masks' own DDS header order vs tools\\assets\\attire_build.py (an independent implementation): mean <= 1
  C  the header-order jacket is black (box mean < 70 where the base is light, > 150): the S100 / Jarrod look
  D  k = entry - 13: a PACH without entry 13 composes like the same PACH with an all-zero entry 13
  E  Jericho ch104 slot 1: a colour of 255 paints white (R == G == B on every texel that differs from "keep"), >= 5000 texels differ
Planted (each must FAIL its check): swap_rb (A, B, C), rotl4 (A, B, C), slot0 / wrong-offset colours (A), enumerate (D), white_keep_default (E),
compose_mode0 / compose_mode2 (U).
Exit 0 = every check passes and every planted fault is caught.
"""
import argparse
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'wm30'))
import attire_build as ab  # noqa: E402

EXE = os.path.join(REPO, 'out', 'tools', 'attire_compose_check.exe')
WORK = os.path.join(REPO, 'out', 'cmb')
MAIN = os.environ.get('WWE_MAIN', r'C:\Users\Jarrod\wwe2k14-recomp')
CAPTURE = os.path.join(MAIN, 'out', 'captures', 's100_heenan_build')
REPLAY_MASKS = 'F000,000F,00F0,0F00'
W, H = 2048, 1024


def run(*args):
    return subprocess.run([EXE, *map(str, args)], capture_output=True, text=True)


def compose(indir, name, *flags):
    out = os.path.join(WORK, name + '.rgba')
    r = run('compose', indir, out, *flags)
    if r.returncode != 0:
        raise SystemExit('compose failed: ' + r.stdout + r.stderr)
    return np.fromfile(out, np.uint8, W * H * 4).reshape(H, W, 4)


def dump(cid, slot, name, *extra):
    d = os.path.join(WORK, name)
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'attire_compose_dump.py'), '--id', str(cid), '--slot', str(slot), '--out', d, *extra],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit('dump failed: ' + r.stdout + r.stderr)
    return d


def reference_atlas():
    """The capture's atlas after the build: fview --surface 1640 --save-case at frame 30 (the replay's own picture), RGB."""
    p = os.path.join(WORK, 'ref_heenan_atlas.png')
    if not os.path.exists(p):
        fv = os.path.join(MAIN, 'out', 'renderer', 'dev', 'fview.exe')
        r = subprocess.run([fv, '--capture', CAPTURE, '--root', MAIN, '--frame', '30', '--pixel', '1000,500', '--surface', '1640', '--save-case'], capture_output=True, text=True)
        case = [ln.split('case saved:')[1].strip() for ln in r.stdout.splitlines() if 'case saved:' in ln]
        if not case:
            raise SystemExit('fview gave no case: ' + r.stdout[-400:])
        Image.open(os.path.join(case[0], 'picture.png')).save(p)
    return np.asarray(Image.open(p).convert('RGB')).astype(np.float32)


def mean_diff(a, b):
    return float(np.abs(a[..., :3].astype(np.float32) - b[..., :3].astype(np.float32)).mean())


def checks(plant, inputs, ref, pytool):
    """{id: (ok, detail)} for one run; `plant` = the fault name or None."""
    flt = ['--fault', plant] if plant in ('swap_rb', 'rotl4', 'enumerate', 'white_keep_default') else []
    res = {}
    u = run('unit', *(['--fault', plant] if plant in ('compose_mode0', 'compose_mode2') else []))
    res['U unit (hook read)'] = (u.returncode == 0, (u.stdout.strip().splitlines() or ['?'])[-1])
    heen = inputs['wrong'] if plant == 'slot0' else inputs['heenan']
    a = compose(heen, 'A', '--masks', REPLAY_MASKS, *flt)
    d = mean_diff(a, ref)
    res['A vs capture replay (<= 1)'] = (d <= 1.0, '%.3f' % d)
    b = compose(heen, 'B', *flt)
    d = mean_diff(b, pytool)
    res['B vs attire_build.py (<= 1)'] = (d <= 1.0, '%.3f' % d)
    box = (slice(150, 250), slice(820, 940))   # the jacket's left front (no emblem), 2048x1024 atlas
    jacket = float(b[box][..., :3].mean())
    res['C jacket black (< 70)'] = (jacket < 70, 'jacket box mean %.1f' % jacket)
    # D: no entry 13 vs a zero entry 13
    dropped = compose(heen, 'D1', '--drop', 13, *flt)
    zeroed = compose(heen, 'D2', '--zero', 13)
    d = mean_diff(dropped, zeroed)
    res['D k = entry - 13 (== 0)'] = (d < 0.01, '%.3f' % d)
    # E: Jericho: 255 paints white
    jer = inputs['jericho']
    paint = compose(jer, 'E1', *flt)
    keep = compose(jer, 'E2', '--white', 'keep')
    diff = (paint[..., :3] != keep[..., :3]).any(-1)
    n = int(diff.sum())
    grey = bool(((paint[diff][:, 0] == paint[diff][:, 1]) & (paint[diff][:, 1] == paint[diff][:, 2])).all()) if n else False
    res['E 255 paints white (>= 5000 texels, grey)'] = (n >= 5000 and grey, '%d texels differ, all grey %s' % (n, grey))
    # F: the BC1 form (the default output) read back vs the RGBA8 form, and its size
    bc = compose(heen, 'F', '--bc1', *flt)
    d = mean_diff(bc, b)
    sz = run('compose', heen, os.path.join(WORK, 'F2.rgba'), '--bc1').stdout.split()
    res['F BC1 vs RGBA8 (<= 3)'] = (d <= 3.0, '%.3f, %s bytes with mips (RGBA8: 10.7 MB)' % (d, sz[-1] if sz else '?'))
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--no-build', action='store_true')
    a = ap.parse_args()
    os.makedirs(WORK, exist_ok=True)
    if not a.no_build:
        r = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(HERE, 'build_attire_compose_check.ps1')], capture_output=True, text=True)
        if r.returncode != 0:
            print(r.stdout[-1500:], r.stderr[-800:])
            return 2
    inputs = {'heenan': dump(249, 1, 'in_249_1'), 'wrong': dump(249, 1, 'in_249_wrong', '--raw-offset', '244'), 'jericho': dump(104, 1, 'in_104_1')}
    ref = reference_atlas()
    table = ab.colour_table()
    entries = ab.first_archive(249)
    atlas, _ = ab.build(249, table[249][1], entries, 'paint')
    pytool = (atlas.clip(0, 1) * 255 + 0.5).astype(np.float32)
    bad = 0
    base = checks(None, inputs, ref, pytool)
    print('== real inputs, no fault ==')
    for k, (ok, det) in base.items():
        print('  %s  %s  (%s)' % ('PASS' if ok else 'FAIL', k, det))
        bad += not ok
    planted = {'swap_rb': ('A', 'B', 'C'), 'rotl4': ('A', 'B', 'C'), 'slot0': ('A',), 'enumerate': ('D',), 'white_keep_default': ('E',),
               'compose_mode0': ('U',), 'compose_mode2': ('U',)}
    print('== planted faults (each named check must FAIL) ==')
    for name, must in planted.items():
        res = checks(name, inputs, ref, pytool)
        failed = [k for k, (ok, _) in res.items() if not ok]
        caught = all(any(f.startswith(m + ' ') for f in failed) for m in must)
        print('  %s  %-18s fails: %s' % ('CAUGHT' if caught else 'MISSED', name, '; '.join('%s [%s]' % (f, res[f][1]) for f in failed) or 'nothing'))
        bad += not caught
    print('RESULT: %s' % ('ok' if not bad else '%d problem(s)' % bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
