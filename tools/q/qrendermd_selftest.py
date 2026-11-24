r"""Selftest of tools/q/qrendermd.py: temp databases, no index needed; the real part SKIPs without one.

    py -I tools\q\qrendermd_selftest.py
"""
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qrendermd as R  # noqa: E402
import qread  # noqa: E402

FAILS = []
WHEN = '2026-01-01 00:00:00'


def check(ok, what):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        FAILS.append(what)


def mk(tmp):
    """cluster u (funcs u*100+1..3) writes the listed sites of one family; a site is (family, n)"""
    seen = sqlite3.connect(os.path.join(tmp, 'seen.db'))
    seen.executescript('create table reads(file text, line integer, addr integer, width integer, runs integer);'
                       'create table writers(file text, line integer, func integer, n_addrs integer, runs integer, via text, nocaller integer);'
                       'create table runs(run text, read_rows integer, store_rows integer, mtime integer, lr_dropped integer);')
    seen.execute("insert into runs values ('r1', 1, 1, 0, 0)")
    units = sqlite3.connect(os.path.join(tmp, 'units.db'))
    units.executescript('create table units(func integer primary key, unit_id integer);'
                        'create table unit(unit_id integer primary key, lo integer, hi integer, n_funcs integer);')
    rows = []
    nxt = [1]

    def site(fam):
        n = nxt[0]
        nxt[0] += 1
        f = 'src/%s.h' % fam
        rows.append({'kind': 'field', 'family': fam, 'file': f, 'line': n, 'result': 'unknown'})
        seen.execute('insert into reads values (?,?,?,?,?)', (f, n, 0x1000 + n * 8, 4, 1))
        return f, n

    def cluster(u, fam, nsites):
        units.execute('insert into unit values (?,?,?,?)', (u, u * 100 + 1, u * 100 + 3, 3))
        for k in (1, 2, 3):
            units.execute('insert into units values (?,?)', (u * 100 + k, u))
        for _ in range(nsites):
            f, n = site(fam)
            seen.execute('insert into writers values (?,?,?,?,?,?,?)', (f, n, u * 100 + 1, 1, 1, '', 0))
    return seen, units, rows, cluster


def answer(rc, u, purpose, conf, system='', fun='', listing=0, when=WHEN):
    rc.execute('insert into unit_read(unit_id,purpose,confidence,because,missing,listing_needed,batch,model,when_,system,fun) values (?,?,?,?,?,?,?,?,?,?,?)',
               (u, purpose, conf, '', '', listing, 'b', 'm', when, system, fun))


def fact(rc, text, unit, conf, system, source):
    rc.execute('insert into fact_read(text,unit_id,confidence,system,source,when_) values (?,?,?,?,?,?)', (text, unit, conf, system, source, WHEN))


def explain(rc, system, text, when):
    rc.execute('insert into system_read(system,explain,model,when_) values (?,?,?,?)', (system, text, 'writerM', when))


COVER = {'functions': 1000, 'callees_pct': 80.0, 'callers_pct': 50.0, 'both_pct': 40.0, 'game_named': 20, 'game_named_pct': 2.0, 'n_multi': 50}


def build(tmp, setup):
    tmp = tempfile.mkdtemp(dir=tmp)
    seen, units, rows, cluster = mk(tmp)
    rpath = os.path.join(tmp, 'read.s2.db')
    rc = qread.open_rw(rpath)
    setup(cluster, rc)
    rc.commit()
    return R.gather(rows, seen, units, rc, lambda a: None, COVER, ([('g:1', 'nm', 10, 2, 8)], 8, 'foot'), [])


LONG = ('The skeleton sampler turns packed keyframes into poses for every drawn character; this purpose is long on purpose, ' * 4).strip()
EXPLAIN = 'First paragraph of the pose story.\n\nSecond paragraph, with `unit 1` named (unit 1) and a plain sentence that is not known yet.\n\nThird paragraph ' + 'x' * 600


def main():
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as tmp:
        def s1(cluster, rc):
            cluster(1, 'model', 3)          # observed writing model sites, the reader says pose
            cluster(2, 'model', 5)          # reader: pose, long purpose
            cluster(3, 'model', 4)          # unread
            cluster(4, 'light', 2)          # no system stated (read before the field existed)
            cluster(5, 'crowd', 2)          # crowd, medium
            answer(rc, 1, 'Small pose thing.', 'high', 'pose', 'Poses are packed into small integers.')
            answer(rc, 2, LONG, 'high', 'pose')
            answer(rc, 4, 'Old light thing.', 'low')
            answer(rc, 5, 'Crowd thing.', 'medium', 'crowd', 'Crowd members hold signs.')
            answer(rc, 6, 'Allocator.', 'high', 'engine')
            explain(rc, 'pose', EXPLAIN, '2026-06-01 00:00:00')                        # newer than its readings: fresh
            explain(rc, 'crowd', 'Crowd story.', '2025-01-01 00:00:00')                # older than the crowd reading: stale
            fact(rc, 'Poses are packed into small integers.', 1, 'high', 'pose', 'reader')
            fact(rc, 'Crowd members hold signs.', 5, 'medium', 'crowd', 'reader')
            fact(rc, 'A writer fact about poses.', 2, 'medium', 'pose', 'writer')
            fact(rc, 'a low  fact that must not appear', 4, 'low', 'light', 'writer')
            fact(rc, 'Poses are packed into SMALL integers!', 1, 'high', 'pose', 'reader')      # duplicate text
        d = build(tmp, s1)
        txt = R.render(d, '2026-01-01 00:00')
        hs = [h for h, _ in R.sections(txt)]
        check(hs == ['## Part 1: How the systems work', '## Part 2: Fun facts', '## Part 3: Reference'], 'three parts in order (%s)' % hs)
        p1 = txt.split('## Part 1')[1].split('## Part 2')[0]
        pose = p1.split('### pose')[1].split('###')[0]
        check(EXPLAIN in pose and 'written by writerM on 2026-06-01 from 2 cluster readings [reader-based: a lead, not a fact]' in pose and 'STALE' not in pose,
              'a written system prints in full with its header line, not stale')
        check('not written yet (0 clusters read, 1 not read)' in p1.split('### model')[1].split('###')[0], 'an unwritten feed system says so, with its unread count')
        crowd = p1.split('### crowd')[1].split('###')[0]
        check('Crowd story.' in crowd and 'STALE:' in crowd, 'a stale explanation is printed and flagged')
        check('not written yet (1 clusters read)' in p1.split('### engine')[1].split('###')[0], 'a non-feed system says how many clusters are read')
        p2 = txt.split('## Part 2')[1].split('## Part 3')[0]
        check('low  fact' not in p2 and 'must not appear' not in p2, 'a low fact never appears')
        check(p2.count('packed into') == 1 and '[reader: high]' in p2 and '[writer: medium]' in p2, 'facts deduped by text, each ends in its mark')
        check(p2.index('### pose') < p2.index('### crowd'), 'facts grouped by system in the fixed order')
        p3 = txt.split('## Part 3')[1]
        posec = p3.split('#### pose')[1].split('####')[0]
        check(LONG in posec and '...' not in posec, 'a purpose is never cut')
        check(posec.index('unit 2,') < posec.index('unit 1,') and '(seen writing: model 5)' in posec, 'clusters sit under the reader\'s system, most sites first, with the seen-writing note')
        check('#### model' not in p3 and '#### unclassified' in p3 and 'unit 4,' in p3.split('#### unclassified')[1], 'the family of the observed writes does not group; a reading without system is unclassified')
        check('unit 3, 4 read sites (seen writing: model 4): top function sub_%08X' % 301 in p3.split('#### not read yet')[1], 'an unread cluster says so with its top function')
        check('[reader: high]' in txt and '[seen 1 runs]' in txt and '[static]' in txt, 'standing marks present')

        def s2(cluster, rc):
            for u in range(10, 10 + R.CAP_READ_PER_SYSTEM + 3):
                cluster(u, 'crowd', 2)
                answer(rc, u, 'Crowd unit %d.' % u, 'high', 'crowd')
            for u in range(40, 40 + R.CAP_UNREAD_CLUSTERS + 2):
                cluster(u, 'crowd', 1)
            for i in range(R.CAP_FACTS + 5):
                fact(rc, 'Fact number %d' % i, 10, 'high', 'crowd', 'writer')
        t2 = R.render(build(tmp, s2), 'x')
        crowd3 = t2.split('#### crowd')[1].split('####')[0]
        check(sum(1 for ln in crowd3.splitlines() if ln.startswith('- unit')) == R.CAP_READ_PER_SYSTEM and 'not shown: 3 more read clusters of this system (cap %d)' % R.CAP_READ_PER_SYSTEM in crowd3,
              'the per-system cap holds and says what it dropped')
        check('not shown: 2 more unread clusters' in t2, 'the unread cap says what it dropped')
        p2b = t2.split('## Part 2')[1].split('## Part 3')[0]
        check(p2b.count('\n- Fact number') == R.CAP_FACTS and 'not shown: 5 more facts (cap %d)' % R.CAP_FACTS in p2b, 'the fact cap holds and says what it dropped')
        check(len(t2.encode('utf-8')) < R.MAX_BYTES, 'file under the byte cap')

    a = 'line one\nGENERATED by tools\\q\\qrendermd.py on 2026-01-01 00:00 from x\nline three\n'
    b = a.replace('2026-01-01 00:00', '2027-02-02 11:11')
    check(R.same_but_date(a, b), '--check passes on a fresh file (date line ignored)')
    check(not R.same_but_date(a, b.replace('line three', 'line 3')), '--check fails after one edited line')

    idx = r'C:\Users\Jarrod\wwe2k14-recomp\out\harness\index\feedseen.s2.db'
    if not os.path.exists(idx):
        print('SKIP  real index part (no %s)' % idx)
    else:
        doc = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'docs', 'RENDERER.md')
        if os.path.exists(doc):
            n = os.path.getsize(doc)
            check(n <= R.MAX_BYTES, 'docs/RENDERER.md is %d bytes (cap %d)' % (n, R.MAX_BYTES))
    print('%d failed' % len(FAILS) if FAILS else 'all ok')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
