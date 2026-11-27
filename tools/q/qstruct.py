r"""q struct / q class: an object's layout and its class picture, recovered from every access in the GAME's code (S111 STRUCTREC,
tool order step 4).

    py tools/q/q.py struct CLASS|ROOT [--range LO-HI] [--json] [--no-cache]
    py tools/q/q.py class CLASS [--json] [--no-cache]

CLASS|ROOT: a vtable address (`0x82023298`, `vt:0x82023298`), a global word whose object is wanted (`g:0x837635B0`: the object the word
points to), or a function (`sub_X` / `ctor:sub_X`: a constructor names the class whose vptr it stores at this+0, a slot function its
vtable's class; a constructor that stores no vptr names the object its `this` is).

Built on what exists (one engine, no fork): the accesses are read off qslice's per-function facts (qdecomp's interpreter: every load's
and store's address as an expression, cached in out/harness/index/slice/), and each address is named by qslice's canonical root
(Slicer.canon: a class by its vtable, a subobject vptr stored at this+off by a constructor = that class +off, a global word a
constructor's result is stored in = that class; an array element = base + stride*k, the stride from a loop induction or a scaled
index such as 5292*[SM+268]). The functions read are qslice's for a field's writers (Slicer.candidates: the class's slot functions
and the functions the field index types with it, the users of its globals, its constructors) plus their direct callees that get a
pointer to the object (bound through the call, as qslice's writer scan does). Vtable slots are cut by tools/q/vtcut.py (shared with
build_fields and q slice).

q struct: one row per offset: width(s), kind (u32 / s16 / f32 ...; `extsh` / `extsb` of a load marks it signed), reads / writes /
pointer hand-offs (P = this+off passed to a callee), constant values written, the functions (slot numbers sN of the class, +OFF.sN of a
subobject's vtable), the index of an array row, and a name: a lodged field claim (db/claims `kind field`, keys vt:VT+OFF / g:ADDR+OFF /
ctor:sub_X+OFF, also through a subobject's vtable; [C] / [L] carried) or a generated one (`f_0x18_u32`, `a_0x9A0_x48_f32`). The header
gives the constructors, the allocation size (the constant r3 of the allocator call whose result a constructor's caller hands it),
the subobjects (a vptr a constructor stores at this+off) and the globals holding an instance.
q class: the vtable slots (vtcut) with each function's db name, the constructors / destructors storing the vptr, the parent and
subobject relations (a constructor calling another class's constructor on this+0 first = its base, on this+off = a member; a
vtable whose every slot equals ours at the same index = a base by prefix; a partial share is a lead), the classes that call our
constructor on their own this (derived / embedding classes) and the globals holding an instance.

Every row is a lead read off the listing by a tool [C tool]; confirm a surprising one in the listing (q asm FUNC --at OFF).
Known limits: indexed accesses whose base is no named root are not seen; a callee with more than LIB_CALLERS direct callers (a
library routine: memset, memcpy) is not read inside, its pointer argument shows as a P access; an array lattice has no upper bound.
"""
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qdecomp  # noqa: E402  the listing reader / interpreter (through qslice's facts)
import qslice  # noqa: E402  facts, canonical roots, class naming
import vtcut  # noqa: E402  where a vtable ends

MAX_FUNCS = 240          # candidate functions read (all are counted)
MAX_CALLEES = 240        # direct callees read with the object bound into them
LIB_CALLERS = 64         # a callee with more direct callers is a library routine: not read inside
MAX_CTOR_CALLERS = 12
ARG_R = [f'r{i}' for i in range(3, 11)]


class StructError(Exception):
    pass


def hx(a):
    return f'0x{a:08X}'


# ------------------------------------------------------------------ small source helpers (QSource or a planted one)
def is_func(src, a):
    f = getattr(src, 'is_func', None)
    if f is not None:
        return f(a)
    return bool(a) and src.func(a) is not None


def slot_list(src, vt):
    """[func] of vtable vt: its words from vt that are function starts, cut at the next vtable start (vtcut, the rule build_fields and
    q slice use); a planted source answers vt_slot_list"""
    f = getattr(src, 'vt_slot_list', None)
    if f is not None:
        return list(f(vt))
    starts = sorted(src._starts()) if hasattr(src, '_starts') else []
    end = vtcut.next_start(vt, starts)
    out, a = [], vt
    while len(out) < 512 and (end is None or a < end):
        w = src.word(a)
        if not is_func(src, w):
            break
        out.append(w)
        a += 4
    return out


def resolve_def(F, v, d=0):
    while v is not None and v[0] == 'def' and not v[1].startswith('L') and d < 8:
        full = F['defs'].get(f'{v[1]}@{v[2]:X}')
        if full is None:
            break
        v, d = full, d + 1
    return v


def expand(F, v, d=0):
    """a value as its alternatives: a join value's inputs, a long def's full expression"""
    v = resolve_def(F, v)
    if v is None or d > 4:
        return []
    if v[0] == 'phi':
        out = []
        for _p, x in F['phis'].get(v[2], {}).get(v[1], []):
            if x is not None and x != v:
                out += expand(F, x, d + 1)
        return out
    return [v]


def load_addr(rec):
    """the effective address of a load record (qslice facts `ins`), as qdecomp's Ctx.ea builds it from the base register values"""
    t = qdecomp.split_ops(rec['ops'])
    mn = rec['mn']
    b = rec['b']
    if len(t) < 2:
        return None
    if mn.endswith('x') or mn.endswith('ux') or mn.endswith('x128'):
        if len(t) < 3:
            return None
        base = ('c', 0) if t[1] == 'r0' else b.get(t[1])
        rb = ('c', 0) if t[2] == 'r0' else b.get(t[2])
        if base is None or rb is None:
            return None
        if base == ('c', 0):
            return rb
        if rb[0] == 'c':
            return qdecomp.mk_add(base, rb[1])
        if base[0] == 'c':
            return qdecomp.mk_add(rb, base[1])
        return ('op', '+', base, rb)
    m = qdecomp.MEM_RE.match(t[1])
    if not m:
        return None
    base = ('c', 0) if m.group(2) == 'r0' else b.get(m.group(2))
    if base is None:
        return None
    return qdecomp.mk_add(base, int(m.group(1), 0))


def value_text(v, width):
    """a stored value worth printing: an integer constant, or a float constant read from the image"""
    if v is None:
        return None
    if v[0] == 'c':
        n = v[1] & ((1 << (8 * width)) - 1 if width < 8 else 0xFFFFFFFFFFFFFFFF)
        return f'0x{n:X}' if n >= 0x10000 else str(n)
    if v[0] == 'ld' and v[2][0] == 'c' and v[3] is not None:
        return f'{v[3]}[{hx(v[2][1])}]'
    return None


# ------------------------------------------------------------------ the target: which object
class Target:
    def __init__(self, root, label, vt=None, note=''):
        self.root, self.label, self.vt, self.note = root, label, vt, note
        self.globals, self.ctors, self.subs, self.ctor_fn = set(), [], [], None

    def js(self):
        return {'root': qslice.root_text(self.root), 'label': self.label, 'vtable': hx(self.vt) if self.vt else None,
                'note': self.note, 'globals': [hx(g) for g in sorted(self.globals)]}


def parse_addr(text, resolve=None):
    s = text.strip()
    m = re.fullmatch(r'(?:0x)?([0-9A-Fa-f]{8})', s) or re.fullmatch(r'sub_([0-9A-Fa-f]{8})', s)
    if m:
        return int(m.group(1), 16)
    if resolve is not None:
        return resolve(s)
    raise StructError(f'{text!r}: an address (0x82023298), sub_X, vt:ADDR, g:ADDR or ctor:sub_X')


def class_target(sl, src, vt, given=None):
    v0, o = sl.norm_vt(vt)
    note = ''
    if o:
        note = f'{hx(vt)} is the vtable of the subobject at +{o} of class {hx(v0)} (its constructor stores it there): the layout is the outer class\'s'
    t = Target(('type', (v0,)), f'class {hx(v0)}', v0, note)
    t.globals = set(sl.globals_of(v0))
    return t


def global_target(sl, src, a):
    vts = src.global_types(a)
    if vts:
        r, o = sl.type_root(vts)
        v0 = r[1][0] if len(r[1]) == 1 else None
        t = Target(r, f'the object [{hx(a)}] holds: class ' + '/'.join(hx(v) for v in r[1]), v0,
                   f'the field index types the global word {hx(a)} with ' + ', '.join(hx(v) for v in sorted(vts)))
    else:
        gr = sl.global_root(a, 0)
        if gr is not None and gr[0] == 'type':
            v0 = gr[1][0] if len(gr[1]) == 1 else None
            t = Target(gr, f'the object [{hx(a)}] holds: class ' + '/'.join(hx(v) for v in gr[1]), v0,
                       f'every non-constant value stored in {hx(a)} is that class (its writers)')
        else:
            t = Target(('g', a), f'the object the global word [{hx(a)}] points to', None, 'no class: the word\'s writers store no typed object')
    if t.vt:
        t.globals = set(sl.globals_of(t.vt))
    t.globals.add(a)
    return t


def func_target(sl, src, f):
    vs = src.vslots(f)
    if vs:
        t = class_target(sl, src, vs[0][0])
        t.note = (t.note + '; ' if t.note else '') + f'sub_{f:08X} is slot {vs[0][1]} of vtable {hx(vs[0][0])}'
        return t
    T = sl.facts(f)
    if T is None:
        raise StructError(f'sub_{f:08X} has no generated body')
    vts = [v for v in sl.this_consts(T).get(0, ()) if src.is_vt(v)]
    if vts:
        t = class_target(sl, src, vts[-1])
        t.note = (t.note + '; ' if t.note else '') + f'sub_{f:08X} stores the vptr {hx(vts[-1])} at this+0 (a constructor)'
        t.ctor_fn = f
        return t
    cn = sl.canon(T, ('in', 'r3'), None)
    root = cn[0] if cn is not None and cn[0][0] not in ('u', 'K') else ('arg', f, 'r3')
    t = Target(root, f'the object sub_{f:08X} initialises (its this; no vptr stored)', None, 'no vtable: the accesses are those of this function and of the users of the globals its result is stored in')
    t.ctor_fn = f
    t.globals = result_globals(sl, src, f)
    return t


def result_globals(sl, src, f):
    """the global words a caller stores f's result in (qslice's globals_of, for a function rather than a class)"""
    out = set()
    for s_f, site, _k in src.callers(f)[:MAX_CTOR_CALLERS]:
        S = sl.facts(s_f)
        if S is None:
            continue
        for s in S['stores']:
            if s['val'] == ('ret', site - s_f, 'r3'):
                cn = sl.canon(S, s['af'], None, 3)
                if cn is not None and cn[0][0] == 'K' and not cn[1][1]:
                    out.add(cn[1][0])
    return out


def parse_target(sl, src, text, resolve=None):
    s = text.strip()
    m = re.fullmatch(r'(vt|g|ctor):(.+)', s)
    pref, body = (m.group(1), m.group(2)) if m else (None, s)
    a = parse_addr(body, resolve)
    if pref == 'g':
        return global_target(sl, src, a)
    if pref == 'vt':
        if not src.is_vt(a):
            raise StructError(f'{hx(a)}: its first word is not a function start: not a vtable (q words {hx(a)} 4)')
        return class_target(sl, src, a)
    if pref == 'ctor' or src.func(a) is not None:
        return func_target(sl, src, a)
    if src.is_vt(a):
        return class_target(sl, src, a)
    return global_target(sl, src, a)


def getters_of(sl, src, a):
    """S111 QFIX5: the getters of the global word a = the functions whose every return value is [a] (the object the word points to, offset 0):
    `lis r11,..; lwz r3,OFF(r11); blr`. Found among the functions that build the address a as a constant."""
    out = []
    for f in sorted(src.g_loaders(a)):
        T = sl.facts(f)
        if T is None:
            continue
        rv = [sl.canon(T, r['r3'], None) for r in T['rets'] if r['r3'][0] != 'undef']
        if rv and all(x is not None and x[0] == ('g', a) and x[1][0] == 0 and not x[1][1] for x in rv):
            out.append(f)
    return out


# ------------------------------------------------------------------ the class's own facts
def vptr_store_offs(sl, T, vt):
    """the function offsets of T's stores of the constant vt at this+0"""
    out = []
    for s in T['stores']:
        if s['stk'] is not None or s['val'][0] != 'c' or (s['val'][1] & 0xFFFFFFFF) != vt:
            continue
        c, terms = sl.lin(T, s['af'], None)
        if c == 0 and len(terms) == 1 and terms[0][0] == 1 and terms[0][2][2] == ('in', 'r3'):
            out.append(s['o'])
    return out


def role_of(sl, src, f, T, vt, slots):
    """ctor / dtor / dtor body: a slot of the class's own vtable is its destructor; a function that stores the vptr BEFORE calling
    another class's vptr-storing function on this+0 is a destructor body (a constructor calls its base first, then stores its vptr)"""
    if f in slots:
        return 'dtor'
    vrel = vptr_store_offs(sl, T, vt)
    base = [rel for c, _v2, _callee, rel in ctor_calls_on_this(sl, src, f, vt) if c == 0]
    if vrel and base and min(base) > min(vrel):
        return 'dtor body'
    return 'ctor'


def ctors_of(sl, src, vt):
    """[(func, role)]: the functions storing vt at this+0, each with role_of"""
    slots = set(slot_list(src, vt))
    out = []
    for f in sorted(src.vt_store_funcs(vt)):
        T = sl.facts(f)
        if T is None or vt not in sl.this_consts(T).get(0, ()):
            continue
        out.append((f, role_of(sl, src, f, T, vt, slots)))
    return out


def subobjects(sl, src, vt, ctors):
    """[(off, sub vt, func)]: a vptr a constructor / destructor of vt stores at this+off (off != 0)"""
    out = {}
    for f, _role in ctors:
        T = sl.facts(f)
        if T is None:
            continue
        for o, vals in sl.this_consts(T).items():
            if not o:
                continue
            for v in vals:
                if v != vt and src.is_vt(v):
                    out.setdefault((o, v), f)
    return [(o, v, f) for (o, v), f in sorted(out.items())]


def allocs(sl, src, f):
    """[{caller, site, alloc, size}] how f's callers make the object they hand it: the allocator call whose result is f's r3"""
    out = []
    for s_f, site, _k in src.callers(f)[:MAX_CTOR_CALLERS]:
        S = sl.facts(s_f)
        call = S['calls'].get(site - s_f) if S else None
        if not call:
            continue
        for e in expand(S, call['regs'].get('r3')):
            if e[0] == 'ret':
                c2 = S['calls'].get(e[1])
                if c2 and c2['k'] in ('C', 'TAIL') and c2.get('t'):
                    a = resolve_def(S, c2['regs'].get('r3'))
                    out.append({'caller': s_f, 'site': site, 'alloc': c2['t'], 'size': a[1] if a and a[0] == 'c' else None})
            elif e[0] == 'in':
                out.append({'caller': s_f, 'site': site, 'this_of': e[1]})
            elif e[0] == 'add' and qdecomp.is_stack(e):
                out.append({'caller': s_f, 'site': site, 'stack': qdecomp.stack_off(e)})
    return out


def ctor_calls_on_this(sl, src, f, vt):
    """[(off, other vt, callee, rel)]: the calls in f of another class's constructor on this+off (in listing order)"""
    T = sl.facts(f)
    out = []
    if T is None:
        return out
    for rel, call in sorted(T['calls'].items()):
        if call['k'] not in ('C', 'TAIL') or not call.get('t'):
            continue
        v = call['regs'].get('r3')
        if v is None:
            continue
        c, terms = sl.lin(T, v, None)
        if len(terms) == 1 and terms[0][0] == 1 and terms[0][2][2] == ('in', 'r3'):
            U = sl.facts(call['t'])
            if U is None:
                continue
            k = [x for x in sl.this_consts(U).get(0, ()) if src.is_vt(x) and x != vt]
            if k:
                out.append((c, k[-1], call['t'], rel))
    return out


def fill_class(sl, src, t):
    if t.vt is None:
        if t.ctor_fn:
            t.ctors = [(t.ctor_fn, 'ctor')]
        return t
    t.ctors = ctors_of(sl, src, t.vt)
    if t.ctor_fn and all(f != t.ctor_fn for f, _r in t.ctors):
        t.ctors.append((t.ctor_fn, 'ctor'))
    t.subs = subobjects(sl, src, t.vt, t.ctors)
    return t


# ------------------------------------------------------------------ the accesses
class Scan:
    def __init__(self, sl, src, t):
        self.sl, self.src, self.t = sl, src, t
        self.acc, self.seen = [], set()
        self.funcs, self.callees, self.lib_skipped, self.cut = 0, 0, set(), 0
        self.getters = []

    def match(self, r2):
        if qslice.same_root(self.t.root, r2):
            return 'C'
        if r2 is not None and r2[0] == 'g' and r2[1] in self.t.globals:
            return 'L'
        return None

    def index_text(self, rest):
        """the index terms of an array access; a load from the object itself prints as [this+OFF] (the same word whichever subobject's
        method reads it: sub_8283EAD8's [in_r3+268] and a +4 subobject's [in_r3+264] are both [this+268])"""
        out = []
        for coef, _k, (AF, ab, a), kind in rest:
            s = None
            if kind != 'atom':
                s = '?loop'
            elif a[0] == 'ld':
                cn = self.sl.canon(AF, a[2], ab)
                if cn is not None and self.match(cn[0]):
                    s = f'[this+{cn[1][0]}]' + (f'(+{cn[1][1]}*k)' if cn[1][1] else '')
            if s is None:
                s = self.sl.clip(self.sl.render(AF, a, ab), 60)
            out.append(s if abs(coef) == 1 else f'{abs(coef)}*{s}')
        return ' + '.join(out)

    def add(self, T, rel, rw, width, kind, cn, mark, via, val=None, signed=False, callee=None):
        key = (T['addr'], rel, rw, callee)
        if key in self.seen:
            return
        self.seen.add(key)
        r2, (c, g, anys), rest = cn
        strides = tuple(sorted({abs(t[0]) for t in rest if t[0]}, reverse=True))
        looped = bool(rest) and all(t[3] != 'atom' for t in rest)    # every index term a loop induction (not a loaded index)
        self.acc.append({'f': T['addr'], 'o': rel, 'rw': rw, 'w': width, 'kind': kind, 'c': c, 'g': g, 'st': strides, 'looped': looped, 'anys': anys,
                         'idx': self.index_text(rest) if g else '', 'val': val, 'via': via, 'mark': mark, 'signed': signed,
                         'callee': callee})

    def scan(self, T, bind, via):
        sl = self.sl
        ins = self.src.ins(T)
        signed = set()
        for rec in ins.values():
            if rec['mn'].startswith('exts'):
                toks = qdecomp.split_ops(rec['ops'])
                v = resolve_def(T, rec['b'].get(toks[1])) if len(toks) > 1 else None
                if v is not None and v[0] == 'ld':
                    signed.add((v[1], qslice.ekey(v[2])))
        for rel, rec in sorted(ins.items()):
            mn = rec['mn']
            if mn not in qdecomp.LOADS:
                continue
            af = load_addr(rec)
            if af is None or qdecomp.is_stack(af):
                continue
            cn = sl.canon(T, af, bind)
            m = self.match(cn[0]) if cn is not None else None
            if m:
                width, kind = qdecomp.LOADS[mn]
                self.add(T, rel, 'R', width, kind, cn, m, via, signed=(width, qslice.ekey(af)) in signed)
        for s in T['stores']:
            if s['stk'] is not None:
                continue
            cn = sl.canon(T, s['af'], bind)
            m = self.match(cn[0]) if cn is not None else None
            if m:
                self.add(T, s['o'], 'W', s['w'], s['kind'], cn, m, via, val=value_text(s['val'], s['w']))
        for rel, call in sorted(T['calls'].items()):
            for reg in ARG_R:
                v = call['regs'].get(reg)
                if v is None or v[0] in ('c', 'undef'):
                    continue
                cn = sl.canon(T, v, bind)
                m = self.match(cn[0]) if cn is not None else None
                if m and (cn[1][0] or cn[1][1]):
                    nm = self.src.name(call['t']) if call.get('t') else sl.vcall_text(call)
                    self.add(T, rel, 'P', 0, f'{reg}', cn, m, via, callee=nm)

    def callees_of(self, T, cands):
        out = []
        for rel, call in sorted(T['calls'].items()):
            if call['k'] not in ('C', 'TAIL') or not call.get('t') or call['t'] in cands:
                continue
            for reg in ARG_R:
                v = call['regs'].get(reg)
                if v is None or v[0] in ('c', 'undef'):
                    continue
                cn = self.sl.canon(T, v, None, 2)
                if cn is not None and self.match(cn[0]):
                    out.append((call['t'], (T, call['regs'], None), (T['addr'], rel)))
                    break
        return out

    def run(self):
        t, src, sl = self.t, self.src, self.sl
        cands = set()
        if t.root[0] == 'type':
            vts = set(t.root[1]) | {v for _o, v, _f in t.subs}
            for v in list(t.root[1]):
                vts |= sl.sub_vts(v)
            cands |= set(src.type_funcs(vts))
            for v in t.root[1]:
                cands |= set(src.vt_store_funcs(v))
        for g in t.globals:
            cands |= set(src.g_funcs(g))
        getter_callers = set()      # S111 QFIX5: the callers of a getter reach the object through the call's result (the setters they pass it to are read as callees)
        self.getters = []
        for g in sorted(t.globals):
            for gt in getters_of(sl, src, g):
                self.getters.append(gt)
                getter_callers |= {s for s, _site, _k in src.callers(gt)}
        cands |= getter_callers | set(self.getters)
        cands |= {f for f, _r in t.ctors}
        if t.root[0] in ('arg', 'r'):
            cands.add(t.root[1])
        allc = sorted(cands)
        keep = MAX_FUNCS + len(getter_callers)      # a getter's callers are the point of following it: they do not use up the cap
        self.cut = max(0, len(allc) - keep)
        cands = set(allc[:keep])
        todo = []
        for f in sorted(cands):
            T = sl.facts(f)
            if T is None:
                continue
            self.funcs += 1
            self.scan(T, None, None)
            todo += self.callees_of(T, cands)
        done = set()
        for callee, bind, via in todo:
            if self.callees >= MAX_CALLEES:
                self.cut += 1
                continue
            if (callee, via) in done:
                continue
            done.add((callee, via))
            if len(src.callers(callee)) > LIB_CALLERS:
                self.lib_skipped.add(callee)
                continue
            U = sl.facts(callee)
            if U is None:
                continue
            self.callees += 1
            self.scan(U, bind, via)
        return self.acc


# ------------------------------------------------------------------ rows
UNROLL_MAX = 16          # an unrolled loop's stride is at most this many elements of the array it walks


def group(accs):
    """one row per field: (base, strides) exact, every stride of a multi-index access kept (SM + 1388 + 5292*ctx + 4*i is not the
    row SM + 1388 + 5292*ctx); a one-stride row walked by a loop joins another loop-walked one-stride row when it is the same field
    walked by an unrolled loop: its stride a small multiple (<= UNROLL_MAX) of the other's, its base inside one of its own elements and
    in the same residue (the splat records: the reader's +2464 + 48*k, the unrolled writer's +2464 / +2512 ... + 480*k). A row indexed
    by a loaded value (a context number) never joins: SM + 1388 + 5292*[SM+268] stays its own row though 1388 = 800 + 588 and 588
    divides 5292"""
    by = {}
    for a in accs:
        by.setdefault((a['c'], a['st'], len(a['st']) == 1 and a['looped']), []).append(a)
    rows = {(k[0], k[1]): v for k, v in by.items() if not k[2]}
    arr = []
    for k in sorted((k for k in by if k[2]), key=lambda k: (k[1][0], k[0])):
        c2, g2 = k[0], k[1][0]
        for i, (c1, g1, lst) in enumerate(arr):
            if g2 % g1 == 0 and g2 // g1 <= UNROLL_MAX and (c2 - c1) % g1 == 0 and 0 <= c2 - c1 < g2:
                arr[i] = (c1, g1, lst + by[k])
                break
        else:
            arr.append((c2, g2, list(by[k])))
    for c, g, lst in arr:
        rows.setdefault((c, (g,)), []).extend(lst)
    return [(k, rows[k]) for k in sorted(rows, key=lambda k: (k[0], k[1]))]


class Namer:
    def __init__(self, t, claims):
        self.t, self.claims = t, claims

    def keys(self, c):
        t = self.t
        out = []
        if t.vt:
            out.append(f'vt:0x{t.vt:08X}{c:+d}')
            for o, v, _f in t.subs:
                out.append(f'vt:0x{v:08X}{c - o:+d}')
        for g in sorted(t.globals):
            out.append(f'g:0x{g:08X}{c:+d}')
        if t.ctor_fn:
            out.append(f'ctor:sub_{t.ctor_fn:08X}{c:+d}')
        return out

    def lodged(self, c):
        want = set(self.keys(c))
        return [r for r in self.claims if r.get('kind') == 'field' and r.get('key') in want]


def gen_name(c, st, kinds, vals, sub_at):
    if sub_at is not None:
        return f'sub_0x{c:X}_vptr'
    if c == 0 and not st and vals and all(v.startswith('0x8') for v in vals):
        return 'vptr'
    k = sorted(kinds)[0] if kinds else 'p'
    k = k.split()[0]
    return f'a_0x{c:X}_x' + 'x'.join(str(g) for g in st) + f'_{k}' if st else f'f_0x{c:X}_{k}'


class Labels:
    """a function's label in the table: its slot numbers in the class (sN) or a subobject's vtable (+OFF.sN), then its name"""
    def __init__(self, sl, src, t):
        self.src = src
        self.slot = {}
        if t.vt:
            for i, f in enumerate(slot_list(src, t.vt)):
                self.slot.setdefault(f, []).append(f's{i}')
            for o, v, _f in t.subs:
                for i, f in enumerate(slot_list(src, v)):
                    self.slot.setdefault(f, []).append(f'+{o}.s{i}')
        for f, role in t.ctors:
            if f not in self.slot:
                self.slot[f] = [role]

    def __call__(self, f):
        s = self.slot.get(f)
        return (s[0] + ':' if s else '') + f'sub_{f:08X}'


def build_rows(t, accs, namer, labels):
    rows = []
    subs_at = {o: v for o, v, _f in t.subs}
    for (c, st), lst in group(accs):
        R = [a for a in lst if a['rw'] == 'R']
        W = [a for a in lst if a['rw'] == 'W']
        P = [a for a in lst if a['rw'] == 'P']
        kinds = set()
        for a in R + W:
            k = a['kind']
            if a['signed'] and k.startswith('u'):
                k = 's' + k[1:]
            kinds.add(k)
        widths = sorted({a['w'] for a in R + W})
        vals = []
        for a in W:
            if a['val'] is not None and a['val'] not in vals:
                vals.append(a['val'])
        sub_at = subs_at.get(c) if not st else None
        lodged = namer.lodged(c) if not st else []
        if lodged:
            r0 = lodged[0]
            name = f'[{r0.get("mark")}] ' + (r0.get('name') or (r0.get('claim') or '')[:60])
        else:
            name = gen_name(c, st, kinds, vals, sub_at)
        idx = []
        for a in lst:
            if a['idx'] and a['idx'] not in idx:
                idx.append(a['idx'])

        def who(xs):
            fs = []
            for a in xs:
                lab = labels(a['f']) + (f' via sub_{a["via"][0]:08X}' if a['via'] else '') + (' [L]' if a['mark'] == 'L' else '')
                if lab not in fs:
                    fs.append(lab)
            return fs
        rows.append({'off': c, 'stride': list(st), 'widths': widths, 'kinds': sorted(kinds), 'reads': len(R), 'writes': len(W),
                     'passed': len(P), 'values': vals, 'name': name, 'lodged': [r.get('key') for r in lodged],
                     'index': idx, 'readers': who(R), 'writers': who(W),
                     'passed_to': sorted({a['callee'] for a in P if a['callee']}),
                     'subobject': hx(sub_at) if sub_at else None,
                     'sites': [f'sub_{a["f"]:08X}+0x{a["o"]:X} {a["rw"]}' for a in lst[:12]]})
    return rows


def off_text(c, st):
    ks = ['k'] if len(st) == 1 else [f'k{i + 1}' for i in range(len(st))]
    return (f'+{c}' if c >= 0 else f'{c}') + ''.join(f' + {g}*{k}' for g, k in zip(st, ks))


def cut_list(xs, n=3):
    return ', '.join(xs[:n]) + (f' +{len(xs) - n}' if len(xs) > n else '')


# ------------------------------------------------------------------ the two answers
def load_claims():
    try:
        import qfactkey
        return qfactkey.load_typed()
    except Exception:  # noqa: BLE001 - no claims folder: generated names only
        return []


def db_names():
    try:
        import q as qmod
        return {a: e.get('name') for a, _f, e in qmod.load_db_entries() if e.get('name')}
    except Exception:  # noqa: BLE001 - no db: sub_X names
        return {}


def struct_of(src, text, resolve=None, claims=None):
    """-> (target, rows, alloc rows, scan) for one CLASS|ROOT"""
    sl = qslice.Slicer(src)
    t = fill_class(sl, src, parse_target(sl, src, text, resolve))
    sc = Scan(sl, src, t)
    accs = sc.run()
    namer = Namer(t, load_claims() if claims is None else claims)
    labels = Labels(sl, src, t)
    rows = build_rows(t, accs, namer, labels)
    al = []
    for f, role in t.ctors:
        if role == 'ctor':
            for a in allocs(sl, src, f):
                al.append(dict(a, ctor=f))
    return t, rows, al, sc


def alloc_text(al, extent):
    parts = []
    for a in al:
        if 'alloc' in a:
            sz = a['size']
            s = f'{sz}' if sz is not None else '?'
            note = f' (< the layout\'s extent {extent}: not its size?)' if sz is not None and sz < extent else ''
            parts.append(f'{s} by sub_{a["alloc"]:08X} in sub_{a["caller"]:08X}{note}')
        elif 'this_of' in a:
            parts.append(f'sub_{a["caller"]:08X} passes its own {a["this_of"]} (a base / member)')
        elif 'stack' in a:
            parts.append(f'on sub_{a["caller"]:08X}\'s stack')
    return parts


def print_struct(t, rows, al, sc, dt, lo=None, hi=None):
    extent = max([r['off'] + max(r['widths'] or [0]) for r in rows if not r['stride']] or [0])
    print(f'q struct {t.label}' + (f'   -- {t.note}' if t.note else ''))
    if t.ctors:
        print('  ctors: ' + ', '.join(f'sub_{f:08X} [{r}]' for f, r in t.ctors))
    a = alloc_text(al, extent)
    if a:
        print('  alloc: ' + '; '.join(a))
    if t.subs:
        print('  subobjects: ' + ', '.join(f'+{o} vtable {hx(v)} (stored by sub_{f:08X})' for o, v, f in t.subs))
    if t.globals:
        print('  instances in: ' + ', '.join(f'[{hx(g)}]' for g in sorted(t.globals)))
    shown = [r for r in rows if (lo is None or r['off'] >= lo) and (hi is None or r['off'] <= hi)]
    print(f'  {len(rows)} fields' + (f' ({len(shown)} in the range)' if len(shown) != len(rows) else '') +
          f'; layout extent {extent} bytes (fixed offsets)')
    print(f'  {"off":<18} {"w":<4} {"kind":<12} {"R":>3} {"W":>3} {"P":>3}  {"name":<30} values / index / by')
    for r in shown:
        ex = []
        if r['values']:
            ex.append('= ' + cut_list(r['values'], 4))
        if r['index']:
            ex.append('k = ' + cut_list(r['index'], 2))
        if r['subobject']:
            ex.append(f'subobject vptr {r["subobject"]}')
        if r['readers']:
            ex.append('R ' + cut_list(r['readers']))
        if r['writers']:
            ex.append('W ' + cut_list(r['writers']))
        if r['passed_to']:
            ex.append('P to ' + cut_list(r['passed_to']))
        w = '/'.join(str(x) for x in r['widths']) or '-'
        k = '/'.join(r['kinds']) or '-'
        print(f'  {off_text(r["off"], r["stride"]):<18} {w:<4} {k:<12} {r["reads"]:>3} {r["writes"]:>3} {r["passed"]:>3}  '
              f'{r["name"][:30]:<30} {" | ".join(ex)}')
    tail = f'({sc.funcs} functions + {sc.callees} callees read, {sc.src.read if hasattr(sc.src, "read") else 0} analysed now, {dt:.1f} s'
    if sc.cut:
        tail += f'; {sc.cut} not read: MAX_FUNCS / MAX_CALLEES'
    if sc.lib_skipped:
        tail += f'; {len(sc.lib_skipped)} library callees (> {LIB_CALLERS} callers) not read inside: their pointer argument is a P access'
    print(tail + ')')


def class_of(src, text, resolve=None, names=None):
    sl = qslice.Slicer(src)
    t = parse_target(sl, src, text, resolve)
    if t.vt is None:
        raise StructError(f'{text}: no class (no vtable): q struct {text} gives its layout')
    t = fill_class(sl, src, t)
    names = db_names() if names is None else names
    vt = t.vt
    slots = slot_list(src, vt)
    out = {'vtable': hx(vt), 'note': t.note, 'slots': [], 'ctors': [], 'parents': [], 'subobjects': [], 'children': [],
           'globals': [hx(g) for g in sorted(t.globals)]}
    for i, f in enumerate(slots):
        out['slots'].append({'slot': i, 'off': 4 * i, 'func': f'sub_{f:08X}', 'name': names.get(f)})
    for f, role in t.ctors:
        out['ctors'].append({'func': f'sub_{f:08X}', 'role': role, 'name': names.get(f)})
        first = True
        for c, v2, callee, rel in ctor_calls_on_this(sl, src, f, vt):
            if c == 0:
                how = f'sub_{f:08X} [{role}]+0x{rel:X} calls sub_{callee:08X} (it stores the vptr {hx(v2)} at this+0) on this' + \
                    (' first' if first and role == 'ctor' else ' after storing ours' if role == 'dtor body' else '')
                p = next((p for p in out['parents'] if p['vtable'] == hx(v2)), None)
                if p is None:
                    out['parents'].append({'vtable': hx(v2), 'how': how, 'mark': 'C'})
                else:
                    p['how'] += '; ' + how
                first = False
            else:
                how = f'sub_{f:08X} [{role}]+0x{rel:X} calls sub_{callee:08X} (it stores the vptr {hx(v2)} at this+0) on this+{c}'
                s = next((s for s in out['subobjects'] if s['off'] == c and s['vtable'] == hx(v2)), None)
                if s is None:
                    out['subobjects'].append({'off': c, 'vtable': hx(v2), 'how': how})
                else:
                    s['how'] += '; ' + how
        if role == 'ctor':
            for s_f, site, _k in src.callers(f)[:MAX_CTOR_CALLERS]:
                S = sl.facts(s_f)
                call = S['calls'].get(site - s_f) if S else None
                if not call or call['regs'].get('r3') is None:
                    continue
                c, terms = sl.lin(S, call['regs']['r3'], None)
                if len(terms) == 1 and terms[0][0] == 1 and terms[0][2][2] == ('in', 'r3'):
                    k = [x for x in sl.this_consts(S).get(0, ()) if src.is_vt(x) and x != vt]
                    if k:
                        out['children'].append({'vtable': hx(k[-1]), 'off': c, 'how': f'sub_{s_f:08X}+0x{site - s_f:X} calls sub_{f:08X} on its this' + (f'+{c}' if c else '')})
    for o, v, f in t.subs:
        if not any(s['off'] == o and s['vtable'] == hx(v) for s in out['subobjects']):
            out['subobjects'].append({'off': o, 'vtable': hx(v), 'how': f'sub_{f:08X} stores the vptr at this+{o}', 'slots': len(slot_list(src, v))})
    # parents by vtable prefix: a vtable every slot of which equals ours at the same index. A partial share is printed only for a
    # parent already named (the base methods it keeps); the other partial shares are counted (a common base's methods are in hundreds)
    share = {}
    for i, f in enumerate(slots):
        for v2, i2 in src.vslots(f):
            if v2 != vt and i2 == i:
                share[v2] = share.get(v2, 0) + 1
    partial = 0
    for v2 in sorted(share):
        s2 = slot_list(src, v2)
        eq = sum(1 for i, f in enumerate(s2) if i < len(slots) and slots[i] == f)
        p = next((p for p in out['parents'] if p['vtable'] == hx(v2)), None)
        if s2 and eq == len(s2) and len(s2) < len(slots):
            how = f'its {len(s2)} slots are our first {len(s2)}: a base by vtable prefix'
        elif s2 and eq == len(s2) == len(slots):
            how = f'the same {eq} slots (an identical table)'
        elif p is not None:
            how = f'{eq} of its {len(s2)} slots are ours at the same index (the rest overridden)'
        else:
            partial += eq >= 2
            continue
        if p is None:
            out['parents'].append({'vtable': hx(v2), 'how': how, 'mark': 'C'})
        else:
            p['how'] += '; ' + how
    out['partial_shares'] = partial
    return t, out


def print_class(out, dt):
    print(f'q class {out["vtable"]}: {len(out["slots"])} slots' + (f'   -- {out["note"]}' if out['note'] else ''))
    for s in out['slots']:
        print(f'  s{s["slot"]:<3} +{s["off"]:<4} {s["func"]}' + (f'  {s["name"]}' if s['name'] else ''))
    print('ctors / dtors (store the vptr at this+0): ' + (', '.join(f'{c["func"]} [{c["role"]}]' + (f' {c["name"]}' if c['name'] else '') for c in out['ctors']) or 'none found'))
    for k, title in (('parents', 'parents'), ('subobjects', 'subobjects'), ('children', 'classes built on it (call its constructor on their this)')):
        print(f'{title}:' + ('' if out[k] else ' none'))
        for r in out[k]:
            head = f'+{r["off"]} ' if 'off' in r and k != 'parents' else ''
            print(f'  {head}{r["vtable"]}' + (f' [{r["mark"]}]' if r.get('mark') else '') + f'  {r["how"]}')
        if k == 'parents' and out.get('partial_shares'):
            print(f'  ({out["partial_shares"]} other vtables share 2+ of our slots at the same index: common base methods, not a parent by'
                  ' themselves; q vtable --of sub_X lists the tables holding a slot)')
    print('globals holding an instance: ' + (', '.join(f'[{g}]' for g in out['globals']) or 'none found'))
    print(f'({dt:.1f} s)')


# ------------------------------------------------------------------ CLI
def _resolver(con):
    def res(s):
        import q as qmod
        try:
            return qmod.resolve(con, s)[0]
        except Exception as ex:  # noqa: BLE001 - q.py run as __main__ owns another QError class
            if type(ex).__name__ == 'QError':
                raise StructError(str(ex))
            raise
    return res


def _range(s):
    m = re.fullmatch(r'(\d+|0x[0-9A-Fa-f]+)-(\d+|0x[0-9A-Fa-f]+)', s or '')
    if not m:
        raise StructError(f'--range {s!r}: LO-HI (decimal or 0xHEX)')
    return int(m.group(1), 0), int(m.group(2), 0)


def main(argv, con):
    import argparse
    ap = argparse.ArgumentParser(prog='q struct', description=__doc__.split('\n')[0])
    ap.add_argument('target', help='0xVTABLE | vt:ADDR | g:ADDR | sub_X | ctor:sub_X')
    ap.add_argument('--range', help='only the offsets LO-HI')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--no-cache', action='store_true', help='re-read every function (ignore out/harness/index/slice/)')
    a = ap.parse_args(argv)
    t0 = time.time()
    src = qslice.QSource(con, use_cache=not a.no_cache)
    t, rows, al, sc = struct_of(src, a.target, _resolver(con))
    import qfield_strides
    qfield_strides.record(t.label, a.target, rows)      # S111 QFIX4: `q field OFF` reports these stride rows
    dt = time.time() - t0
    lo, hi = _range(a.range) if a.range else (None, None)
    if a.json:
        print(json.dumps({'target': t.js(), 'ctors': [{'func': f'sub_{f:08X}', 'role': r} for f, r in t.ctors],
                          'subobjects': [{'off': o, 'vtable': hx(v), 'by': f'sub_{f:08X}'} for o, v, f in t.subs],
                          'alloc': [{k: (f'sub_{v:08X}' if k in ('caller', 'alloc', 'ctor') else v) for k, v in x.items()} for x in al],
                          'rows': [r for r in rows if (lo is None or r['off'] >= lo) and (hi is None or r['off'] <= hi)],
                          'functions': sc.funcs, 'callees': sc.callees, 'seconds': round(dt, 2)}, indent=1))
        return 0
    print_struct(t, rows, al, sc, dt, lo, hi)
    return 0


def main_class(argv, con):
    import argparse
    ap = argparse.ArgumentParser(prog='q class', description='the class picture of a vtable: slots, ctors, parents, subobjects, globals')
    ap.add_argument('target', help='0xVTABLE | vt:ADDR | sub_X (a slot or constructor)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--no-cache', action='store_true')
    a = ap.parse_args(argv)
    t0 = time.time()
    src = qslice.QSource(con, use_cache=not a.no_cache)
    _t, out = class_of(src, a.target, _resolver(con))
    dt = time.time() - t0
    if a.json:
        print(json.dumps(dict(out, seconds=round(dt, 2)), indent=1))
        return 0
    print_class(out, dt)
    return 0
