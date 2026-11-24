r"""qpattern selftest: planted functions per kind in temp dbs (index, units, fields, seen, names, db json), each with a TWIN that lacks the one
fact the rule needs and must get no label; then the unresolved-helper report, the sheet tag, and the real index (SKIPPED without one).
py tools/q/qpattern_selftest.py"""
import json
import os
import sqlite3
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qpattern  # noqa: E402

FAILS = []


def check(ok, what):
    print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        FAILS.append(what)


def mk(path, script, rows=()):
    c = sqlite3.connect(path)
    c.executescript(script)
    for sql, args in rows:
        c.executemany(sql, args)
    c.commit()
    c.close()


FREE, FIND, BASE, TICK, REND, THR, RDE = 0x82F00000, 0x82F00010, 0x82F00020, 0x82F00030, 0x82F00040, 0x82F00050, 0x82F00060
VT = 0x82E00000


def synthetic(drop=()):
    d = tempfile.mkdtemp(prefix='qpattern_')
    G = {}          # name -> addr of planted functions
    funcs, edges, units, rows_this, rows_res, nrows, seen, names, ptrs = [], [], [], [], [], {}, [], [], []
    nxt = [0x82100000]

    def fn(name, size, outs=(), unit=1, slot=None, this=(), res=(), caller=None, gname=None):
        a = nxt[0]
        nxt[0] += 0x100
        G[name] = a
        funcs.append((a, size))
        units.append((a, unit))
        for o in outs:
            edges.append((a, o))
        if slot is not None:
            ptrs.append((VT + 4 * len(ptrs), a, slot))
        for rw, off in this:
            rows_this.append((a, rw, off))
        for site, rw, off in res:
            rows_res.append((a, site, rw, off))
        if caller:
            seen.append((caller, a))
        if gname:
            names.append((a, gname))
        return a
    for nm, a in (('cymem_free_tagged_wrapper', FREE), ('component_find_by_tag', FIND), ('component_base_ctor', BASE),
                  ('sequence_owner_tick_sequence_hooks', TICK), ('render_category_walk_enabled_nodes', REND),
                  ('guest_thread_start_trampoline', THR), ('nonblock_reader_read_end_trampoline', RDE)):
        if nm not in drop:
            G[nm] = a
        funcs.append((a, 64))
        units.append((a, 9))
    body = fn('body', 200)                                           # a unit function the dtors call
    other = fn('other_unit_fn', 200, unit=2)
    # deleting_dtor and its twins (slot +0 / +12 and one unit function + free; twins: wrong slot, callee outside the unit, size 40)
    fn('dtor', 84, (body, FREE), slot=0)
    fn('dtor12', 96, (body, FREE), slot=12)
    fn('dtor_no_slot', 84, (body, FREE))
    fn('dtor_slot8', 84, (body, FREE), slot=8)
    fn('dtor_other_unit', 84, (other, FREE), slot=0)
    fn('dtor_small', 40, (body, FREE), slot=0)
    tgt = fn('tgt', 100)
    # this_adjust_thunk: 8 B, slot, takes address of this-28, one callee
    fn('adj', 8, (tgt,), slot=0, this=(('A', -28),))
    fn('adj_no_slot', 8, (tgt,), this=(('A', -28),))
    fn('adj_reads', 8, (tgt,), slot=0, this=(('R', 4),))
    # forward_thunk: 4..16 B, one callee, no field facts
    fn('fwd', 4, (tgt,))
    fn('fwd_two_callees', 4, (tgt, body))
    fn('fwd_with_field', 12, (tgt,), this=(('R', 8),))
    fn('fwd_big', 64, (tgt,))
    # component_getter: 80 B, only callee find, reads result+24
    fn('getter', 80, (FIND,), res=((0x82100999, 'R', 24),))
    fn('getter_off', 80, (FIND,), res=((0x82100999, 'R', 28),))
    fn('getter_other_callee', 80, (FIND, tgt), res=((0x82100999, 'R', 24),))
    # observed callers
    fn('tick', 120, slot=4, caller=TICK)
    fn('tick_twin', 120, slot=4, caller=tgt)
    fn('draw', 120, caller=REND)
    fn('thread', 120, caller=THR)
    fn('readend_obs', 120, caller=RDE)
    fn('readend_name', 120, gname='enum NonBlockReader::EnReadEndResult __cdecl CFoo::ReadEndProcess(void)')
    fn('readend_twin', 120, gname='CFoo::ReadEndProcessing')
    # component_ctor
    fn('ctor', 200, (BASE,), this=(('W', 0), ('W', 32)))
    fn('ctor_no_write', 200, (BASE,), this=(('R', 0),))
    fn('ctor_no_base', 200, (tgt,), this=(('W', 0),))
    # field_accessor
    fn('get', 8, this=(('R', 40),))
    fn('set', 8, this=(('W', 44),))
    fn('get_two', 8, this=(('R', 40), ('R', 44)))
    fn('get_big', 16, this=(('R', 40),))
    mk(os.path.join(d, 'index.db'),
       'create table funcs(addr integer, name text, size integer, insns integer, has_body integer);'
       'create table edges(src integer, dst integer, target integer, site integer, kind text);'
       'create table ptrs(at integer, value integer, kind text);create table tables(start integer primary key, rule text, len integer);',
       [('insert into funcs values (?,null,?,0,1)', funcs), ('insert into edges values (?,?,?,0,"call")', [(s, t, t) for s, t in edges]),
        ('insert into ptrs values (?,?,"func")', [(VT + 4 * i, v) for i, (_a, v, _s) in enumerate(ptrs)]),
        ('insert into tables values (?,?,?)', [(VT + 0x1000 * k, 'code', 1) for k in range(0)])])
    # one table per slot offset: table k holds one pointer at +off (a table starts at a word whose address minus off is the start)
    c = sqlite3.connect(os.path.join(d, 'index.db'))
    c.execute('delete from ptrs')
    for i, (_at, v, slot) in enumerate(ptrs):
        st = VT + 0x100 * (i + 1)
        c.execute('insert into tables values (?,?,?)', (st, 'code', slot // 4 + 1))
        c.execute('insert into ptrs values (?,?,"func")', (st + slot, v))
    c.commit()
    c.close()
    mk(os.path.join(d, 'units.s1.db'), 'create table units(func integer primary key, unit_id integer);', [('insert into units values (?,?)', units)])
    mk(os.path.join(d, 'fields.s2.db'),
       'create table roots(id integer primary key, text text unique, glob integer);'
       'create table rows(func integer, addr integer, rw text, width integer, fl integer, off integer, root integer);',
       [('insert into roots values (?,?,0)', [(1, 'a0'), (2, 'r:82100999')]),
        ('insert into rows values (?,0,?,4,0,?,1)', rows_this), ('insert into rows values (?,0,?,4,0,?,2)', [(a, rw, o) for a, _s, rw, o in rows_res])])
    mk(os.path.join(d, 'seen.s1.db'), 'create table seen(site integer, func integer, target integer, runs integer);',
       [('insert into seen values (0,?,?,3)', seen)])
    mk(os.path.join(d, 'names.s2.db'), 'create table names(func integer, name text, source text, table_addr integer);',
       [('insert into names values (?,?,"fn_string",0)', names)])
    os.makedirs(os.path.join(d, 'db'))
    json.dump([{'address': '0x%08X' % a, 'name': n, 'confidence': 'high'} for n, a in G.items() if n in (
        'cymem_free_tagged_wrapper', 'component_find_by_tag', 'component_base_ctor', 'sequence_owner_tick_sequence_hooks',
        'render_category_walk_enabled_nodes', 'guest_thread_start_trampoline', 'nonblock_reader_read_end_trampoline')],
              open(os.path.join(d, 'db', 'h.json'), 'w'))
    return d, G


def main():
    d, G = synthetic()
    p = qpattern.paths(idx=d, dbdir=os.path.join(d, 'db'))
    out = qpattern.build(p, verbose=False)

    def kinds(n):
        return sorted(k for k, _e in out.get(G[n], []))
    for n, k in (('dtor', 'deleting_dtor'), ('dtor12', 'deleting_dtor'), ('adj', 'this_adjust_thunk'), ('fwd', 'forward_thunk'),
                 ('getter', 'component_getter'), ('tick', 'sequence_tick'), ('draw', 'render_node_draw'), ('thread', 'thread_entry'),
                 ('readend_obs', 'read_end_process'), ('readend_name', 'read_end_process'), ('ctor', 'component_ctor'),
                 ('get', 'field_accessor'), ('set', 'field_accessor')):
        check(kinds(n) == [k], '%s -> %s (got %s)' % (n, k, kinds(n)))
    check(kinds('dtor_other_unit') == ['deleting_dtor'], 'a dtor whose body sits in another unit is still a deleting dtor (got %s)' % kinds('dtor_other_unit'))
    for n in ('dtor_no_slot', 'dtor_slot8', 'dtor_small', 'adj_no_slot', 'adj_reads', 'fwd_two_callees', 'fwd_with_field',
              'fwd_big', 'getter_off', 'getter_other_callee', 'tick_twin', 'readend_twin', 'ctor_no_write', 'ctor_no_base', 'get_two', 'get_big'):
        check(kinds(n) == [], 'twin %s has no label (got %s)' % (n, kinds(n)))
    check(kinds('adj_no_slot') == [] and kinds('fwd_with_field') == [], 'adj without slot and fwd with a field fact are not mislabelled')
    # stored db + verbs
    lab = qpattern.labels(p['out'])
    check(lab.get(G['dtor']) == ['deleting_dtor'] and qpattern.tag(lab, G['dtor']) == '[p:deleting_dtor]' and qpattern.tag(lab, G['body']) == '',
          'labels() and tag(): `[p:kind]`, empty without a label')
    check('deleting_dtor' in qpattern.show_one('%08X' % G['dtor'], p['out']) and 'no pattern label' in qpattern.show_one('%08X' % G['body'], p['out']),
          'q pattern FUNC prints the label / says none')
    check('deleting_dtor' in qpattern.stats(p['out']), 'stats lists the kinds')
    # a helper that does not resolve: the rules that need it report 0 and the build says why
    d2, G2 = synthetic(drop=('cymem_free_tagged_wrapper',))
    p2 = qpattern.paths(idx=d2, dbdir=os.path.join(d2, 'db'))
    f2 = qpattern.load(p2)
    out2 = qpattern.classify(f2)
    # the planted json still lists the helper by name only when it is in G2; it was dropped, so the helper must be unresolved
    check(any(a == 'free' for a, _ in f2.unresolved) and not any(k == 'deleting_dtor' for v in out2.values() for k, _e in v),
          'unresolved helper cymem_free_tagged_wrapper: deleting_dtor reports 0 and the unresolved list says why')
    # the sheet tag goes AFTER the names; qsheet's header line with a tag and no name
    import qsheet  # noqa: E402
    check(qsheet.qpattern is qpattern, 'qsheet imports qpattern (the hook)')
    # real index
    if os.path.exists(qpattern.PATTERN_DB):
        n = sum(1 for _ in qpattern.labels().values())
        check(n > 0, 'real pattern.s1.db holds labels (%d functions)' % n)
    else:
        print('SKIPPED  real index: no pattern.s1.db (run q pattern --build)')
    print('%d failed' % len(FAILS) if FAILS else 'all ok')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
