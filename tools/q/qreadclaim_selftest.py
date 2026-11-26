"""Selftest of q read claims (S110 VERIFYMECH): a planted TRUE and a planted FALSE claim per kind, over a tiny fake index.
Works on temp databases only, never the real read.s2.db or index.

    py -I tools/q/qreadclaim_selftest.py
"""
import contextlib
import io
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qread  # noqa: E402
import qreadclaim as qc  # noqa: E402
import qreadverify  # noqa: E402

fails = []
A, B, C, D, FAR, NOISE = 0x82100000, 0x82100100, 0x82100200, 0x82100300, 0x82900000, 0x82C78DF0
E = 0x82100400                 # carries a game name (CFoo::ReadEndProcess), a low db name, called by FAR
VT = 0x82001000


def ok(cond, what):
    print('%s  %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        fails.append(what)


def fake():
    """A calls B, C and a hidden __save helper; D calls C (tail) and B; A is slot +8 of VT (RTTI CFoo, base CBase); A references one
    string and writes this+16 and global 0x83000000 +4; B carries the game name Foo.BetaRun; units: {A,B,C}=1, {D}=2, {FAR}=3"""
    ix = sqlite3.connect(':memory:')
    ix.executescript('create table funcs(addr integer primary key, name text, size integer, has_body integer);'
                     'create table edges(src integer, dst integer, target integer, site integer, kind text);'
                     'create table strrefs(func integer, site integer, addr integer, how text, op text);'
                     'create table strings(addr integer primary key, enc text, text text);'
                     'create table ptrs(at integer, value integer, kind text);'
                     'create table rtti_class(td integer primary key, mangled text, name text);'
                     'create table rtti_vtable(vt integer primary key, col integer, td integer, nslots integer);'
                     'create table rtti_base(cls text, idx integer, base text);')
    for a, n in ((A, None), (B, None), (C, None), (D, None), (E, None), (FAR, None), (NOISE, '__savegprlr_26')):
        ix.execute('insert into funcs values (?,?,64,1)', (a, n))
    for s, d, k in ((A, B, 'call'), (A, NOISE, 'call'), (A, C, 'call'), (D, C, 'tail'), (D, B, 'call'), (FAR, E, 'call')):
        ix.execute('insert into edges values (?,?,?,0,?)', (s, d, d, k))
    for i, t in enumerate(('Hello World of strings', 'Action 2', 'Action 3', 'Invalid "', '" cloth instance. Ignoring.', '?333C\\'), 1):
        ix.execute("insert into strings values (?, 'ascii', ?)", (i, t))
        ix.execute('insert into strrefs values (?,0,?,null,null)', (A, i))
    ix.execute("insert into ptrs values (?,?,'func')", (VT + 8, A))
    ix.execute("insert into rtti_class values (1,'x','CFoo'),(2,'y','CBar')")
    ix.execute('insert into rtti_vtable values (?,0,1,4)', (VT,))
    ix.execute("insert into rtti_base values ('CFoo',0,'CBase')")
    fd = sqlite3.connect(':memory:')
    fd.executescript('create table roots(id integer primary key, text text unique); create table rows(func integer, addr integer, rw text,'
                     ' width integer, fl integer, off integer, root integer);')
    fd.execute("insert into roots values (1,'a0'),(2,'g:83000000')")
    fd.execute("insert into rows values (?,0,'W',4,0,16,1)", (A,))
    fd.execute("insert into rows values (?,0,'W',4,0,4,2)", (A,))
    vc = sqlite3.connect(':memory:')
    vc.executescript('create table icall(site integer, func integer, kind text, shape text, off integer, recv text, status text, n integer);'
                     'create table icall_target(site integer, target integer, how text);')
    sn = sqlite3.connect(':memory:')
    sn.executescript('create table seen(site integer, func integer, target integer, runs integer);')
    nm = sqlite3.connect(':memory:')
    nm.executescript('create table names(func integer, name text, source text, table_addr integer);')
    nm.execute("insert into names values (?, 'Foo.BetaRun', 'lua_binding', 0)", (B,))
    nm.execute("insert into names values (?, 'CFoo::ReadEndProcess', 'name_table', 0)", (E,))
    uc = sqlite3.connect(':memory:')
    uc.executescript('create table units(func integer primary key, unit_id integer); create table unit(unit_id integer primary key, lo integer,'
                     ' hi integer, n_funcs integer);')
    for a in (A, B, C):
        uc.execute('insert into units values (?,1)', (a,))
    uc.execute('insert into units values (?,2)', (D,))
    uc.execute('insert into units values (?,3)', (FAR,))
    uc.execute('insert into unit values (1,?,?,3)', (A, C))
    best = {A: ('alpha_func', 'high'), B: ('beta_func', 'medium'), C: ('gamma_func', 'low'), D: ('delta_func', 'low'),
            E: ('eps_func', 'low'), FAR: ('far_func', 'medium'), NOISE: ('__savegprlr_26', 'high')}
    byname = {n: {a} for a, (n, _c) in best.items()}
    return qc.Facts(index=ix, fields=fd, vcall=vc, seen=sn, names=nm, units=uc, db=(best, byname))


def verdict(fx, text, subj, unit=False, kind=None):
    """the verdicts of the claims of `kind` in text"""
    return [v for k, _c, v, _f in qc.check_reading(text, subj, fx, unit=unit) if kind is None or k == kind]


def main():
    fx = fake()
    # one planted TRUE and one planted FALSE claim per kind: (kind, true text, its subject, false text, its subject)
    cases = [
        ('str', 'str "Hello World~"', [A], 'str "Goodbye"', [A]),
        ('calls', 'calls beta_func', [A], 'calls far_func', [A]),
        ('only_callee', 'out only beta_func and gamma_func', [D], 'out only beta_func', [D]),
        ('caller', 'called by alpha_func', [B], 'called by far_func', [B]),
        ('only_caller', 'called only from alpha_func and delta_func', [B], 'called only from alpha_func', [C]),
        ('n_callers', '2 callers', [C], '5 callers', [C]),
        ('vslot', 'vslot vt82001000[+8]', [A], 'vslot vt82001000[+12]', [A]),
        ('vslot_class', 'vt82001000[+8] CBase', [A], 'vt82001000[+8] CBar', [A]),
        ('field', 'writes this +16', [A], 'writes +20', [A]),
    ]
    for kind, good, gs, bad, bs in cases:
        ok(verdict(fx, good, gs, kind=kind) == ['checked'], '%s: planted true claim checked (%s)' % (kind, verdict(fx, good, gs, kind=kind)))
        ok(verdict(fx, bad, bs, kind=kind) == ['refuted'], '%s: planted false claim refuted (%s)' % (kind, verdict(fx, bad, bs, kind=kind)))
    ok(fx.direct_callees(A) == {B, C}, 'a __save/__rest callee is hidden, as on the sheet')
    # global field
    ok(verdict(fx, 'writes global 0x83000000 +4', [A], kind='field') == ['checked'], 'field: write to a global at an offset checked')
    ok(verdict(fx, 'reads global 0x83000000', [A], kind='field') == ['refuted'], 'field: a read of a global it only writes refuted')
    ok(verdict(fx, 'writes global 0x83000000', [A], kind='field') == ['checked'], 'field: "writes global G" without offset = any field of G')
    # mention (unit because): a quoted name that is a member / callee / caller, and one that is none
    ok(verdict(fx, '"beta_func(m)", "delta_func(l)"', [A, B, C], unit=True, kind='mention') == ['checked', 'checked'], 'mention: planted true names checked')
    ok(verdict(fx, '"far_func(m)"', [A, B, C], unit=True, kind='mention') == ['refuted'], 'mention: planted false name refuted')
    # name checks
    r = qc.name_checks(A, 'alpha_setup', fx)
    ok([x[2] for x in r] == ['checked'] and r[0][0] == 'name_vs_db', 'name_vs_db: a proposal sharing a word with the high db name checked (%s)' % r)
    r = qc.name_checks(A, 'zeta_thing', fx)
    ok([x[2] for x in r] == ['refuted'], 'name_vs_db: a proposal sharing no word with the high db name refuted (%s)' % r)
    ok(qc.name_checks(B, 'beta_runner', fx)[0][:3] == ('name_vs_game', 'name vs game Foo.BetaRun', 'checked'), 'name_vs_game: shares "beta" checked')
    ok(qc.name_checks(B, 'omega_thing', fx)[0][2] == 'refuted', 'name_vs_game: planted false refuted')
    ok(qc.name_checks(C, 'anything_here', fx) == [], 'no name check against a low db name')
    # forms that are NOT claims about the subject
    ok(verdict(fx, 'called by beta_func which writes +20', [C], kind='field') == [], 'a field claim of another function ("which writes") is not read')
    ok(verdict(fx, 'calls far_func via beta_func', [A], kind='calls') == [], '"calls X via Y" is not a direct-call claim')
    ok(verdict(fx, 'reached from vt82001000[+12]', [A], kind='vslot') == [], 'a vslot reached from is another function\'s slot')
    ok(verdict(fx, 'calls made_up_name', [A], kind='calls') == ['not_checkable'], 'a name that resolves to nothing is not checkable')
    ok(verdict(fx, 'loops over the table and returns 0', [A]) == ['not_checkable'], 'prose: one not_checkable row')
    ok(verdict(fx, 'calls table', [A], kind='calls') == [], 'a bare word that is no db name is prose, not a callee')
    # S110 VERIFYMECH review: the parser's false refutations and missed forms, read from the stored readings (one twin per class)
    ok(verdict(fx, 'str "Hello World/Moon"', [A], kind='str') == ['checked'], 'str: "A/B" alternation, the first alternative is literal text')
    ok(verdict(fx, 'str "Goodbye/Moon"', [A], kind='str') == ['refuted'], 'str: an alternation whose text is absent is refuted')
    ok(verdict(fx, 'str "Action 2..3"', [A], kind='str') == ['checked'], 'str: a range "N..M" checks its literal start')
    ok(verdict(fx, 'str "Action 7..9"', [A], kind='str') == ['refuted'], 'str: a range whose literal start is absent is refuted')
    ok(verdict(fx, 'str "Hello ... strings"', [A], kind='str') == ['checked'], 'str: an elision "A ... B" checks each literal run')
    ok(verdict(fx, 'str "Hello ... planets"', [A], kind='str') == ['refuted'], 'str: an elision with an absent run is refuted')
    ok(verdict(fx, r'str "Invalid \"\" cloth instance. Ignoring."', [A], kind='str') == ['checked'], 'str: escaped quotes inside the quote')
    ok(verdict(fx, r'"str 2: \"Hello World\""', [A, B, C], unit=True, kind='str') == ['checked'], 'str: a quoted sheet line with escaped quotes')
    ok(verdict(fx, 'str "Hello World" "beta_func(m)"', [A], kind='str') == ['checked'], 'str: a quoted name with a confidence tag is no string')
    ok(verdict(fx, 'calls beta_func (str "Goodbye")', [A], kind='str') == [], 'str: a str claim in a parenthesised aside is about another function')
    ok(verdict(fx, 'calls 82100100 far_func', [A], kind='calls') == ['checked'], 'calls ADDR NAME: the name labels the address (one claim)')
    ok(verdict(fx, 'calls 82900000 beta_func', [A], kind='calls') == ['refuted'], 'calls ADDR NAME: the address is the claim')
    ok(verdict(fx, '"vslot vt82001000[+8]"', [A, B, C], unit=True, kind='vslot') == ['checked'], 'a quoted sheet line is read: vslot')
    ok(verdict(fx, '"82100000 writes +16*2 +20*3"', [A, B, C], unit=True, kind='field') == ['checked', 'refuted'], 'a quoted sheet line is read: field, *N counts')
    ok(verdict(fx, 'calls "beta_func"', [A], kind='calls') == ['checked'], 'a quoted name after calls is read')
    ok(verdict(fx, 'str "calls far_func"', [A], kind='calls') == [], 'a quoted game string is never read as a claim')
    ok(verdict(fx, '16109 lists "vt82001000[+12]"', [A, B, C], unit=True, kind='vslot') == [], 'a quoted sheet line inside a sentence is another subject\'s')
    ok(verdict(fx, r'str "?333C\" (bytes); "memset(h)"', [A], kind='str') == ['checked'], 'str: a game string that ends in a backslash')
    ok(verdict(fx, '"82100000 writes 0x82005FB0/ED8 to +0/+28"', [A, B, C], unit=True, kind='field') == ['not_checkable'],
       'field: a bare "writes 0xV" may be a value written (a vtable): not found is not checkable, never refuted')
    ok(verdict(fx, 'writes 0x83000004', [A], kind='field') == ['not_checkable'] and verdict(fx, 'writes to address 0x83000004', [A], kind='field') == ['refuted'],
       'field: "writes to address G" (q sheet\'s form) is refuted when absent; a bare one is not checkable')
    ok(verdict(fx, '"global 0x83000000: writes +4"', [A], kind='field') == ['checked'], 'field: q sheet\'s "global G: writes +N" is G\'s field +N')
    ok(verdict(fx, '"global 0x83000000: writes +16"', [A], kind='field') == ['refuted'], 'field: "global G: writes +N" is not met by +N of another base')
    ok(qc.reading_status(['checked', 'not_checkable']) == 'all_checked' and qc.reading_status(['checked', 'refuted']) == 'refuted'
       and qc.reading_status(['not_checkable']) == 'nothing', 'reading status')

    # store round trip: build, stats, show, promote
    tmp = tempfile.mkdtemp(prefix='qclaim_t')
    db = os.path.join(tmp, 'read.s2.db')
    c = qread.open_rw(db)
    c.execute("insert into unit_read(unit_id,purpose,confidence,because,batch,model,when_) values (1,'p','medium','\"beta_func(m)\"','b1','m','2026-11-23 00:40:00')")
    c.execute("insert into unit_read(unit_id,purpose,confidence,because,batch,model,when_) values (2,'q','low','\"far_func(m)\"','b2','m','2026-11-23 02:00:00')")
    c.execute("insert into name_read(func,name,evidence,confidence,unit_id,batch,model) values (?,'gamma_dtor','called by alpha_func, delta_func','medium',1,'b1','m')", (C,))
    c.execute("insert into name_read(func,name,evidence,confidence,unit_id,batch,model) values (?,'delta_thing','calls far_func','medium',2,'b2','m')", (D,))
    c.execute("insert into name_read(func,name,evidence,confidence,unit_id,batch,model) values (?,'far_x','vague words','high',3,'b3','m')", (FAR,))
    c.commit()
    c.close()
    out = []
    n = qc.build(db, fx, out.append)
    ok(n > 0 and 'for 2 unit readings and 3 name readings' in out[0], 'build: %s' % out)
    qc.build(db, fx, out.append)
    ok(sqlite3.connect(db).execute('select count(*) from claim_read').fetchone()[0] == n, 'build again replaces the table (no duplicates)')
    st, kv = qc.stats(db)
    ok(st['unit'] == {'all_checked': 1, 'refuted': 1} and st['name'] == {'all_checked': 1, 'refuted': 1, 'nothing': 1},
       'stats per reading (%s)' % dict(st))
    lines = qc.stats_lines(db)
    ok(any('name readings 3: all checkable claims checked 1, at least one refuted 1, nothing checkable 1' in x for x in lines), 'stats lines (%s)' % lines)
    sh = qc.show_lines(1, db)
    ok(any('REFUTED' not in x and 'mention' in x for x in sh) and any('caller' in x for x in sh), 'show lines of unit 1 (%s)' % sh)
    ok(sqlite3.connect(db).execute('select count(*) from unit_read').fetchone()[0] == 2, 'readings untouched by the check')
    # promotion: a pattern db with a deleting_dtor label on C and on D; C's reading predates the tag on sheets; D has a refuted claim
    pdb = os.path.join(tmp, 'pattern.s1.db')
    p = sqlite3.connect(pdb)
    p.executescript('create table pattern_fn(func integer, kind text, evidence text);')
    p.execute("insert into pattern_fn values (?, 'deleting_dtor', 'x')", (C,))
    p.execute("insert into pattern_fn values (?, 'deleting_dtor', 'x')", (D,))
    # E: the game names it CFoo::ReadEndProcess and the read_end_process rule is made FROM that name, which the reader saw on its
    # sheet: no second source (the game's name is the path). Read before the tag, a checked claim, a low db name: only the game name stops it.
    p.execute("insert into pattern_fn values (?, 'read_end_process', 'game name CFoo::ReadEndProcess')", (E,))
    p.commit()
    p.close()
    c = sqlite3.connect(db)
    c.execute("insert into name_read(func,name,evidence,confidence,unit_id,batch,model) values (?,'cfoo_read_end_process','called by far_func','medium',1,'b1','m')", (E,))
    c.commit()
    c.close()
    qc.build(db, fx, out.append)
    ok(any(r[0] == E and r[2] == 'checked' for r in sqlite3.connect(db).execute("select target,kind,verdict from claim_read where read_kind='name'")),
       'E carries a checked claim (the promotion test below rests on it)')
    rows = qc.promote(db, fx, pdb)
    ok([(r[0], r[1], r[5]) for r in rows] == [(C, 'gamma_dtor', 'pattern')], 'promote: the pattern-agreeing proposal, read before the tag, with a checked claim (%s)' % rows)
    c = sqlite3.connect(db)
    c.execute("update unit_read set when_='2026-11-23 02:00:00' where unit_id=1")
    c.commit()
    c.close()
    ok(qc.promote(db, fx, pdb) == [], 'promote: a reading made after the tag was on the sheet is not independent of it')
    c = sqlite3.connect(db)
    rid = c.execute('select max(id) from name_read where func=?', (C,)).fetchone()[0]
    c.execute("update name_read set verdict='confirmed', v_note='probe says so', v_source='docs/X.md [C]', v_when='t' where id=?", (rid,))
    c.commit()
    c.close()
    rows = qc.promote(db, fx, pdb)
    ok([(r[0], r[5]) for r in rows] == [(C, 'probe')], 'promote: a probe verdict confirmed on the newest reading is source B (%s)' % rows)
    buf = io.StringIO()
    tsv = os.path.join(tmp, 'sub', 'p.tsv')
    with contextlib.redirect_stdout(buf):
        qc.main(['--promote', '--out', tsv], db, fx)
    ok(os.path.exists(tsv) and open(tsv).read().count('\n') == 2 and 'would promote 1' in buf.getvalue(), 'promote --out writes the list (%r)' % buf.getvalue())
    print('%d failure(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
