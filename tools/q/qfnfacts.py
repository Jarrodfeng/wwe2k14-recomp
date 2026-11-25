r"""q fnfacts: per-function STRUCTURAL facts read from the generated listing in one pass (S110 lane IDXFACTS).

    py tools/q/q.py fnfacts FUNC                 every fact row of one function
    py tools/q/q.py fnfacts --stats              per fact: rows, functions marked, breakdowns, the clone groups, the parse check
    py tools/q/q.py fnfacts --build [--out FILE] [--gen DIR] [--jobs N]
                                                 scan generated/default/*_recomp.*.cpp and write out/harness/index/fnfacts.s1.db
                                                 (WWE_QFNFACTS_DB=FILE = another file; put in place with qpublish.publish)

Every fact is a SHAPE of the listing, never a meaning (docs/research/s110/IDXFACTS.md). One must-dataflow per function (entry args r3..r10,
`mr` copies, constants from li / lis / ori / addi, a call kills r0 r3-r12 and defines r3; the save / restore helpers kill r0 r11 r12) gives
every register a VALUE id at every instruction; paths that disagree drop it. The tables:
  loop(func, head, latch, n_body, n_calls, n_icalls)       a back edge of a depth-first walk from the entry (the head is still on the path
                                                           when the latch branches to it: a cycle); the body is the natural loop, the head +
                                                           every instruction that reaches a latch WITHOUT passing the head (an inner loop never
                                                           takes in its outer loop's path); one row per head (several latches merge). A cycle
                                                           with a second way in (irreducible) keeps its row, with no inductions
  loop_ind(func, head, reg, kind, step, init, bound)       an induction of that loop: a register changed in the body ONLY by one `addi rX,rX,K` /
                                                           `subi` / update form; ptr = an address operand in the body (a K(rX) base, or either
                                                           register of an indexed load / store, so a byte offset too) | arg = not an address
                                                           operand but copied into r3..r6 in the body | idx = neither. link (rX = lwz K(rX), its
                                                           only write) | ctr (a bdnz latch; init = the constant moved to CTR). init = the constant
                                                           the register holds on entry to the loop on every outside path; bound = the constant of
                                                           the last compare of it whose branch can LEAVE the loop (an exit test; an in-body range
                                                           test is not a bound)
  switch(func, site, reg, n_cases, n_targets, lo, hi, bias, deflt)   a jump-table switch: the recompiler's `switch (` block under a `bctr`
  switch_case(func, site, idx, value, target)              (tools/q/qgx.parse_switches: index -> target). value = idx + bias, where bias undoes
                                                           the add chain (addi / addic / subi / addis, through `mr`) since the index register's
                                                           last other write, the same on every path into the bctr that does not come around a
                                                           loop (NULL bias and values when they add different constants); values and bias are
                                                           signed 32-bit (an HRESULT case is negative); deflt = the target of the bgt / bge after
                                                           the bound compare of the index register (NULL: none, or a conditional return)
  chain(func, val, reg, first, last, n, n_eq)              >= 3 compares of ONE VALUE (val: arg:rN | ret:SITE | def:SITE | bits:rN&MASK) against
  chain_val(func, first, site, value, cond)                distinct constants (an immediate, or a constant in the other register); cond = eq
                                                           (a beq / bne reads it) | rng (only other branches) | none
  argbit(func, arg, mask, site, how)                       bits `mask` of the ENTRY value of argument r<arg> are tested: a rlwinm-family / andi.
                                                           extract of that value compared with 0 (how=cmp) or in record form (how=rec)
  fn(func, n_insns, n_loops, n_switch, n_chain, n_argbit, h_exact, h_noimm, h_nocall, h_shape)
                                                           h_exact: the instruction text, in-function branch targets made relative; h_noimm: +
                                                           every immediate / displacement -> #; h_nocall: + every call / tail target -> F
                                                           (immediates kept); h_shape: both. Equal hash = a clone group (GROUP BY).
Not here (held already): the slot offset of an indirect call = vcall.s4.db `icall(site, func, kind, shape, off, ...)` (tools/q/qx7.py).
Selftest: py -I tools\q\qfnfacts_selftest.py
"""
import argparse
import collections
import glob
import hashlib
import multiprocessing
import os
import re
import sqlite3
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qgx  # noqa: E402  (parse_switches: the one switch reader q already uses)
import qpublish  # noqa: E402

SCHEMA = 1
MAIN = r'C:\Users\Jarrod\wwe2k14-recomp'
IDX = os.path.join(MAIN, 'out', 'harness', 'index')
GEN = os.path.join(MAIN, 'generated', 'default')
M32 = 0xFFFFFFFF
DEF = 'DEFINE_REX_FUNC('
MAP_RE = re.compile(r'^\s*\{ 0x([0-9A-Fa-f]+), (\w+) \},?\s*$', re.M)
INS_RE = re.compile(r'^\t// ([^ ]+) ?(.*?)\s*$')
LABEL_RE = re.compile(r'^loc_([0-9A-F]{8}):')
NUM_RE = re.compile(r'^-?(?:0x[0-9a-fA-F]+|\d+)$')
MEM_RE = re.compile(r'^(-?(?:0x[0-9a-fA-F]+|\d+))\((r\d+)\)$')
UPD_RE = re.compile(r'^(?:lbz|lhz|lha|lwz|lwa|ld|lfs|lfd|stb|sth|stw|std|stfs|stfd)u(x?)$')
COND = {'beq', 'bne', 'blt', 'bgt', 'ble', 'bge', 'bnl', 'bng', 'bso', 'bns', 'bun', 'bnu', 'bdnz', 'bdz',
        'bdnzt', 'bdnzf', 'bdzt', 'bdzf', 'bt', 'bf', 'bc'}
CTR_ONLY = ('bdnz', 'bdz')
NO_DEST = ('st', 'cmp', 'b', 'mt', 'tw', 'td', 'dcb', 'icb', 'sync', 'lwsync', 'eieio', 'fcmp', 'vcmp', 'nop', 'db16cyc', 'cctp')
VOLATILE = frozenset(['r0'] + ['r%d' % i for i in range(3, 13)])
HELPER_KILL = frozenset(['r0', 'r11', 'r12'])          # __savegprlr_N / __restgprlr_N / fpr / vmx helpers
ARGS = ('r3', 'r4', 'r5', 'r6')
CMPI = ('cmpwi', 'cmplwi', 'cmpdi', 'cmpldi')
CMPR = ('cmpw', 'cmplw', 'cmpd', 'cmpld')


class FnFactsError(Exception):
    pass


def db_path():
    return os.environ.get('WWE_QFNFACTS_DB') or os.path.join(IDX, 'fnfacts.s%d.db' % SCHEMA)


def signature(gen):
    parts = ['fnfacts%d' % SCHEMA]
    for p in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        st = os.stat(p)
        parts.append('%s:%d:%d' % (os.path.basename(p), st.st_size, int(st.st_mtime)))
    return ';'.join(parts)


# ---------------------------------------------------------------------------------------------------- one instruction
def num(s):
    s = s.strip()
    if not NUM_RE.match(s):
        return None
    return int(s, 0)


def s32(v):
    v &= M32
    return v - (1 << 32) if v >= 0x80000000 else v


def split_ops(ops):
    return [o.strip() for o in ops.split(',')] if ops else []


def base_mn(mn):
    return mn.rstrip('+-')


def is_gpr(t):
    return len(t) > 1 and t[0] == 'r' and t[1:].isdigit()


def mask_be(mb, me):
    """rlwinm mask: IBM bits mb..me (0 = most significant), wrapping when mb > me"""
    m = 0
    b = mb
    while True:
        m |= 1 << (31 - b)
        if b == me:
            break
        b = (b + 1) & 31
    return m


def rotr(v, n):
    n &= 31
    return ((v >> n) | (v << (32 - n))) & M32 if n else v & M32


def bit_extract(mn, ops):
    """(dest, src, SOURCE-bit mask, record form) for a bit extract, else None"""
    rec = mn.endswith('.')
    o = split_ops(ops)
    b = mn.rstrip('.')
    try:
        if b == 'andi' and len(o) == 3:
            return o[0], o[1], num(o[2]) & 0xFFFF, True
        if b == 'andis' and len(o) == 3:
            return o[0], o[1], (num(o[2]) & 0xFFFF) << 16, True
        if b == 'rlwinm' and len(o) == 5:
            sh, mb, me = num(o[2]), num(o[3]), num(o[4])
            return o[0], o[1], rotr(mask_be(mb, me), sh), rec
        if b == 'clrlwi' and len(o) == 3:
            n = num(o[2])
            return o[0], o[1], ((1 << (32 - n)) - 1) if n else M32, rec
        if b == 'extrwi' and len(o) == 4:
            n, s = num(o[2]), num(o[3])
            return o[0], o[1], rotr(mask_be(32 - n, 31), (s + n) & 31), rec
        if b == 'srwi' and len(o) == 3:
            n = num(o[2])
            return o[0], o[1], rotr(mask_be(n, 31), 32 - n), rec
        if b == 'slwi' and len(o) == 3:
            n = num(o[2])
            return o[0], o[1], rotr(mask_be(0, 31 - n), n), rec
    except (TypeError, ValueError):
        return None
    return None


def writes(mn, ops):
    """the GPRs an instruction writes (calls are handled by the caller)"""
    o = split_ops(ops)
    m = UPD_RE.match(mn)
    out = set()
    if m:
        if m.group(1):                       # indexed update: rA = ops[1]
            if len(o) > 1 and is_gpr(o[1]):
                out.add(o[1])
        elif len(o) > 1:
            mm = MEM_RE.match(o[1])
            if mm:
                out.add(mm.group(2))
        if mn.startswith('l') and o and is_gpr(o[0]):
            out.add(o[0])
        return out
    if mn.startswith(NO_DEST):
        return out
    if mn == 'lmw' and o and is_gpr(o[0]):
        return set('r%d' % i for i in range(int(o[0][1:]), 32))
    if o and is_gpr(o[0]):
        out.add(o[0])
    return out


def branch(mn, ops):
    """-> (kind, target): kind call | icall | ret | ijump | jump | cond | ccall | cret | None"""
    b = base_mn(mn)
    if not b.startswith('b'):
        return None, None
    o = split_ops(ops)
    t = num(o[-1]) if o and o[-1].startswith('0x') else None
    if b == 'b':
        return 'jump', t
    if b == 'bl':
        return 'call', t
    if b == 'blr':
        return 'ret', None
    if b == 'bctr':
        return 'ijump', None
    if b in ('bctrl', 'blrl'):
        return 'icall', None
    if b.endswith('lrl') or b.endswith('ctrl'):
        return 'ccall', None
    if b.endswith('lr') or b.endswith('ctr'):
        return 'cret', None
    if b in COND:
        return 'cond', t
    if b.endswith('l') and b[:-1] in COND:
        return 'ccall', t
    return None, None


def cr_of_cmp(o):
    """compare operands -> (cr, rest)"""
    if o and o[0].startswith('cr'):
        return o[0], o[1:]
    return 'cr0', o


def cr_read(mn, ops):
    """the cr field a conditional branch reads (None: not a branch on a cr field)"""
    b = base_mn(mn)
    for suf in ('lrl', 'ctrl', 'lr', 'ctr', 'l', ''):
        if suf and not b.endswith(suf):
            continue
        core = b[:len(b) - len(suf)] if suf else b
        if core in COND:
            if core in CTR_ONLY:
                return None
            o = split_ops(ops)
            return o[0] if o and o[0].startswith('cr') else 'cr0'
    return None


# ---------------------------------------------------------------------------------------------------- one function
class Fn:
    def __init__(self, addr, lines):
        self.addr = addr
        self.lines = lines
        self.ins = []                 # [(addr, mn, ops)]
        self.line_addr = {}           # line index -> address of the last instruction at or before it
        self.labels = set()
        a = addr
        last = None
        for i, l in enumerate(lines):
            m = INS_RE.match(l)
            if m:
                self.ins.append((a, m.group(1), m.group(2)))
                last = a
                a += 4
            else:
                lm = LABEL_RE.match(l)
                if lm:
                    a = int(lm.group(1), 16)
                    self.labels.add(a)
            self.line_addr[i] = last
        self.at = {x[0]: k for k, x in enumerate(self.ins)}


def switches(fn):
    """the recompiler's `switch (` blocks at their bctr: the index register and the index -> target table (qgx.parse_switches). The bias and
    the default are NOT taken from qgx (its line windows miss a bias 3+ instructions back, an addis, a default 2+ instructions after the
    bound compare, and read an addi before a call as a bias): switch_bias / switch_default below read them from this function's CFG"""
    sws, _labels = qgx.parse_switches(fn.lines)
    out = []
    for sw in sws:
        site = fn.line_addr.get(sw['line'] - 1) if sw['line'] else None
        if site is None:
            continue
        out.append({'site': site, 'reg': ('r%d' % sw['reg']) if sw['reg'] is not None else None, 'idx': list(sw['cases'])})
    return out


ADDS = ('addi', 'addic', 'addic.', 'subi', 'addis')


def switch_bias(fn, succ, back, helpers, site, reg):
    """case value = table index + bias. Walks back from the bctr along every path that does not come around a loop (no back edge: a loop
    counter switched on, `addi r29,r29,1` at the latch, is the switched value, not a bias): through `mr` copies and adds of the index
    register to itself (addi / addic / subi / addis, summed) to its last other write (a load, a call, the entry), or to an add FROM
    another register (that register holds the switched value: `addi r11,r23,-7`). One sum on every such path = its bias; None when they
    add different constants."""
    back = set(back)
    k = fn.at.get(site)
    if k is None or not reg:
        return None
    live = reach(0, succ, len(fn.ins))
    pred = [[] for _ in range(len(fn.ins))]
    for i, ss in enumerate(succ):
        if live[i]:
            for s in ss:
                pred[s].append(i)
    seen = {}
    ends = set()
    work = [(k, reg, 0)]                              # (instruction, the register that holds the index - delta before it, delta)
    while work:
        i, r, delta = work.pop()
        if i == 0:
            ends.add(-delta)                          # the entry value of r
        for p in pred[i]:
            if (p, i) in back:
                continue
            a, mn, ops = fn.ins[p]
            kind, t = branch(mn, ops)
            o = split_ops(ops)
            nr, nd = r, delta
            if kind in ('call', 'icall', 'ccall'):
                if r in (HELPER_KILL if (kind == 'call' and t in helpers) else VOLATILE):
                    ends.add(-delta)
                    continue
            elif r in writes(mn, ops):
                c = num(o[2]) if mn in ADDS and len(o) == 3 and o[0] == r else None
                if mn == 'mr' and len(o) == 2:
                    nr = o[1]
                elif c is not None:
                    nd = delta + (-c if mn == 'subi' else ((c << 16) if mn == 'addis' else c))
                    if o[1] != r:
                        ends.add(-nd)
                        continue
                else:
                    ends.add(-delta)
                    continue
            key = (p, nr)
            if key in seen:
                if seen[key] != nd:
                    return None
                continue
            seen[key] = nd
            work.append((p, nr, nd))
        if len(ends) > 1:
            return None
    return ends.pop() if len(ends) == 1 else None


def remade(fn, j, rewrites, reg):
    """True when the writes of `reg` between its compare at j and the bctr (`rewrites`) make ANOTHER value: they must repeat, text for
    text, the write of `reg` just before the compare (the compiler recomputing `addi r11,r30,-39` after the bound test), with its source
    registers (reg not among them, no load) unchanged in between"""
    if not rewrites:
        return False
    prev = next((p for p in range(j - 1, max(-1, j - 40), -1) if reg in writes(fn.ins[p][1], fn.ins[p][2])), None)
    if prev is None or branch(fn.ins[prev][1], fn.ins[prev][2])[0] is not None:
        return True
    mn, ops = fn.ins[prev][1], fn.ins[prev][2]
    srcs = set(r for r in split_ops(ops)[1:] if is_gpr(r))
    if reg in srcs or mn.startswith('l') or any(fn.ins[w][1:] != (mn, ops) for w in rewrites):
        return True
    return any(srcs & writes(fn.ins[q][1], fn.ins[q][2]) for q in range(prev + 1, max(rewrites)) if q not in rewrites)


def switch_default(fn, site, reg):
    """-> (found, target): the nearest compare of the index register before the bctr that the register still holds at the bctr (see
    remade: a compare before the register is made anew is no bound test), then the bgt / bge / bnl that reads its cr field (target None:
    a conditional return, `bgtlr`)"""
    k = fn.at.get(site)
    if k is None or not reg:
        return False, None
    rewrites = []
    for j in range(k - 1, max(-1, k - 40), -1):
        mn, ops = fn.ins[j][1], fn.ins[j][2]
        if mn not in ('cmplwi', 'cmpwi'):
            kind = branch(mn, ops)[0]
            if reg in writes(mn, ops) or (kind in ('call', 'icall', 'ccall') and reg in VOLATILE):
                rewrites.append(j)
            continue
        cr, rest = cr_of_cmp(split_ops(ops))
        if len(rest) != 2 or rest[0] != reg:
            continue
        if remade(fn, j, rewrites, reg):
            return False, None
        for q in range(j + 1, k):
            mq, oq = fn.ins[q][1], fn.ins[q][2]
            if cr_read(mq, oq) == cr and base_mn(mq).startswith(('bgt', 'bge', 'bnl')):
                kind, t = branch(mq, oq)
                return True, (t if kind == 'cond' else None)
        return False, None
    return False, None


def back_edges(succ):
    """the back edges of a depth-first walk from the entry: (latch, head) where the head is still on the walk's path (listing order plays
    no part, so a loop entered at its bottom test gets that test as its head)"""
    n = len(succ)
    back = []
    if not n:
        return back
    state = [0] * n                  # 0 unseen, 1 on the path, 2 done
    state[0] = 1
    stack = [(0, 0)]
    while stack:
        v, k = stack[-1]
        if k < len(succ[v]):
            stack[-1] = (v, k + 1)
            w = succ[v][k]
            if state[w] == 0:
                state[w] = 1
                stack.append((w, 0))
            elif state[w] == 1:
                back.append((v, w))
        else:
            state[v] = 2
            stack.pop()
    return back


def cfg(fn, sw_targets):
    """instruction-level successors -> (succ list per index, back edges [(latch, head)] of a depth-first walk)"""
    n = len(fn.ins)
    succ = [[] for _ in range(n)]
    for i, (a, mn, ops) in enumerate(fn.ins):
        kind, t = branch(mn, ops)
        nxt = [i + 1] if i + 1 < n else []
        ti = fn.at.get(t) if t is not None else None
        if kind == 'jump':
            succ[i] = [ti] if ti is not None else []
        elif kind == 'ret':
            succ[i] = []
        elif kind == 'ijump':
            succ[i] = sorted(set(fn.at[x] for x in sw_targets.get(a, ()) if x in fn.at))
        elif kind == 'cond':
            succ[i] = nxt + ([ti] if ti is not None and ti != i + 1 else [])
        else:
            succ[i] = nxt
    return succ, back_edges(succ)


def reach(start, edges, n):
    seen = [False] * n
    st = [start]
    seen[start] = True
    while st:
        x = st.pop()
        for y in edges[x]:
            if not seen[y]:
                seen[y] = True
                st.append(y)
    return seen


# ---------------------------------------------------------------------------------------------------- value flow
def step(fn, i, st, helpers):
    """the register VALUES after instruction i (st = the values before it; changed in place and returned).
    Values: ('a', n) entry arg rN | ('k', v) constant | ('r', site) a call's r3 | ('d', site, reg) any other def | ('m', n, mask, site) bits of arg rN"""
    a, mn, ops = fn.ins[i]
    o = split_ops(ops)
    kind, t = branch(mn, ops)
    if kind in ('call', 'icall', 'ccall'):
        helper = kind == 'call' and t in helpers
        for r in (HELPER_KILL if helper else VOLATILE):
            st.pop(r, None)
        if kind in ('call', 'icall') and not helper:
            st['r3'] = ('r', a)
        return st
    if kind is not None:
        return st
    if mn == 'mr' and len(o) == 2:
        v = st.get(o[1])
        if v is None:
            st.pop(o[0], None)
        else:
            st[o[0]] = v
        return st
    if mn == 'li' and len(o) == 2 and num(o[1]) is not None:
        st[o[0]] = ('k', num(o[1]) & M32)
        return st
    if mn == 'lis' and len(o) == 2 and num(o[1]) is not None:
        st[o[0]] = ('k', (num(o[1]) << 16) & M32)
        return st
    if mn in ('ori', 'addi') and len(o) == 3 and num(o[2]) is not None:
        v = st.get(o[1])
        if v and v[0] == 'k':
            st[o[0]] = ('k', (v[1] | (num(o[2]) & 0xFFFF)) & M32 if mn == 'ori' else (v[1] + num(o[2])) & M32)
            return st
    bx = bit_extract(mn, ops)
    if bx:
        d, s, mask, _rec = bx
        v = st.get(s)
        st[d] = ('m', v[1], mask, a) if v and v[0] == 'a' else ('d', a, d)
        return st
    for r in writes(mn, ops):
        st[r] = ('d', a, r)
    return st


FLOW_LIMIT = 400000


def flow(fn, succ, helpers):
    """-> the values BEFORE each instruction (None: unreachable; None everywhere when the walk did not settle within FLOW_LIMIT steps).
    Must-analysis: the state before an instruction is the meet of its predecessors' CURRENT states after them, recomputed whenever one
    changes, so a register stays only when every path agrees. (Meeting with the instruction's own OLD state instead lost a value for
    good when a path's value changed kind, e.g. a constant folded by `addi` on one path that later turned into the addi's own def.)"""
    n = len(fn.ins)
    inst = [None] * n
    if not n:
        return inst
    pred = [[] for _ in range(n)]
    for i, ss in enumerate(succ):
        for s in ss:
            pred[s].append(i)
    outs = [None] * n
    entry = {'r%d' % k: ('a', k) for k in range(3, 11)}
    work = [0]
    inwork = {0}
    guard = 0
    while work:
        guard += 1
        if guard > FLOW_LIMIT:
            return [None] * n
        i = work.pop()
        inwork.discard(i)
        cur = dict(entry) if i == 0 else None
        for p in pred[i]:
            o = outs[p]
            if o is not None:
                cur = dict(o) if cur is None else {k: v for k, v in cur.items() if o.get(k) == v}
        if cur is None:
            continue
        inst[i] = cur
        out = step(fn, i, dict(cur), helpers)
        if out != outs[i]:
            outs[i] = out
            for s in succ[i]:
                if s not in inwork:
                    work.append(s)
                    inwork.add(s)
    return inst


def vtext(v):
    if v[0] == 'a':
        return 'arg:r%d' % v[1]
    if v[0] == 'r':
        return 'ret:%08X' % v[1]
    if v[0] == 'm':
        return 'bits:r%d&0x%X' % (v[1], v[2])
    return 'def:%08X' % v[1]


def cond_of(fn, i, cr):
    """eq when a beq / bne reads `cr` before it is written again, rng when only other branches do, none when nothing does"""
    seen = set()
    for j in range(i + 1, min(len(fn.ins), i + 9)):
        mj, oj = fn.ins[j][1], fn.ins[j][2]
        c = cr_read(mj, oj)
        if c == cr:
            seen.add('eq' if base_mn(mj).startswith(('beq', 'bne')) else 'rng')
        if (mj in CMPI or mj in CMPR) and cr_of_cmp(split_ops(oj))[0] == cr:
            break
        if cr == 'cr0' and mj.endswith('.'):
            break
        if branch(mj, oj)[0] in ('jump', 'ret', 'ijump'):
            break
    return 'eq' if 'eq' in seen else ('rng' if seen else 'none')


def compares(fn, inst, helpers):
    """-> (chain rows, chain_val rows, argbit rows)"""
    by_val = collections.defaultdict(list)
    bits = {}
    for i, (a, mn, ops) in enumerate(fn.ins):
        st = inst[i]
        if st is None:
            continue
        if mn in CMPI or mn in CMPR:
            cr, rest = cr_of_cmp(split_ops(ops))
            if len(rest) != 2 or not is_gpr(rest[0]):
                continue
            v = st.get(rest[0])
            if v is None or v[0] == 'k':
                continue
            if mn in CMPI:
                c = num(rest[1])
            else:
                k = st.get(rest[1])
                c = k[1] if k and k[0] == 'k' else None
            if c is None:
                continue
            if v[0] == 'm' and c == 0 and mn in ('cmpwi', 'cmplwi'):
                bits.setdefault(v[3], (fn.addr, v[1], v[2], v[3], 'cmp'))
            by_val[v].append((a, c, cond_of(fn, i, cr), rest[0]))
            continue
        if mn.endswith('.'):
            bx = bit_extract(mn, ops)
            if bx and bx[3]:
                v = st.get(bx[1])
                if v and v[0] == 'a':
                    bits.setdefault(a, (fn.addr, v[1], bx[2], a, 'rec'))
    rows, vals = [], []
    for v, cs in by_val.items():
        if len(set(c for _a, c, _d, _r in cs)) < 3:
            continue
        cs.sort()
        first = cs[0][0]
        rows.append((fn.addr, vtext(v), cs[0][3], first, cs[-1][0], len(cs), sum(1 for x in cs if x[2] == 'eq')))
        vals.extend((fn.addr, first, a, c, d) for a, c, d, _r in cs)
    rows.sort(key=lambda r: r[3])
    return rows, vals, sorted(bits.values(), key=lambda r: r[3])


# ---------------------------------------------------------------------------------------------------- loops
def is_indexed(mn):
    """an indexed load / store (`lwzx`, `stbx`, `lvx128`, `stvewx128`, ...): both GPR operands make the address"""
    b = mn[:-3] if mn.endswith('128') else mn
    return b.startswith(('l', 'st')) and b.endswith('x')


def exit_test(fn, k, cr, body, succ):
    """True when the compare at index k (cr field `cr`) is read by a branch that can leave the loop `body`"""
    for j in range(k + 1, min(len(fn.ins), k + 9)):
        a, mj, oj = fn.ins[j]
        if a in fn.labels:
            return False                              # another path joins between the compare and its branch
        if cr_read(mj, oj) == cr:
            kind, t = branch(mj, oj)
            return kind == 'cret' or (kind == 'cond' and t not in fn.at) or any(not body[s] for s in succ[j])
        if (mj in CMPI or mj in CMPR) and cr_of_cmp(split_ops(oj))[0] == cr:
            return False
        if (cr == 'cr0' and mj.endswith('.')) or branch(mj, oj)[0] is not None:
            return False
    return False


def loops(fn, succ, back, helpers, inst):
    n = len(fn.ins)
    if not back:
        return [], []
    live = reach(0, succ, n)
    pred = [[] for _ in range(n)]
    for i, ss in enumerate(succ):
        if live[i]:
            for s in ss:
                pred[s].append(i)
    heads = collections.defaultdict(list)
    for i, t in back:
        heads[t].append(i)
    rows, inds = [], []
    for h in sorted(heads):
        latches = heads[h]
        body = [False] * n                            # the natural loop: the head + every instruction reaching a latch without passing the head
        body[h] = True
        st = [la for la in set(latches) if la != h]
        for la in st:
            body[la] = True
        while st:
            x = st.pop()
            for p in pred[x]:
                if not body[p]:
                    body[p] = True
                    st.append(p)
        irreducible = h != 0 and body[0]              # the entry reaches a latch past the head: a second way into the cycle
        if irreducible:
            fwd = reach(h, succ, n)
            body = [b and f for b, f in zip(body, fwd)]
        idx = [k for k in range(n) if body[k]]
        entry_st = None                               # the values on entry to the loop, met over every outside predecessor
        for p in pred[h]:
            if not body[p] and inst[p] is not None:
                out = step(fn, p, dict(inst[p]), helpers)
                entry_st = out if entry_st is None else {k: v for k, v in entry_st.items() if out.get(k) == v}
        if h == 0:
            entry_st = None
        entry_st = entry_st or {}
        ncall = nicall = 0
        wr = collections.defaultdict(list)
        used_base, passed = set(), set()
        cmpb = {}
        for k in idx:
            a, mn, ops = fn.ins[k]
            kind, t = branch(mn, ops)
            o = split_ops(ops)
            if kind in ('call', 'icall', 'ccall'):
                if kind == 'call' and t not in helpers:
                    ncall += 1
                elif kind == 'icall':
                    nicall += 1
                for r in (HELPER_KILL if (kind == 'call' and t in helpers) else VOLATILE):
                    wr[r].append(('other', None))
                continue
            for x in o:
                mm = MEM_RE.match(x)
                if mm:
                    used_base.add(mm.group(2))
            if is_indexed(mn) and len(o) >= 3:
                used_base.update(r for r in o[1:3] if is_gpr(r))
            if mn == 'mr' and len(o) == 2 and o[0] in ARGS:
                passed.add(o[1])
            if mn == 'addi' and len(o) == 3 and o[0] in ARGS and o[0] != o[1]:
                passed.add(o[1])
            if mn in CMPI:
                cr, rest = cr_of_cmp(o)
                if len(rest) == 2 and num(rest[1]) is not None and exit_test(fn, k, cr, body, succ):
                    cmpb[rest[0]] = num(rest[1])
            upd = UPD_RE.match(mn)
            mm1 = MEM_RE.match(o[1]) if len(o) > 1 else None
            for r in writes(mn, ops):
                if r == 'r1':
                    continue
                if upd and not upd.group(1) and mm1 and mm1.group(2) == r and o[0] != r:
                    wr[r].append(('inc', num(mm1.group(1))))
                elif mn in ('addi', 'addic', 'addic.') and len(o) == 3 and o[0] == o[1] == r and num(o[2]) is not None:
                    wr[r].append(('inc', num(o[2])))
                elif mn == 'subi' and len(o) == 3 and o[0] == o[1] == r and num(o[2]) is not None:
                    wr[r].append(('inc', -num(o[2])))
                elif mn == 'lwz' and mm1 and mm1.group(2) == r and o[0] == r:
                    wr[r].append(('link', num(mm1.group(1))))
                else:
                    wr[r].append(('other', None))
        hd = fn.ins[h][0]
        for r in sorted(wr):
            w = wr[r]
            if irreducible or len(w) != 1:
                continue
            k, v = w[0]
            e = entry_st.get(r)
            init = s32(e[1]) if e and e[0] == 'k' else None
            if k == 'inc' and v:
                kind = 'ptr' if r in used_base else ('arg' if r in passed else 'idx')
                inds.append((fn.addr, hd, r, kind, v, init, cmpb.get(r)))
            elif k == 'link':
                inds.append((fn.addr, hd, r, 'link', v, None, None))
        la_last = max(latches)
        if not irreducible and base_mn(fn.ins[la_last][1]).startswith(CTR_ONLY):
            cnt = None
            for k in range(h - 1, max(-1, h - 16), -1):
                a, mn, ops = fn.ins[k]
                if mn == 'mtctr':
                    o = split_ops(ops)
                    v = inst[k].get(o[0]) if inst[k] is not None and o else None
                    cnt = s32(v[1]) if v and v[0] == 'k' else None
                    break
                if a in fn.labels or branch(mn, ops)[0] is not None:
                    break
            inds.append((fn.addr, hd, 'ctr', 'ctr', -1, cnt, None))
        rows.append((fn.addr, hd, fn.ins[la_last][0], len(idx), ncall, nicall))
    return rows, inds


# ---------------------------------------------------------------------------------------------------- clones
def hashes(fn, sws=()):
    hs = [hashlib.blake2b(digest_size=8) for _ in range(4)]
    maps = {s['site']: ' {%s}' % ' '.join('%d:L%+d' % (i, t - fn.addr) for i, t in s['idx']) for s in sws}   # a bctr's case map is control flow
    for a, mn, ops in fn.ins:
        kind, t = branch(mn, ops)
        o = split_ops(ops)
        e, ni, nc, sh = [], [], [], []
        for k, x in enumerate(o):
            if k == len(o) - 1 and kind and t is not None:
                if t in fn.at:
                    rel = 'L%+d' % (t - fn.addr)
                    e.append(rel); ni.append(rel); nc.append(rel); sh.append(rel)
                else:
                    e.append(x); ni.append(x); nc.append('F'); sh.append('F')
                continue
            mm = MEM_RE.match(x)
            if mm:
                e.append(x); nc.append(x)
                ni.append('#(%s)' % mm.group(2)); sh.append('#(%s)' % mm.group(2))
            elif num(x) is not None:
                e.append(x); nc.append(x); ni.append('#'); sh.append('#')
            else:
                e.append(x); ni.append(x); nc.append(x); sh.append(x)
        for h, parts in zip(hs, (e, ni, nc, sh)):
            h.update(('%s %s%s\n' % (mn, ','.join(parts), maps.get(a, ''))).encode())
    return [int.from_bytes(h.digest(), 'big') & ((1 << 63) - 1) for h in hs]


def analyse(addr, lines, helpers):
    fn = Fn(addr, lines)
    sws = switches(fn)
    succ, back = cfg(fn, {s['site']: set(t for _i, t in s['idx']) for s in sws})
    inst = flow(fn, succ, helpers)
    lrows, linds = loops(fn, succ, back, helpers, inst)
    crows, cvals, arows = compares(fn, inst, helpers)
    srows, scase = [], []
    for s in sws:
        bias = switch_bias(fn, succ, back, helpers, s['site'], s['reg'])
        found, deflt = switch_default(fn, s['site'], s['reg'])
        bias = s32(bias) if bias is not None else None
        vs = [s32(i + bias) for i, _t in s['idx']] if bias is not None else []
        srows.append((addr, s['site'], s['reg'], len(s['idx']), len(set(t for _i, t in s['idx'])),
                      min(vs) if vs else None, max(vs) if vs else None, bias, deflt if found else None))
        scase.extend((addr, s['site'], i, s32(i + bias) if bias is not None else None, t) for i, t in s['idx'])
    h = hashes(fn, sws)
    frow = (addr, len(fn.ins), len(lrows), len(srows), len(crows), len(arows), h[0], h[1], h[2], h[3])
    return frow, lrows, linds, srows, scase, crows, cvals, arows


# ---------------------------------------------------------------------------------------------------- files
def load_mapping(gen):
    out = {}
    for p in glob.glob(os.path.join(gen, '*_init.cpp')):
        with open(p, 'rb') as f:
            for m in MAP_RE.finditer(f.read().decode('utf-8', errors='replace')):
                out[m.group(2)] = int(m.group(1), 16)
    return out


def helper_addrs(mapping):
    return frozenset(a for n, a in mapping.items() if n.startswith(('__savegpr', '__restgpr', '__savefpr', '__restfpr', '__savevmx', '__restvmx')))


def scan_text(text, mapping, helpers):
    """one listing file's text -> ([rows per table], unnamed bodies skipped)"""
    out = [[] for _ in range(8)]
    skipped = 0
    pos = 0
    while True:
        i = text.find(DEF, pos)
        if i < 0:
            break
        j = text.find(')', i)
        name = text[i + len(DEF):j]
        end = text.find('\n}', j)
        end = len(text) if end < 0 else end + 2
        body = text[i:end]
        pos = end
        if re.fullmatch(r'sub_[0-9A-Fa-f]{8}', name):
            addr = int(name[4:], 16)
        elif name in mapping:
            addr = mapping[name]
        else:
            skipped += 1
            continue
        lines = body.replace('\r\n', '\n').split('\n')
        for k, part in enumerate(analyse(addr, lines, helpers)):
            if k == 0:
                out[0].append(part)
            else:
                out[k].extend(part)
    return out, skipped


def scan_file(args):
    path, mapping, helpers = args
    with open(path, 'rb') as f:
        return scan_text(f.read().decode('utf-8', errors='replace'), mapping, helpers)


SCHEMA_SQL = '''
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE fn(func INTEGER PRIMARY KEY, n_insns INTEGER, n_loops INTEGER, n_switch INTEGER, n_chain INTEGER, n_argbit INTEGER,
                h_exact INTEGER, h_noimm INTEGER, h_nocall INTEGER, h_shape INTEGER);
CREATE TABLE loop(func INTEGER, head INTEGER, latch INTEGER, n_body INTEGER, n_calls INTEGER, n_icalls INTEGER);
CREATE TABLE loop_ind(func INTEGER, head INTEGER, reg TEXT, kind TEXT, step INTEGER, init INTEGER, bound INTEGER);
CREATE TABLE switch(func INTEGER, site INTEGER, reg TEXT, n_cases INTEGER, n_targets INTEGER, lo INTEGER, hi INTEGER, bias INTEGER, deflt INTEGER);
CREATE TABLE switch_case(func INTEGER, site INTEGER, idx INTEGER, value INTEGER, target INTEGER);
CREATE TABLE chain(func INTEGER, val TEXT, reg TEXT, first INTEGER, last INTEGER, n INTEGER, n_eq INTEGER);
CREATE TABLE chain_val(func INTEGER, first INTEGER, site INTEGER, value INTEGER, cond TEXT);
CREATE TABLE argbit(func INTEGER, arg INTEGER, mask INTEGER, site INTEGER, how TEXT);
'''
INDEX_SQL = '''
CREATE INDEX loop_f ON loop(func); CREATE INDEX li_f ON loop_ind(func); CREATE INDEX li_k ON loop_ind(kind, step);
CREATE INDEX sw_f ON switch(func); CREATE INDEX sc_f ON switch_case(func, site); CREATE INDEX sc_v ON switch_case(value);
CREATE INDEX ch_f ON chain(func); CREATE INDEX cv_f ON chain_val(func, first); CREATE INDEX cv_v ON chain_val(value);
CREATE INDEX ab_f ON argbit(func); CREATE INDEX ab_k ON argbit(arg, mask);
CREATE INDEX fn_he ON fn(h_exact); CREATE INDEX fn_hi ON fn(h_noimm); CREATE INDEX fn_hc ON fn(h_nocall); CREATE INDEX fn_hs ON fn(h_shape);
'''
TABLES = ('fn', 'loop', 'loop_ind', 'switch', 'switch_case', 'chain', 'chain_val', 'argbit')


def _init_worker():
    try:
        import build_index
        build_index.lower_priority()
    except Exception:       # noqa: BLE001  (priority is a courtesy, never a reason to fail)
        pass


def write_db(out, results, sig, skipped, seconds):
    """results = iterable of [rows per table]; -> the file now holding the table (qpublish.publish)"""
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    tmp = '%s.%d.tmp' % (out, os.getpid())
    if os.path.exists(tmp):
        os.remove(tmp)
    con = sqlite3.connect(tmp)
    con.executescript('PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;' + SCHEMA_SQL)
    for res in results:
        for name, rows in zip(TABLES, res):
            if rows:
                con.executemany('INSERT INTO %s VALUES (%s)' % (name, ','.join('?' * len(rows[0]))), rows)
    con.executescript(INDEX_SQL)
    con.executemany('INSERT INTO meta VALUES (?,?)', [('sig', sig), ('built', time.strftime('%Y-%m-%d %H:%M')),
                                                     ('skipped', str(skipped[0])), ('seconds', '%.0f' % seconds())])
    con.commit()
    con.close()
    return qpublish.publish(tmp, out)


def build(gen=None, out=None, jobs=None, quiet=False):
    gen = gen or GEN
    out = out or db_path()
    files = sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp')))
    if not files:
        raise FnFactsError('no *_recomp.*.cpp in %s' % gen)
    mapping = load_mapping(gen)
    helpers = helper_addrs(mapping)
    t0 = time.time()
    skipped = [0]
    jobs = jobs or max(1, min(4, (os.cpu_count() or 4) // 4))
    work = [(p, mapping, helpers) for p in files]

    def results(it):
        for res, sk in it:
            skipped[0] += sk
            yield res
    if jobs > 1:
        with multiprocessing.Pool(jobs, initializer=_init_worker) as pool:
            where = write_db(out, results(pool.imap_unordered(scan_file, work)), signature(gen), skipped, lambda: time.time() - t0)
    else:
        where = write_db(out, results(scan_file(w) for w in work), signature(gen), skipped, lambda: time.time() - t0)
    if not quiet:
        print('q fnfacts: wrote %s (%d files, %.0f s; %d unnamed bodies skipped)' % (where, len(files), time.time() - t0, skipped[0]))
    return where


# ---------------------------------------------------------------------------------------------------- read
def connect(path=None):
    path = qpublish.newest(path or db_path())
    if not os.path.exists(path):
        raise FnFactsError('no %s: build it with  py tools\\q\\q.py fnfacts --build' % path)
    return sqlite3.connect('file:%s?mode=ro' % path.replace('\\', '/'), uri=True)


def stale_line(con, gen=None):
    r = con.execute("SELECT value FROM meta WHERE key='sig'").fetchone()
    try:
        cur = signature(gen or GEN)
    except OSError:
        return ''
    if not r or r[0] != cur:
        return '  (STALE: the listing changed since this table was built: py tools\\q\\q.py fnfacts --build)'
    return ''


def parse_func(s):
    m = re.match(r'^(?:sub_)?(?:0x)?([0-9A-Fa-f]{8})$', s.strip())
    if not m:
        raise FnFactsError('FUNC must be sub_XXXXXXXX / 0xXXXXXXXX (got %r)' % s)
    return int(m.group(1), 16)


def fourcc(v):
    v &= M32
    return v > 0xFFFF and all(0x20 <= (v >> s) & 0xFF < 0x7F for s in (24, 16, 8, 0))


def show(con, f):
    r = con.execute('SELECT * FROM fn WHERE func=?', (f,)).fetchone()
    if not r:
        print('sub_%08X: no row (not a function body of the listing)' % f)
        return 1
    print('sub_%08X: %d insns, %d loop(s), %d switch(es), %d compare chain(s), %d arg-bit test(s)' % (f, r[1], r[2], r[3], r[4], r[5]))
    for name, col in (('h_exact', 6), ('h_noimm', 7), ('h_nocall', 8), ('h_shape', 9)):
        n = con.execute('SELECT COUNT(*) FROM fn WHERE %s=?' % name, (r[col],)).fetchone()[0]
        if n > 1:
            sib = [x[0] for x in con.execute('SELECT func FROM fn WHERE %s=? AND func<>? ORDER BY func LIMIT 6' % name, (r[col], f))]
            print('  clone group %-8s %d functions: %s%s' % (name, n, ' '.join('sub_%08X' % x for x in sib), ' ...' if n > 7 else ''))
    for h, la, nb, nc, ni in con.execute('SELECT head,latch,n_body,n_calls,n_icalls FROM loop WHERE func=? ORDER BY head', (f,)):
        ind = con.execute('SELECT reg,kind,step,init,bound FROM loop_ind WHERE func=? AND head=? ORDER BY reg', (f, h)).fetchall()
        print('  loop @%08X..%08X  %d insns, %d call(s), %d indirect  %s' % (h, la, nb, nc, ni, ' '.join(
            '%s:%s%+d%s%s' % (rg, k, st, (' from %d' % i) if i is not None else '', (' cmp %d' % b) if b is not None else '')
            for rg, k, st, i, b in ind) or '(no induction)'))
    for site, reg, nca, nt, lo, hi, bias, de in con.execute('SELECT site,reg,n_cases,n_targets,lo,hi,bias,deflt FROM switch WHERE func=?', (f,)):
        print('  switch @%08X on %s: %d cases -> %d targets, values %s..%s (%s), default %s' % (
            site, reg, nca, nt, lo, hi, ('bias %+d' % bias) if bias is not None else 'bias unknown: the index register differs by path',
            ('@%08X' % de) if de else '-'))
    for val, reg, first, last, nn, neq in con.execute('SELECT val,reg,first,last,n,n_eq FROM chain WHERE func=? ORDER BY first', (f,)):
        vs = con.execute('SELECT value,cond FROM chain_val WHERE func=? AND first=? ORDER BY site', (f, first)).fetchall()
        print('  chain %s (%s) @%08X..%08X: %d compares (%d eq): %s' % (val, reg, first, last, nn, neq,
              ' '.join(('0x%X' % v if abs(v) > 0xFFFF else str(v)) + ('' if c == 'eq' else '(' + c + ')') for v, c in vs[:16])))
    for arg, mask, site, how in con.execute('SELECT arg,mask,site,how FROM argbit WHERE func=? ORDER BY site', (f,)):
        print('  argbit @%08X: r%d & 0x%X tested (%s)' % (site, arg, mask, how))
    return 0


def stats(con, index_db=None):
    def q(s, a=()):
        return con.execute(s, a).fetchone()[0]
    meta = dict(con.execute('SELECT key,value FROM meta'))
    nf = q('SELECT COUNT(*) FROM fn')
    print('q fnfacts: %d functions (built %s, %s s, %s unnamed bodies skipped)' % (nf, meta.get('built'), meta.get('seconds'), meta.get('skipped')))
    print('  loop       %7d rows, %6d functions' % (q('SELECT COUNT(*) FROM loop'), q('SELECT COUNT(DISTINCT func) FROM loop')))
    for k, n, nfn in con.execute('SELECT kind, COUNT(*), COUNT(DISTINCT func) FROM loop_ind GROUP BY kind ORDER BY kind'):
        print('    ind %-6s %7d rows, %6d functions' % (k, n, nfn))
    print('    ptr inductions with |step| >= 8: %d functions; of them with an indirect call in the loop: %d functions' % (
        q("SELECT COUNT(DISTINCT func) FROM loop_ind WHERE kind='ptr' AND abs(step)>=8"),
        q("SELECT COUNT(DISTINCT i.func) FROM loop_ind i JOIN loop l ON l.func=i.func AND l.head=i.head WHERE i.kind='ptr' AND abs(i.step)>=8 AND l.n_icalls>0")))
    print('  switch     %7d rows, %6d functions, %d case rows; a non-zero bias: %d, an unknown bias: %d' % (
        q('SELECT COUNT(*) FROM switch'), q('SELECT COUNT(DISTINCT func) FROM switch'), q('SELECT COUNT(*) FROM switch_case'),
        q('SELECT COUNT(*) FROM switch WHERE bias<>0'), q('SELECT COUNT(*) FROM switch WHERE bias IS NULL')))
    fc = set(f for f, v in con.execute('SELECT func, value FROM chain_val') if fourcc(v))
    print('  chain      %7d rows, %6d functions; >= 3 eq tests: %d functions; a printable 4-byte (FourCC) value: %d functions' % (
        q('SELECT COUNT(*) FROM chain'), q('SELECT COUNT(DISTINCT func) FROM chain'), q('SELECT COUNT(DISTINCT func) FROM chain WHERE n_eq>=3'), len(fc)))
    print('  argbit     %7d rows, %6d functions; r4 & 1: %d functions' % (q('SELECT COUNT(*) FROM argbit'), q('SELECT COUNT(DISTINCT func) FROM argbit'),
                                                                       q('SELECT COUNT(DISTINCT func) FROM argbit WHERE arg=4 AND mask=1')))
    for name in ('h_exact', 'h_noimm', 'h_nocall', 'h_shape'):
        for lo in (1, 8):
            g, m = con.execute('SELECT COUNT(*), COALESCE(SUM(c),0) FROM (SELECT COUNT(*) c FROM fn WHERE n_insns>=? GROUP BY %s HAVING c>1)' % name, (lo,)).fetchone()
            print('  clones %-8s n_insns >= %d: %6d groups, %6d functions' % (name, lo, g, m))
    if index_db and os.path.exists(qpublish.newest(index_db)):
        try:
            ix = sqlite3.connect('file:%s?mode=ro' % qpublish.newest(index_db).replace('\\', '/'), uri=True)
            ins = dict(ix.execute('SELECT addr, insns FROM funcs WHERE has_body=1'))
            mine = dict(con.execute('SELECT func, n_insns FROM fn'))
            miss = sum(1 for a in ins if a not in mine)
            diff = sum(1 for a, n in mine.items() if a in ins and ins[a] != n)
            print('  parse check against index.db: %d bodies there, %d not here, %d with another instruction count' % (len(ins), miss, diff))
        except sqlite3.Error as e:
            print('  parse check skipped (%s)' % e)
    return 0


def main(argv):
    ap = argparse.ArgumentParser(prog='q fnfacts')
    ap.add_argument('func', nargs='?')
    ap.add_argument('--build', action='store_true')
    ap.add_argument('--stats', action='store_true')
    ap.add_argument('--out', help='--build: the file to write (default out/harness/index/fnfacts.s1.db or WWE_QFNFACTS_DB)')
    ap.add_argument('--db', help='read this file instead')
    ap.add_argument('--gen', help='the generated listing folder (default generated/default of the main checkout)')
    ap.add_argument('--jobs', type=int)
    a = ap.parse_args(argv)
    try:
        if a.build:
            build(a.gen, a.out, a.jobs)
            return 0
        if not a.stats and not a.func:
            ap.print_help()
            return 2
        con = connect(a.db)
        if a.stats:
            stats(con, os.path.join(IDX, 'index.db'))
        else:
            show(con, parse_func(a.func))
        st = stale_line(con, a.gen)
        if st:
            print(st)
        return 0
    except FnFactsError as e:
        print('q: %s' % e, file=sys.stderr)
        return 2


if __name__ == '__main__':
    multiprocessing.freeze_support()
    sys.exit(main(sys.argv[1:]))
