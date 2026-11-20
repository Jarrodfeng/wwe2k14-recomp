r"""Build the FIELD INDEX (out/harness/index/fields.s<SCHEMA>.db): every load / store with base register + displacement in the
recompiled game, with the base register's provenance folded mechanically (S103 lane QF; query side: tools/q/qfield.py,
`q field`). Sources: generated/default/wwe2k14_recomp.*.cpp (PPC instruction comments + `goto loc_X;` lines, never
committed), out/harness/index/image.bin (vtable slots). Output is game-derived metadata: gitignored folder, never commit.

    py tools/q/build_fields.py [--root DIR] [--gen DIR] [--image FILE] [--db FILE] [-j N]

Method (verify against the listing; this is a mechanical fold, not decompilation):
  - Per function, a forward dataflow over basic blocks (leaders = labels, entry, the instruction after any branch with a
    goto). At a join a register's value survives only if every reaching predecessor agrees (like build_index's lis
    analysis); back edges are iterated to a fixed point (a function with no back edge is one pass).
  - A register holds (root, delta). Roots: 'a0'..'a7' (argument registers r3..r10 at entry), 'K' (a constant built by
    li / lis / addi / ori; delta = the value), 'g:ADDR' (a pointer loaded from the global word at ADDR: lis pair + lwz),
    'p(ROOT,D)' (a pointer loaded from ROOT+D, depth <= 3), 'r:SITE' (the return value of the call at SITE), 's' (the
    stack pointer; stack ops are not indexed). addi / mr carry the root and change the delta. A call kills r0 and r3-r12
    (the __savegprlr_ / __restgprlr_ helper calls do not). Anything else a register is written by kills its value.
  - Row = (func, addr, R|W|A, width, float?, off, root). off = delta + displacement (root 'K': the absolute address).
    A = `addi rD,rS,imm` handing on a field's address (a sub-object passed to a callee). Indexed forms (lwzx...) are not
    indexed (no displacement).
  - Object TYPE = the vtable address V stored (as a lis-pair constant) at offset 0 of a root: in a constructor, a0 gets
    V (also the ctors table: function -> last V it stores at a0+0); a call of a constructor gives its r3 root, and the
    returned r:SITE, the constructor's V (the constructor returns this); a function that is a slot of a vtable V (words
    from V on that are function starts, V = a stored vptr value) gets V for a0 (functions in more than 3 vtables are not
    typed: shared stubs). A root made only of globals (g:..., p(g:..., d)) carries its type to every function.
    Limits: a subobject vptr at a non-zero offset is not a type; a type is not passed down to callees (`how` column).
  - Not modelled: indexed accesses, register-indirect copies through memory, tail calls' arguments, switch-table state
    beyond the generated gotos, values the compiler spilled to the stack.
"""
import argparse
import glob
import multiprocessing
import os
import re
import sqlite3
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
SCHEMA = 2
IMAGE_BASE = 0x82000000
M32 = 0xFFFFFFFF

TOKEN_RE = re.compile(r'^(?:[ \t]*// ([^\n]*)|[ \t]*loc_([0-9A-F]{8}):|(DEFINE_REX_FUNC\(sub_[0-9A-Fa-f]{8}\))'
                      r'|[ \t]*(__(?:save|rest)\w+)\(ctx|[^\n/]*?\bgoto loc_([0-9A-F]{8});)', re.M)
MEM_RE = re.compile(r'^(lbz|lhz|lha|lwz|lwa|ld|lfs|lfd|stb|sth|stw|std|stfs|stfd)(u?)\s+([rf]\d+),\s*(-?(?:0x)?[0-9A-Fa-f]+)\((r\d+)\)\s*$')
WIDTH = {'lbz': 1, 'lhz': 2, 'lha': 2, 'lwz': 4, 'lwa': 4, 'ld': 8, 'lfs': 4, 'lfd': 8, 'stb': 1, 'sth': 2, 'stw': 4,
         'std': 8, 'stfs': 4, 'stfd': 8}
NODEST = ('st', 'cmp', 'b', 'mt', 'tw', 'td', 'dcb', 'icb', 'sync', 'lwsync', 'eieio', 'isync', 'nop', 'cr', 'db16cyc',
          'cctp', 'fcmp', 'vcmp', 'ptesync', 'sc')
ENDERS = ('b', 'blr', 'bctr')
MAXDEPTH = 3

_IMG = None   # worker global: (bytes, base)


def gen_dir(root=None):
    """The generated/default folder: under ROOT when it holds the recompiled sources, else the main checkout's."""
    cands = []
    if os.environ.get('WWE_GENERATED'):
        cands.append(os.environ['WWE_GENERATED'])
    for r in ([root] if root else []) + [REPO]:
        cands.append(os.path.join(r, 'generated', 'default'))
    try:
        common = subprocess.run(['git', '-C', REPO, 'rev-parse', '--git-common-dir'], capture_output=True, text=True).stdout.strip()
        cands.append(os.path.join(os.path.dirname(os.path.abspath(os.path.join(REPO, common))), 'generated', 'default'))
    except OSError:
        pass
    for c in cands:
        if glob.glob(os.path.join(c, 'wwe2k14_recomp.*.cpp')):
            return c
    raise SystemExit('q field: generated/default not found (set WWE_GENERATED or pass --gen)')


def data_root(gen):
    """The checkout that owns `gen` (generated/default -> its root): its out/harness/index holds image.bin."""
    return os.path.normpath(os.path.join(gen, '..', '..'))


def db_name():
    """S105 D (F1): the index file's name carries the schema (fields.s2.db): a q of another schema uses ANOTHER file and never opens, wipes or rebuilds this one
    (the design of qasset.db_name). The next SCHEMA bump gets a new name by itself."""
    return 'fields.s%d.db' % SCHEMA


def other_index_files(db):
    """index files of OTHER names beside ours (the old fields.db, fields.s1.db ...): left alone by q, named in `q field info` / the build line so the tidy / a person can clear them"""
    d = os.path.dirname(os.path.abspath(db))
    me = os.path.basename(db)
    try:
        return sorted(f for f in os.listdir(d) if re.match(r'^fields(\.s\d+)?\.db$', f) and f != me)
    except OSError:
        return []


def default_paths(root=None, gen=None):
    """(generated/default, image.bin, fields.s<SCHEMA>.db); WWE_QFIELD_DB=<file> = another index file (how a builder / the selftest tests without touching the shared index)"""
    gen = gen or gen_dir(root)
    d = os.path.join(data_root(gen), 'out', 'harness', 'index')
    return gen, os.path.join(d, 'image.bin'), os.environ.get('WWE_QFIELD_DB') or os.path.join(d, db_name())


# ----------------------------------------------------------------------------------------------------------------------- the rebuild lock (S105 D, F2)
class Busy(Exception):
    """another process holds the rebuild lock; .holder = its pid text"""
    def __init__(self, holder):
        super().__init__(holder)
        self.holder = holder


def lock_stale_s():
    try:
        return float(os.environ.get('WWE_QFIELD_LOCK_STALE', '1200'))
    except ValueError:
        return 1200.0


def pid_alive(pid):
    """True when a process with this pid exists (Windows: OpenProcess + exit code; elsewhere signal 0; the design of qasset.pid_alive)"""
    if pid <= 0:
        return False
    try:
        if os.name == 'nt':
            import ctypes
            h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)       # PROCESS_QUERY_LIMITED_INFORMATION
            if not h:
                return False
            code = ctypes.c_ulong()
            ok = ctypes.windll.kernel32.GetExitCodeProcess(h, ctypes.byref(code))
            ctypes.windll.kernel32.CloseHandle(h)
            return bool(ok) and code.value == 259                              # STILL_ACTIVE
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except OSError:
        return False


def _read_pid(lk):
    try:
        with open(lk) as f:
            return f.read().strip()
    except OSError:
        return None


def lock_is_stale(lk):
    """S105 D (F-1): a lock is a dead builder's when its pid is gone (a killed q never runs its `finally`: taken over at once); a live holder that has not touched the lock
    (a heartbeat per scanned batch) for WWE_QFIELD_LOCK_STALE s (default 20 min) is a hung one"""
    txt = _read_pid(lk)
    try:
        age = time.time() - os.path.getmtime(lk)
    except OSError:
        return False
    try:
        pid = int(txt or 0)
    except ValueError:
        pid = 0
    return (not pid_alive(pid)) or age > lock_stale_s()


def touch_lock(lk):
    if lk:
        try:
            os.utime(lk, None)
        except OSError:
            pass


def acquire_lock(db):
    """one rebuilder at a time (db.lock, created exclusively, holding the pid; the builder touches it as a heartbeat). A dead holder's lock (or a live one idle past WWE_QFIELD_LOCK_STALE)
    is taken over, the old holder's pid printed. Raises Busy(holder) when a live builder holds it."""
    lk = db + '.lock'
    os.makedirs(os.path.dirname(os.path.abspath(db)), exist_ok=True)
    for _ in range(4):
        try:
            fd = os.open(lk, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, str(os.getpid()).encode())
            os.close(fd)
            return lk
        except FileExistsError:
            holder = _read_pid(lk)
            if holder is None:
                continue      # released in between: try again
            if not lock_is_stale(lk):
                raise Busy(holder or '?')
            print(f'q field: taking over a stale rebuild lock (pid {holder}: gone, or idle over {int(lock_stale_s())} s)', file=sys.stderr)
            mine = f'{lk}.stale.{os.getpid()}'
            try:
                os.rename(lk, mine)      # atomic: of several takers only one wins the rename
            except OSError:
                continue
            if _read_pid(mine) != holder:      # we renamed a FRESH lock a faster taker had just made: put it back and stand aside
                try:
                    os.rename(mine, lk)
                except OSError:
                    pass
                raise Busy(_read_pid(lk) or '?')
            try:
                os.remove(mine)
            except OSError:
                pass
    raise Busy('?')


def release_lock(lk):
    for _ in range(5):      # a Windows sharing violation can refuse the remove for a moment
        try:
            with open(lk) as f:
                mine = f.read().strip() == str(os.getpid())
            if mine:
                os.remove(lk)
            return
        except FileNotFoundError:
            return
        except OSError:
            time.sleep(0.2)


def signature(gen, image):
    parts = [f'v{SCHEMA}']
    for p in sorted(glob.glob(os.path.join(gen, 'wwe2k14_recomp.*.cpp'))):
        st = os.stat(p)
        parts.append(f'{os.path.basename(p)}:{st.st_size}:{int(st.st_mtime)}')
    try:
        st = os.stat(image)
        parts.append(f'image:{st.st_size}:{int(st.st_mtime)}')
    except OSError:
        parts.append('image:none')
    return ';'.join(parts)


def num(s):
    return int(s, 0)


def regn(tok):
    tok = tok.strip()
    return int(tok[1:]) if len(tok) > 1 and tok[0] == 'r' and tok[1:].isdigit() else None


def parse_function(items, start=0):
    """items: tokens after DEFINE_REX_FUNC: ('c', text) ('l', addr) ('h',) helper call ('g', target). Returns
    (instrs [[addr, mn, ops, gotos, helper]], labels addr->index)."""
    instrs, labels = [], {}
    address = start
    for it in items:
        k = it[0]
        if k == 'c':
            text = it[1].strip()
            parts = text.split(None, 1)
            instrs.append([address, parts[0].rstrip('.') if parts else '', parts[1] if len(parts) > 1 else '', [], False])
            address += 4
        elif k == 'l':
            labels[it[1]] = len(instrs)
            address = it[1]
        elif k == 'h' and instrs:
            instrs[-1][4] = True
        elif k == 'g' and instrs:
            instrs[-1][3].append(it[1])
    return instrs, labels


def root_str(rt):
    return rt


def meet(a, b):
    if a is None:
        return b
    return {r: v for r, v in a.items() if b.get(r) == v}


def step(st, ins, out, func):
    """Apply one instruction to the register state `st`; append facts to `out` (rows, vstores, calls) when out is not None."""
    addr, mn, ops, gotos, helper = ins
    if mn.startswith(('l', 's')) and (MEM_RE.match(mn + ' ' + ops)):
        m = MEM_RE.match(mn + ' ' + ops)
        bmn, upd, reg, disp, base = m.groups()
        disp = num(disp)
        b = int(base[1:])
        store = bmn[0] == 's'
        bs = st.get(b)
        if out is not None and b != 1 and not (bs and bs[0] == 's'):
            if bs is None:
                root, off = 'u', disp
            else:
                root, off = bs[0], bs[1] + disp
                if root == 'K':
                    off &= M32
            fl = 1 if bmn in ('lfs', 'lfd', 'stfs', 'stfd') else 0
            out[0].append((func, addr, 'W' if store else 'R', WIDTH[bmn], fl, off, root))
            if store and bmn == 'stw' and bs is not None and root not in ('K', 's') and off == 0:
                v = st.get(int(reg[1:]))
                if v is not None and v[0] == 'K' and v[1] >= IMAGE_BASE and v[1] % 4 == 0:
                    out[1].append((func, root, v[1]))
        if not store:
            d = regn(reg)
            if d is not None:
                nv = None
                if bmn == 'lwz' and bs is not None and b != 1:
                    if bs[0] == 'K':
                        nv = ('g:%08X' % ((bs[1] + disp) & M32), 0)
                    elif bs[0] not in ('s',) and bs[0].count('p(') < MAXDEPTH:
                        nv = ('p(%s,%d)' % (bs[0], bs[1] + disp), 0)
                if nv is not None:
                    st[d] = nv
                else:
                    st.pop(d, None)
        if upd and b != 1:
            if bs is not None:
                st[b] = (bs[0], bs[1] + disp)
            else:
                st.pop(b, None)
        return
    if mn in ('bl', 'bctrl', 'blrl'):
        if helper:
            return
        if out is not None:
            tgt = None
            if mn == 'bl':
                try:
                    tgt = int(ops.strip(), 16)
                except ValueError:
                    tgt = None
            args = []
            for ri in range(3, 11):
                v = st.get(ri)
                if v is not None and v[1] == 0 and v[0] not in ('s', 'K'):
                    args.append((ri - 3, v[0]))
            if tgt is not None and args:
                out[2].append((func, addr, tgt, args))
        for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
            st.pop(r, None)
        st[3] = ('r:%08X' % addr, 0)
        return
    if mn.startswith(NODEST):
        return
    o = ops.split(',')
    d = regn(o[0]) if o else None
    if d is None:
        return
    if mn == 'li' and len(o) == 2:
        try:
            st[d] = ('K', num(o[1]) & M32)
        except ValueError:
            st.pop(d, None)
    elif mn == 'lis' and len(o) == 2:
        try:
            st[d] = ('K', (num(o[1]) << 16) & M32)
        except ValueError:
            st.pop(d, None)
    elif mn in ('addi', 'addis') and len(o) == 3:
        s = regn(o[1])
        try:
            imm = num(o[2]) << (16 if mn == 'addis' else 0)
        except ValueError:
            st.pop(d, None)
            return
        sv = st.get(s) if s is not None else None
        if s == 0:
            st[d] = ('K', imm & M32)
        elif sv is not None:
            if sv[0] == 'K':
                st[d] = ('K', (sv[1] + imm) & M32)
            else:
                st[d] = (sv[0], sv[1] + imm)
                if out is not None and sv[0] != 's' and mn == 'addi':
                    out[0].append((func, addr, 'A', 0, 0, sv[1] + imm, sv[0]))
        else:
            st.pop(d, None)
    elif mn in ('ori', 'oris') and len(o) == 3:
        s = regn(o[1])
        sv = st.get(s) if s is not None else None
        try:
            imm = num(o[2]) << (16 if mn == 'oris' else 0)
        except ValueError:
            sv = None
        if sv is not None and sv[0] == 'K':
            st[d] = ('K', (sv[1] | imm) & M32)
        else:
            st.pop(d, None)
    elif mn == 'mr' and len(o) == 2:
        s = regn(o[1])
        sv = st.get(s) if s is not None else None
        if sv is not None:
            st[d] = sv
        else:
            st.pop(d, None)
    else:
        st.pop(d, None)


def analyze(func, items):
    """-> (rows, vstores, calls) for one function."""
    instrs, labels = parse_function(items, func)
    n = len(instrs)
    out = ([], [], [])
    if n == 0:
        return out
    init = {3 + i: ('a%d' % i, 0) for i in range(8)}
    init[1] = ('s', 0)
    leaders = {0}
    back = False
    for i, ins in enumerate(instrs):
        if ins[3] or ins[1] in ENDERS:
            leaders.add(i + 1)
        for t in ins[3]:
            ti = labels.get(t)
            if ti is not None and ti <= i:
                back = True
    for i in labels.values():
        leaders.add(i)
    starts = sorted(x for x in leaders if x < n)
    bidx = {s: j for j, s in enumerate(starts)}
    ends = starts[1:] + [n]

    def succs(j):
        last = instrs[ends[j] - 1]
        res = []
        if last[1] not in ENDERS and ends[j] < n:
            res.append(bidx[ends[j]])
        for t in last[3]:
            ti = labels.get(t)
            if ti is not None and ti in bidx:
                res.append(bidx[ti])
        return res

    def run(j, st, emit):
        for k in range(starts[j], ends[j]):
            step(st, instrs[k], out if emit else None, func)
        return st

    if not back:
        ins_ = [None] * len(starts)
        ins_[0] = init
        for j in range(len(starts)):
            st = ins_[j]
            st = dict(st) if st is not None else {}
            run(j, st, True)
            for s in succs(j):
                ins_[s] = meet(ins_[s], st)
        return out
    ins_ = [None] * len(starts)
    ins_[0] = init
    work = [0]
    inq = {0}
    guard = 0
    while work and guard < 20 * len(starts) + 50:
        guard += 1
        j = min(work)
        work.remove(j)
        inq.discard(j)
        st = run(j, dict(ins_[j]), False)
        for s in succs(j):
            new = meet(ins_[s], st)
            if new != ins_[s]:
                ins_[s] = new
                if s not in inq:
                    work.append(s)
                    inq.add(s)
    for j in range(len(starts)):
        run(j, dict(ins_[j]) if ins_[j] is not None else {}, True)
    return out


def scan_file(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    rows, vst, calls, funcs = [], [], [], []
    cur, items = None, []

    def flush():
        if cur is not None:
            r, v, c = analyze(cur, items)
            rows.extend(r); vst.extend(v); calls.extend(c)
            funcs.append(cur)

    for m in TOKEN_RE.finditer(text):
        c, label, fn, helper, goto = m.groups()
        if fn:
            flush()
            cur = int(fn[len('DEFINE_REX_FUNC(sub_'):-1], 16)
            items = []
        elif cur is not None:
            if label:
                items.append(('l', int(label, 16)))
            elif c is not None:
                items.append(('c', c))
            elif helper:
                items.append(('h',))
            elif goto:
                items.append(('g', int(goto, 16)))
    flush()
    return rows, vst, calls, funcs


def _scan_one(path):
    try:
        return scan_file(path)
    except Exception as e:   # a bug here must be loud, not a silently short index
        raise RuntimeError(f'{path}: {e!r}')


def build(gen, image, db_path, jobs=None, quiet=False, say=None, if_stale=False):
    """Build db_path under the rebuild lock (raises Busy when another process is building). say = a line printed once the lock is held; if_stale = after taking the lock,
    return False (nothing built) when the index already carries the current signature (another builder finished while we waited)."""
    files = sorted(glob.glob(os.path.join(gen, 'wwe2k14_recomp.*.cpp')))
    if not files:
        raise SystemExit(f'build_fields: no wwe2k14_recomp.*.cpp in {gen}')
    lk = acquire_lock(db_path)
    try:
        if if_stale and current_signature(db_path) == signature(gen, image):
            return False
        if say:
            other = other_index_files(db_path)
            print(say + (f' (older index files left alone: {", ".join(other)})' if other else ''), file=sys.stderr)
        _build_locked(gen, image, db_path, jobs, quiet, files, lk)
        return True
    finally:
        release_lock(lk)


def current_signature(db):
    if not os.path.exists(db):
        return None
    try:
        c = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
        try:
            r = c.execute("select v from meta where k='signature'").fetchone()
            return r[0] if r else None
        finally:
            c.close()
    except sqlite3.Error:
        return None


def _sweep_tmps(db_path):
    """leftovers of crashed builds (we hold the lock, so only a dead builder's): older than an hour, `.tmp` and sqlite's `-journal` / `-wal` sidecars, and the pre-S105 fixed `<db>.tmp`"""
    for old in glob.glob(glob.escape(db_path) + '.*.tmp*') + glob.glob(glob.escape(db_path) + '.tmp*'):
        try:
            if time.time() - os.path.getmtime(old) > 3600:
                os.remove(old)
        except OSError:
            pass


def _swap_in(tmp, db_path, quiet):
    """os.replace with a ~30 s backoff (a reader holding the old file open refuses it on Windows). -> True when swapped; False (the temp deleted by the caller) when a reader still holds it"""
    delay, waited = 0.25, 0.0
    while True:
        try:
            os.replace(tmp, db_path)
            return True
        except PermissionError:
            if waited >= float(os.environ.get('WWE_QFIELD_SWAP_WAIT', '30')):
                return False
            time.sleep(delay)
            waited += delay
            delay = min(delay * 2, 4.0)


def _build_locked(gen, image, db_path, jobs, quiet, files, lk=None):
    t0 = time.time()
    hold = float(os.environ.get('WWE_QFIELD_BUILD_HOLD', '0') or 0)
    if hold:
        time.sleep(hold)      # test hook (qfield_selftest): the lock holder lingers so a second caller meets the lock
        touch_lock(lk)
    os.makedirs(os.path.dirname(os.path.abspath(db_path)), exist_ok=True)
    _sweep_tmps(db_path)
    tmp = f'{db_path}.{os.getpid()}.tmp'
    try:
        if os.path.exists(tmp):
            os.remove(tmp)
        nf, nrows, nroots, t_scan = _build_body(gen, image, tmp, jobs, files, lk, t0)
        if _swap_in(tmp, db_path, quiet):
            if not quiet:
                print(f'built {db_path}: {nf} functions, {nrows} rows, {nroots} roots, scan {t_scan:.0f}s, '
                      f'total {time.time() - t0:.0f}s, {os.path.getsize(db_path) / 1e6:.0f} MB', file=sys.stderr)
        else:
            print(f'q field: a reader holds the index: the new one was not swapped in, answering from the old one ({db_path}); the next call rebuilds', file=sys.stderr)
    finally:
        for x in (tmp, tmp + '-journal', tmp + '-wal', tmp + '-shm'):
            try:
                os.remove(x)
            except OSError:
                pass


def _build_body(gen, image, tmp, jobs, files, lk, t0):
    con = sqlite3.connect(tmp)
    con.executescript('''
      create table roots(id integer primary key, text text unique, glob integer);
      create table rows(func integer, addr integer, rw text, width integer, fl integer, off integer, root integer);
      create table calls(func integer, site integer, callee integer, argi integer, root integer);
      create table vstores(func integer, root integer, vt integer);
      create table meta(k text primary key, v text);
    ''')
    rootid = {}

    def rid(text):
        i = rootid.get(text)
        if i is None:
            i = len(rootid) + 1
            rootid[text] = i
        return i

    funcs = set()
    nrows = 0
    ctx = multiprocessing.get_context('spawn')
    with ctx.Pool(jobs or min(16, os.cpu_count() or 4)) as pool:
        for rows, vst, calls, fl in pool.imap_unordered(_scan_one, files):
            con.executemany('insert into rows values (?,?,?,?,?,?,?)',
                            [(f, a, rw, w, fp, off, rid(root)) for f, a, rw, w, fp, off, root in rows])
            con.executemany('insert into vstores values (?,?,?)', [(f, rid(r), v) for f, r, v in vst])
            con.executemany('insert into calls values (?,?,?,?,?)',
                            [(f, s, t, ai, rid(r)) for f, s, t, args in calls for ai, r in args])
            funcs.update(fl)
            nrows += len(rows)
            touch_lock(lk)      # heartbeat: a live builder's lock never looks idle
    for text, i in rootid.items():
        m = re.match(r'^g:([0-9A-F]{8})$', text)
        con.execute('insert into roots values (?,?,?)', (i, text, int(m.group(1), 16) if m else None))
    t_scan = time.time() - t0
    build_types(con, funcs, image, rootid)
    con.execute('create index i_off on rows(off, rw)')
    con.execute('create index i_func on rows(func, addr)')
    con.execute('create index i_root on rows(root)')
    con.execute('create index i_ot on objtype(vt)')
    for k, v in (('signature', signature(gen, image)), ('functions', len(funcs)), ('rows', nrows), ('roots', len(rootid)),
                 ('built_at', time.strftime('%Y-%m-%d %H:%M:%S')), ('gen_dir', gen), ('image', image)):
        con.execute('insert into meta values (?,?)', (k, str(v)))
    con.execute('drop table calls')
    con.commit()
    con.execute('vacuum')
    con.close()
    return len(funcs), nrows, len(rootid), t_scan


def build_types(con, funcs, image, rootid):
    """Fill vtables / slots / ctors / objtype from the stored vptrs, the vtable slot words and the constructor calls."""
    con.executescript('''
      create table vtables(vt integer primary key, nslots integer, nstores integer);
      create table slots(vt integer, idx integer, func integer);
      create table ctors(func integer primary key, vt integer);
      create table objtype(func integer, root integer, vt integer, how text, primary key(func, root, vt));
    ''')
    img = open(image, 'rb').read() if os.path.exists(image) else b''

    def word(a):
        o = a - IMAGE_BASE
        if o < 0 or o + 4 > len(img):
            return None
        return struct.unpack_from('>I', img, o)[0]

    vts = {}
    for vt, cnt in con.execute('select vt, count(*) from vstores group by vt'):
        # a vtable: its first word is a function start (an address stored at +0 that is not a table of code pointers is not a type)
        w = word(vt)
        if w is not None and w in funcs:
            vts[vt] = cnt
    for vt, cnt in vts.items():
        k = 0
        while k < 512:
            w = word(vt + 4 * k)
            if w is None or w not in funcs:
                break
            con.execute('insert into slots values (?,?,?)', (vt, k, w))
            k += 1
        con.execute('insert into vtables values (?,?,?)', (vt, k, cnt))
    # constructors: a function storing a vptr at a0+0 (the last one stored wins: the most derived)
    a0 = rootid.get('a0')
    if a0 is not None:
        last = {}
        for f, vt in con.execute('select s.func, s.vt from vstores s where s.root=? order by s.rowid', (a0,)):
            if vt in vts:
                last[f] = vt
        con.executemany('insert into ctors values (?,?)', list(last.items()))
    # types
    for f, r, vt in con.execute('select func, root, vt from vstores').fetchall():
        if vt in vts:
            con.execute('insert or ignore into objtype values (?,?,?,?)', (f, r, vt, 'vptr'))
    if a0 is not None:
        shared = {}
        for vt, idx, f in con.execute('select vt, idx, func from slots'):
            shared.setdefault(f, []).append(vt)
        for f, lst in shared.items():
            if len(lst) <= 3:
                for vt in lst:
                    con.execute('insert or ignore into objtype values (?,?,?,?)', (f, a0, vt, 'slot'))
    # a constructor call types its r3 root and the returned r:SITE (the constructor returns this)
    ret = {}
    for f, site, callee, root in con.execute('select func, site, callee, root from calls where argi=0').fetchall():
        c = con.execute('select vt from ctors where func=?', (callee,)).fetchone()
        if not c:
            continue
        con.execute('insert or ignore into objtype values (?,?,?,?)', (f, root, c[0], 'ctor_call'))
        r = rootid.get('r:%08X' % site)
        if r is not None:
            con.execute('insert or ignore into objtype values (?,?,?,?)', (f, r, c[0], 'ctor_ret'))
    # roots made only of globals carry their type to every function (func = 0 means "all functions")
    gre = re.compile(r'^(?:p\()*g:[0-9A-F]{8}(?:,-?\d+\))*$')
    glob_roots = {i for t, i in rootid.items() if gre.match(t)}
    # arguments flow down one call edge at a time (3 rounds): a callee's aN gets the global-only roots and the types its
    # callers pass in aN. Recorded apart (argflow, how='callarg'): inferred from callers, never a proof for one call site.
    con.execute('create table argflow(func integer, argroot integer, groot integer, n integer)')
    argroot = {i: rootid.get('a%d' % i) for i in range(8)}
    typed = {}
    for f, r, vt, how in con.execute('select func, root, vt, how from objtype').fetchall():
        typed.setdefault((0 if r in glob_roots else f, r), set()).add(vt)
    edges = con.execute('select func, callee, argi, root from calls').fetchall()
    flow = {}      # (func, argi) -> {groot: n}
    for _round in range(3):
        new = {}
        newt = {}
        for f, callee, ai, r in edges:
            if r in glob_roots:
                new.setdefault((callee, ai), {}).setdefault(r, 0)
                new[(callee, ai)][r] += 1
            else:
                for j in range(8):
                    if r == argroot[j] and (f, j) in flow:
                        for g, n in flow[(f, j)].items():
                            new.setdefault((callee, ai), {}).setdefault(g, 0)
                            new[(callee, ai)][g] += n
            for vt in typed.get((f, r), set()) | typed.get((0, r), set()):
                newt.setdefault((callee, ai), set()).add(vt)
        flow = {k: dict(sorted(v.items(), key=lambda kv: -kv[1])[:6]) for k, v in new.items()}
        for (callee, ai), vs in newt.items():
            if argroot[ai] is None or len(vs) > 2:
                continue
            for vt in vs:
                if (callee, argroot[ai]) not in typed or vt not in typed[(callee, argroot[ai])]:
                    typed.setdefault((callee, argroot[ai]), set()).add(vt)
                    con.execute('insert or ignore into objtype values (?,?,?,?)', (callee, argroot[ai], vt, 'callarg'))
    for (callee, ai), d in flow.items():
        if argroot[ai] is None:
            continue
        for g, n in d.items():
            con.execute('insert into argflow values (?,?,?,?)', (callee, argroot[ai], g, n))
    con.execute('create index i_af on argflow(func, argroot)')
    con.execute('create index i_afg on argflow(groot)')
    for f, r, vt, how in con.execute('select func, root, vt, how from objtype').fetchall():
        if r in glob_roots:
            con.execute('insert or ignore into objtype values (?,?,?,?)', (0, r, vt, 'global'))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--root'); ap.add_argument('--gen'); ap.add_argument('--image'); ap.add_argument('--db')
    ap.add_argument('-j', type=int, default=None)
    a = ap.parse_args(argv)
    gen, image, db = default_paths(a.root, a.gen)
    try:
        build(gen, a.image or image, a.db or db, a.j)
    except Busy as e:
        print(f'build_fields: another process (pid {e.holder}) is rebuilding {a.db or db}: not building in parallel', file=sys.stderr)
        return 3
    return 0


if __name__ == '__main__':
    multiprocessing.freeze_support()
    sys.exit(main())
