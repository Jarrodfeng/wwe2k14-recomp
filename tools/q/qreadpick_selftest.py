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
