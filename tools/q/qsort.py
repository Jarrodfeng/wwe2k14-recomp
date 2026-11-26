r"""q sort: tool-only sorting of the functions that sit outside every call cluster of 3+ (S111 lane SORT40).

    py tools/q/q.py sort --stats                the count (and bytes) per class over those functions, and how many are LEFT
    py tools/q/q.py sort FUNC                   the class of one function and the evidence (rule / twin / library / name)
    py tools/q/q.py sort --left [--limit N]     the LEFT functions, biggest first: size, callers (what a reading would start from)
    py tools/q/q.py sort --build                rebuild the table now

The set: every function whose unit (units.s1.db, K=32) holds fewer than 3 functions (33,121 of 83,012 at S111). A function of a
bigger unit answers "in unit N (n functions): not in the sorted set". Classes, the FIRST that holds (no model time anywhere):
  DB          has a db/*.json entry (any confidence)
  GAME-NAMED  the game itself names it (names.s2.db, not the shared-string candidates; the rule of qcover.py)
  LIB         its unit is third-party library code (lib.s1.db, tools/q/qlib.py)
  PATTERN     a shape label of pattern.s1.db (tools/q/qpattern.py)
  THUNK       <= THUNK_MAX_INSNS instructions, no loop, and either exactly one tail branch and nothing else, or one indirect branch (bctr)
  LEAF-TRIVIAL  <= TRIVIAL_MAX_INSNS instructions, no call / tail edge, no indirect branch, no loop / switch / compare chain
  CLONE       a byte-identical twin (fnfacts h_exact) that HAS a db entry or a reading (name_read, or a unit_read of its unit).
              Inherits nothing: it names the twin.
  LEFT        none of these. A twin equal only with the immediates ignored (fnfacts h_noimm) is named in the evidence as a hint and the
              function stays LEFT: its constants are its meaning (another field, tag, global; S110 RULES2 review: 30 of 30 read pairs,
              tools/q/qpattern_rules2.py shape_consts; review SORT40: sub_826781D0 bumps +130 where its twin increment_halfword_568 bumps +568).
A twin is any function of the whole index with a db entry or a reading (not only of the sorted set). h_shape is not used (it also ignores call targets).
MAY BE INCOMPLETE: it counts what the index and the listing facts show; a LEFT function is not known to be hard, only unsorted.
Table: out/harness/index/sort.s1.db (rebuilt by the first call after index.db, units, fnfacts, pattern, lib, names or read.s2 changed (size +
mtime) or the CONTENT of db/*.json changed: the table is shared by every checkout and a worktree has other mtimes for the same files). Read-only
over every other store (lib.s1.db is built by qlib when stale); two builders at once each write their own tmp and publish it (qpublish).
Selftest: py -I tools\q\qsort_selftest.py
"""
import collections
import glob
import hashlib
import json
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402
import qpublish  # noqa: E402

SRC = os.path.dirname(os.path.dirname(HERE))
SORT_DB = os.path.join(qnames.IDX, 'sort.s1.db')
MIN_UNIT = 3
THUNK_MAX_INSNS = 6
TRIVIAL_MAX_INSNS = 8
TRIVIAL_ALLOW_LOOPS = False          # the selftest loosens these two to see the planted case fail
CLASSES = ('DB', 'GAME-NAMED', 'LIB', 'PATTERN', 'THUNK', 'LEAF-TRIVIAL', 'CLONE', 'LEFT')
HOW_EXACT, HOW_SHAPE = 'identical text', 'same text, immediates ignored'


class SortError(Exception):
    pass


def classify_one(f):
    """f: dict with db (name|None), game (name|None), lib ((lib, how, evidence)|None), pat ([kind]), n_insns, n_call, n_tail,
    n_bctr (indirect branches, bctr + bctrl), loops, switches, chains, twin ((addr, how, why)|None) -> (class, evidence)"""
    if f.get('db'):
        return 'DB', 'db entry %s' % f['db']
    if f.get('game'):
        return 'GAME-NAMED', 'the game names it %s' % f['game']
    if f.get('lib'):
        return 'LIB', '%s (%s: %s)' % tuple(f['lib'])
    if f.get('pat'):
        return 'PATTERN', 'pattern.s1.db: %s' % ','.join(sorted(set(f['pat'])))
    n, nc, nt, nb = f['n_insns'], f['n_call'], f['n_tail'], f['n_bctr']
    if n <= THUNK_MAX_INSNS and f['loops'] == 0 and nc == 0:
        if nt == 1 and nb == 0:
            return 'THUNK', 'a single tail branch in %d insns' % n
        if nt == 0 and nb == 1:
            return 'THUNK', 'a single indirect branch in %d insns' % n
    if (n <= TRIVIAL_MAX_INSNS and nc == 0 and nt == 0 and nb == 0 and f['switches'] == 0 and f['chains'] == 0
            and (f['loops'] == 0 or TRIVIAL_ALLOW_LOOPS)):
        return 'LEAF-TRIVIAL', '%d insns, no call, no indirect branch, no loop / switch' % n
    if f.get('twin'):
        a, how, why = f['twin']
        if how == HOW_EXACT:
            return 'CLONE', 'twin sub_%08X (%s) %s' % (a, how, why)
        return 'LEFT', ('shape twin sub_%08X (%s) %s: other constants, so its reading names the template, not this function'
                        % (a, how, why))
    return 'LEFT', ''


def pick_twin(cands, self_addr):
    """cands: [(rank, addr, why)] (lower rank = better: db 0, name_read 1, unit_read 2) -> (addr, why) or None, never self"""
    best = None
    for rank, a, why in cands:
        if a != self_addr and (best is None or (rank, a) < (best[0], best[1])):
            best = (rank, a, why)
    return (best[1], best[2]) if best else None


def ro(path):
    return qnames.ro(path)


def idx(name):
    return os.path.join(qnames.IDX, name)


def inputs():
    return [idx(n) for n in ('units.s1.db', 'fnfacts.s1.db', 'pattern.s1.db', 'lib.s1.db', 'names.s2.db', 'read.s2.db')]


def db_fingerprint(dbdir=None):
    """the CONTENT of db/*.json (file names + bytes, line endings made LF), never mtimes: the table is shared by every checkout (qnames.IDX)
    and a worktree checks the same files out with other mtimes (and the main checkout held 2 files LF that a worktree has CRLF), so an mtime
    key rebuilt it on every call that alternated between two checkouts (review SORT40)"""
    h = hashlib.sha1()
    for p in sorted(glob.glob(os.path.join(dbdir or os.path.join(SRC, 'db'), '*.json'))):
        h.update(os.path.basename(p).encode('utf-8') + b'\0')
        try:
            with open(p, 'rb') as f:
                h.update(f.read().replace(b'\r\n', b'\n'))    # git checks the same file out CRLF in one checkout, LF in another
        except OSError:
            h.update(b'-')
        h.update(b'\0')
    return h.hexdigest()[:20]


def fingerprint():
    parts = [qnames.stamp()]
    for p in inputs():
        try:
            p = qnames.newest(p)
            st = os.stat(p)
            parts.append('%d:%d' % (st.st_size, int(st.st_mtime)))
        except OSError:
            parts.append('-')
    parts.append(db_fingerprint())
    return '|'.join(parts)


def load_db_names():
    out = {}
    for p in glob.glob(os.path.join(SRC, 'db', '*.json')):
        try:
            ents = json.load(open(p, encoding='utf-8'))
        except (ValueError, OSError):
            continue
        for e in ents:
            try:
                a = int(e['address'], 16)
            except (KeyError, ValueError, TypeError):
                continue
            out.setdefault(a, e.get('name') or '?')
    return out


def build(path=None, verbose=False):
    t0 = time.time()
    path = path or SORT_DB
    import qlib
    lb = qlib.open_lib()     # first: a stale lib.s1.db is rebuilt here, and the key below reads its mtime
    fp = fingerprint()       # the key of the inputs as read NOW (taken after the build, an input changed meanwhile was never rebuilt for)
    ix, un, fx = ro(qnames.INDEX_DB), ro(idx('units.s1.db')), ro(idx('fnfacts.s1.db'))
    nm, pt, rd = ro(idx('names.s2.db')), ro(idx('pattern.s1.db')), ro(idx('read.s2.db'))
    unit_of = dict(un.execute('select func,unit_id from units'))
    usize = dict(un.execute('select unit_id,n_funcs from unit'))
    universe = set(a for a, u in unit_of.items() if usize.get(u, 0) < MIN_UNIT)
    size = {a: (s or 0) for a, s in ix.execute('select addr,size from funcs')}
    ncall, ntail, callers = collections.Counter(), collections.Counter(), collections.defaultdict(set)
    for s, d, k in ix.execute('select src,dst,kind from edges'):
        if k == 'tail':
            ntail[s] += 1
        elif k == 'call':
            ncall[s] += 1
        callers[d].add(s)
    nbctr = {a: (b or 0) + (c or 0) for a, b, c in ix.execute('select addr,n_bctrl,n_bctr from funcs')}
    fn = {r[0]: r[1:] for r in fx.execute('select func,n_insns,n_loops,n_switch,n_chain,h_exact,h_noimm from fn')}
    dbn = load_db_names()
    game = {}
    for a, n in nm.execute('select func,name from names where source!="fn_string_shared"'):
        game.setdefault(a, n)
    lib = {u: (l, h, e) for u, l, h, e in lb.execute('select unit_id,lib,how,evidence from lib_unit')}
    pat = collections.defaultdict(list)
    for a, k in pt.execute('select func,kind from pattern_fn'):
        pat[a].append(k)
    # functions that HAVE something to inherit: db entry (0), a name_read (1), a unit_read of their unit (2)
    members = collections.defaultdict(list)
    for a, u in unit_of.items():
        members[u].append(a)
    has = {}
    for a, n in dbn.items():
        has[a] = (0, 'db entry %s' % n)
    for a, n, cf in rd.execute('select func,name,confidence from name_read'):
        has.setdefault(a, (1, 'name_read %s (%s)' % (n, cf)))
    for u, purpose, cf in rd.execute('select unit_id,purpose,confidence from unit_read'):
        for a in members.get(u, ()):
            has.setdefault(a, (2, 'unit_read of unit %d: %s (%s)' % (u, re.sub(r'\s+', ' ', purpose or '')[:70], cf)))
    by_hash = {0: collections.defaultdict(list), 1: collections.defaultdict(list)}
    for a, (rank, why) in has.items():
        r = fn.get(a)
        if r:
            by_hash[0][r[4]].append((rank, a, why))
            by_hash[1][r[5]].append((rank, a, why))
    rows = []
    for a in sorted(universe):
        r = fn.get(a)
        f = {'db': dbn.get(a), 'game': game.get(a), 'lib': lib.get(unit_of[a]), 'pat': pat.get(a, []),
             'n_insns': r[0] if r else 999, 'loops': r[1] if r else 0, 'switches': r[2] if r else 0, 'chains': r[3] if r else 0,
             'n_call': ncall[a], 'n_tail': ntail[a], 'n_bctr': nbctr.get(a, 0), 'twin': None}
        if r is None:
            f['n_insns'] = 999
        else:
            for which, how in ((0, HOW_EXACT), (1, HOW_SHAPE)):
                t = pick_twin(by_hash[which].get(r[4 + which], ()), a)
                if t:
                    f['twin'] = (t[0], how, t[1])
                    break
        cls, ev = classify_one(f)
        rows.append((a, size.get(a, 0), cls, ev, len(callers[a] - {a}), f['n_insns']))
    tmp = path + '.tmp%d' % os.getpid()
    if os.path.exists(tmp):
        os.remove(tmp)
    w = sqlite3.connect(tmp)
    w.executescript('create table sort_fn(func integer primary key, size integer, cls text, evidence text, callers integer, n_insns integer);'
                    'create index sf_cls on sort_fn(cls); create table meta(key text primary key, value text);')
    w.executemany('insert into sort_fn values (?,?,?,?,?,?)', rows)
    w.executemany('insert into meta values (?,?)', [('fingerprint', fp), ('n', str(len(rows))), ('built', time.strftime('%Y-%m-%d %H:%M'))])
    w.commit()
    w.close()
    where = qpublish.publish(tmp, path)
    if verbose:
        print('sort.s1.db: %d functions sorted, %.1f s -> %s' % (len(rows), time.time() - t0, where))
    return where


def open_sort(path=None):
    path = path or SORT_DB
    p = qnames.newest(path)
    ok = False
    if os.path.exists(p):
        c = sqlite3.connect('file:%s?mode=ro' % p.replace('\\', '/'), uri=True)
        try:
            ok = dict(c.execute('select key,value from meta')).get('fingerprint') == fingerprint()
        except sqlite3.Error:
            ok = False
        finally:
            c.close()
    if not ok:
        build(path, True)
        p = qnames.newest(path)
    return sqlite3.connect('file:%s?mode=ro' % p.replace('\\', '/'), uri=True)


def parse_func(s):
    m = re.match(r'^(?:sub_)?(?:0x)?([0-9A-Fa-f]{8})$', s.strip())
    if not m:
        raise SortError('FUNC must be sub_XXXXXXXX / 0xXXXXXXXX (got %r)' % s)
    return int(m.group(1), 16)


def stats(con):
    rows = {c: [0, 0] for c in CLASSES}
    for c, n, b in con.execute('select cls,count(*),sum(size) from sort_fn group by cls'):
        rows[c] = [n, b or 0]
    tot = sum(r[0] for r in rows.values())
    print('%-14s %7s %6s %10s' % ('class', 'funcs', '%', 'bytes'))
    for c in CLASSES:
        n, b = rows[c]
        print('%-14s %7d %5.1f%% %10d' % (c, n, 100.0 * n / tot if tot else 0, b))
    print('%-14s %7d' % ('ALL', tot))
    print('LEFT = %d of %d (%.1f%%): py tools\\q\\q.py sort --left' % (rows['LEFT'][0], tot, 100.0 * rows['LEFT'][0] / tot if tot else 0))


def show(con, a):
    r = con.execute('select size,cls,evidence,callers,n_insns from sort_fn where func=?', (a,)).fetchone()
    if r:
        print('sub_%08X: %s  (%d bytes, %d insns, %d callers)%s' % (a, r[1], r[0], r[4], r[3], ('\n  ' + r[2]) if r[2] else ''))
        return 0
    try:
        u = ro(idx('units.s1.db'))
        x = u.execute('select u.unit_id,n.n_funcs from units u join unit n on n.unit_id=u.unit_id where u.func=?', (a,)).fetchone()
    except sqlite3.Error:
        x = None
    if x:
        print('sub_%08X: in unit %d (%d functions): not in the sorted set (only units of < %d functions are)' % (a, x[0], x[1], MIN_UNIT))
    else:
        print('sub_%08X: not a function of the index' % a)
    return 0


def left(con, limit):
    tot = con.execute("select count(*) from sort_fn where cls='LEFT'").fetchone()[0]
    print('%-12s %8s %8s %6s' % ('function', 'bytes', 'callers', 'insns'))
    for a, s, c, n in con.execute("select func,size,callers,n_insns from sort_fn where cls='LEFT' order by size desc,func limit ?", (limit,)):
        print('sub_%08X %8d %8d %6d' % (a, s, c, n))
    if tot > limit:
        print('(%d of %d LEFT functions; the rest: py tools\\q\\q.py sort --left --limit %d)' % (limit, tot, tot))
    else:
        print('(all %d LEFT functions)' % tot)


def main(argv):
    if '--build' in argv:
        build(None, True)
        return 0
    limit = 50
    if '--limit' in argv:
        i = argv.index('--limit')
        try:
            limit = int(argv[i + 1])
        except (IndexError, ValueError):
            raise SortError('--limit needs a number')
        argv = argv[:i] + argv[i + 2:]
    if '--stats' in argv:
        stats(open_sort())
        return 0
    if '--left' in argv:
        left(open_sort(), limit)
        return 0
    pos = [a for a in argv if not a.startswith('--')]
    if len(pos) != 1:
        print(__doc__)
        return 2
    return show(open_sort(), parse_func(pos[0]))


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (SortError, qnames.NamesError) as e:
        print('q: %s' % e, file=sys.stderr)
        sys.exit(2)
