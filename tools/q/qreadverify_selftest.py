"""Selftest of q read verify (S110 VERIFYMARK, S111 FOLD: the verdict is stored on the reading). Works on a fresh temp db, never the real read.s2.db.

    py -I tools/q/qreadverify_selftest.py
"""
import io
import contextlib
import os
import re
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
          'names: sub_8250B7F0 -> set_up_sign_board_text | evidence: str "sign" | medium\n'
          'names: sub_8250B8F0 -> second_name | evidence: str "board" | low\nmissing: x\nlisting needed: no\n'
          '## b\nunit: 7, r, 1 funcs\npurpose: Seven. | confidence: low | because: x\nmissing: x\nlisting needed: no\n')
ANSWER2 = ('## b\nunit: 4623, r, 2 funcs\npurpose: Sets up the sign boards. | confidence: high | because: probe\n'
           'names: sub_8250B7F0 -> set_sign_board_text | evidence: str "sign" | high\nmissing: x\nlisting needed: no\n')


def sheet(qsheet, db, **kw):
    """the real sheet of unit 4623 over the temp reading store"""
    old = qread.READ_DB
    qread.READ_DB = db
    try:
        return qsheet.build(qsheet.Ctx(), 4623, **kw)[0]
    finally:
        qread.READ_DB = old


def tables(db):
    return {r[0] for r in sqlite3.connect(db).execute("select name from sqlite_master where type='table'")}


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
    # a store made before the verdict columns: readers return nothing, the first write adds the columns
    old = os.path.join(tmp, 'old', 'read.s2.db')
    os.makedirs(os.path.dirname(old))
    o = sqlite3.connect(old)
    o.executescript('create table unit_read(id integer primary key autoincrement, unit_id integer, purpose text, confidence text, because text,'
                    ' missing text, listing_needed integer, batch text, model text, when_ text, system text default \'\', fun text default \'\');'
                    'create table name_read(id integer primary key autoincrement, func integer, name text, evidence text, confidence text,'
                    ' unit_id integer, batch text, model text);')
    o.execute("insert into unit_read(unit_id,purpose,confidence) values (4623,'p','low')")
    o.commit()
    o.close()
    check(qv._ro(old) is None and qv.unit_verdict(4623, old) is None and qv.func_verdicts(0, 1 << 32, old) == {} and qv.show_lines(4623, old) == []
          and qv.stats_lines(old) == ['verdicts: none stored'], 'a store without the verdict columns: readers return nothing')
    n, bad = qv.add_rows([(0, 'unit', '4623', 'confirmed', src, 'first write adds the columns')], old, chk, repo)
    check(n == 1 and qv.unit_verdict(4623, old) == 'confirmed', 'a store without the verdict columns: the first write adds them (%s %s)' % (n, bad))

    qread.store(ANSWER, 'b1', 'mX', db)
    check('verify_read' not in tables(db), 'FOLD: a new store has no separate verdict table')
    out = []
    qread.show(4623, db, out.append)
    check(any('Builds the audience' in x for x in out) and not any('VERIFIED' in x for x in out), 'show works with no verdicts')
    check(qv.stats_lines(db) == ['verdicts: 0 (confirmed 0 refuted 0 partly 0)', 'by kind: unit 0 func 0'], 'stats with none: %s' % qv.stats_lines(db))

    # store + refusals
    n, bad = qv.add_rows([(0, 'unit', '4623', 'confirmed', src + ' [C]', 'the audience factory was seen')], db, chk, repo)
    check(n == 1 and not bad, 'store a unit verdict (source with a [C] mark)')
    w = sqlite3.connect(db)
    row = w.execute('select verdict,v_note,v_source,v_when from unit_read where unit_id=4623').fetchone()
    check(row[0] == 'confirmed' and row[1].startswith('the audience') and row[2] == 'docs/PROBE.md [C]' and row[3], 'FOLD: the verdict, note, source and time are on the reading row (%s)' % (row,))
    w.close()
    n, bad = qv.add_rows([(0, 'func', '8250B7F0', 'refuted', 'docs/PROBE.md', 'it is the board text SETTER, not the getter')], db, chk, repo)
    check(n == 1 and not bad, 'store a func verdict on the reader name')
    n, bad = qv.add_rows([(0, 'func', '8250B8F4', 'partly', src, 'inside a function: stored against its start')], db, chk, repo)
    check(n == 1, 'func address inside a known function is stored on the function start')
    n, bad = qv.add_rows([(0, 'func', '82000010', 'confirmed', src, 'a function with no stored reading')], db, chk, repo)
    check(n == 0 and len(bad) == 1 and 'no stored reading' in bad[0][1], 'a function with no reader name is refused: nothing to judge (%s)' % bad)
    refusals = [('unit', '99999', 'confirmed', src, 'x'), ('func', '70000000', 'confirmed', src, 'x'), ('func', '8250B7F0', 'maybe', src, 'x'),
                ('unit', '4623', 'confirmed', os.path.join(repo, 'nope.md'), 'x'), ('unit', '4623', 'confirmed', src, '  '), ('thing', '1', 'confirmed', src, 'x'),
                ('func', 'zzzz', 'confirmed', src, 'x'), ('unit', 'abc', 'confirmed', src, 'x'),
                ('func', '8250B940', 'confirmed', src, 'x'), ('func', '8FFFFFF0', 'confirmed', src, 'x'),
                ('unit', '4623', 'confirmed', outside, 'x'), ('unit', '4623', 'confirmed', '../OUTSIDE.md', 'x')]
    n, bad = qv.add_rows([(i + 1,) + r for i, r in enumerate(refusals)], db, chk, repo)
    check(n == 0 and [b[0] for b in bad] == list(range(1, 13)), 'refused: unknown unit, address in no function, bad verdict, missing source, empty note, bad kind, bad addresses,'
          ' an address past the end of the function before it (in the gap / past the image), a source outside the repo (absolute or ../) (%s)' % bad)
    check(qv.stats_lines(db) == ['verdicts: 3 (confirmed 1 refuted 1 partly 1)', 'by kind: unit 1 func 2'], 'refused rows stored nothing: %s' % qv.stats_lines(db))
    srcs = [r[0] for r in sqlite3.connect(db).execute('select v_source from unit_read where verdict is not null union all select v_source from name_read where verdict is not null order by 1')]
    check(srcs == ['docs/PROBE.md', 'docs/PROBE.md', 'docs/PROBE.md [C]'], 'source stored repo-relative, so it resolves from the main checkout (%s)' % srcs)

    # the same verdict again stores nothing; a later verdict on the same reading replaces
    n, bad = qv.add_rows([(0, 'unit', '4623', 'confirmed', src + ' [C]', 'the audience factory was seen')], db, chk, repo)
    check(n == 0 and not bad, 'the same verdict, note and source again stores nothing')

    # show, stats
    out = []
    qread.show(4623, db, out.append)
    vl = [x for x in out if 'VERIFIED' in x]
    check(len(vl) == 3 and any('VERIFIED partly: sub_8250B8F0:' in x for x in vl) and any('VERIFIED refuted: sub_8250B7F0:' in x for x in vl)
          and any(x.startswith('  VERIFIED confirmed: the audience factory') and 'PROBE.md [C]' in x for x in vl)
          and out.index(vl[0]) > max(i for i, x in enumerate(out) if 'name sub_' in x),
          'show: verdicts of the unit and its functions after the answers (%s)' % vl)
    check(qv.stats_lines(db) == ['verdicts: 3 (confirmed 1 refuted 1 partly 1)', 'by kind: unit 1 func 2'], 'stats: %s' % qv.stats_lines(db))
    check(qv.unit_verdict(4623, db) == 'confirmed' and qv.func_verdicts(0x8250B000, 0x8250C000, db) == {0x8250B7F0: 'refuted', 0x8250B8F0: 'partly'}, 'verdict per target')
    qv.add_rows([(0, 'unit', '4623', 'refuted', src, 'later: it was wrong')], db, chk, repo)
    check(qv.unit_verdict(4623, db) == 'refuted' and sqlite3.connect(db).execute('select count(*) from unit_read where verdict is not null').fetchone()[0] == 1,
          'a later verdict on the same reading replaces the earlier one (one verdict per reading)')
    check(qv.tag('partly') == ' [v:partly]' and qv.tag(None) == '', 'tag text')
    w = sqlite3.connect(db)
    check(w.execute('select count(*) from unit_read').fetchone()[0] == 2 and w.execute('select purpose from unit_read where unit_id=4623').fetchone()[0] == 'Builds the audience.',
          'the reading itself is untouched by verdicts')
    w.close()

    # CLI: a --note value that begins with -- is the note, not an option; a note with a newline is refused
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        rc = qv.main(['unit', '7', '--verdict', 'partly', '--source', 'docs/PROBE.md', '--note', '--stats'], db, chk, repo)
    nt = sqlite3.connect(db).execute('select v_note from unit_read where unit_id=7').fetchone()[0]
    check(rc == 0 and 'stored 1 unit verdict' in buf.getvalue() and nt == '--stats',
          'CLI: a note beginning with -- is stored as the note (%r, %r)' % (buf.getvalue(), nt))
    try:
        qv.main(['unit', '7', '--verdict', 'partly', '--source', 'docs/PROBE.md', '--note', 'line one\nline two'], db, chk, repo)
        refused = False
    except qread.ReadError:
        refused = True
    check(refused, 'CLI: a note with a newline is refused')
    try:
        qv.main(['--file', 'x.tsv'], db, chk, repo)
        refused = False
    except qread.ReadError:
        refused = True
    check(refused, 'FOLD: the TSV hand-entry path is gone (--file is an unknown option)')

    import qsheet
    have_index = os.path.exists(qnames.INDEX_DB)
    if not have_index:
        print('SKIP  real-index part (no index)')
    else:
        txt = sheet(qsheet, db)
        hd = [x for x in txt.splitlines() if x.startswith('read:')]
        check(hd and hd[0].endswith(' [v:refuted]'), 'sheet: read: header carries [v:refuted] (%s)' % hd)
        check('read set_up_sign_board_text [r:medium] [v:refuted]' in txt, 'sheet: the proposed name carries [v:refuted]')
        # S111 FOLD review: a sheet made FOR A READER carries no earlier reading (purpose + confidence, proposed names, their verdicts):
        # a reading shown them took part of its answer and its confidence from them (the rule that removed the # NEIGHBOURS part)
        rt = sheet(qsheet, db, earlier=False)
        check(not [x for x in rt.splitlines() if x.startswith('read:')] and '[r:' not in rt and '[v:' not in rt and 'set_up_sign_board_text' not in rt,
              'reader sheet (earlier=False): no read: line, no proposed name, no verdict tag')
        check(rt.split('\n')[0] == txt.split('\n')[0], 'reader sheet: the same unit header as the full sheet')

    # a newer reading of the unit and a newer name for sub_8250B7F0: the verdicts on the earlier reading tag nothing
    qread.store(ANSWER2, 'b2', 'mY', db)
    check(qv.unit_verdict(4623, db) is None and qv.func_verdicts(0x8250B000, 0x8250C000, db) == {0x8250B8F0: 'partly'},
          'after a re-read: no tag from a verdict on the earlier reading; a function whose newest name is the old one keeps its verdict (%s %s)'
          % (qv.unit_verdict(4623, db), qv.func_verdicts(0x8250B000, 0x8250C000, db)))
    out = []
    qread.show(4623, db, out.append)
    vl = [x for x in out if 'VERIFIED' in x]
    b8 = [x for x in vl if 'sub_8250B8F0' in x]
    check(len(vl) == 3 and sum(x.endswith(' (on an earlier reading)') for x in vl) == 2 and b8 and not b8[0].endswith('(on an earlier reading)'),
          'show: every verdict still listed, the ones on an earlier reading say so (%s)' % vl)
    if have_index:
        txt = sheet(qsheet, db)
        hd = [x for x in txt.splitlines() if x.startswith('read:')]
        check(hd and 'Sets up the sign boards' in hd[0] and '[v:' not in hd[0], 'sheet: the newer read: header is not tagged by the old verdict (%s)' % hd)
        check('read set_sign_board_text [r:high]' in txt and 'read set_sign_board_text [r:high] [v:' not in txt,
              'sheet: name A refuted, newer name B stored: B is not tagged')
    qv.add_rows([(0, 'unit', '4623', 'confirmed', src, 'the new reading holds'), (0, 'func', '8250B7F0', 'confirmed', src, 'B holds')], db, chk, repo)
    check(qv.unit_verdict(4623, db) == 'confirmed' and qv.func_verdicts(0x8250B000, 0x8250C000, db).get(0x8250B7F0) == 'confirmed',
          'a verdict on the newest reading tags it again')
    # every tool that writes sheets for a READER asks for them without the earlier readings
    for rel, call in (('qread.py', 'qsheet.build(ctx, u, earlier=False)'), ('qreadsys.py', 'qsheet.build(ctx, u, earlier=False)'),
                      ('qreadpick.py', "qsheet.build(ctx, r['unit'], earlier=False)"), ('qlsheet_todo.py', 'qsheet.sheet(u, earlier=False)')):
        src = open(os.path.join(HERE, rel), encoding='utf-8').read()
        bare = [x for x in re.findall(r'qsheet\.(?:build|sheet)\([^)]*\)', src) if 'earlier=False' not in x]
        check(call in src and not bare, 'reader sheets: %s builds them with earlier=False (bare calls: %s)' % (rel, bare))
    print('%d failure(s)' % len(fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
