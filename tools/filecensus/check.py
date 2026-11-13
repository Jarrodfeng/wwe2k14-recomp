#!/usr/bin/env python3
"""Lane 21 acceptance test (offline, 0 game runs; the viewer is not the game): the file census on a small fixed subset and its planted-fault twins.

  py -I tools\\filecensus\\check.py [--root DIR]

0. the census viewer is this checkout's own build, rebuilt when any source it is made from is newer (build_util.ensure_census_exe);
1. clean subset: arenas 5, 2, 80 (80 flags no r / y slot) and wrestlers 197, 210, 156 through both backends: no load failure, the draw-list count equals the
   r / y slot count read from the FILE on every arena, every wrestler row passes, and this item's validation errors are 0 (the viewer's total is per process);
2. planted "rdr's object list drops the flagged copies" (WWE_CENSUS_PLANT_MIRROR): flagged on EXACTLY the arenas whose file flags r / y slots (5 and 2, not 80);
3. planted S97 fault at the loader level (WWE_CENSUS_PLANT_LOADER: the model holds no mirrored placement, so the in-memory count is 0 as well): same;
4. planted missing texture (WWE_CENSUS_PLANT_TEX): flagged on that item only (arena_5);
5. wrestler twin: the same wrestlers with WWE_RDR_NO_PALETTE=1 (bind pose): EVERY wrestler row must fail;
6. planted hang (WWE_CENSUS_PLANT_HANG, --timeout 5): the hung item becomes a non-pass row and the next item still runs (any status but load failure: the picture verdicts move with the renderer).
7. lane 257: arenas 0, 2, 29, 40 clean pass; WWE_RDR_SPEC_NO_LIGHTCOL=1 fails 2 and WWE_RDR_NO_LIVE_SCREEN=1 fails 0.
Exit 0 only when all hold. A board that cannot be read, a viewer that cannot start, a missing input: FAIL, never a pass. Tags are per run (pid), so
concurrent gate runs do not share a folder."""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
import build_util  # noqa: E402
import census  # noqa: E402

ARENAS = '5,2,80'
WRESTLERS = '197,210,156'


def run(cmd, timeout):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return p.returncode, p.stdout + p.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=ROOT)
    ap.add_argument('--keep', action='store_true', help='keep the per-run board folders')
    args = ap.parse_args()
    root = os.path.abspath(args.root)
    game = os.path.join(census.MAIN, 'game_data', 'extracted')
    if not os.path.exists(game):
        print('FAIL  cannot read its input: %s' % game)
        return 1
    fails = []

    # unit twins of review fixes 1 and 2 (they fail on the pre-fix code: the old parse took the MAX of the process total for every item; the old exe check
    # looked only at front_end.cpp and make_census_build.py)
    class _L:
        val_last = 0
    lk = _L()
    seq = [census.item_validation(x, lk) for x in (0, 3, 3, 5, None, 5)]  # one error-producing item, then clean ones, then another
    lk.val_last = 0                                                      # a restarted process starts its total at 0
    seq.append(census.item_validation(1, lk))
    ok_u = seq == [0, 3, 0, 2, None, 0, 1]
    print(('PASS' if ok_u else 'FAIL') + '  validation delta per item (process total 0,3,3,5,-,5 then a restart at 1): %s' % seq)
    if not ok_u:
        fails.append('validation delta')
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        os.makedirs(os.path.join(td, 'src', 'assets', 'view'))
        os.makedirs(os.path.join(td, 'out', 'tools'))
        fx = os.path.join(td, 'src', 'assets', 'view', 'x.cpp')
        ex = build_util.exe_path(td)
        open(fx, 'w').write('x')
        open(ex, 'w').write('x')
        os.utime(fx, (1000, 1000))
        os.utime(ex, (2000, 2000))
        fresh = build_util.stale_reason(td)
        os.utime(fx, (3000, 3000))  # a viewer source (not front_end.cpp) newer than the exe
        stale = build_util.stale_reason(td)
        ok_s = fresh is None and stale is not None
        print(('PASS' if ok_s else 'FAIL') + '  stale census viewer detection: current -> %s, a newer viewer source -> %s' % (fresh, stale))
        if not ok_s:
            fails.append('stale exe')

    try:
        exe = build_util.ensure_census_exe(root, census.MAIN)
    except Exception as e:
        print('FAIL  the census viewer could not be built: %s' % e)
        return 1
    run_id = 'gate%d' % os.getpid()
    tags = []

    def census_run(name, extra, ids=ARENAS, wrestlers=WRESTLERS, timeout='120'):
        tag = '%s_%s' % (run_id, name)
        tags.append(tag)
        code, out = run([sys.executable, '-I', os.path.join(HERE, 'census.py'), '--tag', tag, '--exe', exe, '--ids', ids, '--wrestlers', wrestlers,
                         '--no-sheets', '--timeout', timeout] + extra, 1500)
        if code:
            print('FAIL  census %s exited %d: %s' % (name, code, out[-400:]))
            return None, None
        path = os.path.join(census.MAIN, 'out', 's101', 'filecensus', tag, 'board.json')
        try:
            return json.load(open(path))['rows'], path
        except Exception as e:
            print('FAIL  census %s: cannot read the board (%s)' % (name, e))
            return None, None

    def boards(path, *flags):
        code, out = run([sys.executable, '-I', os.path.join(HERE, 'compare_boards.py'), path] + list(flags), 60)
        return code == 0, ' | '.join(out.strip().splitlines())

    # 1. clean
    clean, cpath = census_run('clean', [])
    if clean is None:
        return 1
    arenas = {r['key']: r for r in clean if r['kind'] == 'arena'}
    wr = [r for r in clean if r['kind'] == 'wrestler']
    if sorted(arenas) != ['arena_2', 'arena_5', 'arena_80'] or len(wr) < 3:
        print('FAIL  clean subset: arenas %s, wrestler rows %d' % (sorted(arenas), len(wr)))
        return 1
    ok_b, msg = boards(cpath, '--mirror', '--clean')
    lf = [r['key'] for r in clean if r['status'] == 'load failure']
    badw = [r['key'] for r in wr if r['status'] != 'pass']
    badv = [r['key'] for r in clean if (r.get('validation') or 0) > 0]
    ok = ok_b and not lf and not badw and not badv
    print(('PASS' if ok else 'FAIL') + '  clean subset: %d arenas, %d wrestler rows; %s; load failures %s, wrestlers not passing %s, rows with new validation errors %s'
          % (len(arenas), len(wr), msg, lf, badw, badv))
    if not ok:
        fails.append('clean subset')

    # 2. / 3. the two planted half-arena faults
    for name, plant, what in (('mirror', 'mirror', 'rdr object list drops the flagged copies'), ('loader', 'loader', 'S97 at the loader level (flags never read)')):
        rows, path = census_run(name, ['--only', 'arenas', '--plant', plant])
        if rows is None:
            return 1
        ok_b, msg = boards(path, '--mirror')
        print(('PASS' if ok_b else 'FAIL') + '  planted "%s": %s' % (what, msg))
        if not ok_b:
            fails.append('planted ' + name)

    # 4. missing texture
    rows, path = census_run('tex', ['--only', 'arenas', '--plant', 'tex=arena 5'])
    if rows is None:
        return 1
    ok_b, msg = boards(path, '--tex', 'arena_5')
    print(('PASS' if ok_b else 'FAIL') + '  planted missing texture on arena 5 only: ' + msg)
    if not ok_b:
        fails.append('planted texture')

    # 5. wrestler twin
    rows, path = census_run('nopal', ['--only', 'wrestlers', '--env', 'WWE_RDR_NO_PALETTE=1'])
    if rows is None:
        return 1
    passing = [r['key'] for r in rows if r['status'] == 'pass']
    ok_t = len(rows) >= 3 and not passing
    print(('PASS' if ok_t else 'FAIL') + '  wrestler twin (bind pose, WWE_RDR_NO_PALETTE=1): %d rows, still passing: %s' % (len(rows), passing))
    if not ok_t:
        fails.append('wrestler twin')

    # 6. hang
    rows, path = census_run('hang', ['--only', 'arenas', '--plant', 'hang=arena 5:40'], ids='5,2', timeout='5')
    if rows is None:
        return 1
    by = {r['key']: r for r in rows}
    ok_h = 'arena_5' in by and by['arena_5']['status'] != 'pass' and any('no answer' in n or 'viewer' in n for n in by['arena_5']['notes']) \
        and 'arena_2' in by and by['arena_2']['status'] != 'load failure'
    print(('PASS' if ok_h else 'FAIL') + '  planted hang (timeout 5 s): arena_5 -> %s %s; arena_2 -> %s' % (
        by.get('arena_5', {}).get('status'), by.get('arena_5', {}).get('notes', [])[:1], by.get('arena_2', {}).get('status')))
    if not ok_h:
        fails.append('planted hang')

    # 7. lane 257: the three arena faults the census found (the specular lobe without the light's colour, the stage screen drawn black, a bump-map material with no normal map
    #    in the debug colour). Clean on arenas 0 (screen), 29 (floor highlight), 40 (the bump-map card) = all pass; each planted twin must fail its arena.
    rows, path = census_run('l257clean', ['--only', 'arenas'], ids='0,2,29,40')
    if rows is None:
        return 1
    notpass = [r['key'] for r in rows if r['status'] != 'pass']
    ok7 = len(rows) == 4 and not notpass
    print(('PASS' if ok7 else 'FAIL') + '  lane 257 clean: arenas 0, 2, 29, 40 pass (not passing: %s)' % notpass)
    if not ok7:
        fails.append('lane 257 clean')
    for env, ids, what in (('WWE_RDR_SPEC_NO_LIGHTCOL=1', '2', 'highlight without the first light colour (NXT curtains washed white)'),
                           ('WWE_RDR_NO_LIVE_SCREEN=1', '0', 'the stage screen left black')):
        rows, path = census_run('l257_' + ids, ['--only', 'arenas', '--env', env], ids=ids)
        if rows is None:
            return 1
        ok7 = len(rows) == 1 and rows[0]['status'] != 'pass'
        print(('PASS' if ok7 else 'FAIL') + '  lane 257 planted "%s" on arena %s: %s' % (what, ids, rows[0]['status'] if rows else '?'))
        if not ok7:
            fails.append('lane 257 plant ' + env)

    if not args.keep:
        for t in tags:
            shutil.rmtree(os.path.join(census.MAIN, 'out', 's101', 'filecensus', t), ignore_errors=True)
    print('RESULT ' + ('PASS' if not fails else 'FAIL: ' + ', '.join(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
