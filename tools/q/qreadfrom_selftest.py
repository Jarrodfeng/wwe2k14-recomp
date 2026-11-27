"""Selftest of the reach-path line and `q read system NAME --from` (S111 PICKPATH). Temp data only.

    py -I tools/q/qreadfrom_selftest.py

Each check has a planted twin that must FAIL it: the twin is the old behaviour (no via data; the bundle picked by the system field;
no --max cut), run through the same check function.
"""
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qread  # noqa: E402
import qreadfrom as F  # noqa: E402
import qreadsys  # noqa: E402

fails = []


def check(ok, what):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        fails.append(what)


def main():
    # chain: entries 1 and 7 (units 1, 7); 1 -> 2 -> 3 (units 2, 3); 7 -> 3 also, but 2 comes first in order; 7 -> 8
    uof = {1: 1, 2: 2, 3: 3, 7: 7, 8: 8}
    g = F.make_graph([(1, 2), (2, 3), (7, 8)], uof)
    g.expand = 'none'
    r = F.run(g, [1, 7], 3, 25)

    def path_ok(res):
        if 'via' not in res:
            return False
        ln = {u: F.reached_line(res, uof, u, d) for u, d in res['units']}
        return (ln[1] == 'reached: depth 0 (entry)' and ln[7] == 'reached: depth 0 (entry)'
                and ln[2] == 'reached: depth 1 from sub_00000001 via sub_00000001 (unit 1)'
                and ln[3] == 'reached: depth 2 from sub_00000001 via sub_00000002 (unit 2)'
                and ln[8] == 'reached: depth 1 from sub_00000007 via sub_00000007 (unit 7)')
    check(path_ok(r), 'path line: unit 3 names parent sub_2 and seed sub_1 (the first path), unit 8 names seed sub_7, entries say entry (%s)' %
          [F.reached_line(r, uof, u, d) for u, d in r['units']])
    twin = {k: v for k, v in r.items() if k != 'via'}          # the old result: no path data
    check(not path_ok(twin), 'path check FAILS the planted twin (a result without via)')

    # the sheet text: line 1 kept, the reach line is line 2 (F.with_reached is the function main() writes every sheet through)
    text = '# unit 3  x\ngame names: none\n'

    def sheet_ok(out):
        return (out.split('\n')[0] == '# unit 3  x' and out.split('\n')[1].startswith('reached: depth 2') and out.split('\n')[2] == 'game names: none')
    check(sheet_ok(F.with_reached(text, F.reached_line(r, uof, 3, 2))), 'sheet: line 1 byte-identical, reached line second, the rest follows')
    check(not sheet_ok(text), 'sheet check FAILS the planted twin (the old writer: the sheet as qsheet built it)')

    # a class slot: ctor 2 builds the table of slot 30 (unit 30): via names the ctor as parent
    gc = F.make_graph([(1, 2)], {1: 1, 2: 2, 30: 30})
    gc.expand = 'none'
    gc.cls = {2: [30]}
    rc = F.run(gc, [1], 2, 25)
    check(rc['via'][30] == (2, 1), 'class slot: parent is the ctor sub_2, seed sub_1 (got %s)' % (rc['via'].get(30),))
    # review: the slot unit's parent sub_2 does not CALL it (sub_2 builds the table that holds it, at the same depth): the line says so
    sl = F.reached_line(rc, {1: 1, 2: 2, 30: 30}, 30, 1)
    check(sl == 'reached: depth 1 from sub_00000001 via sub_00000002 (unit 2; class slot: sub_00000002 builds the table that holds it)'
          and F.via_text(rc, 30, 1) == 'depth 1 via sub_00000002 (class slot)' and F.via_text(rc, 2, 1) == 'depth 1 via sub_00000001',
          'class slot: the reached line and the bundle heading say the parent builds a table, not a call (%r)' % sl)

    # middleware: unit 3 is Havok (in the mw set): cut, not listed, not walked through (func 4 behind it); an entry in the set is kept
    gm = F.make_graph([(1, 2), (2, 3), (3, 4)], {1: 1, 2: 2, 3: 3, 4: 4})
    gm.expand = 'none'
    gm.mw = {3}

    def mw_ok(res):
        return 3 not in [u for u, _d in res['units']] and 4 not in res['funcs'] and res.get('middleware') == [3]
    check(mw_ok(F.run(gm, [1], 4, 25)), 'middleware: Havok unit 3 is cut and not walked through, and reported')
    gm.mw = None
    check(not mw_ok(F.run(gm, [1], 4, 25)), 'middleware check FAILS the planted twin (no cut: unit 3 listed)')
    gm.mw = {1}
    check(F.run(gm, [1], 1, 25)['units'][0] == (1, 0), 'middleware: an entry unit is never cut')
    # review: the middleware set is the qlib Havok units whose OWN functions carry the evidence. qlib's `strings` label rests on one hit
    # anywhere in the unit (real data: unit 100, the CAE screen's unit, has one hkMultipleVertexBuffer slot among 150 functions).
    # 40: 1 slot of 34 functions + a game db name; 41: one Havok slot function; 42: 4 of 8 functions; 43: one function, a db name not Havok's
    cand = {40, 41, 42, 43}
    mem = {40: list(range(1000, 1034)), 41: [2000], 42: list(range(3000, 3008)), 43: [4000]}
    fev = {1033, 2000, 3000, 3001, 3002, 3003, 4000}
    dbn = {1001: 'is_value_in_set_5_6_7_8_9_13_20_21', 2000: 'hk_loader_slot7_tail_call', 4000: 'store_zero_word_at_pointer'}

    def mw_rule_ok(s):
        return s == {41, 42}
    got = F.middleware_units(cand, mem, fev, dbn)
    check(mw_rule_ok(got), 'middleware rule: a unit is cut when half its functions carry Havok evidence and no db name is a game one (%s)' % sorted(got))
    check(not mw_rule_ok(set(cand)), 'middleware rule FAILS the planted twin (the qlib `strings` label as it is: all four)')
    # review: --recall names the middleware cut as the reason of a miss (it said "no call path in the index")
    gm.mw = {3}
    gm.expand = 'all'
    r3, r4 = F.miss_reason(gm, [1], 3, 4, 25), F.miss_reason(gm, [1], 4, 4, 25)
    check(r3.startswith('middleware cut: unit 3') and r4.startswith('middleware cut: only reachable'),
          'recall: a miss inside / behind a middleware unit says middleware cut (%r / %r)' % (r3, r4))

    # the bundle: unit 11 is an `engine` reading, unit 12 a `ui` reading, unit 13 not in the reach set, 14 has no reading
    # GATEPICK: gate 50 (unit 50) is called by 60 and 61 (units 60, 61); 50 calls 51 (unit 51, used by nobody else); 60 calls 62 (unit 62);
    # 61 calls 63, a Havok unit. caller_seeds + reach is the production path (reach_set composes exactly these two).
    gg = F.make_graph([(60, 50), (61, 50), (50, 51), (60, 62), (61, 63)], {50: 50, 51: 51, 60: 60, 61: 61, 62: 62, 63: 63})
    gg.expand = 'none'
    gg.mw = {63}
    seeds, why = F.caller_seeds(gg, [50])
    gg.why = why

    def gate_ok(res, ws, depth0=False):
        us = [u for u, _d in res['units']]
        ok = {50, 60, 61} <= set(us) and res['why'].get(60) == 'caller of sub_00000032' and F.reached_line(res, {}, 60, 0) == 'reached: caller of sub_00000032'
        if depth0:
            return ok and 51 not in us and 62 not in us
        return ok and 62 in us and 63 not in us and res.get('middleware') == [63]
    check(sorted(seeds) == [50, 60, 61] and gate_ok(F.run(gg, seeds, 1, 25), why), 'callers-of: the gate and its callers are seeds, depth 1 walks their callees, the Havok callee is cut')
    check(gate_ok(F.run(gg, seeds, 0, 25), why, True), 'callers-of at depth 0: the gate\'s own callee 51 and the callers\' callee 62 are NOT reached')
    # twins: the old behaviour (--from the gate: callees only: no caller is a seed), and a result with no why
    old = F.run(gg, [50], 1, 25)
    check(not gate_ok(old, why), 'callers-of check FAILS the planted twin (--from GATE: the callers 60 / 61 are never reached)')
    gg.why = None
    check(not gate_ok(F.run(gg, seeds, 1, 25), why), 'callers-of check FAILS the planted twin (no why labels)')
    gg.why = why
    gg.mw = None
    check(not gate_ok(F.run(gg, seeds, 1, 25), why), 'callers-of check FAILS the planted twin (no middleware cut: Havok unit 63 listed)')

    # the bundle: unit 11 is an `engine` reading, unit 12 a `ui` reading, unit 13 not in the reach set, 14 has no reading
    tmp = tempfile.mkdtemp()
    db = os.path.join(tmp, 'read.s2.db')
    ans = ''.join('## s%d\nunit: %d\npurpose: p%d | confidence: low | because: b\nsystem: %s\nmissing: m\nlisting needed: no\n\n' % (u, u, u, sy)
                  for u, sy in ((11, 'engine'), (12, 'ui'), (13, 'ui'), (21, 'engine'), (22, 'engine'), (23, 'engine')))
    qread.store(ans, 'b', 'm', db)
    c = qread.open_ro(db)
    sel = [(11, 'depth 0 (entry)'), (12, 'depth 1 via sub_0000000B'), (14, 'depth 1 via sub_0000000B'), (21, 'depth 2 via sub_0000000C')]

    def bundle_ok(bt):
        return ('## unit 11  depth 0 (entry)' in bt and '## unit 12  depth 1 via sub_0000000B' in bt and '## unit 21  depth 2 via sub_0000000C' in bt
                and '## unit 13' not in bt and '## unit 14' not in bt and bt.index('## unit 11') < bt.index('## unit 12') < bt.index('## unit 21'))
    bt = qreadsys.bundle('create_x', c, None, None, None, sel)
    check(bundle_ok(bt), 'bundle --from: the `engine` unit 11 and the `ui` unit 12 are both in, in reach order, headings carry depth and parent')
    check(not bundle_ok(qreadsys.bundle('engine', c, None)), 'bundle check FAILS the planted twin (selection by the system field: no ui unit, no depth)')

    # --max cut
    sel2 = [(u, 'depth 1 via sub_00000001') for u in (11, 12, 13, 21, 22, 23)]
    bt = qreadsys.bundle('create_x', c, None, None, None, sel2, 2, 0, 'py tools\\q\\q.py read system create_x --skip 2')

    def cut_ok(bt):
        return (bt.count('\n## unit ') == 2 and 'NOT SHOWN (--max 2): 4 more units' in bt and ' 13 21 22 23' in bt
                and 'the rest: py tools\\q\\q.py read system create_x --skip 2' in bt)
    check(cut_ok(bt), 'max cut: 2 shown, the other 4 listed by id with their count and the call for the rest')
    check(not cut_ok(qreadsys.bundle('create_x', c, None, None, None, sel2, 250, 0, 'x')), 'max check FAILS the planted twin (no cut)')
    # review: q read system NAME --from passes --middleware on to the reach and into the call for the rest
    import qunits
    seen = {}

    def fake_reach(*a, **kw):
        seen['args'], seen['kw'] = a, kw
        return None, None, [], {'units': [(11, 0), (12, 1)], 'via': {11: (None, 11), 12: (11, 11)}, 'viaclass': []}
    saved = (qread.open_ro, qunits.open_units, F.reach_set, qread.open_rw)
    qread.open_ro, qunits.open_units, F.reach_set, qread.open_rw = (lambda *a: sqlite3.connect(db)), (lambda *a: None), fake_reach, (lambda p=None: sqlite3.connect(db))
    outf = os.path.join(tmp, 'b.txt')
    try:
        qreadsys.main('system', ['create_x', '--from', '0x82000010', '--middleware', 'off', '--max', '1', '--out', outf])
    finally:
        qread.open_ro, qunits.open_units, F.reach_set, qread.open_rw = saved
    chk = sqlite3.connect(db)
    check(sorted(chk.execute("select unit_id from system_unit where system='create_x'")) == [(11,), (12,)],
          'read system --from records the reach set (units 11, 12) as the members of create_x')
    chk.close()
    bt = open(outf, encoding='utf-8').read()
    mwarg = seen.get('kw', {}).get('middleware')
    check(mwarg == 'off' and '--middleware off' in bt, 'read system --from: --middleware off reaches the walk and the call for the rest (%r)' % (mwarg,))

    c.close()

    # S111 READLINK: a bundle system owns the units its bundle reached; STALE compares only its own units
    import contextlib
    import io
    db2 = os.path.join(tmp, 'link.s2.db')
    ans2 = ''.join('## s%d\nunit: %d\npurpose: p%d | confidence: low | because: b\nsystem: %s\nmissing: m\nlisting needed: no\n\n' % (u, u, u, sy)
                   for u, sy in ((31, 'ui'), (32, 'engine'), (33, 'ui'), (34, ''), (35, ''), (36, 'ui')))
    qread.store(ans2, 'b', 'm', db2)
    w = sqlite3.connect(db2)
    w.execute("update unit_read set when_='2026-11-20 00:00:00'")
    w.execute("insert into system_read(system,explain,model,when_) values ('create_p','The page.','writerP','2026-11-25 00:00:00')")
    w.commit()
    w.close()
    qreadsys.record_members('create_p', [(31, 'depth 0 (entry)'), (32, 'depth 1 via sub_0000001F'), (34, 'depth 1 via sub_0000001F'), (99, 'depth 2 via sub_00000020')], db2)

    def state(p):
        r = qread.open_ro(p)
        s = qreadsys.system_state(r)
        r.close()
        return s

    def newer(u):
        x = sqlite3.connect(db2)
        x.execute("update unit_read set when_='2026-11-27 00:00:00' where unit_id=?", (u,))
        x.commit()
        x.close()

    def systems_text():
        real = qread.open_ro
        qread.open_ro = lambda path=None: real(db2)
        buf = io.StringIO()
        try:
            with contextlib.redirect_stdout(buf):
                qreadsys.main('systems', [])
        finally:
            qread.open_ro = real
        return buf.getvalue()
    s = state(db2)['create_p']
    check(s['n'] == 3 and not s['stale'], 'bundle system: units 31, 32, 34 are counted under create_p (99 has no reading), not stale (%s)' % s)
    check(not state(db2)['create_p']['stale'] and state(db2)['ui']['n'] == 3 and state(db2)['engine']['n'] == 1,
          'a unit stays counted under its own system field too (ui 3, engine 1)')
    txt = systems_text()
    check('create_p' in txt and ' 3 units  explanation by writerP' in txt.split('create_p')[1] and '(no system yet): 1 units' in txt,
          'read systems: create_p 3 units; the no-system count leaves out unit 34 (reached) and keeps 35 (%s)' % txt.replace('\n', ' | '))
    newer(33)
    check(not state(db2)['create_p']['stale'], 'a newer reading of an UNRELATED unit (33) does not make create_p STALE')
    newer(34)
    check(state(db2)['create_p']['stale'] and state(db2)['create_p']['newest'].startswith('2026-11-27') and 'STALE' in systems_text().split('create_p')[1],
          'a newer reading of its OWN unit (34, no system field of its own) makes it STALE')
    rc = qread.open_ro(db2)
    bt = qreadsys.bundle('create_p', rc, None)
    check('## unit 31  depth 0 (entry)' in bt and '## unit 34  depth 1 via sub_0000001F' in bt and '## unit 33' not in bt and '## unit 35' not in bt,
          'read system create_p (no --from) lists the readings of its members, with their reach path, and no other unit')
    rc.close()
    # planted twins: the old state (membership not recorded) fails the same checks
    x = sqlite3.connect(db2)
    x.execute('delete from system_unit')
    x.commit()
    x.close()
    check(state(db2).get('create_p', {}).get('n', 0) != 3, 'link check FAILS the planted twin (no membership recorded: create_p counts 0 units)')
    check(not state(db2).get('create_p', {}).get('stale'), 'stale check FAILS the planted twin (no membership: a newer reading of its own unit is not seen)')

    # store-system accepts any [a-z0-9_]+ name
    blocks, skipped = qreadsys.parse_system_answer('## system: create_moveset\nexplain:\nA page.\n## system: bad-name!\nexplain:\nx\n')
    check([b['system'] for b in blocks] == ['create_moveset'] and len(skipped) == 1, 'store-system: create_moveset accepted, a name outside [a-z0-9_]+ refused (%s)' % skipped)

    # review: q read curiosities NAME takes the names store-system now stores (it refused every name outside qread.SYSTEMS)
    import qcurios
    got = []
    saved = qcurios.run
    qcurios.run = lambda system=None, limit=40, low=False, path=None, out=print: got.append(system)
    try:
        qcurios.main(['create_moveset'])
        bad = None
        try:
            qcurios.main(['bad-name!'])
        except qread.ReadError as e:
            bad = str(e)
    finally:
        qcurios.run = saved
    check(got == ['create_moveset'] and bad is not None, 'curiosities: create_moveset answered, a name outside [a-z0-9_]+ refused (%s / %s)' % (got, bad))
    print('%d failure(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
