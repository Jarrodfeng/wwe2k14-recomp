r"""qx7: two whole-game indexes over the generated listing (S103 lane QX4): virtual call sites and the draw-node registry (the run-log index is qrunlog.py).

    py tools/q/q.py vcall SLOT_OFFSET [--class VT] [--all] [--json] [--limit N]
        every VIRTUAL call site in the generated listing with that vtable slot offset: the `lwz rX,OFF(rY)` + `mtctr rX` + `bctrl` pattern (a tail `bctr` too).
        Each row: the site address (the bctrl), the enclosing function (+ db name), `call` / `tail`, and the receiver as far as the straight-line code says:
          `this`            the receiver is the function's own r3 (argument 3, never rewritten); when that function is a slot of an RTTI vtable the classes are listed
          `field+N of X`    the object came from a field of X; `global 0xADDR` an object read from a global; `arg rN`; `ret of call ADDR` the result of a call
          class (listed)    a constant vtable was stored at +0 of the object in this function (exact class), or `this` of a vtable slot function
        SLOT_OFFSET is a byte offset (0x34 = slot 13: offset / 4). Default = sites whose receiver pointer was loaded `lwz rY,0(rZ)` (a real vtable call);
        --all also lists the other `lwz OFF(rY)` + mtctr calls (a callback field of a struct). --class VT = only sites whose class is known to be VT (an address
        of a vtable, any slot address of it, or a class-name substring); it says how many sites of that offset have an UNRESOLVED receiver (they may belong to the class).
    py tools/q/q.py nodes [CATEGORY] [--json] [--limit N]
        the draw-node registry: every `bl 0x82C30A10` (sub_82C30A10(node, category, on)) call site: enclosing function, category (a `li r4,N`, else the unresolved
        source), the node's vtable (the constant stored at +0 of the node in the enclosing function), the class name, and slot 1 (vtable + 4: the node's draw
        method, read from the game image; --slot N for another slot). CATEGORY filters.

Built by a scan of every generated .cpp (350 files, <= 4 workers at below-normal priority, RAM-capped) into out\harness\index\vcall.s<VERSION>.db (WWE_QVCALL_DB=FILE = another file; files of other versions beside it are left alone) (game-derived: stays in
out\). It is automatic: a missing or stale table (codegen fingerprint / version) is rebuilt by the next call; a warm call is one stat + one query.
Limits: the receiver is tracked by a forward flow over the function's own branches (paths met at labels: a def survives a join only when every path agrees; a call kills the volatile registers except a save / restore helper, which kills r0 r11 r12; r3 at the call must be the receiver or a flag is printed); a vtable load separated from the call by a
call is not seen as `via vtable`; a node whose vtable is set by a base constructor in another function shows `?` (stated per row).
"""
import argparse
import glob
import json
import os
import re
import sqlite3
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
IDX = os.path.join(REPO, 'out', 'harness', 'index')
GEN = os.path.join(REPO, 'generated', 'default')
IMAGE = os.path.join(IDX, 'image.bin')
IMAGE_BASE = 0x82000000
NODE_REG = 0x82C30A10
VERSION = 4      # S105 D (F1): also in the FILE NAME (vcall.s4.db, db_name()): a q of another version never opens, wipes or rebuilds this file. S110 ICALL: v3 adds icall / icall_target / fstore; S110 CLASSFLOW: v4 = the class pass (icall_target.how = rule letters, meta classflow)


def db_name():
    """the table's file name carries the version (vcall.s2.db): a q of another version uses ANOTHER file and never opens, wipes or rebuilds this one (the design of qasset.db_name)"""
    return 'vcall.s%d.db' % VERSION


def vdb_path():
    r"""out\harness\index\vcall.s<VERSION>.db; WWE_QVCALL_DB=<file> = another file (how a builder / a test works without touching the shared index)"""
    return os.environ.get('WWE_QVCALL_DB') or os.path.join(IDX, db_name())


def other_index_files(path=None):
    """files of OTHER names beside ours (the old vcall.db, vcall.s1.db ...): left alone by q, named so the tidy / a person can clear them"""
    path = path or vdb_path()
    d = os.path.dirname(path)
    me = os.path.basename(path)
    try:
        return sorted(f for f in os.listdir(d) if re.match(r'^vcall(\.s\d+)?\.db$', f) and f != me)
    except OSError:
        return []

INS_RE = re.compile(r'^\t// ([A-Za-z][\w.+\-]*) ?(.*?)\s*$')
LABEL_RE = re.compile(r'^loc_([0-9A-F]{8}):')
DEF_RE = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
GOTO_RE = re.compile(r'goto loc_([0-9A-F]{8});')
MEM_RE = re.compile(r'^(-?\d+)\((r\d+)\)$')
REG_RE = re.compile(r'^r\d+$')
STORES = {'stb', 'sth', 'stw', 'std', 'stfs', 'stfd', 'stwx', 'stbx', 'sthx', 'stdx', 'stwbrx', 'sthbrx', 'stfsx', 'stfdx', 'stvx', 'stvewx', 'stwcx', 'stdcx'}
UPDATE_STORES = {'stwu', 'stbu', 'sthu', 'stdu', 'stfsu', 'stfdu', 'stwux', 'stbux', 'sthux'}
NODEST = {'nop', 'sync', 'isync', 'eieio', 'lwsync', 'twi', 'tw', 'tdi', 'td', 'trap', 'blr', 'dcbt', 'dcbf', 'dcbz', 'dcbst', 'dcbtst', 'icbi'}
VOLATILE = ['r0'] + [f'r{i}' for i in range(3, 13)]


class XError(Exception):
    pass


def hx(a):
    return f'0x{a:08X}'


def _imm(s):
    try:
        return int(s, 0)
    except (ValueError, TypeError):
        return None


# ------------------------------------------------------------------ the register tracker
class State:
    """register definitions at one point: d (explicit defs), dflt ('in' = a register not written yet holds its entry value, 'unk' = nothing is known), ctr (the def moved to CTR)."""
    __slots__ = ('d', 'dflt', 'ctr', 'oc')

    def __init__(self, d=None, dflt='in', ctr=None, oc=None):
        self.d, self.dflt, self.ctr = d if d is not None else {}, dflt, ctr
        self.oc = oc if oc is not None else {}      # S110 review: def of an object -> the vtable constant stored at +0 of it on EVERY path to here (no call since)

    def get(self, r):
        v = self.d.get(r)
        if v is not None:
            return v
        return ('in', r) if self.dflt == 'in' else ('unk', 0, r)

    def copy(self):
        return State(dict(self.d), self.dflt, self.ctr, dict(self.oc))

    def same(self, o):
        return self.dflt == o.dflt and self.ctr == o.ctr and self.oc == o.oc and all(self.get(r) == o.get(r) for r in set(self.d) | set(o.d))


def meet(a, b, label=0, null_join=False):
    """the state where two paths join: a def survives only when both agree; otherwise it becomes ('phi', label, reg): one value per register per join, never
    equal to another register's unknown (so a vtable stored through r31 is only matched with the node when both name the same r31).
    null_join (S110 CLASSFLOW, the class pass only): phi(x, li 0) = x, the null-checked object keeps its def"""
    if a is None:
        return b
    if b is None:
        return a
    dflt = a.dflt if a.dflt == b.dflt else 'unk'
    d = {}
    for r in set(a.d) | set(b.d):
        x, y = a.get(r), b.get(r)
        d[r] = x if x == y else (y if null_join and x == ('li', 0) else x if null_join and y == ('li', 0) else ('phi', label, r))
    return State(d, dflt, a.ctr if a.ctr == b.ctr else None, {k: v for k, v in a.oc.items() if b.oc.get(k) == v})


def site_cls(s):
    """S110 review: the vtable that reaches an indirect call of shape `lwz off(lwz 0(obj))`: the store at +0 of obj on every path to the call, else 0"""
    c = s.ctr
    if c and c[0] == 'lwz' and c[2][0] == 'lwz' and c[2][1] == 0:
        return s.oc.get(c[2][2], 0)
    return 0


class Tracker:
    """definitions over one function. A definition is a hashable tuple: ('in', rN) value at entry; ('li', v); ('lis', v); ('const', addr); ('glob', addr) = a word loaded
    from a global; ('lwz', off, base_def); ('addi', base_def, off); ('ret', call_site, callee); ('unk', site). A call kills the volatile registers, except a call of a
    register save / restore helper (`__savegprlr_N`...), which kills only r0 / r11 / r12; the paths are met at labels (`run_function`)."""

    def __init__(self, vt_ok, helpers, fconst=None):
        self.s = State()
        self.vt_ok = vt_ok
        self.helpers = helpers
        self.fconst = fconst  # S110 ICALL: a predicate, true for a function start or a table start: a constant of that kind stored to memory is recorded in fstores
        self.fstores = []     # (('field', off) | ('glob', address), constant) for each such store (field = the base is not a constant)
        self.objcls = {}      # def of an object -> vtable constant stored at +0 of it in this function
        self.stores = {}      # (def of base, offset) -> [vtable constants stored]
        self.rec = None       # S110 CLASSFLOW: the class pass's recorder (qclassflow.Rec): every stw, r3 at each return, the tail branches

    def get(self, r):
        return self.s.get(r)

    def step(self, addr, mn, ops, hit):
        p = [x.strip() for x in ops.split(',')] if ops else []
        s = self.s
        d = s.d
        if mn in STORES or mn in UPDATE_STORES:
            if len(p) >= 2:
                m = MEM_RE.match(p[1])
                if m:
                    off, base = int(m.group(1)), m.group(2)
                    if mn == 'stw':
                        src = s.get(p[0])
                        bd = s.get(base)
                        if self.rec is not None:
                            self.rec.on_stw(addr, src, bd, off)
                        if src[0] == 'const' and self.vt_ok(src[1]):
                            self.stores.setdefault((bd, off), []).append(src[1])
                            if off == 0:
                                self.objcls[bd] = src[1]
                                s.oc[bd] = src[1]
                        elif off == 0:
                            s.oc.pop(bd, None)      # another word over the vtable pointer: the class is no longer known here
                        if src[0] == 'const' and self.fconst is not None and self.fconst(src[1]):
                            key = ('glob', (bd[1] + off) & 0xFFFFFFFF) if bd[0] in ('lis', 'const') else ('field', off)
                            self.fstores.append((key, src[1]))
                    if mn in UPDATE_STORES:
                        d[base] = ('unk', addr, base)
            return
        if mn in ('bl', 'bctrl'):
            tgt = _imm(p[0]) if (mn == 'bl' and p) else None
            hit('call', addr, (tgt, s))
            if mn == 'bctrl' and s.ctr is not None:
                hit('vcall', addr, (s.ctr, s.get('r3'), site_cls(s)))
            if tgt is None or tgt not in self.helpers:
                s.oc = {}      # a call can construct / destroy an object it reaches: no vtable store is trusted past it
            if tgt is not None and tgt in self.helpers:
                kill = ('r0', 'r11', 'r12')
            else:
                kill = VOLATILE
            for r in kill:
                d[r] = ('unk', addr, r)
            if tgt is None or tgt not in self.helpers:
                d['r3'] = ('ret', addr, tgt)
            s.ctr = None
            return
        if mn[0] == 'b' and mn.endswith('ctr'):      # bctr, and a conditional one (bnectr: 1 site, a tail call through a global)
            if s.ctr is not None:
                hit('vtail', addr, (s.ctr, s.get('r3'), site_cls(s)))
            return
        if self.rec is not None and mn[0] == 'b':
            self.rec.on_branch(addr, mn, p, s)      # a return (r3) or a direct tail branch (its arguments)
        if mn == 'mtctr':
            s.ctr = s.get(p[0]) if p else None
            return
        if mn.startswith('bdnz') or mn.startswith('bdz'):
            s.ctr = None      # S110: a counted branch changes CTR itself
            return
        if mn in NODEST or mn[0] == 'b' or mn.startswith('cmp') or mn.startswith('mt') or mn.startswith('fcmp'):
            return
        if not p or not REG_RE.match(p[0]):
            return
        rd = p[0]
        if mn == 'lwzx' and len(p) == 3:      # S110: rD = word at rA + rB; either operand may be the constant table base
            d[rd] = ('lwzx', s.get(p[1]), s.get(p[2]))
            return
        if mn in ('lwz', 'lwzu') and len(p) >= 2:
            m = MEM_RE.match(p[1])
            if m:
                off, base = int(m.group(1)), m.group(2)
                bd = s.get(base)
                if mn == 'lwzu':
                    d[base] = ('unk', addr, base)
                d[rd] = ('glob', (bd[1] + off) & 0xFFFFFFFF) if bd[0] == 'lis' else ('lwz', off, bd)
                return
        elif mn == 'li' and len(p) >= 2:
            v = _imm(p[1])
            d[rd] = ('li', v) if v is not None else ('unk', addr, rd)
            return
        elif mn == 'lis' and len(p) >= 2:
            v = _imm(p[1])
            d[rd] = ('lis', (v & 0xFFFF) << 16) if v is not None else ('unk', addr, rd)
            return
        elif mn == 'mr' and len(p) >= 2:
            d[rd] = s.get(p[1])
            return
        elif mn == 'addi' and len(p) >= 3:
            v = _imm(p[2])
            sd = s.get(p[1])
            if v is not None:
                if sd[0] == 'lis':
                    d[rd] = ('const', (sd[1] + v) & 0xFFFFFFFF)
                elif v == 0:
                    d[rd] = sd
                elif sd[0] == 'addi':
                    d[rd] = ('addi', sd[1], sd[2] + v)
                else:
                    d[rd] = ('addi', sd, v)
                return
        elif mn == 'rlwinm' and len(p) == 5:      # S110 CLASSFLOW review: `rlwinm rD,rS,SH,0,31-SH` = rS << SH (an index scaled for a table read)
            sh, mb, me = _imm(p[2]), _imm(p[3]), _imm(p[4])
            if sh is not None and mb == 0 and me == 31 - sh:
                d[rd] = ('shl', sh, s.get(p[1]))
                return
        elif mn == 'ori' and len(p) >= 3:
            v = _imm(p[2])
            sd = s.get(p[1])
            if v is not None and sd[0] == 'lis':
                d[rd] = ('const', sd[1] | (v & 0xFFFF))
                return
        d[rd] = ('unk', addr, rd)


def describe(d, depth=0):
    t = d[0]
    if t == 'in':
        return 'this' if d[1] == 'r3' else f'arg {d[1]}'
    if t == 'glob':
        return f'global {hx(d[1])}'
    if t == 'const':
        return f'const {hx(d[1])}'
    if t == 'ret':
        return f'result of sub_{d[2]:08X}()' if len(d) > 2 and d[2] else f'result of the call at {hx(d[1])}'
    if t == 'lwz':
        return 'field of ...' if depth > 3 else f'field+{d[1]} of {describe(d[2], depth + 1)}'
    if t == 'addi':
        return f'{describe(d[1], depth + 1)}+{d[2]}'
    if t == 'li':
        return f'li {d[1]}'
    return 'unknown'


HEX_RE = re.compile(r'^0x([0-9a-fA-F]+)$')


def branch_info(mn, ops, cases):
    """-> (targets, falls_through) for one instruction"""
    if mn == 'blr':
        return [], False
    if mn == 'bctr':
        return list(cases or []), False
    if mn[0] != 'b' or mn in ('bl', 'bctrl', 'blrl') or mn.endswith('lr') or mn.endswith('ctr') or mn.endswith('lrl') or mn.endswith('ctrl'):
        return [], True            # not a branch, a call, or a conditional return
    last = ops.split(',')[-1].strip() if ops else ''
    m = HEX_RE.match(last)
    t = [int(m.group(1), 16)] if m else []
    return t, mn != 'b'


def run_function(insns, vt_ok, helpers, hit, fconst=None, rec_new=None):
    """forward flow over the instruction list with the paths met at every label (a def survives a join only when every incoming path agrees); no fall-through
    after b / blr / bctr; a backward branch feeds its state into the label on the next pass (<= 5 passes, monotone: defs only turn unknown); a label nothing
    reaches starts with nothing known. `hit` fires on the last pass only. Returns the final Tracker."""
    addrs = {x[0]: i for i, x in enumerate(insns)}
    targets = set()
    for a, mn, ops, cases in insns:
        for t in branch_info(mn, ops, cases)[0]:
            if t in addrs:
                targets.add(t)
    back_in = {}
    tr = None
    for _pass in range(5):
        tr = Tracker(vt_ok, helpers, fconst)
        if rec_new is not None:      # S110 CLASSFLOW: the class pass (a recorder per pass; the null-check join)
            tr.rec = rec_new()
        events = []
        pend = {}
        back_out = {}
        cur = State()
        reach = True
        for i, (a, mn, ops, cases) in enumerate(insns):
            if a in targets:
                inc = cur if reach else None
                for src in pend.get(a, []) + back_in.get(a, []):
                    inc = meet(inc, src, a, rec_new is not None)
                cur = inc.copy() if inc is not None else State(dflt='unk')
                reach = True
            elif not reach:
                cur = State(dflt='unk')       # unreachable code with no label (padding): nothing known
                reach = True
            tr.s = cur
            tr.step(a, mn, ops, lambda k, ad, info: events.append((k, ad, info if k != 'call' else (info[0], info[1].copy()))))
            tg, falls = branch_info(mn, ops, cases)
            for t in tg:
                if t in addrs:
                    st = cur.copy()      # S110: CTR is machine state, it survives a branch (`meet` keeps it when both paths agree)
                    (back_out if addrs[t] <= i else pend).setdefault(t, []).append(st)
            reach = falls
        if len(back_out) == len(back_in) and all(k in back_in and len(back_in[k]) == len(v) and all(x.same(y) for x, y in zip(back_in[k], v)) for k, v in back_out.items()):
            break
        back_in = back_out
    for k, ad, info in events:
        hit(k, ad, info, tr)
    return tr


# ------------------------------------------------------------------ the scan (worker side)
class Img:
    def __init__(self):
        try:
            with open(os.environ.get('WWE_Q_IMAGE') or IMAGE, 'rb') as f:
                self.data = f.read()
        except OSError:
            self.data = b''

    def word(self, a):
        o = a - IMAGE_BASE
        if a & 3 or o < 0 or o + 4 > len(self.data):
            return None
        return struct.unpack('>I', self.data[o:o + 4])[0]


_W = {}


def _init_worker(funcs, vts, helpers, tstarts=frozenset(), scanset=frozenset(), cf=False):
    """tstarts = the table starts of the index (`tables`); scanset = the functions that load a function / table-start constant (they are scanned for stores even without an indirect call);
    cf = the S110 CLASSFLOW class pass over every function (qclassflow.func_facts)"""
    _W['funcs'], _W['vts'], _W['helpers'], _W['img'] = funcs, vts, helpers, Img()
    _W['tstarts'], _W['scanset'], _W['cf'] = tstarts, scanset, cf
    try:
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x00004000)   # BELOW_NORMAL
    except Exception:
        pass


def _vt_ok(a):
    """a vtable start: an RTTI one, or any table whose first word is a function start"""
    if a in _W['vts']:
        return True
    w = _W['img'].word(a)
    return w is not None and w in _W['funcs']


def _read_body(lines, i, fa):
    """the instructions of the function whose DEFINE_REX_FUNC line is lines[i]: -> ([[addr, mnemonic, operands, switch cases or None], ...], index of its closing `}`)"""
    n = len(lines)
    j = i + 1
    insns = []
    addr = fa
    last_bctr = None
    while j < n and lines[j] != '}':
        l = lines[j]
        lm = LABEL_RE.match(l)
        if lm:
            addr = int(lm.group(1), 16)
            last_bctr = None
        else:
            im = INS_RE.match(l)
            if im:
                ins = [addr, im.group(1), im.group(2), None]
                insns.append(ins)
                last_bctr = ins if ins[1] == 'bctr' else None
                addr += 4
            elif last_bctr is not None:
                g = GOTO_RE.search(l)
                if g:
                    last_bctr[3] = (last_bctr[3] or []) + [int(g.group(1), 16)]
        j += 1
    return insns, j


def _fconst(a):
    return a in _W['funcs'] or a in _W['tstarts']


def scan_file(path):
    """-> (vcall rows, node rows, icall rows, function-pointer stores, class-pass facts {function: qclassflow.func_facts}) of one generated .cpp"""
    with open(path, encoding='utf-8', errors='replace') as f:
        text = f.read()
    if not _W['scanset'] and not _W.get('cf') and 'bctr' not in text and 'bl 0x82c30a10' not in text:
        return [], [], [], [], {}
    vrows, nrows, irows, frows, crows = [], [], [], [], {}
    lines = text.split('\n')
    n = len(lines)
    i = 0
    while i < n:
        m = DEF_RE.match(lines[i])
        if not m:
            i += 1
            continue
        fa = int(m.group(1), 16)
        insns, j = _read_body(lines, i, fa)
        i = j + 1
        if _W.get('cf'):
            import qclassflow
            crows[fa] = qclassflow.func_facts(fa, insns, _vt_ok, _W['helpers'], _W['funcs'])
        if fa in _W['scanset'] or any(x[1] == 'bctrl' or (x[1][0] == 'b' and x[1].endswith('ctr')) or (x[1] == 'bl' and x[2].lower() == '0x82c30a10') for x in insns):
            _scan_func(fa, insns, vrows, nrows, irows, frows)
    return vrows, nrows, irows, frows, crows


def objtag(o):
    """S110: what the receiver object of an indirect call is, as far as the function's own flow says"""
    t = o[0]
    if t == 'addi':
        return ('emb', o[2], o[1][0])      # embedded at +N of a base
    if t == 'lwz':
        return ('ptr', o[1], o[2][0])      # the pointer held at +N of a base
    if t == 'in':
        return ('arg', o[1])
    if t == 'ret':
        return ('ret', o[2] or 0)
    if t == 'glob':
        return ('glob', o[1])
    if t == 'const':
        return ('const', o[1])
    return (t,)


def classify(c, tr, cls=0):
    """S110: the shape of an indirect call from the def moved to CTR -> (shape, slot offset, value, receiver tag). Shapes: V_exact (class stored in the same function),
    V_this (`this` of the function), V_open (another receiver), V_const (a vtable constant), F (a struct field), G (a global pointer), T (lwzx on a constant
    table base), T_open, A (an argument: a callback), R (a call result), K (a constant), U (lost)."""
    t = c[0]
    if t == 'lwz':
        off, bd = c[1], c[2]
        if bd[0] == 'lwz' and bd[1] == 0:
            obj = bd[2]
            if cls:      # the vtable stored at +0 of obj on every path to the call (site_cls), not the function's last store
                return 'V_exact', off, cls, objtag(obj)
            if obj == ('in', 'r3'):
                return 'V_this', off, 0, objtag(obj)
            return 'V_open', off, 0, objtag(obj)
        if bd[0] == 'const':
            if _vt_ok(bd[1]):
                return 'V_const', off, bd[1], None
            return 'G', off, (bd[1] + off) & 0xFFFFFFFF, None
        return 'F', off, 0, objtag(bd)
    if t == 'glob':
        return 'G', 0, c[1], None
    if t == 'lwzx':
        # S110 CLASSFLOW review: a table of function WORDS only when the index is scaled by 4; another stride reads a field of a record array
        # (sub_827DCDB0 0x827DCEC4: index << 4), whose entries the word run from the base does not list (coverage run 083 saw 4 targets outside it)
        for b, i in ((c[1], c[2]), (c[2], c[1])):
            if b[0] == 'const' and i[0] == 'shl' and i[1] == 2:
                return 'T', 0, b[1], None
        return 'T_open', 0, 0, (objtag(c[1]), objtag(c[2]))
    if t == 'in':
        return 'A', 0, c[1], None
    if t == 'ret':
        return 'R', 0, c[2] or 0, None
    if t == 'const':
        return 'K', 0, c[1], None
    return 'U', 0, 0, (t,)


def _scan_func(fa, insns, vrows, nrows, irows, frows):
    node_hits = []
    vhits = []
    ihits = []
    cases = {x[0]: x[3] for x in insns if x[1] == 'bctr' and x[3]}

    def hit(kind, a, info, tr):
        if kind == 'call':
            tgt, st = info
            if tgt == NODE_REG:
                node_hits.append((a, st.get('r3'), st.get('r4'), st.get('r5'), tr))
            return
        c, r3d = info[0], info[1]
        if a not in cases:
            ihits.append((kind, a, c, tr, info[2]))
        if c[0] != 'lwz':
            return
        off, bd = c[1], c[2]
        cls, obj_this, r3ok = 0, 0, 1
        if bd[0] == 'lwz' and bd[1] == 0:
            via, obj = 1, bd[2]
            recv = describe(obj)
            cls = tr.objcls.get(obj, 0)
            obj_this = 1 if obj == ('in', 'r3') else 0
            r3ok = 1 if (r3d == obj or (r3d[0] == 'addi' and r3d[1] == obj)) else 0
        elif bd[0] == 'const' and _vt_ok(bd[1]):
            via, recv, cls = 1, 'vtable constant', bd[1]
        else:
            via, recv = 0, describe(bd)
        vhits.append((off, a, fa, 'call' if kind == 'vcall' else 'tail', via, recv, cls, obj_this, r3ok))

    tr0 = run_function(insns, _vt_ok, _W['helpers'], hit, _fconst)
    vrows.extend(vhits)
    # S110 ICALL: every indirect call / tail jump (a switch `bctr` is not a call: its row is kind 'switch', counted, not stored as a call)
    seen = set()
    for kind, a, c, tr, scls in ihits:
        seen.add(a)
        shape, off, val, recv = classify(c, tr, scls)
        irows.append((a, fa, 'call' if kind == 'vcall' else 'tail', shape, off, val, recv, 0))
    for a, mn, ops, cs in insns:
        if (mn == 'bctrl' or (mn[0] == 'b' and mn.endswith('ctr'))) and a not in seen:
            if mn == 'bctr' and cs:
                irows.append((a, fa, 'switch', 'S', 0, 0, None, len(cs)))
            else:
                irows.append((a, fa, 'call' if mn == 'bctrl' else 'tail', 'U', 0, 0, ('ctrlost',), 0))
    for (bd, off), vts in tr0.stores.items():
        for v in vts:
            frows.append(('vtfield', off, v))
    for key, v in tr0.fstores:
        frows.append((key[0], key[1], v))
    for (a, rd3, rd4, rd5, tr) in node_hits:
        cat = rd4[1] if rd4[0] == 'li' else -1
        on = rd5[1] if rd5[0] == 'li' else -1
        base, off = (rd3[1], rd3[2]) if rd3[0] == 'addi' else (rd3, 0)
        vts = tr.stores.get((base, off)) or []      # the function's LAST such store (a derived ctor sets its vtable after the base one registers)
        nrows.append((a, fa, cat, '' if cat >= 0 else describe(rd4), on, describe(rd3), vts[-1] if vts else 0))


# ------------------------------------------------------------------ resolving the indirect calls (main process)
def _tag_text(t):
    if not t:
        return ''
    return ':'.join(hx(x) if isinstance(x, int) and x > 0xFFFF else str(x) for x in (t if not isinstance(t[0], tuple) else (t[0][0], t[1][0])))


class Resolver:
    """the targets a site can reach, from the tables of the index, the image words and the stores the scan saw (one rule per shape; INDIRECT.md `Method`)"""

    def __init__(self, icon, img, fst):
        self.img = img
        self.funcs = frozenset(r[0] for r in icon.execute('SELECT addr FROM funcs'))
        self.tables, self.rule = {}, {}
        for s, rule, n in icon.execute('SELECT start, rule, len FROM tables'):
            self.tables[s] = [img.word(s + 4 * i) for i in range(n)]
            self.rule[s] = rule
        # S110 ICALL review: a function some code calls directly (call / tail / cond edge) can run with a `this` whose vtable does not list it
        # (Derived::f calling Base::f with its own this: sub_823CCFA0 -> sub_823E6F90 at 0x823CD014); its `this` class is then not known
        self.direct = frozenset(r[0] for r in icon.execute("SELECT DISTINCT dst FROM edges WHERE kind IN ('call', 'tail', 'cond')"))
        self.member = {}      # function -> [(table start, slot offset)]
        self.by_off = {}      # slot offset -> every function in a table slot at it
        for s, e in self.tables.items():
            for i, f in enumerate(e):
                self.member.setdefault(f, []).append((s, 4 * i))
                self.by_off.setdefault(4 * i, set()).add(f)
        self.cls_of_vt = {vt: n for vt, n in icon.execute('SELECT v.vt, c.name FROM rtti_vtable v JOIN rtti_class c ON c.td = v.td')}
        self.vts_of_cls = {}
        for vt, n in self.cls_of_vt.items():
            self.vts_of_cls.setdefault(n, set()).add(vt)
        self.derived = {}
        for cl, base in icon.execute('SELECT cls, base FROM rtti_base'):
            self.derived.setdefault(base, set()).add(cl)
        self.vtfield, self.fptr = {}, {}
        for kind, key, v in fst:
            if kind == 'vtfield':
                self.vtfield.setdefault(key, set()).add(v)
            else:
                self.fptr.setdefault((kind, key), set()).add(v)

    def slot_targets(self, vts, off):
        """-> the words at +off of each vtable, or None when one of them does not reach the slot (the table ends before it, or the word is not a function):
        a set that silently drops that vtable would read as complete"""
        out = set()
        for vt in vts:
            e = self.tables.get(vt)
            if e is not None:
                if off // 4 >= len(e):
                    return None
                out.add(e[off // 4])
            else:
                w = self.img.word(vt + off)
                if w not in self.funcs:
                    return None
                out.add(w)
        return out

    def derived_vts(self, vts):
        allv = set(vts)
        for vt in vts:
            n = self.cls_of_vt.get(vt)
            if n:
                stack, seen = [n], set()
                while stack:
                    x = stack.pop()
                    for d in self.derived.get(x, ()):
                        if d not in seen:
                            seen.add(d)
                            stack.append(d)
                            allv |= self.vts_of_cls.get(d, set())
        return allv

    def resolve(self, site):
        """-> (targets, how) with how in NARROW_HOW / 'class' / 'const' / 'offset' / 'field-offset' / 'none'; an empty set = open"""
        a, fa, kind, shape, off, val, recv = site[:7]
        if shape in ('V_exact', 'V_const'):
            return self.slot_targets({val}, off) or set(), 'class'
        if shape == 'V_this':
            mem = self.member.get(fa, [])
            # only when nothing calls fa directly and every table listing it is a vtable start (RTTI / code-loaded): a 'run' table (no code loads it)
            # is no object's vtable, or a vtable cut at a non-function word, whose slot offsets are then shifted
            t = (self.slot_targets(self.derived_vts({s for s, o in mem}), off)
                 if mem and fa not in self.direct and all(self.rule.get(s) != 'run' for s, o in mem) else None)
            if t:
                return t, 'this-class'
            return self.by_off.get(off, set()), 'offset'
        if shape == 'V_open':
            if recv and recv[0] == 'emb' and self.vtfield.get(recv[1]):
                t = self.slot_targets(self.vtfield[recv[1]], off)
                if t:
                    return t, 'embedded-field'
            return self.by_off.get(off, set()), 'offset'
        if shape == 'G':
            t = set(self.fptr.get(('glob', val), set()))
            w = self.img.word(val)
            if w in self.funcs:
                t.add(w)
            return t, 'global'
        if shape == 'F':
            return self.fptr.get(('field', off), set()), 'field-offset'
        if shape == 'T':
            e = self.tables.get(val)
            if e is None:
                e, x = [], val
                while self.img.word(x) in self.funcs:
                    e.append(self.img.word(x))
                    x += 4
            return set(e), 'table'
        if shape == 'K':
            return {val}, 'const'
        return set(), 'none'


def resolve_sites(icon, img, rows, fst, gen=None):
    """-> (icall rows, icall_target rows). status: exact (one target, or every caller of a callback passes a function), narrow (a smaller set than the slot offset
    gives), cand (embedded-field: the vtables stored at that field offset of any object; stored, printed apart, still counted as incomplete),
    offset (only the set of everything in a table slot / field at that offset: NOT stored, counted at query time), open (CTR not resolved)"""
    R = Resolver(icon, img, fst)
    out, targets = [], []
    arg_cache = {}
    for site in rows:
        a, fa, kind, shape, off, val, recv = site[:7]
        if shape == 'S':
            continue
        if shape == 'A':
            key = (fa, val)
            if key not in arg_cache:
                cv = caller_values(icon, fa, val, gen)
                ok = bool(cv and cv['n'] and cv['n'] == cv['shown'] and not cv['lits'] and cv['other'] and
                          all(o[1].startswith('const 0x') and int(o[1][6:], 16) in R.funcs for o in cv['other']))
                arg_cache[key] = {int(o[1][6:], 16) for o in cv['other']} if ok else set()
            t, how = arg_cache[key], 'arg'
        else:
            t, how = R.resolve(site)
        if not t:
            status = 'open'
        elif how in ('offset', 'field-offset'):
            status = 'offset'
        elif how == 'embedded-field':
            status = 'cand'      # S110 review: vtables stored at +N of ANY object (out-of-line constructors unseen): candidates, the class is not checked
        else:
            status = 'exact' if len(t) == 1 else 'narrow'
        out.append((a, fa, kind, shape, off, _tag_text(recv), status, len(t)))
        if status in ('exact', 'narrow', 'cand'):
            targets.extend((a, x, how) for x in sorted(t))
    return out, targets


# ------------------------------------------------------------------ build / open
def _fingerprint():
    try:
        with open(os.path.join(GEN, 'codegen.stamp'), encoding='utf-8') as f:
            return json.load(f).get('fingerprint', '?')
    except (OSError, ValueError):
        return '?'


def _free_ram_gb():
    try:
        import ctypes

        class MS(ctypes.Structure):
            _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong), ('ullTotalPhys', ctypes.c_ulonglong),
                        ('ullAvailPhys', ctypes.c_ulonglong), ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                        ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong), ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
        m = MS()
        m.dwLength = ctypes.sizeof(MS)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
        return m.ullAvailPhys / 2**30
    except Exception:
        return None


def workers():
    free = _free_ram_gb()
    by_ram = 4 if free is None else int((free - 4) // 0.4)      # a worker holds one ~1 MB file and its parse (~0.4 GB with the image copy)
    return max(1, min(4, by_ram))


def _index_con():
    p = os.environ.get('WWE_Q_INDEX') or os.path.join(IDX, 'index.db')      # WWE_Q_INDEX = another index file (a builder working beside the shared one)
    if not os.path.exists(p):
        raise XError(f'the code index {p} is missing: py tools\\q\\build_index.py')
    c = sqlite3.connect(f'file:{p}?mode=ro', uri=True)
    c.row_factory = sqlite3.Row
    return c


def build(path=None, gen=None, jobs=None, quiet=False, files=None, index_con=None):
    """scan every generated recomp .cpp into vcall.s<VERSION>.db (atomic: temp file, then replace)"""
    from concurrent.futures import ProcessPoolExecutor
    path = path or vdb_path()
    gen = gen or GEN
    t0 = time.time()
    con = index_con or _index_con()
    funcs = frozenset(r[0] for r in con.execute('SELECT addr FROM funcs'))
    vts = frozenset(r[0] for r in con.execute('SELECT vt FROM rtti_vtable'))
    helpers = frozenset(r[0] for r in con.execute("SELECT addr FROM funcs WHERE name GLOB '__save*' OR name GLOB '__rest*'"))      # register save / restore helpers: a call of one kills only r0 / r11 / r12
    files = files or sorted(glob.glob(os.path.join(gen, 'wwe2k14_recomp.*.cpp')))
    if not files:
        raise XError(f'no generated .cpp under {gen} (generated\\ is local only)')
    try:      # S110 ICALL: the table starts and the functions that load a function / table-start constant (they are scanned for the stores of function pointers)
        tstarts = frozenset(r[0] for r in con.execute('SELECT start FROM tables'))
    except sqlite3.Error:
        raise XError('the code index has no `tables` (built before S110 ICALL): py tools\\q\\build_index.py')
    scanset = set()
    for fn, v in con.execute("SELECT func, value FROM imms WHERE op IN ('lis+addi', 'lis+ori', 'lis+addi?', 'lis+ori?')"):
        if v in funcs or v in tstarts:
            scanset.add(fn)
    tmp = f'{path}.{os.getpid()}.tmp'
    if os.path.exists(tmp):
        os.remove(tmp)
    out = sqlite3.connect(tmp)
    out.executescript('''CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE vcall(off INTEGER, site INTEGER, func INTEGER, kind TEXT, via_vt INTEGER, recv TEXT, cls INTEGER, this_recv INTEGER, r3ok INTEGER);
        CREATE TABLE nodes(site INTEGER, func INTEGER, cat INTEGER, cat_src TEXT, on_flag INTEGER, node TEXT, vt INTEGER);
        CREATE TABLE icall(site INTEGER, func INTEGER, kind TEXT, shape TEXT, off INTEGER, recv TEXT, status TEXT, n INTEGER);
        CREATE TABLE icall_target(site INTEGER, target INTEGER, how TEXT);
        CREATE TABLE fstore(kind TEXT, key INTEGER, value INTEGER);''')
    jobs = jobs or workers()
    isites, fsts, cfacts = [], set(), {}
    with ProcessPoolExecutor(max_workers=jobs, initializer=_init_worker, initargs=(funcs, vts, helpers, tstarts, frozenset(scanset), True)) as ex:
        for vrows, nrows, irows, frows, crows in ex.map(scan_file, files, chunksize=4):
            out.executemany('INSERT INTO vcall VALUES (?,?,?,?,?,?,?,?,?)', vrows)
            out.executemany('INSERT INTO nodes VALUES (?,?,?,?,?,?,?)', nrows)
            isites.extend(irows)
            fsts.update(frows)
            cfacts.update(crows)
    n_switch = sum(1 for r in isites if r[3] == 'S')
    icalls, itargets = resolve_sites(con, Img(), isites, fsts, gen)
    import qclassflow      # S110 CLASSFLOW: the receiver's class across functions narrows the offset / cand sites (how = the rule letters)
    icalls, itargets, cftext = qclassflow.build(con, gen, icalls, itargets, cfacts)
    del cfacts
    out.executemany('INSERT INTO icall VALUES (?,?,?,?,?,?,?,?)', icalls)
    out.executemany('INSERT INTO icall_target VALUES (?,?,?)', itargets)
    out.executemany('INSERT INTO fstore VALUES (?,?,?)', sorted(x for x in fsts if x[0] != 'vtfield'))
    out.executescript('CREATE INDEX vcall_off ON vcall(off); CREATE INDEX nodes_cat ON nodes(cat); CREATE INDEX icall_off ON icall(status, shape, off); '
                      'CREATE INDEX icall_func ON icall(func); CREATE INDEX icall_site ON icall(site); CREATE INDEX icall_target_t ON icall_target(target); CREATE INDEX fstore_v ON fstore(value);')
    nv = out.execute('SELECT COUNT(*) FROM vcall').fetchone()[0]
    nn = out.execute('SELECT COUNT(*) FROM nodes').fetchone()[0]
    out.executemany('INSERT INTO meta VALUES (?,?)', [('version', str(VERSION)), ('fingerprint', _fingerprint()), ('built', time.strftime('%Y-%m-%d %H:%M')),
                                                       ('seconds', f'{time.time() - t0:.1f}'), ('jobs', str(jobs)), ('files', str(len(files))),
                                                       ('switch_bctr', str(n_switch)), ('icall_sites', str(len(icalls))), ('classflow', cftext)])
    out.commit()
    out.close()
    delay, waited = 0.25, 0.0      # a reader holding the old file open (Windows) refuses the replace: back off ~30 s, then give up quietly (S105 D, F-2)
    while True:
        try:
            os.replace(tmp, path)
            break
        except PermissionError:
            if waited >= 30:
                try:
                    os.remove(tmp)
                except OSError:
                    pass
                print(f'q: a reader holds {path}: the new table was not swapped in, answering from the old one', file=sys.stderr)
                return path
            time.sleep(delay)
            waited += delay
            delay = min(delay * 2, 4.0)
    if not quiet:
        other = other_index_files(path)      # S105 D: files of other versions beside ours are left alone, named here
        print(f'q: built {path}: {nv} virtual-call rows, {nn} node registrations, {len(files)} files, {jobs} workers, {time.time() - t0:.1f} s'
              + (f' (older index files left alone: {", ".join(other)})' if other else ''), file=sys.stderr)
    return path


def open_vdb(path=None):
    path = path or vdb_path()
    stale = True
    if os.path.exists(path):
        try:
            c = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
            meta = dict(c.execute('SELECT key, value FROM meta').fetchall())
            c.close()
            stale = meta.get('version') != str(VERSION) or (path == vdb_path() and meta.get('fingerprint') != _fingerprint() and os.path.isdir(GEN))
        except sqlite3.Error:
            stale = True
    if stale:
        build(path)
    c = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    c.row_factory = sqlite3.Row
    return c


# ------------------------------------------------------------------ queries
def icall_callers(icon, func, db=None):
    """S110 ICALL: the indirect callers of `func` and what this list cannot see.
    -> {'sites': [{site, func, shape, off, status, n, how}] (exact: the one target; narrow: one of n targets; cand: a candidate, class not checked),
        'offsets': {slot offset: sites that may call it}, 'n_incomplete': their sum, 'n_open': sites whose CTR is not resolved at all (counted when the function's
        address is taken), 'address_taken': bool}
    The offset count is every virtual / field call (V_this, V_open, F) at an offset where the function sits in a table slot or has been stored into a field, minus the
    exact / narrow sites (a cand site stays in the count)."""
    v = open_vdb(db)
    sites = [{'site': r[0], 'func': r[1], 'shape': r[2], 'off': r[3], 'status': r[4], 'n': r[5], 'how': r[6]} for r in v.execute(
        'SELECT i.site, i.func, i.shape, i.off, i.status, i.n, t.how FROM icall_target t JOIN icall i ON i.site = t.site WHERE t.target = ? ORDER BY i.site', (func,))]
    listed = {s['site'] for s in sites if s['status'] in ('exact', 'narrow')}
    slots, fields = set(), set()
    for (at,) in icon.execute("SELECT at FROM ptrs WHERE value = ? AND kind = 'func'", (func,)).fetchall():
        t = icon.execute('SELECT start, len FROM tables WHERE start <= ? ORDER BY start DESC LIMIT 1', (at,)).fetchone()
        if t and at < t[0] + 4 * t[1]:
            slots.add(at - t[0])
    for (k,) in v.execute("SELECT DISTINCT key FROM fstore WHERE kind = 'field' AND value = ?", (func,)):
        fields.add(k)
    offsets = {}
    for off in sorted(slots | fields):
        # both shapes at either kind of offset: an F site is often a slot call on an embedded object (0x8336CA1C: `lwz r11,36(r31); lwz r10,8(r11); bctrl`,
        # the vtable of this+36 read in one step), and a field call may read a table (S110 ICALL review)
        # S110 CLASSFLOW: a site that is exact / narrow (to any function) is no longer incomplete; the line counts every site that is not
        n = sum(1 for (site,) in v.execute("SELECT site FROM icall WHERE off = ? AND shape IN ('V_this', 'V_open', 'F') AND status NOT IN ('exact', 'narrow')", (off,)) if site not in listed)
        if n:
            offsets[off] = n
    taken = bool(slots or fields or icon.execute("SELECT 1 FROM imms WHERE value = ? AND op IN ('lis+addi', 'lis+ori', 'lis+addi?', 'lis+ori?') LIMIT 1", (func,)).fetchone())
    n_open = v.execute("SELECT COUNT(*) FROM icall WHERE status = 'open'").fetchone()[0] if (taken or offsets) else 0
    return {'sites': sites, 'offsets': offsets, 'n_incomplete': sum(offsets.values()), 'n_open': n_open, 'address_taken': taken}


def _db_names():
    try:
        import qvtable
        return qvtable.db_names()
    except Exception:
        return {}


def _vt_classes(con):
    cls = {}
    for r in con.execute('SELECT v.vt, c.name FROM rtti_vtable v JOIN rtti_class c ON c.td = v.td'):
        cls[r[0]] = r[1]
    slot_of = {}
    for r in con.execute('SELECT vt, idx, func FROM rtti_slot'):
        slot_of.setdefault(r[2], []).append((r[0], r[1]))
    return cls, slot_of


def parse_num(s):
    """0x.. = hex; all digits = decimal (036 is 36, never 0x36); anything else with hex letters = hex"""
    t = s.strip()
    try:
        if t.lower().startswith('0x'):
            return int(t, 16)
        if t.isdigit():
            return int(t, 10)
        return int(t, 16)
    except ValueError:
        raise XError(f'{s}: not a number')


def vcall_main(argv, con=None):
    ap = argparse.ArgumentParser(prog='q vcall')
    ap.add_argument('offset', help='slot byte offset (0x34 = slot 13)')
    ap.add_argument('--class', dest='cls', default=None, help='vtable address (any slot address of it) or a class-name substring')
    ap.add_argument('--all', action='store_true', help='also the non-vtable `lwz OFF(rY)` calls')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--limit', type=int, default=60)
    ap.add_argument('--db', default=None)
    a = ap.parse_args(argv)
    off = parse_num(a.offset)
    if off % 4 or off < 0:
        raise XError(f'offset {off} is not a non-negative multiple of 4')
    v = open_vdb(a.db)
    icon = con if con is not None else _index_con()
    names = _db_names()
    cnames, slot_of = _vt_classes(icon)
    rows = v.execute('SELECT * FROM vcall WHERE off = ? ORDER BY site', (off,)).fetchall()
    total = len(rows)
    nonvt = sum(1 for r in rows if not r['via_vt'])
    if not a.all:
        rows = [r for r in rows if r['via_vt']]

    def classes_of(r):
        if r['cls']:
            return [r['cls']]
        if r['this_recv']:
            return sorted({vt for vt, _ in slot_of.get(r['func'], [])})
        return []

    sel_vt = None
    if a.cls:
        c = a.cls
        sel_vt = set()
        if re.fullmatch(r'(?:0x|sub_)?[0-9A-Fa-f]{8}', c):
            addr = int(c[-8:], 16)
            for vt, ns in icon.execute('SELECT vt, nslots FROM rtti_vtable'):
                if vt <= addr < vt + 4 * max(ns, 1):
                    sel_vt.add(vt)
        if not sel_vt:
            sel_vt = {vt for vt, n in cnames.items() if c.lower() in (n or '').lower()}
        if not sel_vt:
            raise XError(f'--class {c}: no RTTI vtable / class matches')
    res = []
    unresolved = 0
    for r in rows:
        cl = classes_of(r)
        if sel_vt is not None:
            if not cl:
                unresolved += 1
                continue
            if not (set(cl) & sel_vt):
                continue
        res.append({'site': hx(r['site']), 'func': f"sub_{r['func']:08X}", 'name': names.get(r['func'], ''), 'kind': r['kind'], 'via_vtable': bool(r['via_vt']),
                    'receiver': r['recv'], 'r3_is_receiver': bool(r['r3ok']), 'classes': [{'vt': hx(c), 'name': cnames.get(c, '?')} for c in cl]})
    if a.json:
        print(json.dumps({'offset': off, 'slot': off // 4, 'sites_at_offset': total, 'non_vtable_hidden': 0 if a.all else nonvt,
                          'unresolved_omitted': unresolved if sel_vt is not None else None, 'sites': res}, indent=1))
        return 0
    print(f'q vcall 0x{off:X} (slot {off // 4}): {len(res)} site(s) listed of {total} `lwz {off}(rY)` + mtctr call site(s) in the generated code')
    for x in res[:a.limit]:
        cl = ', '.join(f"{c['name']}@{c['vt']}" for c in x['classes'][:3]) + (' ...' if len(x['classes']) > 3 else '')
        print(f"  {x['site']}  {x['func']:13s} {x['name'][:34]:34s} {x['kind']:4s} recv: {x['receiver']}" + (f'  class: {cl}' if cl else '')
              + ('' if x['via_vtable'] else '  [not a vtable load]') + ('' if x['r3_is_receiver'] else '  [FLAG: r3 at the call is not the receiver]'))
    if len(res) > a.limit:
        print(f'  ... {len(res) - a.limit} more (--limit N)')
    flagged = sum(1 for x in res if not x['r3_is_receiver'])
    if flagged:
        print(f'  ({flagged} listed site(s) are flagged: r3 at the call is not the object the vtable was read from, so the callee may not get that object as `this`)')
    if not a.all and nonvt:
        print(f'  ({nonvt} more site(s) read the pointer from a field that is not `lwz rY,0(rZ)`: --all)')
    if sel_vt is not None:
        print(f'  ({unresolved} site(s) of this offset have a receiver whose class the straight-line code does not show: they may belong to the class)')
    print("  (receiver tracked through the function's branches (paths met at labels); `this` = the function's own r3; classes for `this` = RTTI vtables the function is a slot of)")
    return 0


ARG_RE = re.compile(r'^arg (r\d+)$')
CALLER_CAP = 60      # callers parsed per registration (a base constructor with hundreds of callers is capped, said in the row)


def caller_values(icon, func, reg, gen=None, cap=CALLER_CAP):
    """QAF: one level of argument passing. A registration whose category is the ARGUMENT `reg` of its enclosing function `func` (a builder that passes the category on
    to sub_82C30A10): the value every caller puts in `reg` at its `bl func`, from the same forward flow the scan uses, run on each caller only.
    -> {'lits': [(value, caller name, site)] in caller order, 'other': [(caller name, what the register holds / `tail call`)], 'n': callers, 'shown': parsed} or None
    when the call edges / the listing are not readable (an index without edges, no generated\\)."""
    gen = gen or os.environ.get('WWE_Q_GEN') or GEN
    try:
        rows = icon.execute("SELECT e.src, e.site, e.kind, f.name, f.file, f.line FROM edges e JOIN funcs f ON f.addr = e.src WHERE e.dst = ? AND e.kind IN ('call', 'tail') "
                            "GROUP BY e.src, e.site ORDER BY e.src, e.site", (func,)).fetchall()
        helpers = frozenset(r[0] for r in icon.execute("SELECT addr FROM funcs WHERE name GLOB '__save*' OR name GLOB '__rest*'"))
    except sqlite3.Error:
        return None
    lits, other, files, bodies = [], [], {}, {}
    srcs = []
    for r in rows:
        if r[0] not in srcs:
            srcs.append(r[0])
    keep = set(srcs[:cap])
    for src, site, kind, name, file, line in rows:
        if src not in keep:
            continue
        if kind == 'tail':
            other.append((name or f'sub_{src:08X}', 'tail call'))
            continue
        if src not in bodies:
            path = os.path.join(gen, file or '')
            if path not in files:
                try:
                    with open(path, encoding='utf-8', errors='replace') as f:
                        files[path] = f.read().split('\n')
                except OSError:
                    files[path] = None
            lines = files[path]
            vals = None
            if lines is not None:
                want = f'DEFINE_REX_FUNC(sub_{src:08X})'
                i = max(0, (line or 1) - 1)
                if i >= len(lines) or not lines[i].startswith(want):
                    i = next((k for k, l in enumerate(lines) if l.startswith(want)), None)
                if i is not None:
                    insns, _ = _read_body(lines, i, src)
                    vals = {}

                    def hit(kind_, a, info, tr, vals=vals):
                        if kind_ == 'call' and info[0] == func:
                            vals[a] = info[1].get(reg)
                    run_function(insns, lambda a: False, helpers, hit)
            bodies[src] = vals
        v = (bodies[src] or {}).get(site)
        who = name or f'sub_{src:08X}'
        if v is None:
            other.append((who, 'not read'))
        elif v[0] == 'li':
            lits.append((v[1], who, site))
        else:
            other.append((who, describe(v)))
    return {'lits': lits, 'other': other, 'n': len(srcs), 'shown': len(keep)}


def category_text(cv, reg):
    """the `cat` column for a registration whose category came from callers (cv = caller_values)"""
    vals = sorted({v for v, _, _ in cv['lits']})
    cap = f'; the first {cv["shown"]} of {cv["n"]} callers' if cv['n'] > cv['shown'] else ''
    if vals and not cv['other']:
        return '|'.join(str(v) for v in vals) + f' (from callers{cap})'
    who = ', '.join(sorted({w for w, _ in cv['other']}))
    if not cv['n']:
        return f'? (arg {reg}; no callers in the call edges)'
    if vals:
        return '|'.join(str(v) for v in vals) + f'|? (from callers; non-literal from: {who}{cap})'
    return f'? (argument {reg}; callers: {who}{cap})'


def nodes_main(argv, con=None):
    ap = argparse.ArgumentParser(prog='q nodes')
    ap.add_argument('category', nargs='?', default=None)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--limit', type=int, default=300)
    ap.add_argument('--slot', type=int, default=1, help='which vtable slot to resolve (default 1 = the draw method)')
    ap.add_argument('--db', default=None)
    a = ap.parse_args(argv)
    v = open_vdb(a.db)
    icon = con if con is not None else _index_con()
    names = _db_names()
    cnames, _ = _vt_classes(icon)
    img = Img()
    q, pr = 'SELECT * FROM nodes', ()
    want = None
    if a.category is not None:
        want = parse_num(a.category)
        q, pr = q + ' WHERE cat = ? OR cat = -1', (want,)      # -1 = an unresolved site: kept when its callers' literals (QAF) contain the category
    res = []
    for r in v.execute(q + ' ORDER BY cat, site', pr).fetchall():
        vt = r['vt']
        s1 = img.word(vt + 4 * a.slot) if vt else None
        if s1 is not None and icon.execute('SELECT 1 FROM funcs WHERE addr = ?', (s1,)).fetchone() is None:
            s1 = None      # past the end of that table: not a function
        ctext, clits, cother = None, None, None
        m = ARG_RE.match(r['cat_src'] or '') if r['cat'] < 0 else None
        if m:      # QAF: the category is an argument of the builder: the literals its callers pass (one level)
            cv = caller_values(icon, r['func'], m.group(1))
            if cv is not None:
                ctext = category_text(cv, m.group(1))
                clits = sorted({x[0] for x in cv['lits']}) or None
                cother = sorted({x[0] for x in cv['other']}) or None
        if want is not None and r['cat'] != want and want not in (clits or []):
            continue
        res.append({'category': r['cat'] if r['cat'] >= 0 else None, 'category_source': r['cat_src'] or None, 'site': hx(r['site']), 'func': f"sub_{r['func']:08X}",
                    'func_name': names.get(r['func'], ''), 'node': r['node'], 'vtable': hx(vt) if vt else None, 'class': cnames.get(vt) if vt else None,
                    'slot1': f'sub_{s1:08X}' if s1 else None, 'slot1_name': names.get(s1, '') if s1 else '',
                    'category_from_callers': clits, 'category_callers_nonliteral': cother, 'category_text': ctext})
    if a.json:
        print(json.dumps({'count': len(res), 'nodes': res}, indent=1))
        return 0
    print(f'q nodes{" " + a.category if a.category else ""}: {len(res)} registration(s) of sub_82C30A10 (node, category, on)')
    for x in res[:a.limit]:
        cat = x['category'] if x['category'] is not None else (x['category_text'] or f"? ({x['category_source']})")
        print(f"  cat {str(cat):>10}  {x['site']}  {x['func']:13s} {x['func_name'][:26]:26s} node {x['node'][:22]:22s} vt {x['vtable'] or '?':10s} slot{a.slot} {x['slot1'] or '?':13s} {x['slot1_name'][:28]}"
              + (f"  [{x['class']}]" if x['class'] else ''))
    if len(res) > a.limit:
        print(f'  ... {len(res) - a.limit} more (--limit N)')
    print("  (category = the `li r4,N` before the call; when it is an ARGUMENT of the builder, the literals its callers pass (one level: `22|30 (from callers)`), else `?` and who calls; vtable = the constant stored at +0 of the node in the SAME function, `?` = a base constructor in another function sets it; slotN = the image word at vtable+4*N, N = --slot, default 1)")
    return 0


def main(argv, con=None):
    if not argv:
        print(__doc__)
        return 2
    verb, rest = argv[0], argv[1:]
    if verb == 'vcall':
        return vcall_main(rest, con)
    if verb == 'nodes':
        return nodes_main(rest, con)
    if verb == 'build':
        build()
        return 0
    raise XError(f'unknown verb {verb}')


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except XError as e:
        print(f'q: {e}', file=sys.stderr)
        sys.exit(2)
