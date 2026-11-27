r"""q reach A B [--depth N] [--indirect] [--all] [--json]   (S111 QREACH)

Does function A reach function B through calls within N levels (default 6)? Prints the shortest path(s) (`A +OFF -> X +OFF -> Y +OFF -> B`: each
`+OFF` is the call site's offset inside the function before it), the count of functions visited, and the CUTS: so an absence is stated with
its coverage ("no path within 4 (visited 575 functions; cut: ...)"), never as a bare "none".

Edges (the q index, same sources as `q callees` / `q callargs`):
  call, tail    index.db edges kind call / tail (the __savegprlr_ / __restgprlr_ helpers are not followed)
  --indirect    adds, for each function walked: the vcall table's RESOLVED indirect calls (icall status exact / narrow: the receiver's class
                is typed, one slot call -> its table targets; never a guess) and the indirect edges the coverage runs OBSERVED (seen.s1.db,
                the edges `q callargs` marks `indirect`; a tail site is shown `tail`).
Cuts, counted and printed:
  depth         functions at the last level whose callees were not read (only when B was not found)
  indirect      indirect call sites in the walked functions that were NOT followed: without --indirect every bctrl; with it the sites that are
                neither resolved nor observed
Path length = number of calls; ties: every shortest path is listed up to 3 (--all: up to 200).
"""
import collections
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

NOISE = re.compile(r'^__(save|rest)')      # q callees hides these helpers (qsheet.NOISE)
DEPTH = 6
SHOW = 3
SHOW_ALL = 200


class ReachError(Exception):
    pass


class Graph:
    """out(f) -> {target: (site offset or None for a tail / unknown, kind)}; sources are sqlite connections (a test passes its own):
    ix = index.db (funcs, edges), vc = vcall db (icall, icall_target) or None, seen = seen db (seen) or None."""

    def __init__(self, ix, vc=None, seen=None, indirect=False):
        self.ix, self.vc, self.seen, self.indirect = ix, vc, seen, indirect
        self.name = {a: n for a, n in ix.execute('select addr,name from funcs where name is not null')}
        self.nb = {}
        self._o = {}

    def label(self, f):
        n = self.name.get(f)
        s = 'sub_%08X' % f
        return s if not n or n == s else '%s (%s)' % (s, n)

    def n_bctrl(self, f):
        if f not in self.nb:
            r = self.ix.execute('select n_bctrl from funcs where addr=?', (f,)).fetchone()
            self.nb[f] = (r[0] or 0) if r else 0
        return self.nb[f]

    def out(self, f):
        if f in self._o:
            return self._o[f]
        o = {}

        def add(t, off, kind):
            if t == f or NOISE.match(self.name.get(t) or ''):
                return
            if t not in o or (o[t][1] not in ('call', 'tail') and kind in ('call', 'tail')):
                o[t] = (off, kind)
            elif o[t][1] == kind and off is not None and (o[t][0] is None or off < o[t][0]):
                o[t] = (off, kind)
        for d, site, kind in self.ix.execute("select dst,site,kind from edges where src=? and kind in ('call','tail') and dst is not null", (f,)):
            add(d, (site - f) if site is not None and site >= f else None, kind)
        if self.indirect:
            if self.vc is not None:
                for t, site in self.vc.execute("select t.target,i.site from icall i join icall_target t on t.site=i.site"
                                               " where i.func=? and i.status in ('exact','narrow')", (f,)):
                    add(t, site - f if site >= f else None, 'vcall')
            if self.seen is not None:
                for t, site in self.seen.execute('select target,site from seen where func=?', (f,)):
                    add(t, (site - f) if site else None, 'seen')
        self._o[f] = o
        return o

    def unfollowed(self, f):
        """indirect call sites of f that this walk did not follow"""
        n = self.n_bctrl(f)
        if not self.indirect:
            return n
        done = set()
        if self.vc is not None:
            done |= {s for (s,) in self.vc.execute("select i.site from icall i join icall_target t on t.site=i.site where i.func=?"
                                                   " and i.status in ('exact','narrow')", (f,))}
        if self.seen is not None:
            done |= {s for (s,) in self.seen.execute('select site from seen where func=? and site<>0', (f,))}
        return max(0, n - len(done))


def walk(g, seeds, b, depth):
    """BFS from the seeds (A may be several functions, comma separated). -> dict(found, dist, parents {f: [parent funcs at the shortest distance]}, visited {f: level}, expanded [f], frontier [f])"""
    level = {a: 0 for a in seeds}
    parents = collections.defaultdict(list)
    cur, expanded, frontier = list(dict.fromkeys(seeds)), [], []
    d = 0
    while cur and b not in level:
        if d >= depth:
            frontier = cur
            break
        nxt = []
        for f in cur:
            expanded.append(f)
            for t in g.out(f):
                if t not in level:
                    level[t] = d + 1
                    nxt.append(t)
                    parents[t].append(f)
                elif level[t] == d + 1 and f not in parents[t]:
                    parents[t].append(f)
        cur, d = nxt, d + 1
    return {'found': b in level, 'dist': level.get(b), 'parents': parents, 'visited': level, 'expanded': expanded, 'frontier': frontier}


def paths(r, g, seeds, b, cap):
    """the shortest paths a -> b as lists of (func, off, kind of the edge leaving it); at most cap"""
    out = []

    def back(f, tail):
        if len(out) >= cap:
            return
        if f in seeds:
            out.append(list(reversed(tail)))
            return
        for p in sorted(r['parents'][f]):
            off, kind = g.out(p)[f]
            back(p, tail + [(p, off, kind)])
    back(b, [])
    return out


def reach(g, a, b, depth=DEPTH, cap=SHOW):
    seeds = list(a) if isinstance(a, (list, tuple)) else [a]
    r = walk(g, seeds, b, depth)
    cuts = {}
    walked = r['expanded']
    ind = sum(g.unfollowed(f) for f in walked)
    if ind:
        cuts['indirect'] = ind
    if not r['found'] and r['frontier']:
        cuts['depth'] = len(r['frontier'])
    res = {'a': seeds, 'b': b, 'depth': depth, 'found': r['found'], 'dist': r['dist'], 'visited': len(r['visited']),
           'walked': len(walked), 'cuts': cuts, 'indirect_followed': g.indirect, 'paths': []}
    if r['found']:
        res['paths'] = [[{'func': f, 'off': off, 'kind': k} for f, off, k in p] for p in paths(r, g, seeds, b, cap)]
    return res


def cut_text(res):
    c = res['cuts']
    parts = []
    if 'depth' in c:
        parts.append('depth %d (%d functions at level %d not expanded)' % (res['depth'], c['depth'], res['depth']))
    if 'indirect' in c:
        what = 'not followed' if res['indirect_followed'] else 'not followed, no --indirect'
        parts.append('%d indirect call sites in the walked functions (%s)' % (c['indirect'], what))
    return '; '.join(parts) if parts else 'none'


def show_hop(g, f, off, kind):
    w = 'tail' if kind == 'tail' or off is None else '+0x%X' % off
    mark = '' if kind in ('call', 'tail') else ' [%s]' % kind
    return '%s %s%s' % (g.label(f), w, mark)


def print_result(g, res, all_paths):
    a, b = ', '.join(g.label(x) for x in res['a']), res['b']
    if res['found']:
        ps = res['paths']
        print('%s reaches %s in %d call(s) (%d shortest path(s) shown)' % (a, g.label(b), res['dist'], len(ps)))
        for p in ps:
            print('  ' + ' -> '.join(show_hop(g, f, off, k) for f, off, k in
                                      [(h['func'], h['off'], h['kind']) for h in p]) + ' -> ' + g.label(b))
        if len(ps) >= (SHOW_ALL if all_paths else SHOW):
            print('  (more shortest paths exist; --all lists up to %d)' % SHOW_ALL if not all_paths else '  (capped at %d)' % SHOW_ALL)
        print('visited %d functions (%d walked); cuts: %s' % (res['visited'], res['walked'], cut_text(res)))
    else:
        print('no path within %d (visited %d functions; cut: %s)' % (res['depth'], res['visited'], cut_text(res)))


def main(argv, con):
    import argparse
    ap = argparse.ArgumentParser(prog='q reach', description=__doc__.split('\n')[0])
    ap.add_argument('a', help='function, or several comma separated (the walk starts from all of them)')
    ap.add_argument('b')
    ap.add_argument('--depth', type=int, default=DEPTH, help='most calls on the path (default %d)' % DEPTH)
    ap.add_argument('--indirect', action='store_true', help='also follow resolved vtable slot calls and observed indirect edges')
    ap.add_argument('--all', action='store_true', help='list every shortest path (up to %d)' % SHOW_ALL)
    ap.add_argument('--json', action='store_true')
    o = ap.parse_args(argv)
    if o.depth < 1:
        raise ReachError('--depth must be at least 1')
    import q as qmod
    import qnames
    import qseen
    ends = []
    for t in o.a.split(',') + [o.b]:
        try:
            ends.append(qmod.resolve(con, t)[0])
        except Exception as ex:  # noqa: BLE001
            if type(ex).__name__ == 'QError':
                raise ReachError(str(ex))
            raise
    vc = seen = None
    notes = []
    if o.indirect:
        vf = qseen.vdb_file()
        if vf:
            vc = qnames.ro(vf)
        else:
            notes.append('no vcall db: resolved slot calls not followed')
        try:
            seen = qseen.open_seen()
        except Exception:  # noqa: BLE001 - no run index here
            notes.append('no seen db: observed indirect edges not followed')
    g = Graph(con, vc, seen, o.indirect)
    res = reach(g, ends[:-1], ends[-1], o.depth, SHOW_ALL if o.all else SHOW)
    if notes:
        res['notes'] = notes
    if o.json:
        print(json.dumps(res, indent=1))
    else:
        print_result(g, res, o.all)
        for n in notes:
            print('note: ' + n)
    return 0
