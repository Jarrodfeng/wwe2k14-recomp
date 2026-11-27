r"""Checks of `q exprdiff --values` (tools/q/qexprvals.py) on invented records (no game data): called by qexprdiff_selftest.py part A (A8).

A8a  a row whose two sides are one parameter each: 5 records, 2 differ -> `2 of 5 records differ`, 2 example ids with both values;
A8b  the same row on records where the game's parameter is missing: counted apart (the game writes nothing), not compared;
A8c  a row with the same values on every record = `0 of M records differ` (a structural DIFF with no data difference);
A8d  a game image constant (code leaf in GAME_CODE_VALUES) against ours' default: the records that lack the name differ, the others do not;
A8e  the Material default parser (initializer lists, scalars, comments masked);
A8f  an ours leaf that is no record value (ours:c.x) = `not a record value`.
The planted twin no_ours_default (WWE_QEXPRDIFF_PLANT, run by qexprdiff_selftest) makes ours' default 0: A8a must then fail.
"""
import json

import qexprvals as V


class FakeRecords:
    def __init__(self, rows):
        self.rows = [(p, n, i, json.dumps(p)) for p, n, i in rows]

    def sets(self, effect):
        return self.rows

    def ids(self, effect, params_text, n=3):
        return [next(i for p, _n, i, t in self.rows if t == params_text)]


def run(X, check):
    ctx = V.Ctx({'ych': {'a[0]': [('g_fB', 0)], 'k[0]': [('g_fvK', 0)]}}, {'a[0]': 1.0, 'k[0]': 10.0})
    row = {'effect': 'yCh_Fake', '_g': ('leaf', 'mat:g_fA[0]'), '_o': ('leaf', 'mt:a[0]'), '_gn': ('g_fA', 0)}
    recs = FakeRecords([({'g_fA': [1.0], 'g_fB': [1.0]}, 1, 11), ({'g_fA': [2.0], 'g_fB': [2.0]}, 1, 12), ({'g_fA': [1.0]}, 1, 13),
                        ({'g_fA': [0.0]}, 1, 14), ({'g_fA': [5.0], 'g_fB': [7.0]}, 1, 15)])
    gv = V.group_values(X, [row], recs, ctx)
    ok = gv.get('m') == 5 and gv.get('differ') == 2 and [e[0] for e in gv['examples']] == [14, 15] and (gv['examples'][0][2], gv['examples'][0][3]) == (0.0, 1.0)
    check('A8a a planted pair: 5 records, 2 differ prints 2 (default 1 against the game 0; 5 against 7)', ok, str(gv))
    out = V.lines(gv, 'DIFF')
    check('A8a the printed line', out[0].startswith('values: DIFF, 2 of 5 records differ') and len(out) == 3 and 'game 0, ours 1' in out[1], str(out))
    gv = V.group_values(X, [row], FakeRecords([({'g_fB': [3.0]}, 2, 21), ({'g_fA': [3.0], 'g_fB': [3.0]}, 1, 22)]), ctx)
    check('A8b a record without the game parameter: counted apart, not compared', gv['m'] == 3 and gv['absent'] == 2 and gv['differ'] == 0, str(gv))
    gv = V.group_values(X, [row], FakeRecords([({'g_fA': [3.0], 'g_fB': [3.0]}, 4, 31)]), ctx)
    check('A8c same values on every record: 0 of 4 differ', gv['m'] == 4 and gv['differ'] == 0 and V.lines(gv, 'DIFF')[0].startswith('values: DIFF, 0 of 4 records differ'), str(gv))
    row2 = {'effect': 'yCh_Fake', '_g': ('leaf', 'code:g_fvK[0]'), '_o': ('leaf', 'mt:k[0]'), '_gn': None}
    V.GAME_CODE_VALUES['g_fvK'] = ((100.0, 0, 0, 0), 'invented')
    try:
        gv = V.group_values(X, [row2], FakeRecords([({'g_fvK': [10.0]}, 1, 41), ({'g_fvK': [100.0]}, 1, 42), ({}, 3, 43)]), ctx)
    finally:
        del V.GAME_CODE_VALUES['g_fvK']
    check('A8d the game constant against ours default: the 3 records that lack the name differ (100 / 10), the 2 that name it are equal',
          gv['m'] == 5 and gv['lack'] == 3 and gv['differ'] == 3 and gv['lack_names'] == {'g_fvK'}, str(gv))
    d = V.material_defaults('struct Material {\n  float a[2] = {1, 2};\n  float b = 300;\n  float c = 0.5f, d[3] = {0, 0, 1};\n  bool e = false;\n};\nint z = 9;')
    check('A8e Material defaults parsed', d == {'a[0]': 1.0, 'a[1]': 2.0, 'b': 300.0, 'c': 0.5, 'd[0]': 0.0, 'd[1]': 0.0, 'd[2]': 1.0, 'e': 0.0}, str(d))
    r1 = {'input': 'yCh_Reflect:ps.c204.w', 'name': 'Reflect', 'lane': 'w', 'game_name': 'g_f4Ref_COL'}
    r2 = {'input': 'yCh_skin:ps.c204.w', 'name': 'Reflect', 'lane': 'w', 'game_name': 'g_f4Ref_COL'}
    r3 = {'input': 'yCh_skin:ps.c205.x', 'name': 'Fairy', 'lane': 'x', 'game_name': 'g_fFairy'}
    check('A9 --input: a constant\'s name exact / lane / prefix / game name matches every effect that binds it; a glob and another name still behave',
          all(X.input_match(r, p) for r in (r1, r2) for p in ('Reflect', 'reflect.w', 'Refl', 'g_f4Ref', 'g_f4Ref_COL', 'yCh_*:ps.c204.w'))
          and not X.input_match(r3, 'Reflect') and not X.input_match(r1, 'Reflect.x') and not X.input_match(r1, 'lect'), '')
    gv = V.group_values(X, [dict(row, _o=('leaf', 'ours:c.x'))], recs, ctx)
    check('A8f an ours context leaf = not a record value', 'why' in gv and 'ours:c.x' in gv['why'], str(gv))
