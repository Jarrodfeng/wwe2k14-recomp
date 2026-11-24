r"""q cover: the session's progress measure, MECHANICAL and MEANING coverage over every game function (S110 lane NAMES;
brought in from COVER.md's cover.py, definitions unchanged). Read-only over out/harness/index and the repo's db/*.json.

    py tools/q/qcover.py [--json] [--no-names] [--k 32]      (about 5 s)

MECHANICAL: a function is complete when its callers are complete (a direct edge, address never taken) AND its callees are
complete (no indirect call site, or every one has an exact class in vcall).
MEANING, first level that holds: 0a db entry; 0b anchor (hook, mid-asm hook, db high, string ref, RTTI slot, binding pointer,
and, unless --no-names, a function the game itself names: names.s1.db); 1 or 2 call hops from an anchor (never through a hub
with more than 50 callers); 3 shares a unit with an anchor; 4 none.
--no-names reproduces COVER.md's numbers exactly; --json prints the counts for a later diff.
MAY BE INCOMPLETE: it counts what the index can see; indirect callers and callees are only as resolved as vcall.
"""
import bisect
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
import qseen  # noqa: E402

SRC = os.path.dirname(os.path.dirname(HERE))      # the checkout this file lives in (db/*.json, source hooks)
HUB = 50
RANK = {'high': 3, 'medium': 2, 'low': 1, 'unknown': 0}


def ro(name):
    return qnames.ro(os.path.join(qnames.IDX, name))


def compute(k=32, use_names=True):
    t0 = time.time()
    ix = ro('index.db')
    meta = dict(ix.execute('select key,value from meta'))
    code_lo, code_hi = json.loads(meta['code_range'])
    funcs, callers, callees = qunits.load_graph(ix)
    addrs = sorted(funcs)
    pos = {a: i for i, a in enumerate(addrs)}
    n = len(addrs)
    n_all = ix.execute('select count(*) from funcs').fetchone()[0]

    def owner(x):
        i = bisect.bisect_right(addrs, x) - 1
        return addrs[i] if i >= 0 else None

    fstr = collections.defaultdict(set)
    for f, a in ix.execute('select func,addr from strrefs'):
        fstr[f].add(a)
    fimm = collections.defaultdict(set)
    code_taken = set()
    for f, v in ix.execute('select func,value from imms'):
        if v in funcs:
            code_taken.add(v)
        elif abs(v) >= 0x100:
            fimm[f].add(v)
    P = dict(ix.execute("select at,value from ptrs where kind='func'"))
    SP = set(at for at, in ix.execute("select at from ptrs where kind='str'"))
    ptab = collections.defaultdict(set)
    for at, v in P.items():
        if v not in funcs:
            continue
        run = (at - 4) in P or (at + 4) in P
        if at < code_lo and not run and (((at + 8) in P and P[at + 8] > v) or ((at - 8) in P and P[at - 8] < v)) and at >= 0x821C0000:
            kind = 'pdata'
        elif (at - 4) in SP or (at + 4) in SP:
            kind = 'beside_str'
        elif at > code_hi:
            kind = 'data_rw'
        elif run:
            kind = 'rdata_run'
        else:
            kind = 'rdata_single'
        ptab[v].add(kind)
    data_taken = set(f for f, ks in ptab.items() if ks - {'pdata'})
    rtti = set(r[0] for r in ix.execute('select s.func from rtti_slot s join rtti_vtable v on s.vt=v.vt join rtti_class c on v.td=c.td'))
    vc = ro('vcall.s2.db')
    vres = collections.Counter()
    for func, cls in vc.execute('select func,cls from vcall'):
        if cls:
            vres[func] += 1
    nvc = vc.execute('select count(*) from vcall').fetchone()[0]
    fd = ro('fields.s2.db')
    roots = dict(fd.execute('select id,text from roots'))
    ffld = collections.defaultdict(set)
    for func, rw, off, root in fd.execute('select func,rw,off,root from rows'):
        ffld[func].add((roots.get(root, '?'), off, rw))
    vt_slot = set(r[0] for r in fd.execute('select distinct func from slots') if r[0] in funcs)

    db, db_rows, db_nonfunc = {}, 0, 0
    for f in glob.glob(os.path.join(SRC, 'db', '*.json')):
        for e in json.load(open(f, encoding='utf-8')):
            db_rows += 1
            try:
                a = int(e['address'], 16)
            except Exception:
                continue
            if a not in funcs:
                db_nonfunc += 1
                continue
            c = e.get('confidence', 'unknown')
            if a not in db or RANK.get(c, 0) > RANK.get(db[a], 0):
                db[a] = c

    import qx6
    src_hooks, midasm = qx6.scan_hooks(SRC)
    hook_f, mid_f = set(), set()
    for h in src_hooks:
        if h.get('tools'):
            continue
        m = re.search(r'([0-9A-Fa-f]{8})', str(h['target']))
        if m and int(m.group(1), 16) in funcs:
            hook_f.add(int(m.group(1), 16))
    for h in midasm:
        try:
            o = owner(int(h.get('address', ''), 16))
        except ValueError:
            o = None
        if o:
            mid_f.add(o)

    mech = collections.Counter()
    full = 0
    for a in addrs:
        nb = funcs[a][2]
        taken = a in data_taken or a in code_taken or a in vt_slot
        cs = 'complete' if (callers[a] and not taken) else ('partial' if taken else 'none')
        mech['callers ' + cs] += 1
        ce = 'complete' if (nb == 0 or vres[a] >= nb) else 'partial'
        mech['callees ' + ce] += 1
        if cs == 'complete' and ce == 'complete':
            full += 1
        mech['fields: has non-stack rows'] += a in ffld
        mech['strings: refs >= 1'] += a in fstr
        mech['consts: imm >= 0x100'] += a in fimm
    mech['module known (units are clusters, no module table)'] = 0

    # S110 SEEN: observed indirect callers (never changes the static rule above; a count of what runs have shown)
    obs = None
    try:
        sc = qseen.open_seen()
        reached = qseen.targets_with_callers(sc) & set(funcs)
        sites_st = qseen.static_sites(qseen.vdb_file())
        obs = {'runs': qseen.n_runs(sc), 'funcs_with_observed_caller': len(reached),
               'funcs_not_complete_with_observed_caller': sum(1 for a in reached if not (callers[a] and not (a in data_taken or a in code_taken or a in vt_slot))),
               'sites_observed': len(qseen.sites_seen(sc) & set(sites_st)), 'sites_static': len(sites_st),
               'data_word_only_now_observed': sum(1 for a in reached if a in data_taken and not callers[a])}
    except (qseen.SeenError, sqlite3.Error):
        pass

    units = qunits.compute_units(addrs, callees, k)
    unit_of = [0] * n
    for u, (i, j) in enumerate(units):
        for x in range(i, j):
            unit_of[x] = u
    multi = sorted(j - i for i, j in units if j - i >= 3)

    binding = set(f for f, ks in ptab.items() if 'beside_str' in ks)
    game_named = set()
    if use_names:
        game_named = set(r[0] for r in qnames.open_names().execute('select distinct func from names where source!="fn_string_shared"')) & set(funcs)
    anchors = set(hook_f) | mid_f | set(a for a, c in db.items() if c == 'high') | set(fstr) | rtti | binding | game_named
    named = set(db)
    hop1 = set()
    for a in anchors:
        for m in callers[a] | callees[a]:
            if len(callers[m]) <= HUB:
                hop1.add(m)
    hop2 = set()
    for a in hop1:
        for m in callers[a] | callees[a]:
            if len(callers[m]) <= HUB:
                hop2.add(m)
    unit_has_anchor = set(unit_of[pos[a]] for a in anchors)
    meaning = collections.Counter()
    for a in addrs:
        if a in named:
            kk = '0a named (a db entry)'
        elif a in anchors:
            kk = '0b anchor, no db entry'
        elif a in hop1:
            kk = '1 one call hop from an anchor'
        elif a in hop2:
            kk = '2 two hops'
        elif unit_of[pos[a]] in unit_has_anchor:
            kk = '3 same unit as an anchor'
        else:
            kk = '4 none'
        meaning[kk] += 1

    tail = collections.Counter()
    for a in addrs:
        ks = ptab.get(a, set())
        if funcs[a][1] <= 4:
            kk = 'A tiny <= 4 insns'
        elif a in vt_slot or 'rdata_run' in ks:
            kk = 'B virtual'
        elif 'beside_str' in ks:
            kk = 'C data-driven: beside a name string'
        elif 'data_rw' in ks or 'rdata_single' in ks:
            kk = 'D data-driven: callback in .data or lone rdata pointer'
        elif not callers[a] and a not in code_taken:
            kk = 'E orphan'
        elif not callees[a] and len(callers[a]) == 1 and a not in fstr:
            kk = 'F one-off leaf'
        else:
            kk = 'G ordinary'
        tail[kk] += 1

    return {
        'functions_rows': n_all, 'functions': n, 'names_counted': use_names, 'k': k,
        'mechanical': dict(mech), 'callers_and_callees_complete': full, 'observed': obs,
        'indirect': {'n_bctrl': sum(v[2] for v in funcs.values()), 'vcall_rows': nvc, 'exact_class': sum(vres.values())},
        'meaning': dict(meaning),
        'anchors': {'src_hook_funcs': len(hook_f), 'midasm_funcs': len(mid_f), 'db_rows': db_rows, 'db_funcs': len(db),
                    'db_nonfunc_rows': db_nonfunc, 'db_high': sum(1 for c in db.values() if c == 'high'),
                    'string_ref_funcs': len(fstr), 'rtti_slot_funcs': len(rtti), 'binding_funcs': len(binding),
                    'game_named_funcs': len(game_named), 'union': len(anchors)},
        'units': {'n_units': len(units), 'n_multi': len(multi), 'funcs_in_multi': sum(multi),
                  'median': multi[len(multi) // 2], 'p90': multi[len(multi) * 9 // 10], 'max': multi[-1]},
        'tail': dict(tail), 'seconds': round(time.time() - t0, 1)}


def main(argv):
    k = int(argv[argv.index('--k') + 1]) if '--k' in argv else 32
    r = compute(k, '--no-names' not in argv)
    if '--json' in argv:
        print(json.dumps(r, indent=1, sort_keys=True))
        return 0
    n = r['functions']

    def pct(v):
        return '%6d (%4.1f %%)' % (v, 100.0 * v / n)
    print('functions: %d rows, %d with a body; names %s; K=%d' % (r['functions_rows'], n, 'counted as anchors' if r['names_counted'] else 'NOT counted', r['k']))
    print('MECHANICAL:')
    for kk in sorted(r['mechanical']):
        print('  %-52s %s' % (kk, pct(r['mechanical'][kk])))
    print('  %-52s %s' % ('callers AND callees complete', pct(r['callers_and_callees_complete'])))
    i = r['indirect']
    print('  indirect sites: n_bctrl %d, vcall rows %d, with an exact class %d' % (i['n_bctrl'], i['vcall_rows'], i['exact_class']))
    o = r.get('observed')
    if o:
        tag = '(observed, %d runs)' % o['runs']
        print('  %-52s %s' % ('callers: has an observed indirect caller ' + tag, pct(o['funcs_with_observed_caller'])))
        print('  %-52s %s' % ('  of those, not callers-complete by the static rule', pct(o['funcs_not_complete_with_observed_caller'])))
        print('  %-52s %6d of %d (%4.1f %%)' % ('indirect sites: observed ' + tag, o['sites_observed'], o['sites_static'], 100.0 * o['sites_observed'] / max(1, o['sites_static'])))
        print('  %-52s %s' % ('data-word-only funcs, now an observed caller ' + tag, pct(o['data_word_only_now_observed'])))
        print('  observed = seen in %d runs, never a complete list; the static COMPLETE rule is unchanged.' % o['runs'])
    print('MEANING (hub = > %d callers):' % HUB)
    for kk in sorted(r['meaning']):
        print('  %-52s %s' % (kk, pct(r['meaning'][kk])))
    print('ANCHORS: ' + ', '.join('%s %d' % kv for kv in r['anchors'].items()))
    u = r['units']
    print('UNITS: %d units; %d with >= 3 funcs holding %d funcs; size median %d, p90 %d, max %d' % (u['n_units'], u['n_multi'], u['funcs_in_multi'], u['median'], u['p90'], u['max']))
    print('TAIL (first class wins):')
    for kk in sorted(r['tail']):
        print('  %-56s %s' % (kk, pct(r['tail'][kk])))
    print('%.1f s. %s' % (r['seconds'], 'May be incomplete: counts what the index can see.'))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except qnames.NamesError as e:
        print('q: %s' % e, file=sys.stderr)
        sys.exit(2)
