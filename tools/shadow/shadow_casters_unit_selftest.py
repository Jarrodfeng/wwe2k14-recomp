#!/usr/bin/env python3
"""Gate check of lane 99 (S101): out\\tools\\shadow_casters_unit.exe (built by tools\\shadow\\build_shadow_casters_unit.ps1) against Check B's capture.
Clean run exits 0; the planted fault arena_stale (the old bug: ring pairings and buffers kept across an arena change) exits 1 and fails exactly swap.join_not_reused, swap.buffer_not_reused and
no_arena.not_replaced; a missing capture exits 2; an unknown fault 3. 0 game runs.   py -I tools\\shadow\\shadow_casters_unit_selftest.py"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


def main_checkout():
    try:
        common = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return ROOT


MAIN = main_checkout()
EXE = os.path.join(ROOT, 'out', 'tools', 'shadow_casters_unit.exe')
CAP = os.path.join(MAIN, 'out', 'lane40', 'brock', 'capture')
GAME = os.path.join(MAIN, 'game_data', 'extracted')
bad = 0


def run(*args):
    p = subprocess.run([EXE, '--game', GAME] + list(args), capture_output=True, text=True, timeout=600)
    failed = {l.split()[1] for l in p.stdout.splitlines() if l.startswith('FAIL')}
    return p.returncode, failed, p.stdout + p.stderr


def check(ok, text):
    global bad
    print(('PASS  ' if ok else 'FAIL  ') + text)
    bad += not ok


def main():
    for need in (EXE, CAP, GAME):
        if not os.path.exists(need):
            print('FAIL  input missing: ' + need)
            return 1
    rc, failed, out = run('--capture', CAP)
    check(rc == 0 and not failed, 'clean: exit %d, failed %s' % (rc, sorted(failed) or 'none'))
    if rc:
        print(out)
    rc, failed, out = run('--capture', CAP, '--fault', 'arena_stale')
    want = {'swap.join_not_reused', 'swap.buffer_not_reused', 'no_arena.not_replaced'}
    check(rc == 1 and failed == want, 'plant arena_stale: exit %d, failed %s (must be exactly %s)' % (rc, sorted(failed), sorted(want)))
    rc, failed, out = run('--capture', CAP, '--fault', 'flat_indices')   # lane 192: the old flat index rule in the wrestler class
    check(rc == 1 and failed == {'multigroup.joined_wrestler'}, 'plant flat_indices: exit %d, failed %s (must be exactly multigroup.joined_wrestler)' % (rc, sorted(failed)))
    rc, failed, out = run('--capture', CAP + '.missing')
    check(rc == 2, 'missing capture: exit %d (must fail loudly)' % rc)
    rc, failed, out = run('--capture', CAP, '--fault', 'nonsense')
    check(rc == 3, 'unknown fault: exit %d' % rc)
    print('%d problems' % bad)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
