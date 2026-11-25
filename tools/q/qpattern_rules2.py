r"""q pattern rules, third set (S110 lane RULES2): kinds that need the STRUCTURAL facts of tools/q/qfnfacts.py (fnfacts.s1.db: loop inductions,
compare chains, entry-argument bit tests, clone hashes). qpattern.build() calls extend() after the first two sets: the purposes go into the same
table (pattern.s1.db pattern_fn), the CLONE / SHAPE RELATIONS into their own table (pattern_rel: func, rel, other, evidence), which q sheet
prints as ` [~clone ADDR]` / ` [~shape:consts ADDR]` / ` [~shape:callee ADDR]` (only on a function with no HIGH db name) and `q pattern FUNC`
prints whole. A label is a KIND ("looks like"), never a name:
never db\*.json, never [g], never a cover anchor. A missing fnfacts.s1.db makes these rules report 0 (the unresolved list says so).

    py tools/q/q.py pattern --count     functions with a label: before round one (qpattern.KINDS) / after round one (+ qpattern_rules.KINDS) /
                                        after round two (+ KINDS below), all functions and loners; then per new kind and relation: labelled,
                                        loners, new, with a HIGH db name, agreeing, contradicted, precision; and the first contradicted names

Kinds (predicate; the false positive its selftest twin plants):
  scalar_deleting_dtor  a vtable slot, tests bit 0 of its entry r4 (fnfacts argbit arg 4 mask 1), calls cymem_free_tagged_wrapper, does not
                        test bit 1. FP: the same with no vtable slot (a function that frees by a flag argument).
  vector_deleting_dtor  the same and also tests bit 1 of r4 (the array form). FP: tests r4 & 1 and r4 & 2 but frees nothing (d3d_begin_tiling).
  list_walk             a loop with a link induction (rX = lwz K(rX), its only write in the body): "follows the link at +K".
                        FP: a pointer stepped by addi (an array walk, not a list).
  arg_value_test        <= TEST_MAX bytes, a compare chain (fnfacts chain) on the ENTRY value of an argument with >= 3 equality tests, in a
                        function that CHANGES NOTHING (side_effect_free: no field store, no indirect call, every callee the same) and has no
                        loop: "compares rN with v1 v2 ...". FP: a chain read only by range branches (n_eq < 3: a bounds tree); the same chain
                        in a large function (the test is one step of it, not its purpose); the same chain in a setter by mode, a message
                        handler, an id-guarded allocator or a walk (a store / a virtual call / a storing callee / a loop: the chain picks what
                        the function DOES, it is not what it is. S110 review: 8 of 108 HIGH names were that, 1 of 97 after).
  field_value_test      the same on a value loaded from this (the chain's def site is a field load row on root a0): "compares this+N with ...".
                        FP: the def site loads a field of param 1 (root a1); the same with a store (S110 review: 10 of 37 HIGH names did work
                        selected by the field, 0 of 23 after). A missing vcall.s4.db makes both report 0 (side effects cannot be ruled out).
  fourcc_compare        a chain with >= 3 distinct printable 4-byte constants: the label carries the tags and claims nothing more than the
                        chain fact. FP: 3 large constants that are not printable (HRESULTs).
Relations (pattern_rel, never a purpose; the sibling is a hint for a reader, not a name):
  clone_exact           the function's fnfacts hash h_exact equals that of a sibling with a HIGH db name (both >= MIN_INSNS instructions),
                        and the two share a vtable column (one slot offset) or a direct caller: byte-identical code, the same job.
  shape_consts          the same on h_noimm: the same SHAPE with other constants. NOT a clone (S110 review: in 30 of 30 read pairs the
                        constants were the meaning: another component tag, field, vtable slot, class or record count), so the sibling's
                        name tells the template, never the function's own name.
  shape_callee          the same on h_nocall, and only through a shared vtable column: under one caller, the callee is what tells two
                        such siblings apart (measured: 40 of 44 caller-only pairs share a name word, 53 of 56 slot pairs). The same
                        shape with another callee (the 860 deleting dtors of the h_nocall template differ by their class's body).
                        The strongest relation is kept (exact, then consts, then callee). FP: the same hash with no shared slot or caller (a
                        template reused in an unrelated place); two bodies under MIN_INSNS instructions (blr / one-branch thunks).
Dropped / not built (docs\research\s110\RULES2.md): a loop calling a vtable slot of each element (103 functions, 2 HIGH names: cannot be
measured; a loop-carried receiver does not prove it is the element); a fixed-count init loop (88.8 % on 107 HIGH names: copies whose loads
are indexed pass, since fields rows hold D-form accesses only); a SWITCH on a field of this or on an argument (fnfacts switch rows carry the
index register, not its value id: a `val` column like chain.val would make it a rule); FourCC tags as names.
"""
import collections
import glob
import json
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402

KINDS = ('scalar_deleting_dtor', 'vector_deleting_dtor', 'list_walk', 'arg_value_test', 'field_value_test', 'fourcc_compare')
RELS = ('clone_exact', 'shape_consts', 'shape_callee')
RELTEXT = {'clone_exact': 'the same instructions as',
           'shape_consts': 'the same shape with other constants (so another field / tag / slot: the meaning differs by them) as',
           'shape_callee': 'the same shape with other callee(s) as'}
RELTAG = {'clone_exact': 'clone', 'shape_consts': 'shape:consts', 'shape_callee': 'shape:callee'}
MIN_INSNS = 8                    # clone groups: smaller bodies (blr, one-branch thunks) form huge groups that say nothing
TEST_MAX = 256                   # arg_value_test / field_value_test: a function this small is the test; a larger one only holds it
# precision: a HIGH db name AGREES with a kind when it holds one of these words (lower case, substring); every other HIGH name is
# counted CONTRADICTED. Written before the first measured run and not widened after it.
AGREE = {
    'scalar_deleting_dtor': r'dtor|destructor|destroy|delete',
    'vector_deleting_dtor': r'dtor|destructor|destroy|delete',
    'list_walk': r'list|chain|node|link|hash|bucket|find|count|each|_all|all_|any|last|first|tail|head|unlink|insert|remove|search|lookup|'
                 r'walk|next|prev|sum|collect|queue|pool|entr|item|sorted|append|child|tree|parent|sibling',
    'arg_value_test': r'is_|_is|in_|_in|set|one_of|_or_|pred|map|select|by_|switch|classify|code|id|value|range|kind|type|mode|match|check|'
                      r'test|lookup|state|flag|compare|cmp|which|index|valid|has_',
    'field_value_test': r'is_|_is|in_|_in|set|one_of|_or_|pred|map|select|by_|switch|classify|code|id|value|range|kind|type|mode|match|check|'
                        r'test|lookup|state|flag|compare|cmp|which|index|valid|has_|field|word|byte|half|hw',
    'fourcc_compare': r'tag|fourcc|chunk|type|magic|header|parse|read|open|find|component|kind|bink',
}


def ro(path):
    path = path and qnames.newest(path)
    if not path or not os.path.exists(path):
        raise qnames.NamesError('no file %s' % path)
    return sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)


def fnfacts_path(p):
    """fnfacts.s1.db beside the index (p may name another: 'fnfacts')"""
    return p.get('fnfacts') or os.path.join(os.path.dirname(p['index']), 'fnfacts.s1.db')


def vcall_path(p):
    """vcall.s<N>.db (qx7: the indirect-call sites, `icall.func`) beside the index (p may name another: 'vcall')"""
    import qx7
    return p.get('vcall') or os.path.join(os.path.dirname(p['index']), qx7.db_name())


def printable(v):
    v &= 0xFFFFFFFF
    return v > 0xFFFF and all(0x20 <= (v >> s) & 0xFF < 0x7F for s in (24, 16, 8, 0))


def tag4(v):
    return (v & 0xFFFFFFFF).to_bytes(4, 'big').decode('ascii')


class Extra2:
    """the facts this rule set reads on top of qpattern.Facts"""

    def __init__(self):
        self.links = collections.defaultdict(list)       # func -> [(head, step)] link inductions
        self.argbit = collections.defaultdict(set)       # func -> {(arg, mask)}
        self.chains = collections.defaultdict(list)      # func -> [(val, n, n_eq, [values])]
        self.hash = {}                                   # func -> (n_insns, h_exact, h_noimm, h_nocall)
        self.thisload = {}                               # instruction address -> this+off it loads (root a0, rw R), chain def sites only
        self.loops = set()                               # functions with a loop (fnfacts loop)
        self.stores = set()                              # functions with a field store (fields.s2.db W row, any root)
        self.icall = None                                # functions with an indirect call (vcall icall.func); None = not known
        self.missing = []                                # [(alias, [path])] a fact file that is not there: its rules report 0


def load_extra(f, p):
    x = Extra2()
    ffx = fnfacts_path(p)
    if not os.path.exists(qnames.newest(ffx)):
        x.missing.append(('fnfacts', [ffx]))
        return x
    c = ro(ffx)
    for a, h, st in c.execute("select func,head,step from loop_ind where kind='link' order by func,head"):
        x.links[a].append((h, st))
    for a, arg, mask in c.execute('select func,arg,mask from argbit'):
        x.argbit[a].add((arg, mask))
    vals = collections.defaultdict(list)
    for a, first, v in c.execute('select func,first,value from chain_val order by site'):
        vals[(a, first)].append(v)
    for a, val, first, n, neq in c.execute('select func,val,first,n,n_eq from chain order by func,first'):
        x.chains[a].append((val, n, neq, vals.get((a, first), [])))
    for a, n, he, hn, hc in c.execute('select func,n_insns,h_exact,h_noimm,h_nocall from fn'):
        x.hash[a] = (n, he, hn, hc)
    x.loops = set(a for (a,) in c.execute('select distinct func from loop'))
    c.close()
    vp = vcall_path(p)
    if os.path.exists(qnames.newest(vp)):
        vc = ro(vp)
        x.icall = set(a for (a,) in vc.execute('select distinct func from icall'))
        vc.close()
    else:
        x.missing.append(('vcall', [vp]))
    defs = set(int(v[4:], 16) for ch in x.chains.values() for v, _n, _q, _vs in ch if v.startswith('def:'))
    fd = ro(p['fields'])
    a0 = [i for i, t in fd.execute('select id,text from roots') if t == 'a0']
    for addr, off in fd.execute("select addr,off from rows where rw='R' and root=?", (a0[0] if a0 else -1,)):
        if addr in defs:
            x.thisload[addr] = off
    x.stores = set(a for (a,) in fd.execute("select distinct func from rows where rw='W'"))
    fd.close()
    return x


def side_effect_free(f, x):
    """-> {func}: no field store, no indirect call, and every direct callee the same (over the whole call graph: a function that calls
    one that is not free, or one with no body, is not free). Empty when the indirect calls are not known. MAY BE INCOMPLETE the other
    way: an indexed store (stwx) is not a fields row, so a function storing only through one passes."""
    if x.icall is None:
        return set()
    free = set(a for a in f.size if a not in x.stores and a not in x.icall)
    callers = collections.defaultdict(set)
    for s, ds in f.outs.items():
        for d in ds:
            callers[d].add(s)
    todo = [a for a in free if any(d not in free for d in f.outs.get(a, ()))]
    while todo:
        a = todo.pop()
        if a in free:
            free.discard(a)
            todo.extend(s for s in callers.get(a, ()) if s in free)
    return free


def extend(f, out, p, x=None):
    """add this rule set's purposes to `out` ({func: [(kind, evidence)]}) and its clone relations to f.prel ([(func, rel, other, evidence)])"""
    x = x or load_extra(f, p)
    for alias, ns in x.missing:
        if alias not in [u[0] for u in f.unresolved]:
            f.unresolved.append((alias, ns))
    free = f.helper.get('free', set())
    pure = side_effect_free(f, x)
    for a in f.size:
        lab = []
        offs = sorted(set(o for _vt, o in f.vslot.get(a, [])))
        ab = x.argbit.get(a, set())
        # destructors by their flag test
        if (4, 1) in ab and offs and free and f.outs.get(a, set()) & free:
            kind = 'vector_deleting_dtor' if (4, 2) in ab else 'scalar_deleting_dtor'
            lab.append((kind, 'slot +%s, tests r4 & 1%s, calls free' % ('/+'.join(str(o) for o in offs[:3]), ' and r4 & 2' if (4, 2) in ab else '')))
        # a list walk: the first loop with a link induction
        if a in x.links:
            head, st = x.links[a][0]
            lab.append(('list_walk', 'loop @%08X follows the link at +%d' % (head, st)))
        # compare chains: one label per kind, from the first chain that has it
        got = set()
        for val, _n, neq, vs in x.chains.get(a, []):
            tags = sorted(set(tag4(v) for v in vs if printable(v)))
            if len(tags) >= 3:
                if 'fourcc_compare' not in got:
                    lab.append(('fourcc_compare', 'compares %s with tags %s' % (val, ' '.join("'%s'" % t for t in tags[:8]))))
                    got.add('fourcc_compare')
                continue
            if neq < 3 or f.size[a] > TEST_MAX or a not in pure or a in x.loops:
                continue                 # a value test is small, changes nothing and does not loop (S110 RULES2 review)
            shown = ' '.join(str(v) for v in sorted(set(vs))[:8]) + (' ...' if len(set(vs)) > 8 else '')
            if val.startswith('arg:') and 'arg_value_test' not in got:
                lab.append(('arg_value_test', 'compares entry %s with %d constants (%d equality tests): %s' % (val[4:], len(set(vs)), neq, shown)))
                got.add('arg_value_test')
            elif val.startswith('def:') and int(val[4:], 16) in x.thisload and 'field_value_test' not in got:
                lab.append(('field_value_test', 'compares this+%d with %d constants (%d equality tests): %s' % (x.thisload[int(val[4:], 16)], len(set(vs)), neq, shown)))
                got.add('field_value_test')
        if lab:
            out[a].extend(lab)
    f.prel = relations(f, x)
    return out


def relations(f, x):
    """-> [(func, rel, other, evidence)]: one row per function, the strongest clone relation to a HIGH-named sibling in one role"""
    hi = {a: n for a, lst in f.names.items() for n, s in lst if s == 'db:high'}
    callers = collections.defaultdict(set)
    for s, ds in f.outs.items():
        for d in ds:
            callers[d].add(s)
    offs = {a: set(o for _vt, o in v) for a, v in f.vslot.items()}
    rows, done = [], set()
    for k, rel in ((1, 'clone_exact'), (2, 'shape_consts'), (3, 'shape_callee')):
        groups = collections.defaultdict(list)
        for a, h in x.hash.items():
            if h[0] >= MIN_INSNS and a in f.size:
                groups[h[k]].append(a)
        for g in groups.values():
            named = sorted(a for a in g if a in hi)
            if len(g) < 2 or not named:
                continue
            for m in sorted(g):
                if m in done:
                    continue
                for s in named:
                    if s == m:
                        continue
                    so = offs.get(m, set()) & offs.get(s, set())
                    sc = set() if rel == 'shape_callee' else callers.get(m, set()) & callers.get(s, set())
                    if so or sc:
                        why = ('shared vtable slot +%d' % min(so)) if so else ('shared caller %08X' % min(sc))
                        rows.append((m, rel, s, '%s %s (%08X; group of %d; %s)' % (RELTEXT[rel], hi[s], s, len(g), why)))
                        done.add(m)
                        break
    return rows


def write_rel(w, f):
    """qpattern.write() hook: the relation table beside pattern_fn"""
    w.executescript('create table pattern_rel(func integer, rel text, other integer, evidence text); create index pr_func on pattern_rel(func);')
    w.executemany('insert into pattern_rel values (?,?,?,?)', getattr(f, 'prel', []))


def relations_of(path=None):
    """{func: [(rel, other, evidence)]} from pattern.s1.db; {} when the file or the table is not there (a db built before RULES2)"""
    path = path or os.path.join(qnames.IDX, 'pattern.s1.db')
    if not os.path.exists(qnames.newest(path)):
        return {}
    c = ro(path)
    try:
        d = collections.defaultdict(list)
        for a, r, o, e in c.execute('select func,rel,other,evidence from pattern_rel order by func'):
            d[a].append((r, o, e))
        return dict(d)
    except sqlite3.Error:
        return {}
    finally:
        c.close()


def rel_tag(rel, a):
    """the sheet tag of a relation: `[~clone 8xxxxxxx]` / `[~shape:consts 8xxxxxxx]` / `[~shape:callee 8xxxxxxx]` (the sibling's address;
    `q pattern FUNC` prints its name and the evidence)"""
    v = rel.get(a)
    return '[~%s %08X]' % (RELTAG.get(v[0][0], v[0][0]), v[0][1]) if v else ''


def show_rel(a, path=None):
    """q pattern FUNC: the relation lines of one function"""
    return ['sub_%08X [~%s] %s' % (a, r, e) for r, _o, e in relations_of(path).get(a, [])]


def rel_stats(path=None):
    """q pattern --stats: one line per relation"""
    by = collections.defaultdict(list)
    for a, v in sorted(relations_of(path).items()):
        for r, _o, _e in v:
            by[r].append(a)
    return ['%-20s %7d  %s' % ('~' + r, len(by[r]), ' '.join('%08X' % a for a in by[r][:5]) or '-') for r in RELS]


def high_names(dbdir):
    """{addr: name} of the HIGH entries of db/*.json"""
    best = {}
    for fp in glob.glob(os.path.join(dbdir, '*.json')):
        try:
            ents = json.load(open(fp, encoding='utf-8'))
        except (ValueError, OSError):
            continue
        for e in ents:
            try:
                a = int(e['address'], 16)
            except (KeyError, ValueError, TypeError):
                continue
            if e.get('confidence') == 'high':
                best.setdefault(a, e.get('name') or '?')
    return best


def name_words(name):
    """qsheet.name_words: the words of 4+ letters of a name (camel case split, lower case)"""
    tail = re.split(r'::|\.', name or '')[-1]
    tail = re.sub(r'([a-z])([A-Z])', r'\1 \2', tail)
    return set(w.lower() for w in re.findall(r'[A-Za-z]{4,}', tail))


def count(p, show=4):
    """-> text: functions with a purpose label before round one / after round one / after round two (all + loners); per new kind and
    relation: labelled, loners, new (no earlier label), with a HIGH db name, agreeing, contradicted, precision; the first contradicted names"""
    import qpattern
    import qpattern_rules
    c = ro(p['out'])
    by = collections.defaultdict(set)
    for a, k in c.execute('select func,kind from pattern_fn'):
        by[k].add(a)
    rel = collections.defaultdict(dict)
    try:
        for a, r, o in c.execute('select func,rel,other from pattern_rel'):
            rel[r][a] = o
    except sqlite3.Error:
        pass
    c.close()
    unit = dict(ro(p['units']).execute('select func,unit_id from units'))
    n = collections.Counter(unit.values())
    allf = set(unit)
    lon = set(a for a, u in unit.items() if n[u] <= 2)
    r0 = set().union(*[by.get(k, set()) for k in qpattern.KINDS])
    r1 = r0.union(*[by.get(k, set()) for k in qpattern_rules.KINDS])
    r2 = r1.union(*[by.get(k, set()) for k in KINDS])
    lines = ['functions with a pattern label (a derived purpose, 0 tokens): round 0 = qpattern.KINDS, round 1 = + qpattern_rules.KINDS, '
             'round 2 = + qpattern_rules2.KINDS (relations are not purposes and are not counted here)',
             '  %-24s %7s %16s %16s %16s' % ('', 'funcs', 'before round 1', 'after round 1', 'after round 2')]
    for name, s in (('all functions', allf), ('loners (units of 1-2)', lon)):
        lines.append('  %-24s %7d' % (name, len(s)) + ''.join(' %7d (%5.1f%%)' % (len(x & s), 100.0 * len(x & s) / max(1, len(s))) for x in (r0, r1, r2)))
    hi = high_names(p['dbdir'])
    lines.append('per new kind: labelled, loners, new (no round 0/1 label), with a HIGH db name, agree (AGREE words), contradicted, precision')
    bad = []
    for k in KINDS:
        s = by.get(k, set()) & allf
        h = [a for a in sorted(s) if a in hi]
        ag = set(a for a in h if re.search(AGREE[k], hi[a].lower()))
        con = [a for a in h if a not in ag]
        lines.append('  %-22s %6d %6d %6d   HIGH %5d agree %5d contradicted %4d  %s' % (k, len(s), len(s & lon), len(s - r1), len(h), len(ag), len(con),
                                                                                    ('%5.1f%%' % (100.0 * len(ag) / len(h))) if h else '  n/a'))
        bad.extend((k, a, hi[a]) for a in con[:show])
    lines.append('per relation (pattern_rel): functions, loners, judged = own and sibling HIGH names both with a word of 4+ letters, agree = they '
                 'share one, contradicted = they share none (qsheet CONFLICT rule)')
    for r in RELS:
        m = rel.get(r, {})
        h = [a for a in sorted(m) if a in hi and m[a] in hi and name_words(hi[a]) and name_words(hi[m[a]])]
        ag = set(a for a in h if name_words(hi[a]) & name_words(hi[m[a]]))
        con = [a for a in h if a not in ag]
        lines.append('  %-22s %6d %6d          judged %3d agree %5d contradicted %4d  %s' % (r, len(m), len(set(m) & lon), len(h), len(ag), len(con),
                                                                                       ('%5.1f%%' % (100.0 * len(ag) / len(h))) if h else '  n/a'))
        bad.extend((r, a, '%s  vs  %s' % (hi[a], hi[m[a]])) for a in con[:show])
    lines.append('first contradicted (up to %d per kind):' % show)
    lines.extend('  %-22s %08X %s' % (k, a, nm[:110]) for k, a, nm in bad)
    return '\n'.join(lines)
