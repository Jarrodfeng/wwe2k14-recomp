#!/usr/bin/env python3
"""S103 lane X1: the address lint. Every guest address constant (0x82xxxxxx, 0x83xxxxxx) and every named structure offset
(kSomethingOff / Offset / Stride = N) in the scene readers must be recomputable from the generated listing of the function
its comment cites. Two hand-typed misreads cost run 26 a whole input bit (lane C6): `lis 1 | ori 62056` typed as +62048 (ori ORs
the low half, it does not add a signed one) and `lis -31889` typed as 0x8376 instead of 0x836F. This is the same check, over all of them.

  py -I tools\\analysis\\address_lint.py [--root DIR] [--files F...] [--verbose] [--list-uncited]
Scans src\\scene_live, src\\renderer_scene, src\\renderer_adapter (not *_test.cpp). Reads generated\\ locally only; no listing text is written anywhere.
Exit 0 = no mismatch, 1 = at least one mismatch (a value the cited function does NOT give, with a near miss that explains the slip).

CITATION CONVENTION (light): a sub_XXXXXXXX name, or an instruction address 0x825xxxxx (resolved to its function), in the constant's own line,
within 3 lines of it, within 30 lines above it, or anywhere in the file (the "file-wide" tier). The nearest tier that gives the value wins.
RECOMPUTE: a small abstract interpreter over the listing's instruction comments: lis / li / addi / addis (sign-extended), ori / oris (OR),
mr, lwz/stw/lfs/stfs... displacements and effective addresses on a known base, mulli / slwi strides, cmpwi immediates; a derived offset is a
known constant plus a displacement seen in the same function (lis 1 | ori 62056, then -8).
RESULT per constant: verified | mismatch (not given by the cited function, near miss given: same low half under a different high half, an ori immediate
without its lis half, off by 65536) | notfound (cited, not found, no near miss) | uncited (no function named anywhere).
A reviewed false positive goes in tools\\analysis\\address_lint_ok.txt as `path|value|reason`."""
import argparse
import bisect
import json
import os
import re
import subprocess
import sys

M32 = 0xFFFFFFFF
DIRS = ('src/scene_live', 'src/renderer_scene', 'src/renderer_adapter')
SUB_RE = re.compile(r'sub_([0-9A-Fa-f]{8})')
INS_RE = re.compile(r'\b0x(82[0-9A-Fa-f]{6})\b')
ADDR_RE = re.compile(r'\b0x(8[23][0-9A-Fa-f]{6})[uU]?\b')
OFF_RE = re.compile(r'\b(k[A-Za-z0-9_]*(?:Off|Offset|Stride)[A-Za-z0-9_]*)\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?\b')
LOADS = ('lwz', 'lbz', 'lhz', 'lha', 'lfs', 'lfd', 'ld', 'lwa', 'lvx', 'lvlx', 'lvrx')
STORES = ('stw', 'stb', 'sth', 'stfs', 'stfd', 'std', 'stvx', 'stvlx', 'stvrx')


class Gen:
    """The generated listing, found by the partition map (function start -> file). Read-only."""

    def __init__(self, gdir):
        self.dir = gdir
        with open(os.path.join(gdir, 'codegen.partition.json'), encoding='utf-8') as f:
            asg = json.load(f)['assignments']
        self.file_of = {int(k, 16): v for k, v in asg.items()}
        self.starts = sorted(self.file_of)
        self.text_cache = {}
        self.fn_cache = {}

    def containing(self, addr):
        i = bisect.bisect_right(self.starts, addr) - 1
        return self.starts[i] if i >= 0 and addr - self.starts[i] < 0x40000 else None

    def end_of(self, start):
        i = bisect.bisect_right(self.starts, start)
        return self.starts[i] if i < len(self.starts) else start + 0x1000

    def body(self, start):
        if start in self.fn_cache:
            return self.fn_cache[start]
        r = None
        idx = self.file_of.get(start)
        if idx is not None:
            if idx not in self.text_cache:
                if len(self.text_cache) > 6:
                    self.text_cache.clear()
                with open(os.path.join(self.dir, 'wwe2k14_recomp.%d.cpp' % idx), encoding='utf-8', errors='replace') as f:
                    self.text_cache[idx] = f.read()
            t = self.text_cache[idx]
            i = t.find('DEFINE_REX_FUNC(sub_%08X)' % start)
            if i >= 0:
                j = t.find('DEFINE_REX_FUNC(', i + 10)
                r = t[i:j if j > 0 else len(t)]
        self.fn_cache[start] = r
        return r


class Facts:
    def __init__(self):
        self.abs = set()      # absolute values: lis/li/addi/ori results, effective addresses on a known base
        self.off = set()      # offset values: displacements, symbolic-base offsets (addis 1 / addi -4084), effective offsets
        self.disp = set()     # plain displacements / addi immediates (signed)
        self.stride = set()   # mulli immediates, 1 << slwi n
        self.callees = set()
        self.hexes = set()    # every 0x8xxxxxxx a branch / lr line names


INSN = re.compile(r'^\s*// ([a-z][a-z0-9_.+-]*)\s*(.*)$')
MEM = re.compile(r'^(?:r|f|v)\d+,\s*(-?\d+)\(r(\d+)\)$')


def interpret(body):
    f = Facts()
    sym = [0]
    st = {}

    def fresh():
        sym[0] += 1
        return ('s', sym[0], 0)

    def get(n):
        if n not in st:
            st[n] = fresh()
        return st[n]

    def put_abs(v):
        f.abs.add(v & M32)

    def note(val):
        if val[0] == 'k':
            put_abs(val[2])
            f.off.add(val[2] & M32)
        else:
            f.off.add(val[2])
            f.off.add(val[2] & M32)

    for m in re.finditer(r'0x(8[0-9a-f]{7})', body):
        f.hexes.add(int(m.group(1), 16))
    for ln in body.splitlines():
        m = INSN.match(ln)
        if not m:
            continue
        mn, ops = m.group(1), m.group(2).strip()
        a = [x.strip() for x in ops.split(',')] if ops else []
        reg = lambda s: int(s[1:]) if re.fullmatch(r'r\d+', s) else None
        try:
            if mn == 'lis' and len(a) == 2:
                v = ('k', 0, (int(a[1], 0) << 16) & M32)
                st[reg(a[0])] = v
                note(v)
            elif mn == 'li' and len(a) == 2:
                i = int(a[1], 0)
                v = ('k', 0, i & M32)
                st[reg(a[0])] = v
                note(v)
                f.disp.add(i)
                f.off.add(i)
            elif mn in ('addi', 'addis') and len(a) == 3:
                i = int(a[2], 0) << (16 if mn == 'addis' else 0)
                b = get(reg(a[1]))
                if b[0] == 'k':
                    v = ('k', 0, (b[2] + i) & M32)
                else:
                    v = ('s', b[1], b[2] + i)
                f.disp.add(int(a[2], 0)) if mn == 'addi' else None
                st[reg(a[0])] = v
                note(v)
            elif mn in ('ori', 'oris') and len(a) == 3:
                i = int(a[2], 0) << (16 if mn == 'oris' else 0)
                b = get(reg(a[1]))
                if b[0] == 'k':
                    v = ('k', 0, (b[2] | i) & M32)
                    note(v)
                else:
                    v = fresh()
                st[reg(a[0])] = v
            elif mn == 'mr' and len(a) == 2:
                st[reg(a[0])] = get(reg(a[1]))
            elif mn.rstrip('u') in LOADS + STORES or mn in ('lwzu', 'stwu', 'lbzu', 'lhzu', 'stbu', 'sthu', 'lfsu', 'stfsu'):
                mm = MEM.match(ops.replace(' ', ''))
                if mm:
                    d, rb = int(mm.group(1)), int(mm.group(2))
                    f.disp.add(d)
                    f.off.add(d)
                    b = get(rb)
                    ea = ('k', 0, (b[2] + d) & M32) if b[0] == 'k' else ('s', b[1], b[2] + d)
                    note(ea)
                    if mn.endswith('u') and mn not in ('lfdu',):
                        st[rb] = ea
                    if mn[0] == 'l' and not mn.startswith('lf') and not mn.startswith('lv'):
                        st[reg(a[0])] = fresh()
            elif mn in ('cmpwi', 'cmplwi', 'cmpdi', 'cmpldi') and len(a) == 3:
                f.disp.add(int(a[2], 0))
                f.off.add(int(a[2], 0))
            elif mn == 'mulli' and len(a) == 3:
                f.stride.add(int(a[2], 0))
                st[reg(a[0])] = fresh()
            elif mn == 'slwi' and len(a) == 3:
                f.stride.add(1 << int(a[2], 0))
                st[reg(a[0])] = fresh()
            elif mn.startswith('rlwinm') and len(a) >= 3:
                f.stride.add(1 << int(a[2], 0))
                st[reg(a[0])] = fresh()
            elif mn == 'bl':
                for x in a:
                    if x.startswith('0x'):
                        f.callees.add(int(x, 16))
                for n in [0] + list(range(3, 13)):
                    st[n] = fresh()
            elif a and reg(a[0]) is not None and not mn.startswith(('st', 'mt', 'cmp', 'b', 'tw', 'td', 'dcb', 'icb', 'sync', 'eieio')):
                st[reg(a[0])] = fresh()
        except (ValueError, TypeError):
            pass
    return f


def near_miss(v, kind, facts_list):
    """The value the listing gives that explains the slip, or None."""
    for fa in facts_list:
        if kind == 'addr':
            for a in fa.abs:
                if a != v and (a & 0xFFFF) == (v & 0xFFFF) and (a >> 24) in (0x82, 0x83):
                    return a
                if a != v and (a == ((v + 0x10000) & M32) or a == ((v - 0x10000) & M32)):
                    return a
        else:
            for a in fa.off | fa.abs:
                if a > 0xFFFF and a != v and v >= 1024 and (a & 0xFFFF) >= 1024 and (v == (a & 0xFFFF) or 0 < abs((a & 0xFFFF) - v) <= 64) and a < 0x10000000:
                    return a
                if a != v and v >= 1024 and (a & 0xFFFF) and a < 0x10000000 and (a == v + 0x10000 or a == v - 0x10000):
                    return a
    return None


def gives(v, kind, fa):
    if kind == 'addr':
        return v in fa.abs
    if v in fa.off or v in fa.abs or v in fa.stride or v in fa.disp:
        return True
    return any((v - d) in fa.off or (v - d) in fa.abs for d in fa.disp if d)   # a known constant plus a displacement (lis 1 | ori, then -8)


def code_text(line):
    """The line without string literals and without its // comment; and the comment."""
    s = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)
    k = s.find('//')
    return (s[:k], s[k:]) if k >= 0 else (s, '')


def cites(gen, text, own=None):
    """sub_ names and instruction addresses of a text -> function starts."""
    out = set()
    for m in SUB_RE.finditer(text):
        out.add(int(m.group(1), 16))
    for m in INS_RE.finditer(text):
        v = int(m.group(1), 16)
        if v == own or v < 0x82250000:
            continue
        c = gen.containing(v)
        if c is not None and gen.body(c) is not None:
            out.add(c)
    return out


def load_ok(root):
    ok = set()
    p = os.path.join(root, 'tools', 'analysis', 'address_lint_ok.txt')
    if os.path.exists(p):
        for ln in open(p, encoding='utf-8'):
            ln = ln.strip()
            if ln and not ln.startswith('#'):
                ok.add(tuple(x.strip() for x in ln.split('|')[:2]))
    return ok


def find_gen(root, given):
    cands = [given] if given else [os.path.join(root, 'generated', 'default')]
    if not given:
        try:
            cd = subprocess.run(['git', 'rev-parse', '--git-common-dir'], capture_output=True, text=True, cwd=root).stdout.strip()
            cands.append(os.path.join(os.path.dirname(os.path.abspath(os.path.join(root, cd))), 'generated', 'default'))
        except OSError:
            pass
    for c in cands:
        if c and os.path.exists(os.path.join(c, 'codegen.partition.json')):
            return c
    return None


def scan(path, display, gen, fcache):
    """-> list of entries {file, line, name, value, kind, status, detail}."""
    with open(path, encoding='utf-8', errors='replace') as f:
        lines = f.read().splitlines()
    parts = [code_text(l) for l in lines]
    filecites = set()
    for l in lines:
        filecites |= cites(gen, l)

    def facts(fn):
        if fn not in fcache:
            b = gen.body(fn)
            fcache[fn] = interpret(b) if b else None
        return fcache[fn]

    res = []

    def judge(i, name, v, kind):
        own = v
        t1 = cites(gen, lines[i], own)
        t2 = set()
        for j in range(max(0, i - 3), min(len(lines), i + 4)):
            t2 |= cites(gen, lines[j], own)
        t3 = set()
        for j in range(max(0, i - 30), max(0, i - 3)):
            t3 |= cites(gen, lines[j], own)
        t4 = filecites
        tiers = [('line', t1), ('near', t2 - t1), ('above', t3 - t2 - t1), ('file-wide', t4 - t3 - t2 - t1)]
        allc = t1 | t2 | t3 | t4
        e = {'file': display, 'line': i + 1, 'name': name, 'value': v, 'kind': kind}
        if kind == 'code':
            c = gen.containing(v)
            b = gen.body(c) if c is not None else None
            if b is not None and 'ctx.lr = 0x%08x;' % (v + 4) in b.lower():   # a bl at v: its return address is v + 4 (a call-site constant)
                e.update(status='verified', detail='call site (bl) in sub_%08X' % c)
                return e
            if not allc:
                # a code-range table entry (origin_table.h, surface_role_table.h: [lo, hi) of a function, hi = the next function start): no
                # function to cite, but it must sit on a function boundary; a typo off a boundary stays "uncited" and is listed
                i = bisect.bisect_left(gen.starts, v)
                if i < len(gen.starts) and 0 <= gen.starts[i] - v <= 16:
                    e.update(status='verified', detail='code boundary (no function cited): sub_%08X' % gen.starts[i])
                else:
                    e.update(status='uncited', detail='not on a function boundary')
                return e
            for tn, ts in tiers:
                for fn in sorted(ts):
                    fa = facts(fn)
                    if v == fn or (c == fn and v < gen.end_of(fn)) or (fa and v in fa.callees):
                        e.update(status='verified', detail='%s sub_%08X' % (tn, fn))
                        return e
            if v in gen.file_of:   # the entry of a function, named by its address (a compare against a function start)
                e.update(status='verified', detail='function entry sub_%08X (not a cited one)' % v)
                return e
            e.update(status='notfound', detail='in sub_%08X, not a cited function' % c if c else 'not in the code')
            return e
        if not allc:
            e.update(status='uncited', detail='')
            return e
        pool = []
        for ti, (tn, ts) in enumerate(tiers):
            for fn in sorted(ts):
                fa = facts(fn)
                if fa is None:
                    continue
                pool.append(fa)
                if gives(v, kind, fa):
                    # CONFLICT: a function cited as near (the same tier or nearer) gives a near miss of it and not the value itself:
                    # the value matches one reader while the nearest cited reader says otherwise (C6's kCvDescOff: the init function had 62048, the reader 127584).
                    for tn2, ts2 in tiers[:ti + 1]:
                        for fn2 in sorted(ts2):
                            fb = facts(fn2)
                            if fb is None or fn2 == fn or gives(v, kind, fb):
                                continue
                            nm = near_miss(v, kind, [fb])
                            if nm is not None:
                                e.update(status='mismatch', detail='matches sub_%08X, but the %s-cited sub_%08X gives 0x%X, not 0x%X' % (fn, tn2, fn2, nm, v))
                                return e
                    e.update(status='verified', detail='%s sub_%08X' % (tn, fn))
                    return e
        # one level of callees of the cited functions
        for tn, ts in tiers:
            for fn in sorted(ts):
                fa = facts(fn)
                for cal in sorted(fa.callees if fa else ()):
                    if gen.body(cal) is None:
                        continue
                    cf = facts(cal)
                    pool.append(cf)
                    if cf and gives(v, kind, cf):
                        e.update(status='verified', detail='%s callee sub_%08X of sub_%08X' % (tn, cal, fn))
                        return e
        nm = near_miss(v, kind, pool)
        if nm is not None:
            e.update(status='mismatch', detail='the cited functions give 0x%X, not 0x%X' % (nm, v))
        elif not pool:
            e.update(status='notfound', detail='a cited function has no listing')
        else:
            e.update(status='notfound', detail='no cited function (%d) gives it' % len(allc))
        return e

    for i, (code, comment) in enumerate(parts):
        if not code.strip():
            continue
        for m in ADDR_RE.finditer(code):
            v = int(m.group(1), 16)
            kind = 'code' if (v >= gen.starts[0] and v <= gen.starts[-1] + 0x10000 and v >= 0x82250000) else 'addr'
            res.append(judge(i, '0x%08X' % v, v, kind))
        for m in OFF_RE.finditer(code):
            if m.group(1).startswith('kOff') or m.group(1).endswith('ClipOff'):   # host-side blocks / flags, not guest offsets
                continue
            res.append(judge(i, m.group(1), int(m.group(2), 0), 'off'))
    return res


def selftest(root, a):
    gd = find_gen(root, a.generated)
    if gd is None:
        print('FAIL: generated\\default (codegen.partition.json) not found')
        return 1
    r = subprocess.run(['git', 'show', 'd8dc878c^:src/scene_live/ring_recompute_core.h'], capture_output=True, cwd=root)
    if r.returncode:
        print('FAIL: selftest: git show d8dc878c^ failed')
        return 1
    out = os.path.join(root, 'out', 's103', 'x1')
    os.makedirs(out, exist_ok=True)
    p = os.path.join(out, 'old_core.h')
    with open(p, 'wb') as f:
        f.write(r.stdout)
    ents = scan(p, 'src/scene_live/ring_recompute_core.h', Gen(gd), {})
    got = {e['name'] for e in ents if e['status'] == 'mismatch'}
    want = {'kCvDescOff', '0x83763AA8', '0x83763928'}
    for w in sorted(want):
        print('selftest %-12s %s' % (w, 'caught' if w in got else 'NOT CAUGHT'))
    ok = want <= got
    print('RESULT', 'OK' if ok else 'FAIL')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default='.')
    ap.add_argument('--generated', default=None)
    ap.add_argument('--files', nargs='*', help='scan these files instead (display name = the path as given)')
    ap.add_argument('--as-name', default=None, help='with one --files: the name the ok-list knows it by')
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('--list-uncited', action='store_true')
    ap.add_argument('--selftest', action='store_true', help="prove the tool on C6's known bugs: the parent of commit d8dc878c must give 3 mismatches (kCvDescOff, the two 0x8376 globals)")
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    if a.selftest:
        return selftest(root, a)
    gd = find_gen(root, a.generated)
    if gd is None:
        print('FAIL: generated\\default (codegen.partition.json) not found')
        return 1
    gen = Gen(gd)
    fcache = {}
    targets = []
    if a.files:
        targets = [(p, a.as_name or p.replace('\\', '/')) for p in a.files]
    else:
        for d in DIRS:
            for dp, _, fns in os.walk(os.path.join(root, d)):
                for fn in sorted(fns):
                    if fn.endswith(('.h', '.cpp', '.inc')) and not fn.endswith('_test.cpp'):
                        full = os.path.join(dp, fn)
                        targets.append((full, os.path.relpath(full, root).replace('\\', '/')))
    ok = load_ok(root)
    entries = []
    for p, disp in targets:
        entries += scan(p, disp, gen, fcache)
    cnt = {'verified': 0, 'mismatch': 0, 'notfound': 0, 'uncited': 0}
    waived = 0
    bad = []
    for e in entries:
        if e['status'] == 'mismatch' and (e['file'], '0x%X' % e['value']) in ok or (e['status'] == 'mismatch' and (e['file'], str(e['value'])) in ok):
            e['status'] = 'verified'
            e['detail'] = 'waived (address_lint_ok.txt)'
            waived += 1
        cnt[e['status']] += 1
        if e['status'] == 'mismatch':
            bad.append(e)
        if a.verbose or e['status'] == 'mismatch' or (a.list_uncited and e['status'] in ('uncited', 'notfound')):
            print('%-9s %s:%d %s (%s) %s' % (e['status'].upper(), e['file'], e['line'], e['name'], e['kind'], e['detail']))
    nb = sum(1 for e in entries if e['detail'].startswith('code boundary'))
    nc = sum(1 for e in entries if e['detail'].startswith('call site'))
    print('address_lint: %d constants in %d files: verified %d (of which %d code-range boundaries, %d call sites; waived %d), mismatch %d, not found in the cited function %d, no source cited %d' % (
        len(entries), len(targets), cnt['verified'], nb, nc, waived, cnt['mismatch'], cnt['notfound'], cnt['uncited']))
    print('RESULT', 'OK' if not bad else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
