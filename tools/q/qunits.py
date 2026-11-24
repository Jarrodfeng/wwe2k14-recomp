r"""q unit: the unit (module cluster) a function belongs to (S110 lane NAMES, rule from COVER.md).

    py tools/q/q.py unit ADDR [--json] [--rebuild]

A unit is an address interval of game functions closed under direct call / tail edges that span at most K=32 functions
(the union of the `reach` intervals; link order keeps an object file together). The unit id is the interval's rank in address
order. The answer lists the unit's range, its functions, and the members that carry a game name (q name, names.s1.db).
MAY BE INCOMPLETE: a unit is a heuristic cluster, not a module table (none exists): a boundary can fall inside a real module
or join two; units of 1-2 functions are the rest (about 33,000 functions), and calls through pointers do not join units.
The tables are in out/harness/index/units.s1.db (own file, rebuilt when index.db changes).
"""
import collections
import os
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402

UNITS_DB = os.path.join(qnames.IDX, 'units.s1.db')
K = 32
INCOMPLETE = ('may be incomplete: a unit is a cluster by link order (K=32), not a module table; '
              'calls through pointers do not join units; units of 1-2 functions say nothing about a module.')


def load_graph(con):
    funcs = {a: (n, i, b) for a, n, i, b in con.execute('select addr,name,insns,n_bctrl from funcs where has_body=1')}
    callers, callees = collections.defaultdict(set), collections.defaultdict(set)
    for s, d in con.execute("select src,dst from edges where kind in ('call','tail')"):
        if s in funcs and d in funcs and s != d:
            callers[d].add(s)
            callees[s].add(d)
    return funcs, callers, callees


def compute_units(addrs, callees, k=K):
    """[(i, j)] half-open index ranges into the sorted `addrs`: the union of reach intervals of edges spanning <= k functions."""
    pos = {a: i for i, a in enumerate(addrs)}
    n = len(addrs)
    reach = list(range(n))
    for s, ds in callees.items():
        for d in ds:
            i, j = sorted((pos[s], pos[d]))
            if j - i <= k and reach[i] < j:
                reach[i] = j
    units, i = [], 0
    while i < n:
        end, j = reach[i], i
        while j <= end:
            end = max(end, reach[j])
            j += 1
        units.append((i, j))
        i = j
    return units


def build(verbose=False):
    t0 = time.time()
    con = qnames.ro(qnames.INDEX_DB)
    funcs, _, callees = load_graph(con)
    addrs = sorted(funcs)
    units = compute_units(addrs, callees)
    tmp = UNITS_DB + '.tmp%d' % os.getpid()
    if os.path.exists(tmp):
        os.remove(tmp)
    w = sqlite3.connect(tmp)
    w.executescript('create table units(func integer primary key, unit_id integer);'
                    'create table unit(unit_id integer primary key, lo integer, hi integer, n_funcs integer);'
                    'create table meta(key text primary key, value text);')
    w.executemany('insert into unit values (?,?,?,?)', [(u, addrs[i], addrs[j - 1], j - i) for u, (i, j) in enumerate(units)])
    w.executemany('insert into units values (?,?)', [(addrs[x], u) for u, (i, j) in enumerate(units) for x in range(i, j)])
    w.executemany('insert into meta values (?,?)', [('index_stamp', qnames.stamp()), ('k', str(K)), ('n_units', str(len(units)))])
    w.commit()
    w.close()
    os.replace(tmp, UNITS_DB)
    if verbose:
        print('units.s1.db: %d units over %d functions, %.1f s' % (len(units), len(addrs), time.time() - t0))


def open_units(rebuild=False):
    p = UNITS_DB.replace('\\', '/')
    if not rebuild and os.path.exists(UNITS_DB):
        c = sqlite3.connect('file:%s?mode=ro' % p, uri=True)
        try:
            ok = dict(c.execute('select key,value from meta')).get('index_stamp') == qnames.stamp()
        finally:
            c.close()
        if ok:
            return sqlite3.connect('file:%s?mode=ro' % p, uri=True)
    build(True)
    return sqlite3.connect('file:%s?mode=ro' % p, uri=True)


def main(argv):
    import json
    js = '--json' in argv
    pos = [x for x in argv if not x.startswith('--')]
    u = open_units('--rebuild' in argv)
    if not pos:
        print(__doc__)
        return 2
    a = qnames.parse_addr(pos[0])
    if a is None:
        raise qnames.NamesError('q unit takes a function address (0x82xxxxxx or sub_82xxxxxx), not %r' % pos[0])
    ix = qnames.ro(qnames.INDEX_DB)
    o = qnames.owner(ix, a)
    r = u.execute('select unit_id from units where func=?', (o,)).fetchone() if o else None
    if not r:
        raise qnames.NamesError('%s is not inside a function of the index' % pos[0])
    uid = r[0]
    lo, hi, nf = u.execute('select lo,hi,n_funcs from unit where unit_id=?', (uid,)).fetchone()
    members = [x[0] for x in u.execute('select func from units where unit_id=? order by func', (uid,))]
    names = collections.defaultdict(list)
    n = qnames.open_names()
    for f, nm in n.execute('select func,name from names where func between ? and ? and source!="fn_string_shared" order by name', (lo, hi)):
        names[f].append(nm)
    dbn = {}
    res = {'func': o, 'unit_id': uid, 'lo': lo, 'hi': hi, 'n_funcs': nf, 'members': members,
           'named': {f: names[f] for f in members if f in names}, 'incomplete': INCOMPLETE}
    if js:
        print(json.dumps(res, indent=1))
        return 0
    print('sub_%08X is in unit %d: 0x%08X..0x%08X, %d function(s), %d with a game name' % (o, uid, lo, hi, nf, len(res['named'])))
    shown = members if len(members) <= 60 else members[:60]
    for f in shown:
        print('  sub_%08X%s%s' % (f, '  <--' if f == o else '', '  ' + ', '.join(names[f][:3]) if f in names else ''))
    if len(members) > len(shown):
        print('  ... %d more (--json lists all)' % (len(members) - len(shown)))
    import qread
    rl = qread.read_line(uid)
    if rl:
        print(rl)
    print(INCOMPLETE)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except qnames.NamesError as e:
        print('q: %s' % e, file=sys.stderr)
        sys.exit(2)
