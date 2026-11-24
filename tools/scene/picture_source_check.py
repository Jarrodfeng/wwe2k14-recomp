#!/usr/bin/env python3
"""S103 PN2 (lane PN23): runs out\\tools\\picture_source_unit.exe on the c2d captures, clean and with every planted twin (each must fail its NAMED check).

  py -I tools\\scene\\picture_source_check.py --root . [--only c2d_menu,...] [--no-plants]

Inputs (read only, from the MAIN checkout: out\\captures\\c2d_*, game_data\\extracted): a missing input FAILS loudly (exit 2), never a pass. 0 game runs. The unit is built by
tools\\scene\\build_picture_source_unit.ps1 (a separate check)."""
import argparse
import os
import subprocess
import sys

PLANTS = ['death_ignored', 'counter_ignored', 'rehash_every_hit', 'rt_as_file', 'wrong_mip', 'no_swap', 'tiled_ignored', 'race_ignored', 'swizzle_ignored', 'unknown_8888', 'file_any_order', 'no_budget', 'keep_blocks', 'no_bridge', 'bridge_cap', 'scene_copy_old']


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--only', default='')
    ap.add_argument('--no-plants', action='store_true')
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    main_co = main_checkout(root)
    exe = os.path.join(root, 'out', 'tools', 'picture_source_unit.exe')
    game = os.path.join(main_co, 'game_data', 'extracted')
    caps = os.path.join(main_co, 'out', 'captures')
    cache = os.path.join(main_co, 'out', 's103', 'picsrc_idx')
    for what, p in (('the unit', exe), ('the game files', os.path.join(game, 'pac', 'menu')), ('the c2d captures', os.path.join(caps, 'c2d_menu', 'images.bin'))):
        if not os.path.exists(p):
            print('FAIL input missing: %s (%s)' % (what, p))
            return 2
    os.makedirs(cache, exist_ok=True)
    base = [exe, '--game', game, '--cache', cache, '--captures', caps] + (['--only', a.only] if a.only else [])
    bad = 0
    r = subprocess.run(base, capture_output=True, text=True, timeout=1500)
    sys.stdout.write(r.stdout)
    if r.returncode:
        print('FAIL clean run exit %d %s' % (r.returncode, r.stderr[-300:]))
        bad += 1
    if not a.no_plants:
        for p in PLANTS:
            r = subprocess.run(base + ['--plant', p], capture_output=True, text=True, timeout=1500)
            last = (r.stdout.strip().splitlines() or [''])[-1]
            ok = r.returncode == 0 and last.startswith('PASS planted')
            print('%s plant %s: %s' % ('PASS' if ok else 'FAIL', p, last))
            bad += not ok
    print('%s: picture_source_check' % ('FAIL' if bad else 'PASS'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
