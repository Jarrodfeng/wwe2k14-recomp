r"""S110 CATALOGUE: tools\scene\catalogue.py on synthetic sources (no game text: every name here is made up or a public event name).

    py -I tools\scene\catalogue_selftest.py        (exit 0 = every check passed, 1 = a check failed: the FAIL lines say which)

Builds a catalogue from a planted tree in a temp folder (db\ids, the atlas, a run log with a menu listing that holds one of our own items,
a Create a Superstar flow json; no game files, so the 30 Years and stage sections must come out missing), then: a planted near-miss must
rank first, a planted ambiguity must return both, a planted unknown must exit 2 with the five nearest, the menu item shifted by our own item
must keep its stock label, check must see a changed source. Our own vocabulary (moments, bus kinds, verbs) is read from this checkout.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import catalogue  # noqa: E402

TOOL = os.path.join(HERE, 'catalogue.py')
FAILS = []
CHECKS = [0]


def check(ok, what):
    CHECKS[0] += 1
    if not ok:
        FAILS.append(what)
        print('FAIL', what)


def write(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8') as f:
        if isinstance(obj, str):
            f.write(obj)
        else:
            json.dump(obj, f, ensure_ascii=False)


def plant(root):
    data, local = os.path.join(root, 'data'), os.path.join(root, 'local')
    ids = os.path.join(data, 'db', 'ids')
    write(os.path.join(ids, 'characters.json'), {'entries': [
        {'id': 900, 'name': 'Alfa Bravo', 'kind': 'wrestler'},
        {'id': 901, 'name': 'Charlie Bravo', 'kind': 'wrestler'},
        {'id': 904, 'name': 'Zoë Quebec', 'kind': 'wrestler'},
        {'id': 902, 'name': 'Delta Echo', 'kind': 'manager'}]})
    write(os.path.join(ids, 'dlc_characters.json'), {'entries': [{'id': 950, 'name': 'Foxtrot Golf', 'pack': 1}]})
    write(os.path.join(ids, 'arenas.json'), {'entries': [
        {'id': 0, 'name': 'Testville'}, {'id': 13, 'name': 'Hotel Arena'}, {'id': 22, 'name': 'Hotel Arena'},
        {'id': 15, 'name': 'WrestleMania 29'}, {'id': 56, 'name': 'WrestleMania XXX'}]})
    write(os.path.join(ids, 'match_types.json'), {'entries': [{'id': 0, 'name': 'NORMAL', 'menu_paths': ['PLAY > ONE ON ONE > NORMAL']}]})
    write(os.path.join(data, 'docs', 'systems', 'game_atlas.json'), {
        'nodes': {'list:1': {'kind': 'list', 'labels': ['ALPHA', 'BRAVO'], 'opened_from': {'menu': 0, 'item': 0}},
                  'list:2': {'kind': 'list', 'labels': ['KILO', 'TOPS'], 'opened_from': {'menu': 1, 'item': 0}}},
        'edges': [{'from': 'list:1', 'to': 'list:2', 'how': {'item': 0}}, {'from': 'list:1', 'to': 'screen:1500', 'how': {'item': 1}}]})
    # the listing: our own item (record 7168, code 1300) sits at menu 1 item 1, so the game's BRAVO (record 1) is listed at item 2
    pre = '[2026-01-01 00:00:00.000] [info] [core] [t1] [wwe2k14][menu_goto] '
    write(os.path.join(local, 'logs', 'wwe2k14_001.log'), ''.join(pre + x + '\n' for x in (
        'listing: 6 records, 3 menus, record array 30000000 (frame 10)',
        'rec 110 menu 0 item 0 code 3000 label 4294967295 parent -1 enterable no(top-level row)',
        'rec 0 menu 1 item 0 code 3000 label 1 parent 110 enterable yes',
        'rec 7168 menu 1 item 1 code 1300 label 2080374785 parent 110 enterable yes',
        'rec 1 menu 1 item 2 code 1500 label 2 parent 110 enterable yes',
        'rec 5 menu 2 item 0 code 2000 label 3 parent 0 enterable yes',
        'rec 6 menu 2 item 1 code 2000 label 4 parent 0 enterable no(planted refusal)')))
    write(os.path.join(local, 'out', 'harness', 'atlas', 'flow_cas_flow_slot1.json'), {'records': 3, 'menus': [
        {'menu': 0, 'opened_from': None, 'items': [{'item': 0, 'record': 0, 'label': 'LIMA', 'leads_to': 'menu 1'}]},
        {'menu': 1, 'opened_from': {'menu': 0, 'item': 0}, 'items': [
            {'item': 0, 'record': 75, 'label': 'MIKE', 'leads_to': 'screen 1002'},
            {'item': 1, 'record': 76, 'label': 'TOPS', 'leads_to': 'screen 1002'},
            {'item': 2, 'record': 77, 'label': 'TOPS', 'leads_to': 'screen 1002'}]}]})   # the game's own table: TOPS twice (records 76, 77)
    return data, local


def run(*args):
    p = subprocess.run([sys.executable, '-I', TOOL] + list(args), capture_output=True, text=True, encoding='utf-8')
    return p.returncode, [ln for ln in p.stdout.splitlines() if ln.strip()]


def ids_of(lines):
    return [ln.split('\t')[1] for ln in lines if not ln.startswith('#')]


def main():
    # in-process: folding, the edit distance, the WrestleMania aliases
    check(catalogue.fold('Zoë-Örn!') == ['zoe', 'orn'], 'fold drops diacritics and punctuation')
    check(catalogue.osa('brayn', 'bryan') == 1, 'a transposition is one edit')
    check('wm17' in catalogue.arena_aliases('WrestleMania X-Seven') and 'wm30' in catalogue.arena_aliases('WrestleMania XXX'),
          'WrestleMania aliases (ordinal table and roman numerals)')
    check(catalogue.arena_aliases('Hotel Arena') == [], 'no alias for a plain arena')
    root = tempfile.mkdtemp(prefix='catalogue_selftest_')
    try:
        data, local = plant(root)
        cat = os.path.join(local, 'out', 'scene', 'catalogue.txt')
        game = os.path.join(root, 'no_game_here')
        rc, out = run('find', 'alfa', '--local', local, '--catalogue', cat)
        check(rc == 1, 'find without a catalogue exits 1 (got %d)' % rc)
        rc, out = run('build', '--data', data, '--local', local, '--game', game, '--out', cat)
        check(rc == 0 and os.path.isfile(cat), 'build exits 0 and writes the file (got %d: %s)' % (rc, out[-3:]))
        head, ents = catalogue.read_catalogue(cat)
        with open(cat, encoding='utf-8') as f:
            body = [ln.rstrip('\n') for ln in f if ln.strip() and not ln.startswith('#')]
        check(all(len(ln.split('\t')) == 5 for ln in body), 'every line has 5 TAB fields')
        check(all(e.kind in catalogue.KINDS for e in ents), 'every KIND is a known one')
        check(any('THE SCENE MODEL' in h for h in head) and 30 <= len([h for h in head if not h.startswith('# source')]) <= 60,
              'the header carries the scene model in about 40 lines')
        kinds = {k: sum(1 for e in ents if e.kind == k) for k in catalogue.KINDS}
        sys.path.insert(0, HERE)
        import mode_table
        rows = mode_table.rows()
        want = {'wrestler': 4, 'character': 1, 'arena': 5, 'type': 1, 'menu': 5, 'cas': 4, 'wm30': 0, 'stage': 0, 'mode': len(rows)}
        check(all(kinds[k] == v for k, v in want.items()), 'counts per kind %s (wanted %s)' % (kinds, want))
        check(kinds['moment'] > 5 and kinds['event'] > 5 and kinds['do'] >= 8 and kinds['take'] >= 5, 'our vocabulary read: %s' % kinds)
        got = {(e.kind, e.id) for e in ents}
        check({('moment', 'bell'), ('moment', 'pin'), ('event', 'match/pin'), ('do', 'place'), ('take', 'screenshot')} <= got,
              'bell, pin, the bus pin, place, screenshot are in the vocabulary')
        # S110 ELIMFIN review: the decision line's what is chosen at run time (src\event_bus_finish_core.h What): finish and fall stay in the vocabulary
        check({('event', 'match/finish'), ('event', 'match/fall'), ('event', 'match/scene')} <= got,
              'the bus finish / fall / scene lines are in the vocabulary (%s)' % sorted(i for k, i in got if k == 'event'))
        # S110 BUSMOVE review: the move line is emitted from src\move_log_hook.h (the hook of the move log), not from event_bus.h's poll
        check({('event', 'move'), ('event', 'break')} <= got, 'the bus move / break kinds are in the vocabulary (%s)' % sorted(i for k, i in got if k == 'event'))
        missing =[h.split('\t')[1] for h in head if h.startswith('# missing\t')]
        check('wm30' in missing and 'stage' in missing and 'mode' not in missing, 'wm30 / stage named missing without game files, mode never (%s)' % missing)
        modes = {e.id: e for e in ents if e.kind == 'mode'}
        check(sorted(modes) == sorted(r['name'] for r in rows) and 'scene cas new' in modes['cas'].where
              and all(modes[r['name']].where.startswith('scene mode %s ' % r['name']) for r in rows if r['name'] != 'cas'),
              'the mode lines are the rows of src\\mode_table.h (%s)' % sorted(modes))
        menu = {e.id: e for e in ents if e.kind == 'menu'}
        check(menu.get('1') is not None and menu['1'].name == 'BRAVO', 'record 1 keeps its stock label behind our item (%s)' % (menu.get('1'),))
        check(menu.get('7168') is not None and menu['7168'].name == 'GRAPHICS', 'our item 7168 is GRAPHICS')
        check('110' not in menu, 'the top-level row is left out')
        check(menu.get('5') is not None and menu['5'].name == 'ALPHA > KILO', 'a submenu record carries its path')
        check(menu.get('6') is not None and 'planted refusal' in menu['6'].where, 'a not-enterable record says why')
        cas = {e.id: e for e in ents if e.kind == 'cas'}
        check(cas.get('76') is not None and cas['76'].name == 'LIMA > TOPS' and 'page=76' in cas['76'].where, 'cas record 76 with path and page')
        check(cas.get('77') is not None and cas['77'].name == 'LIMA > TOPS (2)' and cas['77'].aliases == ['TOPS (2)'],
              'the game\'s second TOPS is numbered: "LIMA > TOPS (2)" (%s)' % (cas.get('77'),))
        # find: the planted near-miss ranks first
        rc, out = run('find', 'Alfa', 'Brvo', '--local', local, '--catalogue', cat)
        check(rc == 0 and ids_of(out)[:1] == ['900'], 'near-miss "Alfa Brvo" -> 900 first (got %d %s)' % (rc, out))
        rc, out = run('find', 'brvo', '--kind', 'wrestler', '--local', local, '--catalogue', cat)
        check(rc == 0 and set(ids_of(out)[:2]) == {'900', '901'}, 'a near-miss on a shared word returns both owners (got %s)' % out)
        # the planted ambiguity returns both
        rc, out = run('find', 'hotel', 'arena', '--local', local, '--catalogue', cat)
        check(rc == 0 and ids_of(out)[:2] in (['13', '22'], ['22', '13']), 'ambiguity "hotel arena" -> 13 and 22 (got %s)' % out)
        # the planted unknown exits 2 with the five nearest
        rc, out = run('find', 'zzqx', 'qqq', '--local', local, '--catalogue', cat)
        check(rc == 2 and out and out[0].startswith('# NO MATCH\t') and len(ids_of(out)) == 5, 'unknown -> exit 2 + five nearest (got %d %s)' % (rc, out))
        rc, out = run('find', 'Hotl', 'Arenq', 'Xyzzy', 'Plugh', '--kind', 'arena', '--local', local, '--catalogue', cat)
        check(rc == 2 and set(ids_of(out)[:2]) == {'13', '22'}, 'a far query still lists the nearest names first (got %s)' % out)
        for q, want_id in ((['wm30', '--kind', 'arena'], '56'), (['wrestlemania', 'xxx'], '56'), (['WrestleMania', 'XXIX'], '15'), (['wm', '29'], '15'),
                           (['zoe', 'quebec'], '904'), (['ZOË'], '904'), (['#900'], '900'), (['foxtrot'], '950'), (['threads'], 'threads')):
            rc, out = run('find', *q, '--local', local, '--catalogue', cat)
            check(rc == 0 and ids_of(out)[:1] == [want_id], 'find %s -> %s (got %d %s)' % (' '.join(q), want_id, rc, out))
        rc, out = run('find', 'tops', '--local', local, '--catalogue', cat)
        check(rc == 0 and {'cas', 'menu'} <= {ln.split('\t')[0] for ln in out}, 'tops is a cas and a menu label (got %s)' % out)
        rc, out = run('find', 'tops', '--kind', 'cas', '--local', local, '--catalogue', cat)
        check(rc == 0 and {ln.split('\t')[0] for ln in out} == {'cas'} and ids_of(out)[:1] == ['76'], '--kind cas narrows, the plain TOPS first (got %s)' % out)
        # check: fresh, then a changed source is seen
        rc, out = run('check', '--local', local, '--catalogue', cat)
        check(rc == 0 and any(ln.startswith('fresh') for ln in out), 'check is fresh after the build (got %d %s)' % (rc, out[-2:]))
        with open(os.path.join(data, 'db', 'ids', 'arenas.json'), 'a', encoding='utf-8') as f:
            f.write('\n')
        rc, out = run('check', '--local', local, '--catalogue', cat)
        check(rc == 1 and any('CHANGED' in ln and 'arenas.json' in ln for ln in out), 'check sees the changed arenas.json (got %d)' % rc)
        check(any('arenas.json' in w for w in catalogue.stale(cat)), 'stale() names the changed source')
        # the front-end record ids survive without the log (the cache under out\scene)
        os.remove(os.path.join(local, 'logs', 'wwe2k14_001.log'))
        rc, out = run('build', '--data', data, '--local', local, '--game', game, '--out', cat)
        _, ents2 = catalogue.read_catalogue(cat)
        check(rc == 0 and any(e.kind == 'menu' and e.id == '7168' for e in ents2), 'record ids come from the cache once the log is gone')
        os.remove(os.path.join(local, 'out', 'scene', 'front_end_records.json'))
        rc, out = run('build', '--data', data, '--local', local, '--game', game, '--out', cat)
        head3, ents3 = catalogue.read_catalogue(cat)
        check(rc == 0 and any(e.kind == 'menu' and e.id == 'm1.1' for e in ents3) and any(h.startswith('# missing\tmenu\t') for h in head3),
              'without a listing the menu lines keep their names, ids m<menu>.<item>, and menu is named missing')
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print('catalogue selftest: %d checks, %d failed' % (CHECKS[0], len(FAILS)))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
