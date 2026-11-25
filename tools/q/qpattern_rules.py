r"""q pattern rules, second set (S110 lane PATRULES): rule families taken from the PATTERNS the unit readers wrote more than once
(`q read stats`) that a SHAPE over facts the index already holds can prove. qpattern.build() calls extend() after its own rules: the
labels go into the same table (pattern.s1.db pattern_fn: func, kind, evidence), so q sheet prints them as ` [p:kind]` and
`q pattern FUNC` prints the evidence. A label is a KIND ("looks like"), never a name: never db\*.json, never [g], never a cover anchor.

    py tools/q/q.py pattern --count     functions with a label before (qpattern.KINDS) and after (+ KINDS below), for all functions and
                                        for the loners (units of 1 or 2 functions), and per new kind (after `q pattern --build`)

Facts read on top of qpattern.Facts: funcs.n_bctrl / n_bctr / insns (indirect call / indirect tail / instruction counts), the call site
of every call to a helper (edges.site), every field row of functions of <= SMALL bytes with its root text (a0 = this, a1 = param 1,
K / g:ADDR = absolute global, p(...) = pointer loaded from a field, u = unknown base), fields ctors (the vtable a function stores at
this+0: build_fields keeps a stored image address there only when its first word is a function). A rule that needs "no indirect branch"
does not fire when the index has no n_bctrl column (an old test index).
LIMIT (S110 review): fields rows hold D-form loads / stores only (build_fields MEM_RE); an indexed access (lwzx / stwx / stvx) is no row.
So "no field row" / "every row a read" can miss such an access: measured on the S110 index, 68 of 1,619 trivial_return, 8 of 333
global_getter and 4 of 720 wrapper hold one. The structural fix is in build_fields (rows for indexed accesses), not here.

Families (kind: predicate; the false positive its selftest twin plants):
  destructors  deleting_dtor_inline: vslot +0/+12, 40..160 B, its only callee is cymem_free_tagged_wrapper, writes this+0 (body inlined).
                 FP: the same without the this+0 write (a member free at slot 0); the same without a vtable slot.
               deleting_dtor_sized: vslot +0, 96..160 B, <= 1 callee, >= 1 indirect call, this rows = one read of +4, reads +0 +8 +44 of the
                 unknown base (the class frees itself through an allocator object's slot). FP: the same at vslot +8; without the +44 read.
               dtor_body: the non-free callee of a deleting_dtor (first rule set) that writes this+0.
                 FP: the callee of a deleting dtor that does not write this+0 (a release helper).
  ctors        vptr_store: stores a vtable at this+0 (fields ctors) and carries no destructor label. The label states the store; what
                 it means is "a ctor, or a dtor body that resets its vtable" (S110 review: of 467 HIGH db names, 397 name a ctor, 12 a
                 dtor). FP: a vtable stored through another root (param 1: placement into a caller's buffer); an image address that
                 is not a vtable (a static record, a function pointer: 24 on the S110 index).
  stubs        empty_stub: 4 B, no callee, no field row, no indirect branch (blr). FP: a 4 B function with an indirect tail.
               trivial_return: 8 or 12 B, no callee, no field row, no indirect branch: returns a constant (78 %) or a value computed
                 from its arguments (S110 review, read from the words). FP: an 8 B function with one param field row.
  accessors    field_address_getter: 8 B, no callee, the only field fact takes the address of this+N, N != 0 (returns a sub-object).
                 FP: the same with a callee (an adjust-this jump, first rule set's shape).
               global_getter: <= 48 B, no callee, no indirect branch, every field row a READ of an absolute global or a pointer chain from
                 one. FP: a write to the global (a setter).
               inner_field_getter: <= 40 B, no callee, no indirect branch, >= 2 rows, all reads of this or a pointer chain from this, at
                 least one through a pointer. FP: the same with a write through the pointer.
               field_copy: no callee, no indirect branch, writes this+{S} and reads param1+{S} (the same >= 2 offsets), nothing else, and
                 no instruction besides those loads / stores and the return. FP: one more instruction (a scale: a transform, not a copy).
  thunks       vcall_forwarder / inner_forwarder: <= 32 B, no callee, exactly one indirect tail (bctr), no indirect call, every row a
                 read of this or a pointer chain from this. vcall_forwarder: the chain is this's OWN vtable (rows only on this and
                 p(a0,0): a tail call of one of its virtual slots, 585 on the S110 index); inner_forwarder: the jump goes through a
                 pointer held in a member (an inner object's method or a stored callback). FP: the same with a direct callee.
               wrapper: 20..56 B, exactly one callee, no field row, no indirect branch (sets up arguments and calls one function).
                 FP: the same with a field row; the same at 100 B.
  helper-named ui_element_setter: writes a field of the result of a call to get_element_192_by_index_with_optional_remap.
                 FP: only reads that result.
               component_lookup: <= 256 B, <= 3 callees, reads +24 of the result of a call to component_find_by_tag (the first rule
                 set's component_getter is the 72..96 B one-callee case and is not repeated). FP: reads +24 of another call's result;
                 a function that only USES a component (S110 review: thread loops, AI checks, constructors of 300..17,860 B; 4+ callees).
  factories    alloc_construct: <= 512 B, calls sub_82C2E1E0 (the heap pick), one of sub_82C2DCD8 / sub_82C2F5A8 (the allocators) and
                 1..4 other functions (the ctor). FP: the pick and an allocator and nothing else; a large function with 10 other callees.
               hook_alloc: calls alloc_via_hook_8395c770_or_null and set_global_word_8395c780 (allocation through the hook allocator).
                 FP: the allocation call alone.
  state machines state_dispatcher / state_handler: a function that reads this+X (X > 0), holds an indirect jump (n_bctr >= 1: its
                 switch's jump table) and calls >= 3 functions of its own unit that each write this+X WITHOUT reading it (they set a
                 state; a reader's cursor advance reads it first); those callees are state_handler. FP: only 2 callees set this+X;
                 callees that read and write this+X (a stream cursor); a reader with no jump table (S110 review: the LZX decoder
                 0x82CA49C0, whose three block decoders each store the window position at +11960).
Patterns that stay with readers (they name game meaning a shape cannot prove) are listed in docs\research\s110\PATRULES.md.
"""
import collections
import os
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qnames  # noqa: E402

SMALL = 256                      # field rows are kept (with their root text) only for functions of at most this many bytes
KINDS = ('deleting_dtor_inline', 'deleting_dtor_sized', 'dtor_body', 'vptr_store', 'empty_stub', 'trivial_return',
         'field_address_getter', 'global_getter', 'inner_field_getter', 'field_copy', 'vcall_forwarder', 'inner_forwarder', 'wrapper',
         'ui_element_setter', 'component_lookup', 'alloc_construct', 'hook_alloc', 'state_dispatcher', 'state_handler')
FAMILY = {'deleting_dtor_inline': 'destructors', 'deleting_dtor_sized': 'destructors', 'dtor_body': 'destructors',
          'vptr_store': 'ctors', 'empty_stub': 'stubs', 'trivial_return': 'stubs', 'field_address_getter': 'accessors',
          'global_getter': 'accessors', 'inner_field_getter': 'accessors', 'field_copy': 'accessors', 'inner_forwarder': 'thunks', 'vcall_forwarder': 'thunks',
          'wrapper': 'thunks', 'ui_element_setter': 'helper-named', 'component_lookup': 'helper-named', 'alloc_construct': 'factories',
          'hook_alloc': 'factories', 'state_dispatcher': 'state machines', 'state_handler': 'state machines'}
HELPERS = {                      # alias -> names (db / game / index names) or sub_XXXXXXXX (the function at that address)
    'free': ('cymem_free_tagged_wrapper',),
    'find': ('component_find_by_tag', 'component_find_by_tag_with_cache_at_12'),
    'ui_elem': ('get_element_192_by_index_with_optional_remap',),
    'hook_alloc': ('alloc_via_hook_8395c770_or_null',),
    'hook_size': ('set_global_word_8395c780',),
    'heap_pick': ('sub_82C2E1E0',),
    'heap_alloc': ('sub_82C2DCD8', 'sub_82C2F5A8'),
}
DTOR_KINDS = ('deleting_dtor', 'deleting_dtor_inline', 'deleting_dtor_sized', 'dtor_body', 'component_ctor')


def ro(path):
    path = path and qnames.newest(path)
    if not path or not os.path.exists(path):
        raise qnames.NamesError('no file %s' % path)
    return sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)


def base(t):
    """the innermost root of a root text: 'p(p(a0,4),0)' -> 'a0', 'p(g:83000000,16)' -> 'g:83000000'"""
    while t.startswith('p(') and ',' in t:
        t = t[2:t.rindex(',')]
    return t


def is_global(t):
    b = base(t)
    return b == 'K' or b.startswith('g:')


def global_addr(row):
    _rw, off, t = row
    if t == 'K':
        return off
    if t.startswith('g:'):
        return int(t[2:], 16) + off
    b = base(t)
    return int(b[2:], 16) if b.startswith('g:') else 0


class Extra:
    """the facts this rule set reads on top of qpattern.Facts"""

    def __init__(self):
        self.rows = collections.defaultdict(list)        # func -> [(rw, off, root text)], functions of <= SMALL bytes only
        self.vstore = collections.defaultdict(set)       # func -> {vt}: the vtable it stores at this+0 (fields ctors)
        self.ind = {}                                    # func -> (n_bctrl, n_bctr); absent = not known
        self.insns = {}                                  # func -> instruction count
        self.site = {}                                   # call site -> callee, for calls to the helpers only
        self.helper = {}                                 # alias -> set(addr)
        self.unresolved = []                             # [(alias, [names])]


def resolve(f, x):
    byname = collections.defaultdict(set)
    for a, lst in f.names.items():
        for n, _s in lst:
            byname[n].add(a)
    for alias, ns in HELPERS.items():
        got = set()
        for n in ns:
            if n.startswith('sub_'):
                a = int(n[4:], 16)
                if a in f.size:
                    got.add(a)
            else:
                got |= byname.get(n, set())
        x.helper[alias] = got
        if not got:
            x.unresolved.append((alias, list(ns)))


def load_extra(f, p):
    x = Extra()
    resolve(f, x)
    ix = ro(p['index'])
    try:
        for a, nb, nt, ni in ix.execute('select addr,n_bctrl,n_bctr,insns from funcs where has_body=1'):
            x.ind[a] = (nb or 0, nt or 0)
            x.insns[a] = ni or 0
    except sqlite3.OperationalError:
        pass                                             # no columns: the rules that need "no indirect branch" do not fire
    want = x.helper['ui_elem'] | x.helper['find']
    if want:
        for s, d in ix.execute("select site,dst from edges where kind in ('call','tail') and dst in (%s)" % ','.join('%d' % a for a in want)):
            x.site[s] = d
    fd = ro(p['fields'])
    roots = dict(fd.execute('select id,text from roots'))
    for fn, rw, off, root in fd.execute('select func,rw,off,root from rows'):
        if f.size.get(fn, SMALL + 1) <= SMALL:
            x.rows[fn].append((rw, off, roots.get(root, '')))
    try:        # S110 review: ctors, not raw vstores (those hold ANY image address stored at this+0: 24 were records / function pointers)
        for fn, vt in fd.execute('select func,vt from ctors'):
            x.vstore[fn].add(vt)
    except sqlite3.OperationalError:
        pass
    return x


def extend(f, out, p, x=None):
    """add this rule set's labels to `out` ({func: [(kind, evidence)]}, the first rule set's result); -> out"""
    x = x or load_extra(f, p)
    for alias, ns in x.unresolved:
        if alias not in [u[0] for u in f.unresolved]:
            f.unresolved.append((alias, ns))
    h = x.helper
    free, find, ui, hal, hsz, pick, alloc = (h[k] for k in ('free', 'find', 'ui_elem', 'hook_alloc', 'hook_size', 'heap_pick', 'heap_alloc'))
    wr0 = collections.defaultdict(set)                   # func -> offsets of this it writes
    rd0 = collections.defaultdict(set)                   # func -> offsets of this it reads
    for a, lst in f.this.items():
        for rw, o in lst:
            if rw == 'W':
                wr0[a].add(o)
            elif rw == 'R':
                rd0[a].add(o)

    def has(a, ks):
        return any(k in ks for k, _e in out.get(a, []))
    handlers = {}
    for a, sz in f.size.items():
        outs = f.outs.get(a, set())
        offs = set(o for _vt, o in f.vslot.get(a, []))
        nrows = f.nrows.get(a, 0)
        rows = x.rows.get(a, [])
        this = f.this.get(a, [])
        ind = x.ind.get(a)
        quiet = ind == (0, 0)                            # known: no indirect call, no indirect tail
        lab = []
        # destructors
        if free and offs & {0, 12} and 40 <= sz <= 160 and outs and outs <= free and 0 in wr0.get(a, ()):
            lab.append(('deleting_dtor_inline', '%dB slot +%s, only callee free, writes this+0' % (sz, '/+'.join(str(o) for o in sorted(offs)))))
        if 0 in offs and 96 <= sz <= 160 and len(outs) <= 1 and ind and ind[0] >= 1 and this == [('R', 4)] and \
                {0, 8, 44} <= set(o for rw, o, t in rows if t == 'u' and rw == 'R'):
            lab.append(('deleting_dtor_sized', '%dB slot +0, reads this+4 and unknown base +0 +8 +44, %d indirect call(s)' % (sz, ind[0])))
        # stubs
        if not outs and not nrows and quiet:
            if sz == 4:
                lab.append(('empty_stub', '4B, no call, no field'))
            elif sz in (8, 12):
                lab.append(('trivial_return', '%dB, no call, no field' % sz))
        # accessors
        if sz == 8 and not outs and nrows == 1 and len(this) == 1 and this[0][0] == 'A' and this[0][1] != 0:
            lab.append(('field_address_getter', '8B returns this%+d' % this[0][1]))
        if not outs and quiet and rows and sz <= 48:
            if all(rw == 'R' and is_global(t) for rw, _o, t in rows):
                lab.append(('global_getter', '%dB reads global 0x%08X%s' % (sz, global_addr(rows[0]), ' (+%d rows)' % (len(rows) - 1) if len(rows) > 1 else '')))
            elif sz <= 40 and len(rows) >= 2 and all(rw == 'R' and base(t) == 'a0' for rw, _o, t in rows) and any(t.startswith('p(') for _rw, _o, t in rows):
                first = sorted(o for _rw, o, t in rows if t == 'a0')
                lab.append(('inner_field_getter', '%dB reads this%s then through the pointer' % (sz, ('+%d' % first[0]) if first else '')))
        if not outs and quiet and rows and len(rows) == nrows:
            w = set(o for rw, o, t in rows if t == 'a0' and rw == 'W')
            if len(w) >= 2 and x.insns.get(a) == len(rows) + 1 and w == set(o for rw, o, t in rows if t == 'a1' and rw == 'R') and \
                    all((t == 'a0' and rw == 'W') or (t == 'a1' and rw == 'R') for rw, _o, t in rows):
                lab.append(('field_copy', '%dB copies param1 -> this at %d offsets' % (sz, len(w))))
        # thunks
        if sz <= 32 and not outs and ind == (0, 1) and rows and all(rw == 'R' and base(t) == 'a0' for rw, _o, t in rows):
            own = sorted(set(o for _rw, o, t in rows if t == 'p(a0,0)'))
            if own and all(t in ('a0', 'p(a0,0)') for _rw, _o, t in rows):
                lab.append(('vcall_forwarder', '%dB tail-calls its own vtable +%s' % (sz, '/+'.join(str(o) for o in own))))
            else:
                held = sorted(set(o for _rw, o, t in rows if t == 'a0'))
                lab.append(('inner_forwarder', '%dB jumps through a pointer loaded from this+%s' % (sz, '/+'.join(str(o) for o in held))))
        if 20 <= sz <= 56 and len(outs) == 1 and not nrows and quiet:
            lab.append(('wrapper', '%dB -> %08X, no field' % (sz, next(iter(outs)))))
        # helper-named
        if ui:
            ws = sorted(set(o for s, rw, o in f.res.get(a, []) if rw == 'W' and x.site.get(s) in ui))
            if ws:
                lab.append(('ui_element_setter', 'writes +%s of get_element_192 result' % ' +'.join(str(o) for o in ws[:6])))
        if find and sz <= 256 and len(outs) <= 3 and not has(a, ('component_getter',)) and \
                any(rw == 'R' and o == 24 and x.site.get(s) in find for s, rw, o in f.res.get(a, [])):
            lab.append(('component_lookup', '%dB reads +24 of a component_find_by_tag result' % sz))
        # factories
        if pick and outs & pick and outs & alloc and 1 <= len(outs - pick - alloc) <= 4 and sz <= 512:
            lab.append(('alloc_construct', 'heap pick + allocator, then %s' % ' '.join('%08X' % c for c in sorted(outs - pick - alloc))))
        if hal and hsz and outs & hal and outs & hsz:
            lab.append(('hook_alloc', 'alloc_via_hook + set_global_word_8395c780'))
        # state machines
        rd = sorted(set(o for rw, o in this if rw == 'R' and o > 0))
        u = f.unit.get(a)
        if rd and len(outs) >= 3 and u is not None and ind and ind[1] >= 1:
            for off in rd:
                hs = sorted(c for c in outs if c != a and f.unit.get(c) == u and off in wr0.get(c, ()) and off not in rd0.get(c, ()))
                if len(hs) >= 3:
                    lab.append(('state_dispatcher', 'reads this+%d, holds an indirect jump, %d unit callees set it' % (off, len(hs))))
                    for c in hs:
                        handlers.setdefault(c, (off, a))
                    break
        if lab:
            out[a].extend(lab)
    for c, (off, a) in sorted(handlers.items()):
        out[c].append(('state_handler', 'sets this+%d, called by state_dispatcher %08X' % (off, a)))
    # dtor_body: the other callee of a first-set deleting_dtor, when it writes this+0
    for a in sorted(a for a, v in out.items() if any(k == 'deleting_dtor' for k, _e in v)):
        for b in sorted(f.outs.get(a, set()) - free):
            if 0 in wr0.get(b, ()) and not has(b, ('dtor_body',)):
                out[b].append(('dtor_body', 'called by deleting_dtor %08X, writes this+0' % a))
    for a in sorted(x.vstore):
        if a in f.size and not has(a, DTOR_KINDS):
            out[a].append(('vptr_store', 'stores vtable vt%08X at this+0 (a ctor, or a dtor body resetting it)' % min(x.vstore[a])))
    return out


def count(p):
    """-> text: labelled functions before (first set) / after (both sets), for all functions and the loners, and per new kind"""
    import qpattern
    c = ro(p['out'])
    by = collections.defaultdict(set)
    for a, k in c.execute('select func,kind from pattern_fn'):
        by[k].add(a)
    c.close()
    unit = dict(ro(p['units']).execute('select func,unit_id from units'))
    n = collections.Counter(unit.values())
    allf = set(unit)
    lon = set(a for a, u in unit.items() if n[u] <= 2)
    old = set().union(*[by.get(k, set()) for k in qpattern.KINDS])
    after = old.union(*[by.get(k, set()) for k in KINDS])
    lines = ['functions with a pattern label (derived purpose, 0 tokens): before = first rule set (qpattern.KINDS), after = + PATRULES (qpattern_rules.KINDS)',
             '  %-26s %7s %16s %16s' % ('', 'funcs', 'before', 'after')]
    for name, s in (('all functions', allf), ('loners (units of 1-2)', lon)):
        lines.append('  %-26s %7d %7d (%5.1f%%) %7d (%5.1f%%)' % (name, len(s), len(old & s), 100.0 * len(old & s) / max(1, len(s)),
                                                              len(after & s), 100.0 * len(after & s) / max(1, len(s))))
    lines.append('per new kind: labelled (all / loners); of those, with no first-set label (all / loners)')
    fam = collections.defaultdict(set)
    for k in KINDS:
        s = by.get(k, set()) & allf
        fam[FAMILY[k]] |= s
        lines.append('  %-14s %-22s %7d %7d   new %7d %7d' % (FAMILY[k], k, len(s), len(s & lon), len(s - old), len((s - old) & lon)))
    lines.append('per family (functions): ' + ', '.join('%s %d' % (k, len(v)) for k, v in fam.items()))
    return '\n'.join(lines)
