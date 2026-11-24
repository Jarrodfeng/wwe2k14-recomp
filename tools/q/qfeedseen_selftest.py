r"""selftest of qfeedseen.py: a synthetic pair of tsv files in a temp folder, then (when the files exist) the real run 17."""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qfeedseen as fs   # noqa: E402
import qcopier           # noqa: E402

NOCOP = qcopier.Copiers({})   # the first cases are about overlap only: no copier, whatever the real index holds

REAL = r'C:\Users\Jarrod\wwe2k14-recomp\out\s110\run17_cov'
FAIL = []


def check(name, cond, extra=''):
    print('%s %s %s' % ('ok  ' if cond else 'FAIL', name, extra))
    if not cond:
        FAIL.append(name)


def put(d, name, text):
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), 'w', newline='\n') as fh:
        fh.write(text)


def synthetic():
    with tempfile.TemporaryDirectory() as t:
        cov = os.path.join(t, 'cov')
        a, b = os.path.join(cov, 'runA'), os.path.join(cov, 'runB')
        put(a, 'feed_reads.tsv', 'file\tline\taddr\twidth\tn_addrs_dropped\n'
            'src/renderer_game/../scene_live/x.h\t10\t0x80001000\t4\t0\n'     # overlapped by a store at 0x80000FFE w4
            'src/scene_live/x.h\t20\t0x80002000\t4\t0\n'                      # only a width-0 row
            'src/scene_live/x.h\t30\t0x80003000\t4\t0\n'                      # only a func-0 row
            'src/scene_live/x.h\t40\t0x80004000\t4\t0\n')                     # nothing
        put(a, 'stores.tsv', 'func\tline\taddr\twidth\n'
            '0x82001234\t5\t0x80000FFE\t4\n'
            '0x82005678\t6\t0x80002000\t0\n'
            '0x00000000\t7\t0x80003000\t4\n'
            '0x82009999\t8\t0x80001004\t4\n')                                 # adjacent, not overlapping
        put(b, 'feed_reads.tsv', 'file\tline\taddr\twidth\tn_addrs_dropped\nsrc/scene_live/x.h\t10\t0x80001000\t4\t0\n')
        put(b, 'stores.tsv', 'func\tline\taddr\twidth\n0x82001234\t5\t0x80001000\t4\n')
        db = os.path.join(t, 'f.db')
        con = fs.open_db(cov=cov, db_path=db, cop=NOCOP)
        check('path normalised: one site (src/scene_live/x.h, 10)', ('src/scene_live/x.h', 10) in fs.sites(con))
        w = fs.writers_of('src/scene_live/x.h', 10, con)
        check('an overlapping store names a writer, the adjacent one does not', [x[0] for x in w] == [0x82001234], str(w))
        check('a second run raises runs', w and w[0][2] == 2, str(w))
        check('a width-0 row is not a writer', fs.writers_of('src/scene_live/x.h', 20, con) == [] and fs.unnamed_of('src/scene_live/x.h', 20, con) == (0, 0))
        check('a func-0 row is counted apart', fs.writers_of('src/scene_live/x.h', 30, con) == [] and fs.unnamed_of('src/scene_live/x.h', 30, con) == (1, 1))
        check('no store: no writer', fs.writers_of('src/scene_live/x.h', 40, con) == [])
        check('reverse lookup', fs.sites_of_func(0x82001234, con) == [('src/scene_live/x.h', 10, 1, 2)], str(fs.sites_of_func(0x82001234, con)))
        check('runs table', fs.n_runs(con) == 2)
        con.close()
        # a third run added through --also: the db rebuilds
        c = os.path.join(t, 'extra')
        put(c, 'feed_reads.tsv', 'file\tline\taddr\twidth\tn_addrs_dropped\nsrc/scene_live/x.h\t40\t0x80004000\t4\t0\n')
        put(c, 'stores.tsv', 'func\tline\taddr\twidth\n0x82000042\t1\t0x80003FFF\t2\n')
        con = fs.open_db(also=[c], cov=cov, db_path=db, cop=NOCOP)
        check('--also adds a run and a writer', fs.n_runs(con) == 3 and [x[0] for x in fs.writers_of('src/scene_live/x.h', 40, con)] == [0x82000042])
        con.close()
    copier()
    # nothing at all
    with tempfile.TemporaryDirectory() as t:
        try:
            fs.open_db(cov=t, db_path=os.path.join(t, 'z.db'), cop=NOCOP)
            check('no files raises', False)
        except fs.FeedSeenError:
            check('no files raises', True)


def copier():
    """S110 COPIER: a copier row with an lr names its caller; without one (old format) or for a non-leaf copier it stays flagged"""
    MEMSET, MEMCPY, CALLER, DIRECT = 0x82C79530, 0x82C79000, 0x82200000, 0x82201111
    idx = qcopier.Index([(CALLER, 0x100, 'sub_82200000'), (MEMSET, 0xA0, 'memset'), (MEMCPY, 0x488, 'memcpy')], edge_srcs={MEMCPY})
    cop = qcopier.compute({MEMSET: 'memset', MEMCPY: 'memcpy', DIRECT: 'draw_thing'}, idx)
    check('copier set: memset and memcpy by name, draw_thing not', set(cop.copiers) == {MEMSET, MEMCPY}, str(sorted(cop.copiers)))
    check('memset is a leaf (lr used), memcpy with a call edge is not', cop.attrib(MEMSET) and not cop.attrib(MEMCPY))
    check('name rule', qcopier.name_rule('memset') and qcopier.name_rule('__memcpy_aligned') and qcopier.name_rule('strcpy')
          and qcopier.name_rule('bzero') and not qcopier.name_rule('memory_pool')
          and not qcopier.name_rule('memset_zero_56_records_of_12_bytes_stride_88') and not qcopier.name_rule(None))
    f = 'src/scene_live/y.h'
    with tempfile.TemporaryDirectory() as t:
        cov = os.path.join(t, 'cov')
        a, b = os.path.join(cov, 'runA'), os.path.join(cov, 'runB')
        reads = ['file\tline\taddr\twidth\tn_addrs_dropped\n']
        for i in range(1, 6):
            reads.append('%s\t%d\t0x%08X\t4\t0\n' % (f, i * 10, 0x80005000 + i * 0x1000))
        put(a, 'feed_reads.tsv', ''.join(reads))
        put(a, 'stores.tsv', 'func\tline\taddr\twidth\tlr\n'
            '0x%08X\t1\t0x80006000\t4\t0x82200044\n' % MEMSET +          # site 10: memset called from CALLER -> CALLER writes
            '0x%08X\t1\t0x80007000\t4\t0x00000000\n' % MEMSET +          # site 20: memset, no lr -> copier only
            '0x%08X\t1\t0x80008000\t4\t0x00000000\n' % MEMSET +          # site 30: memset without lr ...
            '0x%08X\t2\t0x80008000\t4\t0x00000000\n' % DIRECT +          # ... and a real writer -> named
            '0x%08X\t1\t0x80009000\t4\t0x82200044\n' % MEMCPY +          # site 40: non-leaf copier with an lr: NOT attributed
            '0x%08X\t1\t0x8000A000\t4\t0x82200050\n' % MEMSET +          # site 50: lr 0x82200050 -> CALLER too
            '# lr_dropped 3\n')
        put(b, 'feed_reads.tsv', 'file\tline\taddr\twidth\tn_addrs_dropped\n%s\t20\t0x80007000\t4\t0\n' % f)
        put(b, 'stores.tsv', 'func\tline\taddr\twidth\n0x%08X\t1\t0x80007000\t4\n' % MEMSET)   # an old-format file
        con = fs.open_db(cov=cov, db_path=os.path.join(t, 'c.db'), cop=cop)
        check('a copier row with an lr: the caller is the writer', [x[0] for x in fs.writers_of(f, 10, con)] == [CALLER], str(fs.writers_of(f, 10, con)))
        check('... and the copier is kept as via', fs.writers_via(f, 10, con) == {CALLER: [MEMSET]}, str(fs.writers_via(f, 10, con)))
        check('a copier row with no lr stays the copier, flagged, not a writer',
              fs.writers_of(f, 20, con) == [] and [x[0] for x in fs.copier_only_of(f, 20, con)] == [MEMSET])
        check('a site with a real writer and a copier: the real one is the writer',
              [x[0] for x in fs.writers_of(f, 30, con)] == [DIRECT] and [x[0] for x in fs.copier_only_of(f, 30, con)] == [MEMSET])
        check('a non-leaf copier is not attributed through lr', fs.writers_of(f, 40, con) == [] and [x[0] for x in fs.copier_only_of(f, 40, con)] == [MEMCPY])
        check('copier-only sites: 20 and 40', fs.copier_only_sites(con) == {(f, 20), (f, 40)}, str(fs.copier_only_sites(con)))
        check('named sites: 10, 30, 50', fs.named_sites(con) == {(f, 10), (f, 30), (f, 50)}, str(fs.named_sites(con)))
        check('per-caller listing of the copier', [(c, l) for c, _f, l, _n, _r in fs.callers_of_copier(MEMSET, con)] == [(CALLER, 10), (CALLER, 50)],
              str(fs.callers_of_copier(MEMSET, con)))
        check('the copier rows (flagged) are the copier\'s own sites', [(x[0], x[1]) for x in fs.sites_of_func(MEMSET, con)] == [(f, 20), (f, 30)])
        check('the lr_dropped count is read', con.execute('select lr_dropped from runs where run=?', ('runA',)).fetchone()[0] == 3)
        check('a copier is in the table; a non-copier is not', fs.copier_info(MEMSET, con)[1] == 1 and fs.copier_info(MEMCPY, con)[1] == 0
              and fs.copier_info(DIRECT, con) is None)
        rows = [{'kind': 'x', 'family': 'FAM', 'file': f, 'line': i * 10, 'result': 'unknown'} for i in range(1, 6)]
        lines = []
        fs.seen_summary(rows, con, ['FAM'], out=lines.append)
        allrow = [x for x in lines if x.startswith('  ALL')][0]
        check('summary: ALL row has the only-a-copier column (5 sites, 5 reached, 0 static, 3 observed, 3 either, 2 only a copier, 0 unknown)',
              [int(x) for x in allrow.split()[1:] if x != '/'] == [5, 5, 0, 3, 3, 2, 0], allrow)
        con.close()
    # a function writing sites in 6 different files is a candidate, not a copier
    with tempfile.TemporaryDirectory() as t:
        cov = os.path.join(t, 'cov')
        a = os.path.join(cov, 'runA')
        put(a, 'feed_reads.tsv', 'file\tline\taddr\twidth\tn_addrs_dropped\n' +
            ''.join('src/scene_live/f%d.h\t1\t0x%08X\t4\t0\n' % (i, 0x80010000 + i * 0x100) for i in range(6)))
        put(a, 'stores.tsv', 'func\tline\taddr\twidth\n' + ''.join('0x82300000\t1\t0x%08X\t4\n' % (0x80010000 + i * 0x100) for i in range(6)))
        con = fs.open_db(cov=cov, db_path=os.path.join(t, 'd.db'), cop=cop)
        check('breadth: 6 files make a candidate row, not a copier', fs.breadth_rows(con) == [(0x82300000, 6, 6)] and fs.copier_info(0x82300000, con) is None,
              str(fs.breadth_rows(con)))
        con.close()


def real():
    if not os.path.exists(os.path.join(REAL, 'feed_reads.tsv')):
        print('SKIP real run 17: %s not present' % REAL)
        return
    with tempfile.TemporaryDirectory() as t:
        con = fs.open_db(also=[REAL], cov=t, db_path=os.path.join(t, 'r.db'))
        s = fs.summary(con)
        print(s)
        check('run 17: 435 sites', s['sites'] == 435, str(s['sites']))
        check('run 17: 249 sites with an overlapping store row', s['sites_any_overlap'] == 249, str(s['sites_any_overlap']))
        con.close()


if __name__ == '__main__':
    synthetic()
    real()
    print('FAILED: %s' % FAIL if FAIL else 'all ok')
    sys.exit(1 if FAIL else 0)
