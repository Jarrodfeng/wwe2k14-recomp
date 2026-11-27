r"""q read pick --from (S111 lane SYSPICK): every function unit a game SYSTEM reaches from its entry functions, one fact sheet per unit.

    py tools/q/q.py read pick --from ENTRY[,ENTRY...] [--depth 3] [--k 25] [--out DIR] [--count] [--with-read] [--recall DB.json]

ENTRY = a function address (0x82866D80, sub_82866D80, 82866D80). Mechanical, no model: the point is to hand the whole system to
cheap sheet-readers (.claude/agents/sheet-reader.md) before narrow probes settle the parts.

  seeds     the entry functions; their units are the first units of the batch.
  walk      callees of each reached function, level by level up to --depth: direct edges (index.db call / tail), OBSERVED indirect
            (seen.s1.db) and STATIC indirect (vcall.s*.db icall_target). A reached function adds its unit (units of 1-2 functions too),
            and every function of an added unit joins the walk (--expand all, the default; seed | none are narrower, see reach()).
  classes   (--classes on|off, default on) a reached function that builds the address of a table (a constructed class: imms lis+addi /
            lis+ori of an index `tables` start) brings every code word of that table into the walk at its own depth: page bodies, thumbnail
            and camera objects are reached through vtables, never by a call. The ctor's store to +0 is not checked.
  direction callees only: a unit is never reached because it CALLS a reached function (the report counts those units: cut by direction).
  --match   W1,W2: the precision proxy, how many reached units' stored readings mention one of the words.
  library   a unit is cut (not listed, not walked through) when its functions are called from more than --k (default 25) distinct units
            OUTSIDE the units reached so far: memcpy, the allocator, the draw submit. The cut is a heuristic and is printed by unit name.
  middleware (--middleware on|off, default on) a qlib Havok unit (how = strings) is cut like a library unit when its OWN functions
            carry the evidence: at least half of them reference a Havok string, carry a Havok game name or sit in a Havok RTTI class's
            vtable, and none has a db name that is not Havok's (middleware_units). qlib's label alone rests on one hit anywhere in the
            unit (unit 100, the CAE screen's unit: one hkMultipleVertexBuffer slot among 150 functions). The report prints
            `cut as middleware: N units`.
  path      every sheet's second line: `reached: depth D from sub_SEED via sub_PARENT (unit U)` (entries: `reached: depth 0 (entry)`); a unit
            first reached through a class slot ends `(unit U; class slot: sub_PARENT builds the table that holds it)`: that parent does not
            call it and sits at the same depth (cae entries: 118 of 305 non-entry units).
  output    sheets for units with no stored reading (DIR/<unit>.txt) and batch.txt in reach order (entries first); with --with-read one
            line per already-read unit with its stored purpose. --count writes nothing.
  --recall  a db/*.json file: how many of its functions the reached set holds, and for each miss the reason.

SOUNDNESS: reached = "the index shows a call path of <= depth edges from an entry"; it is NOT "belongs to the system". An edge the
index cannot see (an indirect call no run observed, no static resolution) is a miss; a library unit is a guess by fan-in.
"""
import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

DEPTH = 2     # S111 SYSPICK2, CAC title editor, classes on: recall 40 / 68 / 71 of 81 at depth 1 / 2 / 3 (units 247 / 655 / 1362): 2 is where it flattens
LIB_K = 25
SLOT_K = 2


def reach(seeds, out, uof, ucallers, members, depth=DEPTH, k=LIB_K, expand='all', cls=None, mw=None, why=None):
    """seeds [func]; out {func: iterable of callee funcs} (all edge sources merged); uof {func: unit}; ucallers {unit: set(caller units)};
    members {unit: [func]}. k None = no library cut (the twin the selftest must see fail).
    cls {func: [slot func]} (S111 SYSPICK2): a function that builds the address of a table (a constructed class) brings every slot function
    of that table into the reach at the SAME depth as itself; None = off (the twin the selftest must see fail).
    The walk goes along callee edges only: nothing is reached because it CALLS a reached function (ucallers only feeds the library cut).
    expand: 'all' = a unit that joins the reach brings ALL its functions into the walk (a unit is one source file's run of functions: its
    methods, dtor and update are called through vtables the index does not see); 'seed' = only the entry units do; 'none' = only the
    called function walks on (S111 SYSPICK measured on the CAC title editor: recall 41 / 19 / 7 of 51).
    -> dict(units [(unit, depth)] in reach order, funcs {func: depth}, cut [unit] in cut order, via {unit: (parent func, seed func)} the BFS
    parent that first added the unit and the entry its path started at; (None, seed) for an entry unit (S111 PICKPATH), middleware [unit]).
    mw: a set of units that are third-party middleware (Havok: middleware_units): such a unit is cut like a library unit (not listed, not
    walked through) unless it is an entry; None = no middleware cut (the twin the selftest must see fail).
    why (S111 GATEPICK): {seed func: text} for seeds that are not plain entries (callers of a gate); the first seed of a unit that has a text
    labels the unit in the result's 'why' {unit: text} (reached_line says `reached: TEXT`)."""
    middleware = []
    whyu = {}
    funcs = {}
    seedof = {}                # {func: the entry function its path started at}
    via = {}
    units = []                 # [(unit, depth)]
    inreach = set()
    cut = []
    cutset = set()
    viaclass = []
    partial = set()            # units that entered through a class slot (only that function walks) until a call reaches them
    frontier = []
    clsq = []

    def add(x, d, slot=False, sd=None):
        """slot: x came in as a class slot: its own tables are NOT followed at this depth (that closure would take in every
        class the slot's unit-siblings build); they are followed when the walk reaches the function that builds them by a call"""
        if x not in funcs:
            funcs[x] = d
            seedof[x] = x if sd is None else sd
            frontier.append(x)
            if cls and not slot:
                clsq.append(x)

    def join(u, d, f, slot=False, src=None):
        """unit u enters the reach at depth d through function f (called from src; None for an entry)"""
        inreach.add(u)
        units.append((u, d))
        sd = seedof.get(src, f) if src is not None else f
        via[u] = (src, sd)
        if slot and expand == 'all':
            partial.add(u)
        for x in (sorted(members.get(u, ())) if (not slot and (expand == 'all' or (expand == 'seed' and d == 0))) else [f]):
            add(x, d, slot, sd)

    def visit(c, d, slot=False, src=None):
        u = uof.get(c)
        if u is None or u in cutset:
            return
        sd = seedof.get(src) if src is not None else None
        if u in inreach:
            if not slot and u in partial:        # entered by one slot function only: a call brings the whole unit in
                partial.discard(u)
                for x in sorted(members.get(u, ())):
                    add(x, d, False, sd)
            add(c, d, slot, sd)
            return
        if mw and u in mw:
            cutset.add(u)
            middleware.append(u)
            return
        if k is not None and len(ucallers.get(u, set()) - inreach) > k:
            cutset.add(u)
            cut.append(u)
            return
        if slot:
            viaclass.append(u)
        join(u, d, c, slot, src)

    def drain(d):
        while clsq:
            f = clsq.pop()
            for s in cls.get(f, ()):
                visit(s, d, True, f)
    for f in seeds:
        u = uof.get(f)
        if why and f in why and u is not None and u not in inreach:
            whyu[u] = why[f]
        if u is not None and u not in inreach:
            join(u, 0, f)
        else:
            add(f, 0)
    if cls:
        drain(0)
    for d in range(1, depth + 1):
        cur, frontier[:] = list(frontier), []
        for f in cur:
            for c in sorted(out.get(f, ())):
                visit(c, d, False, f)
        if cls:
            drain(d)
        if not frontier:
            break
    return dict(units=units, funcs=funcs, cut=cut, viaclass=viaclass, via=via, middleware=middleware, why=whyu)


def reached_line(res, uof, u, d):
    """the second line of a sheet: how the unit was reached (S111 PICKPATH). A unit first reached through a class slot (res['viaclass'])
    has as parent the function that BUILDS the table holding the slot, at the same depth, not a caller: the line says so."""
    p, sd = res['via'].get(u, (None, None))
    if p is None:
        return ('reached: ' + res['why'][u]) if u in res.get('why', ()) else 'reached: depth %d (entry)' % d
    return 'reached: depth %d from sub_%08X via sub_%08X (unit %s%s)' % (
        d, sd, p, uof.get(p, '?'), ('; class slot: sub_%08X builds the table that holds it' % p) if u in res.get('viaclass', ()) else '')


def with_reached(text, line):
    """a sheet with the reach line as its second line (line 1 stays byte-identical)"""
    head, _nl, tail = text.partition('\n')
    return head + '\n' + line + '\n' + tail


def via_text(res, u, d):
    """'depth D via sub_PARENT' (a bundle heading) or 'depth 0 (entry)'"""
    p, _sd = res['via'].get(u, (None, None))
    if p is None and u in res.get('why', ()):
        return res['why'][u]
    return 'depth %d (entry)' % d if p is None else 'depth %d via sub_%08X%s' % (d, p, ' (class slot)' if u in res.get('viaclass', ()) else '')


def caller_seeds(g, gates):
    """S111 GATEPICK: -> ([func] seeds, {func: text}): per gate the gate itself, then every function that CALLS it (index.db call / tail, observed
    and static indirect: g.inn holds all three sources), sorted. Not a walk: the callers' own bodies are the seeds."""
    seeds, why = [], {}
    for gt in gates:
        for f in [gt] + sorted(g.inn.get(gt, ())):
            if f not in why:
                why[f] = ('caller of sub_%08X' % gt) if f != gt else ('the gate sub_%08X' % gt)
                seeds.append(f)
    return seeds, why


def reach_set(entries, depth=DEPTH, k=LIB_K, classes='on', expand='all', slot_k=SLOT_K, ctx=None, middleware='on', callers_of=()):
    """the real index's reach of entries [func] -> (ctx, graph, entries (owners; with callers_of: + the gates and their callers), result)"""
    import qsheet
    ctx = ctx or qsheet.Ctx()
    g = load_graph(ctx)
    g.expand = expand
    if g.expand not in ('all', 'seed', 'none'):
        raise ValueError('--expand is one of: all seed none')
    entries = list(entries)
    for i, a in enumerate(entries):          # an address inside a function: its owner
        if a not in g.uof:
            import qnames
            o = qnames.owner(ctx.ix, a)
            if o in g.uof:
                entries[i] = o
    g.cls = load_classes(ctx.ix, slot_k)
    if classes not in ('on', 'off'):
        raise ValueError('--classes is on or off')
    g.use_cls = classes != 'off'
    if middleware not in ('on', 'off'):
        raise ValueError('--middleware is on or off')
    g.mw = load_middleware(ctx) if middleware == 'on' else None
    g.why = None
    if callers_of:
        gates = list(callers_of)
        for i, a in enumerate(gates):
            if a not in g.uof and a not in g.inn:
                import qnames
                gates[i] = qnames.owner(ctx.ix, a) or a
        seeds, g.why = caller_seeds(g, gates)
        entries = entries + [f for f in seeds if f not in entries]
    return ctx, g, entries, run(g, entries, depth, k)


class Graph:
    def __init__(self, out, uof, ucallers, members):
        self.out, self.uof, self.ucallers, self.members = out, uof, ucallers, members
        self.expand = 'all'
        self.cls = {}          # {func: [slot func]} constructed classes (see reach)
        self.use_cls = True
        self.inn = {}          # {func: set(caller func)}, only for the report of units cut by direction
        self.mw = None         # {unit} of middleware (Havok) cut from the walk, see reach
        self.why = None        # {seed func: text} callers of a gate (see reach)


def make_graph(edges, uof):
    """edges [(src, dst)] -> Graph; self-unit edges do not count as callers"""
    out = collections.defaultdict(set)
    inn = collections.defaultdict(set)
    uc = collections.defaultdict(set)
    for s, d in edges:
        if s == d:
            continue
        out[s].add(d)
        inn[d].add(s)
        us, ud = uof.get(s), uof.get(d)
        if us is not None and ud is not None and us != ud:
            uc[ud].add(us)
    members = collections.defaultdict(list)
    for f, u in uof.items():
        members[u].append(f)
    g = Graph(out, uof, uc, members)
    g.inn = inn
    return g


def load_classes(ix, slot_k=SLOT_K):
    """constructed classes of the real index: {func: [slot func]} for every function that builds the address of a table (imms lis+addi /
    lis+ori of a `tables` start of rule rtti or code) -> the code words of that table (ptrs kind func inside it). The ctor's store of the
    table word to +0 of `this` is NOT checked: a function that builds a table address and the table's slots belong together either way.
    A slot function held by more than slot_k distinct tables is a shared base method (no-op, thunk, base dtor), the class analogue of the
    library cut: it is not a slot of any class here (None = keep all)."""
    slots = collections.defaultdict(list)
    held = collections.defaultdict(set)
    for st, v in ix.execute("select t.start,p.value from tables t join ptrs p on p.at>=t.start and p.at<t.start+4*t.len "
                            "where t.rule in ('rtti','code') and p.kind='func' order by t.start,p.at"):
        slots[st].append(v)
        held[v].add(st)
    if slot_k is not None:
        slots = {st: [v for v in vs if len(held[v]) <= slot_k] for st, vs in slots.items()}
    cls = collections.defaultdict(list)
    for f, v in ix.execute("select distinct i.func,i.value from imms i join tables t on t.start=i.value "
                           "where t.rule in ('rtti','code') and i.op in ('lis+addi','lis+ori','lis+addi?','lis+ori?')"):
        for s in slots.get(v, ()):
            if s != f and s not in cls[f]:
                cls[f].append(s)
    return dict(cls)


def direction_strays(g, res):
    """units cut by direction (rule 2): units not reached that hold a CALLER of a reached function. The walk follows callees only, so
    each of them is out; the count says how much a two-way walk would have added."""
    inr = {u for u, _d in res['units']}
    seen = set()
    for f in res['funcs']:
        for c in g.inn.get(f, ()):
            u = g.uof.get(c)
            if u is not None and u not in inr:
                seen.add(u)
    return len(seen)


def load_graph(ctx):
    """the three call sources of the real index merged (see the module doc)"""
    uof = dict(ctx.units.execute('select func,unit_id from units'))
    edges = list(ctx.ix.execute("select src,dst from edges where kind in ('call','tail')"))
    edges += list(ctx.seen.execute('select func,target from seen'))
    edges += list(ctx.vc.execute('select i.func,t.target from icall_target t join icall i on i.site=t.site'))
    g = make_graph(edges, uof)
    g.cls = load_classes(ctx.ix)
    return g


def run(g, entries, depth=DEPTH, k=LIB_K):
    return reach(entries, g.out, g.uof, g.ucallers, g.members, depth, k, g.expand, g.cls if g.use_cls else None, g.mw, g.why)


def middleware_units(cand, members, fev, dbn):
    """cand {unit} qlib Havok `strings` units; members {unit: [func]}; fev {func} the functions with Havok evidence of their OWN (a string
    they reference, a game-carried name, the RTTI class of a vtable holding them); dbn {func: db name} (db/*.json, medium / high).
    -> {unit}: the units of cand where at least half the functions are in fev and no function has a db name that does not name Havok
    (qlib.db_name_ok). qlib's label rests on one hit anywhere in the unit (S111 PICKPATH review on the cae / moveset / rumble entries:
    unit 100 the CAE screen's unit, 4601, 7261 (a `wepn` fourcc), 4610 (CGameHavokAction / CGameHavokUpdate strings) were each labelled
    by 1 to 5 functions among 34-150)."""
    import qlib
    out = set()
    for u in cand:
        fs = members.get(u, ())
        if not fs or 2 * sum(1 for f in fs if f in fev) < len(fs):
            continue
        if any(f in dbn and not qlib.db_name_ok(dbn[f], 'Havok') for f in fs):
            continue
        out.add(u)
    return out


def load_middleware(ctx):
    """{unit} of Havok middleware for the walk: the qlib Havok units (how = strings; the `range` guess is not used) that middleware_units
    keeps, from the same evidence qlib reads (index.db strrefs + strings, names.s2.db, RTTI vtables) taken per function"""
    import qlib
    import qnames
    c = qlib.open_lib()
    try:
        cand = {u for (u,) in c.execute("select unit_id from lib_unit where lib='Havok' and how='strings'")}
    finally:
        c.close()
    members = collections.defaultdict(list)
    for f, u in ctx.units.execute('select func,unit_id from units'):
        if u in cand:
            members[u].append(f)
    funcs = {f for fs in members.values() for f in fs}

    def havok(t):
        return 'Havok' in qlib.libs_of(t)
    fev = {f for f, t in ctx.ix.execute('select r.func,s.text from strrefs r join strings s on s.addr=r.addr') if f in funcs and havok(t)}
    nm = qlib.ro(qnames.NAMES_DB)
    try:
        fev |= {f for f, n, src in nm.execute('select func,name,source from names') if f in funcs and src not in qlib.GAME_SRC and havok(n)}
    finally:
        nm.close()
    if ctx.ix.execute("select 1 from sqlite_master where name='rtti_vtable'").fetchone():
        for vt, ns, name in ctx.ix.execute('select v.vt,v.nslots,c.name from rtti_vtable v join rtti_class c on c.td=v.td').fetchall():
            if name and havok(name):
                fev |= {v for (v,) in ctx.ix.execute("select value from ptrs where at>=? and at<? and kind='func'", (vt, vt + 4 * ns)) if v in funcs}
    return middleware_units(cand, members, fev, qlib.db_names())


def parse_entries(text):
    ents = []
    for w in re.split(r'[,\s]+', text.strip()):
        if not w:
            continue
        m = re.fullmatch(r'(?:0x|sub_)?([0-9A-Fa-f]{8})', w)
        if not m:
            raise ValueError('entry %r is not a function address (0x82866D80)' % w)
        ents.append(int(m.group(1), 16))
    if not ents:
        raise ValueError('--from / --callers-of needs at least one function address')
    return ents


def miss_reason(g, entries, f, depth, k):
    """why a function is not in the reached set: unit cut as library / only reachable deeper / no edge path the index sees"""
    if f not in g.uof:
        return 'no unit holds it'
    deep = run(g, entries, 12, k)
    if f in deep['funcs']:
        return 'depth: reached at depth %d' % deep['funcs'][f]
    if g.uof[f] in deep['cut']:
        return 'library cut: unit %d is called from > %d units outside the reach' % (g.uof[f], k)
    if g.uof[f] in deep.get('middleware', ()):
        return 'middleware cut: unit %d is labelled Havok by its own evidence (q lib %d; --middleware off walks it)' % (g.uof[f], g.uof[f])
    if k is not None and f in run(g, entries, 12, None)['funcs']:
        return 'library cut: only reachable through a unit cut as library'
    if g.mw:
        mw, g.mw = g.mw, None
        try:
            behind = f in run(g, entries, 12, k)['funcs']
        finally:
            g.mw = mw
        if behind:
            return 'middleware cut: only reachable through a unit cut as middleware (--middleware off reaches it)'
    if g.expand != 'all':
        return 'not called from a walked function (its unit is not in the reach; try --expand all)'
    return 'no call path in the index (an indirect call neither observed nor resolved)'


def recall(g, entries, res, path, depth, k, names=None):
    """-> (lines, held, total) for the functions of a db/*.json"""
    import json
    ents = json.load(open(path, encoding='utf-8'))
    addrs = []
    for e in ents:
        try:
            a = int(e['address'], 16)
        except (KeyError, ValueError, TypeError):
            continue
        addrs.append((a, e.get('name', '?')))
    held = [(a, n) for a, n in addrs if a in res['funcs']]
    uheld = [(a, n) for a, n in addrs if g.uof.get(a) in {u for u, _ in res['units']}]
    lines = ['recall %s: %d of %d functions reached (%d of %d lie in a listed unit)' % (os.path.basename(path), len(held), len(addrs), len(uheld), len(addrs))]
    for a, n in addrs:
        if a not in res['funcs']:
            lines.append('  miss %08X %s: %s' % (a, n, miss_reason(g, entries, a, depth, k)))
    return lines, len(held), len(addrs)


def read_purposes():
    import qread
    c = qread.open_ro(None)
    if c is None:
        return {}
    try:
        return {u: p or '' for u, p in c.execute('select unit_id,purpose from unit_read order by id')}
    finally:
        c.close()


def read_rows():
    """{unit: [purpose, because, system, missing of every stored reading]} (the text --match looks in)"""
    import qread
    c = qread.open_ro(None)
    if c is None:
        return {}
    try:
        d = collections.defaultdict(list)
        for u, *cols in c.execute('select unit_id,purpose,because,system,missing from unit_read'):
            d[u] += [x or '' for x in cols]
        return d
    finally:
        c.close()


def main(rest):
    import qsheet
    import qreadpick

    def opt(flag, default=None):
        return rest[rest.index(flag) + 1] if flag in rest and rest.index(flag) + 1 < len(rest) else default
    gates = parse_entries(opt('--callers-of')) if '--callers-of' in rest else []
    entries = parse_entries(opt('--from')) if '--from' in rest else []
    depth = int(opt('--depth', DEPTH if entries else 1))
    k = int(opt('--k', LIB_K))
    outdir = opt('--out')
    ctx, g, entries, res = reach_set(entries, depth, k, opt('--classes', 'on'), opt('--expand', 'all'), int(opt('--slot-k', SLOT_K)), qsheet.Ctx(), opt('--middleware', 'on'), gates)
    answered, _rs = qreadpick.read_state()
    purposes = read_purposes() if '--with-read' in rest else {}
    ctx.hooks()

    def nm(f):
        return (ctx.game[f][0] if f in ctx.game else (ctx.db[f][0] if f in ctx.db else None))

    def top(u, n=4):
        return ' '.join(x for x in (nm(f) for f in sorted(g.members.get(u, ())) if f in ctx.game or f in ctx.db) if x)[:200] if n else ''
    unread = [(u, d) for u, d in res['units'] if u not in answered]
    isread = [(u, d) for u, d in res['units'] if u in answered]
    nreach = collections.Counter(g.uof[f] for f in res['funcs'] if f in g.uof)
    if outdir and '--count' not in rest:
        os.makedirs(outdir, exist_ok=True)
    total = 0
    sizes = {}
    for u, d in unread:
        text, fl = qsheet.build(ctx, u, earlier=False)
        sizes[u] = fl['bytes']
        total += fl['bytes']
        if outdir and '--count' not in rest:
            text = with_reached(text, reached_line(res, g.uof, u, d))
            with open(os.path.join(outdir, '%d.txt' % u), 'w', encoding='utf-8') as fh:
                fh.write(text)
    batch = []
    for u, d in res['units']:
        if u in answered:
            if '--with-read' in rest:
                batch.append('read\tunit %d\tdepth %d\t%d reached of %d funcs\t%s\n' % (u, d, nreach[u], len(g.members.get(u, ())), (purposes.get(u) or '(no purpose)').replace('\n', ' ')))
        else:
            batch.append('%d.txt\tunit %d\tdepth %d\t%d reached of %d funcs\t%d B\t%s\n' % (u, u, d, nreach[u], len(g.members.get(u, ())), sizes[u], top(u)))
    if outdir and '--count' not in rest:
        with open(os.path.join(outdir, 'batch.txt'), 'w', encoding='utf-8') as fh:
            fh.writelines(batch)
    print('from %s  depth %d  library cut k=%d' % (','.join('%08X' % a for a in entries), depth, k))
    if gates:
        print('callers of %s: %d seed functions in %d units (the gate + every function that calls it; entries at depth 0)' % (
            ','.join('%08X' % a for a in gates), len(g.why or ()), len({g.uof[f] for f in (g.why or ()) if f in g.uof})))
    print('units %d (%d to read, %d already read), functions reached %d, sheet bytes %d' %
          (len(res['units']), len(unread), len(isread), len(res['funcs']), total))
    bydepth = collections.Counter(d for _u, d in res['units'])
    print('units by depth: %s' % ' '.join('%d:%d' % kv for kv in sorted(bydepth.items())))
    print('cut as library (%d): %s%s' % (len(res['cut']), '; '.join('unit %d %s' % (u, top(u)[:60] or '(unnamed)') for u in res['cut'][:40]),
                                       ' ... (+%d more)' % (len(res['cut']) - 40) if len(res['cut']) > 40 else ''))
    print('classes %s: %d units first reached through a constructed class\'s slot; cut by direction (callers only, never walked): %d units; '
          'cut as library: %d units' % ('on' if g.use_cls else 'off', len(res['viaclass']), direction_strays(g, res), len(res['cut'])))
    print('cut as middleware: %d units%s' % (len(res['middleware']), (' (Havok: half or more of the unit\'s functions carry Havok strings / names / RTTI, no game db name): ' + '; '.join(
        'unit %d %s' % (u, top(u)[:60] or '(unnamed)') for u in res['middleware'][:40]) + (' ... (+%d more)' % (len(res['middleware']) - 40) if len(res['middleware']) > 40 else '')) if res['middleware'] else ''))
    if '--match' in rest:                    # precision proxy: reached units with a stored reading that mentions the system's words
        words = [w.lower() for w in opt('--match', '').split(',') if w]
        hit = 0
        c = read_rows()
        for u, _d in res['units']:
            txt = ' '.join(c.get(u, ())).lower()
            hit += 1 if txt and any(w in txt for w in words) else 0
        print('stored readings: %d of %d reached units have one; %d mention %s' % (sum(1 for u, _d in res['units'] if u in c), len(res['units']), hit, '/'.join(words)))
    if isread:
        print('already read: %s' % ' '.join(str(u) for u, _d in isread))
    if '--recall' in rest:
        lines, _h, _t = recall(g, entries, res, opt('--recall'), depth, k)
        print('\n'.join(lines))
    if outdir and '--count' not in rest:
        print('wrote %d sheets + batch.txt -> %s' % (len(unread), outdir))
    print('reached = an index call path from an entry, not proof of membership; a library cut is a fan-in guess (see the module doc)')
    return 0
