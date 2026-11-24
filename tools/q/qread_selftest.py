"""Selftest of q read (S110 READSTORE). Hand-written fixtures in the sheet-reader answer format. The sheet part needs the real index and is skipped without one.

    py -I tools/q/qread_selftest.py
"""
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402
import qread  # noqa: E402

ANSWER = """## sheet A
unit: 4623, 0x82507240..0x8250C870, 46 funcs (5 named)
purpose: Builds the audience and its sign boards. | confidence: high-ish (medium) | because: "CStageAudienceManager::static_Factory", factory names
system: crowd
fun: The crowd holds up hand-made sign boards with their own text.
names: sub_8250B7F0 -> set_up_sign_board_text | evidence: str "sign", called by static_SetUpHandSignBoard | medium
names: 8250B8F0 -> bare_address_name | evidence: out edge to the factory | low
missing: who calls the factory
listing needed: yes

## sheet B
unit: 100, 0x82000000..0x82000100, 4 funcs (0 named)
purpose: unknown | confidence: low | because: no names
names:
- sub_82000010 -> header_entry_one | evidence: fld +4 written, vslot 2 | high
  sub_82000020 -> header_entry_two | evidence: fld +8, str "x" | medium
- this line is not an entry
missing: any name
listing needed: no

## sheet C
unit: 101, 0x82001000..0x82001100, 3 funcs (0 named)
purpose: Math helpers. | confidence: medium | because: no calls out
system: Engine (math)
fun: "none"
names: none (nothing supported by two facts)
missing: callers
listing needed: no

## sheet D
unit: 102, r, 3 funcs (0 named)
purpose: Pads. | confidence: low | because: small
system: nonsense
(none, nothing)
missing: x
listing needed: no

## sheet E
Same unit as sheet A, answered above.

## sheet F
unit: 103, r, 3 funcs (0 named)
purpose: Something | confidence: maybe | because: x
missing: x
listing needed: no

## patterns
- tiny functions with one callee are thunks
- Tiny functions with one callee are thunks
none
"""
ANSWER2 = """## again
unit: 4623, 0x82507240..0x8250C870, 46 funcs (5 named)
purpose: Second opinion: audience factory code. | confidence: high | because: factory name
names: none
missing: nothing
listing needed: no
## patterns
none
"""
SYS_ANSWER = """## system: engine
explain:
The engine layer is the math and memory code every other part sits on.

It is not drawn anywhere itself. Not known yet: who calls it first (unit 101).
facts:
- The math cluster carries no strings at all | from: unit 101 | medium
- this line has no source or confidence
- A low one | from: unit 101 | low
open:
- the callers of unit 101
## system: bogus
explain:
Text for a system that does not exist.
## system: crowd
facts:
- no explanation in this block | from: unit 4623 | high
"""
OLD_SCHEMA = ('create table unit_read(id integer primary key autoincrement, unit_id integer, purpose text, confidence text, because text,'
              ' missing text, listing_needed integer, batch text, model text, when_ text);'
              'create table name_read(id integer primary key autoincrement, func integer, name text, evidence text, confidence text,'
              ' unit_id integer, batch text, model text);'
              'create table pattern_read(id integer primary key autoincrement, text text, batch text, model text);')
fails = []


def check(cond, what):
    print('%s  %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        fails.append(what)


def main():
    tmp = tempfile.mkdtemp(prefix='qread_t')
    db = os.path.join(tmp, 'read.s2.db')
    blocks, pats, skipped = qread.parse_answer(ANSWER)
    by = {b['unit_id']: b for b in blocks}
    check(by[4623]['system'] == 'crowd' and by[4623]['fun'].startswith('The crowd holds up'), 'parser: system and fun lines read')
    check(by[101]['system'] == 'engine' and by[101]['fun'] == '', 'parser: "Engine (math)" -> engine, fun "none" -> empty')
    check(by[100]['system'] == '' and by[100]['fun'] == '' and by[102]['system'] == '', 'parser: answers without the two lines (or an unknown system) parse, system empty')
    check(sorted(by) == [100, 101, 102, 4623], 'parsed units 100 101 102 4623 (got %s)' % sorted(by))
    check(by[4623]['confidence'] == 'medium', 'high-ish (medium) -> medium')
    check([n[1] for n in by[4623]['names']] == ['set_up_sign_board_text', 'bare_address_name'] and by[4623]['names'][1][0] == 0x8250B8F0,
          'inline names, bare 8-hex address accepted')
    check(by[4623]['listing'] is True and by[100]['listing'] is False, 'listing needed')
    check([n[1] for n in by[100]['names']] == ['header_entry_one', 'header_entry_two'] and len(by[100]['bad_names']) == 1,
          'names: header with entries on following lines; one unparsable name line counted, not guessed')
    check(by[101]['names'] == [] and by[102]['names'] == [], 'names: none ... and (none ...) -> no names')
    check(sorted(l for l, _ in skipped) == ['sheet E', 'sheet F'], 'same-unit back reference and bad confidence skipped by label (got %s)' % skipped)
    check(pats == ['tiny functions with one callee are thunks', 'Tiny functions with one callee are thunks'], 'patterns parsed, none dropped')
    r = qread.store(ANSWER, 'b1', 'modelX', db)
    check(r['parsed'] == 4 and len(r['skipped']) == 2 and r['names'] == 4 and r['listing'] == [4623], 'store counts %s' % {k: v for k, v in r.items() if k != 'skipped'})
    out = []
    qread.show(4623, db, out.append)
    check(any('Builds the audience' in x for x in out) and any('[r:medium]' in x for x in out), 'show round-trips purpose and names')
    qread.store(ANSWER2, 'b2', 'modelY', db)
    out = []
    qread.show(4623, db, out.append)
    ps = [x for x in out if x.strip().startswith('purpose:')]
    check(len(ps) == 2 and 'Second opinion' in ps[0] and 'Builds' in ps[1], 'second answer keeps both, newest first')
    check(qread.read_line(4623, db).startswith('read: Second opinion') and qread.read_line(999, db) is None, 'read_line is the newest')
    check(qread.proposed_names(0x8250B7F0, 0x8250B7F0, db) == {0x8250B7F0: ('set_up_sign_board_text', 'medium')}, 'proposed_names')
    out = []
    qread.stats(db, units=_FakeUnits(), out=out.append)
    check(any('candidates' in x for x in out) and any('units read: 4 of' in x for x in out) and sum(1 for x in out if 'thunks' in x) == 1,
          'stats: totals, candidate line, patterns deduped')

    # the system layer (tools/q/qreadsys.py)
    import qreadsys
    c = qread.open_ro(db)
    nfact = c.execute("select count(*) from fact_read where source='reader'").fetchone()[0]
    check(nfact == 1, 'store: the reader fun line went into fact_read with source reader (%d)' % nfact)
    out = []
    qread.stats(db, units=_FakeUnits(), out=out.append)
    check(any(x.startswith('units per system') and 'engine 1' in x and '(no system yet) 3' in x for x in out), 'stats counts units per system (%s)' % [x for x in out if 'per system' in x])
    check([u for u, _b in qreadsys.pick_reread(10, None, path=db)] == [100, 102, 4623], 'pick --reread: answered units with no system, lowest id first')
    udb2 = sqlite3.connect(':memory:')
    udb2.executescript('create table unit(unit_id integer, lo integer, hi integer, n_funcs integer);')
    udb2.execute('insert into unit values (101, 0x82001000, 0x82001100, 3)')
    bt = qreadsys.bundle('engine', c, udb2, None, {101: {'model': 3, 'ui': 5}})
    check('unit 101, 0x82001000..0x82001100, 3 funcs' in bt and 'purpose: Math helpers.' in bt and 'ui 5, model 3' in bt and 'unit 100' not in bt,
          'bundle: the readings of one system with range and observed-writer counts, no other unit')
    check('FEED BAR' in qreadsys.bundle('model', c, udb2, [10, 5, 1, 2, 3, 0, 2], None) and 'no stored reading' in qreadsys.bundle('ai', c, udb2),
          'bundle: feed bar row when given; an empty system says so')
    c.close()
    r = qreadsys.store_system(SYS_ANSWER, 'writerM', db)
    check(r['parsed'] == ['engine'] and len(r['skipped']) == 2 and r['facts'] == 2 and r['bad_facts'] == 1,
          'store-system: one block stored, the unknown-system and the explain-less blocks skipped, a bad fact line counted (%s)' % r)
    c = qread.open_ro(db)
    st = qreadsys.system_state(c)
    ex = st['engine']['written'][0]
    check(st['engine']['written'][1] == 'writerM' and ex.startswith('The engine layer') and 'Not known yet' in ex and 'Next to read or observe:\n- the callers of unit 101' in ex
          and not st['engine']['stale'], 'store-system round trip: explanation, model, open lines kept; not stale')
    c.close()
    w = sqlite3.connect(db)
    w.execute("update unit_read set when_='2999-01-01 00:00:00' where unit_id=101")
    w.commit()
    w.close()
    c = qread.open_ro(db)
    check(qreadsys.system_state(c)['engine']['stale'], 'a reading newer than the explanation makes it stale')
    c.close()
    # migration from a read.s1.db
    mt = tempfile.mkdtemp(prefix='qread_m')
    o1 = sqlite3.connect(os.path.join(mt, 'read.s1.db'))
    o1.executescript(OLD_SCHEMA)
    o1.execute("insert into unit_read(unit_id,purpose,confidence,because,missing,listing_needed,batch,model,when_) values (7,'old','high','b','m',0,'x','y','2026-01-01 00:00:00')")
    o1.commit()
    o1.close()
    mc = qread.open_ro(os.path.join(mt, 'read.s2.db'))
    check(mc is not None and qreadsys.latest_units(mc)[7]['system'] == '' and qreadsys.latest_units(mc)[7]['purpose'] == 'old',
          'read.s2.db is made from read.s1.db, old rows have an empty system')
    mc.close()

    # pick on a temp feedseen db and units db
    import qfeedseen
    fdb = os.path.join(tmp, 'feedseen.db')
    w = sqlite3.connect(fdb)
    w.executescript('create table writers(file text, line integer, func integer, n_addrs integer, runs integer);')
    rows = [('a.h', 1, 0x1010), ('a.h', 2, 0x1010), ('a.h', 3, 0x1020),      # unit 1: 3 sites
            ('b.h', 1, 0x2010),                                            # unit 2: 1 site
            ('c.h', 1, 0x3010), ('c.h', 2, 0x3010), ('c.h', 3, 0x3010), ('c.h', 4, 0x3010),   # unit 3: 4 sites, all a copier
            ('d.h', 1, 0x4010), ('d.h', 2, 0x4010), ('d.h', 3, 0x4010), ('d.h', 4, 0x4010), ('d.h', 5, 0x4010)]   # unit 4: 5, answered
    w.executemany('insert into writers values (?,?,?,1,1)', rows)
    w.commit()
    fc = sqlite3.connect(fdb)
    udb = sqlite3.connect(':memory:')
    udb.executescript('create table units(func integer, unit_id integer); create table unit(unit_id integer, lo integer, hi integer, n_funcs integer);')
    for u, base in ((1, 0x1000), (2, 0x2000), (3, 0x3000), (4, 0x4000)):
        udb.execute('insert into unit values (?,?,?,5)', (u, base, base + 0x40))
        for k in range(0, 0x40, 0x10):
            udb.execute('insert into units values (?,?)', (base + k, u))
    qread.store('## z\nunit: 4\npurpose: p | confidence: low | because: b\nmissing: m\nlisting needed: no\n', 'b0', 'm', db)

    class Ctx:
        def label(self, f):
            return 'memset' if f == 0x3010 else None
    res = qread.pick_feed(10, None, ctx=Ctx(), feed=fc, units=udb, path=db)
    check([(r[0], r[1]) for r in res] == [(1, 3), (2, 1)], 'pick ranks by read sites, skips the copier unit and the answered unit (got %s)' % [(r[0], r[1]) for r in res])

    # the sheet on the real index
    if not os.path.exists(qnames.INDEX_DB):
        print('SKIP  real-index part (no index)')
    else:
        import qsheet
        old = qread.READ_DB
        qread.READ_DB = db
        try:
            ctx = qsheet.Ctx()
            txt = qsheet.build(ctx, 4623)[0]
        finally:
            qread.READ_DB = old
        check('read: Second opinion' in txt and '(high, modelY, b2)' in txt, 'sheet shows the read: header line')
        check('[r:medium]' in txt and 'read set_up_sign_board_text [r:medium]' in txt, 'sheet tags the proposed name [r:medium]')
        check('[g]' not in txt.split('read set_up_sign_board_text')[1].split('\n')[0], 'the proposed name line carries no [g]')
    print('%d failure(s)' % len(fails))
    return 1 if fails else 0


class _FakeUnits:
    def execute(self, sql):
        return [(100,), (101,), (102,), (4623,), (5,)]


if __name__ == '__main__':
    sys.exit(main())
