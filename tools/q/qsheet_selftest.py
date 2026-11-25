r"""qsheet selftest: a synthetic 3-function unit in temp dbs (every fact kind appears; the cap drops lines and says so), then the real
index (SKIPPED without one). py tools/q/qsheet_selftest.py"""
import json
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qsheet  # noqa: E402
import qnames  # noqa: E402

FAILS = []
LONG_OTHER = 'an ordinary string with no signature in it that runs on and on well past eighty characters so that the cut applies to it'
SIGNATURE = 'void __cdecl CStage::VeryLongSetupName(class CStage *, int, int, int, int, int, int, int, int, int, int)'
LONG_DB = 'db_name_of_b_which_is_a_very_long_name_that_the_sheet_must_print_to_its_very_last_word_end'


def check(ok, what):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        FAILS.append(what)


def mk(path, script, rows=()):
    c = sqlite3.connect(path)
    c.executescript(script)
    for sql, args in rows:
        c.executemany(sql, args)
    c.commit()
    c.close()


def synthetic():
    d = tempfile.mkdtemp(prefix='qsheet_')
    A, B, C, X, Y = 0x82100000, 0x82100100, 0x82100200, 0x82300000, 0x82300100
    Z1, Z2, Z3 = 0x82300200, 0x82300300, 0x82300400
    mk(os.path.join(d, 'index.db'),
       'create table funcs(addr integer, name text, size integer, insns integer, has_body integer);'
       'create table edges(src integer, dst integer, target integer, site integer, kind text);'
       'create table strrefs(func integer, site integer, addr integer, how text, op text);'
       'create table strings(addr integer, enc text, text text);'
       'create table rtti_slot(vt integer, idx integer, func integer);create table rtti_vtable(vt integer, col integer, td integer, nslots integer);'
       'create table rtti_class(td integer, mangled text, name text);'
       'create table ptrs(at integer, value integer, kind text);create table tables(start integer primary key, rule text, len integer);',
       [('insert into funcs values (?,?,?,?,1)', [(A, 'sub_82100000', 256, 64), (B, 'sub_82100100', 16, 32), (C, 'sub_82100200', 64, 16), (X, 'sub_82300000', 8, 2), (Y, 'sub_82300100', 8, 2),
                                                  (Z1, 'empty_stub_return', 8, 2), (Z2, 'empty_stub_return', 8, 2), (Z3, 'empty_stub_return', 8, 2)]),
        ('insert into edges values (?,?,?,?,?)', [(A, B, B, 1, 'call'), (X, A, A, 2, 'call'), (Y, A, A, 3, 'call'), (B, Y, Y, 4, 'call'), (C, B, B, 5, 'tail'),
                                                  (A, Z1, Z1, 6, 'call'), (A, Z2, Z2, 7, 'call'), (A, Z3, Z3, 8, 'call')]),
        ('insert into strrefs values (?,?,?,?,?)', [(A, 1, 0x82001000, 'x', 'y'), (A, 2, 0x82001100, 'x', 'y'), (A, 3, 0x82001200, 'x', 'y')]),
        ('insert into strings values (?,?,?)', [(0x82001000, 'a', 'a string that is far longer than forty characters for sure'), (0x82001100, 'a', LONG_OTHER),
                                                (0x82001200, 'a', SIGNATURE)]),
        ('insert into rtti_slot values (?,?,?)', [(0x82005000, 3, B)]), ('insert into rtti_vtable values (?,?,?,?)', [(0x82005000, 1, 7, 9)]),
        ('insert into rtti_class values (?,?,?)', [(7, '.?AVFoo', 'Foo')]),
        ('insert into ptrs values (?,?,?)', [(0x82005000 + 12, B, 'func'), (0x82006004, C, 'func'), (0x82007000, A, 'func')]),
        ('insert into tables values (?,?,?)', [(0x82005000, 'rtti', 9), (0x82006000, 'run', 3)])])
    mk(os.path.join(d, 'units.db'), 'create table units(func integer primary key, unit_id integer);create table unit(unit_id integer, lo integer, hi integer, n_funcs integer);',
       [('insert into units values (?,?)', [(A, 0), (B, 0), (C, 0)]), ('insert into unit values (?,?,?,?)', [(0, A, C, 3)])])
    mk(os.path.join(d, 'names.db'), 'create table names(func integer, name text, source text, table_addr integer);',
       [('insert into names values (?,?,?,?)', [(A, 'yTest.DoIt', 'lua_binding', 0), (X, 'Outer.Caller', 'lua_binding', 0), (Y, 'Outer.Other', 'name_table', 0),
                                              (C, 'CStage::RingObjSetup_Anime', 'fn_string', 0)])])
    mk(os.path.join(d, 'seen.db'), 'create table seen(site integer, func integer, target integer, runs integer);create table seen_runs(run text, pairs integer, mtime integer);',
       [('insert into seen values (?,?,?,?)', [(0x82300010, X, B, 1), (0x82100020, A, X, 1)]), ('insert into seen_runs values (?,?,?)', [('r1', 2, 0)])])
    mk(os.path.join(d, 'vcall.db'), 'create table icall(site integer, func integer, kind text, shape text, off integer, recv text, status text, n integer);'
       'create table icall_target(site integer, target integer, how text);',
       [('insert into icall values (?,?,?,?,?,?,?,?)', [(0x82100020, A, 'k', 's', 8, 'r', 'exact', 1), (0x82100030, A, 'k', 's', 8, 'r', 'cand', 5), (0x82100130, B, 'k', 's', 8, 'r', 'narrow', 2)]),
        ('insert into icall_target values (?,?,?)', [(0x82100020, X, 'a'), (0x82100130, X, 'a'), (0x82100130, Y, 'a')])])
    mk(os.path.join(d, 'fields.db'), 'create table roots(id integer primary key, text text);create table rows(func integer, addr integer, rw text, width integer, fl integer, off integer, root integer);'
       'create table slots(func integer, vt integer, idx integer);',
       [('insert into roots values (?,?)', [(1, 'a0'), (2, 'g:8200'), (3, 'K'), (4, 'a2')]),
        ('insert into rows values (?,?,?,?,?,?,?)', [(A, 1, 'W', 4, 0, 124, 1)] * 3 + [(A, 2, 'R', 4, 0, 16, 1), (A, 3, 'R', 4, 0, 0, 2), (A, 4, 'W', 4, 0, 0x82001234, 3), (A, 5, 'R', 4, 0, 8, 4)])])
    os.makedirs(os.path.join(d, 'db'))
    json.dump([{'address': '0x82100100', 'name': LONG_DB, 'confidence': 'high', 'system': 'render'},
               {'address': '0x82100200', 'name': 'stage_build_cycled_slot_groups', 'confidence': 'medium', 'system': 'render'}], open(os.path.join(d, 'db', 'x.json'), 'w'))
    p = lambda n: os.path.join(d, n)
    return qsheet.Ctx(index=p('index.db'), names=p('names.db'), units=p('units.db'), seen=p('seen.db'), vcall=p('vcall.db'), fields=p('fields.db'), dbdir=p('db'),
                      hooks=({A: ['h.cpp:9']}, [(0x82100210, 'mid_c')])), d


def main():
    c, d = synthetic()
    t, fl = qsheet.build(c, 0, 12)
    print(t)
    for w, what in (('yTest.DoIt', 'game name'), ('db_name_of_b', 'db name'), ('systems: render=2', 'db system'), ('hook h.cpp:9', 'hook anchor'),
                    ('mid-asm mid_c', 'mid-asm anchor'), ('"a string that is far longer than forty characters for sure"', 'a short string whole'),
                    ('outside 2: Outer.Caller[g] Outer.Other[g]', 'outside callers named'), ('exact->', 'static exact target'), ('narrow->2 tgts', 'static narrow targets'),
                    ('cand1', 'wide site'), ('obs callers 1', 'observed callers'), ('targets 1', 'observed targets'), ('cannot see: 1 wide', 'footer: wide sites'),
                    ('vslot vt82005000[+12] Foo', 'NEW: a slot of a catalogued rtti table: vtADDR[+OFF]'),
                    ('in run-table 82006000[+4] (no code loads it: may not be a vtable)', 'NEW: a slot only a run table lists'),
                    ('vslot: none known', 'NEW: a pointed-at function in no catalogued table says so'),
                    (LONG_DB + '(h)', 'NEW: a long db name is printed uncut'),
                    ('"%s"' % SIGNATURE, 'NEW: a signature string is printed uncut'),
                    ('"%s~"' % LONG_OTHER[:79], 'NEW: another long string is cut at 80 and ends in ~'),
                    ('CONFLICT 82100200: db name stage_build_cycled_slot_groups and game name CStage::RingObjSetup_Anime', 'NEW: CONFLICT line'),
                    ('empty_stub_return[i] x3', 'NEW: repeated identical out targets collapse'),
                    ('this: writes +124*3; reads +16', 'NEW: root this in words'), ('global 0x8200: reads +0', 'NEW: root global in words'),
                    ('param 2: reads +8', 'NEW: root param N in words'), ('absolute address: writes to address 0x82001234', 'NEW: constant-address root in words, hex')):
        check(w in t, what + '   [%s]' % w)
    for w, what in (('nearest named outside', 'NEW: the neighbours footer is gone'), (' in unit', 'NEW: no in-unit caller list on a 16-byte function'),
                    ('vt82005000[3]', 'NEW: no slot index guess'), ('a0:', 'NEW: no letter codes')):
        check(w not in t, what + '   [%s]' % w)
    check(t.count('CONFLICT') == 1 and t.index('CONFLICT') < t.index('82100000 256B'), 'NEW: one CONFLICT line, before the function lines')
    check(t.index('CONFLICT') > t.index('db names:'), 'NEW: CONFLICT is listed under the header')
    ok3 = False
    for kb in [x / 20.0 for x in range(60, 10, -1)]:
        t3, _ = qsheet.build(c, 0, kb)
        if 'DROPPED' in t3 and ' out ' in t3.split('DROPPED')[1].split('\n')[0]:
            ok3 = 'strsig' not in t3.split('DROPPED')[1].split('\n')[0] and '"%s"' % SIGNATURE in t3
            break
    check(ok3, 'NEW: when out lines are dropped for the cap, the signature string line is still there')
    check(fl['has_name'] and fl['has_anchor'] and fl['has_observed'], 'flags: name, anchor, observed')
    t2, fl2 = qsheet.build(c, 0, 0.5)
    check('DROPPED' in t2 and fl2['dropped'] > 0, 'a small cap drops lines and says so: ' + t2.split('DROPPED')[1].split('\n')[0] if 'DROPPED' in t2 else 'a small cap drops lines and says so')
    check(fl2['bytes'] < fl['bytes'] and 'fld' in t2.split('DROPPED')[1].split('\n')[0], 'the fields (least informative) go first')
    check('unit 0  82100000..82100200' in t, 'header range')
    if os.path.exists(qnames.INDEX_DB) and os.path.exists(qsheet.qunits.UNITS_DB) and os.path.exists(qsheet.qseen.SEEN_DB):
        r = qsheet.Ctx()
        u1 = r.unit_of(0x82BDB460)
        s1, _ = qsheet.build(r, u1, 12)
        # The count comes from seen.s1.db, which grows with every recorded run (19 when this was written, 20 since 2026-11-24): at least 19.
        import re as _re
        m1 = _re.search(r'\n82BDB460[^\n]*\n(?: [^\n]*\n)*? obs callers (\d+) ', s1)   # the function's own block (its indented lines)
        check(bool(m1) and int(m1.group(1)) >= 19, 'real: sub_82BDB460 shows its observed indirect callers (at least 19)')
        u2 = r.unit_of(0x826D44A0)
        s2, _ = qsheet.build(r, u2, 12)
        check('yGame.SetMotion' in s2, 'real: the unit of sub_826D44A0 shows yGame.SetMotion')
        check(len(s1.encode()) <= 12 * 1024, 'real: the 12 KB cap holds')
        s3, _ = qsheet.build(r, r.unit_of(0x833693C8), 12)
        check('vslot vt821B76D0[+4]' in s3 and 'vt821B73B8' not in s3, 'NEW real: sub_833693C8 is slot +4 of the table 0x821B76D0, not the old run-start guess')
        s4, _ = qsheet.build(r, r.unit_of(0x824E7AE8), 12)
        check('CONFLICT 824E82A0: db name stage_build_cycled_slot_groups' in s4, 'NEW real: 824E82A0 db name against its CStage::RingObjSetup_Anime string')
        check('"void __cdecl CStage::TextureSetup(' in s4 and 'TextureSetup(class~' not in s4, 'NEW real: the signature string of 824E7AE8 is not cut')
    else:
        print('SKIP  real part: no index')
    print('%d failure(s)' % len(FAILS))
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
