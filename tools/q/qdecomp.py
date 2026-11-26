"""q decomp: a compact, mechanical view of ONE recompiled game function (S103 lane QD).

    py tools/q/q.py decomp FUNC [--callers N] [--fields] [--json] [--no-cache] [--raw-names]

Reads the PPC `// mnemonic operands` comments of the local generated listing (generated/default, never committed)
and runs a small abstract interpreter over them: constants and `lis`/`addi` address pairs fold, a register's value
keeps its provenance (entry argument `in_r3`, `[in_r3+112]`, `[[0x83930050]+80]`, a call result `ret2`, a loop
value `r30@82842724`), and only the events that matter are printed per basic block: loads, stores, calls, branches,
return. MECHANICAL: no intent is guessed; an instruction the interpreter does not model becomes an opaque
`mnemonic(args)` value; a register value it cannot know prints as `?rN` / `rN@block`.

The output (a cache under out/harness/index/decomp/, keyed by the sha1 of the generated source file, so it rebuilds
when the recompile changes) is derived from the game and lives only in the gitignored out/ folder. Never commit it.
Notation, limits and the proof: docs/agents/q_decomp.md. The db overlay (names, labels) is applied at print time, so
db edits show at once without a rebuild.
"""
import hashlib
import json
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qdecomp_phi  # noqa: E402  what flowed into a join; the audit
import qfactcard  # noqa: E402  S111 FACTCARD: the settled claims printed in this output
REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
GEN = os.path.join(REPO, 'generated', 'default')
IMAGE = os.path.join(REPO, 'out', 'harness', 'index', 'image.bin')
CACHE_DIR = os.path.join(REPO, 'out', 'harness', 'index', 'decomp')
IMAGE_BASE = 0x82000000
VERSION = 4          # bump when the output format changes: old cache files are not read

INS_RE = re.compile(r'^\t// ([A-Za-z][\w.+\-]*) ?(.*?)\s*$')
LABEL_RE = re.compile(r'^loc_([0-9A-F]{8}):')
DEF_RE = re.compile(r'^DEFINE_REX_FUNC\((\w+)\)')
GOTO_RE = re.compile(r'goto loc_([0-9A-F]{8});')
HELPER_RE = re.compile(r'^__(save|rest)(gpr|fpr|vmx)')
REG_RE = re.compile(r'^(r\d+|f\d+|v\d+)$')
MEM_RE = re.compile(r'^(-?(?:0x[0-9a-fA-F]+|\d+))\((r\d+)\)$')
M32 = 0xFFFFFFFF


class DecompError(Exception):
    pass


# ------------------------------------------------------------------ expressions
# tuples. ('c', n) constant; ('in', 'r3') entry value; ('sp',); ('add', base, off) base + constant;
# ('op', name, args...) opaque or arithmetic op; ('ld', width, addr, imgval|None) value loaded from memory;
# ('ret', site, 'r3'|'f1') a call result; ('undef', 'r5') clobbered by a call; ('phi', 'r30', blockaddr);
# ('def', reg, site) a long value named where it was computed; ('lr',) the link register at entry;
# ('cmp', kind, a, b) a compare record held in a CR field; ('raw', text).
INFIX = {'+': '+', '-': '-', '*': '*', '/': '/', '&': '&', '|': '|', '^': '^', '<<': '<<', '>>': '>>',
         '>>s': '>>', '!=': '!='}


def sc(n):
    n &= M32
    return n - (1 << 32) if n >= 0xFFFF0000 else n


def fnum(n):
    if -0x10000 < n < 0x10000:
        return str(n)
    return f'0x{n & ((1 << 64) - 1) if n < -0x80000000 else n & M32:X}'


def fmt_float(v):
    if v != v:
        return 'nan'
    if math.isinf(v):
        return 'inf' if v > 0 else '-inf'
    s = f'{v:.9g}'
    try:
        for d in range(1, 10):
            t = f'{v:.{d}g}'
            if struct.unpack('>f', struct.pack('>f', float(t)))[0] == v:
                s = t
                break
        if v != 0 and abs(v) < 1:
            n = round(1 / abs(v))
            if n > 1 and struct.unpack('>f', struct.pack('>f', 1.0 / n))[0] == abs(v):
                s += f' (1/{n})'
    except (OverflowError, ZeroDivisionError):
        pass
    return s


def depth(e):
    if e[0] in ('op', 'add', 'ld', 'cmp'):
        return 1 + max((depth(x) for x in e[1:] if isinstance(x, tuple)), default=0)
    return 0


def fmt(e):
    k = e[0]
    if k == 'c':
        return fnum(e[1])
    if k == 'in':
        return 'in_' + e[1]
    if k == 'sp':
        return 'sp'
    if k == 'lr':
        return 'lr'
    if k == 'add':
        b = e[1]
        bs = fmt(b)
        if b[0] == 'op':
            bs = f'({bs})' if bs[0] != '(' else bs
        off = e[2]
        return f'{bs}+{off}' if off >= 0 else f'{bs}-{-off}'
    if k == 'op':
        name = e[1]
        if name in INFIX and len(e) == 4:
            return f'({fmt(e[2])} {INFIX[name]} {fmt(e[3])})'
        return f'{name}(' + ','.join(fmt(x) if isinstance(x, tuple) else str(x) for x in e[2:]) + ')'
    if k == 'ld':
        s = f'[{fmt(e[2])}]'
        if e[3] is not None:
            return f'{e[3]}{s}'
        return s
    if k == 'ret':
        return f'ret#{e[1]:X}' + ('' if e[2] == 'r3' else '.' + e[2])
    if k == 'undef':
        return '?' + e[1]
    if k in ('phi', 'crphi'):
        return f'{e[1]}@{e[2]:08X}'
    if k == 'def':
        return f'{e[1]}@+{e[2]:X}'
    if k == 'cmp':
        return f'cmp({fmt(e[2])},{fmt(e[3])})'
    if k == 'raw':
        return e[1]
    return str(e)


def mk_add(b, off):
    if b[0] == 'c':
        s = (b[1] + off)
        return ('c', s & M32 if (abs(s) >= 0x10000) else s)
    if b[0] == 'add':
        return mk_add(b[1], b[2] + off) if b[2] + off != 0 else b[1]
    if off == 0:
        return b
    return ('add', b, off)


def split_ops(s):
    return [x.strip() for x in s.split(',')] if s else []


def int_of(s):
    return int(s, 0)


# ------------------------------------------------------------------ listing
class Insn:
    __slots__ = ('addr', 'mn', 'ops', 'line', 'cases', 'rel')

    def __init__(self, addr, mn, ops, line):
        self.addr, self.mn, self.ops, self.line, self.cases, self.rel = addr, mn, ops, line, None, 0


def file_sha1(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def read_listing(path, line, name, start):
    """(insns, labels, first_line, last_line): parse the function body from the generated .cpp. `line` = the DEFINE line
    the index recorded (1-based); if it does not hold the DEFINE (the index is stale) the file is searched."""
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.read().split('\n')
    i = (line or 0) - 1
    if not (0 <= i < len(lines)) or not (m := DEF_RE.match(lines[i])) or m.group(1) != name:
        i = next((k for k, l in enumerate(lines) if l.startswith(f'DEFINE_REX_FUNC({name})')), -1)
        if i < 0:
            raise DecompError(f'{name} not found in {os.path.basename(path)}')
    insns, labels = [], {}
    addr = start
    j = i + 1
    last_bctr = None
    while j < len(lines) and lines[j] != '}':
        l = lines[j]
        m = LABEL_RE.match(l)
        if m:
            a = int(m.group(1), 16)
            addr = a
            labels[a] = j + 1
            last_bctr = None
        else:
            m = INS_RE.match(l)
            if m:
                ins = Insn(addr, m.group(1), m.group(2), j + 1)
                insns.append(ins)
                last_bctr = ins if ins.mn == 'bctr' else None
                addr += 4
            elif last_bctr is not None:
                g = GOTO_RE.search(l)
                if g:
                    last_bctr.cases = (last_bctr.cases or []) + [int(g.group(1), 16)]
        j += 1
    return insns, labels, i + 1, j + 1


# ------------------------------------------------------------------ image (initial values of read-only data)
class Image:
    def __init__(self):
        self.data = None
        try:
            with open(IMAGE, 'rb') as f:
                self.data = f.read()
        except OSError:
            pass

    def word(self, a):
        if self.data is None or a < IMAGE_BASE or a + 4 > IMAGE_BASE + len(self.data) or a & 3:
            return None
        return struct.unpack('>I', self.data[a - IMAGE_BASE:a - IMAGE_BASE + 4])[0]


# ------------------------------------------------------------------ the interpreter
CMP_OPS = {'beq': '==', 'bne': '!=', 'blt': '<', 'bgt': '>', 'ble': '<=', 'bge': '>=', 'bnl': '>=', 'bng': '<='}
NEG = {'==': '!=', '!=': '==', '<': '>=', '>': '<=', '<=': '>', '>=': '<'}
CR_BIT = {'beq': 'eq', 'bne': '!eq', 'blt': 'lt', 'bgt': 'gt', 'ble': '!gt', 'bge': '!lt', 'bnl': '!lt', 'bng': '!gt',
          'bso': 'so', 'bns': '!so', 'bun': 'un', 'bnu': '!un'}
LOADS = {  # mnemonic: (width, kind, update, indexed)
    'lwz': (4, 'u32'), 'lwzu': (4, 'u32'), 'lwzx': (4, 'u32'), 'lwzux': (4, 'u32'), 'lhz': (2, 'u16'),
    'lhzu': (2, 'u16'), 'lhzx': (2, 'u16'), 'lhzux': (2, 'u16'), 'lha': (2, 's16'), 'lhau': (2, 's16'),
    'lhax': (2, 's16'), 'lbz': (1, 'u8'), 'lbzu': (1, 'u8'), 'lbzx': (1, 'u8'), 'lbzux': (1, 'u8'),
    'ld': (8, 'u64'), 'ldu': (8, 'u64'), 'ldx': (8, 'u64'), 'ldux': (8, 'u64'), 'lwa': (4, 's32'), 'lwax': (4, 's32'),
    'lwbrx': (4, 'u32 byteswapped'), 'lhbrx': (2, 'u16 byteswapped'), 'lwarx': (4, 'u32 reserve'),
    'ldarx': (8, 'u64 reserve'),
    'lfs': (4, 'f32'), 'lfsu': (4, 'f32'), 'lfsx': (4, 'f32'), 'lfsux': (4, 'f32'),
    'lfd': (8, 'f64'), 'lfdu': (8, 'f64'), 'lfdx': (8, 'f64'),
    'lvx': (16, 'vec'), 'lvx128': (16, 'vec'), 'lvlx': (16, 'vec left'), 'lvlx128': (16, 'vec left'),
    'lvrx': (16, 'vec right'), 'lvrx128': (16, 'vec right'), 'lvewx': (4, 'vec elem u32'), 'lvewx128': (4, 'vec elem u32'),
    'lvehx': (2, 'vec elem u16'), 'lvebx': (1, 'vec elem u8'),
}
STORES = {
    'stw': (4, 'u32'), 'stwu': (4, 'u32'), 'stwx': (4, 'u32'), 'stwux': (4, 'u32'), 'sth': (2, 'u16'),
    'sthu': (2, 'u16'), 'sthx': (2, 'u16'), 'sthux': (2, 'u16'), 'stb': (1, 'u8'), 'stbu': (1, 'u8'),
    'stbx': (1, 'u8'), 'stbux': (1, 'u8'), 'std': (8, 'u64'), 'stdu': (8, 'u64'), 'stdx': (8, 'u64'),
    'stdux': (8, 'u64'), 'stwbrx': (4, 'u32 byteswapped'), 'sthbrx': (2, 'u16 byteswapped'),
    'stwcx.': (4, 'u32 conditional'), 'stdcx.': (8, 'u64 conditional'),
    'stfs': (4, 'f32'), 'stfsu': (4, 'f32'), 'stfsx': (4, 'f32'), 'stfsux': (4, 'f32'), 'stfd': (8, 'f64'),
    'stfdu': (8, 'f64'), 'stfdx': (8, 'f64'), 'stfiwx': (4, 'f32 as int'),
    'stvx': (16, 'vec'), 'stvx128': (16, 'vec'), 'stvlx': (16, 'vec left'), 'stvlx128': (16, 'vec left'),
    'stvrx': (16, 'vec right'), 'stvrx128': (16, 'vec right'), 'stvewx': (4, 'vec elem u32'),
    'stvewx128': (4, 'vec elem u32'), 'stvehx': (2, 'vec elem u16'), 'stvebx': (1, 'vec elem u8'),
}
UPDATE_FORMS = {m for m in list(LOADS) + list(STORES) if m.endswith('u') or m.endswith('ux')} - {'lhau_'}
NO_DEST = ('st', 'cmp', 'b', 'mt', 'tw', 'td', 'dcb', 'icb', 'sync', 'lwsync', 'eieio', 'fcmp', 'nop', 'db16cyc',
           'cctp', 'cr', 'isync', 'mtfsf', 'mtfsb')
FLOAT_BIN = {'fadds': '+', 'fadd': '+', 'fsubs': '-', 'fsub': '-', 'fdivs': '/', 'fdiv': '/'}
FLOAT_MUL = {'fmuls', 'fmul'}
FMA = {'fmadds': ('', '+'), 'fmadd': ('', '+'), 'fmsubs': ('', '-'), 'fmsub': ('', '-'),
       'fnmadds': ('-', '+'), 'fnmadd': ('-', '+'), 'fnmsubs': ('-', '-'), 'fnmsub': ('-', '-')}


def entry_value(r):
    if r == 'r1':
        return ('sp',)
    return ('in', r)


def vol_regs():
    return ['r0'] + [f'r{i}' for i in range(3, 13)] + [f'f{i}' for i in range(0, 14)] + \
           [f'v{i}' for i in list(range(0, 14)) + list(range(32, 128))]


class State:
    # fresh: registers written since the last call on every path here (the likely call arguments); ca: the XER carry as
    # an expression (None = not known); nopred: the block has no known predecessor (a register it never wrote is unknown)
    __slots__ = ('regs', 'cr', 'stack', 'ctr', 'lr', 'fresh', 'ca', 'nopred')

    def __init__(self):
        self.regs, self.cr, self.stack, self.ctr, self.lr = {}, {}, {}, None, None
        self.fresh, self.ca, self.nopred = set(), None, False

    def copy(self):
        s = State()
        s.regs, s.cr, s.stack, s.ctr, s.lr = dict(self.regs), dict(self.cr), dict(self.stack), self.ctr, self.lr
        s.fresh, s.ca, s.nopred = set(self.fresh), self.ca, self.nopred
        return s

    def get(self, r):
        v = self.regs.get(r)
        if v:
            return v
        if self.nopred and r != 'r1':
            return ('raw', f'?{r} ({qdecomp_phi.NO_PRED})')
        return entry_value(r)


def merge(states, addr, preds, fn):
    """Join of the predecessor states (`preds`: their block addresses, None = the function entry): a register keeps its
    value when every predecessor agrees, else it becomes a join value `rN@block` and qdecomp_phi keeps what each
    predecessor held (printed at the block). Same for ctr / lr and a CR field. Stack slots survive only when all agree."""
    if len(states) == 1:
        return states[0].copy()
    qdecomp_phi.reset(fn, addr)
    out = State()
    keys = set()
    for s in states:
        keys |= set(s.regs)
    for k in keys:
        vals = [s.get(k) for s in states]
        if all(v == vals[0] for v in vals):
            out.regs[k] = vals[0]
        else:
            out.regs[k] = ('phi', k, addr)
            qdecomp_phi.record(fn, 'reg', k, addr, zip(preds, vals))
    for k in set().union(*[set(s.stack) for s in states]):
        vals = [s.stack.get(k) for s in states]
        if all(v is not None and v == vals[0] for v in vals):
            out.stack[k] = vals[0]
    for k in set().union(*[set(s.cr) for s in states]):
        vals = [s.cr.get(k) for s in states]
        if all(v is not None and v == vals[0] for v in vals):
            out.cr[k] = vals[0]
        elif any(v is not None for v in vals):
            out.cr[k] = ('crphi', k, addr)
            qdecomp_phi.record(fn, 'cr', k, addr, zip(preds, vals))
    for nm in ('ctr', 'lr'):
        vals = [getattr(s, nm) for s in states]
        if all(v == vals[0] for v in vals):
            setattr(out, nm, vals[0])
        else:
            setattr(out, nm, ('phi', nm, addr))
            qdecomp_phi.record(fn, nm, nm, addr, zip(preds, vals))
    # an argument is a register written since the last call on EVERY path here: one only some paths wrote may be stale
    # (a previous loop iteration's value, or a value from before another path's call) and is not printed as one
    out.fresh = set.intersection(*[s.fresh for s in states])
    out.ca = states[0].ca if all(s.ca == states[0].ca for s in states) else None
    out.nopred = any(s.nopred for s in states)
    return out


class Block:
    def __init__(self, idx, insns):
        self.idx = idx
        self.insns = insns
        self.start = insns[0].addr
        self.end = insns[-1].addr + 4
        self.line = insns[0].line
        self.succ = []          # (kind, addr): 'fall', 'br', 'case'
        self.term = None        # dict describing the terminator
        self.events = []
        self.entry = None
        self.exit = None


class Func:
    def __init__(self, name, addr, insns, labels, names, image):
        self.name, self.addr, self.insns, self.labels = name, addr, insns, labels
        self.names = names              # callable addr -> name or None
        self.image = image
        self.end = insns[-1].addr + 4 if insns else addr
        self.frame = None
        self.saves = set()
        self.helpers = []
        self.converged = True
        self.blocks = []
        self.by_start = {}

    # ---- structure
    def target_inside(self, t):
        return t is not None and self.addr <= t < self.end

    def build_blocks(self):
        leaders = {self.insns[0].addr}
        for i, ins in enumerate(self.insns):
            t = branch_target(ins)
            if t is not None and self.target_inside(t) and ins.mn not in ('bl',):
                leaders.add(t)
            if ins.cases:
                leaders.update(c for c in ins.cases if self.target_inside(c))
            if is_terminator(ins) and i + 1 < len(self.insns):
                leaders.add(self.insns[i + 1].addr)
        leaders |= set(self.labels)
        cur = []
        for ins in self.insns:
            if ins.addr in leaders and cur:
                self.blocks.append(Block(len(self.blocks), cur))
                cur = []
            cur.append(ins)
        if cur:
            self.blocks.append(Block(len(self.blocks), cur))
        self.by_start = {b.start: b for b in self.blocks}
        for b in self.blocks:
            last = b.insns[-1]
            t = branch_target(last)
            nxt = self.by_start.get(b.end)
            mn = last.mn.rstrip('+-')
            uncond = mn in ('b', 'blr', 'bctr')
            if mn == 'bctr' and last.cases:
                b.succ = [('case', c) for c in dict.fromkeys(last.cases) if c in self.by_start]
            elif mn == 'b':
                if t in self.by_start:
                    b.succ = [('br', t)]
            elif mn.startswith('b') and mn not in ('bl', 'blr', 'bctrl') and not mn.endswith('lr') and \
                    not mn.endswith('ctr') and not mn.endswith('ctrl'):
                if t in self.by_start:
                    b.succ.append(('br', t))
                if nxt:
                    b.succ.append(('fall', nxt.start))
            elif not uncond:
                if nxt:
                    b.succ.append(('fall', nxt.start))
        self.preds = {b.start: [] for b in self.blocks}
        for b in self.blocks:
            for kind, t in b.succ:
                self.preds[t].append((b.start, kind))


def branch_target(ins):
    last = ins.ops.rsplit(',', 1)[-1].strip() if ins.ops else ''
    if ins.mn in ('blr', 'bctr', 'bctrl') or ins.mn.endswith('lr') or ins.mn.endswith('ctr') or ins.mn.endswith('ctrl'):
        return None
    if ins.mn.startswith('b') and last.startswith('0x'):
        try:
            return int(last, 16)
        except ValueError:
            return None
    return None


def is_terminator(ins):
    m = ins.mn.rstrip('+-')
    if m in ('bl', 'bctrl'):
        return False
    return m.startswith('b') and m not in ('bl',)


# ------------------------------------------------------------------ analysis of one block
class Ctx:
    """One run of the interpreter over a block: collects events."""

    def __init__(self, fn, blk, st):
        self.fn, self.blk, self.st = fn, blk, st
        self.events = []
        self.term = None

    @property
    def fresh(self):            # registers written since the last call on every path here: the likely arguments of the next
        return self.st.fresh

    @fresh.setter
    def fresh(self, v):
        self.st.fresh = v

    # registers
    def g(self, r):
        return self.st.get(r)

    def gv(self, tok):
        """operand token -> expression (register or immediate)."""
        if REG_RE.match(tok):
            return self.g(tok)
        try:
            return ('c', int_of(tok))
        except ValueError:
            return ('raw', tok)

    def setreg(self, r, e, site):
        if r == 'r1':
            return
        if depth(e) > 7 or len(fmt(e)) > 110:
            full = fmt(e)
            rel = site - self.fn.addr
            self.events.append({'k': 'D', 'site': site, 'text': f'{r}@+{rel:X} = {full[:240]}' + ('...' if len(full) > 240 else '')})
            e = ('def', r, rel)
        self.st.regs[r] = e
        self.fresh.add(r)

    def ea(self, ra, off_or_rb, site):
        """effective address expression. ra == r0 means zero in an address."""
        base = ('c', 0) if ra == 'r0' else self.g(ra)
        if isinstance(off_or_rb, int):
            return mk_add(base, off_or_rb)
        rb = self.g(off_or_rb) if off_or_rb != 'r0' else ('c', 0)
        if base[0] == 'c' and base[1] == 0:
            return rb
        if rb[0] == 'c':
            return mk_add(base, rb[1])
        if base[0] == 'c':
            return mk_add(rb, base[1])
        return ('op', '+', base, rb)


def run_block(fn, blk, st):
    c = Ctx(fn, blk, st)
    for ins in blk.insns:
        try:
            step(c, ins)
        except (ValueError, KeyError, IndexError) as e:
            raise DecompError(f'{fn.name} @0x{ins.addr:08X} {ins.mn} {ins.ops}: {e!r}')
    blk.events, blk.term = c.events, c.term
    return c.st


def is_stack(addr):
    b = addr
    while b[0] == 'add':
        b = b[1]
    return b[0] == 'sp'


def stack_off(addr):
    return addr[2] if addr[0] == 'add' else 0


def volatile_after_call(c, site):
    for r in vol_regs():
        if r in c.st.regs or True:
            c.st.regs[r] = ('undef', r)
    c.st.regs['r3'] = ('ret', site - c.fn.addr, 'r3')
    c.st.regs['f1'] = ('ret', site - c.fn.addr, 'f1')
    c.fresh = set()
    c.st.cr = {}
    c.st.lr = None
    c.st.ctr = None
    c.st.ca = None
    # a callee may write our locals through a pointer we passed: only prologue saves survive a call
    c.st.stack = {k: v for k, v in c.st.stack.items() if v[0] == 'save'}


def arg_list(c, forward=False):
    """Likely arguments (the arity is not in the listing): registers written since the last call in this block, r3
    always, and the entry arguments r3.. that the function never touched (a forwarded argument list)."""
    show = set(i for i in range(3, 11) if f'r{i}' in c.fresh)
    show.add(3)
    i = 3
    while forward and i <= 10 and c.g(f'r{i}') == ('in', f'r{i}'):    # a tail call forwards the untouched arguments
        show.add(i)
        i += 1
    out = []
    for i in sorted(show):
        v = c.g(f'r{i}')
        if v[0] == 'undef':
            continue
        out.append(f'r{i}={fmt(v)}')
    for i in range(1, 14):
        v = c.g(f'f{i}')
        if f'f{i}' in c.fresh and v[0] != 'undef':
            out.append(f'f{i}={fmt(v)}')
    return out


def callee_name(c, t):
    n = c.fn.names(t)
    return n or f'sub_{t:08X}'


def cond_of(c, mn, ops):
    """(text, negated text, structured) for a conditional branch mnemonic and operands."""
    m = mn.rstrip('+-')
    toks = split_ops(ops)
    cr = 'cr0'
    if toks and toks[0].startswith('cr'):
        cr = toks[0]
    if m in ('bdnz', 'bdz'):
        return ('--ctr != 0' if m == 'bdnz' else '--ctr == 0'), ('--ctr == 0' if m == 'bdnz' else '--ctr != 0'), \
               {'op': '!=' if m == 'bdnz' else '==', 'a': '--ctr', 'b': '0', 'unsigned': False}
    base = m[:-2] if m.endswith('lr') and m != 'blr' else m
    base = base[:-3] if base.endswith('ctr') and len(base) > 3 else base
    if base in CMP_OPS:
        cmp_ = c.st.cr.get(cr)
        if cmp_ is not None and cmp_[0] == 'cmp':
            op = CMP_OPS[base]
            kind, a, b = cmp_[1], fmt(cmp_[2]), fmt(cmp_[3])
            uns = kind.startswith('u') and op not in ('==', '!=')
            if kind == 'f' and base in ('ble', 'bge', 'bng', 'bnl'):
                pass
            return f'{a} {op}{"u" if uns else ""} {b}', f'{a} {NEG[op]}{"u" if uns else ""} {b}', \
                   {'op': op, 'a': a, 'b': b, 'unsigned': uns}
    if base in CR_BIT:
        bit = CR_BIT[base]
        neg = bit.startswith('!')
        crv = c.st.cr.get(cr)
        name = f'{fmt(crv) if crv is not None and crv[0] == "crphi" else cr}.{bit.lstrip("!")}'
        return (f'!{name}' if neg else name), (name if neg else f'!{name}'), None
    return f'{mn} {ops}', f'!({mn} {ops})', None


def step(c, ins):
    mn, ops, site = ins.mn, ins.ops, ins.addr
    st = c.st
    t = split_ops(ops)
    rec = mn.endswith('.') and mn not in ('stwcx.', 'stdcx.')
    base_mn = mn[:-1] if rec else mn
    m0 = mn.rstrip('+-') if mn.startswith('b') else mn

    def done(r, e):
        c.setreg(r, e, site)
        if rec:
            st.cr['cr0'] = ('cmp', 'i32', c.g(r), ('c', 0))

    ca_in = st.ca                               # the carry this instruction reads; whatever writes XER.CA clears it
    if base_mn in qdecomp_phi.CA_WRITERS:
        st.ca = None

    # ---- moves, immediates
    if base_mn == 'li':
        return done(t[0], ('c', int_of(t[1])))
    if base_mn == 'lis':
        return done(t[0], ('c', (int_of(t[1]) << 16) & M32))
    if base_mn in ('mr', 'fmr', 'vmr'):
        return done(t[0], c.g(t[1]))
    if base_mn == 'addi':
        return done(t[0], mk_add(('c', 0) if t[1] == 'r0' else c.g(t[1]), int_of(t[2])))
    if base_mn == 'addis':
        return done(t[0], mk_add(('c', 0) if t[1] == 'r0' else c.g(t[1]), int_of(t[2]) << 16))
    if base_mn in ('addic', 'addic.'):
        st.cr['cr0'] = None
        st.ca = qdecomp_phi.ca_addic(c.g(t[1]), int_of(t[2]))
        return c.setreg(t[0], mk_add(c.g(t[1]), int_of(t[2])), site)
    if base_mn == 'add':
        a, b = c.g(t[1]), c.g(t[2])
        if b[0] == 'c':
            return done(t[0], mk_add(a, b[1]))
        if a[0] == 'c':
            return done(t[0], mk_add(b, a[1]))
        return done(t[0], ('op', '+', a, b))
    if base_mn == 'subf':   # rd = rb - ra
        a, b = c.g(t[1]), c.g(t[2])
        if a[0] == 'c':
            return done(t[0], mk_add(b, -a[1]))
        return done(t[0], ('op', '-', b, a))
    if base_mn == 'subfic':
        return done(t[0], ('op', '-', ('c', int_of(t[2])), c.g(t[1])))
    if base_mn == 'neg':
        return done(t[0], ('op', 'neg', c.g(t[1])))
    if base_mn in ('mullw', 'mulld'):
        return done(t[0], ('op', '*', c.g(t[1]), c.g(t[2])))
    if base_mn == 'mulli':
        return done(t[0], ('op', '*', c.g(t[1]), ('c', int_of(t[2]))))
    if base_mn in ('divw', 'divd'):
        return done(t[0], ('op', '/', c.g(t[1]), c.g(t[2])))
    if base_mn in qdecomp_phi.CA_USERS:
        return done(t[0], qdecomp_phi.ca_use(base_mn, [c.gv(x) for x in t[1:]], ca_in))
    if base_mn in ('divwu', 'divdu', 'mulhw', 'mulhwu', 'subfc', 'addc'):
        return done(t[0], ('op', base_mn, *[c.gv(x) for x in t[1:]]))
    if base_mn in ('and', 'or', 'xor'):
        a, b = c.g(t[1]), c.g(t[2])
        if base_mn == 'or' and a == b:
            return done(t[0], a)
        return done(t[0], ('op', {'and': '&', 'or': '|', 'xor': '^'}[base_mn], a, b))
    if base_mn in ('andc', 'orc', 'nor', 'nand', 'eqv'):
        return done(t[0], ('op', base_mn, c.g(t[1]), c.g(t[2])))
    if base_mn == 'not':
        return done(t[0], ('op', 'not', c.g(t[1])))
    if base_mn in ('ori', 'oris', 'xori', 'xoris', 'andi.', 'andis.', 'andi', 'andis'):
        imm = int_of(t[2])
        op = {'ori': '|', 'oris': '|', 'xori': '^', 'xoris': '^', 'andi': '&', 'andis': '&'}[base_mn.rstrip('.')]
        if base_mn.rstrip('.').endswith('s'):
            imm <<= 16
        a = c.g(t[1])
        if op == '|' and a[0] == 'c':
            e = ('c', (a[1] | imm) & M32)
        elif op == '|' and imm == 0:
            e = a
        else:
            e = ('op', op, a, ('c', imm))
        c.setreg(t[0], e, site)
        if mn.endswith('.'):
            st.cr['cr0'] = ('cmp', 'i32', c.g(t[0]), ('c', 0))
        return
    if base_mn in ('slw', 'srw', 'sld', 'srd'):
        return done(t[0], ('op', '<<' if base_mn in ('slw', 'sld') else '>>', c.g(t[1]), c.g(t[2])))
    if base_mn in ('sraw', 'srad'):
        return done(t[0], ('op', '>>s', c.g(t[1]), c.g(t[2])))
    if base_mn in ('srawi', 'sradi'):
        return done(t[0], ('op', '>>s', c.g(t[1]), ('c', int_of(t[2]))))
    if base_mn in ('extsb', 'extsh', 'extsw', 'cntlzw', 'cntlzd'):
        return done(t[0], ('op', base_mn, c.g(t[1])))
    if base_mn == 'clrlwi':
        return done(t[0], ('op', '&', c.g(t[1]), ('c', (1 << (32 - int_of(t[2]))) - 1)))
    if base_mn == 'clrldi':
        return done(t[0], ('op', '&', c.g(t[1]), ('c', (1 << (64 - int_of(t[2]))) - 1)))
    if base_mn == 'rotlwi':
        return done(t[0], ('op', 'rotl32', c.g(t[1]), ('c', int_of(t[2]))))
    if base_mn == 'rotldi':
        return done(t[0], ('op', 'rotl64', c.g(t[1]), ('c', int_of(t[2]))))
    if base_mn == 'rlwinm':
        sh, mb, me = int_of(t[2]), int_of(t[3]), int_of(t[4])
        x = c.g(t[1])
        mask = 0
        i = mb
        while True:
            mask |= 1 << (31 - i)
            if i == me:
                break
            i = (i + 1) % 32
        if sh == 0:
            return done(t[0], ('op', '&', x, ('c', mask)))
        if mb == 0 and me == 31 - sh:
            return done(t[0], ('op', '<<', x, ('c', sh)))
        if me == 31 and sh == 32 - mb:
            return done(t[0], ('op', '>>', x, ('c', mb)))
        return done(t[0], ('op', '&', ('op', 'rotl32', x, ('c', sh)), ('c', mask)))
    if base_mn in ('rlwimi', 'rldimi', 'rldicr', 'rldicl'):
        extra = [c.g(t[0])] if base_mn in ('rlwimi', 'rldimi') else []
        return done(t[0], ('op', base_mn, *extra, c.g(t[1]), *[('c', int_of(x)) for x in t[2:]]))
    # ---- compares
    if base_mn in ('cmpwi', 'cmplwi', 'cmpdi', 'cmpldi', 'cmpw', 'cmplw', 'cmpd', 'cmpld'):
        cr = 'cr0'
        a = t
        if a and a[0].startswith('cr'):
            cr, a = a[0], a[1:]
        kind = ('u' if 'l' in base_mn[3:4] else 'i') + ('64' if base_mn.endswith('d') or base_mn.endswith('di') else '32')
        st.cr[cr] = ('cmp', kind, c.g(a[0]), c.gv(a[1]))
        return
    if base_mn == 'fcmpu' or base_mn == 'fcmpo':
        st.cr[t[0]] = ('cmp', 'f', c.g(t[1]), c.g(t[2]))
        return
    # ---- special registers
    if base_mn == 'mflr':
        return c.setreg(t[0], ('lr',), site)
    if base_mn == 'mtlr':
        st.lr = c.g(t[0])
        return
    if base_mn == 'mtctr':
        st.ctr = c.g(t[0])
        return
    if base_mn == 'mfctr':
        return c.setreg(t[0], st.ctr or ('in', 'ctr'), site)
    # ---- loads / stores
    if mn in LOADS or mn in STORES:
        return mem_op(c, ins, t)
    if base_mn in ('dcbz', 'dcbzl'):
        a = c.ea(t[0], t[1], site)
        c.events.append({'k': 'W', 'site': site, 'w': 'zero' + ('128' if mn == 'dcbzl' else '32'),
                         'addr': fmt(a), 'stack': is_stack(a), 'val': '0', 'af': a})
        return
    if mn.startswith(('dcb', 'icb')) or mn in ('sync', 'lwsync', 'eieio', 'isync', 'nop', 'db16cyc', 'cctp'):
        return
    # ---- floating point
    if base_mn in FLOAT_BIN:
        return done_f(c, t[0], ('op', FLOAT_BIN[base_mn], c.g(t[1]), c.g(t[2])), site)
    if base_mn in FLOAT_MUL:
        return done_f(c, t[0], ('op', '*', c.g(t[1]), c.g(t[2])), site)
    if base_mn in FMA:
        neg, sign = FMA[base_mn]
        e = ('op', sign, ('op', '*', c.g(t[1]), c.g(t[2])), c.g(t[3]))
        return done_f(c, t[0], ('op', 'neg', e) if neg else e, site)
    if base_mn in ('fneg', 'fabs', 'fnabs', 'fsqrts', 'fsqrt', 'fres', 'frsqrte', 'frsp', 'fctiwz', 'fctidz', 'fctid',
                   'fcfid', 'fsel', 'fctiw'):
        return done_f(c, t[0], ('op', base_mn, *[c.g(x) for x in t[1:]]), site)
    if base_mn == 'mffs':
        return c.setreg(t[0], ('op', 'mffs'), site)
    # ---- calls and branches
    if m0 == 'bl':
        return do_call(c, ins, branch_target(ins), None)
    if m0 == 'bctrl':
        return do_call(c, ins, None, st.ctr)
    if m0 == 'blr':
        return do_return(c, ins, None)
    if m0 == 'bctr':
        return do_bctr(c, ins)
    if m0 == 'b':
        return do_branch(c, ins)
    if m0.startswith('b') and (m0 in CMP_OPS or m0 in CR_BIT or m0.startswith('bdn') or m0.startswith('bdz') or
                               m0[:-2] in CMP_OPS or m0[:-2] in CR_BIT or m0 in ('bt', 'bf', 'bc')):
        return do_cond(c, ins)
    if mn.startswith(('tw', 'td')):
        c.events.append({'k': 'X', 'site': site, 'text': f'{mn} {ops}  (trap/assert instruction)'})
        return
    # ---- unmodelled: an opaque op over the operands
    if t and REG_RE.match(t[0]) and not mn.startswith(NO_DEST):
        e = ('op', base_mn, *[c.gv(x) for x in t[1:]])
        c.setreg(t[0], e, site)
        if rec:
            st.cr['cr6' if base_mn.startswith('vcmp') else 'cr0'] = ('raw', f'cr[{base_mn}]')
        return
    c.events.append({'k': 'X', 'site': site, 'text': f'{mn} {ops}  (not modelled)'})


def done_f(c, r, e, site):
    c.setreg(r, e, site)


def mem_op(c, ins, t):
    mn, site, st = ins.mn, ins.addr, c.st
    if mn in LOADS:
        width, kind = LOADS[mn]
        is_store = False
    else:
        width, kind = STORES[mn]
        is_store = True
    indexed = mn.endswith('x') or mn.endswith('ux') or mn.endswith('x128') or mn.endswith('cx.')
    if indexed:
        a = c.ea(t[1], t[2], site)
        base_reg = t[1]
    else:
        m = MEM_RE.match(t[1])
        if not m:
            raise ValueError(f'bad memory operand {t[1]}')
        a = c.ea(m.group(2), int_of(m.group(1)), site)
        base_reg = m.group(2)
    rt = t[0]
    stk = is_stack(a)
    af = a
    if not is_store:
        if stk and is_reg_kind(rt) and (a, width, kind) in st.stack:
            # a spill reloaded: the value is the stored one; a reload of a prologue save is the epilogue (not shown)
            st.regs[rt] = st.stack[(a, width, kind)][1]
            c.fresh.add(rt)
            return
        img = None
        if a[0] == 'c' and width == 4 and kind in ('u32', 'f32'):
            w = c.fn.image.word(a[1])
            if w:
                img = fmt_float(struct.unpack('>f', struct.pack('>I', w))[0]) if kind == 'f32' else fnum(w)
        e = ('ld', width, a, img)
        if stk:     # a local read back from memory: named by its load site (the slot may be rewritten later)
            e = ('def', f'L{stack_off(a)}', site - c.fn.addr)
        c.events.append({'k': 'R', 'site': site, 'w': kind, 'addr': fmt(a), 'stack': stk, 'dest': rt, 'af': a,
                         'img': img})
        c.setreg(rt, e, site)
    else:
        v = c.g(rt) if is_reg_kind(rt) else ('c', 0)
        saved = stk and (v[0] == 'lr' or (v[0] == 'in' and v[1] != 'r1' and re.match(r'^(r(1[4-9]|2\d|3[01])|f(1[4-9]|2\d|3[01])|v\d+)$', v[1])))
        if mn == 'stfiwx':
            v = ('op', 'int32', v)
        if mn == 'stwu' and base_reg == 'r1':      # frame setup `stwu r1,-N(r1)`
            c.fn.frame = -stack_off(a) if stack_off(a) < 0 else c.fn.frame
            return
        if stk:
            key = (a, width, kind)
            # drop overlapping slots
            lo, hi = stack_off(a), stack_off(a) + width
            for k in [k for k in st.stack if stack_off(k[0]) < hi and stack_off(k[0]) + k[1] > lo]:
                del st.stack[k]
            st.stack[key] = ('save', v) if saved else ('val', v)
            if saved:
                fn = c.fn
                fn.saves.add('lr' if v[0] == 'lr' else v[1])
                return
        c.events.append({'k': 'W', 'site': site, 'w': kind, 'addr': fmt(a), 'stack': stk, 'val': fmt(v), 'af': af})
    # update forms: the base register receives the effective address
    if (mn.endswith('u') or mn.endswith('ux')) and base_reg != 'r1':
        c.setreg(base_reg, a, site)


def is_reg_kind(r):
    return bool(REG_RE.match(r))


def reg_vals(c):
    """S111 QARGS: every argument register's value at a call, as fmt text (`q args` keeps those the callee reads; a value passed through is `in_rN`)"""
    return {f'r{i}': fmt(c.g(f'r{i}')) for i in range(3, 11) if c.g(f'r{i}')[0] != 'undef'}


def do_call(c, ins, t, ctr):
    site = ins.addr
    st = c.st
    if t is not None:
        name = callee_name(c, t)
        if HELPER_RE.match(name):
            c.fn.helpers.append(name)
            return
        args = arg_list(c)
        c.events.append({'k': 'C', 'site': site, 'target': t, 'name': name, 'args': args, 'regs': reg_vals(c)})
    else:
        tgt = fmt(ctr) if ctr else '?'
        args = arg_list(c)
        c.events.append({'k': 'V', 'site': site, 'target': tgt, 'args': args, 'af': ctr, 'regs': reg_vals(c)})
    volatile_after_call(c, site)


def ret_values(c):
    out = []
    r3 = c.g('r3')
    if r3[0] != 'undef':
        out.append(f'r3={fmt(r3)}')
    f1 = c.g('f1')
    if f1[0] not in ('undef', 'in', 'ret'):     # an f1 that is a join value is printed (its inputs: the join lines)
        out.append(f'f1={fmt(f1)}')
    return out


def do_return(c, ins, cond):
    c.events.append({'k': 'RET', 'site': ins.addr, 'vals': ret_values(c), 'cond': cond})
    if cond is None:
        c.term = {'k': 'ret'}


def do_branch(c, ins):
    t = branch_target(ins)
    site = ins.addr
    if t is None:
        raise ValueError('b without target')
    if c.fn.target_inside(t):
        c.term = {'k': 'goto', 'target': t, 'site': site}
        return
    name = callee_name(c, t)
    if HELPER_RE.match(name):
        c.events.append({'k': 'RET', 'site': site, 'vals': ret_values(c), 'cond': None})
        c.fn.helpers.append(name)
        c.term = {'k': 'ret'}
        return
    c.events.append({'k': 'TAIL', 'site': site, 'target': t, 'name': name, 'args': arg_list(c, True), 'regs': reg_vals(c)})
    c.term = {'k': 'ret'}


def do_bctr(c, ins):
    if ins.cases:
        sel = ins.cases
        c.events.append({'k': 'SW', 'site': ins.addr, 'targets': list(dict.fromkeys(sel)),
                         'ctr': fmt(c.st.ctr) if c.st.ctr else '?'})
        c.term = {'k': 'switch'}
    else:
        c.events.append({'k': 'TAILV', 'site': ins.addr, 'target': fmt(c.st.ctr) if c.st.ctr else '?',
                         'args': arg_list(c, True), 'af': c.st.ctr})
        c.term = {'k': 'ret'}


def do_cond(c, ins):
    text, neg, struct_ = cond_of(c, ins.mn, ins.ops)
    mn = ins.mn.rstrip('+-')
    t = branch_target(ins)
    if mn.endswith('lr') and mn != 'blr':
        c.events.append({'k': 'RET', 'site': ins.addr, 'vals': ret_values(c), 'cond': text})
        c.term = {'k': 'fallthrough'}
        return
    if t is None:
        c.events.append({'k': 'X', 'site': ins.addr, 'text': f'{ins.mn} {ins.ops}  (indirect conditional, not modelled)'})
        c.term = {'k': 'fallthrough'}
        return
    if c.fn.target_inside(t):
        c.term = {'k': 'cond', 'cond': text, 'neg': neg, 's': struct_, 'target': t, 'site': ins.addr}
    else:
        c.events.append({'k': 'TAIL', 'site': ins.addr, 'target': t, 'name': callee_name(c, t), 'args': arg_list(c, True),
                         'cond': text})
        c.term = {'k': 'fallthrough'}


# ------------------------------------------------------------------ dataflow over the blocks
def analyse(fn):
    fn.build_blocks()
    exits = {}
    entries = {}
    for _round in range(12):
        changed = False
        for b in fn.blocks:
            if b.start == fn.addr:
                ps = [p for p, _ in fn.preds[b.start] if p in exits]
                srcs = [State()] + [exits[p] for p in ps]
                # the function entry state is the clean one; back edges into the entry merge with it
                st = merge(srcs, b.start, [None] + ps, fn) if len(srcs) > 1 else srcs[0]
            else:
                ps = [p for p, _ in fn.preds[b.start] if p in exits]
                srcs = [exits[p] for p in ps]
                if not srcs:
                    st = State()
                    st.nopred = True        # no known predecessor: a register the block never wrote is unknown, not an input
                else:
                    st = merge(srcs, b.start, ps, fn)
            if entries.get(b.start) is not None and _same(entries[b.start], st):
                pass
            else:
                changed = True
                entries[b.start] = st
            ex = run_block(fn, b, st.copy())
            if b.start not in exits or not _same(exits[b.start], ex):
                changed = True
            exits[b.start] = ex
            b.entry, b.exit = st, ex
        if not changed:
            fn.converged = True
            return
    fn.converged = False
    # no fixed point in 12 rounds: values stay as of the last round (still sound: join values only widen)


def _same(a, b):
    return a.regs == b.regs and a.cr == b.cr and a.stack == b.stack and a.ctr == b.ctr and a.lr == b.lr and \
        a.fresh == b.fresh and a.ca == b.ca and a.nopred == b.nopred


# ------------------------------------------------------------------ structure + text
def negate_struct(t):
    return t['neg']


def render_func(fn, db_head, listing_name, first_line):
    """-> (text lines, summary dict). Structured `if`/`do-while` where the shape is plain (single entry region)."""
    blocks = fn.blocks
    by_idx_start = {b.start: b.idx for b in blocks}
    # call ordinals: ret<site> labels are printed by site offset, no counter needed
    reads, writes = {}, {}
    calls = {}
    sites = {}
    vsites = []
    consts = []
    out = []
    ev_cache = {}
    counts = {'R': 0, 'W': 0, 'C': 0}

    def rel(a):
        return a - fn.addr

    def event_text(e):
        k = e['k']
        s = f'+{rel(e["site"]):X}'
        if k == 'R':
            a = e['addr']
            if e['stack']:
                return f'{s}  {e["dest"]} = {e["w"]} local[{a}]'
            note = f'  = {e["img"]} (image)' if e['img'] else ''
            return f'{s}  {e["dest"]} = {e["w"]} [{a}]{note}'
        if k == 'W':
            if e['stack']:
                return f'{s}  local[{e["addr"]}] = {e["val"]}  ({e["w"]})'
            return f'{s}  [{e["addr"]}] = {e["val"]}  ({e["w"]})'
        if k == 'C':
            return f'{s}  call#{rel(e["site"]):X} {e["name"]}(' + ', '.join(e['args']) + ')'
        if k == 'V':
            return f'{s}  vcall#{rel(e["site"]):X} *({e["target"]})(' + ', '.join(e['args']) + ')'
        if k == 'TAIL':
            cond = f'if ({e["cond"]}) ' if e.get('cond') else ''
            return f'{s}  {cond}tail {e["name"]}(' + ', '.join(e['args']) + ')'
        if k == 'TAILV':
            return f'{s}  tail *({e["target"]})(' + ', '.join(e['args']) + ')'
        if k == 'RET':
            cond = f'if ({e["cond"]}) ' if e.get('cond') else ''
            return f'{s}  {cond}return' + (' ' + ', '.join(e['vals']) if e['vals'] else '')
        if k == 'SW':
            return f'{s}  switch ({e["ctr"]}) -> ' + ' '.join(f'@{x:08X}' for x in e['targets'])
        return f'{s}  {e["text"]}'

    # gather summary data
    for b in blocks:
        for e in b.events:
            k = e['k']
            if k == 'R' or k == 'W':
                if e['stack']:
                    continue
                d = reads if k == 'R' else writes
                key = (e['addr'], e['w'])
                d[key] = d.get(key, 0) + 1
                counts[k] += 1
            elif k in ('C', 'TAIL'):
                calls[e['name']] = calls.get(e['name'], 0) + 1
                sites.setdefault(e['target'], []).append({'site': e['site'], 'args': e['args'],
                                                         'tail': k == 'TAIL', 'regs': e.get('regs')})
                counts['C'] += 1
            elif k in ('V', 'TAILV'):
                calls['*indirect'] = calls.get('*indirect', 0) + 1
                vsites.append({'site': e['site'], 'target': e['target'], 'regs': e.get('regs'), 'tail': k == 'TAILV'})
    # constants: immediates in compares
    seen = set()
    for b in blocks:
        for ins in b.insns:
            if ins.mn in ('cmpwi', 'cmplwi', 'cmpdi', 'cmpldi', 'mulli', 'li', 'subfic', 'ori', 'andi.'):
                try:
                    v = int_of(split_ops(ins.ops)[-1])
                except ValueError:
                    continue
                if v not in seen and abs(v) >= 16:
                    seen.add(v)
                    consts.append(fnum(v))
    lis_addrs = []
    for b in blocks:
        for e in b.events:
            if e['k'] in ('R', 'W') and not e['stack'] and e['af'][0] == 'c':
                if e['af'][1] not in lis_addrs:
                    lis_addrs.append(e['af'][1])

    # ---- structure
    preds = fn.preds
    out_lines = []
    emitted = set()

    def region_ok(i0, i1, head_idx=None):
        """blocks [i0, i1): only block i0 may be entered from outside the region (or the branch block)."""
        lo, hi = blocks[i0].start, blocks[i1].start if i1 < len(blocks) else fn.end
        for i in range(i0, i1):
            for p, kind in preds[blocks[i].start]:
                inside = lo <= p < hi
                if inside:
                    continue
                if i == i0:
                    continue
                return False
        return True

    def edges_from_outside(i0, i1, allowed_src, skip_first=False):
        lo, hi = blocks[i0].start, blocks[i1].start if i1 < len(blocks) else fn.end
        for i in range(i0 + (1 if skip_first else 0), i1):
            for p, kind in preds[blocks[i].start]:
                if lo <= p < hi or p == allowed_src:
                    continue
                return True
        return False

    def emit_block(b, ind, skip_term=False, term_override=None):
        pad = '  ' * ind
        marks = []
        tgt_preds = [p for p, kind in preds[b.start] if kind != 'fall' or p != b.start]
        back = [p for p, kind in preds[b.start] if p >= b.start]
        hdr = f'{pad}@{b.start:08X}  [listing :{b.line}]' + (f'  (loop head; back edge from {", ".join(f"@{p:08X}" for p in back)})' if back else '')
        out_lines.append(hdr)
        out_lines.append(qdecomp_phi.slot(b.start, pad))    # the join values this block defines (filled by the audit)
        for e in b.events:
            if e['k'] == 'RET' and e.get('cond') is None and False:
                continue
            out_lines.append(pad + '  ' + event_text(e))
        return

    def emit_term(b, ind, text=None):
        pad = '  ' * ind
        t = b.term
        if t is None:
            return
        if t['k'] == 'goto':
            out_lines.append(f'{pad}  +{rel(t["site"]):X}  goto @{t["target"]:08X}' + (' (loop back)' if t['target'] <= b.start else ''))
        elif t['k'] == 'cond':
            out_lines.append(f'{pad}  +{rel(t["site"]):X}  if ({t["cond"]}) goto @{t["target"]:08X}' + (' (loop back)' if t['target'] <= b.start else ''))

    def emit_range(i0, i1, ind, open_loops):
        i = i0
        while i < i1:
            b = blocks[i]
            # do-while loop: a back edge from block z (conditional or not) to this block, region single-entry
            backs = sorted([p for p, kind in preds[b.start] if p >= b.start and kind != 'fall'])
            if backs and i not in open_loops:
                z = by_idx_start[backs[-1]]
                zb = blocks[z]
                if z < i1 and zb.term and zb.term['k'] == 'cond' and zb.term['target'] == b.start \
                        and not edges_from_outside(i, z + 1, None, skip_first=True):
                    tail = zb.term
                    zb.term = {'k': 'loop_tail'}
                    out_lines.append('  ' * ind + 'do {')
                    emit_range(i, z + 1, ind + 1, open_loops | {i})
                    out_lines.append('  ' * ind + f'}} while ({tail["cond"]});   // +{rel(tail["site"]):X}')
                    i = z + 1
                    continue
            emit_block(b, ind)
            t = b.term
            if t and t['k'] == 'cond' and t['target'] > b.start and t['target'] in by_idx_start:
                T = by_idx_start[t['target']]
                if i + 1 < T <= i1 and not edges_from_outside(i + 1, T, b.start) and \
                        not any(p < blocks[i + 1].start and kind != 'fall' for p, kind in preds[blocks[i + 1].start] if p != b.start):
                    last = blocks[T - 1]
                    # if / else: the then-part ends in a plain `b END` past T
                    if last.term and last.term['k'] == 'goto' and last.term['target'] > blocks[T].start \
                            and last.term['target'] in by_idx_start:
                        E = by_idx_start[last.term['target']]
                        if E <= i1 and T < E and not edges_from_outside(T, E, b.start) and \
                                all(p in range(blocks[i].start, blocks[E].start) for p, _ in preds[blocks[T].start]):
                            out_lines.append('  ' * ind + f'if ({t["neg"]}) {{   // +{rel(t["site"]):X}, else @{blocks[T].start:08X}')
                            emit_range(i + 1, T, ind + 1, open_loops, ) if not _strip_last_goto(blocks, T - 1) else None
                            out_lines.append('  ' * ind + '} else {')
                            emit_range(T, E, ind + 1, open_loops)
                            out_lines.append('  ' * ind + f'}}   // join @{blocks[E].start:08X}')
                            i = E
                            continue
                    out_lines.append('  ' * ind + f'if ({t["neg"]}) {{   // +{rel(t["site"]):X}, skips to @{blocks[T].start:08X}')
                    emit_range(i + 1, T, ind + 1, open_loops)
                    out_lines.append('  ' * ind + '}')
                    i = T
                    continue
            emit_term(b, ind)
            i += 1

    def _strip_last_goto(blocks_, idx):
        blocks_[idx].term = {'k': 'goto_elided'}
        return False

    # the if/else path mutates a term; run on copies of the terms
    saved_terms = [b.term for b in blocks]
    emit_range(0, len(blocks), 0, frozenset())
    for b, t in zip(blocks, saved_terms):
        b.term = t
    out_lines, n_simplified = qdecomp_phi.audit(out_lines, fn, fmt)
    summary = {
        'reads': [{'addr': a, 'w': w, 'n': n} for (a, w), n in reads.items()],
        'writes': [{'addr': a, 'w': w, 'n': n} for (a, w), n in writes.items()],
        'calls': calls, 'sites': {f'{k:08X}': v for k, v in sites.items()}, 'vsites': vsites, 'consts': consts[:24],
        'consts_total': len(consts), 'globals_total': len(lis_addrs), 'simplified': n_simplified,
        'globals': [f'0x{a:08X}' for a in lis_addrs[:16]], 'frame': fn.frame, 'saves': sorted(fn.saves),
        'helpers': sorted(set(fn.helpers)), 'counts': counts, 'converged': fn.converged,
    }
    return out_lines, summary


def summary_lines(s):
    def fld(items):
        return ', '.join(f'[{x["addr"]}] {x["w"]}' + (f' x{x["n"]}' if x['n'] > 1 else '') for x in items) or '-'
    lines = [f'fields read:    {fld(s["reads"])}', f'fields written: {fld(s["writes"])}']
    cl = ', '.join(f'{k}' + (f' x{v}' if v > 1 else '') for k, v in s['calls'].items()) or '-'
    lines.append(f'calls:          {cl}')
    if s.get('simplified'):
        lines.append(f'WARNING: {s["simplified"]} names simplified: each is marked ?(simplified: see listing :A-:B) in the body')
    if s['globals']:
        more = s.get('globals_total', 0) - len(s['globals'])
        lines.append('absolute addrs: ' + ', '.join(s['globals']) +
                     (f'  (+{more} more not shown: {s["globals_total"]} in all; see listing)' if more > 0 else ''))
    if s['consts']:
        more = s.get('consts_total', 0) - len(s['consts'])
        lines.append('constants:      ' + ', '.join(s['consts']) +
                     (f'  (+{more} more not shown: {s["consts_total"]} in all; see listing)' if more > 0 else ''))
    bits = []
    if s['frame'] is not None:
        bits.append(f'frame {s["frame"]} bytes')
    if s['saves']:
        bits.append('saves ' + ','.join(s['saves']))
    if s['helpers']:
        bits.append('helpers ' + ','.join(s['helpers']))
    if not s.get('converged', True):
        lines.append('WARNING: the value analysis did not settle in 12 rounds; loop values may be stale')
    if bits:
        lines.append('prologue:       ' + '; '.join(bits) + '  (prologue/epilogue saves and reloads are not shown)')
    return lines


# ------------------------------------------------------------------ cache + public API
def _lookup(con, addr):
    r = con.execute('SELECT name FROM funcs WHERE addr = ?', (addr,)).fetchone()
    return r['name'] if r else None


_CODE_SHA = []


def _code_sha():
    """sha1 over qdecomp's own code (this file and qdecomp_phi.py): a change of either rebuilds every cache entry on demand,
    so no reader is shown text from an older qdecomp"""
    if not _CODE_SHA:
        h = hashlib.sha1()
        for n in ('qdecomp.py', 'qdecomp_phi.py'):
            with open(os.path.join(HERE, n), 'rb') as f:
                h.update(f.read())
        _CODE_SHA.append(h.hexdigest())
    return _CODE_SHA[0]


def _cache_path(addr, sha):
    return os.path.join(CACHE_DIR, f'{addr:08X}.{sha[:12]}.v{VERSION}.{_code_sha()[:10]}.json')


def decompile(con, addr, use_cache=True):
    """-> dict {addr, name, size, insns, listing, source_sha1, lines, summary}. `con` = the q index connection."""
    f = con.execute('SELECT * FROM funcs WHERE addr = ?', (addr,)).fetchone()
    if f is None or not f['file']:
        raise DecompError(f'0x{addr:08X} has no generated body (import / helper)')
    path = os.path.join(GEN, f['file'])
    if not os.path.exists(path):
        raise DecompError(f'generated source {path} is missing (generated/ is local only)')
    sha = _cached_sha1(path)
    cpath = _cache_path(addr, sha)
    if use_cache and os.path.exists(cpath):
        try:
            with open(cpath, encoding='utf-8') as fh:
                return json.load(fh)
        except (OSError, ValueError):
            pass
    r = decompile_uncached(con, f, path, sha)
    try:
        os.makedirs(CACHE_DIR, exist_ok=True)
        for old in os.listdir(CACHE_DIR):         # a stale entry for this function (older hash / version)
            if old.startswith(f'{addr:08X}.') and old != os.path.basename(cpath):
                try:
                    os.remove(os.path.join(CACHE_DIR, old))
                except OSError:
                    pass
        tmp = cpath + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as fh:
            json.dump(r, fh)
        os.replace(tmp, cpath)
    except OSError:
        pass
    return r


_SHA = {}


def _cached_sha1(path):
    """sha1 of the generated source file; memoised on (size, mtime) in decomp/_hashes.json so a query does not
    re-read a multi-MB file."""
    st = os.stat(path)
    sig = [st.st_size, st.st_mtime_ns]
    key = os.path.basename(path)
    hp = os.path.join(CACHE_DIR, '_hashes.json')
    if not _SHA:
        try:
            with open(hp, encoding='utf-8') as fh:
                _SHA.update(json.load(fh))
        except (OSError, ValueError):
            pass
    ent = _SHA.get(key)
    if ent and ent[0] == sig:
        return ent[1]
    h = file_sha1(path)
    _SHA[key] = [sig, h]
    try:
        os.makedirs(CACHE_DIR, exist_ok=True)
        with open(hp + '.tmp', 'w', encoding='utf-8') as fh:
            json.dump(_SHA, fh)
        os.replace(hp + '.tmp', hp)
    except OSError:
        pass
    return h


_IMG = None


def decompile_uncached(con, f, path, sha):
    insns, labels, first, last = read_listing(path, f['line'], f['name'], f['addr'])
    if not insns:
        raise DecompError(f'{f["name"]}: no instructions parsed')
    global _IMG
    if _IMG is None:
        _IMG = Image()
    image = _IMG
    cache = {}

    def names(a):
        if a not in cache:
            cache[a] = _lookup(con, a)
        return cache[a]

    fn = Func(f['name'], f['addr'], insns, labels, names, image)
    analyse(fn)
    lines, summ = render_func(fn, None, f['name'], first)
    return {'version': VERSION, 'addr': f['addr'], 'name': f['name'], 'size': f['size'], 'insns': f['insns'],
            'listing': f'{f["file"]}:{first}', 'source_sha1': sha, 'lines': lines, 'summary': summ}


# ------------------------------------------------------------------ CLI (called from q.py)
def _qmod():
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    import q
    return q


def db_overlay(addr_to_entry, text):
    """sub_XXXXXXXX -> sub_XXXXXXXX (db name) on call / tail lines (applied at print time: db edits show at once)."""
    def one(m):
        e = addr_to_entry.get(int(m.group(2), 16))
        return f'{m.group(1)}sub_{m.group(2)} ({e["name"]})' if e and e.get('name') else m.group(0)
    return re.sub(r'((?:call#[0-9A-F]+|tail) )sub_([0-9A-F]{8})', one, text)


def _names_overlay(dbi, line):
    if not line.startswith('calls:'):
        return line
    return re.sub(r'sub_([0-9A-F]{8})', lambda m: m.group(0) + (f' ({dbi[int(m.group(1), 16)]["name"]})'
                  if int(m.group(1), 16) in dbi and dbi[int(m.group(1), 16)].get('name') else ''), line)


def db_index():
    q = _qmod()
    d = {}
    for a, fname_, e in q.load_db_entries():
        d.setdefault(a, e)
    return d


def head_lines(r, dbi):
    out = [f'{r["name"]}  0x{r["addr"]:08X}  size {r["size"]} ({r["insns"]} insns)  listing generated/default/{r["listing"]}']
    e = dbi.get(r['addr'])
    if e:
        out.append(f'db: {e.get("name")} [{e.get("system")}, {e.get("confidence")}]: {str(e.get("summary", ""))[:200]}')
        labs = [x for x in e.get('evidence', []) if str(x).startswith('labels:')]
        if labs:
            out.append('    ' + str(labs[0]))
    return out


def callers_summary(con, addr, n, dbi):
    rows = con.execute('SELECT src, site, kind FROM edges WHERE dst = ? ORDER BY src, site', (addr,)).fetchall()
    srcs = []
    for r in rows:
        if r['src'] not in [x[0] for x in srcs]:
            srcs.append((r['src'], []))
        srcs[-1][1].append((r['site'], r['kind']))
    srcs = [s for s in srcs if s[0] != addr]
    q = _qmod()
    lines = [f'callers: {len(rows)} sites in {len(srcs)} functions' + (f' (first {n})' if len(srcs) > n else '')]
    for src, sites in srcs[:n]:
        nm = q.fname(con, src)
        e = dbi.get(src)
        head = f'  {nm}' + (f' ({e["name"]})' if e and e.get('name') else '')
        try:
            r = decompile(con, src)
        except DecompError as ex:
            lines.append(f'{head}: no body ({ex})')
            continue
        bits = []
        for site, kind in sites[:3]:
            hit = next((x for x in r['summary']['sites'].get(f'{addr:08X}', []) if x['site'] == site), None)
            if hit:
                bits.append(f'+{site - src:X} {"tail " if hit["tail"] else ""}({", ".join(hit["args"]) or "no args shown"})')
            else:
                bits.append(f'+{site - src:X} {kind}')
        s = r['summary']
        lines.append(f'{head}: size {r["size"]}, reads {len(s["reads"])} / writes {len(s["writes"])} fields, '
                     f'{sum(s["calls"].values())} calls; ' + '; '.join(bits) + (' ...' if len(sites) > 3 else ''))
    return lines


def run(con, addr, args):
    """text for `q decomp`. args: fields, callers (N or 0), no_cache, json, raw_names."""
    r = decompile(con, addr, use_cache=not getattr(args, 'no_cache', False))
    if getattr(args, 'json', False):
        return json.dumps(r, indent=1)
    dbi = {} if getattr(args, 'raw_names', False) else db_index()
    fm = qfactcard.mode_of(args)     # S111 FACTCARD: the settled claims print in this output (header + `; [C]` comments), --facts none|all
    body = None if getattr(args, 'fields', False) else [db_overlay({a: e for a, e in dbi.items()}, l) for l in r['lines']]
    hdr, body = qfactcard.apply(body, qfactcard.collect(addr, con, r['size'], 'decomp', fm), fm) if fm != 'none' else ([], body)
    out = head_lines(r, dbi) + hdr + [_names_overlay(dbi, l) for l in summary_lines(r['summary'])]
    if not getattr(args, 'fields', False):
        out.append('')
        out += body
    if getattr(args, 'callers', 0):
        out.append('')
        out += callers_summary(con, addr, args.callers, dbi)
    return '\n'.join(out)


def fn_summary(con, addr):
    """The two summary lines `q fn` adds (fields read / written + a pointer); cached decomp only, never builds."""
    f = con.execute('SELECT * FROM funcs WHERE addr = ?', (addr,)).fetchone()
    if f is None or not f['file']:
        return []
    path = os.path.join(GEN, f['file'])
    try:
        sha = _cached_sha1(path)
    except OSError:
        return []
    cpath = _cache_path(addr, sha)
    if not os.path.exists(cpath):
        return [f'  decomp: py tools\\q\\q.py decomp {f["name"]}   (not built yet; builds in a moment)']
    with open(cpath, encoding='utf-8') as fh:
        r = json.load(fh)
    s = r['summary']
    return [f'  decomp: {len(s["reads"])} fields read, {len(s["writes"])} written, {sum(s["calls"].values())} calls; '
            f'q decomp {f["name"]}']


def main(argv, con, resolve):
    import argparse
    ap = argparse.ArgumentParser(prog='q decomp', description=__doc__.split('\n\n')[0])
    ap.add_argument('func')
    ap.add_argument('--callers', type=int, default=0, metavar='N', help='after the body: one line per caller (N of them) with its call-site args; --fields --callers N = no body')
    ap.add_argument('--fields', action='store_true', help='only the header, fields read / written, calls, constants')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--no-cache', action='store_true', help='rebuild this function ignoring the cache')
    ap.add_argument('--raw-names', action='store_true', help='no db overlay (names / labels)')
    qfactcard.add_arg(ap)
    a = ap.parse_args(argv)
    addr, note = resolve(con, a.func)
    if note:
        print(f'({note})')
    print(run(con, addr, a))
    return 0
