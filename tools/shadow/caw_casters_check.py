#!/usr/bin/env python3
"""Lane 225 offline proof, 0 game runs: the Create a Superstar editor's shadow-pass casters and rim-map casters drawn by us (ShadowCore's Editor class, joined through the caw hook).

  py -I tools\\shadow\\caw_casters_check.py [--root DIR] [--zip CASE_ZIP] [--no-build]

Input: the K5w case zip of the editor walk (run 7099, out\\s101\\runs\\20261113_035959_s101_checkB_caw_walk\\case_*.zip; any caw_walk zip works: pass --zip). fview --case ZIP --sweep (every frame in order,
hooks keep their state as live), two arms:
  off  --caw-ours --wrestlers-ours                                       (the state before lane 225: the editor's 23 shadow-map draws and 12 rim-map draws a frame are the stream's)
  on   the same + --shadow-casters-ours --rim-ours --validation 1
Bar (each line PASS / FAIL; a missing input FAILS, never skips):
  1. the picture is byte-equal on EVERY frame (the editor's float noise moves the shadow maps by one mantissa step: the slot-12 images are reported, not required equal);
  2. validation errors 0;
  3. the shadow casters replace 23 draws a frame and the rim map 12 on every frame but the first (the caw hook joins at the colour pass, after the first frame's shadow passes), verify bad 0, mismatch 0;
  4. plants (each must fail its exact check, draws stay the stream's): shadow.editor_exact (byte-for-byte rule: replaced 0, verify bad > 0 = the state before this lane), shadow.bone_zero (the byte check sees it).
"""
import argparse
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
import fview_exe  # noqa: E402  (lane 218: one locator, refuses a stale exe)

SHADOW = re.compile(r'shadow casters: claimed (\d+) replaced (\d+) \(map (\d+), plane (\d+); by class .*?\) \| kept: map not ours (\d+), .*? verify (\d+) bad (\d+),')
SHADOW_C = re.compile(r'\[rdr\]\[shadow_casters\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+)')
RIM = re.compile(r'rim map: claimed (\d+) replaced (\d+) \| kept: not ours (\d+), unjoined (\d+) \| verify (\d+) bad (\d+),')
RIM_C = re.compile(r'\[rdr\]\[rim_map\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+)')
VALID = re.compile(r'validation errors (\d+)')


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def run(cmd, timeout=1500):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, (p.stdout or '') + (p.stderr or '')


def read_csv(path):
    rows = []
    with open(path, encoding='utf-8') as f:
        f.readline()
        for l in f:
            v = l.strip().split(',')
            if len(v) >= 4:
                rows.append(v)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--zip', default='')
    ap.add_argument('--no-build', action='store_true')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    fails = 0

    def line(ok, text):
        nonlocal fails
        print(('PASS  ' if ok else 'FAIL  ') + text)
        fails += 0 if ok else 1

    zips = [args.zip] if args.zip else sorted(glob.glob(os.path.join(main_co, 'out', 's101', 'runs', '20261113_035959_s101_checkB_caw_walk', 'case_*.zip')))
    game = os.path.join(main_co, 'game_data', 'extracted')
    if not zips or not os.path.isfile(zips[0]) or not os.path.isdir(game):
        print('FAIL  input missing: the caw_walk case zip (%s) or the game files (%s)' % (zips[:1], game))
        return 1
    zip_path = zips[0]
    fv = fview_exe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 'l225check')
    os.makedirs(work, exist_ok=True)
    base = [fv, '--case', zip_path, '--game', game, '--caw-ours', '--wrestlers-ours']

    def sweep(tag, extra):
        csv = os.path.join(work, tag + '.csv')
        if os.path.isfile(csv):
            os.remove(csv)
        rc, text = run(base + ['--sweep', csv, '--sweep-slot', '12'] + extra)
        if not os.path.isfile(csv):
            print(text[-1500:])
            raise RuntimeError('sweep %s failed (exit %d)' % (tag, rc))
        return rc, text, read_csv(csv)

    on_args = ['--shadow-casters-ours', '--rim-ours', '--validation', '1']
    rc0, t0, off = sweep('off', [])
    rc1, t1, on = sweep('on', on_args)
    frames = len(on)
    pic = [a[0] for a, b in zip(off, on) if a[1] != b[1]]
    s12 = [a[0] for a, b in zip(off, on) if a[3] != b[3]]
    line(len(off) == len(on) and frames > 100 and not pic, '%d frames: picture differs on %d (first %s); the slot-12 shadow-map images differ on %d (reported: one-mantissa-step noise in the positions)' % (frames, len(pic), pic[:5], len(s12)))
    v = VALID.search(t1)
    line(rc1 == 0 and v is not None and int(v.group(1)) == 0, 'validation errors %s (exit %d)' % (v.group(1) if v else '?', rc1))
    sm, sc = SHADOW.search(t1), SHADOW_C.search(t1)
    rm, rc_ = RIM.search(t1), RIM_C.search(t1)
    if not (sm and sc and rm and rc_):
        line(False, 'census lines missing from the fview output')
        return 1
    claimed, rep, _, _, not_ours, ver, bad = map(int, sm.groups())
    line(rep == 23 * (frames - 1) and not_ours == 23 and bad == 0 and ver == rep and int(sc.group(7)) == 0,
         'shadow casters: replaced %d (want 23 x %d frames), kept not ours %d (want 23: the first frame), verify %d bad %d, mismatch %s' % (rep, frames - 1, not_ours, ver, bad, sc.group(7)))
    rclaimed, rrep, rno, runj, rver, rbad = map(int, rm.groups())
    line(rrep == 12 * (frames - 1) and rno == 12 and rbad == 0 and rver == rrep and int(rc_.group(7)) == 0,
         'rim map: replaced %d (want 12 x %d), kept not ours %d (want 12), verify %d bad %d, mismatch %s' % (rrep, frames - 1, rno, rver, rbad, rc_.group(7)))

    def plant(name, fault):
        rc, t = run(base + ['--frame', '40', '--ops', '--json', os.path.join(work, 'plant.json'), '--shadow-casters-ours', '--replace-fault', fault])
        m = SHADOW.search(t)
        if not m:
            line(False, 'plant %s: no census line' % name)
            return
        _, prep, _, _, _, pver, pbad = map(int, m.groups())
        line(prep == 0 and pbad > 0, 'plant %s: replaced %d (want 0), verify bad %d (want > 0): the check sees it, the draws stay the stream\'s' % (name, prep, pbad))

    plant('editor_exact (the byte-for-byte rule = the state before lane 225)', 'shadow.editor_exact')
    plant('bone_zero', 'shadow.bone_zero')
    print('caw_casters_check: ' + ('PASS' if not fails else 'FAIL (%d)' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
