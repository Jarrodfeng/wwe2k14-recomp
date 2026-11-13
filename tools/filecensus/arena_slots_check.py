#!/usr/bin/env python3
"""Lane 37 acceptance test (offline, 0 game runs; the census viewer is not the game): the arena r / y slot count, all 57 arenas, and arena 54's two slots.

  py -I tools\\filecensus\\arena_slots_check.py [--root DIR]

Arena 54 (WrestleMania XXVII) was the census's one load failure: the file flags 50 slots `r` / `y`, the draw list got 48. Cause [L, S101 lane 37]: slots 216 and 220
carry the legend flag `e` (shown only in events / entrances); the loader's match look hides them (src\\assets\\asset_load.h `slot_when`, S98), so the game does not draw
them twice in a match, and the census counted them anyway. The class fix is the counting rule (filecensus_flags.drawn_in_match), not the loader.

1. old rule control: counting every flagged slot (look='all') gives 50 on arena 54 and differs from the match-look count on EXACTLY arena 54 (the before line);
2. all 57 arenas, clean census viewer: the draw-list count equals the file's match-look count on every arena, no arena is short, and the slot LISTS are equal;
   one line per arena is printed, with the run time;
3. planted fault WWE_CENSUS_PLANT_SLOT=44 (the loader never makes slot 44's flagged copies, arena 54 only): the check must fail on arena 54 and name slot 44, and on
   no other arena.
Match look only; the entrance look (`e` slots drawn, `g` hidden) is not counted by the board (filecensus_flags look='entrance' gives arena 54 -> 44).
Exit 0 only when all hold. Any input that cannot be read is a FAIL, never a pass."""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import build_util  # noqa: E402
import census  # noqa: E402
import filecensus_flags  # noqa: E402


def run(cmd, timeout):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--ids', default='', help='arena ids (default: all 57); the old-rule control always reads all of them')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    t0 = time.time()
    game = os.path.join(census.MAIN, 'game_data', 'extracted')
    if not os.path.exists(game):
        print('FAIL  cannot read its input: %s' % game)
        return 1
    fails = []

    # 1. the old counting rule, from the file alone
    try:
        ids = [int(x) for x in args.ids.split(',') if x] or sorted(int(f[2:4]) for f in os.listdir(os.path.join(game, 'pac', 'bg')) if f.startswith('bg') and f[2:4].isdigit() and f.endswith('.pac'))
        old = {i: filecensus_flags.arena_flagged_slots(i, 'all') for i in ids}
        new = {i: filecensus_flags.arena_flagged_slots(i) for i in ids}
    except Exception as e:
        print('FAIL  cannot read the arena files: %s' % e)
        return 1
    diff = sorted(i for i in ids if old[i] != new[i])
    ok = (diff == [54]) if 54 in ids else (diff == [])
    print(('PASS' if ok else 'FAIL') + '  old rule (every r / y slot counted) differs from the match-look rule on %s; arena 54: old %s, new %s'
          % (diff, old.get(54), new.get(54)))
    if not ok:
        fails.append('old rule control')

    try:
        exe = build_util.ensure_census_exe(root, census.MAIN)
    except Exception as e:
        print('FAIL  the census viewer could not be built: %s' % e)
        return 1
    run_id = 'slots%d' % os.getpid()

    def census_run(name, extra, idlist):
        tag = '%s_%s' % (run_id, name)
        code, out = run([sys.executable, '-I', os.path.join(HERE, 'census.py'), '--tag', tag, '--exe', exe, '--only', 'arenas', '--ids', idlist,
                         '--no-sheets', '--timeout', '120'] + extra, 3300)
        if code:
            print('FAIL  census %s exited %d: %s' % (name, code, out[-400:]))
            return None
        try:
            return {r['key']: r for r in json.load(open(os.path.join(census.MAIN, 'out', 's101', 'filecensus', tag, 'board.json')))['rows']}
        except Exception as e:
            print('FAIL  census %s: cannot read the board (%s)' % (name, e))
            return None

    # 2. all arenas, clean
    rows = census_run('clean', [], ','.join(str(i) for i in ids))
    if rows is None:
        return 1
    bad = []
    for i in ids:
        r = rows.get('arena_%d' % i)
        if r is None:
            bad.append('arena_%d: no row' % i)
            continue
        fl, sub = r.get('file_slot_list'), r.get('submitted_slots')
        print('  arena %-3d file %-3s draw list %-3s %s' % (i, r.get('file_slots'), r.get('submitted_flagged'), 'ok' if fl == sub else 'SLOTS DIFFER'))
        if fl is None or sub is None:
            bad.append('arena_%d: no slot lists on the board (needs the census build)' % i)
        elif fl != sub or r.get('obj_short') or r.get('file_slots') != r.get('submitted_flagged'):
            bad.append('arena_%d: file slots %s, draw list slots %s' % (i, fl, sub))
    print(('PASS' if not bad else 'FAIL') + '  all %d arenas: draw-list count and slots = what the file asks the game to draw %s' % (len(ids), '; '.join(bad)))
    if bad:
        fails.append('clean arenas')

    # 3. planted: slot 44 of arena 54 dropped
    if 54 in ids:
        rows = census_run('plant44', ['--plant', 'slot=44'], '54')
        if rows is None:
            return 1
        r = rows.get('arena_54', {})
        ok = bool(r.get('obj_short')) and r.get('missing_slots') == [44]
        print(('PASS' if ok else 'FAIL') + '  planted (slot 44 loses its flagged copies): arena 54 flagged=%s, missing slots %s (expected [44])'
              % (r.get('obj_short'), r.get('missing_slots')))
        if not ok:
            fails.append('planted slot')
    print('run time %.0f s' % (time.time() - t0))
    if fails:
        print('FAIL  ' + ', '.join(fails))
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
