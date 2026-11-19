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

Built by a scan of every generated .cpp (350 files, <= 4 workers at below-normal priority, RAM-capped) into out\harness\index\vcall.db (game-derived: stays in
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
VDB = os.path.join(IDX, 'vcall.db')
IMAGE = os.path.join(IDX, 'image.bin')
IMAGE_BASE = 0x82000000
NODE_REG = 0x82C30A10
VERSION = 2

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
    __slots__ = ('d', 'dflt', 'ctr')

    def __init__(self, d=None, dflt='in', ctr=None):
        self.d, self.dflt, self.ctr = d if d is not None else {}, dflt, ctr

    def get(self, r):
        v = self.d.get(r)
        if v is not None:
            return v
        return ('in', r) if self.dflt == 'in' else ('unk', 0, r)

    def copy(self):
        return State(dict(self.d), self.dflt, self.ctr)

    def same(self, o):
        return self.dflt == o.dflt and self.ctr == o.ctr and all(self.get(r) == o.get(r) for r in set(self.d) | set(o.d))


def meet(a, b, label=0):
    """the state where two paths join: a def survives only when both agree; otherwise it becomes ('phi', label, reg): one value per register per join, never
    equal to another register's unknown (so a vtable stored through r31 is only matched with the node when both name the same r31)"""
    if a is None:
        return b
    if b is None:
        return a
    dflt = a.dflt if a.dflt == b.dflt else 'unk'
    d = {}
    for r in set(a.d) | set(b.d):
        x, y = a.get(r), b.get(r)
        d[r] = x if x == y else ('phi', label, r)
    return State(d, dflt, a.ctr if a.ctr == b.ctr else None)


class Tracker:
    """definitions over one function. A definition is a hashable tuple: ('in', rN) value at entry; ('li', v); ('lis', v); ('const', addr); ('glob', addr) = a word loaded
    from a global; ('lwz', off, base_def); ('addi', base_def, off); ('ret', call_site, callee); ('unk', site). A call kills the volatile registers, except a call of a
    register save / restore helper (`__savegprlr_N`...), which kills only r0 / r11 / r12; the paths are met at labels (`run_function`)."""

    def __init__(self, vt_ok, helpers):
        self.s = State()
        self.vt_ok = vt_ok
        self.helpers = helpers
        self.objcls = {}      # def of an object -> vtable constant stored at +0 of it in this function
        self.stores = {}      # (def of base, offset) -> [vtable constants stored]

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
                        if src[0] == 'const' and self.vt_ok(src[1]):
                            self.stores.setdefault((bd, off), []).append(src[1])
                            if off == 0:
                                self.objcls[bd] = src[1]
                    if mn in UPDATE_STORES:
                        d[base] = ('unk', addr, base)
            return
        if mn in ('bl', 'bctrl'):
            tgt = _imm(p[0]) if (mn == 'bl' and p) else None
            hit('call', addr, (tgt, s))
            if mn == 'bctrl' and s.ctr is not None:
                hit('vcall', addr, (s.ctr, s.get('r3')))
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
        if mn == 'bctr':
            if s.ctr is not None:
                hit('vtail', addr, (s.ctr, s.get('r3')))
            return
        if mn == 'mtctr':
            s.ctr = s.get(p[0]) if p else None
            return
        if mn in NODEST or mn[0] == 'b' or mn.startswith('cmp') or mn.startswith('mt') or mn.startswith('fcmp'):
            return
        if not p or not REG_RE.match(p[0]):
            return
        rd = p[0]
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


def run_function(insns, vt_ok, helpers, hit):
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
        tr = Tracker(vt_ok, helpers)
        events = []
        pend = {}
        back_out = {}
        cur = State()
        reach = True
        for i, (a, mn, ops, cases) in enumerate(insns):
            if a in targets:
                inc = cur if reach else None
                for src in pend.get(a, []) + back_in.get(a, []):
                    inc = meet(inc, src, a)
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
                    st = cur.copy()
                    st.ctr = None
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


def _init_worker(funcs, vts, helpers):
    _W['funcs'], _W['vts'], _W['helpers'], _W['img'] = funcs, vts, helpers, Img()
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


def scan_file(path):
    """-> (vcall rows, node rows) of one generated .cpp"""
    with open(path, encoding='utf-8', errors='replace') as f:
        text = f.read()
    if 'bctr' not in text and 'bl 0x82c30a10' not in text:
        return [], []
    vrows, nrows = [], []
    lines = text.split('\n')
    n = len(lines)
    i = 0
    while i < n:
        m = DEF_RE.match(lines[i])
        if not m:
            i += 1
            continue
        fa = int(m.group(1), 16)
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
        i = j + 1
        if any(x[1] in ('bctrl', 'bctr') or (x[1] == 'bl' and x[2].lower() == '0x82c30a10') for x in insns):
            _scan_func(fa, insns, vrows, nrows)
    return vrows, nrows


def _scan_func(fa, insns, vrows, nrows):
    node_hits = []
    vhits = []

    def hit(kind, a, info, tr):
        if kind == 'call':
            tgt, st = info
            if tgt == NODE_REG:
                node_hits.append((a, st.get('r3'), st.get('r4'), st.get('r5'), tr))
            return
        c, r3d = info
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

    run_function(insns, _vt_ok, _W['helpers'], hit)
    vrows.extend(vhits)
    for (a, rd3, rd4, rd5, tr) in node_hits:
        cat = rd4[1] if rd4[0] == 'li' else -1
        on = rd5[1] if rd5[0] == 'li' else -1
        base, off = (rd3[1], rd3[2]) if rd3[0] == 'addi' else (rd3, 0)
        vts = tr.stores.get((base, off)) or []      # the function's LAST such store (a derived ctor sets its vtable after the base one registers)
        nrows.append((a, fa, cat, '' if cat >= 0 else describe(rd4), on, describe(rd3), vts[-1] if vts else 0))


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
    p = os.path.join(IDX, 'index.db')
    if not os.path.exists(p):
        raise XError(f'the code index {p} is missing: py tools\\q\\build_index.py')
    c = sqlite3.connect(f'file:{p}?mode=ro', uri=True)
    c.row_factory = sqlite3.Row
    return c


def build(path=None, gen=None, jobs=None, quiet=False, files=None, index_con=None):
    """scan every generated recomp .cpp into vcall.db (atomic: temp file, then replace)"""
    from concurrent.futures import ProcessPoolExecutor
    path = path or VDB
    gen = gen or GEN
    t0 = time.time()
    con = index_con or _index_con()
    funcs = frozenset(r[0] for r in con.execute('SELECT addr FROM funcs'))
    vts = frozenset(r[0] for r in con.execute('SELECT vt FROM rtti_vtable'))
    helpers = frozenset(r[0] for r in con.execute("SELECT addr FROM funcs WHERE name GLOB '__save*' OR name GLOB '__rest*'"))      # register save / restore helpers: a call of one kills only r0 / r11 / r12
    files = files or sorted(glob.glob(os.path.join(gen, 'wwe2k14_recomp.*.cpp')))
    if not files:
        raise XError(f'no generated .cpp under {gen} (generated\\ is local only)')
    tmp = f'{path}.{os.getpid()}.tmp'
    if os.path.exists(tmp):
        os.remove(tmp)
    out = sqlite3.connect(tmp)
    out.executescript('''CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE vcall(off INTEGER, site INTEGER, func INTEGER, kind TEXT, via_vt INTEGER, recv TEXT, cls INTEGER, this_recv INTEGER, r3ok INTEGER);
        CREATE TABLE nodes(site INTEGER, func INTEGER, cat INTEGER, cat_src TEXT, on_flag INTEGER, node TEXT, vt INTEGER);''')
    jobs = jobs or workers()
    with ProcessPoolExecutor(max_workers=jobs, initializer=_init_worker, initargs=(funcs, vts, helpers)) as ex:
        for vrows, nrows in ex.map(scan_file, files, chunksize=4):
            out.executemany('INSERT INTO vcall VALUES (?,?,?,?,?,?,?,?,?)', vrows)
            out.executemany('INSERT INTO nodes VALUES (?,?,?,?,?,?,?)', nrows)
    out.executescript('CREATE INDEX vcall_off ON vcall(off); CREATE INDEX nodes_cat ON nodes(cat);')
    nv = out.execute('SELECT COUNT(*) FROM vcall').fetchone()[0]
    nn = out.execute('SELECT COUNT(*) FROM nodes').fetchone()[0]
    out.executemany('INSERT INTO meta VALUES (?,?)', [('version', str(VERSION)), ('fingerprint', _fingerprint()), ('built', time.strftime('%Y-%m-%d %H:%M')),
                                                       ('seconds', f'{time.time() - t0:.1f}'), ('jobs', str(jobs)), ('files', str(len(files)))])
    out.commit()
    out.close()
    for attempt in range(8):      # a reader holding the old file open (Windows) refuses the replace for a moment
        try:
            os.replace(tmp, path)
            break
        except PermissionError:
            if attempt == 7:
                raise
            time.sleep(0.5)
    if not quiet:
        print(f'q: built {path}: {nv} virtual-call rows, {nn} node registrations, {len(files)} files, {jobs} workers, {time.time() - t0:.1f} s', file=sys.stderr)
    return path


def open_vdb(path=None):
    path = path or VDB
    stale = True
    if os.path.exists(path):
        try:
            c = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
            meta = dict(c.execute('SELECT key, value FROM meta').fetchall())
            c.close()
            stale = meta.get('version') != str(VERSION) or (path == VDB and meta.get('fingerprint') != _fingerprint() and os.path.isdir(GEN))
        except sqlite3.Error:
            stale = True
    if stale:
        build(path)
    c = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    c.row_factory = sqlite3.Row
    return c


# ------------------------------------------------------------------ queries
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
    if a.category is not None:
        q, pr = q + ' WHERE cat = ?', (parse_num(a.category),)
    res = []
    for r in v.execute(q + ' ORDER BY cat, site', pr).fetchall():
        vt = r['vt']
        s1 = img.word(vt + 4 * a.slot) if vt else None
        if s1 is not None and icon.execute('SELECT 1 FROM funcs WHERE addr = ?', (s1,)).fetchone() is None:
            s1 = None      # past the end of that table: not a function
        res.append({'category': r['cat'] if r['cat'] >= 0 else None, 'category_source': r['cat_src'] or None, 'site': hx(r['site']), 'func': f"sub_{r['func']:08X}",
                    'func_name': names.get(r['func'], ''), 'node': r['node'], 'vtable': hx(vt) if vt else None, 'class': cnames.get(vt) if vt else None,
                    'slot1': f'sub_{s1:08X}' if s1 else None, 'slot1_name': names.get(s1, '') if s1 else ''})
    if a.json:
        print(json.dumps({'count': len(res), 'nodes': res}, indent=1))
        return 0
    print(f'q nodes{" " + a.category if a.category else ""}: {len(res)} registration(s) of sub_82C30A10 (node, category, on)')
    for x in res[:a.limit]:
        cat = x['category'] if x['category'] is not None else f"? ({x['category_source']})"
        print(f"  cat {str(cat):>10}  {x['site']}  {x['func']:13s} {x['func_name'][:26]:26s} node {x['node'][:22]:22s} vt {x['vtable'] or '?':10s} slot{a.slot} {x['slot1'] or '?':13s} {x['slot1_name'][:28]}"
              + (f"  [{x['class']}]" if x['class'] else ''))
    if len(res) > a.limit:
        print(f'  ... {len(res) - a.limit} more (--limit N)')
    print("  (category = the `li r4,N` before the call; vtable = the constant stored at +0 of the node in the SAME function, `?` = a base constructor in another function sets it; slotN = the image word at vtable+4*N, N = --slot, default 1)")
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
