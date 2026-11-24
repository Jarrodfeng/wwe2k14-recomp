r"""q name: the names the game itself carries for functions (S110 lane NAMES).

    py tools/q/q.py name ADDR|WORD [--json]

ADDR (0x826D44A0, sub_826D44A0, a bare 8-digit hex) = every name the game holds for that function.
WORD = the functions whose name contains WORD (case-insensitive), one line per name.

Two sources, both read from the player's image (out/harness/index/image.bin), written only under out/ (rule 1):
  lua_binding  the Lua binding tables {name pointer, function} ending in a null name, found by decoding r4 / r5 before each
               call of the register function sub_82C4F258 (51 tables, name = Library.Function), plus the 11 globals pushed
               one by one in sub_826DE820 (pushcclosure sub_82C4ABD8 + setfield sub_82C4B490; name = the bare global).
  name_table   a function pointer in data with a string pointer at +-4 (index `ptrs`; the exception table is excluded by
               the .pdata rule of COVER.md). The name is the string. Mostly script or state-name tables.
Two more (S110 FNSTR, tools/q/qfnstr.py): fn_string = the only function referencing a signature string such as
`void __cdecl CStage::TextureSetup(...)` (a name); fn_string_shared = 2..4 references, or a function holding two different
signatures: a CANDIDATE only, never printed as the function's name by q sheet / q unit / q cover.
MAY BE INCOMPLETE: the Lua libraries `io` and `package` use another registration form and are not walked; a name table whose
string is not beside the pointer (an index column, a separate name array) is not found; a function that carries no name is
not in the answer, which says nothing about it.

The table lives in out/harness/index/names.s1.db (own file, rebuilt when index.db changes or with --rebuild).
"""
import os
import re
import sqlite3
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
IDX = os.path.join(MAIN, 'out', 'harness', 'index')
INDEX_DB = os.path.join(IDX, 'index.db')
IMAGE = os.path.join(IDX, 'image.bin')
NAMES_DB = os.path.join(IDX, 'names.s2.db')     # s2: + fn_string / fn_string_shared (qfnstr.py)
BASE = 0x82000000
REGISTER = 0x82C4F258
PUSHCLOSURE = 0x82C4ABD8
SETFIELD = 0x82C4B490
GLOBALS_FN = 0x826DE820
MASK = 0xFFFFFFFF
INCOMPLETE = ('may be incomplete: Lua libraries io / package (another registration form) are not walked; '
              'name tables whose string is not beside the function pointer are not found.')


class NamesError(Exception):
    pass


def ro(path):
    if not os.path.exists(path):
        raise NamesError('no index at %s: build it with py tools\\q\\build_index.py' % path)
    return sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)


class Image:
    def __init__(self, path=IMAGE):
        try:
            self.d = open(path, 'rb').read()
        except OSError as e:
            raise NamesError('cannot read the game image %s (%s)' % (path, e.strerror or e))

    def word(self, a):
        o = a - BASE
        if o < 0 or o + 4 > len(self.d):
            return None
        return struct.unpack_from('>I', self.d, o)[0]

    def cstr(self, a):
        o = a - BASE
        if o < 0 or o >= len(self.d):
            return None
        e = self.d.find(b'\x00', o, o + 200)
        if e < 0:
            return None
        s = self.d[o:e]
        if not s or any(b < 32 or b > 126 for b in s):
            return None
        return s.decode('ascii')


def track(img, lo, hi, on_call):
    """Walk lo..hi linearly, tracking lis / addi / ori / li / or into registers; on each `bl` call on_call(site, target, regs)
    and forget the volatile registers (r3..r12) afterwards."""
    regs = {}
    for a in range(lo, hi, 4):
        ins = img.word(a)
        if ins is None:
            break
        op = ins >> 26
        rd = (ins >> 21) & 31
        ra = (ins >> 16) & 31
        imm = ins & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 15 and ra == 0:
            regs[rd] = (imm << 16) & MASK
        elif op == 14:
            if ra == 0:
                regs[rd] = simm & MASK
            elif ra in regs:
                regs[rd] = (regs[ra] + simm) & MASK
            else:
                regs.pop(rd, None)
        elif op == 24:                      # ori ra, rs, imm  (rs = bits 21-25, dest = bits 16-20)
            if rd in regs:
                regs[ra] = regs[rd] | imm
            else:
                regs.pop(ra, None)
        elif op == 31 and ((ins >> 1) & 0x3FF) == 444 and not ins & 1:   # or ra, rs, rb (mr)
            rb = (ins >> 11) & 31
            if rd in regs and rb == rd:
                regs[ra] = regs[rd]
            else:
                regs.pop(ra, None)
        elif op == 18 and (ins & 3) == 1:   # bl
            off = ins & 0x03FFFFFC
            if off & 0x02000000:
                off -= 0x04000000
            on_call(a, (a + off) & MASK, dict(regs))
            for r in range(3, 13):
                regs.pop(r, None)


def lua_tables(img, con):
    """[(lib, table_addr, [(name, fn, row_addr)])], one entry per distinct table, from the register call sites."""
    sites = [r[0] for r in con.execute('select site from edges where dst=? and kind in (?,?) order by site', (REGISTER, 'call', 'tail'))]
    seen, out = set(), []
    for site in sites:
        # the 16 instructions before the call: r4 = library name, r5 = the table
        r = _regs_at(img, site - 64, site)
        r4, r5 = r.get(4), r.get(5)
        lib = img.cstr(r4) if r4 else None
        if r5 is None or r5 in seen:
            continue
        seen.add(r5)
        rows, a = [], r5
        while True:
            nm, fn = img.word(a), img.word(a + 4)
            if not nm:
                break
            s = img.cstr(nm)
            if s is None or fn is None or not (0x82000000 <= fn < 0x84000000):
                rows.append(('?bad', fn, a))      # counted as the walker of DRIVE.md counted it; not written as a name
                break
            rows.append((s, fn, a))
            a += 8
        out.append((lib, r5, rows))
    return out


def _regs_at(img, lo, hi):
    """register values (from lis/addi/ori/li/mr) just before address hi, scanning from lo."""
    regs = {}
    for a in range(lo, hi, 4):
        ins = img.word(a)
        op = ins >> 26
        rd = (ins >> 21) & 31
        ra = (ins >> 16) & 31
        imm = ins & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 15 and ra == 0:
            regs[rd] = (imm << 16) & MASK
        elif op == 14 and ra in regs:
            regs[rd] = (regs[ra] + simm) & MASK
        elif op == 24 and rd in regs:
            regs[ra] = regs[rd] | imm
    return regs


def lua_globals(img, con):
    """[(name, fn)] pushed one by one in sub_826DE820: pushcclosure(fn) then setfield(-10002, name)."""
    row = con.execute('select addr, size from funcs where addr=?', (GLOBALS_FN,)).fetchone()
    if not row:
        return []
    last = {}
    out = []

    def on_call(site, t, regs):
        if t == PUSHCLOSURE:
            last['fn'] = regs.get(4)
        elif t == SETFIELD and 'fn' in last and regs.get(4) == (-10002 & MASK):
            nm = img.cstr(regs.get(5) or 0)
            if nm:
                out.append((nm, last['fn']))
            last.clear()
    track(img, row[0], row[0] + row[1], on_call)
    return out


def name_table(img, con, funcs):
    """[(at, fn, name)] every function pointer in data with a string pointer at +-4 (pdata excluded)."""
    code_lo, code_hi = __import__('json').loads(con.execute("select value from meta where key='code_range'").fetchone()[0])
    P = {at: v for at, v in con.execute("select at,value from ptrs where kind='func'")}
    SP = {at: v for at, v in con.execute("select at,value from ptrs where kind='str'")}
    strs = dict(con.execute('select addr,text from strings'))
    out = []
    for at, v in sorted(P.items()):
        if v not in funcs:
            continue
        run = (at - 4) in P or (at + 4) in P
        if at < code_lo and not run and (((at + 8) in P and P[at + 8] > v) or ((at - 8) in P and P[at - 8] < v)) and at >= 0x821C0000:
            continue                                  # exception table (.pdata), COVER.md
        s = SP.get(at - 4)
        if s is None:
            s = SP.get(at + 4)
        if s is None:
            continue
        t = img.cstr(s) or strs.get(s)
        if not t or not t.strip():
            continue
        out.append((at, v, t.strip()[:120]))
    return out


def stamp():
    st = os.stat(INDEX_DB)
    return '%d:%d' % (st.st_size, int(st.st_mtime))


def build(verbose=False):
    t0 = time.time()
    con = ro(INDEX_DB)
    img = Image()
    funcs = set(r[0] for r in con.execute('select addr from funcs where has_body=1'))
    tabs = lua_tables(img, con)
    glob = lua_globals(img, con)
    nt = name_table(img, con, funcs)
    rows, pairs = [], set()
    nrows = 0
    for lib, base, trows in tabs:
        nrows += len(trows)
        for nm, fn, a in trows:
            if nm == '?bad':
                continue
            rows.append((fn, '%s.%s' % (lib, nm) if lib else nm, 'lua_binding', base))
            pairs.add(a + 4)
    for nm, fn in glob:
        rows.append((fn, nm, 'lua_binding', GLOBALS_FN))
    nlua = len(rows)
    for at, fn, t in nt:
        if at not in pairs:
            rows.append((fn, t, 'name_table', at))
    fs_rows = __import__('qfnstr').rows(con)     # S110 FNSTR: signature strings (fn_string = a name, fn_string_shared = candidate)
    rows.extend(fs_rows)
    tmp = NAMES_DB + '.tmp%d' % os.getpid()
    if os.path.exists(tmp):
        os.remove(tmp)
    w = sqlite3.connect(tmp)
    w.executescript('create table names(func integer, name text, source text, table_addr integer);'
                    'create index names_func on names(func); create table meta(key text primary key, value text);')
    w.executemany('insert into names values (?,?,?,?)', rows)
    dt = time.time() - t0
    meta = {'index_stamp': stamp(), 'lua_tables': str(len(tabs)), 'lua_rows': str(nrows), 'lua_globals': str(len(glob)),
            'lua_names': str(nlua), 'name_table_names': str(len(rows) - nlua - len(fs_rows)), 'fn_string_rows': str(len(fs_rows)), 'build_s': '%.1f' % dt}
    w.executemany('insert into meta values (?,?)', list(meta.items()))
    w.commit()
    w.close()
    os.replace(tmp, NAMES_DB)
    if verbose:
        print('names.s2.db: %d lua tables, %d rows, %d globals, %d lua names, %d name_table names, %d fn_string rows, %.1f s'
              % (len(tabs), nrows, len(glob), nlua, len(rows) - nlua - len(fs_rows), len(fs_rows), dt))
    return meta


def open_names(rebuild=False):
    if rebuild or not os.path.exists(NAMES_DB):
        build(True)
    else:
        c = sqlite3.connect('file:%s?mode=ro' % NAMES_DB.replace('\\', '/'), uri=True)
        try:
            ok = dict(c.execute('select key,value from meta')).get('index_stamp') == stamp()
        finally:
            c.close()
        if not ok:
            build(True)
    return sqlite3.connect('file:%s?mode=ro' % NAMES_DB.replace('\\', '/'), uri=True)


def parse_addr(s):
    m = re.fullmatch(r'(?:sub_|0x)?([0-9A-Fa-f]{8})', s.strip())
    return int(m.group(1), 16) if m else None


def owner(con, a):
    r = con.execute('select addr from funcs where addr<=? and has_body=1 order by addr desc limit 1', (a,)).fetchone()
    return r[0] if r else None


def main(argv):
    import json
    js = '--json' in argv
    rebuild = '--rebuild' in argv
    pos = [x for x in argv if not x.startswith('--')]
    if not pos and not rebuild:
        print(__doc__)
        return 2
    n = open_names(rebuild)
    if not pos:
        return 0
    q = pos[0]
    a = parse_addr(q)
    res = {'query': q, 'rows': [], 'incomplete': INCOMPLETE}
    if a is not None:
        ix = ro(INDEX_DB)
        o = owner(ix, a)
        if o is None:
            raise NamesError('%s is not inside a function of the index' % q)
        res['func'] = o
        res['inside'] = o != a
        res['rows'] = [dict(func=f, name=nm, source=s, table_addr=t) for f, nm, s, t in
                       n.execute('select func,name,source,table_addr from names where func=? order by source,name', (o,))]
    else:
        res['rows'] = [dict(func=f, name=nm, source=s, table_addr=t) for f, nm, s, t in
                       n.execute("select func,name,source,table_addr from names where name like ? escape '\\' order by func,name limit 200",
                                 ('%' + q.replace('\\', '\\\\').replace('%', '\\%').replace('_', '\\_') + '%',))]
    if js:
        print(json.dumps(res, indent=1))
        return 0
    if a is not None:
        print('sub_%08X%s: %d game-carried name(s) or candidate(s)' % (res['func'], ' (contains %s)' % q if res['inside'] else '', len(res['rows'])))
        if not res['rows']:
            print('  none: no Lua binding or name table points at it (says nothing about the function).')
    else:
        print('%d name(s) containing %r%s' % (len(res['rows']), q, ' (first 200)' if len(res['rows']) == 200 else ''))
    for r in res['rows']:
        sg = r['source'] in ('fn_string', 'fn_string_shared')
        print('  sub_%08X  %-40s %-16s %s 0x%08X%s' % (r['func'], r['name'], r['source'], 'string' if sg else 'table', r['table_addr'],
                                                       '  candidate (shared string, not its name)' if r['source'] == 'fn_string_shared' else ''))
    print(INCOMPLETE)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except NamesError as e:
        print('q: %s' % e, file=sys.stderr)
        sys.exit(2)
