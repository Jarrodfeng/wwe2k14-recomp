r"""Selftest of tools/q/qfeedkey.py (S110 lane FEEDKEY): synthetic source + a synthetic field index, one base per route resolves,
an ambiguous base stays unresolved; then (when the real index exists) the real table's invariants. Run: py -I tools\q\qfeedkey_selftest.py"""
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qfeed      # noqa: E402
import qfeedkey   # noqa: E402

FAILS = []


def check(name, cond, detail=''):
    print('%s %s%s' % ('ok  ' if cond else 'FAIL', name, (' -- ' + detail) if (detail and not cond) else ''))
    if not cond:
        FAILS.append(name)


def synthetic_db():
    c = sqlite3.connect(':memory:')
    c.executescript('''
        create table roots(id integer primary key, text text unique, glob integer);
        create table rows(func integer, addr integer, rw text, width integer, fl integer, off integer, root integer);
        create table objtype(func integer, root integer, vt integer, how text, primary key(func, root, vt));
        create table ctors(func integer primary key, vt integer);
        create table argflow(func integer, argroot integer, groot integer, n integer);
    ''')
    for i, t in enumerate(['a0', 'a1', 'g:83930050', 'p(g:83930050,8)', 'a2']):
        c.execute('insert into roots values(?,?,0)', (i + 1, t))
    # hook sub_82500000 a0 = vtable 0x82010000 (one type); a function writes +16 of that type
    c.execute('insert into objtype values(?,?,?,?)', (0x82500000, 1, 0x82010000, 'slot'))
    c.execute('insert into objtype values(?,?,?,?)', (0x82511111, 1, 0x82010000, 'slot'))
    c.execute('insert into rows values(?,?,?,?,?,?,?)', (0x82511111, 0x82511120, 'W', 4, 0, 16, 1))
    # hook sub_82500100 a0 has TWO types: ambiguous
    c.execute('insert into objtype values(?,?,?,?)', (0x82500100, 1, 0x82010000, 'slot'))
    c.execute('insert into objtype values(?,?,?,?)', (0x82500100, 1, 0x82010004, 'slot'))
    # global g:83930050: a writer at +8; the pointer at +8 of it is p(g:83930050,8) with a writer at +4
    c.execute('insert into rows values(?,?,?,?,?,?,?)', (0x82522222, 0x82522230, 'W', 4, 0, 8, 3))
    c.execute('insert into rows values(?,?,?,?,?,?,?)', (0x82533333, 0x82533340, 'W', 4, 0, 4, 4))
    # constructor sub_82600000 stores vtable 0x82020000; a typed function writes +4
    c.execute('insert into ctors values(?,?)', (0x82600000, 0x82020000))
    c.execute('insert into objtype values(?,?,?,?)', (0x82544444, 5, 0x82020000, 'slot'))
    c.execute('insert into rows values(?,?,?,?,?,?,?)', (0x82544444, 0x82544450, 'W', 4, 0, 4, 5))
    return c


SRC = '''\
REX_HOOK_RAW(sub_82500000) {
  uint32_t obj = ctx.r3.u32;
  use(obj + 16);
}
REX_HOOK_RAW(sub_82500100) {
  uint32_t two = ctx.r3.u32;
  use(two + 16);
}
inline void ReadG(M& mem) {
  const uint32_t mgr = mem.u32(kG);
  use(mgr + 8);
  const uint32_t sub = mem.u32(mgr + 8);
  use(sub + 4);
}
inline void Mixed(M& mem) {
  uint32_t m = mem.u32(kG);
  m = mem.u32(kH);
  use(m + 8);
}
inline void Param(uint32_t held) {
  use(held + 8);
}
inline void Built(M& mem) {
  uint32_t o = Find(mem, 3);   // constructor sub_82600000 allocates it
  use(o + 4);
}
inline void Walk(M& mem) {
  uint32_t w = mem.u32(kG);
  w += 8;
  use(w + 4);
}
'''


def find(code, needle):
    return next(i for i, ln in enumerate(code) if needle in ln)


def main():
    orig = SRC.splitlines()
    code = qfeed.strip_code(orig)
    consts = {'kG': 0x83930050, 'kH': 0x83930054, 'kBuilt': 0x83930058}
    ix = qfeedkey.Index(synthetic_db())
    kr = qfeedkey.Keyer(ix, consts)

    def res(needle, base):
        return kr.resolve(code, orig, find(code, needle), base)

    r = res('use(obj + 16)', 'obj')
    check('route 1: hook argument typed by one vtable', r['route'] == 'hookarg' and r['id'] == 'vt:82010000', str(r))
    w = ix.writers(r['handles'], 16)
    check('route 1: its writer is found', list(w) == [0x82511111], str(dict(w)))
    r = res('use(two + 16)', 'two')
    check('route 1: a hook argument with two types stays unresolved', r['route'] == 'unresolved' and 'ambiguous' in r['reason'], str(r))
    r = res('use(mgr + 8)', 'mgr')
    check('route 2: pointer loaded from a global', r['route'] == 'global' and r['id'] == 'g:83930050', str(r))
    check('route 2: its writer is found', list(ix.writers(r['handles'], 8)) == [0x82522222])
    r = res('use(sub + 4)', 'sub')
    check('route 2 through a field: p(g:G,D)', r['route'] == 'chain' and r['id'] == 'p(g:83930050,8)', str(r))
    check('route 2 through a field: its writer is found', list(ix.writers(r['handles'], 4)) == [0x82533333])
    r = res('use(o + 4)', 'o')
    check('route 3: the constructor named on the assignment line', r['route'] == 'ctor' and r['id'] == 'vt:82020000', str(r))
    check('route 3: its writer is found', list(ix.writers(r['handles'], 4)) == [0x82544444])
    r = res('use(m + 8)', 'm')
    check('ambiguous: two globals stay unresolved', r['route'] == 'unresolved' and 'different globals' in r['reason'], str(r))
    r = res('use(held + 8)', 'held')
    check('a parameter of a helper stays unresolved', r['route'] == 'unresolved' and 'parameter' in r['reason'], str(r))
    r = res('use(w + 4)', 'w')
    check('a walking pointer stays unresolved', r['route'] == 'unresolved' and 'compound' in r['reason'], str(r))
    r = res('use(obj + 16)', 'x.y')
    check('a member expression stays unresolved', r['route'] == 'unresolved', str(r))

    # real part: needs the field index
    W = qfeed.Writers()
    if not W.ok:
        print('SKIP real index part: no field index (py tools\\q\\q.py field build)')
    else:
        import qfield
        names = {}
        rix = qfeedkey.Index(W.con, names, qfield.rtti_names(os.path.dirname(qfeed.DB)))
        rows, files, _p, _W = qfeed.collect(W)
        reads, handles = qfeedkey.key_rows(rows, files, W, rix)
        fields = [x for x in rows if x['kind'] == 'field']
        check('real: every unresolved field row says why', all(x['reason'] for x in fields if x['route'] == 'unresolved'))
        check('real: a known row has a writer count, a key or the K / named status',
              all((x['writers_n'] not in ('', '0')) or x['old_status'] == 'named' for x in fields if x['result'] == 'known'))
        check('real: every keyed row names its evidence', all(x['evidence'] for x in fields if x['route'] not in ('', 'unresolved')))
        tmp = os.path.join(tempfile.gettempdir(), 'qfeedkey_selftest.tsv')
        qfeedkey.write_tsv(rows, tmp)
        check('real: the tsv is written', os.path.getsize(tmp) > 1000)
        os.remove(tmp)
        lines = []
        qfeedkey.summary(rows, lines.append)
        check('real: the summary prints the overall share', any(l.startswith('overall') for l in lines))
    print('FAILED: %s' % ', '.join(FAILS) if FAILS else 'all ok')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
