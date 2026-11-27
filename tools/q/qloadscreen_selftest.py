"""Self-test of q loadscreen (S111 QFIX4): `py -I tools/q/qloadscreen_selftest.py`   (0 game runs; planted tables, then the real index + the player's LoadHD.pac when present)

A. pattern_rx: /LOAD/GN%02d matches GN07 and nothing else; %04d, %s, a literal.
B. classes_by_id on a planted builder: each constructor call goes to the case body that holds its site, ids above the bound take the default body, a call to a
   function that stores no vptr is ignored.
C. range_text / pach_groups on a planted asset db: PACHes with the same layouts + textures group, the odd one stands alone, the common textures are not listed.
D. planted twins that must FAIL: a call site assigned to the NEXT body instead of the one holding it; PACHes grouped by name only (the textures differ).
E. the real tables (skipped without the index / the files): the M100_ART Q14.1 table re-found: class of ids 1 / 2 / 3 / 4 / 5 / 8 / 12 / 14 / 17 / 18 / 19 / 21, their
   patterns, PACH counts (GN 32, IG 26, UN 22, SC 10 ...), scenes and textures; class 8 (SNT) in none of the files.
Exit 0 = pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import contextlib
import io
import json
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qloadscreen as L  # noqa: E402

B = L.BUILDER
BODIES = [(0x1000, [0]), (0x1100, [1, 10]), (0x1200, [2]), (0x1300, [3])]      # case bodies in address order; the default body is 0x1100's
FAKE = {'bodies': BODIES, 'bound': 3, 'default': 0x1100, 'bias': 0}


def planted():
    con = sqlite3.connect(':memory:')
    con.execute('CREATE TABLE edges (src INT, dst INT, site INT, kind TEXT)')
    for site, dst in ((0x1108, 0x5100), (0x1208, 0x5200), (0x1308, 0x5300), (0x1304, 0x7777)):
        con.execute('INSERT INTO edges VALUES (?, ?, ?, ?)', (B, dst, site, 'call'))
    fdb = sqlite3.connect(':memory:')
    fdb.execute('CREATE TABLE ctors (func INT, vt INT)')
    for f, vt in ((0x5100, 0x9100), (0x5200, 0x9200), (0x5300, 0x9300)):
        fdb.execute('INSERT INTO ctors VALUES (?, ?)', (f, vt))
    return con, fdb


def class_checks():
    bad = []
    keep = L.case_map
    L.case_map = lambda con, addr=B: FAKE
    try:
        con, fdb = planted()
        r = L.classes_by_id(con, fdb)
        want = {1: 0x5100, 10: 0x5100, 2: 0x5200, 3: 0x5300}
        for i, c in want.items():
            if not r['ids'].get(i) or r['ids'][i][0] != c:
                bad.append(f'id {i}: want ctor {c:#x}, got {r["ids"].get(i)}')
        if r['ids'].get(0) is not None:
            bad.append('id 0: no ctor call in its body, want none')
        if not r['default'] or r['default'][0] != 0x5100:
            bad.append(f'default (ids > 3): want the 0x1100 body ctor 0x5100, got {r["default"]}')
        if any(v and v[0] == 0x7777 for v in r['ids'].values()):
            bad.append('a callee that stores no vptr was taken as a class')
    finally:
        L.case_map = keep
    return bad


def asset_db():
    c = sqlite3.connect(':memory:')
    c.row_factory = sqlite3.Row
    c.execute('CREATE TABLE pach (id INT, pac_id INT, idx INT, vpath TEXT)')
    c.execute('CREATE TABLE entry (id INT, pach_id INT, kind TEXT)')
    c.execute('CREATE TABLE texture (id INT, entry_id INT, name TEXT)')
    tex = {0: ['a', 'b', 'wwe_circle'], 1: ['a', 'b', 'wwe_circle'], 2: ['a', 'z']}
    for i in range(3):
        c.execute('INSERT INTO pach VALUES (?, 1, ?, ?)', (i, 10 + i, f'/LOAD/GN0{i}'))
        c.execute('INSERT INTO entry VALUES (?, ?, "texdir")', (i, i))
        for k, t in enumerate(tex[i]):
            c.execute('INSERT INTO texture VALUES (?, ?, ?)', (i * 10 + k, i, t))
    return c


def group_checks(by_name_only=False):
    bad = []
    c = asset_db()
    lay = {10: [('p10/2', {'scenes': ['S'], 'sets': ['x']})], 11: [('p11/2', {'scenes': ['S'], 'sets': ['x']})], 12: [('p12/2', {'scenes': ['S'], 'sets': ['x']})]}
    keep = L.sig_of
    if by_name_only:
        L.sig_of = lambda acon, pac_id, idx, lay: ((), ())
    try:
        g = L.pach_groups(c, 1, [(10, '/LOAD/GN00'), (11, '/LOAD/GN01'), (12, '/LOAD/GN02')], lay)
    finally:
        L.sig_of = keep
    if [len(x['pachs']) for x in g] != [2, 1] or g[0]['textures'] != ['a', 'b'] or g[1]['textures'] != ['a', 'z']:
        bad.append(f'pach_groups: want [2 PACHes a b][1 PACH a z] (wwe_circle not listed), got {[(len(x["pachs"]), x["textures"]) for x in g]}')
    if 'GN00..GN01 (2 PACHes, PACH 10-11)' not in L.range_text([(10, '/LOAD/GN00'), (11, '/LOAD/GN01')]):
        bad.append(f'range_text: {L.range_text([(10, "/LOAD/GN00"), (11, "/LOAD/GN01")])!r}')
    return bad


def rx_checks():
    bad = []
    rx = L.pattern_rx('/LOAD/GN%02d')
    if not (rx.match('/LOAD/GN07') and not rx.match('/LOAD/GN7') and not rx.match('/LOAD/UN07') and not rx.match('/LOAD/GN071')):
        bad.append('pattern_rx %02d')
    if not (L.pattern_rx('/LOAD/%04d').match('/LOAD/9001') and not L.pattern_rx('/LOAD/%04d').match('/LOAD/GN00')):
        bad.append('pattern_rx %04d')
    if not (L.pattern_rx('/LOAD/SC%s').match('/LOAD/SC03') and L.pattern_rx('/LOAD/ON01').match('/LOAD/ON01') and not L.pattern_rx('/LOAD/ON01').match('/LOAD/ON02')):
        bad.append('pattern_rx %s / literal')
    return bad


def real_checks():
    try:
        import q as qm
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = qm.main(['loadscreen', '--all', '--json'])
        if rc != 0:
            return [], f'q loadscreen exit {rc}'
        data = json.loads(buf.getvalue())
    except BaseException as e:      # noqa: BLE001 - no index / no game files (SystemExit too)
        return [], f'{type(e).__name__}: {e}'
    by = {}
    for c in data['classes']:
        for i in c['ids']:
            by[i] = c
        if c['default']:
            by['default'] = c
    bad = []

    def need(i, ctor, pat, n, scene=None, tex=None):
        c = by.get(i)
        if not c or c['ctor'] != ctor:
            bad.append(f'id {i}: want class {ctor}, got {c and c["ctor"]}')
            return
        p = [x for x in c['patterns'] if x['pattern'] == pat]
        if not p or p[0]['pachs'] != n:
            bad.append(f'id {i}: want {pat} with {n} PACHes, got {[(x["pattern"], x["pachs"]) for x in c["patterns"]]}')
            return
        if scene and not any(scene in lf['scenes'] for g in p[0]['groups'] for lf in g['leaves']):
            bad.append(f'id {i}: want scene {scene}')
        if tex and not any(set(tex) <= set(g['textures']) for g in p[0]['groups']):
            bad.append(f'id {i}: want textures {tex}')
    need(1, 'sub_82807388', '/LOAD/GN%02d', 32, 'Draw_load_general', ['SS01', 'SS05'])
    need('default', 'sub_82807388', '/LOAD/GN%02d', 32)
    need(2, 'sub_828086C8', '/LOAD/IG%02d', 26, 'Draw_ingame')
    need(3, 'sub_82811CE0', '/LOAD/WS01', 1, 'Draw_WSD', ['WSD01'])
    need(4, 'sub_82811B48', '/LOAD/SC%s', 10, 'WSD_scene')
    need(5, 'sub_82811640', '/LOAD/SM%s', 11, 'WSDmatch_load')
    need(12, 'sub_8280A8F0', '/LOAD/ON01', 1, 'Draw_online')
    need(14, 'sub_82809480', '/LOAD/UN%02d', 22, 'Draw_load_universe')
    need(17, 'sub_82809EB8', '/LOAD/UNDF', 1, 'DrawLoadDraft')
    need(18, 'sub_82807418', '/LOAD/G2%02d', 1, 'Draw_load_general2')
    need(19, 'sub_82807EE8', '/LOAD/ST00', 1)
    need(21, 'sub_82807FC0', '/LOAD/ST02', 1)
    c8 = by.get(8)
    if not c8 or not c8['patterns'] or any(p['pachs'] for p in c8['patterns']):
        bad.append('id 8 (SNT): want /LOAD/SNT* patterns with no PACH in the files')
    return bad, None


def main():
    fails = rx_checks() + class_checks() + group_checks()
    twins = {'call site assigned to the next body': None, 'PACHes grouped by name only': None}
    missed = []
    keep = L.body_start
    L.body_start = lambda starts, site: next((s for s in starts if s > site), None)
    try:
        if not class_checks():
            missed.append('call site assigned to the next body')
    finally:
        L.body_start = keep
    if not group_checks(by_name_only=True):
        missed.append('PACHes grouped by name only')
    rb, skipped = real_checks()
    fails += rb
    for f in fails:
        print('FAIL', f)
    for m in missed:
        print('NOT CAUGHT', m)
    print(f"qloadscreen_selftest: planted {'ok' if not fails else 'FAILED'}; {len(twins) - len(missed)}/{len(twins)} planted twins caught; "
          + (f'real tables skipped ({skipped})' if skipped else f"real tables {'ok' if not rb else 'FAILED'}"))
    return 1 if fails else (2 if missed else 0)


if __name__ == '__main__':
    sys.exit(main())
