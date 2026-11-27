"""qgstate_inherit_selftest: q gstate --inherit (S111 GINHERIT) on planted call chains, each case with the planted twin that must FAIL it,
then the codediff hook on the planted chains and the real chains of the q index.
  A  a draw under a node the colour walk calls after its reset: the reset's value      twin no_reset
  B  a caller that sets the field before its call site (and again after it: ignored)  -
  C  a callee the caller calls before the site saves and restores: the caller's value  twin no_restore
  D  a caller's own save / restore around a set: the value above it (its caller's)     twin no_restore
  E  a callee that sets and leaves its value: the callee's                             twin no_callee
  F  two caller paths, two values: UNSETTLED with both                                 twin first_path
  G  a caller that sets it from its own argument: its caller's forwarder row           twin no_forward
  H  the codediff hook: OK / DIFF / UNSETTLED (paths differ, a path open, a callee sets it), with codediff's own judge
  I  draw positions (GINHERIT review): the value at each draw-recorder call under the function, not at its entry: a draw inside
     a callee's set section (twin no_draws), two draws on either side of a callee's restore (UNSETTLED), a callee setting it from
     its argument at the draw (the caller's forwarder row at that call site; twin no_forward)
  M  the colour write mask at the draw from the five recorder words (slot 13 +92 and the channels 14..17 +96..+108, the bits of
     the channel methods sub_82C23980..sub_82C23B60): channels 14 / 15 off -> 0xC (twin mask_raw); a slot-13 set alone -> its 15 / 0;
     codediff's game unit of a slot-13 value (non-zero -> 15)
  N  the node categories from docs/systems/render_categories.md (slot 1 functions, the Reset column, the Re-keys `-> N` moves): a
     node re-keyed into a category with no reset object is open (twin no_rekey)
  real  sub_825A3908 (the character opaque node): one path is the colour walk's reset (alpha test 1, func 5, ref 1 at sub_82C22E88
        +38 / +40 / +48); sub_825185B0 (the crowd cards): cull 1 set by the crowd node sub_82519508 +178 before its call; the real
        doc's five categories with no reset and three re-keyed nodes; sub_82813228 (the circle blobs) draws with mask 0xC
Skipped (said) when the q index is absent. 0 game runs.  Run:  py -I tools/q/qgstate_inherit_selftest.py"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qgstate_inherit as GI  # noqa: E402

fails = []
CW = GI.COLOUR_WALK
SETTER = 0x82BB75E8          # a setter wrapper (never read as a callee)


def check(what, ok, detail=''):
    print('%s %s%s' % ('ok  ' if ok else 'FAIL', what, '' if ok else '  ' + str(detail)[:900]))
    if not ok:
        fails.append(what)


def setrow(off, v, key='alpha_test', root=None, via=None):
    a = {'imm': [v] if not isinstance(v, list) else v, 'root': None} if root is None else {'imm': None, 'root': root}
    return {'off': off, 'kind': 'set', 'key': key, 'fn': SETTER, 'via': via, 'tail': False, 'args': {'r4': a}, 'restore': None, 'value_reg': 'r4'}


def read(off, key='alpha_test'):
    return {'off': off, 'kind': 'read', 'key': key, 'fn': 0x82BB8188, 'tail': False}


def restore(off, save, key='alpha_test'):
    return {'off': off, 'kind': 'restore', 'key': key, 'fn': SETTER, 'via': None, 'tail': False, 'args': {'r4': {'imm': None, 'root': 'ret#X'}},
            'restore': save, 'value_reg': 'r4'}


class Fake:
    """a planted graph: rows {fn: [row]}, callers {fn: [(caller, site offset, how)]}, callees {fn: {off: [t]}}; leaves = the draw
    recorders; cats = (node categories, categories with no reset) as GI.categories gives them"""
    def __init__(self, rows=None, callers=None, callees=None, reset=None, census=None, leaves=(), cats=None):
        self.r, self.c, self.e = rows or {}, callers or {}, callees or {}
        self.rs = reset if reset is not None else {'alpha_test': (1, 0x38), 'alpha_func': (5, 0x40), 'alpha_ref': (1, 0x48)}
        self.cs = census or {}
        self.lv = frozenset(leaves)
        self.ct = cats if cats is not None else ({}, frozenset(), [])

    def draw_leaves(self):
        return self.lv

    def categories(self):
        return self.ct

    def rows(self, f):
        return self.r.get(f, [])

    def callers(self, f):
        return self.c.get(f, [])

    def callees(self, f):
        return self.e.get(f, {})

    def is_setter(self, t):
        return t == SETTER

    def reset(self):
        return self.rs

    def census(self, field):
        return self.cs.get(field)


D, N, P, P2, X, Y, Q = 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700
CASES = {
    'A': (Fake(callers={D: [(N, 0x10, 'call')], N: [(CW, 0x100, 'observed')]}), D, ('settled', 1)),
    'B': (Fake(rows={P: [setrow(0x10, 0), setrow(0x30, 5)]}, callers={D: [(P, 0x20, 'call')], P: [(CW, 0x100, 'observed')]}), D, ('settled', 0)),
    'C': (Fake(rows={P: [setrow(0x10, 0)], X: [read(0x4), setrow(0x8, 1), restore(0xC, 0x4)]}, callers={D: [(P, 0x30, 'call')]},
               callees={P: {0x20: [X], 0x30: [D]}}), D, ('settled', 0)),
    'D': (Fake(rows={P: [read(0x4), setrow(0x8, 0), restore(0x10, 0x4)], Q: [setrow(0x8, 1)]}, callers={D: [(P, 0x20, 'call')], P: [(Q, 0x10, 'call')]}),
          D, ('settled', 1)),
    'E': (Fake(rows={P: [setrow(0x10, 0)], Y: [setrow(0x8, 1)]}, callers={D: [(P, 0x30, 'call')]}, callees={P: {0x20: [Y], 0x30: [D]}}), D, ('settled', 1)),
    'F': (Fake(rows={P: [setrow(0x10, 0)], P2: [setrow(0x10, 1)]}, callers={D: [(P, 0x20, 'call'), (P2, 0x20, 'call')]}), D, ('unsettled', {0, 1})),
    'G': (Fake(rows={P: [setrow(0x10, None, 'alpha_ref', root='in_r6')], Q: [setrow(0x40, 128, 'alpha_ref', via=P)]},
               callers={D: [(P, 0x20, 'call')], P: [(Q, 0x40, 'call')]}), D, ('settled', 128)),
}
TWINS = {'A': 'no_reset', 'C': 'no_restore', 'D': 'no_restore', 'E': 'no_callee', 'F': 'first_path', 'G': 'no_forward'}


def holds(g, fn, want, field='alpha_test'):
    s = GI.Walker(g).summary(fn, field)
    kind, v = want
    if kind == 'settled':
        return s['settled'] and s['value'] == v, s
    return (not s['settled']) and {p['v'] for p in s['paths']} == v, s


def planted():
    for name, (g, fn, want) in sorted(CASES.items()):
        field = 'alpha_ref' if name == 'G' else 'alpha_test'
        GI.PLANT = ''
        ok, s = holds(g, fn, want, field)
        check(f'{name} {want}', ok, [GI.path_text(p) for p in s['paths']])
        if name in TWINS:
            GI.PLANT = TWINS[name]
            ok2, s2 = holds(g, fn, want, field)
            check(f'{name} twin {TWINS[name]} FAILS the case', not ok2, [GI.path_text(p) for p in s2['paths']])
            GI.PLANT = ''


class Units:
    @staticmethod
    def game(field, v):
        return [(field, v)]


def hook_cases():
    try:
        import qcodediff as CD
    except ImportError as e:
        print(f'SKIP H: qcodediff not importable ({e})')
        return
    for name, ours, want in (('A', [1], 'OK'), ('A', [0], 'DIFF'), ('F', [0], 'UNSETTLED'), ('F', [2], 'DIFF')):
        g, fn, _w = CASES[name]
        hook = GI.codediff_hook(Units, CD.judge, CD.MOOT, CD.DERIVED, GI.Walker(g))
        v, why, _x, _i = hook(f'sub_{fn:08X}', 'alpha_test', ours, None)
        check(f'H {name} ours {ours}: {want}', v == want, (v, why))
    g = Fake(callers={D: [(N, 0x10, 'call'), (Q, 0x10, 'call')], N: [(CW, 0x100, 'observed')]})   # Q: no caller found (open)
    v, why, _x, _i = GI.codediff_hook(Units, CD.judge, CD.MOOT, CD.DERIVED, GI.Walker(g))(f'sub_{D:08X}', 'alpha_test', [0], None)
    check('H an open path makes the DIFF UNSETTLED', v == 'UNSETTLED' and 'open' in why, (v, why))
    g = Fake(callers={D: [(N, 0x10, 'call')], N: [(CW, 0x100, 'observed')]}, rows={Y: [setrow(0x8, 0)]}, callees={D: {0x40: [Y]}})
    v, why, _x, _i = GI.codediff_hook(Units, CD.judge, CD.MOOT, CD.DERIVED, GI.Walker(g))(f'sub_{D:08X}', 'alpha_test', [0], None)
    check('H a callee that sets it makes the DIFF UNSETTLED', v == 'UNSETTLED' and 'callee sets it' in why, (v, why))
    GI.PLANT = 'no_inner'
    v2, _w, _x, _i = GI.codediff_hook(Units, CD.judge, CD.MOOT, CD.DERIVED, GI.Walker(g))(f'sub_{D:08X}', 'alpha_test', [0], None)
    GI.PLANT = ''
    check('H twin no_inner FAILS it (DIFF)', v2 == 'DIFF', v2)


LEAF = 0x900                 # a planted draw recorder
MASK_CENSUS = {'colour_write_mask': 15, 'colour_mask_14': 1, 'colour_mask_15': 1, 'colour_mask_16': 1, 'colour_mask_17': 1}


def draw_cases():
    try:
        import qcodediff as CD
    except ImportError as e:
        print(f'SKIP I / M: qcodediff not importable ({e})')
        return
    walk = {D: [(N, 0x10, 'call')], N: [(CW, 0x100, 'observed')]}          # D's entry: the reset's alpha test 1
    # I1: D calls Y at +10; Y sets the test off at +8 and calls the recorder at +20: D draws with 0, not its entry's 1
    g = Fake(rows={Y: [setrow(0x8, 0)]}, callers=walk, callees={D: {0x10: [Y]}, Y: {0x20: [LEAF]}}, leaves=(LEAF,))
    s = GI.Walker(g).draw_summary(D, 'alpha_test')
    check('I1 a draw inside a callee\'s set section: 0 at the draw', s is not None and s['settled'] and s['value'] == 0 and s['draws'] == 1,
          s and [GI.path_text(p) for p in s['paths']])
    v, why, _x, _i = GI.codediff_hook(Units, CD.judge, CD.MOOT, CD.DERIVED, GI.Walker(g))(f'sub_{D:08X}', 'alpha_test', [0], None)
    check('I1 the hook: OK at the draw (no "a callee sets it")', v == 'OK' and 'callee sets it' not in why, (v, why))
    GI.PLANT = 'no_draws'
    v2, why2, _x, _i = GI.codediff_hook(Units, CD.judge, CD.MOOT, CD.DERIVED, GI.Walker(g))(f'sub_{D:08X}', 'alpha_test', [0], None)
    GI.PLANT = ''
    check('I1 twin no_draws FAILS it (the entry value and the callee band-aid)', v2 != 'OK', (v2, why2))
    # I2: Y saves, sets 0, draws (+20), restores (+30); D draws again at +40 after Y: the two draws hold 0 and 1
    g = Fake(rows={Y: [read(0x4), setrow(0x8, 0), restore(0x30, 0x4)]}, callers=walk, callees={D: {0x10: [Y], 0x40: [LEAF]}, Y: {0x20: [LEAF]}},
             leaves=(LEAF,))
    s = GI.Walker(g).draw_summary(D, 'alpha_test')
    check('I2 two draws either side of the restore: UNSETTLED {0, 1}', s is not None and not s['settled'] and {p['v'] for p in s['paths']} == {0, 1}
          and s['draws'] == 2, s and [GI.path_text(p) for p in s['paths']])
    v, why, _x, _i = GI.codediff_hook(Units, CD.judge, CD.MOOT, CD.DERIVED, GI.Walker(g))(f'sub_{D:08X}', 'alpha_test', [0], None)
    check('I2 the hook: UNSETTLED (the game has two values for its two draws)', v == 'UNSETTLED', (v, why))
    # I3: Y sets the ref from its argument r6 and draws; D's forwarder row at its call of Y passes 128
    g = Fake(rows={Y: [setrow(0x8, None, 'alpha_ref', root='in_r6')], D: [setrow(0x10, 128, 'alpha_ref', via=Y)]}, callers=walk,
             callees={D: {0x10: [Y]}, Y: {0x20: [LEAF]}}, leaves=(LEAF,))
    s = GI.Walker(g).draw_summary(D, 'alpha_ref')
    check('I3 a callee setting it from its argument at the draw: the caller\'s forwarder row (128)', s is not None and s['settled'] and s['value'] == 128,
          s and [GI.path_text(p) for p in s['paths']])
    GI.PLANT = 'no_forward'
    s2 = GI.Walker(g).draw_summary(D, 'alpha_ref')
    GI.PLANT = ''
    check('I3 twin no_forward FAILS it', not (s2 and s2['settled'] and s2['value'] == 128), s2 and [GI.path_text(p) for p in s2['paths']])
    # M1: D turns channels 14 / 15 off (+8 / +C), draws (+10), turns them on again (+18 / +1C): the draw's mask is 0xC
    rows = [setrow(0x8, 0, 'colour_mask_14'), setrow(0xC, 0, 'colour_mask_15'), setrow(0x18, 1, 'colour_mask_14'), setrow(0x1C, 1, 'colour_mask_15')]
    g = Fake(rows={D: rows}, callers=walk, callees={D: {0x10: [LEAF]}}, leaves=(LEAF,), census=MASK_CENSUS)
    s = GI.Walker(g).mask_summary(D)
    check('M1 channels 14 / 15 off at the draw: mask 0xC', s['settled'] and s['value'] == 0xC, [GI.path_text(p) for p in s['paths']])
    v, why, _x, _i = GI.codediff_hook(Units, CD.judge, CD.MOOT, CD.DERIVED, GI.Walker(g))(f'sub_{D:08X}', 'colour_write_mask', [4], None)
    check('M1 the hook: ours 0x4 is a DIFF against the game\'s 0xC (not 0xF)', v == 'DIFF' and 'the game has 0xC' in why, (v, why))
    v, why, _x, _i = GI.codediff_hook(Units, CD.judge, CD.MOOT, CD.DERIVED, GI.Walker(g))(f'sub_{D:08X}', 'colour_write_mask', [0xC], None)
    check('M1 the hook: ours 0xC is OK', v == 'OK', (v, why))
    GI.PLANT = 'mask_raw'
    v2, why2, _x, _i = GI.codediff_hook(Units, CD.judge, CD.MOOT, CD.DERIVED, GI.Walker(g))(f'sub_{D:08X}', 'colour_write_mask', [0xC], None)
    GI.PLANT = ''
    check('M1 twin mask_raw (the slot-13 word alone) FAILS it', v2 != 'OK', (v2, why2))
    # M2: a slot-13 set of 0 before the draw, no channel set: the row records 0
    g = Fake(rows={D: [setrow(0x8, 0, 'colour_write_mask')]}, callers=walk, callees={D: {0x10: [LEAF]}}, leaves=(LEAF,), census=MASK_CENSUS)
    s = GI.Walker(g).mask_summary(D)
    check('M2 a slot-13 set of 0 alone: mask 0', s['settled'] and s['value'] == 0, [GI.path_text(p) for p in s['paths']])
    u = CD.Units(None)
    check("M3 codediff's game unit of a slot-13 value: 1 -> 15, 0 -> 0 (sub_82C23910 records 15 / 0)",
          u.game('colour_write_mask', 1) == [('colour_write_mask', 15)] and u.game('colour_write_mask', 0) == [('colour_write_mask', 0)],
          (u.game('colour_write_mask', 1), u.game('colour_write_mask', 0)))


CAT_DOC = '''
| Cat | What registers there (builder / slot 1) | Reset | State | Passes | Re-keys | Ours | Marks |
|---|---|---|---|---|---|---|---|
| 13..21 | the node (`sub_00000100`, slot 1 `sub_00000200`) | R | x | C | -> 41 at an entrance | - | [C] |
| 41 | nothing registers | **-** | inherits 40's | C | from 13 | - | [C] |
| 22 | the other node (slot 1 `sub_00000600`) | R | x | C | - | - | [C] |
'''


def cat_cases():
    node_cats, no_reset, _p = GI.categories(CAT_DOC)
    check('N the planted doc: node 0x200 in 13..21 and 41 (its re-key), 41 has no reset',
          set(node_cats.get(N, ((), ''))[0]) == set(range(13, 22)) | {41} and no_reset == frozenset({41}), (node_cats, no_reset))
    g = Fake(callers={D: [(N, 0x10, 'call')], N: [(CW, 0x100, 'observed')]}, cats=GI.categories(CAT_DOC))
    s = GI.Walker(g).summary(D, 'alpha_test')
    check('N a node re-keyed into a category with no reset: open', not s['settled'] and any(p['open'] and '41' in p['open'] for p in s['paths']),
          [GI.path_text(p) for p in s['paths']])
    GI.PLANT = 'no_rekey'
    g = Fake(callers={D: [(N, 0x10, 'call')], N: [(CW, 0x100, 'observed')]}, cats=GI.categories(CAT_DOC))
    s2 = GI.Walker(g).summary(D, 'alpha_test')
    GI.PLANT = ''
    check('N twin no_rekey FAILS it (settled from the reset)', s2['settled'], [GI.path_text(p) for p in s2['paths']])


def real():
    try:
        import q as Q_
        con = Q_.connect(Q_.DB)
        con.execute('SELECT 1 FROM edges LIMIT 1')
    except Exception as e:  # noqa: BLE001 - no index: said, exit 0
        print(f'SKIP real: no q index ({e})')
        return
    r = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py'), 'gstate', '--inherit', 'sub_825A3908', 'sub_825185B0', '--json'],
                       capture_output=True, text=True, timeout=600)
    try:
        doc = json.loads(r.stdout)
    except ValueError:
        check('real: --inherit --json answers', False, r.stdout[-400:] + r.stderr[-400:])
        return
    got = {(f['fn'], f['field']): f for f in doc['inherited']}

    def paths(fn, fld):
        v = got[(fn, fld)]['value']
        return v['unsettled'] if isinstance(v, dict) else [{'value': v, 'at': got[(fn, fld)]['at'], 'via': got[(fn, fld)]['via']}]
    for fld, v, at in (('alpha_test', 1, '0x82C22EC0'), ('alpha_func', 5, '0x82C22EC8'), ('alpha_ref', 1, '0x82C22ED0')):
        ps = paths('sub_825A3908', fld)
        check(f'real sub_825A3908 {fld}: a path takes the category reset {v} @{at} through the colour walk',
              any(p['value'] == v and p['at'] == at and 'sub_82C2FA10' in p['via'] for p in ps), ps[:4])
    c = got[('sub_825185B0', 'cull')]
    check('real sub_825185B0 cull: 1, set by the crowd node sub_82519508 +178', c['value'] == 1 and c['at'] == '0x82519680', c)
    node_cats, no_reset, probs = GI.categories()
    check('real render_categories.md: the five categories with no reset object (32, 34, 41, 61, 63), the layout qwhen reads',
          no_reset == frozenset({32, 34, 41, 61, 63}) and not probs, (sorted(no_reset), probs))
    for fn, want in ((0x825A3908, set(range(13, 22)) | {42}), (0x825ABC20, {33, 43}), (0x8272D378, {26, 41, 43})):
        got_c = set(node_cats.get(fn, ((), ''))[0])
        check(f'real {GI.sub(fn)}: categories {sorted(want)} from the doc (slot 1 row + re-key)', got_c == want, sorted(got_c))
    w = GI.Walker(GI.Graph(con))
    s = w.mask_summary(0x82813228)
    check('real sub_82813228 (the circle blobs): mask 0xC at its draw (channels 14 / 15 off at +CC / +D8, 16 / 17 and +92 the start 1)',
          s['settled'] and s['value'] == 0xC and s.get('draws') == 1, [GI.path_text(p) for p in s['paths']][:4])
    s = w.draw_summary(0x825185B0, 'cull')
    check('real sub_825185B0 cull at its draws: 1', s is not None and s['settled'] and s['value'] == 1, s and [GI.path_text(p) for p in s['paths']][:4])


planted()
hook_cases()
draw_cases()
cat_cases()
real()
print('qgstate_inherit_selftest: ' + ('FAILED: ' + '; '.join(fails) if fails else 'all passed'))
sys.exit(1 if fails else 0)
