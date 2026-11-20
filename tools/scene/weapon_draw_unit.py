#!/usr/bin/env python3
"""S103 PW-3 gate check: the weapon drawer's offline proof (0 game runs). Needs out\\tools\\weapon_draw_unit.exe (tools\\scene\\build_weapon_draw_unit.ps1), the main checkout's game_data\\extracted and
out\\captures\\a04_match (read by absolute path, never copied).
  py -I tools\\scene\\weapon_draw_unit.py --root .
Dumps the capture's stream draws of the ring-steps object (pw3_steps_dump.py), runs the unit clean (exit 0), then each planted twin, which must exit 1 AND fail its own named check."""
import argparse
import os
import subprocess
import sys

TWINS = {'tex_key': 'draw.diffuse_picture_equal', 'transpose': 'draw.positions_equal_arena_steps', 'merge': 'draw.two_instances_two_items', 'stage_id_plain': 'plan.stage_id_without_flag_refused', 'm2_reopen': 'loader.m2_open_once'}


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
    ap.add_argument('--root', default='.')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    exe = os.path.join(root, 'out', 'tools', 'weapon_draw_unit.exe')
    game = os.path.join(main_co, 'game_data', 'extracted')
    cap = os.path.join(main_co, 'out', 'captures', 'a04_match')
    for need in (exe, game, os.path.join(cap, 'frames')):
        if not os.path.exists(need):
            print('FAIL  missing input: ' + need)
            return 1
    dump = os.path.join(root, 'out', 'pw3', 'steps_a04.txt')
    os.makedirs(os.path.dirname(dump), exist_ok=True)
    rc, out = run([sys.executable, '-I', os.path.join(root, 'tools', 'scene', 'pw3_steps_dump.py'), cap, dump])
    print(out.strip())
    if rc:
        print('FAIL  steps dump')
        return 1
    bad = 0
    rc, out = run([exe, '--game', game, '--steps', dump])
    print(out.rstrip())
    if rc != 0 or 'RESULT PASS' not in out:
        print('FAIL  clean arm (exit %d)' % rc)
        bad += 1
    for plant, check in TWINS.items():
        rc, out = run([exe, '--game', game, '--steps', dump, '--plant', plant])
        ok = rc == 1 and ('FAIL  ' + check) in out
        print('%s  twin %s: exit %d, fails %s' % ('PASS' if ok else 'FAIL', plant, rc, check if ok else '(the named check did not fail)'))
        bad += 0 if ok else 1
    print('RESULT %s' % ('FAIL %d' % bad if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
