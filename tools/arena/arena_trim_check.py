#!/usr/bin/env python3
"""Lane 243 offline proof (0 game runs): a stage mesh the game loads without its leading unused vertices (arena 44, slot 111 part 2: file 47 vertices, stream 45 = file vertices 2..46)
is replaced by our arena drawer instead of being refused (`mismatch 72` in the live run 7162).

  py -I tools\\arena\\arena_trim_check.py [--root DIR] [--zip ZIP]

Input: the case zip of run 7162 (route s101_sweep_match_type, WrestleMania X-Seven ladder, arena 44; KEEP in its run folder) and the game files. A missing input FAILS, never skips.
Arms (one fview replay of the zip's frames each, through the real seam, --arena-ours; the position compare runs on every draw offline):
  clean                 mismatch 0, joins dropped 0, trimmed meshes >= 1, draws of a trimmed window >= 1, replaced = the old rule's replaced + its mismatch count, validation errors 0
  no_trim_window        (the past bug: the old whole-mesh rule) mismatch > 0 and replaced lower than clean: re-finds the live numbers
  window_shift          (the window one vertex late) the position compare refuses it: mismatch or not-ours grows, replaced lower than clean
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
CENSUS = re.compile(r'\[rdr\]\[arena\] ours frames (\d+), fallback (\d+), replaced (\d+), kept (\d+) \(not ready (\d+), unjoined (\d+), mismatch (\d+), variant (\d+), texture (\d+), palette (\d+), not ours (\d+)\)')
DETAIL = re.compile(r'joins dropped (\d+);.*trimmed meshes (\d+), draws of a trimmed window (\d+)')


def main_checkout(here):
    try:
        common = subprocess.run(['git', '-C', here, 'rev-parse', '--path-format=absolute', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if common:
            return os.path.dirname(common)
    except Exception:
        pass
    return here


def fail(msg):
    print('FAIL  ' + msg)
    return 1


def arm(exe, zip_path, game, fault, frames):
    cmd = [exe, '--case', zip_path, '--replay', '--frames', str(frames), '--arena-ours', '--game', game]
    if fault:
        cmd += ['--replace-fault', fault]
    env = dict(os.environ, CLAUDECODE='1')   # headless: fview refuses window mode for agents
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env=env)
    text = (p.stdout or '') + (p.stderr or '')
    m = None
    for m in CENSUS.finditer(text):
        pass
    d = None
    for d in DETAIL.finditer(text):
        pass
    err = re.search(r'validation errors (\d+)', text)
    if not m or not d or not err:
        print(text[-1500:])
        raise RuntimeError('arm %r printed no arena census (exit %d)' % (fault or 'clean', p.returncode))
    g = [int(x) for x in m.groups()]
    return {'replaced': g[2], 'kept': g[3], 'mismatch': g[6], 'not_ours': g[10], 'dropped': int(d.group(1)), 'trimmed': int(d.group(2)), 'windows': int(d.group(3)), 'validation': int(err.group(1))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--zip', default='')
    ap.add_argument('--frames', type=int, default=301)
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    main_co = main_checkout(root)
    game = os.path.join(main_co, 'game_data', 'extracted')
    runs = os.path.join(main_co, 'out', 's101', 'runs', '20261113_121725_s101_sweep_match_type')
    zips = [os.path.join(runs, f) for f in os.listdir(runs) if f.startswith('case_') and f.endswith('.zip')] if os.path.isdir(runs) else []
    zip_path = args.zip or (zips[0] if zips else '')
    if not zip_path or not os.path.isfile(zip_path):
        return fail("input missing: run 7162's case zip under " + runs)
    if not os.path.isdir(game):
        return fail('input missing: the game files ' + game)
    sys.path.insert(0, os.path.join(HERE, '..', 'fview'))
    import fview_exe as fe
    exe = fe.find_or_exit(root=root, kind='dev')
    fails = 0
    try:
        clean = arm(exe, zip_path, game, '', args.frames)
        old = arm(exe, zip_path, game, 'no_trim_window', args.frames)
        shift = arm(exe, zip_path, game, 'window_shift', args.frames)
    except Exception as e:
        return fail(str(e))

    def check(ok, text):
        nonlocal fails
        print(('PASS  ' if ok else 'FAIL  ') + text)
        fails += not ok

    print('INFO  clean %s\nINFO  no_trim_window %s\nINFO  window_shift %s' % (clean, old, shift))
    check(clean['mismatch'] == 0 and clean['dropped'] == 0, 'clean: mismatch %d, joins dropped %d (both must be 0)' % (clean['mismatch'], clean['dropped']))
    check(clean['trimmed'] >= 1 and clean['windows'] >= 1, 'clean: trimmed meshes %d, draws of a trimmed window %d (both >= 1: the case is in the capture, so a 0 is not vacuous)' % (clean['trimmed'], clean['windows']))
    check(clean['validation'] == 0 and old['validation'] == 0 and shift['validation'] == 0, 'validation errors 0 in every arm')
    check(old['mismatch'] > 0 and old['replaced'] < clean['replaced'], 'past bug no_trim_window: mismatch %d (> 0), replaced %d < clean %d' % (old['mismatch'], old['replaced'], clean['replaced']))
    check(clean['replaced'] >= old['replaced'] + old['mismatch'], 'clean replaces at least the old rule\'s replaced + its mismatch draws: %d >= %d + %d' % (clean['replaced'], old['replaced'], old['mismatch']))
    check(shift['replaced'] < clean['replaced'] and (shift['mismatch'] > 0 or shift['not_ours'] > clean['not_ours']), 'plant window_shift is refused by the position compare: replaced %d < clean %d, mismatch %d, not ours %d (clean %d)' % (
        shift['replaced'], clean['replaced'], shift['mismatch'], shift['not_ours'], clean['not_ours']))
    print('arena_trim_check: ' + ('PASS' if not fails else 'FAIL'))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
