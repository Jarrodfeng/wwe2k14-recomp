r"""qpattern_rules2 selftest: for every kind of the third rule set, a planted function that must get it and a planted TWIN that has the one
fact the rule's false positive has and must not; the clone relations (and their twins); the hooks (qpattern.build writes the kinds and
pattern_rel, stats / show_one list them, the sheet tag); a missing fnfacts.s1.db (the rules report 0 and say why); the count text with its
third column and its precision lines. Synthetic facts only (invented addresses), no game code.
py -I tools/q/qpattern_rules2_selftest.py"""
import json
import os
import re
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qfnfacts  # noqa: E402
import qpattern  # noqa: E402
import qpattern_rules2  # noqa: E402

FAILS = []


def check(ok, what):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        FAILS.append(what)


FREE = 0x82F00000
VT = 0x82E00000


def synthetic(with_facts=True, with_vcall=True):
    d = tempfile.mkdtemp(prefix='qpatrules2_')
    G = {}
    funcs, edges, units, rows, ptrs = [], [], [], [], []
    fn_rows, inds, chains, cvals, abits, loops, icalls = [], [], [], [], [], [], []
    roots = {'a0': 1, 'a1': 2}
    nxt = [0x82100000]
    high = {'cymem_free_tagged_wrapper': FREE}

    def fn(name, size=200, outs=(), unit=1, slots=(), hashes=None, n_insns=10, at=None, high_name=None):
        a = at or nxt[0]
        if not at:
            nxt[0] += 0x100
        G[name] = a
        funcs.append((a, size, size // 4, 0, 0))
        units.append((a, unit))
        for i, o in enumerate(outs):
            edges.append((a, o, a + 4 * (i + 1)))
        for sl in slots:
            ptrs.append((a, sl))
        if hashes:
            fn_rows.append((a, n_insns, 0, 0, 0, 0, hashes[0], hashes[1], hashes[2], hashes[0]))
        if high_name:
            high[high_name] = a
        return a
    fn('free', 64, at=FREE)
    body = fn('body', 300)
    # destructors by their flag test
    for n, outs, slots, masks in (('sdd', (FREE, body), (0,), (1,)), ('sdd_noslot', (FREE, body), (), (1,)),
                                  ('vdd', (FREE,), (0,), (1, 2)), ('vdd_nofree', (body,), (0,), (1, 2))):
        a = fn(n, 100, outs, slots=slots, high_name='planted_scalar_deleting_dtor' if n == 'sdd' else None)
        abits.extend((a, 4, m, a + 8, 'cmp') for m in masks)
    # list walk: a link induction; its twin steps a pointer (an array walk). lw is a loner (unit 7) with a HIGH name the kind contradicts
    a = fn('lw', 200, unit=7, high_name='set_volume')
    inds.append((a, a + 16, 'r31', 'link', 8, None, None))
    a = fn('lw_arr', 200)
    inds.append((a, a + 16, 'r31', 'ptr', 8, 0, None))

    def chain(a, val, values, neq):
        chains.append((a, val, 'r11', a + 8, a + 8 + 8 * len(values), len(values), neq))
        cvals.extend((a, a + 8, a + 8 + 8 * i, v, 'eq' if neq else 'rng') for i, v in enumerate(values))
    # value tests
    chain(fn('avt', 40), 'arg:r4', (1, 2, 3), 3)
    chain(fn('avt_rng', 40), 'arg:r4', (1, 2, 3), 0)
    chain(fn('avt_big', 400), 'arg:r4', (1, 2, 3), 3)
    # S110 RULES2 review: a value test CHANGES NOTHING. Twins: the same chain in a function that stores a field (a setter by mode), makes a
    # virtual call (a handler per message), calls code that stores (an allocator behind an id guard) or loops (a walk); a positive
    # that calls only a side-effect-free helper (a predicate delegating part of its set) keeps the label
    pure_leaf = fn('pure_leaf', 40)
    writer = fn('writer', 40)
    rows.append((writer, writer + 4, 'W', 8, 'a0'))
    a = fn('avt_store', 40)
    rows.append((a, a + 4, 'W', 60, 'a0'))
    chain(a, 'arg:r4', (1, 2, 3), 3)
    a = fn('avt_icall', 40)
    icalls.append((a + 12, a))
    chain(a, 'arg:r4', (1, 2, 3), 3)
    chain(fn('avt_callwriter', 40, (writer,)), 'arg:r4', (1, 2, 3), 3)
    a = fn('avt_loop', 40)
    loops.append((a, a + 8, a + 20, 4, 0, 0))
    chain(a, 'arg:r4', (1, 2, 3), 3)
    chain(fn('avt_callpure', 40, (pure_leaf,)), 'arg:r3', (1, 2, 3), 3)
    for n, root, w in (('fvt', 'a0', False), ('fvt_a1', 'a1', False), ('fvt_store', 'a0', True)):
        a = fn(n, 60)
        rows.append((a, a + 4, 'R', 16, root))
        if w:
            rows.append((a, a + 8, 'W', 32, 'a0'))
        chain(a, 'def:%08X' % (a + 4), (5, 6, 7), 3)
    # FourCC tags; the twin compares three large constants that are not printable (HRESULTs)
    chain(fn('fcc', 300), 'ret:82100000', (0x41424344, 0x45464748, 0x494A4B4C), 3)
    chain(fn('fcc_hr', 300), 'ret:82100000', (-2147467259, -2147024809, -2147024882), 3)
    # clones: sib carries the HIGH name; caller1 calls sib, c_exact and c_nocall_caller
    sib = fn('sib', 120, slots=(4,), hashes=(11, 21, 31), high_name='named_sib')
    fn('c_exact', 120, hashes=(11, 21, 31))
    fn('c_noimm', 120, slots=(4,), hashes=(12, 21, 32))
    fn('c_noimm_alone', 120, hashes=(13, 21, 33))
    fn('c_nocall_slot', 120, slots=(4,), hashes=(14, 24, 31))
    fn('c_nocall_caller', 120, hashes=(15, 25, 31))
    t1 = fn('tiny_named', 12, hashes=(41, 42, 43), n_insns=3, high_name='tiny_named')
    t2 = fn('tiny_twin', 12, hashes=(41, 42, 43), n_insns=3)
    fn('caller1', 300, (sib, G['c_exact'], G['c_nocall_caller'], t1, t2))
    c = sqlite3.connect(os.path.join(d, 'index.db'))
    c.executescript('create table funcs(addr integer, name text, size integer, insns integer, has_body integer, n_bctrl integer, n_bctr integer);'
                    'create table edges(src integer, dst integer, target integer, site integer, kind text);'
                    'create table ptrs(at integer, value integer, kind text);create table tables(start integer primary key, rule text, len integer);')
    c.executemany('insert into funcs values (?,null,?,?,1,?,?)', funcs)
    c.executemany('insert into edges values (?,?,?,?,"call")', [(s, t, t, site) for s, t, site in edges])
    for i, (a, slot) in enumerate(ptrs):        # one table per slot: the table starts `slot` bytes before the pointer
        st = VT + 0x100 * (i + 1)
        c.execute('insert into tables values (?,?,?)', (st, 'code', slot // 4 + 1))
        c.execute('insert into ptrs values (?,?,"func")', (st + slot, a))
    c.commit()
    c.close()
    c = sqlite3.connect(os.path.join(d, 'fields.s2.db'))
    c.executescript('create table roots(id integer primary key, text text unique, glob integer);'
                    'create table rows(func integer, addr integer, rw text, width integer, fl integer, off integer, root integer);'
                    'create table vstores(func integer, root integer, vt integer);')
    c.executemany('insert into rows values (?,?,?,4,0,?,?)', [(a, ad, rw, off, roots[t]) for a, ad, rw, off, t in rows])
    c.executemany('insert into roots values (?,?,0)', [(i, t) for t, i in roots.items()])
    c.commit()
    c.close()
    c = sqlite3.connect(os.path.join(d, 'units.s1.db'))
    c.execute('create table units(func integer primary key, unit_id integer)')
    c.executemany('insert into units values (?,?)', units)
    c.commit()
    c.close()
    for n, sql in (('seen.s1.db', 'create table seen(site integer, func integer, target integer, runs integer)'),
                   ('names.s2.db', 'create table names(func integer, name text, source text, table_addr integer)')):
        c = sqlite3.connect(os.path.join(d, n))
        c.execute(sql)
        c.commit()
        c.close()
    if with_facts:
        c = sqlite3.connect(os.path.join(d, 'fnfacts.s1.db'))
        c.executescript(qfnfacts.SCHEMA_SQL)
        c.executemany('insert into fn values (?,?,?,?,?,?,?,?,?,?)', fn_rows)
        c.executemany('insert into loop_ind values (?,?,?,?,?,?,?)', inds)
        c.executemany('insert into chain values (?,?,?,?,?,?,?)', chains)
        c.executemany('insert into chain_val values (?,?,?,?,?)', cvals)
        c.executemany('insert into argbit values (?,?,?,?,?)', abits)
        c.executemany('insert into loop values (?,?,?,?,?,?)', loops)
        c.commit()
        c.close()
    if with_vcall:
        c = sqlite3.connect(os.path.join(d, 'vcall.s4.db'))
        c.execute('create table icall(site integer, func integer, kind text, shape text, off integer, recv text, status text, n integer)')
        c.executemany("insert into icall values (?,?,'vcall','V_this',8,'a0','open',1)", icalls)
        c.commit()
        c.close()
    os.makedirs(os.path.join(d, 'db'))
    json.dump([{'address': '0x%08X' % a, 'name': n, 'confidence': 'high'} for n, a in high.items()], open(os.path.join(d, 'db', 'h.json'), 'w'))
    return d, G


def main():
    d, G = synthetic()
    p = qpattern.paths(idx=d, dbdir=os.path.join(d, 'db'))
    out = qpattern.build(p, verbose=False)                # the hook: build runs all three rule sets

    def kinds(n):
        return sorted(k for k, _e in out.get(G[n], []) if k in qpattern_rules2.KINDS)
    for n, k in (('sdd', 'scalar_deleting_dtor'), ('vdd', 'vector_deleting_dtor'), ('lw', 'list_walk'), ('avt', 'arg_value_test'),
                 ('fvt', 'field_value_test'), ('fcc', 'fourcc_compare')):
        check(kinds(n) == [k], '%s -> %s (got %s)' % (n, k, kinds(n)))
    for n in ('sdd_noslot', 'vdd_nofree', 'lw_arr', 'avt_rng', 'avt_big', 'fvt_a1', 'fcc_hr', 'body', 'sib'):
        check(kinds(n) == [], 'twin %s has no new label (got %s)' % (n, kinds(n)))
    ev = dict(out[G['fcc']])
    check("'ABCD' 'EFGH' 'IJKL'" in ev.get('fourcc_compare', ''), 'fourcc_compare carries the tags (got %r)' % ev.get('fourcc_compare'))
    check('this+16' in dict(out[G['fvt']]).get('field_value_test', ''), 'field_value_test names the field')
    # relations
    rel = qpattern_rules2.relations_of(p['out'])
    want = {'c_exact': ('clone_exact', 'sib'), 'c_noimm': ('shape_consts', 'sib'), 'c_nocall_slot': ('shape_callee', 'sib')}
    check(kinds('avt_callpure') == ['arg_value_test'], 'a value test calling only a side-effect-free helper keeps its label (got %s)' % kinds('avt_callpure'))
    for n in ('avt_store', 'avt_icall', 'avt_callwriter', 'avt_loop', 'fvt_store'):
        check(kinds(n) == [], 'twin %s (stores / virtual call / calls a writer / loops) is no value test (got %s)' % (n, kinds(n)))
    for n, (r, s) in want.items():
        got = [(x[0], x[1]) for x in rel.get(G[n], [])]
        check(got == [(r, G[s])], '%s -> %s of %s (got %s)' % (n, r, s, got))
    for n in ('c_noimm_alone', 'c_nocall_caller', 'tiny_twin', 'sib'):
        check(G[n] not in rel, 'twin %s has no relation (got %s)' % (n, rel.get(G[n])))
    check(qpattern_rules2.rel_tag(rel, G['c_noimm']) == '[~shape:consts %08X]' % G['sib'], 'the sheet tag of a relation says "shape", not "clone"')
    check(qpattern_rules2.rel_tag(rel, G['c_exact']) == '[~clone %08X]' % G['sib'], 'the sheet tag of a byte-identical clone')
    one = qpattern.show_one('%08X' % G['c_noimm'], p['out'])
    check('[~shape_consts]' in one and 'named_sib' in one and 'shared vtable slot +4' in one and 'same shape' in one,
          'q pattern FUNC prints the relation and its basis (got %r)' % one)
    st = qpattern.stats(p['out'])
    check('list_walk' in st and '~shape_consts' in st, 'q pattern --stats lists the new kinds and relations')
    lab = qpattern.labels(p['out'])
    check('list_walk' in lab.get(G['lw'], []), 'the new kind is in pattern.s1.db (the sheet tag reads it)')
    # count: three columns; loners = unit 7 (lw): 1 function, 0 / 0 / 1 labelled; precision lines
    txt = qpattern_rules2.count(p)
    loner = [ln for ln in txt.splitlines() if 'loners' in ln][0]
    check(re.findall(r'(?<![\d.])\d+(?![\d.%])', loner.split('1-2)')[1]) == ['1', '0', '0', '1'],
          'loner counts: 1 function, 0 before round 1, 0 after it, 1 after round 2 (got %r)' % loner)
    sdl = [ln for ln in txt.splitlines() if ln.strip().startswith('scalar_deleting_dtor')][0]
    check('HIGH     1 agree     1 contradicted    0' in sdl, 'precision: the scalar dtor HIGH name agrees (got %r)' % sdl)
    lwl = [ln for ln in txt.splitlines() if ln.strip().startswith('list_walk')][0]
    check('HIGH     1 agree     0 contradicted    1' in lwl and 'set_volume' in txt, 'precision: a contradicting HIGH name is counted and listed (got %r)' % lwl)
    # no fnfacts.s1.db: the rules report 0 and the unresolved list names the file
    d2, G2 = synthetic(with_facts=False)
    p2 = qpattern.paths(idx=d2, dbdir=os.path.join(d2, 'db'))
    f2 = qpattern.load(p2)
    out2 = qpattern_rules2.extend(f2, qpattern.classify(f2), p2)
    check(any(a == 'fnfacts' for a, _ in f2.unresolved) and not any(k in qpattern_rules2.KINDS for v in out2.values() for k, _e in v)
          and not f2.prel, 'no fnfacts.s1.db: no label, no relation, and the unresolved list says why')
    # no vcall.s4.db: side effects cannot be ruled out, so no value test; the unresolved list names the file
    d3, G3 = synthetic(with_vcall=False)
    p3 = qpattern.paths(idx=d3, dbdir=os.path.join(d3, 'db'))
    f3 = qpattern.load(p3)
    out3 = qpattern_rules2.extend(f3, qpattern.classify(f3), p3)
    vt3 = [k for v in out3.values() for k, _e in v if k in ('arg_value_test', 'field_value_test')]
    check(any(a == 'vcall' for a, _ in f3.unresolved) and not vt3 and 'list_walk' in [k for k, _e in out3.get(G3['lw'], [])],
          'no vcall.s4.db: no value test (got %d), the other kinds stand, and the unresolved list says why' % len(vt3))
    check(qpattern_rules2.printable(0x41424344) and not qpattern_rules2.printable(-2147467259) and not qpattern_rules2.printable(0x4142),
          'printable: four printable bytes, not an HRESULT, not a 2-byte value')
    print('%d failed' % len(FAILS) if FAILS else 'all ok')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
