r"""qpattern_rules selftest: for every kind of the second rule set, a planted function that must get it and a planted TWIN that has the
one fact the rule's false positive has and must not; then the hook (qpattern.build writes the new kinds into pattern.s1.db, stats lists
them), the unresolved-helper report, the count text, and the real pattern db (SKIPPED when it holds none of the new kinds).
py -I tools/q/qpattern_rules_selftest.py"""
import json
import os
import re
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qpattern  # noqa: E402
import qpattern_rules  # noqa: E402

FAILS = []


def check(ok, what):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        FAILS.append(what)


FREE, FIND, UI, HAL, HSZ = 0x82F00000, 0x82F00010, 0x82F00020, 0x82F00030, 0x82F00040
PICK, ALLOC = 0x82C2E1E0, 0x82C2DCD8                    # the helpers this rule set names by address
VT = 0x82E00000
VTABLES = (0x82001000, 0x82002000)                      # the planted vtables (fields ctors holds only stores of a real vtable)


def synthetic(drop=()):
    d = tempfile.mkdtemp(prefix='qpatrules_')
    G = {}
    funcs, edges, units, rows, ptrs, vst = [], [], [], [], [], []
    roots = {'a0': 1, 'a1': 2, 'K': 3, 'u': 4}
    nxt = [0x82100000]

    def rid(t):
        if t not in roots:
            roots[t] = len(roots) + 1
        return roots[t]

    def fn(name, size, outs=(), unit=1, slot=None, rws=(), ind=(0, 0), insns=None, vt=None, res=(), at=None):
        """rws: (rw, off, root text); res: (callee, rw, off) = a field of the result of the call to callee"""
        a = at or nxt[0]
        if not at:
            nxt[0] += 0x100
        G[name] = a
        funcs.append((a, size, insns if insns is not None else size // 4, ind[0], ind[1]))
        units.append((a, unit))
        for i, o in enumerate(outs):
            edges.append((a, o, a + 4 * (i + 1)))
        for callee, rw, off in res:
            i = list(outs).index(callee)
            rows.append((a, rw, off, 'r:%08X' % (a + 4 * (i + 1))))
        if slot is not None:
            ptrs.append((a, slot))
        for rw, off, t in rws:
            rows.append((a, rw, off, t))
        if vt:
            vst.append((a, vt[0], vt[1]))
        return a
    for nm, a in (('cymem_free_tagged_wrapper', FREE), ('component_find_by_tag', FIND), ('get_element_192_by_index_with_optional_remap', UI),
                  ('alloc_via_hook_8395c770_or_null', HAL), ('set_global_word_8395c780', HSZ), ('pick', PICK), ('alloc', ALLOC)):
        fn(nm, 64, unit=9, at=a)
    tgt = fn('tgt', 300, unit=2)
    # destructors
    fn('dinl', 76, (FREE,), slot=0, rws=(('W', 0, 'a0'),))
    fn('dinl_noW', 76, (FREE,), slot=0, rws=(('R', 8, 'a0'),))
    fn('dinl_noslot', 76, (FREE,), rws=(('W', 0, 'a0'),))
    sized = (('R', 4, 'a0'), ('R', 0, 'u'), ('R', 8, 'u'), ('R', 44, 'u'))
    fn('dsz', 128, (tgt,), slot=0, ind=(1, 0), rws=sized)
    fn('dsz_slot8', 128, (tgt,), slot=8, ind=(1, 0), rws=sized)
    fn('dsz_no44', 128, (tgt,), slot=0, ind=(1, 0), rws=sized[:3])
    body = fn('bodyW', 200, rws=(('W', 0, 'a0'),), vt=('a0', 0x82001000))
    fn('ddel', 84, (body, FREE), slot=0)
    body2 = fn('bodyNoW', 200, rws=(('R', 8, 'a0'),))
    fn('ddel2', 84, (body2, FREE), slot=0)
    # ctors
    fn('vctor', 300, rws=(('W', 0, 'a0'),), vt=('a0', 0x82002000))
    fn('vstore_a1', 300, rws=(('W', 0, 'a1'),), vt=('a1', 0x82002000))
    fn('vstore_notvt', 300, rws=(('W', 0, 'a0'),), vt=('a0', 0x82003000))   # S110 review: an image address that is not a vtable (a record)
    # stubs
    fn('stub4', 4, insns=1)
    fn('stub4_bctr', 4, insns=1, ind=(0, 1))
    fn('ret8', 8)
    fn('ret8_row', 8, rws=(('R', 0, 'a1'),))
    # accessors
    fn('addr8', 8, rws=(('A', 16, 'a0'),))
    fn('addr8_call', 8, (tgt,), rws=(('A', 16, 'a0'),))
    fn('gget', 12, rws=(('R', 0x83000010, 'K'),))
    fn('gget_chain', 36, rws=(('R', 16, 'g:83000020'), ('R', 8, 'p(g:83000020,16)')))
    fn('gset', 12, rws=(('W', 0x83000010, 'K'),))
    fn('iget', 32, rws=(('R', 4, 'a0'), ('R', 12, 'p(a0,4)')))
    fn('iget_w', 32, rws=(('R', 4, 'a0'), ('W', 12, 'p(a0,4)')))
    cp = (('R', 0, 'a1'), ('R', 4, 'a1'), ('R', 8, 'a1'))
    fn('copy', 28, insns=7, rws=cp + (('W', 0, 'a0'), ('W', 4, 'a0'), ('W', 8, 'a0')))
    fn('copy_scaled', 32, insns=8, rws=cp + (('W', 0, 'a0'), ('W', 4, 'a0'), ('W', 8, 'a0')))
    fn('copy_shift', 28, insns=7, rws=cp + (('W', 4, 'a0'), ('W', 8, 'a0'), ('W', 12, 'a0')))
    # thunks
    fn('ifwd', 20, ind=(0, 1), rws=(('R', 8, 'a0'), ('R', 0, 'p(a0,8)'), ('R', 12, 'p(p(a0,8),0)')))
    fn('vfwd', 16, ind=(0, 1), rws=(('R', 0, 'a0'), ('R', 36, 'p(a0,0)')))          # S110 review: slot 9 of its OWN vtable, no inner object
    fn('vfwd_guard', 28, ind=(0, 1), rws=(('R', 1928, 'a0'), ('R', 0, 'a0'), ('R', 20, 'p(a0,0)')))
    fn('ifwd_call', 20, (tgt,), ind=(0, 1), rws=(('R', 8, 'a0'), ('R', 0, 'p(a0,8)')))
    fn('wrap', 40, (tgt,))
    fn('wrap_rows', 40, (tgt,), rws=(('R', 4, 'a0'),))
    fn('wrap_big', 100, (tgt,))
    # helper-named
    fn('uiset', 100, (UI,), res=((UI, 'W', 144),))
    fn('uiget', 100, (UI,), res=((UI, 'R', 144),))
    fn('clook', 200, (FIND, tgt), res=((FIND, 'R', 24),))
    fn('clook_other', 200, (FIND, tgt), res=((tgt, 'R', 24),))
    fn('clook_many', 200, (FIND, tgt, FREE, UI), res=((FIND, 'R', 24),))      # S110 review: a function that USES a component (4 callees)
    fn('clook_large', 300, (FIND, tgt), res=((FIND, 'R', 24),))
    # factories
    fn('mk', 150, (PICK, ALLOC, tgt))
    fn('mk_noctor', 150, (PICK, ALLOC))
    big = [fn('bigcallee%d' % i, 40, unit=3) for i in range(10)]
    fn('mk_big', 2000, (PICK, ALLOC) + tuple(big))
    fn('hk', 120, (HAL, HSZ))
    fn('hk_half', 120, (HAL,))
    # state machines: unit 5 sets this+16 three times; unit 6 only twice (its third callee reads +16 first: a cursor)
    hs = [fn('h%d' % i, 60, unit=5, rws=(('W', 16, 'a0'),)) for i in range(3)]
    fn('disp', 300, tuple(hs), unit=5, ind=(0, 1), rws=(('R', 16, 'a0'),))
    gs = [fn('g%d' % i, 60, unit=6, rws=(('W', 16, 'a0'),)) for i in range(2)]
    gs.append(fn('g_cursor', 60, unit=6, rws=(('R', 16, 'a0'), ('W', 16, 'a0'))))
    fn('disp2', 300, tuple(gs), unit=6, ind=(0, 1), rws=(('R', 16, 'a0'),))
    # S110 review: unit 8 sets this+16 three times but its reader holds no indirect jump (no switch: a position the callees return)
    js = [fn('j%d' % i, 60, unit=8, rws=(('W', 16, 'a0'),)) for i in range(3)]
    fn('disp_nojt', 300, tuple(js), unit=8, rws=(('R', 16, 'a0'),))
    fn('loner', 4, insns=1, unit=7)
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
                    'create table vstores(func integer, root integer, vt integer);create table ctors(func integer primary key, vt integer);')
    c.executemany('insert into rows values (?,0,?,4,0,?,?)', [(a, rw, off, rid(t)) for a, rw, off, t in rows])
    c.executemany('insert into vstores values (?,?,?)', [(a, rid(t), v) for a, t, v in vst])
    c.executemany('insert into ctors values (?,?)', [(a, v) for a, t, v in vst if t == 'a0' and v in VTABLES])
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
    os.makedirs(os.path.join(d, 'db'))
    json.dump([{'address': '0x%08X' % G[n], 'name': n, 'confidence': 'high'} for n in (
        'cymem_free_tagged_wrapper', 'component_find_by_tag', 'get_element_192_by_index_with_optional_remap',
        'alloc_via_hook_8395c770_or_null', 'set_global_word_8395c780') if n not in drop], open(os.path.join(d, 'db', 'h.json'), 'w'))
    return d, G


def main():
    d, G = synthetic()
    p = qpattern.paths(idx=d, dbdir=os.path.join(d, 'db'))
    out = qpattern.build(p, verbose=False)                # the hook: build runs both rule sets

    def kinds(n):
        return sorted(k for k, _e in out.get(G[n], []) if k in qpattern_rules.KINDS)
    for n, k in (('dinl', 'deleting_dtor_inline'), ('dsz', 'deleting_dtor_sized'), ('bodyW', 'dtor_body'), ('vctor', 'vptr_store'),
                 ('stub4', 'empty_stub'), ('ret8', 'trivial_return'), ('addr8', 'field_address_getter'), ('gget', 'global_getter'),
                 ('gget_chain', 'global_getter'), ('iget', 'inner_field_getter'), ('copy', 'field_copy'), ('ifwd', 'inner_forwarder'),
                 ('wrap', 'wrapper'), ('uiset', 'ui_element_setter'), ('clook', 'component_lookup'), ('mk', 'alloc_construct'),
                 ('hk', 'hook_alloc'), ('disp', 'state_dispatcher'), ('h0', 'state_handler'), ('h2', 'state_handler'), ('loner', 'empty_stub'),
                 ('vfwd', 'vcall_forwarder'), ('vfwd_guard', 'vcall_forwarder')):
        check(kinds(n) == [k], '%s -> %s (got %s)' % (n, k, kinds(n)))
    check(sorted(k for k, _e in out.get(G['ddel'], [])) == ['deleting_dtor'], 'ddel keeps the first set\'s deleting_dtor and nothing more')
    check('vptr_store' not in kinds('bodyW'), 'a dtor body that stores its vtable is dtor_body, not also vptr_store')
    for n in ('dinl_noW', 'dinl_noslot', 'dsz_slot8', 'dsz_no44', 'bodyNoW', 'vstore_a1', 'stub4_bctr', 'ret8_row', 'addr8_call', 'gset',
              'iget_w', 'copy_scaled', 'copy_shift', 'ifwd_call', 'wrap_rows', 'wrap_big', 'uiget', 'clook_other', 'mk_noctor', 'mk_big',
              'hk_half', 'disp2', 'g0', 'g_cursor', 'vstore_notvt', 'clook_many', 'clook_large', 'disp_nojt', 'j0'):
        check(kinds(n) == [], 'twin %s has no new label (got %s)' % (n, kinds(n)))
    # stored, listed, shown
    lab = qpattern.labels(p['out'])
    check(lab.get(G['dinl']) == ['deleting_dtor_inline'] and qpattern.tag(lab, G['dinl']) == '[p:deleting_dtor_inline]', 'the new kind is in pattern.s1.db and the sheet tag')
    check('state_dispatcher' in qpattern.stats(p['out']) and 'deleting_dtor_inline' in qpattern.stats(p['out']), 'q pattern --stats lists the new kinds')
    check('global 0x83000010' in qpattern.show_one('%08X' % G['gget'], p['out']), 'q pattern FUNC prints the new evidence')
    check('stores vtable vt82002000 at this+0' in qpattern.show_one('%08X' % G['vctor'], p['out']), 'vptr_store evidence states the store it proves')
    txt = qpattern_rules.count(p)
    check('loners (units of 1-2)' in txt and 'all functions' in txt and 'empty_stub' in txt, 'count prints all / loners / per kind')
    loner_line = [ln for ln in txt.splitlines() if 'loners' in ln][0]
    # loners: unit 2 (tgt, no label) and unit 7 (loner, an empty_stub: labelled after, not before); units 1, 3, 5, 6, 9 hold 3 or more
    check(re.findall(r'(?<![\d.])\d+(?![\d.%])', loner_line.split('1-2)')[1]) == ['2', '0', '1'],
          'loner counts: 2 functions, 0 before, 1 after (got %r)' % loner_line)
    # a helper that does not resolve: its rule reports 0 and the build's unresolved list names it
    d2, G2 = synthetic(drop=('get_element_192_by_index_with_optional_remap',))
    p2 = qpattern.paths(idx=d2, dbdir=os.path.join(d2, 'db'))
    f2 = qpattern.load(p2)
    out2 = qpattern_rules.extend(f2, qpattern.classify(f2), p2)
    check(any(a == 'ui_elem' for a, _ in f2.unresolved) and not any(k == 'ui_element_setter' for v in out2.values() for k, _e in v),
          'unresolved get_element_192 helper: ui_element_setter reports 0 and the unresolved list says why')
    # base() / is_global()
    check(qpattern_rules.base('p(p(a0,4),0)') == 'a0' and qpattern_rules.is_global('p(g:83000020,16)') and not qpattern_rules.is_global('p(a0,4)'),
          'root text: innermost base and global test')
    # real index
    if os.path.exists(qpattern.PATTERN_DB) and any(set(v) & set(qpattern_rules.KINDS) for v in qpattern.labels().values()):
        print(qpattern_rules.count(qpattern.paths()))
    else:
        print('SKIPPED  real index: pattern.s1.db holds none of the new kinds yet (run py tools\\q\\q.py pattern --build)')
    print('%d failed' % len(FAILS) if FAILS else 'all ok')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
