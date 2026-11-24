"""Selftest of q read verify (S110 VERIFYMARK). Works on a fresh temp db, never the real read.s2.db.

    py -I tools/q/qreadverify_selftest.py
"""
import io
import contextlib
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402
import qread  # noqa: E402
import qreadverify as qv  # noqa: E402

fails = []


def check(cond, what):
    print('%s  %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        fails.append(what)


def fake_checker():
    u = sqlite3.connect(':memory:')
    u.executescript('create table units(func integer, unit_id integer); create table unit(unit_id integer, lo integer, hi integer, n_funcs integer);')
    u.execute('insert into unit values (4623, 0x8250B000, 0x8250C000, 2)')
    u.execute('insert into unit values (7, 0x82000000, 0x82000100, 1)')
    for f, un in ((0x8250B7F0, 4623), (0x8250B8F0, 4623), (0x82000010, 7)):
        u.execute('insert into units values (?,?)', (f, un))
    ix = sqlite3.connect(':memory:')
    ix.executescript('create table funcs(addr integer, size integer, has_body integer);')
    for f, sz in ((0x8250B7F0, 0x100), (0x8250B8F0, 0x40), (0x82000010, 0x20)):
        ix.execute('insert into funcs values (?,?,1)', (f, sz))
    return qv.Checker(u, ix)


ANSWER = ('## a\nunit: 4623, r, 2 funcs\npurpose: Builds the audience. | confidence: medium | because: names\n'
          'names: sub_8250B7F0 -> set_up_sign_board_text | evidence: str "sign" | medium\nmissing: x\nlisting needed: no\n')
ANSWER2 = ('## b\nunit: 4623, r, 2 funcs\npurpose: Sets up the sign boards. | confidence: high | because: probe\n'
           'names: sub_8250B7F0 -> set_sign_board_text | evidence: str "sign" | high\nmissing: x\nlisting needed: no\n')


def count(db):
    return sqlite3.connect(db).execute('select count(*) from verify_read').fetchone()[0]


def sheet(qsheet, db):
    """the real sheet of unit 4623 over the temp reading store"""
    old = qread.READ_DB
    qread.READ_DB = db
    try:
        return qsheet.build(qsheet.Ctx(), 4623)[0]
    finally:
        qread.READ_DB = old


def main():
    tmp = tempfile.mkdtemp(prefix='qverify_t')
    db = os.path.join(tmp, 'read.s2.db')
    repo = os.path.join(tmp, 'repo')            # the repo root the source check resolves against
    os.makedirs(os.path.join(repo, 'docs'))
    src = os.path.join(repo, 'docs', 'PROBE.md')
    outside = os.path.join(tmp, 'OUTSIDE.md')    # a real file, but not in the repo (a scratch folder)
    for p in (src, outside):
        with open(p, 'w') as fh:
            fh.write('probe note\n')
    chk = fake_checker()
    qread.store(ANSWER, 'b1', 'mX', db)
    c = qread.open_ro(db)
    check(qv._ro(db) is None and qv.unit_verdict(4623, db) is None and qv.func_verdicts(0, 1 << 32, db) == {} and qv.show_lines(4623, db) == [],
          'old store without the table: readers return nothing')
    out = []
    qread.show(4623, db, out.append)
    check(any('Builds the audience' in x for x in out) and not any('VERIFIED' in x for x in out), 'old store: show still works')
    c.close()
    check(qv.stats_lines(db) == ['verdicts: none stored'], 'stats on a store without the table')

    # store + refusals
    n, bad = qv.add_rows([(0, 'unit', '4623', 'confirmed', src + ' [C]', 'the audience factory was seen')], db, chk, repo)
    check(n == 1 and not bad, 'store a unit verdict (source with a [C] mark)')
    n, bad = qv.add_rows([(0, 'func', '8250B7F0', 'refuted', 'docs/PROBE.md', 'it is the board text SETTER, not the getter')], db, chk, repo)
    check(n == 1 and not bad, 'store a func verdict')
    n, bad = qv.add_rows([(0, 'func', '8250B8F4', 'partly', src, 'inside a function: stored against its start')], db, chk, repo)
    check(n == 1, 'func address inside a known function is stored on the function start')
    refusals = [('unit', '99999', 'confirmed', src, 'x'), ('func', '70000000', 'confirmed', src, 'x'), ('func', '8250B7F0', 'maybe', src, 'x'),
                ('unit', '4623', 'confirmed', os.path.join(repo, 'nope.md'), 'x'), ('unit', '4623', 'confirmed', src, '  '), ('thing', '1', 'confirmed', src, 'x'),
                ('func', 'zzzz', 'confirmed', src, 'x'), ('unit', 'abc', 'confirmed', src, 'x'),
                ('func', '8250B940', 'confirmed', src, 'x'), ('func', '8FFFFFF0', 'confirmed', src, 'x'),
                ('unit', '4623', 'confirmed', outside, 'x'), ('unit', '4623', 'confirmed', '../OUTSIDE.md', 'x')]
    n, bad = qv.add_rows([(i + 1,) + r for i, r in enumerate(refusals)], db, chk, repo)
    check(n == 0 and [b[0] for b in bad] == list(range(1, 13)), 'refused: unknown unit, address in no function, bad verdict, missing source, empty note, bad kind, bad addresses,'
          ' an address past the end of the function before it (in the gap / past the image), a source outside the repo (absolute or ../) (%s)' % bad)
    check(sqlite3.connect(db).execute('select count(*) from verify_read').fetchone()[0] == 3, 'refused rows stored nothing')
    srcs = [r[0] for r in sqlite3.connect(db).execute('select source from verify_read order by id')]
    check(srcs == ['docs/PROBE.md [C]', 'docs/PROBE.md', 'docs/PROBE.md'], 'source stored repo-relative, so it resolves from the main checkout (%s)' % srcs)

    # TSV with a bad row
    tsv = os.path.join(tmp, 'v.tsv')
    with open(tsv, 'w') as fh:
        fh.write('# comment\nunit\t7\tconfirmed\t%s\tseven ok\n' % src)
        fh.write('unit\t8\tconfirmed\t%s\tunknown unit\n' % src)
        fh.write('func\t82000010\tpartly\t%s\tonly the first half\n' % src)
        fh.write('func\t82000010\tconfirmed\t%s\n' % src)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = qv.main(['--file', tsv], db, chk, repo)
    t = buf.getvalue()
    check('line 3 SKIPPED' in t and 'line 5 SKIPPED' in t and 'stored 2, skipped 2' in t and rc == 1, 'TSV: bad rows reported with line numbers, good ones stored (%r)' % t)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        qv.main(['--file', tsv], db, chk, repo)
    check('stored 0' in buf.getvalue() and sqlite3.connect(db).execute('select count(*) from verify_read').fetchone()[0] == 5,
          'TSV run again (after a SKIPPED line was fixed): its rows already stored are not stored twice (%r)' % buf.getvalue())
    # a note keeps its tab and a form feed / U+2028 (str.splitlines would cut the row there); a BOM before a comment line
    tsv2 = os.path.join(tmp, 'v2.tsv')
    odd = 'half one\tafter a tab \x0c then   the rest'
    with open(tsv2, 'w', encoding='utf-8-sig') as fh:
        fh.write('# comment after a BOM\r\nfunc\t82000010\trefuted\tdocs/PROBE.md\t%s\r\n' % odd)
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = qv.main(['--file', tsv2], db, chk, repo)
    last = sqlite3.connect(db).execute('select note from verify_read order by id desc limit 1').fetchone()[0]
    check(rc == 0 and 'stored 1, skipped 0' in buf.getvalue() and last == odd, 'TSV: one row per line, note verbatim (%r, %r)' % (buf.getvalue(), last))
    sqlite3.connect(db).execute('delete from verify_read where id=(select max(id) from verify_read)').connection.commit()

    # show, stats
    qv._unit_of_default = lambda: (lambda f: {0x8250B7F0: 4623, 0x8250B8F0: 4623, 0x82000010: 7}.get(f))
    out = []
    qread.show(4623, db, out.append)
    vl = [x for x in out if 'VERIFIED' in x]
    check(len(vl) == 3 and vl[0].startswith('  VERIFIED partly: sub_8250B8F0:') and 'VERIFIED refuted: sub_8250B7F0:' in vl[1]
          and vl[2].startswith('  VERIFIED confirmed: the audience factory') and 'PROBE.md [C]' in vl[2] and out.index(vl[0]) > max(i for i, x in enumerate(out) if 'name sub_' in x),
          'show: verdicts of the unit and its functions after the answers, newest first (%s)' % vl)
    out = []
    qread.show(7, db, out.append)
    check(any('no stored answer' in x for x in out) and sum('VERIFIED' in x for x in out) == 2, 'show of a unit without an answer still lists its verdicts')
    check(qv.stats_lines(db) == ['verdicts: 5 (confirmed 2 refuted 1 partly 2)', 'by kind: unit 2 func 3'], 'stats: %s' % qv.stats_lines(db))
    check(qv.unit_verdict(4623, db) == 'confirmed' and qv.func_verdicts(0x8250B000, 0x8250C000, db) == {0x8250B7F0: 'refuted', 0x8250B8F0: 'partly'}, 'newest verdict per target')
    qv.add_rows([(0, 'unit', '4623', 'refuted', src, 'later: it was wrong')], db, chk, repo)
    check(qv.unit_verdict(4623, db) == 'refuted' and sqlite3.connect(db).execute('select count(*) from verify_read').fetchone()[0] == 6, 'a later verdict wins; the old row stays (appended, never overwritten)')
    check(qv.tag('partly') == ' [v:partly]' and qv.tag(None) == '', 'tag text')
    w = sqlite3.connect(db)
    check(w.execute('select count(*) from unit_read').fetchone()[0] == 1, 'unit_read untouched by verdicts')
    w.close()

    # CLI: a --note value that begins with -- is the note, not an option; a note with a newline is refused
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = qv.main(['unit', '7', '--verdict', 'partly', '--source', 'docs/PROBE.md', '--note', '--stats'], db, chk, repo)
    nt = sqlite3.connect(db).execute('select note from verify_read order by id desc limit 1').fetchone()[0]
    check(rc == 0 and 'stored 1 unit verdict' in buf.getvalue() and nt == '--stats',
          'CLI: a note beginning with -- is stored as the note (%r, %r)' % (buf.getvalue(), nt))
    sqlite3.connect(db).execute('delete from verify_read where id=(select max(id) from verify_read)').connection.commit()
    try:
        qv.main(['unit', '7', '--verdict', 'partly', '--source', 'docs/PROBE.md', '--note', 'line one\nline two'], db, chk, repo)
        refused = False
    except qread.ReadError:
        refused = True
    check(refused and count(db) == 6, 'CLI: a note with a newline is refused')

    import qsheet
    have_index = os.path.exists(qnames.INDEX_DB)
    if not have_index:
        print('SKIP  real-index part (no index)')
    else:
        txt = sheet(qsheet, db)
        hd = [x for x in txt.splitlines() if x.startswith('read:')]
        check(hd and hd[0].endswith(' [v:refuted]'), 'sheet: read: header carries [v:refuted] (%s)' % hd)
        check('read set_up_sign_board_text [r:medium] [v:refuted]' in txt, 'sheet: the proposed name carries [v:refuted]')
        ln = [x for x in txt.splitlines() if x.startswith('8250B8F0 ')]
        check(ln and ln[0].endswith(' [v:partly]'), 'sheet: a func verdict on a function with no read name tags its line (%s)' % ln)

    # D1: a newer reading of the unit and a newer name for sub_8250B7F0: the verdicts on the earlier reading tag nothing
    qread.store(ANSWER2, 'b2', 'mY', db)
    check(qv.unit_verdict(4623, db) is None and qv.func_verdicts(0x8250B000, 0x8250C000, db) == {0x8250B8F0: 'partly'},
          'after a re-read: no tag from a verdict on the earlier reading; a verdict on a function with no read name stays (%s %s)'
          % (qv.unit_verdict(4623, db), qv.func_verdicts(0x8250B000, 0x8250C000, db)))
    out = []
    qread.show(4623, db, out.append)
    vl = [x for x in out if 'VERIFIED' in x]
    b8 = [x for x in vl if 'sub_8250B8F0' in x]
    check(len(vl) == 4 and sum(x.endswith(' (on an earlier reading)') for x in vl) == 3 and b8 and not b8[0].endswith('(on an earlier reading)'),
          'show: every verdict still listed, the ones on an earlier reading say so (%s)' % vl)
    if have_index:
        txt = sheet(qsheet, db)
        hd = [x for x in txt.splitlines() if x.startswith('read:')]
        check(hd and 'Sets up the sign boards' in hd[0] and '[v:' not in hd[0], 'sheet: the newer read: header is not tagged by the old verdict (%s)' % hd)
        check('read set_sign_board_text [r:high]' in txt and 'read set_sign_board_text [r:high] [v:' not in txt,
              'sheet: name A confirmed/refuted, newer name B stored: B is not tagged')
    qv.add_rows([(0, 'unit', '4623', 'confirmed', src, 'the new reading holds'), (0, 'func', '8250B7F0', 'confirmed', src, 'B holds')], db, chk, repo)
    check(qv.unit_verdict(4623, db) == 'confirmed' and qv.func_verdicts(0x8250B000, 0x8250C000, db).get(0x8250B7F0) == 'confirmed',
          'a verdict on the newest reading tags it again')

    # a store whose verify_read predates read_id: it is read, and the next write adds the column
    db2 = os.path.join(tmp, 'old', 'read.s2.db')
    os.makedirs(os.path.dirname(db2))
    qread.store(ANSWER, 'b1', 'mX', db2)
    o = sqlite3.connect(db2)
    o.execute('create table verify_read(id integer primary key autoincrement, target_kind text, target integer, verdict text, note text, source text, when_ text)')
    o.execute("insert into verify_read(target_kind,target,verdict,note,source,when_) values ('func',?,'partly','old row','docs/PROBE.md','x')", (0x8250B8F0,))
    o.commit()
    o.close()
    check(qv.func_verdicts(0x8250B000, 0x8250C000, db2) == {0x8250B8F0: 'partly'} and len(qv.show_lines(4623, db2)) == 1 and qv.unit_verdict(4623, db2) is None,
          'a table without read_id is read (its rows are on no reading)')
    n, bad = qv.add_rows([(0, 'unit', '4623', 'confirmed', src, 'new row')], db2, chk, repo)
    check(n == 1 and qv.unit_verdict(4623, db2) == 'confirmed', 'a table without read_id: the next write adds it (%s %s)' % (n, bad))
    print('%d failure(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
