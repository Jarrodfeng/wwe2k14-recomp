r"""q field OFF --stores: the writers `q field` does not model (S105 lane QG).

    py tools/q/q.py field OFF --stores [--global G | --root-name R] [--func F] [--limit N] [--json]

`q field OFF --writes` lists stw / stb / sth / std with a displacement (`stb r4,15229(r3)`). This adds the two forms it never saw:
  indexed stores   stbx / sthx / stwx / stdx / stwbrx / sthbrx / stfsx / stfdx   EA = rA + rB, the effective offset folded when one
                   side is a constant (`li r9,15229; stbx r4,r3,r9` = +15229 on r3's root); rows whose index is a variable
                   (a loop counter) say "var" and match only an exact base (the array starts at OFF) or --var-span N bytes.
  vector / atomic / cache stores  stvx stvx128 (16, EA aligned down) stvewx(128) (4) stvlx stvrx (+128) (up to 16, partial) stfiwx (4)
                   stwcx. (4) stdcx. (8) dcbz (32) dcbz128 (128, aligned down): an indexed row covers OFF when off <= OFF < off + width.
  block writers    calls AND tail calls (`b`) to memcpy sub_82C79000 / memmove sub_82C79DB0 / memset sub_82C79530 (dest r3, size r5),
                   the zero-fill sub_82CEBA60 (dest r3, size r4) and every 1-instruction `b` thunk to them (resolved from the listing at
                   build: e.g. the memcpy thunk sub_82C2F7B8): covers OFF when d <= OFF < d + size.
  add-formed bases `add rD,rA,rB` (one side a constant) followed by a displacement / indexed store through rD: linear shadow, marked
                   `add-formed` (not checked across labels).
Not modelled (the output says so): stmw / stswi / stswx, and stores whose base is not rooted in a0 / g:ADDR / p(..) / r:SITE / a global.
It reuses build_fields.py's register folding (roots a0 / g:ADDR / p(..) / r:SITE / K, deltas); it adds its own small db,
out/harness/index/stores.s1.db, built on first use from generated/default (~25 s, one process per core), keyed on the same
signature as the field index. Every hit is a lead to confirm in the listing (rule 4).
"""
import argparse
import glob
import json
import multiprocessing
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build_fields as bf   # noqa: E402

SCHEMA = 2
# block writers: address -> (name, dest register, size register); 1-instruction `b` thunks to a member are added at build
CALLEES = {0x82C79000: ('memcpy', 3, 5), 0x82C79DB0: ('memmove', 3, 5), 0x82C79530: ('memset', 3, 5), 0x82CEBA60: ('zerofill', 3, 4)}
IDX = {'stbx': 1, 'sthx': 2, 'stwx': 4, 'stdx': 8, 'stwbrx': 4, 'sthbrx': 2, 'stfsx': 4, 'stfdx': 8, 'stbux': 1, 'sthux': 2, 'stwux': 4, 'stdux': 8,
       'stfiwx': 4, 'stwcx': 4, 'stdcx': 8, 'stvx': 16, 'stvx128': 16, 'stvewx': 4, 'stvewx128': 4,
       'stvlx': 16, 'stvlx128': 16, 'stvrx': 16, 'stvrx128': 16}
CACHE = {'dcbz': 32, 'dcbz128': 128}          # two-operand form `dcbz rA,rB`
PARTIAL = ('stvlx', 'stvlx128', 'stvrx', 'stvrx128')
ALIGNED = ('stvx', 'stvx128', 'dcbz', 'dcbz128')
DISP = {'stb': 1, 'sth': 2, 'stw': 4, 'std': 8, 'stfs': 4, 'stfd': 8}
_CALLEES = dict(CALLEES)
_SH = {'func': None, 'regs': {}}


def db_path(gen):
    return os.path.join(bf.data_root(gen), 'out', 'harness', 'index', 'stores.s%d.db' % SCHEMA)


# -------------------------------------------------------------------------------------------------------- build
def _ra(tok):
    return bf.regn(tok)


def _sx(k):
    return k - (1 << 32) if k >= 0x80000000 else k


def _val(st, reg, r):
    """(value, via_shadow): the folded state of rN, or the linear `add` shadow when the state does not know it."""
    if r == 'r0':
        return ('K', 0), 0
    v = st.get(_ra(r))
    if v is not None:
        return v, 0
    sh = _SH['regs'].get(_ra(r))
    return (sh, 1) if sh is not None else (None, 0)


def _fold(A, B):
    """-> [(root, off, var)] rows for EA = A + B (each a folded (root, delta) or None)."""
    ok = lambda x: x is not None and x[0] != 's'
    if A is not None and B is not None:
        if A[0] == 'K' and B[0] == 'K':
            return [('K', (A[1] + B[1]) & bf.M32, 0)]
        for K, X in ((B, A), (A, B)):
            if K[0] == 'K' and X[0] == 's':
                return []
            if K[0] == 'K' and ok(X) and X[0] != 'K':
                if 0x82000000 <= K[1] < 0x90000000:                              # a global array indexed by X (M2): the global is the base
                    return [('K', K[1], 1)]
                return [(X[0], X[1] + _sx(K[1]), 0)]
        return [(x[0], x[1], 1) for x in (A, B) if ok(x)]          # both rooted: either may be the base (M1)
    x = A if A is not None else B
    if x is not None and ok(x):
        return [(x[0], x[1], 1)]
    return []


def collect(func_rows, calls_rows, st, ins, func):
    """Inspect the register state BEFORE one instruction (called from the wrapped step on the emitting pass)."""
    addr, mn, ops, gotos, helper = ins
    if _SH['func'] != func:
        _SH['func'] = func
        _SH['regs'] = {}
    o = [x.strip() for x in ops.split(',')]
    if mn in IDX and len(o) == 3 and o[1] != 'r1' and o[2] != 'r1':
        (A, va), (B, vb) = _val(st, 0, o[1]), _val(st, 0, o[2])
        for root, off, var in _fold(A, B):
            func_rows.append((func, addr, mn, IDX[mn], root, off, var, va | vb))
    elif mn in CACHE and len(o) == 2 and o[0] != 'r1':
        (A, va), (B, vb) = _val(st, 0, o[0]), _val(st, 0, o[1])
        for root, off, var in _fold(A, B):
            func_rows.append((func, addr, mn, CACHE[mn], root, off, var, va | vb))
    elif mn in DISP and len(o) == 2:
        m = re.match(r'^(-?(?:0x)?[0-9A-Fa-f]+)\((r\d+)\)$', o[1])
        if m and m.group(2) != 'r1' and st.get(_ra(m.group(2))) is None and _ra(m.group(2)) in _SH['regs']:
            v = _SH['regs'][_ra(m.group(2))]
            func_rows.append((func, addr, mn, DISP[mn], v[0], v[1] + int(m.group(1), 0), 0, 1))
    elif mn in ('bl', 'b') and not helper:
        try:
            t = int(ops.strip(), 16)
        except ValueError:
            t = None
        if t in _CALLEES:
            nm, dr, sr = _CALLEES[t]
            d = st.get(dr)
            if d is not None and d[0] != 's':
                n = st.get(sr)
                size = n[1] if n is not None and n[0] == 'K' else None
                calls_rows.append((func, addr, nm + (' (tail)' if mn == 'b' else ''), d[0], d[1], size))
    # the linear `add` shadow: rD = rA + rB with one constant side (or a rooted side + unknown) keeps a base for later stores
    if mn == 'add' and len(o) == 3:
        d = _ra(o[0])
        (A, _), (B, _) = _val(st, 0, o[1]), _val(st, 0, o[2])
        new = None
        if A is not None and B is not None and B[0] == 'K' and A[0] != 's' and A[0] != 'K':
            new = (A[0], A[1] + _sx(B[1]))
        elif A is not None and B is not None and A[0] == 'K' and B[0] != 's' and B[0] != 'K':
            new = (B[0], B[1] + _sx(A[1]))
        if d is not None:
            if new is not None:
                _SH['regs'][d] = new
            else:
                _SH['regs'].pop(d, None)
    elif mn.startswith(bf.NODEST):
        pass
    else:
        d = _ra(o[0]) if o else None
        if d is not None:
            _SH['regs'].pop(d, None)
    if mn in ('bl', 'bctrl', 'blrl'):
        for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
            _SH['regs'].pop(r, None)


_ROWS = ([], [])
_ORIG = bf.step


def _wrapped(st, ins, out, func):
    if out is not None:
        collect(_ROWS[0], _ROWS[1], st, ins, func)
    return _ORIG(st, ins, out, func)


THUNK_RE = re.compile(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\) \{\r?\n\tREX_FUNC_PROLOGUE\(\);\r?\n\t// b 0x([0-9a-f]{8})\r?\n\tsub_[0-9A-F]{8}\(ctx, base\);\r?\n\treturn;\r?\n\}')


def _find_thunks(path):
    """{func: target} for every function that is exactly `b TARGET` (one instruction)."""
    text = open(path, encoding='utf-8', errors='replace').read()
    return {int(m.group(1), 16): int(m.group(2), 16) for m in THUNK_RE.finditer(text)}


def _scan_one(args):
    path, callees = args
    _CALLEES.clear()
    _CALLEES.update(callees)
    _SH['func'] = None
    bf.step = _wrapped
    _ROWS[0].clear()
    _ROWS[1].clear()
    text = open(path, encoding='utf-8', errors='replace').read()
    cur, items = None, []

    def flush():
        if cur is not None:
            bf.analyze(cur, items)

    for m in bf.TOKEN_RE.finditer(text):
        c, label, fn, helper, goto = m.groups()
        if fn:
            flush()
            cur = int(fn[len('DEFINE_REX_FUNC(sub_'):-1], 16)
            items = []
        elif cur is not None:
            if label:
                items.append(('l', int(label, 16)))
            elif c is not None:
                items.append(('c', c))
            elif helper:
                items.append(('h',))
            elif goto:
                items.append(('g', int(goto, 16)))
    flush()
    return list(_ROWS[0]), list(_ROWS[1])


def build(gen, image, path):
    files = sorted(glob.glob(os.path.join(gen, 'wwe2k14_recomp.*.cpp')))
    tmp = '%s.tmp%d' % (path, os.getpid())
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(tmp):
        os.remove(tmp)
    con = sqlite3.connect(tmp)
    con.executescript('CREATE TABLE idx(func INT, addr INT, mn TEXT, width INT, root TEXT, off INT, var INT, via INT);'
                      'CREATE TABLE cpy(func INT, addr INT, callee TEXT, root TEXT, off INT, size INT);'
                      'CREATE TABLE meta(k TEXT PRIMARY KEY, v TEXT);')
    jobs = max(1, min(8, (os.cpu_count() or 4) // 2))
    with multiprocessing.Pool(jobs) as pool:
        thunks = {}
        for t in pool.imap_unordered(_find_thunks, files):
            thunks.update(t)
        callees = dict(CALLEES)
        grew = True
        while grew:                                  # thunks to a block writer (and thunks to those) are block writers too
            grew = False
            for f, t in thunks.items():
                if t in callees and f not in callees:
                    callees[f] = callees[t]
                    grew = True
        con.execute('CREATE TABLE IF NOT EXISTS thunks(func INT, target INT)')
        con.executemany('INSERT INTO thunks VALUES (?,?)', [(f, t) for f, t in thunks.items() if f in callees and f not in CALLEES])
        for rows, calls in pool.imap_unordered(_scan_one, [(f, callees) for f in files]):
            con.executemany('INSERT INTO idx VALUES (?,?,?,?,?,?,?,?)', rows)
            con.executemany('INSERT INTO cpy VALUES (?,?,?,?,?,?)', calls)
    con.execute('CREATE INDEX i1 ON idx(off)')
    con.execute('CREATE INDEX i2 ON idx(var, off)')
    con.execute('CREATE INDEX c1 ON cpy(off)')
    con.execute('INSERT INTO meta VALUES (?,?)', ('sig', 'q%d;%s' % (SCHEMA, bf.signature(gen, image))))
    con.commit()
    con.close()
    os.replace(tmp, path)


def _sig_of(path):
    try:
        c = sqlite3.connect('file:%s?mode=ro' % path, uri=True)
        r = c.execute("SELECT v FROM meta WHERE k='sig'").fetchone()
        c.close()
        return r[0] if r else None
    except sqlite3.Error:
        return None


def open_db(gen, image, quiet=False):
    path = db_path(gen)
    sig = 'q%d;%s' % (SCHEMA, bf.signature(gen, image))
    cur = None
    if os.path.exists(path):
        try:
            c = sqlite3.connect('file:%s?mode=ro' % path, uri=True)
            r = c.execute("SELECT v FROM meta WHERE k='sig'").fetchone()
            c.close()
            cur = r[0] if r else None
        except sqlite3.Error:
            cur = None
    if cur != sig:
        import time
        t0 = time.time()
        lk = None
        while lk is None:
            try:
                lk = bf.acquire_lock(path)       # one rebuilder at a time (L6); a second agent waits for the first
            except bf.Busy:
                if time.time() - t0 > 600:
                    raise SystemExit('q field --stores: another process is rebuilding %s; try again' % path)
                time.sleep(2)
                if _sig_of(path) == sig:
                    break
        if lk is not None:
            try:
                if _sig_of(path) != sig:
                    if not quiet:
                        print('q field --stores: building the indexed-store / block-writer index (~30 s, once per codegen) ...', file=sys.stderr)
                    build(gen, image, path)
            finally:
                bf.release_lock(lk)
    return sqlite3.connect('file:%s?mode=ro' % path, uri=True)


# -------------------------------------------------------------------------------------------------------- query
def query(con, off, root=None, func=None, var_span=0, copy_max=4096):
    q = 'SELECT func, addr, mn, width, root, off, var, via FROM idx WHERE '
    args = []
    if var_span:
        q += '((var=0 AND off BETWEEN ? AND ? AND off+width>?) OR (var=1 AND off BETWEEN ? AND ?))'
        args += [off - 127, off, off, off - var_span, off]
    else:
        q += '(off BETWEEN ? AND ? AND off+width>? AND (var=0 OR off=?))'
        args += [off - 127, off, off, off]
    if root:
        q += ' AND root=?'
        args.append(root)
    if func:
        q += ' AND func=?'
        args.append(func)
    idx = con.execute(q + ' ORDER BY func, addr', args).fetchall()
    q2 = 'SELECT func, addr, callee, root, off, size FROM cpy WHERE (off=? OR (size IS NOT NULL AND size<=? AND off<=? AND ?<off+size))'
    a2 = [off, copy_max, off, off]
    if root:
        q2 += ' AND root=?'
        a2.append(root)
    if func:
        q2 += ' AND func=?'
        a2.append(func)
    cpy = con.execute(q2 + ' ORDER BY func, addr', a2).fetchall()
    return idx, cpy


def hidden(con, off, root, func, var_span, copy_max):
    """(M4) counts of what the filters left out: [(count, text)]."""
    def n(q, args):
        if root:
            q += ' AND root=?'
            args = args + [root]
        if func:
            q += ' AND func=?'
            args = args + [func]
        return con.execute(q, args).fetchone()[0]
    out = []
    c = n('SELECT COUNT(*) FROM cpy WHERE size IS NOT NULL AND size>? AND off<=? AND ?<off+size AND off<>?', [copy_max, off, off, off])
    if c:
        out.append((c, 'block writers with size > %d B cover +%d (--all-copies)' % (copy_max, off)))
    c = n('SELECT COUNT(*) FROM cpy WHERE size IS NULL AND off<? AND off>=?', [off, off - 4096])
    if c:
        out.append((c, 'block writers of UNKNOWN size start up to 4096 B below +%d (matched at the exact dest only)' % off))
    if not var_span:
        c = n('SELECT COUNT(*) FROM idx WHERE var=1 AND off<? AND off>=?', [off, off - 4096])
        if c:
            out.append((c, 'variable-index stores from bases up to 4096 B below +%d (--var-span 4096)' % off))
    return out


def run(a, con_q):
    gen, image, _ = bf.default_paths(a.root, a.gen)
    con = open_db(gen, image)
    off = a.off
    root = a.rootname
    if a.glob and not root:
        root = 'g:%08X' % int(a.glob, 0)
    func = bf_func(a.func) if a.func else None
    copy_max = 1 << 30 if a.all_copies else 4096
    idx, cpy = query(con, off, root, func, a.var_span, copy_max)
    names = {}

    def nm(f):
        if f not in names:
            r = con_q.execute('SELECT name FROM funcs WHERE addr=?', (f,)).fetchone() if con_q else None
            names[f] = r[0] if r else 'sub_%08X' % f
        return names[f]
    if a.json:
        print(json.dumps({'indexed_stores': [dict(zip(('func', 'addr', 'mn', 'width', 'root', 'off', 'var', 'via_add'), r)) for r in idx],
                          'block_writers': [dict(zip(('func', 'addr', 'callee', 'root', 'off', 'size'), r)) for r in cpy],
                          'hidden': [{'count': c, 'what': t} for c, t in hidden(con, off, root, func, a.var_span, copy_max)]}, indent=1))
        return 0
    print('-- indexed / vector / cache stores and block writers at +%d%s (the stw / stb / sth displacement writers: q field %d --writes)' % (
        off, (' root ' + root) if root else '', off))
    if not idx and not cpy:
        print('  none')
    last = None
    for f, ad, mn, w, rt, of, var, via in idx[:a.limit]:
        if f != last:
            print('%s' % nm(f))
            last = f
        what = 'global 0x%08X (variable index)' % of if rt == 'K' and var == 1 else 'root %s +%d%s' % (rt, of, '  (variable index from this base)' if var else '')
        extra = ''.join([' (EA aligned down)' if mn in ALIGNED else '', ' (partial: up to %d B)' % w if mn in PARTIAL else '',
                         '  (add-formed base: linear shadow)' if via else ''])
        print('  0x%08X  W w%d %s %s%s' % (ad, w, mn, what, extra))
    if len(idx) > a.limit:
        print('  ... %d more indexed rows (--limit)' % (len(idx) - a.limit))
    for f, ad, cal, rt, of, size in cpy[:a.limit]:
        print('%s  0x%08X  %s(dest = root %s +%d%s)' % (nm(f), ad, cal, rt, of, (', %d bytes' % size) if size is not None else ', size unknown'))
    for c, t in hidden(con, off, root, func, a.var_span, copy_max):
        print('  (+%d hidden: %s)' % (c, t))
    print('  (modelled: scalar / byte-reversed / vector / atomic / dcbz indexed stores, memcpy / memmove / memset / zero-fill and `b` thunks to them, calls and tail calls;'
          ' NOT modelled: stmw / stswi / stswx, bases without a root)')
    return 0


def bf_func(s):
    m = re.match(r'^(?:sub_)?(?:0x)?([0-9A-Fa-f]{8})$', s.strip())
    if not m:
        raise SystemExit('q field --stores: --func must be sub_XXXXXXXX')
    return int(m.group(1), 16)


def main(argv, con_q=None):
    ap = argparse.ArgumentParser(prog='q field OFF --stores')
    ap.add_argument('off', type=lambda s: int(s, 0))
    ap.add_argument('--stores', action='store_true')
    ap.add_argument('--global', dest='glob')
    ap.add_argument('--root-name', dest='rootname')
    ap.add_argument('--func')
    ap.add_argument('--var-span', type=int, default=0, help='also variable-index stores whose base is up to N bytes below OFF')
    ap.add_argument('--all-copies', action='store_true', help='memcpy rows whose dest range covers OFF for any size (default: exact dest or size <= 4096)')
    ap.add_argument('--limit', type=int, default=25)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--root'); ap.add_argument('--gen')
    a, rest = ap.parse_known_args(argv)
    return run(a, con_q)


if __name__ == '__main__':
    multiprocessing.freeze_support()
    sys.exit(main(sys.argv[1:]))
