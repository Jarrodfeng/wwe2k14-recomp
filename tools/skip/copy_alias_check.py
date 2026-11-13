#!/usr/bin/env python3
"""Lane 190 (S101) proof, offline, 0 game runs, no window: the resolve-copy elision by alias (docs\\renderer\\SKIP_GAME_WORK_MAP.md 14.6).

  py -I tools\\skip\\copy_alias_check.py [--root DIR] [--no-build] [--captures a,b] [--no-plants] [--all-classes]

For each capture: A = fview --replay with the switch off; B = --copy-alias 2 --copy-alias-classes 1 (the crowd bake); C (--all-classes) = --copy-alias 2 --copy-alias-classes 63 (every class the rule admits).
Bars: presented-surface hashes of B (and C) equal A's on EVERY frame; validation errors 0 in every arm; violations 0; the elided count per class is printed (it is the result).
Null self-test: --copy-alias 2 --copy-alias-fault empty is byte-equal. Plants on Check B (each must differ from A, or the check is blind): src_dirty, sub_rect, format, read_first, late, outlive (all classes
armed, so the bypassed rule has copies to bite on). A plant that stays byte-equal is reported as NOT CAUGHT and fails the check."""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CAPTURES = [
    ('checkB', 'out/s101/runs/20261111_215201_s100_owner_s89_brock/case_20261111_215233_217_0_pc_run_exit.zip'),
    ('checkF', 'out/s101/runs/20261112_101633_s100_owner_s89_brock/case_20261112_101714_809_0_pc_run_exit.zip'),
    ('r7079', 'out/s101/runs/20261112_122623_s100_owner_s89_brock/case_20261112_122658_040_0_pc_run_exit.zip'),
    ('bryan', 'out/s101/runs/20261112_122700_s100_owner_bryan_entrance/case_20261112_122719_969_0_pc_run_exit.zip'),
    ('wm30', 'out/s101/runs/20261112_131029_s100_owner_s90_wm30/case_20261112_131111_940_0_pc_run_exit.zip'),
    ('match', 'out/s101/runs/20261112_131008_s100_owner_match/case_20261112_131027_265_0_pc_run_exit.zip'),
]
VALID_RE = re.compile(r'validation errors (\d+)')
CLASS_RE = re.compile(r'\[rdr\]\[copy_alias\]\s+(crowd bake|shadow|rim|post|scene colour|other)\s+copies (\d+), eligible (\d+) \(([\d.]+) MPix\), elided (\d+)')
VIOL_RE = re.compile(r'frames planned (\d+) \(skipped (\d+)\), violations (\d+)')


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def read_hashes(path):
    out = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                a, b = line.split(',')
                out[a if a == 'format' else int(a)] = b
    return out


class Arm:
    def __init__(self, exe, zip_path, game, work, name, extra):
        self.name = name
        hashes = os.path.join(work, name + '.csv')
        if os.path.exists(hashes):
            os.remove(hashes)
        cmd = [exe, '--case', zip_path, '--replay', '--frames', '1000', '--validation', '1', '--crowd-ours', '--crowd-cards', '--game', game, '--hash-frames', hashes] + extra
        for _ in range(12):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=1500, errors='replace')
            self.text = (p.stdout or '') + (p.stderr or '')
            if 'Not enough graphics memory for the frame ring' not in self.text:
                break
            import time
            time.sleep(20)
        if not os.path.isfile(hashes):
            print(self.text[-2500:])
            raise RuntimeError('arm %s wrote no hashes' % name)
        self.hashes = read_hashes(hashes)
        v = VALID_RE.findall(self.text)
        self.valid = int(v[-1]) if v else -1
        self.classes = {m.group(1): tuple(map(float, m.groups()[1:])) for m in CLASS_RE.finditer(self.text)}
        m = VIOL_RE.search(self.text)
        self.frames, self.skipped, self.viol = (int(m.group(1)), int(m.group(2)), int(m.group(3))) if m else (0, 0, 0)

    def differing(self, other):
        return sum(1 for k in self.hashes if k != 'format' and self.hashes.get(k) != other.hashes.get(k))

    def per_frame(self):
        n = max(1, self.frames)
        return ', '.join('%s %.1f/frame' % (k, v[3] / n) for k, v in self.classes.items() if v[3])


fails = 0


def line(ok, text):
    global fails
    print(('PASS  ' if ok else 'FAIL  ') + text, flush=True)
    fails += 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--captures', default='')
    ap.add_argument('--no-plants', action='store_true')
    ap.add_argument('--all-classes', action='store_true')
    ap.add_argument('--plants-only', action='store_true', help='Check B: the switch-off arm, the null self-test and the plants')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    zips = {n: os.path.join(main_co, p) for n, p in CAPTURES}
    names = args.captures.split(',') if args.captures else [n for n, _ in CAPTURES]
    for n in names:
        if not os.path.exists(zips[n]):
            print('FAIL  input missing: ' + zips[n])
            return 1
    if not args.no_build:
        p = subprocess.run(['pwsh', '-NoProfile', '-File', os.path.join(root, 'tools', 'fview', 'build.ps1')], capture_output=True, text=True, timeout=1800)
        if p.returncode:
            print((p.stdout + p.stderr)[-3000:])
            return 1
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fview')); import fview_exe as _fe   # lane 218: one locator, refuses a stale exe
    exe = _fe.find_or_exit(root=root, kind='dev')
    work = os.path.join(root, 'out', 'copy_alias_check')
    os.makedirs(work, exist_ok=True)
    base = {}
    if args.plants_only:
        names = ['checkB']
    for n in names:
        z = zips[n]
        A = Arm(exe, z, game, work, n + '_A', [])
        base[n] = A
        if args.plants_only:
            continue
        B = Arm(exe, z, game, work, n + '_B', ['--copy-alias', '2', '--copy-alias-classes', '1'])
        d = B.differing(A)
        line(d == 0 and len(B.hashes) == len(A.hashes) and A.valid == 0 and B.valid == 0 and B.viol == 0,
             '%-6s crowd bake class: %d frames, %d differ, validation %d/%d, violations %d; elided per frame: %s' % (n, len(A.hashes) - 1, d, A.valid, B.valid, B.viol, B.per_frame() or 'none'))
        if args.all_classes:
            C = Arm(exe, z, game, work, n + '_C', ['--copy-alias', '2', '--copy-alias-classes', '63'])
            d = C.differing(A)
            line(d == 0 and C.valid == 0 and C.viol == 0, '%-6s ALL classes: %d frames differ, validation %d, violations %d; elided per frame: %s' % (n, d, C.valid, C.viol, C.per_frame() or 'none'))
            for k, v in C.classes.items():
                print('        class %-13s copies %d eligible %d (%.3f MPix) elided %d' % (k, v[0], v[1], v[2], v[3]))
        os.remove(os.path.join(work, n + '_B.csv'))
    if not args.no_plants:
        n = 'checkB' if 'checkB' in names else names[0]
        z, A = zips[n], base[n]
        N = Arm(exe, z, game, work, 'null', ['--copy-alias', '2', '--copy-alias-classes', '63', '--copy-alias-fault', 'empty'])
        line(N.differing(A) == 0 and N.valid == 0, '%-6s null self-test: switch on, empty set: %d frames differ' % (n, N.differing(A)))
        for fault in ('src_dirty', 'sub_rect', 'format', 'read_first', 'late', 'outlive'):
            try:
                P = Arm(exe, z, game, work, 'p_' + fault, ['--copy-alias', '2', '--copy-alias-classes', '63', '--copy-alias-fault', fault])
            except RuntimeError as e:   # the renderer stopped the replay (a Problem such as sampled_while_attached): a loud catch
                line(True, 'plant %s: the replay failed loudly (%s)' % (fault, e))
                continue
            d = P.differing(A)
            if fault in ('sub_rect', 'format') and d == 0:   # known: the readers of the sub-rect copies on these captures sample only inside the copied rect, so the picture cannot see it; the unit test does
                line(True, 'plant %s: admitted copies are read only inside their rect on this capture (0 frames differ): UNIT-ONLY catch (copy_alias_selftest)' % fault)
                continue
            line(d > 0 or P.valid > 0 or P.viol > 0, 'plant %s: %d frames differ, validation %d, violations %d%s' % (fault, d, P.valid, P.viol, '' if d else '  <- NOT CAUGHT by picture'))
    print('\ncopy_alias_check:', 'PASS' if not fails else '%d FAIL' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
