#!/usr/bin/env python3
"""S103 lane T5: the resolver census. A static table of every "look up by id or by name" call: for each resolver function, each call
site's constant (the id passed, or the string at the address passed) and where the result is stored. "Find the writer" found the
light-set ids by reading 8 of 62 sites by hand; this reads all of them.

  py -I tools\\analysis\\resolver_census.py                  # census of the resolvers in resolver_census_list.json -> out\\analysis\\resolver_census.md (regenerated, never committed: strings read from the game image)
  py -I tools\\analysis\\resolver_census.py --check          # re-find check: the known light-set answers must be in the table (exit 1 if not)
  py -I tools\\analysis\\resolver_census.py --discover [N]   # rank candidate resolvers (small, loop + compare, many callers)
  py -I tools\\analysis\\resolver_census.py --site 0x825CE05C  # one call site, with its readings

Reads generated\\ (this tree, else the main checkout, like address_lint) and out\\harness\\index\\image.bin (strings) locally. The written
table holds ids, short identifier strings, offsets and addresses only: no listing text, no generated code.
MARKS: [C] the constant AND the store were both read from the listing; [U] otherwise (dynamic argument, no store in reach, pass-on).
METHOD (a linear scan, no control flow: a census, not a proof):
  key register -> the last write before the call in the same function: li / lis+addi / lis+ori -> constant (a constant >= 0x82000000 is an
  address: the string there is the name); mr -> follow; a load / arithmetic -> dynamic.
  result -> the first stw/stb/sth/std of r3 (or of a register copied from r3) within 14 instructions, before the next call or label join.
"""
import argparse
import bisect
import collections
import json
import os
import pickle
import re
import struct
import subprocess
import sys

M32 = 0xFFFFFFFF
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
IMAGE_BASE = 0x82000000
LIST = os.path.join(HERE, 'resolver_census_list.json')


def find_gen():
    cands = [os.path.join(ROOT, 'generated', 'default')]
    try:
        cd = subprocess.run(['git', 'rev-parse', '--git-common-dir'], capture_output=True, text=True, cwd=ROOT).stdout.strip()
        cands.append(os.path.join(os.path.dirname(os.path.abspath(os.path.join(ROOT, cd))), 'generated', 'default'))
    except OSError:
        pass
    for c in cands:
        if os.path.exists(os.path.join(c, 'codegen.partition.json')):
            return c
    sys.exit('generated\\default not found')


def find_image():
    cands = [os.path.join(ROOT, 'out', 'harness', 'index', 'image.bin')]
    try:
        cd = subprocess.run(['git', 'rev-parse', '--git-common-dir'], capture_output=True, text=True, cwd=ROOT).stdout.strip()
        cands.append(os.path.join(os.path.dirname(os.path.abspath(os.path.join(ROOT, cd))), 'out', 'harness', 'index', 'image.bin'))
    except OSError:
        pass
    for c in cands:
        if os.path.exists(c):
            with open(c, 'rb') as f:
                return f.read()
    return b''


INSN = re.compile(r'^\s*// ([a-z][a-z0-9_.+-]*)\s*(.*)$')
LABEL = re.compile(r'^\s*loc_([0-9A-F]{8}):')
DEFN = re.compile(r'DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')


def stamp(gdir):
    try:
        return os.path.getmtime(os.path.join(gdir, 'codegen.stamp'))
    except OSError:
        return 0


def parse_all(gdir, cache=None):
    """{start: [(addr, mnemonic, [operands], label_before)]} for every function. Cached as a pickle (scratch) when `cache` is given."""
    if cache and os.path.exists(cache):
        try:
            with open(cache, 'rb') as f:
                d = pickle.load(f)
            if d.get('gdir') == gdir and d.get('stamp') == stamp(gdir):
                return d['fns']
        except Exception:
            pass
    fns = {}
    for fn in os.listdir(gdir):
        if not re.fullmatch(r'wwe2k14_recomp\.\d+\.cpp', fn):
            continue
        with open(os.path.join(gdir, fn), encoding='utf-8', errors='replace') as f:
            lines = f.read().split('\n')
        cur, addr, lab = None, 0, False
        for ln in lines:
            m = DEFN.search(ln)
            if m:
                cur = int(m.group(1), 16)
                fns[cur] = []
                addr, lab = cur, False
                continue
            if cur is None:
                continue
            m = LABEL.match(ln)
            if m:
                addr, lab = int(m.group(1), 16), True
                continue
            m = INSN.match(ln)
            if m:
                ops = [x.strip() for x in m.group(2).split(',')] if m.group(2).strip() else []
                fns[cur].append((addr, m.group(1), ops, lab))
                lab = False
                addr += 4
    if cache:
        with open(cache, 'wb') as f:
            pickle.dump({'gdir': gdir, 'stamp': stamp(gdir), 'fns': fns}, f)
    return fns


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def num(s):
    try:
        return int(s, 0)
    except (ValueError, TypeError):
        return None


MEMOP = re.compile(r'^(-?(?:0x[0-9a-fA-F]+|\d+))\(r(\d+)\)$')
STORES = ('stw', 'stb', 'sth', 'std')
LOADS = ('lwz', 'lbz', 'lhz', 'lha', 'lwzu', 'lbzu', 'lhzu', 'lwzx', 'lbzx', 'lhzx', 'lhax', 'lwax', 'lwa', 'ld', 'ldx')
ARITH = ('rlwinm', 'slwi', 'srwi', 'add', 'subf', 'mulli', 'extsb', 'extsh', 'clrlwi', 'neg', 'sub', 'and', 'or', 'andi.', 'xori',
         'rlwimi', 'srawi', 'subfic', 'cntlzw', 'xor', 'mullw', 'divwu', 'divw', 'rotlwi')


def reg(s):
    m = re.fullmatch(r'r(\d+)', s)
    return int(m.group(1)) if m else None


def is_call(mn):
    return mn in ('bl', 'bctrl', 'blrl', 'bla')


class Image:
    def __init__(self, data):
        self.d = data

    def cstr(self, a, maxn=80):
        o = a - IMAGE_BASE
        if not (0 <= o < len(self.d)):
            return None
        e = self.d.find(b'\0', o, o + maxn + 1)
        if e < 0:
            return None
        s = self.d[o:e]
        if len(s) < 2 or not all(32 <= c < 127 for c in s):
            return None
        return s.decode('ascii')


def back_value(ins, i, r, depth=0):
    """Trace register r backward from instruction index i (exclusive).
    -> ('const', value, [addrs]) | ('hi', value, [addrs]) | ('dyn', text, [addrs]) | ('?', why, [])"""
    j = i - 1
    steps = 0
    while j >= 0 and steps < 60:
        addr, mn, ops, lab = ins[j]
        steps += 1
        if mn == 'li' and reg(ops[0]) == r:
            return ('const', s16(num(ops[1]) or 0), [addr])
        if mn == 'lis' and reg(ops[0]) == r:
            return ('hi', (num(ops[1]) << 16) & M32, [addr])
        if mn in ('addi', 'ori', 'oris', 'addis') and len(ops) == 3 and reg(ops[0]) == r:
            src, imm = reg(ops[1]), num(ops[2])
            if imm is None or src is None:
                return ('?', mn, [addr])
            if mn == 'addi' and src == 0:
                return ('const', s16(imm), [addr])
            base = back_value(ins, j, src, depth + 1) if depth < 4 else ('?', 'deep', [])
            if base[0] in ('hi', 'const'):
                bv = base[1]
                if mn == 'addi':
                    v = (bv + s16(imm)) & M32
                elif mn == 'ori':
                    v = bv | (imm & 0xFFFF)
                elif mn == 'oris':
                    v = bv | ((imm & 0xFFFF) << 16)
                else:
                    v = (bv + (s16(imm) << 16)) & M32
                return ('const', v, base[2] + [addr])
            return ('dyn', '%s r%d,%s' % (mn, src, ops[2]), [addr])
        if mn == 'mr' and reg(ops[0]) == r:
            src = reg(ops[1])
            if src is not None and depth < 4:
                v = back_value(ins, j, src, depth + 1)
                return v if v[0] != '?' else ('dyn', 'r%d' % src, [addr])
            return ('?', 'mr', [addr])
        if mn in LOADS and ops and reg(ops[0]) == r:
            m = MEMOP.match(ops[1]) if len(ops) > 1 else None
            if m:
                return ('dyn', 'load [r%s%+d]' % (m.group(2), num(m.group(1))), [addr])
            return ('dyn', 'indexed load (%s)' % mn, [addr])
        if mn in ARITH and ops and reg(ops[0]) == r:
            return ('dyn', mn, [addr])
        if is_call(mn) and 3 <= r <= 12:
            return ('dyn', 'call result', [addr]) if r == 3 else ('?', 'clobbered by a call', [addr])
        if lab and depth == 0 and steps > 1:
            pass   # a label join: keep going linearly (the census is linear); the [U] mark covers the doubt where the value is not a constant
        j -= 1
    return ('dyn', 'argument of this function', [])


def fwd_store(ins, i, limit=14, target=None):
    """After the call at index i: where r3 is stored -> (text, addr) or a pass-on / return note, else None."""
    held = {3}
    kept = None
    for j in range(i + 1, min(len(ins), i + 1 + limit)):
        addr, mn, ops, lab = ins[j]
        if mn == 'mr' and len(ops) == 2 and reg(ops[1]) == 3 and reg(ops[0]) is not None and 14 <= reg(ops[0]) <= 31 and kept is None:
            kept = (j, reg(ops[0]))
        if mn == 'stwx' and len(ops) == 3 and reg(ops[0]) in held:
            return ('[r%d+r%d*1]' % (reg(ops[2]), reg(ops[1])), addr)
        if mn in STORES and ops and reg(ops[0]) in held:
            m = MEMOP.match(ops[1]) if len(ops) > 1 else None
            if m:
                return ('[r%s%+d]' % (m.group(2), num(m.group(1))), addr)
        if mn == 'mr' and len(ops) == 2 and reg(ops[1]) in held and reg(ops[0]) is not None:
            held.add(reg(ops[0]))
        if mn in ('addi', 'extsw', 'extsh') and len(ops) >= 2 and reg(ops[1]) in held and reg(ops[0]) is not None:
            held.add(reg(ops[0]))
        if mn == 'bl' and target is not None and num(ops[0]) == target:
            held = {3}          # a fallback call of the same resolver (21 -> 20): its result is the one that counts
            continue
        if mn == 'bl':
            if kept:
                return ('kept in r%d' % kept[1], ins[kept[0]][0])
            return ('passed to %s' % ops[0], addr) if held & {3, 4, 5} else None
        if mn in ('bctrl', 'blrl'):
            return None
        if mn == 'blr':
            return ('returned', addr) if 3 in held else None
        if mn in ('b', 'bctr'):
            return ('returned (tail)', addr) if 3 in held else None
        if mn in LOADS + ('li', 'lis') and ops and reg(ops[0]) == 3:
            held.discard(3)
        if not held:
            return None
    if kept:
        return ('kept in r%d' % kept[1], ins[kept[0]][0])
    return None


def base_note(ins, i, breg):
    """What the store's base register holds: `this` (= the function's own argument rN, copied by an mr near the top) or a local."""
    for j in range(0, i):
        addr, mn, ops, lab = ins[j]
        if mn == 'mr' and reg(ops[0]) == breg and reg(ops[1]) is not None:
            src = reg(ops[1])
            return 'this=arg r%d' % src if 3 <= src <= 10 else 'r%d' % src
        if mn in ('lwz', 'addi') and ops and reg(ops[0]) == breg:
            return 'loaded'
    return ''


def sites_of(fns, target):
    out = []
    t = '0x%08x' % target
    for start, ins in fns.items():
        for i, (addr, mn, ops, lab) in enumerate(ins):
            if mn in ('bl', 'b') and ops and ops[0].lower() == t and not (mn == 'b' and addr == start):
                out.append((start, i, mn))
    out.sort(key=lambda x: fns[x[0]][x[1]][0])
    return out


def stack_key_string(ins, i, img, span=90):
    """The key is a stack buffer (addi rX,r1,N): the nearest string constant passed to a call before it (the copy that filled the buffer).
    Inferred, not read: the caller marks it with '~' and [U]."""
    j = i - 1
    steps = 0
    while j >= 0 and steps < span:
        addr, mn, ops, lab = ins[j]
        steps += 1
        if mn == 'bl':
            for r in (4, 5, 3, 6):
                v = back_value(ins, j, r)
                if v[0] == 'const' and (v[1] & M32) >= IMAGE_BASE:
                    st = img.cstr(v[1] & M32)
                    if st:
                        return st
        j -= 1
    return None


def read_site(fns, img, start, i, mn, keyregs, target=None):
    ins = fns[start]
    keys = []
    for kr in keyregs:
        v = back_value(ins, i, kr)
        if v[0] == 'const':
            val = v[1]
            s = img.cstr(val) if (val & M32) >= IMAGE_BASE else None
            if s is not None:
                keys.append(('str', s, v[2]))
            elif (val & M32) >= IMAGE_BASE:
                keys.append(('addr', val & M32, v[2]))
            else:
                keys.append(('id', val, v[2]))
        elif v[0] == 'hi':
            keys.append(('dyn', 'lis only 0x%08X' % v[1], v[2]))
        else:
            if v[0] == 'dyn' and isinstance(v[1], str) and v[1].startswith('addi r1,'):
                inferred = stack_key_string(ins, i, img)
                if inferred:
                    keys.append(('dyn', 'stack buffer, nearest string passed to a call before it: "%s" ~' % inferred, v[2]))
                    continue
            keys.append(('dyn', v[1], v[2]))
    st = fwd_store(ins, i, target=target) if mn == 'bl' else None
    return {'fn': start, 'call': ins[i][0], 'keys': keys, 'store': st, 'tail': mn == 'b'}


def census(fns, img, addr, spec):
    rows = []
    for s, i, mn in sites_of(fns, addr):
        r = read_site(fns, img, s, i, mn, spec['key_regs'], addr)
        if r['store'] and r['store'][0].startswith('['):
            r['base'] = base_note(fns[s], i, int(re.match(r'\[r(\d+)', r['store'][0]).group(1)))
        rows.append(r)
    return rows


def fmt_key(k, fmt='id'):
    kind, val = k[0], k[1]
    if fmt == 'tag' and kind in ('id', 'addr'):
        b = struct.pack('>I', val & M32)
        if all(32 <= c < 127 for c in b):
            return "'%s' (0x%08X)" % (b.decode('ascii'), val & M32)
    if kind == 'id':
        return str(val)
    if kind == 'str':
        return '"%s"' % val
    if kind == 'addr':
        return '0x%08X' % val
    return '(%s)' % val


def row_mark(r):
    kc = all(k[0] in ('id', 'str', 'addr') for k in r['keys'])
    stc = r['store'] is not None and r['store'][0].startswith('[')
    return '[C]' if (kc and stc) else '[U]'


ROLES = {}


def fn_name(start, db):
    return ROLES.get(start) or db.get(start, '')


def load_db_names():
    names = {}
    d = os.path.join(ROOT, 'db')
    if not os.path.isdir(d):
        return names
    for root, _, files in os.walk(d):
        if 'ids' in root.split(os.sep):
            continue
        for f in files:
            if f.endswith('.json'):
                try:
                    with open(os.path.join(root, f), encoding='utf-8') as fh:
                        data = json.load(fh)
                except Exception:
                    continue
                items = data if isinstance(data, list) else data.get('functions', []) if isinstance(data, dict) else []
                for e in items:
                    if isinstance(e, dict) and 'address' in e and 'name' in e:
                        n = num(e['address'])
                        if n is not None:
                            names[n] = e['name']
    return names


OUT_PATH = ['']
EXTRA = []


def table_rows(rows, fmt, db):
    o = ['| caller | call at | constant | result stored at / consumer | at | role | mark |', '|---|---|---|---|---|---|---|']
    for r in rows:
        const = ' / '.join(fmt_key(k, fmt) for k in r['keys'])
        st = r['store']
        stt = (st[0] + (' (%s)' % r['base'] if r.get('base') else '')) if st else '-'
        m = re.match(r'passed to 0x([0-9a-f]{8})$', stt)
        if m:
            cn = fn_name(int(m.group(1), 16), db)
            stt = 'passed to sub_%s%s' % (m.group(1).upper(), ' (%s)' % cn if cn else '')
        o.append('| sub_%08X | 0x%08X%s | %s | %s | %s | %s | %s |' % (
            r['fn'], r['call'], ' (tail)' if r['tail'] else '', const, stt, ('0x%08X' % st[1]) if st else '-',
            fn_name(r['fn'], db), row_mark(r)))
    return o


def render(fns, img, spec_list):
    db = load_db_names()
    out = []
    out.append('# Resolver census (S103 lane T5)\n')
    out.append('Written by `py -I tools\\analysis\\resolver_census.py`. A static table of "look up by id or by name" calls: for every call site of each resolver, '
               'the constant passed (an id, or the string at the passed address) and where the result is stored. Only ids, short identifier strings, '
               'offsets and addresses; no listing text. **[C]** = constant and store both read from the listing; **[U]** = otherwise (dynamic argument, '
               'result passed on or not stored within 14 instructions). The scan is linear (no control flow): a join before the call can hide a second '
               'value, so a [C] row is "the last write on the straight path", not a proof for every path. Resolver list and evidence: section 1. '
               'The known answers it re-finds are checked by `--check` (S103 lane T5). What the table proves and settles: `resolver_census_findings.md`.\n')
    tables = []
    for spec in spec_list:
        addr = int(spec['addr'], 16)
        rows = census(fns, img, addr, spec)
        tables.append((spec, addr, rows))
    out.append('## 0. Summary\n')
    out.append('| resolver | address | kind | sites | constant read | result stored to memory | [C] (both) | [U] |')
    out.append('|---|---|---|---|---|---|---|---|')
    tot = [0, 0, 0, 0, 0]
    for spec, addr, rows in tables:
        c = sum(1 for r in rows if row_mark(r) == '[C]')
        kc = sum(1 for r in rows if all(k[0] in ('id', 'str', 'addr') for k in r['keys']))
        sm = sum(1 for r in rows if r['store'] and r['store'][0].startswith('['))
        out.append('| %s | 0x%08X | %s | %d | %d | %d | %d | %d |' % (spec['name'], addr, spec['kind'], len(rows), kc, sm, c, len(rows) - c))
        for n, v in enumerate((len(rows), kc, sm, c, len(rows) - c)):
            tot[n] += v
    out.append('| **total** | | | **%d** | **%d** | **%d** | **%d** | **%d** |\n' % tuple(tot))
    out.append('## 1. The resolvers (evidence)\n')
    for spec, addr, rows in tables:
        out.append('- **%s** `0x%08X` (%s): %s' % (spec['name'], addr, spec['kind'], spec['evidence']))
    out.append('')
    out.append('## 2. Tables\n')
    big = []
    for n, (spec, addr, rows) in enumerate(tables, 1):
        fmt = spec.get('fmt', 'id')
        out.append('### 2.%d %s (0x%08X), key in %s\n' % (n, spec['name'], addr, ', '.join('r%d' % k for k in spec['key_regs'])))
        if spec.get('big') or len(rows) > 120:
            out.append('%d sites; the by-constant digest is below, every row is in `docs\\formats\\resolver_census_%s.md`.\n' % (len(rows), spec['name']))
            big.append((spec, addr, rows))
            digest = collections.OrderedDict()
            for r in rows:
                digest.setdefault(' / '.join(fmt_key(k, fmt) for k in r['keys']), []).append(r)
            out.append('| constant | sites | callers (first 6) | stored at (first 3) |')
            out.append('|---|---|---|---|')
            for c, rs in sorted(digest.items(), key=lambda kv: -len(kv[1])):
                stores = []
                for r in rs:
                    if r['store'] and r['store'][0] not in stores:
                        stores.append(r['store'][0])
                out.append('| %s | %d | %s | %s |' % (c, len(rs), ', '.join('sub_%08X' % r['fn'] for r in rs[:6]), ', '.join(stores[:3]) or '-'))
            out.append('')
            continue
        out.extend(table_rows(rows, fmt, db))
        out.append('')
    for spec, addr, rows in big:
        path = os.path.join(os.path.dirname(OUT_PATH[0]), 'resolver_census_%s.md' % spec['name'])
        text = ['# Resolver census: %s (0x%08X), every call site (S103 lane T5)\n' % (spec['name'], addr),
                'Generated by tools\\analysis\\resolver_census.py; see resolver_census.md for the method and marks.\n']
        text.extend(table_rows(rows, spec.get('fmt', 'id'), db))
        EXTRA.append((path, '\n'.join(text) + '\n'))
    return '\n'.join(out), tables


def load_list():
    with open(LIST, encoding='utf-8') as f:
        d = json.load(f)
    for k, v in d.get('roles', {}).items():
        ROLES[int(k, 16)] = v
    return d['resolvers']


def find_row(tables, resolver, fn, key, store):
    for spec, addr, rows in tables:
        if addr != resolver:
            continue
        for r in rows:
            if r['fn'] == fn and any(k[0] == 'id' and k[1] == key for k in r['keys']) and r['store'] and r['store'][0] == store:
                return r
    return None


# the answer key: Fable 5.2's eight light-set sites (docs/renderer/S103_FABLE_REVIEW.md 1.1; each read there from the listing)
KNOWN = [
    ('ring object', 0x825CD8D0, 32, '[r31+9584]'),
    ('ring object', 0x825CD8D0, 31, '[r31+9588]'),
    ('ring-side object', 0x826094A0, 30, '[r31+1232]'),
    ('ring-side object', 0x826094A0, 31, '[r31+1236]'),
    ('crowd node', 0x82513D78, 20, '[r31+408]'),
    ('crowd light getter 21', 0x825BA530, 21, None),
    ('crowd light getter 20', 0x825BA530, 20, None),
    ('character A', 0x825A20A0, 10, '[r31+3920]'),
    ('character B', 0x825A2A88, 10, '[r31+2768]'),
]


def offs(t):
    m = re.match(r'\[r\d+([+-]\d+)\]', t)
    return m.group(1) if m else t


def do_check(tables):
    ok = True
    n = 0
    sixtytwo = [t for t in tables if t[1] == 0x827F6A40]
    nsites = len(sixtytwo[0][2]) if sixtytwo else 0
    for label, fn, key, store in KNOWN:
        found = None
        for spec, addr, rows in tables:
            if addr != 0x827F6A40:
                continue
            for r in rows:
                if r['fn'] == fn and any(k[0] == 'id' and k[1] == key for k in r['keys']):
                    if store is None or (r['store'] and offs(r['store'][0]) == offs(store)):
                        found = r
        if found:
            n += 1
            print('REFIND ok   %-22s sub_%08X id %-3d store %s' % (label, fn, key, found['store'][0] if found['store'] else '-'))
        else:
            ok = False
            print('REFIND FAIL %-22s sub_%08X id %-3d store %s' % (label, fn, key, store))
    # the l(N) rule (A7): the slot-text parser resolves each parsed id at load and stores the position in an indexed table; the init fills it with set 100
    parser = [r for t in sixtytwo for r in t[2] if r['fn'] == 0x824E8628]
    stored = [r for r in parser if r['store'] and r['store'][0].startswith('[r24+')]
    init = [r for t in sixtytwo for r in t[2] if r['fn'] == 0x824E8E38 and any(k[0] == 'id' and k[1] == 100 for k in r['keys'])]
    if len(stored) == 3 and len(init) == 1:
        print('REFIND ok   l(N) parser sub_824E8628: 3 resolves of the parsed id stored into an indexed table (r24); init sub_824E8E38 resolves set 100')
    else:
        print('REFIND FAIL l(N) parser: %d of 3 indexed stores, %d of 1 default-100 site' % (len(stored), len(init)))
        ok = False
    # the stage-slot node: set 500 stored at [node+168]
    node = [r for t in sixtytwo for r in t[2] if r['fn'] == 0x83420CB0 and any(k[0] == 'id' and k[1] == 500 for k in r['keys']) and r['store'] and offs(r['store'][0]) == '+168']
    if node:
        print('REFIND ok   stage-slot node sub_83420CB0: set 500 stored at [node+168]')
    else:
        print('REFIND FAIL stage-slot node set 500 at +168')
        ok = False
    # q callers says 62 call sites in 46 functions
    fnset = {r['fn'] for t in sixtytwo for r in t[2]}
    print('sites %d in %d functions (q callers: 62 in 46)' % (nsites, len(fnset)))
    if nsites != 62 or len(fnset) != 46:
        print('REFIND FAIL site count differs from the q index (62 / 46)')
        ok = False
    print('CHECK %s: %d of %d known light-set answers found' % ('PASS' if ok else 'FAIL', n, len(KNOWN)))
    return 0 if ok else 1


def do_discover(fns, topn):
    """Rank candidate resolvers: small, has a backward branch and a compare, returns a value, called from many places."""
    callers = collections.Counter()
    for start, ins in fns.items():
        seen = set()
        for addr, mn, ops, lab in ins:
            if mn == 'bl' and ops:
                t = num(ops[0])
                if t is not None and t not in seen:
                    seen.add(t)
                    callers[t] += 1
    rows = []
    for start, ins in fns.items():
        n = len(ins)
        if not (10 <= n <= 140) or callers[start] < 4:
            continue
        addrs = {a for a, _, _, _ in ins}
        back = 0
        cmp_ = 0
        calls = 0
        loops_strcmp = False
        for addr, mn, ops, lab in ins:
            if mn.startswith('cmp'):
                cmp_ += 1
            if mn in ('b', 'bdnz', 'bne', 'beq', 'blt', 'bgt', 'ble', 'bge', 'bdz') or mn.startswith('b') and not is_call(mn):
                t = num(ops[-1]) if ops else None
                if t is not None and start <= t < addr:
                    back += 1
            if mn == 'bl':
                calls += 1
        if back and cmp_:
            rows.append((callers[start], n, calls, start))
    rows.sort(reverse=True)
    for c, n, calls, s in rows[:topn]:
        print('sub_%08X  callers %4d  insns %3d  calls %2d' % (s, c, n, calls))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--discover', nargs='?', const=40, type=int)
    ap.add_argument('--site')
    ap.add_argument('--fn', help='census one resolver by address (key regs from --keys, default 4)')
    ap.add_argument('--keys', default='4')
    ap.add_argument('--out', default=os.path.join(ROOT, 'out', 'analysis', 'resolver_census.md'))
    ap.add_argument('--no-write', action='store_true')
    ap.add_argument('--twin', choices=['wrong_key_reg'], help='planted fault for the check: the light-set key read from r3 (the manager) instead of r4')
    ap.add_argument('--expect-fail', action='store_true', help='exit 0 only if the check FAILS (the planted twin must be caught)')
    ap.add_argument('--cache', default=os.path.join(os.environ.get('TEMP', '.'), 't5_parse_cache.pkl'))
    a = ap.parse_args()
    gdir = find_gen()
    fns = parse_all(gdir, a.cache)
    img = Image(find_image())
    if a.discover:
        return do_discover(fns, a.discover)
    if a.fn:
        spec = {'addr': a.fn, 'name': 'adhoc', 'kind': 'adhoc', 'key_regs': [int(x) for x in a.keys.split(',')], 'evidence': ''}
        rows = census(fns, img, int(a.fn, 16), spec)
        for r in rows:
            print('sub_%08X 0x%08X %s -> %s %s' % (r['fn'], r['call'], ' / '.join(fmt_key(k) for k in r['keys']),
                                                   r['store'][0] if r['store'] else '-', row_mark(r)))
        return 0
    spec_list = load_list()
    if a.twin == 'wrong_key_reg':
        spec_list[0]['key_regs'] = [3]
    OUT_PATH[0] = a.out
    text, tables = render(fns, img, spec_list)
    if a.check:
        rc = do_check(tables)
        if a.expect_fail:
            print('expect-fail: the check %s' % ('failed as planned' if rc else 'PASSED (the twin was NOT caught)'))
            if rc:
                print('PLANTED-OK: the planted twin made the resolver census check fail, as it must')   # S105 E
            return 0 if rc else 1
        return rc
    if a.site:
        t = int(a.site, 0)
        for spec, addr, rows in tables:
            for r in rows:
                if r['call'] == t:
                    print(r)
        return 0
    if not a.no_write:
        os.makedirs(os.path.dirname(a.out), exist_ok=True)
        for pth, txt in [(a.out, text + '\n')] + EXTRA:
            with open(pth, 'w', encoding='utf-8', newline='\n') as f:
                f.write(txt)
            print('wrote', pth)
    return 0


if __name__ == '__main__':
    sys.exit(main())
