r"""qclassflow: S110 CLASSFLOW (docs\research\s110\CLASSFLOW.md). Narrows the indirect call sites that ICALL leaves at "every table with a function at this slot"
(status offset, and the embedded-field candidates) by following the receiver's class across functions. Sound rules only; 0 game runs.

  a  `this` of a function = the tables that list it           (always together with e; a function whose address is built in code or listed by a 'run' table: unknown)
  e  the direct callers' r3..r8 (calls and tail branches)      (any caller whose value is unknown: unknown)
  b  a pointer field keyed (owner class, offset): the values stored there by every `stw`; a ctor call on the object in the same function; a ctor that returns
     `this` (also through a `b __restgprlr` tail). ONE unknown stored value (or one store whose owner has no known class) poisons the field: unknown.
     S110 review: a FIELD read is only a candidate ([L]): stwx / std / stvx / memcpy and a store through an interior pointer a callee gets are not seen, so they
     cannot poison; a field of a computed address (an array element) is no answer at all. The ctor / dtor calls on one object give the UNION of their classes
  c  an embedded table keyed (owner class, offset), plus the bases' entries (the ctor chain); every owner class must have one
  d  return values (through tail branches and through indirect calls that resolve to <= 8 functions)
  f  globals / static objects: their stores, static ctor calls, the image word. Escape is NOT checked: [L]. Such a site is only a CANDIDATE (status cand)
  h  stack slots whose address does not escape (args r3..r10; the slot, and every slot above an escaped address, is given up). [L] like b: a candidate
The `drop unknown stores` mode of the probe is not built.

Entry: build(index_con, gen, icalls, itargets, facts) -> (icalls, itargets, text), called by qx7.build after resolve_sites: a site of status offset / cand whose slot
targets are <= 8 becomes exact / narrow with icall_target.how = the rule letters (a result through f, a field read or h: status cand, how = letters + 'L'). The facts are recorded by the class
pass of qx7 (run_function with a recorder: null-check join phi(x, 0) = x; every stw; the arguments at each call; r3 at each return; tail branches).
    py -I tools\q\qclassflow.py --selftest      synthetic units (each rule with its unsound twin), then the totals / pins of a built vcall db
"""
import collections
import os
import re
import sqlite3
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qx7  # noqa: E402

ARGS = ('r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10')      # S110 CLASSFLOW review: the PPC argument registers run to r10 (a stack address passed in r9 escapes)
THIS = ('in', 'r3')
M32 = 0xFFFFFFFF
LETTERS = 'abcdefh'
BIT = {c: 1 << i for i, c in enumerate(LETTERS)}
A, B, C, D, E, F, H = (BIT[c] for c in LETTERS)
MAXSET = 8
DEPTH = 10
SHAPES = ('V_this', 'V_open', 'F')
CODE_OPS = "('lis+addi', 'lis+ori', 'lis+addi?', 'lis+ori?')"
# S110 CLASSFLOW review: a field is read off a value that names an object pointer; a computed address (`add` of an index: 'unk'; a join: 'phi'; a shift) is an array
# element or worse, and "every store at +off" is then no set of the values there (sub_83200D48 0x83200E18: r30 + i * 64 + 6628; coverage run 083 saw another target)
OBJ_ROOTS = ('in', 'lwz', 'lwzx', 'ret', 'glob')
G = 1 << 7      # internal: the result rests on a pointer field (rule b's field read): stores the recorder does not see are not excluded -> candidate, like f
SOFT = F | G | H      # h too: a stack slot is also written by std / stfd / stvx (struct copies) and indexed stores, which the recorder does not see


def letters(mask):
    return ''.join(c for c in LETTERS if mask & BIT[c])


# ------------------------------------------------------------------ the recorder (the class pass of qx7.run_function)
class Rec:
    """Tracker.rec: every stw that is not a literal, r3 at every return, the direct tail branches with their arguments"""
    __slots__ = ('funcs', 'rst', 'rets', 'tails')

    def __init__(self, funcs):
        self.funcs = funcs
        self.rst, self.rets, self.tails = [], [], []

    def on_stw(self, addr, src, base, off):
        if src[0] != 'li':
            self.rst.append((addr, src, base, off))

    def on_branch(self, addr, mn, p, s):
        m = mn.rstrip('+-')
        if m != 'bl' and m.endswith('lr') and not m.endswith('ctrl'):      # blr and the conditional returns
            self.rets.append(s.get('r3'))
        elif m == 'b' and p:
            t = qx7._imm(p[0])
            if t in self.funcs:
                self.tails.append((addr, t, tuple(s.get(r) for r in ARGS)))


def func_facts(fa, insns, vt_ok, helpers, funcs):
    """the class pass over one function -> (indirect sites [(addr, CTR def)], direct calls [(addr, target, r3..r8 defs)], r3 at each return, tails [(addr, target, args)],
    stores [(addr, src def, base def, offset)])"""
    cases = {x[0] for x in insns if x[1] == 'bctr' and x[3]}
    sites, calls = [], []

    def hit(kind, a, info, tr):
        if kind == 'call':
            tgt, st = info
            if tgt is not None:
                calls.append((a, tgt, tuple(st.get(r) for r in ARGS)))
        elif a not in cases and info[0] is not None:
            sites.append((a, info[0]))
    tr = qx7.run_function(insns, vt_ok, helpers, hit, None, lambda: Rec(funcs))
    r = tr.rec
    return (sites, calls, r.rets, r.tails, r.rst)


# ------------------------------------------------------------------ the world and the evaluator
class World:
    def __init__(self, funcs, vtabs, runfuncs, codefn, helpers, facts, word):
        self.funcs, self.vtabs, self.runfuncs, self.codefn, self.helpers, self.facts, self.word = funcs, vtabs, runfuncs, codefn, helpers, facts, word


def norm(e):
    """an address expression -> (root, offset)"""
    if e[0] == 'addi':
        return e[1], e[2]
    return e, 0


def is_stack(r):
    return (r[0] == 'unk' and len(r) > 2 and r[2] == 'r1') or r == ('in', 'r1')


def tag(v, bit):
    return None if v is None else (v[0], v[1] | bit)


def U(vals):
    """union of (tables, rule mask) values; one unknown (None) makes it unknown; nothing makes it unknown"""
    out, m = set(), 0
    for v in vals:
        if v is None:
            return None
        out |= v[0]
        m |= v[1]
    return (frozenset(out), m) if out else None


class CF:
    def __init__(self, w, depth=DEPTH):
        self.w, self.dmax = w, depth
        self.vtabs = w.vtabs
        self.memo, self.fcache, self.cc = {}, {}, {}
        self.depth = self.cut = 0
        facts = w.facts
        self.member = collections.defaultdict(set)
        for s, e in self.vtabs.items():
            for f in e:
                self.member[f].add(s)
        self.bad = frozenset(w.codefn) | frozenset(w.runfuncs)      # a function whose address is built in code / listed by a 'run' table: its `this` is no class
        self.taken = self.bad | frozenset(self.member)
        self.callers = collections.defaultdict(list)
        self.callinfo, self.siteinfo = {}, {}
        self.localtab = collections.defaultdict(lambda: collections.defaultdict(list))
        self.stores_by_off = collections.defaultdict(list)
        self.stack_st = collections.defaultdict(lambda: collections.defaultdict(list))
        self.esc = {}                                    # F -> lowest stack offset whose address escapes (a call argument or a stored value)
        self.glob_st = collections.defaultdict(list)
        self.ctor_calls_on = collections.defaultdict(list)
        self.static_ctor = collections.defaultdict(list)
        for fa, (sites, calls, rets, tails, rst) in facts.items():
            for a, ctr in sites:
                self.siteinfo[(fa, a)] = ctr
            for site, cal, args in calls + list(tails):
                self.callers[cal].append((fa, site, args))
                self.callinfo[(fa, site)] = (cal, args)
                r, o = norm(args[0])
                self.ctor_calls_on[fa].append((site, cal, (r, o)))
                if r[0] == 'const':
                    self.static_ctor[(r[1] + o) & M32].append(cal)
                for x in args:
                    self._escape(fa, x)
            for addr, src, base, off in rst:
                self._escape(fa, src)
                r, o = norm(base)
                o += off
                if src[0] == 'const' and src[1] in self.vtabs:
                    self.localtab[fa][(r, o)].append((addr, src[1]))
                if r[0] in ('lis', 'const'):
                    self.glob_st[(r[1] + o) & M32].append((fa, src))
                elif is_stack(r):
                    self.stack_st[fa][(r, o)].append(src)
                else:
                    self.stores_by_off[o].append((fa, r, src))
        # rule c: embedded tables keyed (owner class, offset), and the ctor chain
        self.emb = collections.defaultdict(set)
        self.bases = collections.defaultdict(set)
        for g in facts:
            c = self.ctorclass(g)
            if not c:
                continue
            for (r, o), lst in (self.localtab[g].items() if g in self.localtab else ()):
                if r == THIS and o:
                    for t in c:
                        self.emb[(t, o)].update(x for _, x in lst)
            for site, cal, (r, o) in self.ctor_calls_on.get(g, ()):
                if r == THIS:
                    cc = self.ctorclass(cal)
                    if cc:
                        for t in c:
                            if o:
                                self.emb[(t, o)].update(cc)
                            elif cc != c:
                                self.bases[t].update(cc - c)

    def _escape(self, f, x):
        r, o = norm(x)
        if is_stack(r):
            self.esc[f] = min(self.esc.get(f, o), o)

    def returns_this(self, g):
        v = self.w.facts.get(g)
        if v is None or not (v[2] or v[3]):
            return False
        return all(r == THIS for r in v[2]) and all(t in self.w.helpers and ar[0] == THIS for a, t, ar in v[3])

    def ctorclass(self, g, depth=0):
        """the table a function leaves at 0(this): the last of its own stores there and of its calls of a function that does (r3 = this)"""
        if g in self.cc:
            return self.cc[g]
        if depth > 40:
            return None
        self.cc[g] = None
        ev = [(a, frozenset([t])) for a, t in self.localtab[g].get((THIS, 0), [])] if g in self.localtab else []
        for site, cal, loc in self.ctor_calls_on.get(g, ()):
            if loc == (THIS, 0):
                c = self.ctorclass(cal, depth + 1)
                if c:
                    ev.append((site, c))
        res = max(ev, key=lambda x: x[0])[1] if ev else None
        self.cc[g] = res
        return res

    def emb_star(self, t, n, seen=None):
        seen = seen or set()
        if t in seen:
            return set()
        seen.add(t)
        out = set(self.emb.get((t, n), ()))
        for b in self.bases.get(t, ()):
            out |= self.emb_star(b, n, seen)
        return out

    def _memo(self, key, fn, *a):
        if key in self.memo:
            return self.memo[key]
        if self.depth >= self.dmax:
            self.cut += 1
            return None
        self.memo[key] = None          # a cycle answers unknown
        self.depth += 1
        try:
            r = fn(*a)
        finally:
            self.depth -= 1
        self.memo[key] = r
        return r

    def val(self, g, e):
        r, o = norm(e)
        return self.tabat(g, r, o)

    def tabat(self, f, root, n):
        return self._memo(('T', f, root, n), self._tabat, f, root, n)

    def _tabat(self, f, root, n):
        ev = [(a, frozenset([t]), 0) for a, t in self.localtab[f].get((root, n), ())] if f in self.localtab else []      # the stores of this function at (root, n)
        for site, cal, loc in self.ctor_calls_on.get(f, ()):                    # rule b: a ctor call on the object in this function
            if loc == (root, n):
                c = self.ctorclass(cal)
                if c:
                    ev.append((site, c, B))
        if n == 0 and root[0] == 'ret':
            cal = root[2]
            if cal is not None and self.returns_this(cal) and self.ctorclass(cal):
                ev.append((root[1], self.ctorclass(cal), B))
        if ev:      # S110 CLASSFLOW review: the union. Address order is not the order at the call: a dtor after the use leaves the base's table (sub_827BFB88:
            # ctor 0x827C5C90, the call, dtor 0x82B04680 on r1+80; run 083 saw the derived slot), and two branches can construct two classes
            return (frozenset().union(*(x[1] for x in ev)), max(x[2] for x in ev))
        if n == 0:
            return self.cls(f, root)
        owner = self.tabat(f, root, 0)                                          # rule c
        if owner is not None:
            out = set()
            for t in owner[0]:
                e = self.emb_star(t, n)
                if not e:
                    return None      # S110 CLASSFLOW review: one owner class with no known table at +n: its object may hold any (not the others' tables alone)
                out |= e
            if out:
                return (frozenset(out), owner[1] | C)
        return None

    def cls(self, f, x):
        k = x[0]
        if x == THIS:
            return self.this(f)
        if k == 'in':
            if f in self.taken or not self.callers.get(f):
                return None
            i = int(x[1][1:]) - 3
            if not 0 <= i < len(ARGS):
                return None
            return tag(U(self.val(c, a[i]) for c, s, a in self.callers[f]), E)
        if k == 'lwz':
            r, o = norm(x[2])
            o += x[1]
            if is_stack(r):                                                     # rule h
                if o >= self.esc.get(f, 1 << 30):
                    return None
                srcs = self.stack_st[f].get((r, o)) if f in self.stack_st else None
                return tag(U(self.val(f, s) for s in srcs), H) if srcs else None
            if r[0] in ('lis', 'const'):
                return self.glob((r[1] + o) & M32)
            return self._memo(('F', f, r, o), self.field, f, r, o)
        if k == 'ret':
            if x[2] is not None:
                return tag(self._memo(('R', f, x[1], x[2]), self.ret, f, x[1], x[2]), D)
            c = self.siteinfo.get((f, x[1]))
            if c is None or c[0] != 'lwz' or c[2][0] != 'lwz' or c[2][1] != 0:
                return None
            r, o = norm(c[2][2])
            S = self.slots(self.tabat(f, r, o), c[1])
            if S is None:
                return None
            v = U(self._memo(('R', f, x[1], g), self.ret, f, x[1], g) for g in sorted(S[0]))
            return (v[0], v[1] | S[1] | D) if v else None
        if k == 'glob':
            return self.glob(x[1])
        if k == 'const':                                                        # rule f: a static object
            a = x[1]
            out = {s[1] for g, s in self.glob_st.get(a, ()) if s[0] == 'const' and s[1] in self.vtabs}
            for cal in self.static_ctor.get(a, ()):
                c = self.ctorclass(cal)
                if c:
                    out |= c
            w = self.w.word(a)
            if w in self.vtabs:
                out.add(w)
            return (frozenset(out), F) if out else None
        return None

    def glob(self, a):
        st = self.glob_st.get(a)
        if not st:
            return None
        return tag(self._memo(('G', a), lambda: U(self.val(g, s) for g, s in st)), F)

    def field(self, f, root, o):
        if root[0] not in OBJ_ROOTS:
            return None
        owner = self.tabat(f, root, 0)
        key = (owner[0] if owner else None, o)
        if key not in self.fcache:
            self.fcache[key] = self._field(owner, o)
        v = self.fcache[key]
        return (v[0], v[1] | B | G | (owner[1] if owner else 0)) if v else None

    def _field(self, owner, o):
        parts = []
        for g, gr, src in self.stores_by_off.get(o, ()):
            if owner is not None:
                og = self.tabat(g, gr, 0)
                if og is not None and not (og[0] & owner[0]):
                    continue                    # a store through an owner of another class
            v = self.val(g, src)                # a store through an owner of unknown class counts: an unknown value poisons the field
            if v is None:
                return None
            parts.append(v)
        return U(parts)

    def ret(self, f, site, g):
        v = self.w.facts.get(g)
        if v is None or (not v[2] and not v[3]):
            return None
        ci = self.callinfo.get((f, site))
        parts = []
        for r in v[2]:
            if r[0] == 'in' and r[1] in ARGS:                  # the callee returns its own argument: the value this call passed
                parts.append(self.val(f, ci[1][ARGS.index(r[1])]) if ci else None)
            else:
                parts.append(self.val(g, r))
        for a, t, ar in v[3]:
            parts.append(self._memo(('R', g, a, t), self.ret, g, a, t))
        return U(parts)

    def this(self, f):
        return self._memo(('H', f), self._this, f)

    def _this(self, f):
        parts = []
        m = self.member.get(f)
        if m:
            if f in self.bad:
                return None
            parts.append((frozenset(m), A | E))
        elif f in self.taken:
            return None
        for c, s, a in self.callers.get(f, ()):
            parts.append(tag(self.val(c, a[0]), E))
        return U(parts)

    def slots(self, S, off):
        """the words at +off of each table of S -> (set of functions, mask), or None when a table does not reach the slot or the set is wider than 8"""
        if S is None:
            return None
        out = set()
        for t in S[0]:
            e = self.vtabs.get(t)
            if e is None or off // 4 >= len(e):
                return None
            out.add(e[off // 4])
        if not out or len(out) > MAXSET:
            return None
        return out, S[1]

    def eval_site(self, fa, ctr):
        """the targets of one indirect call (CTR def) -> (set of functions, rule mask) or None. A result that needs no rule (mask 0: only stores of the same function, read
        without their order) is not taken: the same-function case is V_exact's"""
        if ctr is None or ctr[0] != 'lwz':
            return None
        off, bd = ctr[1], ctr[2]
        if bd[0] == 'lwz' and bd[1] == 0:
            r, o = norm(bd[2])
            res = self.slots(self.tabat(fa, r, o), off)
        elif bd[0] in ('const', 'lis'):
            return None
        else:                                                                       # a function pointer in a struct field
            r, o = norm(bd)
            if r[0] in ('lis', 'const') or is_stack(r):
                return None
            res = self.fieldptrs(fa, r, off + o)
        return res if res and res[1] else None

    def fieldptrs(self, fa, root, off):
        if root[0] not in OBJ_ROOTS:
            return None
        owner = self.tabat(fa, root, 0)
        vals = set()
        for g, gr, src in self.stores_by_off.get(off, ()):
            if owner is not None:
                og = self.tabat(g, gr, 0)
                if og is not None and not (og[0] & owner[0]):
                    continue
            if src[0] == 'const' and src[1] in self.w.funcs:
                vals.add(src[1])
            else:
                return None
        if not vals or len(vals) > MAXSET:
            return None
        return vals, B | G | (owner[1] if owner else 0)


# ------------------------------------------------------------------ the pass over the sites
def refine(cf, icalls, itargets):
    """-> (icalls, itargets, stats). Population: status offset / cand of shape V_this / V_open / F."""
    new_t, out, st = {}, [], collections.Counter()
    for row in icalls:
        a, fa, kind, shape, off, tg, status, n = row
        if status in ('offset', 'cand') and shape in SHAPES:
            st['pop'] += 1
            r = cf.eval_site(fa, cf.siteinfo.get((fa, a)))
            if r:
                ts, mask = r
                if mask & SOFT:      # f, a pointer field or a stack slot: unseen writes not excluded (S110 CLASSFLOW review) -> a candidate, never exact / narrow
                    if status == 'offset':
                        row = (a, fa, kind, shape, off, tg, 'cand', len(ts))
                        new_t[a] = (ts, letters(mask) + 'L')
                        st['f-cand'] += 1
                else:
                    row = (a, fa, kind, shape, off, tg, 'exact' if len(ts) == 1 else 'narrow', len(ts))
                    new_t[a] = (ts, letters(mask))
                    st['exact' if len(ts) == 1 else 'narrow'] += 1
                    if status == 'cand':
                        st['from-cand'] += 1
        out.append(row)
    targets = [t for t in itargets if t[0] not in new_t] + [(a, x, how) for a, (ts, how) in sorted(new_t.items()) for x in sorted(ts)]
    st['cut'] = cf.cut
    return out, targets, st


def world_from_index(con, img, facts):
    funcs = frozenset(r[0] for r in con.execute('SELECT addr FROM funcs'))
    vtabs, runfuncs = {}, set()
    for s, rule, n in con.execute('SELECT start, rule, len FROM tables'):
        ents = [img.word(s + 4 * i) for i in range(n)]
        if rule == 'run':
            runfuncs.update(x for x in ents if x is not None)
        else:
            vtabs[s] = ents
    codefn = frozenset(r[0] for r in con.execute(f'SELECT DISTINCT value FROM imms WHERE op IN {CODE_OPS}')) & funcs
    # S110 CLASSFLOW review: a data word outside the cataloged tables that points at a function (a callback struct, a table the catalog missed) lets an indirect call
    # nobody resolved reach it with any `this` / arguments: rule a / e may not list only its direct callers
    intab = {s + 4 * i for s, rule, n in con.execute('SELECT start, rule, len FROM tables') for i in range(n)}
    codefn |= frozenset(v for at, v in con.execute("SELECT at, value FROM ptrs WHERE kind = 'func'") if at not in intab) & funcs
    helpers = frozenset(r[0] for r in con.execute("SELECT addr FROM funcs WHERE name GLOB '__save*' OR name GLOB '__rest*'"))
    return World(funcs, vtabs, frozenset(runfuncs), codefn, helpers, facts, img.word)


def build(index_con, gen, icalls, itargets, facts, img=None):
    """entry (qx7.build): -> (icalls, itargets, text for the meta table). Runs on a thread with a large stack (the evaluator recurses)."""
    t0 = time.time()
    res = {}
    w = world_from_index(index_con, img or qx7.Img(), facts)      # on this thread: the sqlite connection belongs to it

    def go():
        try:
            cf = CF(w)
            res['v'] = refine(cf, icalls, itargets)
        except BaseException as e:      # noqa: BLE001  re-raised below
            res['e'] = e
    sys.setrecursionlimit(100000)
    threading.stack_size(64 * 1024 * 1024)
    th = threading.Thread(target=go)
    th.start()
    th.join()
    if 'e' in res:
        raise res['e']
    ic, it, st = res['v']
    text = ', '.join(f'{k} {v}' for k, v in sorted(st.items())) + f'; {time.time() - t0:.1f} s'
    return ic, it, text


# ------------------------------------------------------------------ selftest
class Fail(Exception):
    pass


def check(cond, what, out=''):
    if not cond:
        raise Fail(f'{what}  {out}')
    print(f'PASS  {what}')


def _asm(base, lines):
    out, a = [], base
    for ln in lines:
        mn, _, ops = ln.partition(' ')
        out.append([a, mn, ops.strip(), None])
        a += 4
    return out


def _world(code, vtabs, runfuncs=(), codefn=(), helpers=()):
    funcs = set(code) | set(helpers) | {x for e in vtabs.values() for x in e}
    vt = set(vtabs)
    facts = {fa: func_facts(fa, _asm(fa, ln), lambda a: a in vt, frozenset(helpers), frozenset(funcs)) for fa, ln in code.items()}
    return CF(World(frozenset(funcs), vtabs, frozenset(runfuncs), frozenset(codefn), frozenset(helpers), facts, lambda a: None))


TA, TB, TC, TE = 0x82010000, 0x82010100, 0x82010300, 0x82010200
FA, FB, FS, FE = 0x82101000, 0x82102000, 0x82106000, 0x82103000
VT4 = ['lwz r11,0(r31)', 'lwz r10,4(r11)', 'mtctr r10', 'mr r3,r31', 'bctrl', 'blr']      # a virtual call, slot +4, receiver r31
TBL = {TA: [0x82100010, FA], TB: [0x82100020, FB], TE: [0x82100030, FE], TC: [0x82100040, FS]}
LISB = ['lis r9,-32255', 'addi r9,r9,256']      # r9 = TB
ALLOC = 0x82112000


def _res(cf, fa):
    a, ctr = cf.w.facts[fa][0][0]
    r = cf.eval_site(fa, ctr)
    return (r[0], letters(r[1])) if r else None


def unit_checks():
    # the recorder's null-check join
    a = qx7.State({'r31': ('in', 'r4')})
    b = qx7.State({'r31': ('li', 0)})
    check(qx7.meet(a, b, 5, True).get('r31') == ('in', 'r4') and qx7.meet(b, a, 5, True).get('r31') == ('in', 'r4'), 'unit null join: phi(x, li 0) = x in the class pass')
    check(qx7.meet(a, b, 5).get('r31') == ('phi', 5, 'r31'), 'unit null join (twin): the ICALL pass keeps the phi')
    c = qx7.State({'r31': ('in', 'r5')})
    check(qx7.meet(a, c, 5, True).get('r31') == ('phi', 5, 'r31'), 'unit null join (twin): two different non-null values stay a phi')
    # a + e: this of a function only a table lists
    site = ['lwz r11,0(r3)', 'lwz r10,4(r11)', 'mtctr r10', 'bctrl', 'blr']
    cf = _world({FA: site}, TBL)
    check(_res(cf, FA) == ({FA}, 'ae'), 'unit a+e: `this` of a slot function nothing calls = the slot of the table that lists it', _res(cf, FA))
    cf = _world({FA: site, 0x82120000: ['lwz r3,8(r4)', 'bl 0x82101000', 'blr']}, TBL)
    check(_res(cf, FA) is None, 'unit a+e (twin): a direct caller that passes an unknown r3 makes `this` unknown', _res(cf, FA))
    cf = _world({FA: site}, TBL, runfuncs={FA})
    check(_res(cf, FA) is None, "unit a (twin): a function listed by a 'run' table is no class's slot", _res(cf, FA))
    cf = _world({FA: site}, TBL, codefn={FA})
    check(_res(cf, FA) is None, 'unit a (twin): a function whose address is built in code may be a callback', _res(cf, FA))
    # e: an argument
    h0 = ['lwz r11,0(r4)', 'lwz r10,4(r11)', 'mtctr r10', 'mr r3,r4', 'bctrl', 'blr']
    c1 = LISB + ['stw r9,0(r5)', 'mr r4,r5', 'bl 0x82104000', 'blr']
    cf = _world({0x82104000: h0, 0x82105000: c1}, TBL)
    check(_res(cf, 0x82104000) == ({FB}, 'e'), 'unit e: the objects the direct callers pass in r4', _res(cf, 0x82104000))
    cf = _world({0x82104000: h0, 0x82105000: c1, 0x82105100: ['lwz r4,8(r3)', 'bl 0x82104000', 'blr']}, TBL)
    check(_res(cf, 0x82104000) is None, 'unit e (twin): one caller with an unknown object', _res(cf, 0x82104000))
    # b: a pointer field
    s = ['lwz r31,8(r3)'] + VT4
    mk = LISB + ['stw r9,0(r4)', 'stw r4,8(r3)', 'blr']
    cf = _world({FS: s, 0x82107000: mk}, TBL)
    check(_res(cf, FS) == ({FB}, 'abe'), 'unit b: the value stored in field +8 of an owner of the known class', _res(cf, FS))
    cf = _world({FS: s, 0x82107000: mk, 0x82108000: ['stw r5,8(r6)', 'blr']}, TBL)
    check(_res(cf, FS) is None, 'unit b (twin): one store of an unknown value at +8 (owner of no known class) poisons the field', _res(cf, FS))
    cf = _world({FS: s, 0x82107000: mk, 0x82108000: ['stw r5,8(r6)', 'blr'], 0x82108100: ['stw r5,8(r3)', 'blr']}, TBL)
    check(_res(cf, FS) is None, 'unit b (twin): an unknown value stored at +8 through an owner of unknown class', _res(cf, FS))
    tm = {**TBL}
    cf = _world({FS: s, 0x82107000: mk, 0x82108200: ['stw r5,8(r3)', 'blr']}, {**tm, TA: [0x82108200, FA]})
    check(_res(cf, FS) == ({FB}, 'abe'), 'unit b: an unknown store through an owner of ANOTHER known class does not poison', _res(cf, FS))
    # b: ctor result returned through a restore-helper tail
    HELP = 0x82115000
    k = LISB + ['stw r9,0(r3)', 'b 0x82115000']
    sk = ['bl 0x82114000', 'mr r31,r3'] + VT4
    cf = _world({FA: sk, 0x82114000: k}, TBL, helpers=[HELP])
    check(_res(cf, FA) == ({FB}, 'b'), 'unit b: a ctor that returns this through a `b __restgprlr` tail', _res(cf, FA))
    cf = _world({FA: sk, 0x82114000: LISB + ['stw r9,0(r3)', 'li r3,0', 'b 0x82115000']}, TBL, helpers=[HELP])
    check(_res(cf, FA) is None, 'unit b (twin): the tail passes something else than this', _res(cf, FA))
    # c: an embedded table keyed (owner class, offset)
    ca = ['lis r9,-32255', 'addi r9,r9,0', 'stw r9,0(r3)', 'lis r9,-32255', 'addi r9,r9,512', 'stw r9,16(r3)', 'blr']
    emb = ['addi r31,r3,16'] + VT4
    cf = _world({FA: emb, 0x82109000: ca}, TBL)
    check(_res(cf, FA) == ({FE}, 'ace'), 'unit c: the table the ctor of the owner class stores at this+16', _res(cf, FA))
    cf = _world({FA: ['addi r31,r3,20'] + VT4, 0x82109000: ca}, TBL)
    check(_res(cf, FA) is None, 'unit c (twin): no table of the owner class at +20', _res(cf, FA))
    rows = [(0x82001000, FA, 'call', 'V_open', 4, '', 'cand', 3), (0x82001010, FA, 'call', 'V_open', 4, '', 'offset', 3)]
    cf = _world({FA: emb, 0x82109000: ca}, TBL)
    cf.siteinfo[(FA, 0x82001000)] = cf.siteinfo[(FA, 0x82001010)] = cf.w.facts[FA][0][0][1]
    ic, it, st = refine(cf, rows, [(0x82001000, 0x82103000, 'embedded-field')])
    check(ic[0][6] == 'exact' and ic[1][6] == 'exact' and (0x82001000, FE, 'ace') in it and not any(t[2] == 'embedded-field' for t in it),
          'unit c: a cand (embedded-field) site whose class rule c proves moves up to exact with how = ace', (ic, it))
    # d: return values
    mk2 = ['mflr r0', 'bl 0x82112000', 'mr r31,r3'] + LISB + ['stw r9,0(r31)', 'mr r3,r31', 'blr']
    sd = ['bl 0x82111000', 'mr r31,r3'] + VT4
    cf = _world({FA: sd, 0x82111000: mk2}, TBL)
    check(_res(cf, FA) == ({FB}, 'd'), 'unit d: the object a callee returns', _res(cf, FA))
    mk3 = ['bl 0x82112000', 'mr r31,r3'] + LISB + ['stw r9,0(r31)', 'mr r3,r31', 'cmpwi cr6,r4,0', 'bnelr cr6', 'lwz r3,4(r4)', 'blr']
    cf = _world({FA: sd, 0x82111000: mk3}, TBL)
    check(_res(cf, FA) is None, 'unit d (twin): a second return path with an unknown value', _res(cf, FA))
    # f: a global; only a candidate
    sg = ['lis r9,-32000', 'lwz r31,16(r9)'] + VT4
    wg = ['bl 0x82112000', 'mr r30,r3'] + LISB + ['stw r9,0(r30)', 'lis r8,-32000', 'stw r30,16(r8)', 'blr']
    cf = _world({FA: sg, 0x82110000: wg}, TBL)
    check(_res(cf, FA) == ({FB}, 'f'), 'unit f: the object stored in a global', _res(cf, FA))
    ic, it, st = refine(cf, [(cf.w.facts[FA][0][0][0], FA, 'call', 'V_open', 4, '', 'offset', 3)], [])
    check(ic[0][6] == 'cand' and it and it[0][2] == 'fL', 'unit f: the site is a candidate with how fL, never exact / narrow', (ic, it))
    cf = _world({FA: sg}, TBL)
    check(_res(cf, FA) is None, 'unit f (twin): a global nothing stores to', _res(cf, FA))
    # h: a stack slot
    sh = ['bl 0x82112000', 'mr r30,r3'] + LISB + ['stw r9,0(r30)', 'stw r30,80(r1)', 'lwz r31,80(r1)'] + VT4
    cf = _world({FA: sh}, TBL)
    check(_res(cf, FA) == ({FB}, 'h'), 'unit h: an object kept in a stack slot', _res(cf, FA))
    sh2 = ['bl 0x82112000', 'mr r30,r3'] + LISB + ['stw r9,0(r30)', 'stw r30,80(r1)', 'addi r4,r1,64', 'bl 0x82113000', 'lwz r31,80(r1)'] + VT4
    cf = _world({FA: sh2}, TBL)
    check(_res(cf, FA) is None, 'unit h (twin): the address of a slot below it escapes to a call', _res(cf, FA))
    review_checks()


def _ref(cf, fa, shape='V_open', status='offset'):
    """the site of fa through refine -> (status, how of its targets)"""
    a, ctr = cf.w.facts[fa][0][0]
    ic, it, st = refine(cf, [(a, fa, 'call', shape, ctr[1], '', status, 3)], [])
    return ic[0][6], sorted({t[2] for t in it if t[0] == a})


def review_checks():
    """S110 CLASSFLOW review: each planted case gave a wrong answer that read as certain on the code before the review (two of them seen by the game: coverage run 083)"""
    LISFA = ['lis r9,-32240', 'addi r9,r9,4096']      # r9 = FA (a function)
    # 1. a function pointer read at +6628 of a COMPUTED address (r30 + index * 64: sub_83200D48 0x83200E18; the run saw 0x831FFA20 where rule b said exact 0x831E4100)
    site = ['rlwinm r11,r4,6,0,25', 'add r9,r11,r3', 'lwz r8,6628(r9)', 'mtctr r8', 'bctrl', 'blr']
    cf = _world({FS: site, 0x82107000: LISFA + ['stw r9,6628(r5)', 'blr']}, TBL)
    check(_res(cf, FS) is None, 'review: a field of a computed address (an array element) is no object field: no rule b answer', _res(cf, FS))
    # 2. rule b names an exact target while a store it cannot see (through an interior pointer a callee gets) writes another class there: only a candidate
    s = ['lwz r31,8(r3)'] + VT4
    mk = LISB + ['stw r9,0(r4)', 'stw r4,8(r3)', 'blr']
    alias = ['addi r4,r3,8', 'bl 0x82108400', 'blr']            # sub_82108400 stores through r4 = this+8 (keyed at +0, not +8)
    cf = _world({FS: s, 0x82107000: mk, 0x82108300: alias, 0x82108400: ['mr r31,r4', 'bl 0x82112000', 'stw r3,0(r31)', 'blr']}, TBL)
    check(_ref(cf, FS) == ('cand', ['abeL']), 'review: a result read from a pointer field (rule b) is a candidate (how ...L), never exact: unseen stores (interior pointer, stwx, std, memcpy)', _ref(cf, FS))
    # 3. the last ctor / dtor call by address is not the class at the call: a stack object constructed (TB), used, then destroyed (the dtor leaves TA)
    #    (sub_827BFB88: ctor 0x827C5C90, the call, dtor 0x82B04680 on r1+80; the run saw 0x827C5CA0 where rule b+e said the base's 0x82C79E70)
    CT, DT = 0x82108500, 0x82108600
    so = ['addi r3,r1,80', 'bl 0x82108500', 'addi r31,r1,80', 'lwz r11,0(r31)', 'lwz r10,4(r11)', 'mtctr r10', 'mr r3,r31', 'bctrl', 'addi r3,r1,80', 'bl 0x82108600', 'blr']
    cf = _world({FA: so, CT: LISB + ['stw r9,0(r3)', 'blr'], DT: ['lis r9,-32255', 'addi r9,r9,0', 'stw r9,0(r3)', 'blr']}, TBL)
    check(_res(cf, FA) == ({FA, FB}, 'b'), 'review: the classes of every ctor / dtor call on the object (union), not the last by address', _res(cf, FA))
    # 4. rule c: an owner class whose embedded table at +16 is not known makes the answer unknown, not the other classes' tables alone
    G2 = 0x82107800
    tb2 = {**TBL, TA: [G2, FA], TB: [G2, FB]}
    ca = ['lis r9,-32255', 'addi r9,r9,0', 'stw r9,0(r3)', 'lis r9,-32255', 'addi r9,r9,512', 'stw r9,16(r3)', 'blr']
    cf = _world({G2: ['addi r31,r3,16'] + VT4, 0x82109000: ca}, tb2)
    check(_res(cf, G2) is None, 'review: rule c with owner {TA, TB} and an embedded table known for TA only: unknown', _res(cf, G2))
    # 5. rule h: a stack address passed in r9 (an argument register past r8) escapes
    sh3 = ['bl 0x82112000', 'mr r30,r3'] + LISB + ['stw r9,0(r30)', 'stw r30,80(r1)', 'addi r9,r1,64', 'bl 0x82113000', 'lwz r31,80(r1)'] + VT4
    cf = _world({FA: sh3}, TBL)
    check(_res(cf, FA) is None, 'review: rule h: a slot address passed in r9 escapes', _res(cf, FA))
    # 6. rule h is a candidate: a stack slot can be written by std / stfd / stvx (struct copies) the recorder does not see
    sh = ['bl 0x82112000', 'mr r30,r3'] + LISB + ['stw r9,0(r30)', 'stw r30,80(r1)', 'lwz r31,80(r1)'] + VT4
    cf = _world({FA: sh}, TBL)
    check(_ref(cf, FA) == ('cand', ['hL']), 'review: a result read from a stack slot (rule h) is a candidate (how hL)', _ref(cf, FA))
    # 7. rule a / e: a function a data word points at (a callback struct outside the table catalog) can be called with any `this`
    import sqlite3
    c = sqlite3.connect(':memory:')
    c.executescript('''CREATE TABLE funcs(addr INTEGER, name TEXT); CREATE TABLE tables(start INTEGER, rule TEXT, len INTEGER); CREATE TABLE imms(value INTEGER, op TEXT);
        CREATE TABLE ptrs(at INTEGER, value INTEGER, kind TEXT);''')
    c.executemany('INSERT INTO funcs VALUES (?, ?)', [(FA, 'sub'), (FB, 'sub'), (0x82100010, 'sub')])
    c.execute('INSERT INTO tables VALUES (?, ?, ?)', (TA, 'code', 2))
    c.executemany('INSERT INTO ptrs VALUES (?, ?, ?)', [(TA + 4, FA, 'func'), (0x82400000, FB, 'func')])
    w = world_from_index(c, type('I', (), {'word': staticmethod(lambda a: {TA: 0x82100010, TA + 4: FA}.get(a))})(), {})
    check(FB in w.codefn and FA not in w.codefn, 'review: a function a data word outside the tables points at counts as address-taken (rule a / e: its `this` is unknown)', sorted(w.codefn))


def real_checks():
    path = os.environ.get('WWE_QVCALL_DB') or qx7.vdb_path()
    if not os.path.exists(path):
        print(f'SKIP  real totals (no {path}: build it first, e.g. py tools\\q\\q.py callers 0x82BDB460)')
        return
    v = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    meta = dict(v.execute('SELECT key, value FROM meta').fetchall())
    if 'classflow' not in meta:
        print(f'SKIP  real totals ({path} was built before CLASSFLOW)')
        return
    st = dict(v.execute('SELECT status, COUNT(*) FROM icall GROUP BY status').fetchall())
    print(f'INFO  status {st}; classflow: {meta["classflow"]}')
    rule_re = re.compile(r'^[abcdefh]+L?$')
    by = collections.defaultdict(set)
    for site, how in v.execute('SELECT DISTINCT site, how FROM icall_target'):
        if rule_re.match(how):
            by['f' if how.endswith('L') else 'sound'].add(site)
    print(f'INFO  sites closed in sound mode (rules a b c d e h): {len(by["sound"])} (the note: 5,953 of 30,815 with f 1,828 included; 5,915 / 26,189 pinned)')
    print(f'INFO  rule f (candidates, how ends in L): {len(by["f"])} sites (the note: f closes 1,828 alone)')
    con = qx7._index_con()
    img = qx7.Img()
    tabs = {s: [img.word(s + 4 * i) for i in range(n)] for s, n in con.execute('SELECT start, len FROM tables')}
    # S110 CLASSFLOW review: 0x836F3A50 is stored by its base ctor sub_823AE7D0, which first calls ITS base ctor 0x82C37CB0 (table 0x8206F4D4): with the union of a
    # function's ctor calls and stores (not the last by address) that table is in the set too
    for obj, ts in ((0x836F3A50, (0x82000D90, 0x8200F0B8, 0x8206F4D4)), (0x839294BC, (0x82047548,))):
        rows = v.execute("SELECT i.site, i.off, t.target, t.how FROM icall i JOIN icall_target t ON t.site = i.site WHERE i.recv = ?", (f'glob:0x{obj:08X}',)).fetchall()
        bad = [r for r in rows if r[3].endswith('L') and r[2] not in {tabs[t][r[1] // 4] for t in ts if r[1] // 4 < len(tabs.get(t, []))}]
        n = len({r[0] for r in rows if r[3].endswith('L')})
        check(n > 0 and not bad, f'pin: global 0x{obj:08X}: {n} candidate sites, every target a slot of {[hex(t) for t in ts]}', bad[:3])
    hit = v.execute("SELECT COUNT(DISTINCT i.site) FROM icall i JOIN icall_target t ON t.site = i.site WHERE i.off = 56 AND t.target = ? AND t.how GLOB '[abcdefh]*'",
                    (0x82BDB460,)).fetchone()[0]
    check(hit == 0, 'pin: 0 slot +56 sites narrowed to sub_82BDB460 by a class rule', hit)


def explain_global(addr):
    """--why 0xGLOBAL: scan the listing (the class pass) and print, for each store to that global, what the evaluator makes of the stored value (why a global stays open)"""
    import glob
    from concurrent.futures import ProcessPoolExecutor
    con = qx7._index_con()
    funcs = frozenset(r[0] for r in con.execute('SELECT addr FROM funcs'))
    vts = frozenset(r[0] for r in con.execute('SELECT vt FROM rtti_vtable'))
    helpers = frozenset(r[0] for r in con.execute("SELECT addr FROM funcs WHERE name GLOB '__save*' OR name GLOB '__rest*'"))
    facts = {}
    with ProcessPoolExecutor(max_workers=qx7.workers(), initializer=qx7._init_worker, initargs=(funcs, vts, helpers, frozenset(), frozenset(), True)) as ex:
        for r in ex.map(qx7.scan_file, sorted(glob.glob(os.path.join(qx7.GEN, 'wwe2k14_recomp.*.cpp'))), chunksize=4):
            facts.update(r[4])
    sys.setrecursionlimit(100000)
    cf = CF(world_from_index(con, qx7.Img(), facts))
    st = cf.glob_st.get(addr, [])
    print(f'0x{addr:08X}: {len(st)} stores; glob -> {cf.glob(addr)}')
    for g, s in st[:40]:
        print(f'  in sub_{g:08X}: src {s} -> {cf.val(g, s)}; member {sorted(cf.member.get(g, ()))[:2]} taken {g in cf.taken} callers {len(cf.callers.get(g, ()))}')
    return 0


def main(argv):
    if '--why' in argv:
        return explain_global(int(argv[argv.index('--why') + 1], 16))
    if '--selftest' not in argv:
        print(__doc__)
        return 2
    try:
        unit_checks()
        real_checks()
    except Fail as e:
        print(f'FAIL  {e}')
        return 1
    print('OK')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
