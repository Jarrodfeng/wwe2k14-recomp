r"""q read verify (S110 lane VERIFYMARK, S111 lane FOLD): a probe CONFIRMED or REFUTED a stored reading; the verdict is stored ON that reading.

    py tools/q/q.py read verify unit UNIT --verdict confirmed|refuted|partly --source PATH[ [C]] --note "TEXT"
    py tools/q/q.py read verify func ADDR --verdict ... --source ... --note ...
    py tools/q/q.py read verify --stats         counts per verdict and per kind

The verdict is four columns of the reading row it judges (verdict, v_note, v_source, v_when): the newest unit_read row of the unit (unit) or the
newest name_read row of the function (func). A unit or function with no stored reading is refused: there is nothing to judge. One verdict per
reading: a later verdict on the same reading replaces the earlier one (the same verdict, note and source again stores nothing). A re-read is a new
row and starts without a verdict; `q sheet` adds [v:..] to the `read:` line and to `read NAME [r:..]` only on the newest reading.
SOUNDNESS: a verdict is a mark in the store. It never writes db/*.json and never turns a reader-proposed name into a db name.
A store made before the verdict columns is read as having no verdicts (qread.add_verdict_cols adds them on the first write).
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
TABLES = {'unit': ('unit_read', 'unit_id'), 'func': ('name_read', 'func')}     # kind -> (reading table, column the target is matched on)
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
    """rows: [(lineno, kind, target, verdict, source, note)] -> (stored, [(lineno, why)]). Good rows are stored, bad ones refused."""
    chk = chk or Checker()
    good, bad = [], []
    for ln, kind, target, verdict, source, note in rows:
        try:
            k, t, sp = check_row(kind, target, verdict, source, note, chk, root)
            good.append((ln, k, t, verdict, note.strip(), sp))
        except ValueError as e:
            bad.append((ln, str(e)))
    stored = 0
    if good:
        c = qread.open_rw(path)
        c.isolation_level = None                 # explicit transaction below
        now = time.strftime('%Y-%m-%d %H:%M:%S')
        try:
            # IMMEDIATE: the write lock before the read of the newest reading, so two writers never both read then both try to upgrade; the
            # whole batch is one transaction, all or nothing
            c.execute('begin immediate')
            for ln, k, t, v, n, s in good:
                tab, col = TABLES[k]
                r = c.execute('select id,verdict,v_note,v_source from %s where %s=? order by id desc limit 1' % (tab, col), (t,)).fetchone()
                if r is None:
                    bad.append((ln, 'no stored reading of %s %s to judge' % (k, t if k == 'unit' else '%08X' % t)))
                    continue
                if (r[1], r[2], r[3]) == (v, n, s):
                    continue
                c.execute('update %s set verdict=?, v_note=?, v_source=?, v_when=? where id=?' % tab, (v, n, s, now, r[0]))
                stored += 1
            c.execute('commit')
        except sqlite3.OperationalError as e:
            if c.in_transaction:
                c.execute('rollback')
            raise qread.ReadError('q read verify: the store is busy or locked (%s); nothing was stored, run it again' % e)
        finally:
            c.close()
    return stored, bad


# ---------------------------------------------------------------- readers (all tolerate a store without the verdict columns)
def _ro(path):
    c = qread.open_ro(path)
    if c is None:
        return None
    try:
        c.execute('select verdict from unit_read limit 1')
        c.execute('select verdict from name_read limit 1')
    except sqlite3.Error:
        c.close()
        return None
    return c


def unit_verdict(uid, path=None):
    """the verdict on the unit's newest reading (None: that reading has none)"""
    c = _ro(path)
    if c is None:
        return None
    try:
        r = c.execute('select verdict from unit_read where unit_id=? order by id desc limit 1', (uid,)).fetchone()
    finally:
        c.close()
    return r[0] if r else None


def func_verdicts(lo, hi, path=None):
    """{func: verdict} for functions between lo and hi, each the verdict on the function's newest proposed name (a function with none is absent)"""
    c = _ro(path)
    if c is None:
        return {}
    try:
        out = {}
        for f, v in c.execute('select func,verdict from name_read where func between ? and ? order by id', (lo, hi)):
            out[f] = v
        return {f: v for f, v in out.items() if v}
    finally:
        c.close()


def tag(v):
    return ' [v:%s]' % v if v else ''


def show_lines(uid, path=None, unit_of=None):
    """VERIFIED lines of the unit's readings and of its functions' names, newest first; the ones on an earlier reading say so"""
    c = _ro(path)
    if c is None:
        return []
    try:
        ucur = c.execute('select max(id) from unit_read where unit_id=?', (uid,)).fetchone()[0]
        rows = [(w, 'unit', 0, v, n, s, i != ucur) for i, v, n, s, w in
                c.execute('select id,verdict,v_note,v_source,v_when from unit_read where unit_id=? and verdict is not null', (uid,))]
        ncur = dict(c.execute('select func,max(id) from name_read group by func'))
        rows += [(w, 'func', f, v, n, s, i != ncur.get(f)) for i, f, v, n, s, w in
                 c.execute('select id,func,verdict,v_note,v_source,v_when from name_read where unit_id=? and verdict is not null', (uid,))]
    finally:
        c.close()
    rows.sort(key=lambda r: r[0] or '', reverse=True)
    return ['  VERIFIED %s: %s%s (%s, %s)%s' % (v, '' if k == 'unit' else 'sub_%08X: ' % f, n, s, w, EARLIER if old else '')
            for w, k, f, v, n, s, old in rows]


def stats_lines(path=None):
    c = _ro(path)
    if c is None:
        return ['verdicts: none stored']
    try:
        rows = [('unit', v, n) for v, n in c.execute('select verdict,count(*) from unit_read where verdict is not null group by 1')]
        rows += [('func', v, n) for v, n in c.execute('select verdict,count(*) from name_read where verdict is not null group by 1')]
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
        if x in ('--verdict', '--source', '--note'):
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
    if len(pos) != 2:
        raise qread.ReadError('q read verify unit UNIT|func ADDR --verdict confirmed|refuted|partly --source PATH --note "TEXT"  |  --stats')
    n, bad = add_rows([(0, pos[0], pos[1], opt('--verdict', ''), opt('--source', ''), opt('--note', ''))], path, chk, root)
    if bad:
        raise qread.ReadError('refused: %s' % bad[0][1])
    print('stored %d %s verdict%s' % (n, pos[0], '' if n else ' (the same verdict, note and source are already stored)'))
    return 0
