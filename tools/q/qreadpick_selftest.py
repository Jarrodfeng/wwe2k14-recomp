"""Selftest of q read pick --all (S110 PICKALL). Temp data; the real-index part is skipped without an index.

    py -I tools/q/qreadpick_selftest.py
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402
import qread  # noqa: E402
import qreadpick as P  # noqa: E402

fails = []


def check(ok, what):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        fails.append(what)


def fake(kw):
    """Data of units 1..: kw = {unit: dict(text=[...], nbr={...}, dbsys=[...], ev=N, n=funcs)}"""
    members = {u: list(range(v.get('n', 3))) for u, v in kw.items()}
    return P.Data(members, {u: set(v.get('nbr', ())) for u, v in kw.items()}, {u: v.get('text', []) for u, v in kw.items()},
                  {u: v.get('dbsys', []) for u, v in kw.items()}, {u: v.get('ev', 0) for u, v in kw.items()}, {})


def main():
    # rule (a): two read neighbours say audio; the unit's own words say menu (b would pick menus), so (a) must win
    d = fake({1: dict(nbr=(10, 11), text=['menu', 'menu_page']), 10: {}, 11: {}})
    check(P.classify(1, d, {10: 'audio', 11: 'audio'})[0] == 'audio', 'rule a: majority of read neighbours wins over keywords')
    g, why = P.classify(1, d, {10: 'audio', 11: 'audio'})
    check(why.startswith('neighbours'), 'rule a keeps its reason (%s)' % why)
    # tie and fewer than 2 votes: no answer, falls through to (b)
    check(P.neighbours_rule(1, d, {10: 'audio', 11: 'ai'}) is None, 'rule a: a tie gives no answer')
    check(P.classify(1, d, {10: 'audio', 11: 'ai'})[0] == 'menus', 'rule a tie falls through to the keywords')
    check(P.neighbours_rule(1, d, {10: 'audio'}) is None, 'rule a: one read neighbour gives no answer')
    check(P.neighbours_rule(1, d, {10: 'pose', 11: 'pose'}) is None, 'rule a: a system with no group casts no vote')
    # rule (b)
    d = fake({2: dict(text=['CAW_EditMenu', 'create_caw'])})
    r = P.classify(2, d, {})
    check(r[0] == 'creation' and r[1].startswith('keywords'), 'rule b: caw/create/edit beat one menu hit (%s)' % (r,))
    d = fake({3: dict(text=['sound_menu'])})
    check(P.keyword_rule(3, d, {}) is None, 'rule b: a tie of hits (sound, menu) gives no answer')
    d = fake({4: dict(text=['menu'])})
    check(P.keyword_rule(4, d, {}) is None, 'rule b: one hit is not enough')
    d = fake({5: dict(text=['CNonBlock::ReadEndProcess', 'readEndTick'])})
    check(P.keyword_rule(5, d, {})[0] == 'assets', 'rule b: ReadEnd pair matches assets')
    check(P.kw_hits(['maintain', 'again']).get('ai') is None, 'rule b: `ai` needs a whole token')
    # rule (c)
    d = fake({6: dict(dbsys=['camera', 'camera', 'other', 'render'])})
    r = P.classify(6, d, {})
    check(r[0] == 'camera' and r[1].startswith('db system'), 'rule c: db system camera (other and render ignored) (%s)' % (r,))
    d = fake({7: dict(dbsys=['camera', 'audio'])})
    check(P.dbsystem_rule(7, d, {}) is None, 'rule c: a tie gives no answer')
    # rule (d)
    d = fake({8: dict(text=['zzz'], dbsys=['other'])})
    check(P.classify(8, d, {}) == ('rest', 'no rule fired'), 'rule d: rest')
    # plan: read units skipped, units < 3 funcs skipped, order group then evidence, --group
    d = fake({20: dict(text=['audio', 'sound'], ev=1), 21: dict(text=['caw', 'create'], ev=1), 22: dict(text=['caw', 'edit'], ev=9),
                23: dict(text=['xxx'], ev=5), 24: dict(text=['caw', 'edit'], ev=50), 25: dict(text=['caw', 'edit'], n=2, ev=99)})
    rows = P.plan(d, {24}, {})
    check([r['unit'] for r in rows] == [22, 21, 20, 23], 'plan: read unit 24 and 2-function unit 25 skipped, group then evidence (got %s)' % [r['unit'] for r in rows])
    check([r['unit'] for r in P.plan(d, {24}, {}, 'audio')] == [20], 'plan: --group restricts')
    # a stored reading (real qread store) marks a unit read
    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, 'read.s2.db')
    qread.store('## z\nunit: 22\npurpose: p | confidence: low | because: b\nsystem: audio\nmissing: m\nlisting needed: no\n', 'b', 'm', db)
    answered, rs = P.read_state(db)
    check(answered == {22} and rs == {22: 'audio'}, 'read_state: reads the stored unit and its system (got %s %s)' % (answered, rs))
    rows = P.plan(d, answered, rs)
    check(22 not in [r['unit'] for r in rows], 'plan: a stored reading is skipped')
    # --count adds up
    sizes = {r['unit']: 1048576 for r in rows}
    tab, lines = P.count_table(rows, sizes)
    check(sum(t[1] for t in tab) == len(rows) and sum(t[2] for t in tab) == len(rows) * 1048576, 'count: units and bytes add up')
    check(lines[-1].split()[1] == str(len(rows)), 'count: total line (%s)' % lines[-1])
    check([t[0] for t in tab] == list(P.GROUPS), 'count: group order')

    sys.path.insert(0, HERE)
    import qreadfrom as F  # noqa: E402

    def graph(edges, uof):
        return F.make_graph(edges, uof)
    # --from: chain E(1,unit 1) -> 2 (u2) -> 3 (u3) -> 4 (u4) -> 5 (u5); unit 9 (func 90) is called from 2 and 3
    uof = {1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 90: 9, 91: 9}
    base = [(1, 2), (2, 3), (3, 4), (4, 5), (2, 90), (3, 90), (90, 91)]
    g = graph(base, uof)
    g.expand = 'none'
    r = F.run(g, [1], 3, 25)
    check([u for u, _d in r['units']] == [1, 2, 9, 3, 4] or [u for u, _d in r['units']] == [1, 2, 3, 9, 4],
          'from: depth 3 reaches units 1-4 and 9, not 5 (got %s)' % r['units'])
    check(5 not in [u for u, _d in r['units']] and dict(r['units'])[4] == 3, 'from: depth is a hard stop (unit 4 at 3, unit 5 absent)')
    check(dict(F.run(g, [1], 4, 25)['units']).get(5) == 4, 'from: depth 4 reaches unit 5')
    check(r['units'][0] == (1, 0), 'from: the entry unit comes first')
    check(r['funcs'][91] == 3 and 91 in r['funcs'], 'from: a size-1/2 unit (9: 2 funcs, 91 reached by the walk) is included')
    # library cut: 30 other units call func 90; K 25 cuts unit 9, and what is only behind it (91) with it
    lib = base + [(100 + i, 90) for i in range(30)]
    uof2 = dict(uof)
    uof2.update({100 + i: 100 + i for i in range(30)})
    g = graph(lib, uof2)
    g.expand = 'none'
    r = F.run(g, [1], 3, 25)
    names = [u for u, _d in r['units']]
    check(9 not in names and r['cut'] == [9] and 91 not in r['funcs'], 'library: unit 9 (30 outside callers > K 25) is cut and not walked through')
    check(2 in names and 3 in names, 'library: the chain itself stays')
    r = F.run(g, [1], 3, 40)
    check(9 in [u for u, _d in r['units']], 'library: K 40 keeps it (30 callers is not more than 40)')
    # callers INSIDE the reach do not count: the same fan-in from units the reach holds
    check(sorted(graph(base + [(2, 90)], uof).ucallers[9]) == [2, 3], 'library: callers count as distinct UNITS (a repeated edge and a same-unit call add none)')

    def lib_ok(res):
        return 9 not in [u for u, _d in res['units']] and res['cut'] == [9]
    # the planted twin: no library cut at all. The library check must PASS the real walk and FAIL the twin
    check(lib_ok(F.run(g, [1], 3, 25)), 'library check passes the real walk')
    check(not lib_ok(F.run(g, [1], 3, None)), 'library check FAILS the planted twin (no cut: unit 9 listed, nothing cut)')
    # expand all: a unit joining the reach walks with all its functions (unit 2 has a second function 20 that calls 6)
    uof3 = {1: 1, 2: 2, 20: 2, 6: 6}
    g = graph([(1, 2), (20, 6)], uof3)
    g.expand = 'none'
    check(6 not in F.run(g, [1], 3, 25)['funcs'], 'expand none: the unit sibling is not walked')
    g.expand = 'all'
    check(6 in F.run(g, [1], 3, 25)['funcs'], 'expand all: the unit sibling walks and reaches 6')
    # SYSPICK2 classes: entry 1 (unit 1) calls ctor 2 (unit 2) which builds a table whose slots are funcs 30 (unit 30) and 40 (unit 40); nobody calls them
    uofc = {1: 1, 2: 2, 30: 30, 31: 31, 40: 40}
    gc = graph([(1, 2), (30, 31)], uofc)
    gc.expand = 'none'
    gc.cls = {2: [30, 40]}

    def cls_ok(res):
        us = dict(res['units'])
        return us.get(30) == 1 and us.get(40) == 1 and 31 not in res['funcs']
    check(cls_ok(F.run(gc, [1], 1, 25)), 'classes: the ctor (depth 1) brings its vtable slots 30, 40 in at the same depth')
    check(dict(F.run(gc, [1], 1, 25)['units']).get(30) == 1 and F.run(gc, [1], 2, 25)['funcs'].get(31) == 2, 'classes: a slot walks on (its callee 31 at depth 2)')
    gc.use_cls = False
    check(not cls_ok(F.run(gc, [1], 2, 25)), 'classes check FAILS the planted twin (classes off: slots never reached)')
    # a unit first entered by a slot (only that function) is brought in whole when a call reaches it later: unit 30 has sibling 32
    gp = graph([(1, 2), (1, 30)], {1: 1, 2: 2, 30: 30, 32: 30})
    gp.expand = 'all'
    gp.cls = {1: [30]}
    check(32 in F.run(gp, [1], 2, 25)['funcs'], 'classes: a slot unit later reached by a call walks with all its functions')
    # caller-only stray: unit 7 calls reached function 2 but nothing reached calls 7
    gs = graph([(1, 2), (7, 2), (2, 3)], {1: 1, 2: 2, 3: 3, 7: 7})
    gs.expand = 'none'

    def stray_ok(res):
        return 7 not in [u for u, _d in res['units']] and {1, 2, 3} <= {u for u, _d in res['units']}

    def two_way(g, seeds, depth):                # the planted twin: callers are walked too
        rev = {}
        for f, cs in g.inn.items():
            for c in cs:
                rev.setdefault(c, set()).add(f)
        return F.reach(seeds, {f: set(g.out.get(f, ())) | set(g.inn.get(f, ())) for f in set(g.out) | set(g.inn)}, g.uof, g.ucallers, g.members, depth, 25, 'none')
    check(stray_ok(F.run(gs, [1], 3, 25)), 'direction: unit 7 (only a caller of reached 2) is not reached')
    check(F.direction_strays(gs, F.run(gs, [1], 3, 25)) == 1, 'direction: the report counts 1 unit cut by direction')
    check(not stray_ok(two_way(gs, [1], 3)), 'direction check FAILS the planted twin (a two-way walk reaches caller unit 7)')
    check(F.parse_entries('0x82866D80, sub_83328D40 8333BF78') == [0x82866D80, 0x83328D40, 0x8333BF78], 'entries parse')

    if not os.path.exists(qnames.INDEX_DB):
        print('SKIP  real-index part (no index)')
    else:
        import qsheet
        ctx = qsheet.Ctx()
        data = P.load_data(ctx)
        answered, rs = P.read_state()
        rows = P.plan(data, answered, rs)
        big = {r[0] for r in ctx.units.execute('select unit_id from unit where n_funcs>=3')}
        check(len(rows) == len(big - answered), 'real: every unread unit of >= 3 functions is planned (%d of %d)' % (len(rows), len(big)))
        check(all(r['group'] in P.GROUPS and r['reason'] for r in rows), 'real: every unit has a group and a reason')
    print('%d failure(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
