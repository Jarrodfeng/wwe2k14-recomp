r"""q lib (S110 LSHEET2): which units are THIRD-PARTY LIBRARY code (Havok, Wwise, Lua, Bink, zlib, LZX, C runtime), so that the
reading passes can skip them. Stored in out/harness/index/lib.s1.db, table lib_unit(unit_id, lib, how, evidence).

    py tools/q/q.py lib UNIT        the label of one unit (or "not library") and its evidence
    py tools/q/q.py lib --stats     per library: units by how, functions, bytes; units each rule takes
    py tools/q/q.py lib --build     (re)build lib.s1.db from index.db + units.s1.db + names.s2.db + db/*.json

Rules (precision over recall: a wrong "library" label makes the next pass SKIP real game code; when unsure it is NOT library):

  strings   the unit's own evidence names ONE library: a function string (strrefs), a game-carried name (names.s2.db) or the RTTI class
            name of a vtable slot function matches that library's patterns (LIBS below: hk/hkp/hka/hcl class names, "AK::" / "Ak" names,
            lua_* / "LuaAPI" / Lua\source, bink source paths, zlib messages, "R60xx" runtime errors ...). It is dropped when two
            libraries match, or when the unit also holds a game name that is NOT the library's (a Lua binding / name-table name, a
            fn_string of another class, an RTTI class of another name): such a unit mixes game code in.
  range     a unit with no evidence of its own that lies between two CONSECUTIVE `strings` units of the SAME library (consecutive among
            every `strings` unit of every library, so no other library sits between), when
              - the address span from the end of the first to the start of the second is <= SPAN bytes (64 KB),
              - the unit itself and every unit between the two is CLEAN: no string reference at all, no game-carried name, no RTTI class
                of another name, no db name (db/*.json, confidence medium or high) that does not name the library (token starts with
                the library's key), so one game unit inside the block voids the block.
            This is the risky rule: --stats prints how many units each rule takes.
"""
import collections
import glob
import json
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402
import qunits  # noqa: E402

SRC = os.path.dirname(os.path.dirname(HERE))
LIB_DB = os.path.join(qnames.IDX, 'lib.s1.db')
SPAN = 0x10000
MAXSTR = 600

# library -> (patterns on a string / name, key words for a db name token)
LIBS = collections.OrderedDict([
    ('Havok', ([r'\bhk[a-z]{0,4}\d?[A-Z][A-Za-z0-9_]*', r'\bhcl[A-Z][A-Za-z0-9_]*', r'Havok'], ('hk', 'hcl', 'havok'))),
    ('Wwise', ([r'\bAK::\w+', r'\bAk[A-Z][a-z]\w*', r'Wwise'], ('ak', 'wwise'))),
    ('Lua', ([r'\blua[A-Z_]\w*', r'\bLuaAPI\b', r'Lua[\\/]source', r'\bl(api|vm|gc|object|table|string|do|func|state|parser|lex|code|dump|undump|tm|mem|base|strlib|tablib|mathlib|oslib|iolib|auxlib)\.c(pp)?\b'],
             ('lua',))),
    ('Bink', ([r'(?i)[\\/]bink[\\/]', r'(?i)\bbink\w*\.(c|cpp)\b', r'^Bink (global|instance|list|IO|Snd)'], ('bink',))),
    ('zlib', ([r'incorrect header check', r'invalid block type', r'invalid stored block lengths', r'invalid distance too far back',
               r'unknown compression method', r'incorrect data check', r'Mark Adler', r'too many length or distance symbols',
               r'invalid code lengths set'], ('zlib', 'inflate', 'deflate'))),
    ('LZX', ([r'\bLZX\b'], ('lzx',))),
    ('CRT', ([r'\bR60\d\d\b', r'Microsoft Visual C\+\+ Runtime', r'abnormal program termination', r'[\\/]crt[\\/]src[\\/]',
              r'(?i)\b(output|input|_file|stdio|malloc|dbgheap|heapinit|mlock|tidtable|strftime|fflush|fopen|setlocal)\.c\b'],
             ('crt', 'msvcrt'))),
])
RX = {lib: [re.compile(p) for p in pats] for lib, (pats, _) in LIBS.items()}
GAME_SRC = ('lua_binding', 'name_table')


class LibError(Exception):
    pass


def ro(path):
    if not path or not os.path.exists(path):
        raise LibError('no file %s' % path)
    return sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)


def libs_of(text):
    """-> set of library names whose patterns match one string / name"""
    if not text or len(text) > MAXSTR:
        return set()
    return set(lib for lib, rxs in RX.items() if any(r.search(text) for r in rxs))


def db_names(dbdir=None):
    """-> {addr: name} of db/*.json entries of confidence medium or high"""
    out = {}
    for p in glob.glob(os.path.join(dbdir or os.path.join(SRC, 'db'), '*.json')):
        try:
            ents = json.load(open(p, encoding='utf-8'))
        except (ValueError, OSError):
            continue
        for e in ents:
            try:
                a = int(e['address'], 16)
            except (KeyError, ValueError, TypeError):
                continue
            if e.get('confidence') in ('high', 'medium') and e.get('name'):
                out[a] = e['name']
    return out


def db_name_ok(name, lib):
    """a db name that names the library: some word of it starts with the library's key"""
    keys = LIBS[lib][1]
    words = re.split(r'[_\W]+|(?<=[a-z])(?=[A-Z])', name or '')
    return any(w.lower().startswith(k) for w in words if w for k in keys)


def classify(per_unit, spans, dbn=None):
    """per_unit: {uid: dict(texts=[(kind, text)], other=int)}: kind 'str' (a function string), 'name' (a game name of a library-agnostic
    kind that is NOT a lua binding / name table), 'game' (a lua binding / name-table name: always game code), 'rtti' (a class name);
    `dbs` = [db names] of the unit. spans: {uid: (lo, hi)}, uids in address order is sorted(spans).
    -> {uid: (lib, how, evidence)}"""
    res, ev = {}, {}
    for u, d in per_unit.items():
        hits = collections.defaultdict(list)
        for kind, t in d['texts']:
            for lib in libs_of(t):
                hits[lib].append(t)
        ev[u] = hits
        if len(hits) != 1:
            continue
        lib = next(iter(hits))
        mixed = False
        for kind, t in d['texts']:
            if kind == 'game' or (kind in ('name', 'rtti') and lib not in libs_of(t)):
                mixed = True
        if mixed:
            continue
        res[u] = (lib, 'strings', '; '.join(sorted(set(re.sub(r'\s+', ' ', x.strip())[:60] for x in hits[lib]))[:4]) + ' (%d hits)' % len(hits[lib]))

    def clean(u, lib):
        d = per_unit.get(u)
        if d is None:
            return True
        if d['nstr'] or any(lib not in libs_of(t) for _, t in d['texts']):
            return False
        return all(db_name_ok(n, lib) for n in d.get('dbs', []))
    ids = sorted(spans)
    strs = [u for u in ids if u in res]
    for a, b in zip(strs, strs[1:]):
        la, lb = res[a][0], res[b][0]
        if la != lb or b - a < 2:
            continue
        if spans[b][0] - spans[a][1] > SPAN:
            continue
        mid = [u for u in ids if a < u < b]
        if not all(clean(u, la) for u in mid):
            continue
        for u in mid:
            res[u] = (la, 'range', 'between %d and %d' % (a, b))
    return res


def gather(ix, un, nm, dbn):
    """bulk evidence of every unit -> (per_unit, spans)"""
    fu = dict(un.execute('select func,unit_id from units'))
    spans = {u: (lo, hi) for u, lo, hi in un.execute('select unit_id,lo,hi from unit')}
    per = {u: {'texts': [], 'nstr': 0, 'dbs': []} for u in spans}
    seen = set()
    for f, a, t in ix.execute('select r.func,r.addr,s.text from strrefs r left join strings s on s.addr=r.addr'):
        u = fu.get(f)
        if u is None or (f, a) in seen:
            continue
        seen.add((f, a))
        per[u]['nstr'] += 1
        if t:
            per[u]['texts'].append(('str', t))
    for f, n, src in nm.execute('select func,name,source from names'):
        u = fu.get(f)
        if u is not None:
            per[u]['texts'].append(('game' if src in GAME_SRC else 'name', n))
    has = ix.execute("select 1 from sqlite_master where name='rtti_vtable'").fetchone()
    if has:
        for f, name in ix.execute('select p.value,c.name from ptrs p join rtti_vtable v on p.at>=v.vt and p.at<v.vt+4*v.nslots '
                                  "join rtti_class c on c.td=v.td where p.kind='func'"):
            u = fu.get(f)
            if u is not None and name:
                per[u]['texts'].append(('rtti', name))
    for f, n in dbn.items():
        u = fu.get(f)
        if u is not None:
            per[u]['dbs'].append(n)
    return per, spans


def build(path=None, verbose=False, dbdir=None):
    t0 = time.time()
    path = path or LIB_DB
    ix, un, nm = ro(qnames.INDEX_DB), ro(qunits.UNITS_DB), ro(qnames.NAMES_DB)
    per, spans = gather(ix, un, nm, db_names(dbdir))
    res = classify(per, spans)
    tmp = path + '.tmp%d' % os.getpid()
    if os.path.exists(tmp):
        os.remove(tmp)
    w = sqlite3.connect(tmp)
    w.executescript('create table lib_unit(unit_id integer primary key, lib text, how text, evidence text);'
                    'create table meta(key text primary key, value text);')
    w.executemany('insert into lib_unit values (?,?,?,?)', [(u, *v) for u, v in sorted(res.items())])
    w.executemany('insert into meta values (?,?)', [('index_stamp', qnames.stamp()), ('span', str(SPAN)), ('n', str(len(res)))])
    w.commit()
    w.close()
    os.replace(tmp, path)
    if verbose:
        print('lib.s1.db: %d library units, %.1f s' % (len(res), time.time() - t0))


def open_lib(path=None):
    """read-only connection; built when missing or older than the index"""
    path = path or LIB_DB
    ok = False
    if os.path.exists(path):
        c = sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)
        try:
            ok = dict(c.execute('select key,value from meta')).get('index_stamp') == qnames.stamp()
        except sqlite3.Error:
            ok = False
        finally:
            c.close()
    if not ok:
        build(path, True)
    return sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)


def lookup(uid, con=None):
    """-> (lib, how, evidence) or None"""
    con = con or open_lib()
    return con.execute('select lib,how,evidence from lib_unit where unit_id=?', (uid,)).fetchone()


def stats(con=None):
    con = con or open_lib()
    un = ro(qunits.UNITS_DB)
    ix = ro(qnames.INDEX_DB)
    size = dict(ix.execute('select addr,size from funcs'))
    members = collections.defaultdict(list)
    for f, u in un.execute('select func,unit_id from units'):
        members[u].append(f)
    rows = collections.defaultdict(lambda: [0, 0, 0])
    for u, lib, how in con.execute('select unit_id,lib,how from lib_unit'):
        for key in ((lib, how), (lib, 'all'), ('ALL', how), ('ALL', 'all')):
            r = rows[key]
            r[0] += 1
            r[1] += len(members[u])
            r[2] += sum(size.get(f, 0) for f in members[u])
    return rows


def main(argv):
    if '--build' in argv:
        build(None, True)
        return 0
    if '--stats' in argv:
        rows = stats()
        print('%-8s %-8s %6s %8s %10s' % ('library', 'how', 'units', 'funcs', 'bytes'))
        for key in sorted(rows, key=lambda k: (k[0] == 'ALL', k[0], k[1] != 'all', k[1])):
            u, f, b = rows[key]
            print('%-8s %-8s %6d %8d %10d' % (key[0], key[1], u, f, b))
        return 0
    pos = [a for a in argv if not a.startswith('--')]
    if len(pos) != 1 or not pos[0].isdigit():
        print(__doc__)
        return 2
    r = lookup(int(pos[0]))
    print('unit %s: library %s (%s: %s)' % (pos[0], r[0], r[1], r[2]) if r else 'unit %s: not library' % pos[0])
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (LibError, qnames.NamesError) as e:
        print('q: %s' % e, file=sys.stderr)
        sys.exit(2)
