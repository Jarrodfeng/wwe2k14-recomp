r"""q read verify (S110 lane VERIFYMARK): a probe CONFIRMED or REFUTED a stored reading; the mark goes into the reading store.

    py tools/q/q.py read verify unit UNIT --verdict confirmed|refuted|partly --source PATH[ [C]] --note "TEXT"
    py tools/q/q.py read verify func ADDR --verdict ... --source ... --note ...
    py tools/q/q.py read verify --file TSV      rows: kind <TAB> target <TAB> verdict <TAB> source <TAB> note   (bad rows reported with line number, skipped)
    py tools/q/q.py read verify --stats         counts per verdict and per kind

Table verify_read(id, target_kind, target, verdict, note, source, when_, read_id) in out/harness/index/read.s2.db, created on first use; rows
are only appended (a row equal to a stored one is not stored again). read_id pins the verdict to the reading that was newest when it was stored.
`q read show` prints every verdict after the answers (`(on an earlier reading)` when a newer reading came since); `q sheet` adds [v:..] to the
`read:` line and to `read NAME [r:..]` only while that reading is the newest, and to the line of a function with no read name.
SOUNDNESS: a verdict is a mark in the store. It never writes db/*.json and never turns a reader-proposed name into a db name.
A store without the table keeps working (every reader here returns nothing then).
"""
import collections
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402
import qread  # noqa: E402
import qunits  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
VERDICTS = ('confirmed', 'refuted', 'partly')
KINDS = ('unit', 'func')
# read_id: the reading the verdict is about, the newest unit_read id of the unit (unit) or name_read id of the function (func) when it was
# stored; NULL: there was none. A tag shows only while that reading is still the newest one (a re-read is not carried a verdict it never had).
TABLE = ('create table if not exists verify_read(id integer primary key autoincrement, target_kind text, target integer, verdict text,'
         ' note text, source text, when_ text, read_id integer);'
         'create index if not exists verify_read_t on verify_read(target_kind, target);')
EARLIER = ' (on an earlier reading)'


class Checker:
    """what the index knows: unit ids and function -> unit. Injectable for tests."""

    def __init__(self, units=None, index=None):
        self._uc, self._ix = units, index

    def uc(self):
        if self._uc is None:
            self._uc = qunits.open_units()
        return self._uc

    def unit_exists(self, uid):
        return self.uc().execute('select 1 from unit where unit_id=?', (uid,)).fetchone() is not None

    def func_start(self, a):
        """-> (function start, unit id) or None when the address is in no known function of a unit"""
        r = self.uc().execute('select unit_id from units where func=?', (a,)).fetchone()
        if r:
            return a, r[0]
        if self._ix is None:
            self._ix = qnames.ro(qnames.INDEX_DB)
        # the nearest body start below, and only while the address is inside it (qnames.owner has no end: an address in the gap after a
        # function, or past the image, would land on the function before it)
        fs = self._ix.execute('select addr,size from funcs where addr<=? and has_body=1 order by addr desc limit 1', (a,)).fetchone()
        if not fs or a >= fs[0] + (fs[1] or 0):
            return None
        r = self.uc().execute('select unit_id from units where func=?', (fs[0],)).fetchone()
        return (fs[0], r[0]) if r else None


def source_path(source, root=None):
    """`docs/x.md [C]` -> the source as stored: the path made repo-relative ('/' separators) plus its mark, or None. The path must be a
    file inside the repo (relative to the repo root, or absolute): a scratch-folder or a worktree path would dangle once it is cleaned up."""
    s = (source or '').strip()
    m = re.search(r'\s*(\[[^\]]*\])\s*$', s)
    p, mark = (s[:m.start()], ' ' + m.group(1)) if m else (s, '')
    root = os.path.abspath(root or ROOT)
    if not p:
        return None
    full = os.path.abspath(p if os.path.isabs(p) else os.path.join(root, p))
    try:
        rel = os.path.relpath(full, root)
    except ValueError:                            # another drive
        return None
    rel = rel.replace('\\', '/')
    if rel.startswith('../') or rel == '..' or rel.split('/')[0] == '.claude' or not os.path.isfile(full):
        return None
    return rel + mark


def check_row(kind, target, verdict, source, note, chk, root=None):
    """-> (kind, int target, source as stored) or raises ValueError with the reason"""
    if kind not in KINDS:
        raise ValueError('kind must be unit or func, not %r' % kind)
    if verdict not in VERDICTS:
        raise ValueError('verdict must be confirmed|refuted|partly, not %r' % verdict)
    if not (note or '').strip():
        raise ValueError('empty note')
    if '\n' in note or '\r' in note:
        raise ValueError('the note has a line break (a verdict note is one line)')
    sp = source_path(source, root)
    if sp is None:
        raise ValueError('source is not a file inside the repo (not under .claude/): %r' % source)
    t = (target or '').strip()
    if kind == 'unit':
        if not re.fullmatch(r'\d{1,6}', t):
            raise ValueError('unit target must be a decimal unit id, not %r' % t)
        if not chk.unit_exists(int(t)):
            raise ValueError('unknown unit %s' % t)
        return kind, int(t), sp
    a = qnames.parse_addr(t)
    if a is None:
        raise ValueError('func target must be an 8-hex address, not %r' % t)
    r = chk.func_start(a)
    if r is None:
        raise ValueError('%s is in no known function' % t)
    return kind, r[0], sp


def add_rows(rows, path=None, chk=None, root=None):
    """rows: [(lineno, kind, target, verdict, source, note)] -> (stored, [(lineno, why)]). Good rows are stored, bad ones skipped."""
    chk = chk or Checker()
    good, bad = [], []
    for ln, kind, target, verdict, source, note in rows:
        try:
            k, t, sp = check_row(kind, target, verdict, source, note, chk, root)
            good.append((k, t, verdict, note.strip(), sp))
        except ValueError as e:
            bad.append((ln, str(e)))
    stored = 0
    if good:
        c = qread.open_rw(path)
        c.executescript(TABLE)
        if _rid_col(c) == 'NULL':                # a table made before read_id
            try:
                c.execute('alter table verify_read add column read_id integer')
            except sqlite3.OperationalError:
                pass                             # another writer added it just now (a lock error shows at the insert)
        c.isolation_level = None                 # explicit transaction below
        now = time.strftime('%Y-%m-%d %H:%M:%S')
        try:
            # IMMEDIATE: the write lock before the duplicate check, so two writers never both read then both try to upgrade (that fails at
            # once, without the busy wait); the whole batch is one transaction, all or nothing
            c.execute('begin immediate')
            for k, t, v, n, s in good:
                # a TSV run again after its SKIPPED lines were fixed: rows already stored are not stored twice
                if c.execute('select 1 from verify_read where target_kind=? and target=? and verdict=? and note=? and source=? limit 1',
                             (k, t, v, n, s)).fetchone():
                    continue
                rid = c.execute('select max(id) from unit_read where unit_id=?' if k == 'unit' else 'select max(id) from name_read where func=?',
                                (t,)).fetchone()[0]
                c.execute('insert into verify_read(target_kind,target,verdict,note,source,when_,read_id) values (?,?,?,?,?,?,?)',
                          (k, t, v, n, s, now, rid))
                stored += 1
            c.execute('commit')
        except sqlite3.OperationalError as e:
            if c.in_transaction:
                c.execute('rollback')
            raise qread.ReadError('q read verify: the store is busy or locked (%s); nothing was stored, run it again' % e)
        finally:
            c.close()
    return stored, bad


def parse_tsv(text):
    rows = []
    for i, line in enumerate(text.split('\n'), 1):      # not splitlines(): it also cuts at \f, \x1c-\x1e, U+0085, U+2028 inside a note
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        f = line.rstrip('\r\n').split('\t', 4)
        f += [''] * (5 - len(f))
        rows.append((i, f[0].strip(), f[1].strip(), f[2].strip(), f[3].strip(), f[4]))
    return rows


# ---------------------------------------------------------------- readers (all tolerate a store without the table)
def _ro(path):
    c = qread.open_ro(path)
    if c is None:
        return None
    try:
        c.execute('select 1 from verify_read limit 1')
    except sqlite3.Error:
        c.close()
        return None
    return c


def _rid_col(c):
    """'read_id', or 'NULL' for a table made before the column (its rows count as on no reading)"""
    return 'read_id' if any(r[1] == 'read_id' for r in c.execute('pragma table_info(verify_read)')) else 'NULL'


def unit_verdict(uid, path=None):
    """newest verdict of the unit itself on its newest reading (None: no verdict on that reading)"""
    c = _ro(path)
    if c is None:
        return None
    try:
        cur = c.execute('select max(id) from unit_read where unit_id=?', (uid,)).fetchone()[0]
        r = c.execute("select verdict from verify_read where target_kind='unit' and target=? and %s is ? order by id desc limit 1" % _rid_col(c),
                      (uid, cur)).fetchone()
    finally:
        c.close()
    return r[0] if r else None


def func_verdicts(lo, hi, path=None):
    """{func: newest verdict} for functions between lo and hi, each on the function's newest proposed name (or on none, when it has none)"""
    c = _ro(path)
    if c is None:
        return {}
    try:
        cur = dict(c.execute('select func,max(id) from name_read where func between ? and ? group by func', (lo, hi)))
        out = {}
        for f, v, r in c.execute("select target,verdict,%s from verify_read where target_kind='func' and target between ? and ? order by id" % _rid_col(c),
                                 (lo, hi)):
            if r == cur.get(f):
                out[f] = v
        return out
    finally:
        c.close()


def tag(v):
    return ' [v:%s]' % v if v else ''


def show_lines(uid, path=None, unit_of=None):
    """VERIFIED lines of the unit and of its functions, newest first; every verdict, the ones on an earlier reading say so"""
    c = _ro(path)
    if c is None:
        return []
    try:
        rc = _rid_col(c)
        ucur = c.execute('select max(id) from unit_read where unit_id=?', (uid,)).fetchone()[0]
        rows = [(i, 'unit', t, v, n, s, w, r != ucur) for i, t, v, n, s, w, r in
                c.execute("select id,target,verdict,note,source,when_,%s from verify_read where target_kind='unit' and target=?" % rc, (uid,))]
        fr = c.execute("select id,target,verdict,note,source,when_,%s from verify_read where target_kind='func'" % rc).fetchall()
        ncur = dict(c.execute('select func,max(id) from name_read group by func')) if fr else {}
    finally:
        c.close()
    if fr:
        unit_of = unit_of or _unit_of_default()
        rows += [(i, 'func', t, v, n, s, w, r != ncur.get(t)) for i, t, v, n, s, w, r in fr if unit_of(t) == uid]
    rows.sort(key=lambda r: -r[0])
    return ['  VERIFIED %s: %s%s (%s, %s)%s' % (v, '' if k == 'unit' else 'sub_%08X: ' % t, n, s, w, EARLIER if old else '')
            for _i, k, t, v, n, s, w, old in rows]


def _unit_of_default():
    uc = qunits.open_units()
    return lambda f: (lambda r: r[0] if r else None)(uc.execute('select unit_id from units where func=?', (f,)).fetchone())


def stats_lines(path=None):
    c = _ro(path)
    if c is None:
        return ['verdicts: none stored']
    try:
        rows = c.execute('select target_kind,verdict,count(*) from verify_read group by 1,2').fetchall()
    finally:
        c.close()
    bv, bk = collections.Counter(), collections.Counter()
    for k, v, n in rows:
        bv[v] += n
        bk[k] += n
    return ['verdicts: %d (%s)' % (sum(bv.values()), ' '.join('%s %d' % (v, bv.get(v, 0)) for v in VERDICTS)),
            'by kind: %s' % ' '.join('%s %d' % (k, bk.get(k, 0)) for k in KINDS)]


def main(rest, path=None, chk=None, root=None):
    # one pass: the word after a valued option is its value even when it begins with -- (`--note "--stats is wrong"`)
    opts, pos, i = {}, [], 0
    while i < len(rest):
        x = rest[i]
        if x in ('--verdict', '--source', '--note', '--file'):
            if i + 1 >= len(rest):
                raise qread.ReadError('q read verify: %s needs a value' % x)
            opts[x] = rest[i + 1]
            i += 2
        elif x == '--stats':
            opts[x] = True
            i += 1
        elif x.startswith('--'):
            raise qread.ReadError('q read verify: unknown option %s' % x)
        else:
            pos.append(x)
            i += 1

    def opt(flag, default=None):
        return opts.get(flag, default)
    if '--stats' in opts:
        for x in stats_lines(path):
            print(x)
        return 0
    if '--file' in opts:
        fn = opt('--file')
        if not fn or not os.path.isfile(fn):
            raise qread.ReadError('q read verify --file TSV: file not found: %r' % fn)
        with open(fn, encoding='utf-8-sig', errors='replace') as fh:
            rows = parse_tsv(fh.read())
        n, bad = add_rows(rows, path, chk, root)
        for ln, why in bad:
            print('  line %d SKIPPED: %s' % (ln, why))
        print('stored %d, skipped %d' % (n, len(bad)))
        return 0 if not bad else 1
    if len(pos) != 2:
        raise qread.ReadError('q read verify unit UNIT|func ADDR --verdict confirmed|refuted|partly --source PATH --note "TEXT"  |  --file TSV  |  --stats')
    n, bad = add_rows([(0, pos[0], pos[1], opt('--verdict', ''), opt('--source', ''), opt('--note', ''))], path, chk, root)
    if bad:
        raise qread.ReadError('refused: %s' % bad[0][1])
    print('stored %d %s verdict%s' % (n, pos[0], '' if n else ' (the same verdict, note and source are already stored)'))
    return 0
