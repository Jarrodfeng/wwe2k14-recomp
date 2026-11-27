r"""q slice: where does this value come from, in the GAME's code (S111 SLICER: the backward slicer, tool order step 1).

    py tools/q/q.py slice sub_X+OFF REG|[mem] [--depth N] [--budget N] [--expr] [--json] [--no-cache]

Starts at one instruction's operand: REG = the value the register holds as the instruction READS it (a store's value register, a
call's argument r3..r10 / f1..f13, a compare's operand); a register the instruction only WRITES is the value it writes. [mem] = at a
store the value stored, at a load the value loaded. OFF is hex, as `q decomp` prints it (+E0, 0xE0).

From there it walks def-use BACKWARDS over qdecomp's interpreter (one engine: the value of every register at every instruction is
qdecomp's expression; this module only adds the walk):
  * straight line and both arms of a branch: a value that differs per path is a phi, one line per path with that path's condition;
    a loop-carried value is the named leaf ?loop (an induction variable prints as base + step*?loop, the step from the listing);
  * an argument (in_rN at entry) climbs into every caller (q index call / tail edges), depth-limited, each caller listed;
  * a call's result (ret#OFF) goes into the callee: its return value(s), with the callee's arguments bound to this call's;
    an indirect call's result is the leaf ?vcall(+SLOT);
  * a load is a field [base+OFF]: the base is named canonically (a class by its vtable when the field index types the argument, a global
    word, an absolute address, a pointer loaded from another field), and the field's WRITERS are listed: the field index (q field) rows of
    that root, the class's functions and their direct callees read by qdecomp, filtered to the SAME root (two typed roots must share a
    vtable) and the same offset lattice (an array element base + stride*k matches any k). A writer reached through a callee whose base is
    a global word not proven to hold this object is marked [L]; every other writer row is [C tool];
  * the leaves: constants, image data words (`0.1[0x82003E28]`: the value from the image), tables (`[0x83622ACC + 4*idx]` with their
    first image words), arguments with no caller, ?loop, ?vcall, a register a call clobbered (?rN).
--expr prints the value as ONE expression in the start function's input terms (callee returns inlined, phis as phi(cond: a | ...)).
Per function, the facts the walk reads (every instruction's operand values, the stores, the calls, the returns, the joins) are cached
in out/harness/index/slice/ keyed by the generated source's hash and this file's + qdecomp's code (derived from the game: never commit).
Every chain is a lead read off the listing by a tool [C tool]; confirm a surprising hop in the listing (q asm FUNC --at OFF).
"""
import hashlib
import json
import math
import os
import re
import sqlite3
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import vtcut  # noqa: E402  where a vtable starts (shared with build_fields)
import qdecomp  # noqa: E402 the one engine: listing reader, interpreter, expressions

VERSION = 1
CACHE_DIR = os.path.join(os.path.dirname(qdecomp.CACHE_DIR), 'slice')
ARG_REGS = [f'r{i}' for i in range(3, 11)] + [f'f{i}' for i in range(1, 14)]
REG_RE = qdecomp.REG_RE
MAX_CALLERS = 8          # callers sliced per argument (all are counted)
MAX_WRITERS = 6          # writer stores sliced per field (all are counted)
MAX_PATHS = 8            # phi inputs / return paths printed
MAX_CANDS = 160          # functions read when looking for a field's writers
MAX_TABLE = 8            # image words printed for a constant table


class SliceError(Exception):
    pass


# ------------------------------------------------------------------ expression helpers
def tup(x):
    """JSON lists back to the interpreter's tuples (an expression holds no list of its own)"""
    if isinstance(x, list):
        return tuple(tup(i) for i in x)
    return x


def hx(a):
    return f'0x{a:08X}'


def ekey(e):
    return repr(e)


# ------------------------------------------------------------------ per-function facts
class _Ctx(qdecomp.Ctx):
    """qdecomp's block context; it also keeps the FULL value of a long expression qdecomp names `rN@+OFF` (the slice inlines it)"""

    def setreg(self, r, e, site):
        if r != 'r1' and (qdecomp.depth(e) > 7 or len(qdecomp.fmt(e)) > 110):
            self.fn.slice_defs[f'{r}@{site - self.fn.addr:X}'] = e
        super().setreg(r, e, site)


def _mem_addr(c, ins, t):
    """the effective address of a load / store, as qdecomp.mem_op computes it"""
    mn = ins.mn
    if mn.endswith('x') or mn.endswith('ux') or mn.endswith('x128') or mn.endswith('cx.'):
        return c.ea(t[1], t[2], ins.addr)
    m = qdecomp.MEM_RE.match(t[1])
    if not m:
        return None
    return c.ea(m.group(2), int(m.group(1), 0), ins.addr)


def build_facts(name, addr, insns, labels, names, image):
    """-> the facts dict of one function: run qdecomp's analysis, then each block once more from its settled entry state, recording
    per instruction the operand values (b = as read, a = the destination as written), the stores, calls, returns, joins and long defs."""
    fn = qdecomp.Func(name, addr, insns, labels, names, image)
    qdecomp.analyse(fn)
    fn.slice_defs = {}
    ins_out, stores, calls, rets = {}, [], {}, []
    blocks = {}
    for b in fn.blocks:
        term = None
        if b.term:
            term = {k: b.term.get(k) for k in ('k', 'target', 'cond', 'neg')}
        blocks[b.start] = {'preds': [[p, k] for p, k in fn.preds.get(b.start, [])], 'term': term, 'end': b.end,
                           'line': b.line}
        if b.entry is None:
            continue
        c = _Ctx(fn, b, b.entry.copy())
        for ins in b.insns:
            rel = ins.addr - addr
            t = qdecomp.split_ops(ins.ops)
            mn = ins.mn
            m0 = mn.rstrip('+-') if mn.startswith('b') else mn
            bvals = {}
            for tok in t:
                if REG_RE.match(tok):
                    bvals[tok] = c.g(tok)
                else:
                    mm = qdecomp.MEM_RE.match(tok)
                    if mm:
                        bvals[mm.group(2)] = c.g(mm.group(2))
            is_call = m0 in ('bl', 'bctrl') or (m0 == 'b' and not fn.target_inside(qdecomp.branch_target(ins))) or \
                (m0 == 'bctr' and not ins.cases)
            is_ret = m0 == 'blr' or (m0.startswith('b') and m0.endswith('lr') and m0 not in ('bl', 'blr') and not m0.endswith('ctrl'))
            if is_call or is_ret:
                for r in ARG_REGS:
                    v = c.g(r)
                    if v[0] != 'undef':
                        bvals.setdefault(r, v)
            rec = {'mn': mn, 'ops': ins.ops, 'b': bvals, 'blk': b.start}
            if mn in qdecomp.STORES and t:
                width, kind = qdecomp.STORES[mn]
                af = _mem_addr(c, ins, t)
                if af is not None and not (mn == 'stwu' and t[1].endswith('(r1)')):
                    val = c.g(t[0]) if REG_RE.match(t[0]) else ('c', 0)
                    if mn == 'stfiwx':
                        val = ('op', 'int32', val)
                    stk = qdecomp.stack_off(af) if qdecomp.is_stack(af) else None
                    stores.append({'o': rel, 'af': af, 'val': val, 'w': width, 'kind': kind, 'stk': stk, 'blk': b.start})
            tgt = qdecomp.branch_target(ins)
            if m0 == 'bl' and tgt is not None:
                nm = names(tgt) or ''
                if not qdecomp.HELPER_RE.match(nm):
                    calls[rel] = {'t': tgt, 'k': 'C', 'regs': {r: bvals[r] for r in ARG_REGS if r in bvals}}
            elif m0 == 'bctrl':
                calls[rel] = {'t': None, 'k': 'V', 'ctr': c.st.ctr, 'regs': {r: bvals[r] for r in ARG_REGS if r in bvals}}
            elif m0 == 'b' and tgt is not None and not fn.target_inside(tgt):
                nm = names(tgt) or ''
                if qdecomp.HELPER_RE.match(nm):
                    rets.append({'o': rel, 'r3': c.g('r3'), 'f1': c.g('f1'), 'blk': b.start})
                else:
                    calls[rel] = {'t': tgt, 'k': 'TAIL', 'regs': {r: bvals[r] for r in ARG_REGS if r in bvals}}
            elif m0 == 'bctr' and not ins.cases:
                calls[rel] = {'t': None, 'k': 'TAILV', 'ctr': c.st.ctr, 'regs': {r: bvals[r] for r in ARG_REGS if r in bvals}}
            elif is_ret:
                rets.append({'o': rel, 'r3': c.g('r3'), 'f1': c.g('f1'), 'blk': b.start})
            qdecomp.step(c, ins)
            if t and REG_RE.match(t[0]) and not mn.startswith(qdecomp.NO_DEST) and mn not in qdecomp.STORES:
                rec['d'] = t[0]
                rec['a'] = c.g(t[0])
            ins_out[rel] = rec
    phis = {}
    for j, regs in getattr(fn, 'phis', {}).items():
        for key, (kind, inputs) in regs.items():
            if kind == 'reg':
                phis.setdefault(j, {})[key] = [[p, v] for p, v in inputs]
    return {'version': VERSION, 'addr': addr, 'name': name, 'ins': ins_out, 'stores': stores, 'calls': calls, 'rets': rets,
            'phis': phis, 'defs': fn.slice_defs, 'blocks': blocks, 'converged': fn.converged}


def _decode_ins(ins):
    return {int(k): dict(v, b={r: tup(x) for r, x in v['b'].items()}, **({'a': tup(v['a'])} if 'a' in v else {}))
            for k, v in ins.items()}


def _decode(f):
    """a facts dict read from JSON: int keys back, expressions back to tuples (the per-instruction operand values `ins` are
    kept in their own file and read only for the start function: Source.ins)"""
    if 'ins' in f:
        f['ins'] = _decode_ins(f['ins'])
    for s in f['stores']:
        s['af'], s['val'] = tup(s['af']), tup(s['val'])
    f['calls'] = {int(k): dict(v, regs={r: tup(x) for r, x in v['regs'].items()}, **({'ctr': tup(v['ctr'])} if 'ctr' in v else {}))
                  for k, v in f['calls'].items()}
    for r in f['rets']:
        r['r3'], r['f1'] = tup(r['r3']), tup(r['f1'])
    f['phis'] = {int(j): {k: [(p, tup(v)) for p, v in ins] for k, ins in regs.items()} for j, regs in f['phis'].items()}
    f['defs'] = {k: tup(v) for k, v in f['defs'].items()}
    f['blocks'] = {int(k): v for k, v in f['blocks'].items()}
    return f


_CODE = []


def code_sha():
    if not _CODE:
        h = hashlib.sha1()
        for n in ('qslice.py', 'qdecomp.py', 'qdecomp_phi.py'):
            with open(os.path.join(HERE, n), 'rb') as fh:
                h.update(fh.read())
        _CODE.append(h.hexdigest())
    return _CODE[0]


# ------------------------------------------------------------------ the sources: the q index + the field index (planted in the selftest)
class Source:
    """what the walk asks of the outside world. QSource answers from the q index, the generated listing and the field index; the
    selftest plants one from listings it writes itself."""

    def func(self, addr):           # -> {'addr','name'} or None (no body)
        raise NotImplementedError

    def facts(self, addr):
        raise NotImplementedError

    def ins(self, F):               # -> {rel: operand values} of one function (the start function only)
        return F['ins']

    def callers(self, addr):        # -> [(src, site, kind)]
        return []

    def vslots(self, addr):         # -> [(vt, idx)]
        return []

    def arg_types(self, addr, reg):  # -> set of vtables typing argument reg of addr
        return set()

    def global_types(self, a):
        return set()

    def ret_types(self, addr, site):
        return set()

    def type_funcs(self, vts):      # -> functions whose argument / return the index types with one of vts
        return set()

    def k_writers(self, a, g):      # -> functions with a field-index W row at absolute address a (+ g*k)
        return set()

    def vt_store_funcs(self, vt):   # -> functions that store the vtable address vt
        return set()

    def is_vt(self, a):
        return False

    def g_funcs(self, a):           # -> functions with a row through the global word a (or a pointer loaded from it)
        return set()

    def g_loaders(self, a):         # S111 QFIX5 -> functions that build the address a as a constant (lis+addi / lis+lwz ...): the candidate getters of the global word a
        return set()

    def word(self, a):
        return None

    def name(self, addr):
        f = self.func(addr)
        return f['name'] if f else f'sub_{addr:08X}'


class QSource(Source):
    def __init__(self, con, use_cache=True):
        self.con = con
        self.use_cache = use_cache
        self._facts, self._func, self._fdb = {}, {}, None
        self._vts, self._isf, self._gf, self._kw, self._rids, self._memo = None, {}, {}, {}, {}, {}
        self._img = None
        self.read = 0               # functions analysed now (not from the cache)

    def func(self, addr):
        if addr not in self._func:
            r = self.con.execute('SELECT addr, name, file, line, size FROM funcs WHERE addr = ?', (addr,)).fetchone()
            self._func[addr] = {'addr': r[0], 'name': r[1], 'file': r[2], 'line': r[3], 'size': r[4]} if r and r[2] else None
        return self._func[addr]

    def image(self):
        if self._img is None:
            if qdecomp._IMG is None:
                qdecomp._IMG = qdecomp.Image()
            self._img = qdecomp._IMG
        return self._img

    def word(self, a):
        return self.image().word(a)

    def facts(self, addr):
        if addr in self._facts:
            return self._facts[addr]
        f = self.func(addr)
        if f is None:
            self._facts[addr] = None
            return None
        path = os.path.join(qdecomp.GEN, f['file'])
        if not os.path.exists(path):
            raise SliceError(f'generated source {path} is missing (generated/ is local only)')
        sha = qdecomp._cached_sha1(path)
        cpath = os.path.join(CACHE_DIR, f'{addr:08X}.{sha[:12]}.v{VERSION}.{code_sha()[:10]}.json')
        if self.use_cache and os.path.exists(cpath) and os.path.exists(cpath[:-5] + '.ins.json'):
            try:
                with open(cpath, encoding='utf-8') as fh:
                    r = _decode(json.load(fh))
                r['_ins_path'] = cpath[:-5] + '.ins.json'
                self._facts[addr] = r
                return r
            except (OSError, ValueError, KeyError):
                pass
        insns, labels, _first, _last = qdecomp.read_listing(path, f['line'], f['name'], f['addr'])
        if not insns:
            self._facts[addr] = None
            return None

        def names(a):
            g = self.func(a)
            if g:
                return g['name']
            r = self.con.execute('SELECT name FROM funcs WHERE addr = ?', (a,)).fetchone()
            return r[0] if r else None
        try:
            r = build_facts(f['name'], addr, insns, labels, names, self.image())
        except qdecomp.DecompError:
            self._facts[addr] = None
            return None
        self.read += 1
        try:
            os.makedirs(CACHE_DIR, exist_ok=True)
            for old in os.listdir(CACHE_DIR):
                if old.startswith(f'{addr:08X}.') and old != os.path.basename(cpath):
                    try:
                        os.remove(os.path.join(CACHE_DIR, old))
                    except OSError:
                        pass
            ipath = cpath[:-5] + '.ins.json'
            with open(ipath + '.tmp', 'w', encoding='utf-8') as fh:
                json.dump(r['ins'], fh)
            os.replace(ipath + '.tmp', ipath)
            with open(cpath + '.tmp', 'w', encoding='utf-8') as fh:
                json.dump({k: v for k, v in r.items() if k != 'ins'}, fh)
            os.replace(cpath + '.tmp', cpath)
        except OSError:
            pass
        r = _decode(json.loads(json.dumps(r)))      # the same shape whether it came from the cache or not
        self._facts[addr] = r
        return r

    def ins(self, F):
        if 'ins' not in F:
            with open(F['_ins_path'], encoding='utf-8') as fh:
                F['ins'] = _decode_ins(json.load(fh))
        return F['ins']

    def callers(self, addr):
        rows = self.con.execute("SELECT src, site, kind FROM edges WHERE dst = ? AND kind IN ('call', 'tail') ORDER BY src, site",
                                (addr,)).fetchall()
        return [(r[0], r[1], r[2]) for r in rows if r[0] != addr]

    # the field index (read only; a missing index answers nothing)
    def fdb(self):
        if self._fdb is None:
            try:
                import build_fields as bf
                _g, _i, db = bf.default_paths(None, None)
                self._fdb = sqlite3.connect(f'file:{db}?mode=ro', uri=True) if os.path.exists(db) else False
            except Exception:  # noqa: BLE001 - no field index: the walk names no typed root and lists only what it reads itself
                self._fdb = False
        return self._fdb

    def _rid(self, text):
        if text not in self._rids:
            db = self.fdb()
            r = db.execute('SELECT id FROM roots WHERE text = ?', (text,)).fetchone() if db else None
            self._rids[text] = r[0] if r else None
        return self._rids[text]

    def _starts(self):
        """the vtable starts the indexes know: the field index's vtables / vptr stores, the q index's RTTI vtables"""
        if self._vts is None:
            s = set()
            db = self.fdb()
            if db:
                s |= {v for (v,) in db.execute('SELECT vt FROM vtables')}
                s |= {v for (v,) in db.execute('SELECT DISTINCT vt FROM vstores')}
            s |= vtcut.index_starts(self.con, lambda a: self.is_func(self.word(a)))     # the rule shared with build_fields
            self._vts = s
        return self._vts

    def is_func(self, a):
        if not a:
            return False
        if a not in self._isf:
            self._isf[a] = self.con.execute('SELECT 1 FROM funcs WHERE addr = ?', (a,)).fetchone() is not None
        return self._isf[a]

    def is_vt(self, a):
        return self.is_func(self.word(a))

    def vt_start_of(self, w):
        """the vtable the data word w sits in: the nearest start at or below w (a vtable an index lists, or an address the code
        builds as an immediate, e.g. a lis pair storing a subobject's vptr) with only function words from it to w. The field
        index's own slot table reads a vtable past the start of the next one (S111 SLICER: 0x82023270's 18 slots hold
        0x82023290's and 0x82023298's), so slots are taken from here, never from it."""
        m = self._memo.setdefault('vso', {})
        if w not in m:
            m[w] = self._vt_start_of(w)
        return m[w]

    def _vt_start_of(self, w):
        cands = {v for v in self._starts() if w - 256 <= v <= w}
        for v in sorted(cands, reverse=True):
            if (w - v) % 4:
                continue
            if all(self.is_func(self.word(a)) for a in range(v, w + 4, 4)):
                return v
        return None

    def vslots(self, addr):
        out = []
        for (w,) in self.con.execute('SELECT at FROM ptrs WHERE value = ?', (addr,)):
            v = self.vt_start_of(w)
            if v is not None:
                out.append((v, (w - v) // 4))
        return out

    def _objtype(self, addr, rid):
        db = self.fdb()
        return {v for (v,) in db.execute("SELECT vt FROM objtype WHERE func = ? AND root = ? AND how != 'slot'", (addr, rid))}

    def arg_types(self, addr, reg):
        m = self._memo.setdefault('at', {})
        if (addr, reg) not in m:
            m[(addr, reg)] = self._arg_types(addr, reg)
        return m[(addr, reg)]

    def _arg_types(self, addr, reg):
        if not reg.startswith('r'):
            return set()
        out = {v for v, _i in self.vslots(addr)} if reg == 'r3' else set()
        rid = self._rid('a%d' % (int(reg[1:]) - 3))
        if rid is not None:
            out |= self._objtype(addr, rid)
        return out

    def vt_store_funcs(self, vt):
        """the functions that store the vtable address vt (the field index's vptr stores + the code's immediates)"""
        if vt not in self._memo.setdefault('vsf', {}):
            self._memo['vsf'][vt] = self._vt_store_funcs(vt)
        return self._memo['vsf'][vt]

    def _vt_store_funcs(self, vt):
        out = set()
        db = self.fdb()
        if db:
            out |= {f for (f,) in db.execute('SELECT DISTINCT func FROM vstores WHERE vt = ?', (vt,))}
        out |= {f for (f,) in self.con.execute('SELECT DISTINCT func FROM imms WHERE value = ?', (vt,))}
        return out

    def global_types(self, a):
        m = self._memo.setdefault('gt', {})
        if a not in m:
            rid = self._rid('g:%08X' % a)
            m[a] = set() if rid is None else {v for (v,) in self.fdb().execute('SELECT vt FROM objtype WHERE root = ?', (rid,))}
        return m[a]

    def ret_types(self, addr, site):
        rid = self._rid('r:%08X' % site)
        if rid is None:
            return set()
        return self._objtype(addr, rid)

    def type_funcs(self, vts):
        """the slot functions of each vtable (its words up to the next vtable) + the functions the field index types with it"""
        out = set()
        for vt in vts:
            a = vt
            while a < vt + 256 and self.is_func(self.word(a)) and (a == vt or self.vt_start_of(a) == vt):
                out.add(self.word(a))
                a += 4
        db = self.fdb()
        if db and vts:
            q = ','.join(str(int(v)) for v in vts)
            out |= {f for (f,) in db.execute(f"SELECT func FROM objtype WHERE vt IN ({q}) AND func != 0 AND how != 'slot'")}
        return out

    def k_writers(self, a, g):
        if (a, g) not in self._kw:
            self._kw[(a, g)] = self._k_writers(a, g)
        return self._kw[(a, g)]

    def _k_writers(self, a, g):
        db = self.fdb()
        rid = self._rid('K')
        if not db or rid is None:
            return set()
        if not g:
            return {f for (f,) in db.execute("SELECT DISTINCT func FROM rows WHERE root = ? AND rw = 'W' AND off = ?", (rid, a))}
        return {f for (f,) in db.execute("SELECT DISTINCT func FROM rows WHERE root = ? AND rw = 'W' AND off >= ? AND off < ? "
                                         "AND (off - ?) % ? = 0", (rid, a, a + g * 256, a, g))}

    def g_loaders(self, a):
        try:
            return {f for (f,) in self.con.execute("SELECT DISTINCT func FROM imms WHERE value = ? AND op LIKE 'lis+%'", (a,))}
        except Exception:  # noqa: BLE001 - an index without the imms table
            return set()

    def g_funcs(self, a):
        db = self.fdb()
        if not db:
            return set()
        if a in self._gf:
            return self._gf[a]
        t = 'g:%08X' % a
        ids = [i for (i,) in db.execute('SELECT id FROM roots WHERE text = ? OR text LIKE ?', (t, f'p({t},%'))]
        out = set()
        for i in ids:
            out |= {f for (f,) in db.execute('SELECT DISTINCT func FROM rows WHERE root = ?', (i,))}
        rid = self._rid(t)
        if rid is not None:
            out |= {f for (f,) in db.execute('SELECT DISTINCT func FROM argflow WHERE groot = ?', (rid,))}
        self._gf[a] = out
        return out


# ------------------------------------------------------------------ the walk
class Node:
    """one line of the chain: `label = text   -- note`. label = where / which role (a site, a caller, a writer, a path),
    text = the value there, note = what kind of value it is and what was searched"""
    __slots__ = ('label', 'text', 'kind', 'kids', 'mark', 'num', 'note')

    def __init__(self, label, text='', kind='', kids=None, mark=None, note=None):
        self.label, self.text, self.kind, self.kids, self.mark, self.num = label, text, kind, kids or [], mark, None
        self.note = note

    def add_note(self, s):
        self.note = s if not self.note else f'{s}; {self.note}'
        return self

    def js(self):
        d = {'label': self.label, 'text': self.text, 'kind': self.kind}
        if self.note:
            d['note'] = self.note
        if self.mark:
            d['mark'] = self.mark
        if self.num:
            d['num'] = self.num
        if self.kids:
            d['kids'] = [k.js() for k in self.kids]
        return d


def lat_meet(a, b):
    """do two offset lattices (c, g, anysign) share a value? g = 0: exactly c; else c + g*k, k >= 0 (any k when anysign)"""
    c1, g1, s1 = a
    c2, g2, s2 = b
    if not g1 and not g2:
        return c1 == c2
    if not g1:
        return (c1 - c2) % g2 == 0 and (s2 or c1 >= c2)
    if not g2:
        return (c2 - c1) % g1 == 0 and (s1 or c2 >= c1)
    lo, hi = sorted((g1, g2))
    if hi % lo:     # two arrays of different element sizes are not the same field (an unrolled loop's stride is a multiple)
        return False
    return (c1 - c2) % lo == 0


def same_root(a, b):
    if a is None or b is None:
        return False
    if a[0] == 'type' and b[0] == 'type':
        return bool(set(a[1]) & set(b[1]))
    if a[0] == 'u' or b[0] == 'u':
        return False
    if a[0] == 'p' and b[0] == 'p':
        return a[2] == b[2] and same_root(a[1], b[1])
    return a == b


def root_text(r):
    k = r[0]
    if k == 'type':
        return 'class ' + '/'.join(hx(v) for v in sorted(r[1]))
    if k == 'g':
        return f'global word [{hx(r[1])}]'
    if k == 'K':
        return 'absolute address'
    if k == 'p':
        return f'pointer at +{r[2]} of ({root_text(r[1])})'
    if k == 'arg':
        return f'argument {r[2]} of sub_{r[1]:08X} (callers disagree or none: not provable)'
    if k == 'r':
        return f'the result of the call at {hx(r[2])}'
    if k == 'sp':
        return 'the stack'
    return 'a base the walk cannot name'


class Slicer:
    def __init__(self, src, depth=6, budget=160, mode='tree'):
        self.src, self.depth, self.budget = src, depth, budget
        self.nodes = 0
        self.seen_fields = {}       # canonical field key -> its number (printed once)
        self.num = 0
        self.cut = 0                # nodes not expanded (budget)
        self._norm, self._glob, self._argroot = {}, {}, {}

    # ---------------------------------------------------------------- classes: which object a vptr marks
    def this_consts(self, T):
        """{off: [constants stored at in_r3+off, in listing order]} of one function"""
        out = {}
        for s in T['stores']:
            if s['val'][0] != 'c' or s['stk'] is not None:
                continue
            c, terms = self.lin(T, s['af'], None)
            if len(terms) == 1 and terms[0][0] == 1 and terms[0][2][2] == ('in', 'r3'):
                out.setdefault(c, []).append(s['val'][1] & 0xFFFFFFFF)
        return out

    def norm_vt(self, vt, d=0):
        """(vt0, off): the class whose object holds the vptr vt at +off, when every function that stores vt stores it at this+off
        beside a last vptr vt0 at this+0 (a base or member subobject); else (vt, 0)"""
        if vt in self._norm:
            return self._norm[vt]
        self._norm[vt] = (vt, 0)
        got = set()
        for f in sorted(self.src.vt_store_funcs(vt))[:12]:
            T = self.facts(f)
            if T is None:
                continue
            offs = self.this_consts(T)
            for o, vals in offs.items():
                if vt not in vals:
                    continue
                if o == 0:
                    got.add((vt, 0))
                    continue
                prim = [v for v in offs.get(0, ()) if v != vt and self.src.is_vt(v)]
                got.add((prim[-1], o) if prim else (vt, 0))
        if len(got) == 1:
            v0, o = next(iter(got))
            if o and d < 3:
                v1, o1 = self.norm_vt(v0, d + 1)
                self._norm[vt] = (v1, o + o1)
            else:
                self._norm[vt] = (v0, o)
        return self._norm[vt]

    def sub_vts(self, vt):
        """the subobject vtables a class stores beside its own (at this+off, off != 0)"""
        out = set()
        for f in sorted(self.src.vt_store_funcs(vt))[:12]:
            T = self.facts(f)
            if T is None:
                continue
            offs = self.this_consts(T)
            if vt in offs.get(0, ()):
                for o, vals in offs.items():
                    if o:
                        out |= {v for v in vals if v != vt and self.src.is_vt(v) and self.norm_vt(v)[0] == vt}
        return out

    def globals_of(self, vt):
        """the global words a constructor's result is stored in (where the class's objects are kept: their users are writers too)"""
        out = set()
        for f in sorted(self.src.vt_store_funcs(vt))[:12]:
            T = self.facts(f)
            if T is None or vt not in self.this_consts(T).get(0, ()):
                continue
            for src, site, _kind in self.src.callers(f)[:12]:
                S = self.facts(src)
                if S is None:
                    continue
                for s in S['stores']:
                    if s['val'] == ('ret', site - src, 'r3'):
                        cn = self.canon(S, s['af'], None, 3)
                        if cn is not None and cn[0][0] == 'K' and not cn[1][1]:
                            out.add(cn[1][0])
        return out

    def type_root(self, vts):
        pairs = {self.norm_vt(v) for v in vts}
        if len(pairs) == 1:
            v0, o = next(iter(pairs))
            return ('type', (v0,)), o
        return ('type', tuple(sorted(vts))), 0

    def global_root(self, a, d):
        """the class of the object a global word holds, from its writers: every non-constant value stored there is that class"""
        if a in self._glob:
            return self._glob[a]
        self._glob[a] = None
        roots = []
        for f in sorted(self.src.k_writers(a, 0))[:6]:
            T = self.facts(f)
            if T is None:
                continue
            for s in T['stores']:
                if s['stk'] is not None or s['val'][0] == 'c':
                    continue
                cn = self.canon(T, s['af'], None, d + 1)
                if cn is None or cn[0][0] != 'K' or cn[1] != (a, 0, False):
                    continue
                cv = self.canon(T, s['val'], None, d + 1)
                roots.append(cv)
        if roots and all(r is not None and r[0][0] == 'type' and same_root(r[0], roots[0][0]) and r[1] == (0, 0, False) for r in roots):
            self._glob[a] = roots[0][0]
        return self._glob[a]

    # ---------------------------------------------------------------- small lookups
    def facts(self, addr):
        return self.src.facts(addr)

    def fname(self, addr):
        return self.src.name(addr)

    def site(self, F, rel):
        return f'{F["name"]}+0x{rel:X}'

    def full_def(self, F, e):
        return F['defs'].get(f'{e[1]}@{e[2]:X}')

    def local_stores(self, F, e):
        """the stack stores a local read back (`L80@+218`) may read: the nearest store to that slot before the load in its block,
        else every store to the slot"""
        off = int(e[1][1:])
        rel = e[2]
        a = F['addr'] + rel
        blk = next((s for s, b in F['blocks'].items() if s <= a < b['end']), None)
        same = [s for s in F['stores'] if s['stk'] == off and s['blk'] == blk and s['o'] < rel]
        if same:
            return [max(same, key=lambda s: s['o'])]
        return [s for s in F['stores'] if s['stk'] == off]

    def induction(self, F, reg, j):
        """(base, step, bases) of a join value of `reg` at j: every input is one base value or the same register's join value plus a
        constant. step = the gcd of those constants (0: every path holds the base). None when it is not that shape."""
        bases, steps, seen, work = [], [], set(), [j]
        while work:
            jj = work.pop()
            if jj in seen:
                continue
            seen.add(jj)
            ins = F['phis'].get(jj, {}).get(reg)
            if ins is None:
                return None
            for _p, v in ins:
                if v is None:
                    return None
                while v[0] == 'def' and not v[1].startswith('L') and self.full_def(F, v) is not None:
                    v = self.full_def(F, v)
                if v[0] == 'phi' and v[1] == reg:
                    work.append(v[2])
                elif v[0] == 'add' and v[1][0] == 'phi' and v[1][1] == reg:
                    work.append(v[1][2])
                    steps.append(v[2])
                elif v not in bases:
                    bases.append(v)
        if len(bases) != 1:
            return None
        g = 0
        for s in steps:
            g = math.gcd(g, abs(s))
        neg = any(s < 0 for s in steps)
        if neg and not all(s < 0 for s in steps):
            return bases[0], g, 'any'
        return bases[0], (-g if neg else g), None

    def path_cond(self, F, p, j, n=0):
        """the condition of the edge p -> j (climbs a chain of single-predecessor blocks to the branch that decides it)"""
        if p is None:
            return 'at entry'
        b = F['blocks'].get(p)
        if b is None:
            return ''
        kinds = [k for pp, k in F['blocks'].get(j, {}).get('preds', []) if pp == p]
        t = b['term']
        if t and t['k'] == 'cond':
            if t.get('target') == j and 'br' in kinds:
                return t['cond']
            if 'fall' in kinds:
                return t['neg']
        if kinds and kinds[0] == 'case':
            return 'a switch case'
        pr = b['preds']
        if len(pr) == 1 and n < 8:
            return self.path_cond(F, pr[0][0], p, n + 1)
        if not pr:
            return 'at entry' if p == F['addr'] else ''
        return ''

    def block_cond(self, F, blk):
        b = F['blocks'].get(blk)
        if b is None or not b['preds']:
            return 'always' if blk == F['addr'] else ''
        if len(b['preds']) == 1:
            c = self.path_cond(F, b['preds'][0][0], blk)
            return c or 'always'
        return 'one of several paths'

    # ---------------------------------------------------------------- rendering
    def render(self, F, e, bind=None, mode='tree', d=0):
        """expression text in F's input terms. tree: join values by name; expr: inlined (phi(...), callee returns)"""
        if d > 12:
            return '...'
        R = lambda x: self.render(F, x, bind, mode, d + 1)  # noqa: E731
        k = e[0]
        if k == 'c':
            return qdecomp.fnum(e[1])
        if k == 'in':
            if bind is not None and e[1] in bind[1]:
                return self.render(bind[0], bind[1][e[1]], bind[2], mode, d + 1)
            return 'in_' + e[1]
        if k == 'sp':
            return 'sp'
        if k == 'add':
            if qdecomp.is_stack(e):
                return f'&local[sp+{qdecomp.stack_off(e)}]'
            bs = R(e[1])
            if e[1][0] == 'op' and not bs.startswith('('):
                bs = f'({bs})'
            return f'{bs}+{e[2]}' if e[2] >= 0 else f'{bs}-{-e[2]}'
        if k == 'op':
            name = e[1]
            if name in qdecomp.INFIX and len(e) == 4:
                return f'({R(e[2])} {qdecomp.INFIX[name]} {R(e[3])})'
            return f'{name}(' + ','.join(R(x) if isinstance(x, tuple) else str(x) for x in e[2:]) + ')'
        if k == 'ld':
            s = f'[{self.render_addr(F, e[2], bind, mode, d + 1)}]'
            return f'{e[3]}{s}' if e[3] is not None else s
        if k == 'ret':
            call = F['calls'].get(e[1])
            if call is None:
                return f'ret#{e[1]:X}'
            if call['k'] in ('V', 'TAILV'):
                return self.vcall_text(call)
            nm = self.fname(call['t'])
            if mode == 'expr' and d < 6:
                T = self.facts(call['t'])
                vals = self.ret_vals(T, e[2]) if T else []
                if len(vals) == 1:
                    return self.render(T, vals[0][1], (F, call['regs'], bind), mode, d + 1)
            return f'{nm}()' + ('' if e[2] == 'r3' else '.' + e[2])
        if k == 'undef':
            return '?' + e[1]
        if k == 'phi':
            ind = self.induction(F, e[1], e[2])
            if ind is not None:
                base, step, anys = ind
                if not step:
                    return R(base)
                if base == ('c', 0) and not anys:
                    return f'{step}*?loop'
                return f'({R(base)} + {step}*?loop)' if not anys else f'({R(base)} + {step}*?loop(any sign))'
            if mode == 'expr' and d < 8:
                ins = F['phis'].get(e[2], {}).get(e[1], [])
                parts = []
                for p, v in ins[:4]:
                    if v is None or (p is not None and p >= e[2]):
                        parts.append(f'{self.path_cond(F, p, e[2]) or "?"}: ?loop')
                    else:
                        parts.append(f'{self.path_cond(F, p, e[2]) or "?"}: {R(v)}')
                return 'phi(' + ' | '.join(parts) + (' | ...' if len(ins) > 4 else '') + ')'
            return f'{e[1]}@{e[2]:08X}'
        if k == 'crphi':
            return f'{e[1]}@{e[2]:08X}'
        if k == 'def':
            if e[1].startswith('L'):
                st = self.local_stores(F, e)
                if len(st) == 1:
                    return R(st[0]['val'])
                return f'local[sp+{e[1][1:]}]'
            full = self.full_def(F, e)
            return R(full) if full is not None else f'{e[1]}@+{e[2]:X}'
        if k == 'cmp':
            return f'cmp({R(e[2])},{R(e[3])})'
        if k == 'raw':
            return e[1]
        if k == 'lr':
            return 'lr'
        return str(e)

    def render_addr(self, F, e, bind, mode, d):
        """an address as base + terms + offset (the linear form: an induction's step prints as STEP*?loop)"""
        c, terms = self.lin(F, e, bind)
        if not terms:
            return qdecomp.fnum(c & 0xFFFFFFFF if abs(c) >= 0x10000 else c)
        ptr = [t for t in terms if t[0] == 1 and t[3] == 'atom' and t[2][2][0] in ('in', 'ld', 'ret', 'sp', 'def')]
        order = ptr + [t for t in terms if t not in ptr]
        parts = []
        for coef, _k, (AF, ab, a), kind in order:
            if kind != 'atom':
                s = '?loop' + (' (any sign)' if kind == 'any' else '')
            else:
                s = self.render(AF, a, ab, mode, d + 1)
            if coef == 1:
                parts.append(s)
            elif coef == -1:
                parts.append(f'-{s}')
            else:
                parts.append(f'{coef}*{s}')
        out = ' + '.join(parts).replace('+ -', '- ')
        if c:
            cs = qdecomp.fnum(abs(c)) if abs(c) < 0x10000 else qdecomp.fnum(c & 0xFFFFFFFF)
            out += (f' - {cs}' if c < 0 and abs(c) < 0x10000 else f' + {cs}')
        return out

    def clip(self, s, n=260):
        return s if len(s) <= n else s[:n] + '...'

    def vcall_text(self, call):
        ctr = call.get('ctr')
        slot = None
        if ctr and ctr[0] == 'ld':
            a = ctr[2]
            slot = a[2] if a[0] == 'add' else 0
        return f'?vcall(+{slot})' if slot is not None else '?vcall(?)'

    def ret_vals(self, T, reg):
        """[(ret record, value)] distinct by value"""
        out, seen = [], set()
        for r in T['rets']:
            v = r[reg]
            if v[0] == 'undef':
                continue
            kk = ekey(v)
            if kk not in seen:
                seen.add(kk)
                out.append((r, v))
        return out

    # ---------------------------------------------------------------- canonical addresses
    def linear(self, F, e, bind, d=0):
        """e as const + sum(coef * atom): (const, [(coef, key, (F, bind, atom), kind)]); kind 'loop' for an induction's step"""
        k = e[0]
        if d > 20:
            return 0, [(1, ekey(e), (F, bind, e), 'atom')]
        if k == 'c':
            return e[1], []
        if k == 'add':
            c, t = self.linear(F, e[1], bind, d + 1)
            return c + e[2], t
        if k == 'op' and len(e) == 4 and e[1] in ('+', '-'):
            c1, t1 = self.linear(F, e[2], bind, d + 1)
            c2, t2 = self.linear(F, e[3], bind, d + 1)
            if e[1] == '-':
                c2, t2 = -c2, [(-a, b, x, y) for a, b, x, y in t2]
            return c1 + c2, t1 + t2
        if k == 'op' and len(e) == 4 and e[1] in ('*', '<<'):
            a, b = e[2], e[3]
            if a[0] == 'c' and e[1] == '*':
                a, b = b, a
            if b[0] == 'c':
                m = b[1] if e[1] == '*' else (1 << b[1] if 0 <= b[1] < 32 else None)
                if m is not None:
                    c, t = self.linear(F, a, bind, d + 1)
                    return c * m, [(x * m, y, z, w) for x, y, z, w in t]
        if k == 'op' and len(e) == 3 and e[1] == 'neg':
            c, t = self.linear(F, e[2], bind, d + 1)
            return -c, [(-a, b, x, y) for a, b, x, y in t]
        if k == 'def' and not e[1].startswith('L'):
            full = self.full_def(F, e)
            if full is not None:
                return self.linear(F, full, bind, d + 1)
        if k == 'in' and bind is not None and e[1] in bind[1]:
            return self.linear(bind[0], bind[1][e[1]], bind[2], d + 1)
        if k == 'phi':
            ind = self.induction(F, e[1], e[2])
            if ind is not None:
                base, step, anys = ind
                c, t = self.linear(F, base, bind, d + 1)
                if step:
                    t = t + [(abs(step), f'loop@{F["addr"]:X}:{e[2]:X}', (F, bind, e), 'any' if anys else 'loop')]
                return c, t
        return 0, [(1, ekey(e) + repr(F['addr']), (F, bind, e), 'atom')]

    def lin(self, F, e, bind):
        """linear() with the terms of one atom summed (`r11*16 + (r11<<2)*16` is 80*r11) and zero terms dropped"""
        c, terms = self.linear(F, e, bind)
        acc, order = {}, []
        for coef, key, ctx, kind in terms:
            if key not in acc:
                order.append(key)
                acc[key] = [0, ctx, kind]
            acc[key][0] += coef
        return c, [(acc[k][0], k, acc[k][1], acc[k][2]) for k in order if acc[k][0]]

    def canon(self, F, e, bind=None, d=0):
        """the canonical field of an address expression: (root, (c, g, anysign), index atoms) or None"""
        c, terms = self.lin(F, e, bind)
        ptr = [t for t in terms if t[3] == 'atom' and t[0] == 1 and t[2][2][0] in ('in', 'ld', 'ret', 'sp', 'def')]
        rest = [t for t in terms if t not in ptr]
        if len(ptr) > 1:
            pref = [t for t in ptr if t[2][2][0] in ('in', 'ld', 'ret')]
            if len(pref) == 1:
                rest += [t for t in ptr if t is not pref[0]]
                ptr = pref
        g, anys = 0, False
        for coef, _k, _x, kind in rest:
            g = math.gcd(g, abs(coef))
            if kind != 'loop':
                anys = True
        if not ptr:
            return ('K',), (c & 0xFFFFFFFF, g, anys), rest
        if len(ptr) > 1:
            return ('u',), (c, g, anys), rest
        PF, pbind, atom = ptr[0][2]
        root, (c2, g2, a2) = self.root_of(PF, atom, pbind, d)
        g = math.gcd(g, g2)
        return root, (c + c2, g, anys or a2), rest

    def root_of(self, F, atom, bind, d):
        """(root, lattice of the offset the root adds)"""
        k = atom[0]
        z = (0, 0, False)
        if k == 'in':
            mk = (F['addr'], atom[1])
            if mk not in self._argroot:
                self._argroot[mk] = self._arg_root(F, atom, d)
            return self._argroot[mk]
        return self._root_other(F, atom, bind, d)

    def _arg_root(self, F, atom, d):
        z = (0, 0, False)
        if True:
            reg = atom[1]
            vts = self.src.arg_types(F['addr'], reg)
            if vts:
                r, o = self.type_root(vts)
                return r, (o, 0, False)
            if d < 3:
                got = []
                for src, site, _kind in self.src.callers(F['addr'])[:12]:
                    S = self.facts(src)
                    call = S['calls'].get(site - src) if S else None
                    if call is None or reg not in call['regs']:
                        got = None
                        break
                    cn = self.canon(S, call['regs'][reg], None, d + 1)
                    if cn is None:
                        got = None
                        break
                    got.append(cn)
                if got:
                    r0 = got[0][0]
                    if all(same_root(r0, x[0]) for x in got) and r0[0] not in ('u', 'arg'):
                        c0 = got[0][1][0]
                        g = 0
                        anys = False
                        for _r, (c1, g1, a1), _x in got:
                            g = math.gcd(g, math.gcd(g1, abs(c1 - c0)))
                            anys = anys or a1
                        return r0, (c0, g, anys or any(x[1][0] < c0 for x in got))
            return ('arg', F['addr'], reg), z

    def _root_other(self, F, atom, bind, d):
        k = atom[0]
        z = (0, 0, False)
        if k == 'ld':
            inner = self.canon(F, atom[2], bind, d)
            if inner is None:
                return ('u',), z
            r, (c, g, _a), _rest = inner
            if r[0] == 'K' and not g:
                vts = self.src.global_types(c)
                if vts:
                    r, o = self.type_root(vts)
                    return r, (o, 0, False)
                gr = self.global_root(c, d) if d < 2 else None
                if gr is not None:
                    return gr, z
                return ('g', c), z
            if not g and r[0] not in ('u',):
                return ('p', r, c), z
            return ('u',), z
        if k == 'ret':
            call = F['calls'].get(atom[1])
            vts = self.src.ret_types(F['addr'], F['addr'] + atom[1]) if call else set()
            if vts:
                r, o = self.type_root(vts)
                return r, (o, 0, False)
            if call and call['k'] in ('C', 'TAIL') and d < 3:
                T = self.facts(call['t'])      # the callee's own return value, typed in the callee (a constructor returns its this)
                rv = [self.canon(T, r['r3'], None, d + 1) for r in T['rets'] if r['r3'][0] != 'undef'] if T else []
                # a constructor returns its this (a class root); S111 QFIX5: a getter returns the object a global word points to (root ('g', addr), offset 0)
                if rv and all(x is not None and x[0][0] in ('type', 'g') and same_root(x[0], rv[0][0]) and x[1] == rv[0][1] for x in rv):
                    return rv[0][0], rv[0][1]
            return ('r', F['addr'], F['addr'] + atom[1]), z
        if k == 'sp':
            return ('sp', F['addr']), z
        return ('u',), z

    # ---------------------------------------------------------------- writers of a field
    def candidates(self, root, F):
        k = root[0]
        if k == 'type':
            vts = set(root[1])
            for v in list(vts):
                vts |= self.sub_vts(v)
            out = set(self.src.type_funcs(vts))
            for v in root[1]:
                for g in self.globals_of(v):
                    out |= self.src.g_funcs(g)
            return out
        if k == 'g':
            return self.src.g_funcs(root[1])
        if k == 'p':
            return self.candidates(root[1], F)
        if k in ('arg', 'r'):
            return {root[1]}
        return set()

    def writers(self, F, root, lat, width):
        """[(func addr, store, mark, via)] the stores to (root, lattice) found in the candidate functions and their direct callees"""
        out, seen = [], set()
        if root[0] == 'K':
            cands = set(self.src.k_writers(lat[0], lat[1]))
        else:
            cands = set(self.candidates(root, F))
        cands.add(F['addr'])
        cands = sorted(cands)[:MAX_CANDS]
        callee_scan = root[0] in ('type', 'g', 'p')

        def scan(T, bind, mark, via):
            for s in T['stores']:
                if s['stk'] is not None or (T['addr'], s['o']) in seen:
                    continue
                cn = self.canon(T, s['af'], bind)
                if cn is None:
                    continue
                r2, lat2, _x = cn
                ok = same_root(root, r2)
                m = mark
                if not ok and mark == 'L?' and root[0] == 'type' and r2[0] == 'g':
                    ok, m = True, 'L'
                if ok and lat_meet(lat, lat2):
                    seen.add((T['addr'], s['o']))
                    out.append((T['addr'], s, 'C' if m in ('C', 'L?') else m, via, r2, bind))

        for c in cands:
            T = self.facts(c)
            if T is None:
                continue
            scan(T, None, 'C', None)
            if not callee_scan:
                continue
            for rel, call in sorted(T['calls'].items()):
                if call['k'] not in ('C', 'TAIL') or call['t'] in cands:
                    continue
                kinds = []
                for reg, v in call['regs'].items():
                    if not reg.startswith('r') or v[0] in ('c', 'undef'):
                        continue
                    cn = self.canon(T, v, None, 2)
                    if cn is None:
                        continue
                    if same_root(root, cn[0]):
                        kinds.append('C')
                    elif root[0] == 'type' and cn[0][0] == 'g':
                        kinds.append('L?')
                if not kinds:
                    continue
                U = self.facts(call['t'])
                if U is None:
                    continue
                scan(U, (T, call['regs'], None), 'C' if 'C' in kinds else 'L?', (T['addr'], rel))
        return out

    # ---------------------------------------------------------------- the walk
    def budget_ok(self):
        if self.nodes >= self.budget:
            self.cut += 1
            return False
        self.nodes += 1
        return True

    def value(self, F, e, d, bind=None, label=None, path=()):
        """-> Node explaining expression e of function F (bind: F is a callee inlined at a call: its in_rN are the caller's)"""
        text = self.clip(self.render(F, e, bind))
        label = label or 'value'
        if not self.budget_ok():
            return Node(label, text, 'cut', note='... (budget: --budget N for more)')
        k = e[0]
        key = (F['addr'], ekey(e), id(bind[1]) if bind else None)
        if key in path:
            return Node(label, text, 'cycle', note='this value again: a cycle')
        path = path + (key,)
        if k == 'c':
            return Node(label, text, 'const', note='constant')
        if k == 'in':
            if bind is not None and e[1] in bind[1]:
                return self.value(bind[0], bind[1][e[1]], d, bind[2], label, path).add_note(
                    f'{F["name"]}\'s {e[1]}, bound to the call')
            return self.arg_node(F, e[1], d, label, path)
        if k == 'ld':
            return self.field_node(F, e, d, bind, label, path)
        if k == 'ret':
            return self.ret_node(F, e, d, bind, label, path)
        if k == 'phi':
            return self.phi_node(F, e, d, bind, label, path)
        if k == 'def':
            if e[1].startswith('L'):
                st = self.local_stores(F, e)
                if len(st) == 1:
                    return self.value(F, st[0]['val'], d, bind, label, path).add_note(
                        f'local[sp+{e[1][1:]}] read at +0x{e[2]:X}, stored at +0x{st[0]["o"]:X}')
                kids = [self.value(F, s['val'], d, bind, f'stored at +0x{s["o"]:X}', path) for s in st[:MAX_PATHS]]
                return Node(label, f'local[sp+{e[1][1:]}]', 'local', kids,
                            note=f'read at +0x{e[2]:X}: one of {len(st)} stores to the slot' if st else 'no store to the slot in this function')
            full = self.full_def(F, e)
            if full is not None:
                return self.value(F, full, d, bind, label, path)
            return Node(label, text, 'leaf')
        if k == 'op' or k == 'add':
            if k == 'add' and qdecomp.is_stack(e):
                return Node(label, text, 'stack', note='an address on this function\'s stack')
            atoms = []
            self.atoms(F, e, atoms, bind)
            kids = [self.value(AF, a, d, ab, self.clip(self.render(AF, a, ab), 120), path) for AF, a, ab in atoms]
            return Node(label, text, 'op', kids)
        if k == 'undef':
            return Node(label, f'?{e[1]}', 'leaf', note='the register a call before this point clobbered (no value)')
        return Node(label, text, 'leaf')

    def atoms(self, F, e, out, bind, d=0):
        """the non-constant leaves of an op tree (one entry each), resolving long defs and bound arguments"""
        if d > 30:
            return
        k = e[0]
        if k == 'c':
            return
        if k == 'op':
            for x in e[2:]:
                if isinstance(x, tuple):
                    self.atoms(F, x, out, bind, d + 1)
            return
        if k == 'add' and not qdecomp.is_stack(e):
            return self.atoms(F, e[1], out, bind, d + 1)
        if k == 'def' and not e[1].startswith('L') and self.full_def(F, e) is not None:
            return self.atoms(F, self.full_def(F, e), out, bind, d + 1)
        if k == 'in' and bind is not None and e[1] in bind[1]:
            return self.atoms(bind[0], bind[1][e[1]], out, bind[2], d + 1)
        if k == 'raw' and e[1].startswith('?carry'):
            return
        if not any(o[0] is F and o[1] == e for o in out):
            out.append((F, e, bind))

    def arg_node(self, F, reg, d, label, path):
        callers = self.src.callers(F['addr'])
        lab = f'argument {reg} of {F["name"]}'
        if not callers:
            vs = self.src.vslots(F['addr'])
            why = ('called through ' + ', '.join(f'vtable {hx(v)} slot {i}' for v, i in vs[:4]) + ': no direct caller (q vtable)'
                   if vs else 'no direct caller in the q index')
            return Node(label, f'in_{reg}', 'arg', note=f'{lab}: {why}')
        if d >= self.depth:
            return Node(label, f'in_{reg}', 'arg', note=f'{lab}: {len(callers)} caller sites ... (depth: --depth N for more)')
        kids = []
        for src, site, kind in callers[:MAX_CALLERS]:
            S = self.facts(src)
            call = S['calls'].get(site - src) if S else None
            where = f'caller {self.fname(src)}+0x{site - src:X} {reg}' + (' (tail)' if kind == 'tail' else '')
            if call is None or reg not in call['regs']:
                kids.append(Node(where, '?', 'leaf', note='not known at the call'))
                continue
            kids.append(self.value(S, call['regs'][reg], d + 1, None, where, path))
        more = len(callers) - MAX_CALLERS
        return Node(label, f'in_{reg}', 'arg', kids, note=f'{lab}: {len(callers)} caller site' + ('s' if len(callers) != 1 else '') +
                    (f' ({more} more not sliced: q args {F["name"]} --reg {reg})' if more > 0 else ''))

    def ret_node(self, F, e, d, bind, label, path):
        call = F['calls'].get(e[1])
        if call is None:
            return Node(label, f'ret#{e[1]:X}', 'leaf', note='a call result the walk has no call record for')
        if call['k'] in ('V', 'TAILV'):
            ctr = call.get('ctr')
            obj = self.render(F, ctr, bind) if ctr else '?'
            return Node(label, self.vcall_text(call), 'vcall', note=f'the result of the indirect call at +0x{e[1]:X} (target {obj})')
        T = self.facts(call['t'])
        nm = self.fname(call['t'])
        lab = f'return {e[2]} of {nm} (call at +0x{e[1]:X})'
        if T is None:
            return Node(label, f'{nm}()', 'leaf', note=lab + ': no body (import / helper)')
        if d >= self.depth:
            return Node(label, f'{nm}()', 'ret', note=lab + ' ... (depth: --depth N for more)')
        vals = self.ret_vals(T, e[2])
        cb = (F, call['regs'], bind)
        if len(vals) == 1:
            return self.value(T, vals[0][1], d + 1, cb, label, path).add_note(lab)
        kids = []
        for r, v in vals[:MAX_PATHS]:
            c = self.block_cond(T, r['blk'])
            kids.append(self.value(T, v, d + 1, cb, f'if {c}: return (+0x{r["o"]:X})' if c else f'return (+0x{r["o"]:X})', path))
        return Node(label, f'{nm}()', 'ret', kids, note=lab + f': {len(vals)} return values')

    def phi_node(self, F, e, d, bind, label, path):
        reg, j = e[1], e[2]
        ind = self.induction(F, reg, j)
        if ind is not None:
            base, step, anys = ind
            if not step:
                return self.value(F, base, d, bind, label, path).add_note(f'{reg}@{j:08X}: every path holds the same value')
            n = self.value(F, base, d, bind, 'on entry to the loop', path)
            note = f'{reg}@{j:08X} is a loop induction: the value on entry + {step} per iteration ' \
                f'(?loop = the iteration{", any sign" if anys else ""}; loop at @{j:08X})'
            return Node(label, self.clip(self.render(F, e, bind)), 'loop', [n], note=note)
        ins = F['phis'].get(j, {}).get(reg, [])
        kids = []
        for p, v in ins[:MAX_PATHS]:
            c = self.path_cond(F, p, j) or 'a path'
            where = f'@{p:08X}' if p is not None else '@entry'
            if v is None:
                kids.append(Node(f'if {c} ({where})', '?', 'leaf', note='not known on that path'))
            elif p is not None and p >= j:
                kids.append(Node(f'if {c} ({where}, back edge)', '?loop', 'loop',
                                 note=f'the previous iteration\'s {self.clip(self.render(F, v, bind), 120)}'))
            else:
                kids.append(self.value(F, v, d, bind, f'if {c} ({where})', path))
        if len(ins) > MAX_PATHS:
            kids.append(Node(f'+{len(ins) - MAX_PATHS} more inputs', 'q decomp ' + F['name'], 'leaf'))
        return Node(label, f'{reg}@{j:08X}', 'phi', kids, note=f'differs by path ({len(ins)} inputs)')

    def table_words(self, c, g, n=MAX_TABLE):
        ws = [self.src.word(c + g * i) for i in range(n)]
        ws = [w for w in ws if w is not None]
        if not ws:
            return ''
        fl = [struct.unpack('>f', struct.pack('>I', w))[0] for w in ws]
        if all(x == 0 or 1e-6 <= abs(x) <= 1e7 for x in fl if x == x):
            return 'image f32: ' + ', '.join(qdecomp.fmt_float(x) for x in fl)
        return 'image words: ' + ', '.join(f'0x{w:X}' for w in ws)

    def field_node(self, F, e, d, bind, label, path):
        text = self.clip(self.render(F, e, bind))
        width = e[1]
        cn = self.canon(F, e[2], bind)
        if cn is None:
            return Node(label, text, 'field', note='a load')
        root, lat, rest = cn
        c, g, anys = lat
        lab = f'field (w{width})'
        kids = []
        if root[0] == 'K':
            if g:
                tw = self.table_words(c, g)
                lab = f'a table at {hx(c)}, stride {g}' + (f'; {tw}' if tw else '')
                for AF, ab, a in [x[2] for x in rest]:
                    kids.append(self.value(AF, a, d, ab, 'index ' + self.clip(self.render(AF, a, ab), 120), path))
                return Node(label, text, 'table', kids, note=lab)
            w = self.src.word(c)
            lab += f': the word at {hx(c)}' + (f', image value {e[3] if e[3] is not None else f"0x{w:X}"}' if e[3] is not None or w is not None else '')
        else:
            lab += f': {root_text(root)} +{c}' + (f' + {g}*k' if g else '')
        fkey = (repr(root), lat)
        if fkey in self.seen_fields:
            return Node(label, text, 'field', note=lab + f' (writers: see #{self.seen_fields[fkey]} above)')
        if d >= self.depth:
            return Node(label, text, 'field', note=lab + ' ... (depth: --depth N for its writers)')
        if root[0] in ('u', 'sp'):
            return Node(label, text, 'field', note=lab + '; writers not searched (the base is not named)')
        self.num += 1
        num = self.num
        self.seen_fields[fkey] = num
        ws = self.writers(F, root, lat, width)
        if not ws:
            lab += '; no writer found' + (' in the field index' if root[0] == 'K' else ' in the class / global functions or their callees')
        else:
            lab += f'; {len(ws)} writer store' + ('s' if len(ws) != 1 else '')
        bydf = {}
        for wr in ws:
            bydf.setdefault(wr[0], []).append(wr)
        nsl = 0
        for fa, grp in bydf.items():
            T = self.facts(fa)
            first = grp[0]
            _fa, s, mark, via, r2, wbind = first
            vtxt = f' via {self.fname(via[0])}+0x{via[1]:X}' if via else ''
            base = '' if same_root(root, r2) else f'; its base: {root_text(r2)}, not proven to be this object'
            more = f' (+{len(grp) - 1} more stores in this function at ' + ', '.join(f'+0x{x[1]["o"]:X}' for x in grp[1:6]) + \
                ('...' if len(grp) > 6 else '') + ')' if len(grp) > 1 else ''
            wl = f'writer {T["name"]}+0x{s["o"]:X} [{mark}]{vtxt}{base}{more}: [{self.clip(self.render_addr(T, s["af"], wbind, "tree", 0), 120)}]'
            if nsl >= MAX_WRITERS:
                kids.append(Node(wl, self.clip(self.render(T, s['val'], wbind)), 'writer', note='not sliced: MAX_WRITERS'))
                continue
            nsl += 1
            kids.append(self.value(T, s['val'], d + 1, wbind, wl, path))
        n = Node(label, text, 'field', kids, note=lab)
        n.num = num
        return n

    def start_arg(self, F, rel, spec):
        """--arg rN[+K]: the word at [rN+K] live at the call at +rel (a pointer call argument: the create record's +0 at the create).
        A stack address reads that local slot (its stores in this function; a callee that wrote through the pointer is no store here:
        the walk says 'no store to the slot'); any other address is a load of that word."""
        m = ARG_RE.match(spec.strip())
        if not m:
            raise SliceError(f'--arg {spec!r}: rN or rN+K (K decimal or 0x hex), the word at [rN+K] at the call')
        call = F['calls'].get(rel)
        if call is None:
            raise SliceError(f'+0x{rel:X} is not a call in {F["name"]} (calls at: ' + ', '.join(f'+0x{r:X}' for r in sorted(F['calls'])) + ')')
        reg = m.group(1)
        if reg not in call['regs']:
            raise SliceError(f'{reg} has no known value at the call +0x{rel:X} (known: ' + ', '.join(sorted(call['regs'])) + ')')
        k = int(m.group(3), 0) * (-1 if m.group(2) == '-' else 1) if m.group(3) else 0
        base = call['regs'][reg]
        if qdecomp.is_stack(base):
            off = qdecomp.stack_off(base) + k
            return ('def', f'L{off}', rel), f'the word at [{reg}{k:+d}] = local[sp+{off}] live at the call at +0x{rel:X}'
        addr = base if not k else ('add', base, k)
        return ('ld', 4, addr, None), f'the word at [{reg}{k:+d}] live at the call at +0x{rel:X}'

    # ---------------------------------------------------------------- the start
    def start(self, F, rel, opnd):
        ins = self.src.ins(F).get(rel)
        if ins is None:
            near = sorted(self.src.ins(F))
            raise SliceError(f'{F["name"]} has no instruction at +0x{rel:X} (offsets run 0x0..0x{near[-1]:X} in steps of 4; q asm {F["name"]} --at {rel:X})')
        if opnd.startswith('arg:'):
            return self.start_arg(F, rel, opnd[4:])
        if opnd in ('[mem]', 'mem', 'val'):
            st = next((s for s in F['stores'] if s['o'] == rel), None)
            if st is not None:
                return st['val'], f'the value {ins["mn"]} stores to [{self.render(F, st["af"])}]'
            if ins['mn'] in qdecomp.LOADS and 'a' in ins:
                return ins['a'], f'the value {ins["mn"]} loads into {ins["d"]}'
            raise SliceError(f'+0x{rel:X} ({ins["mn"]} {ins["ops"]}) is not a load or store: name a register')
        if not REG_RE.match(opnd):
            raise SliceError(f'operand {opnd!r}: a register (r9, f13) or [mem]')
        toks = [t for t in qdecomp.split_ops(ins['ops'])]
        srcs = set(ins['b']) - ({ins['d']} if ins.get('d') and ins['d'] not in toks[1:] and
                                 not any(re.search(r'\(%s\)' % ins['d'], t) for t in toks[1:]) else set())
        if opnd in srcs:
            return ins['b'][opnd], f'{opnd} as {ins["mn"]} reads it'
        if ins.get('d') == opnd:
            return ins['a'], f'{opnd} as {ins["mn"]} writes it'
        raise SliceError(f'{opnd} is not an operand of +0x{rel:X} ({ins["mn"]} {ins["ops"]}); its registers: ' +
                         (', '.join(sorted(ins['b'])) or 'none'))


ARG_RE = re.compile(r'^(r\d+)\s*(?:([+-])\s*(0x[0-9A-Fa-f]+|\d+))?$')


# ------------------------------------------------------------------ printing
def tree_lines(n, pad=0, out=None):
    out = [] if out is None else out
    num = f'#{n.num} ' if n.num else ''
    txt = n.label + (f' = {n.text}' if n.text and n.text != n.label else '') + (f'   -- {n.note}' if n.note else '')
    out.append('  ' * pad + '- ' + num + txt)
    for k in n.kids:
        tree_lines(k, pad + 1, out)
    return out


def parse_site(s):
    m = re.match(r'^(.+?)\s*\+\s*(?:0x)?([0-9A-Fa-f]+)$', s.strip())
    if not m:
        raise SliceError(f'site {s!r}: sub_X+OFF (OFF hex, as q decomp prints it: sub_826979F8+E0)')
    return m.group(1), int(m.group(2), 16)


def slice_site(src, addr, rel, opnd, depth=6, budget=160):
    """-> (root Node, expr text, header text, slicer)"""
    sl = Slicer(src, depth=depth, budget=budget)
    F = src.facts(addr)
    if F is None:
        raise SliceError(f'0x{addr:08X} has no generated body (import / helper)')
    e, how = sl.start(F, rel, opnd)
    ins = src.ins(F)[rel]
    head = f'{F["name"]}+0x{rel:X} {opnd}: {how}   ({ins["mn"]} {ins["ops"]})'
    expr = sl.clip(sl.render(F, e, None, 'expr'), 600)
    root = sl.value(F, e, 0, None, f'{F["name"]}+0x{rel:X} {opnd}')
    return root, expr, head, sl


def main(argv, con):
    if '--struct' in argv:      # the stack struct at a call: each field's reaching stores (tools/q/qevstruct.py)
        import qevstruct
        try:
            return qevstruct.main(argv, con)
        except qevstruct.StructError as ex:
            raise SliceError(str(ex))
    import argparse
    ap = argparse.ArgumentParser(prog='q slice', description=__doc__.split('\n')[0])
    ap.add_argument('site', help='sub_X+OFF (OFF hex as q decomp prints it)')
    ap.add_argument('operand', nargs='?', help='a register (r9, f13) or [mem] (the value a store stores / a load loads)')
    ap.add_argument('--arg', metavar='rN[+K]', help='instead of an operand: the word at [rN+K] live at the CALL at the site (a pointer argument, '
                    'e.g. --arg r5 = the create record\'s +0 at the create)')
    ap.add_argument('--depth', type=int, default=6, help='hops into callers / callees / field writers (default 6)')
    ap.add_argument('--budget', type=int, default=160, help='most chain lines (default 160)')
    ap.add_argument('--expr', action='store_true', help='only the value as one expression in the start function\'s input terms')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--no-cache', action='store_true', help='re-read every function (ignore out/harness/index/slice/)')
    a = ap.parse_args(argv)
    t0 = time.time()
    fn_s, rel = parse_site(a.site)
    import q as qmod
    try:
        addr = qmod.resolve(con, fn_s)[0]
    except Exception as ex:  # noqa: BLE001 - q.py run as __main__ owns another QError class: said as ours
        if type(ex).__name__ == 'QError':
            raise SliceError(str(ex))
        raise
    src = QSource(con, use_cache=not a.no_cache)
    if (a.operand is None) == (a.arg is None):
        raise SliceError('name an operand (r9, [mem]) or --arg rN[+K], not both / neither')
    root, expr, head, sl = slice_site(src, addr, rel, a.operand if a.arg is None else 'arg:' + a.arg, a.depth, a.budget)
    dt = time.time() - t0
    tail = (f'({sl.nodes} chain nodes, {len(src._facts)} functions read ({src.read} analysed now, the rest from '
            f'out/harness/index/slice/), {dt:.1f} s; depth {a.depth}' + (f'; {sl.cut} not expanded: --budget' if sl.cut else '') + ')')
    if a.json:
        print(json.dumps({'site': head, 'expr': expr, 'chain': root.js(), 'functions': len(src._facts), 'seconds': round(dt, 2)}, indent=1))
        return 0
    if a.expr:
        print(expr)
        return 0
    print('q slice ' + head)
    print('value: ' + expr)
    print('chain (sink first; [C] = read off the listing by the tool, [L] = a writer whose base is not proven to be this object):')
    for l in tree_lines(root, 1):
        print(l)
    print(tail)
    return 0
