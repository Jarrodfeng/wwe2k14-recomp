"""Build the code index (SQLite) that tools/q/q.py queries. Session 8, task 5.

    py tools/q/build_index.py [--image FILE] [--map FILE] [--gen DIR] [--out FILE] [-j N]

Sources (all local; nothing here is committed):
  generated/default/*_recomp.*.cpp   every function body; each PPC instruction is a `// mnemonic operands` comment.
                                      Edges, immediates and lis-pair addresses are parsed from those comments.
  generated/default/*_init.cpp       PPCFuncMappings (address -> name, incl. __imp__ import thunks, xstart, helpers)
  generated/default/codegen.stamp    codegen fingerprint (q warns when it changes after the index was built)
  out/harness/index/image.bin        guest image 0x82000000.. as loaded (app setting --wwe2k14_dump_image, see
                                      src/image_dump.h): printable strings, data pointers, MSVC RTTI
  out/build/win-amd64-release/wwe2k14.map   host symbol -> RVA (host offset -> guest function)
Output: out/harness/index/index.db (game-derived metadata: gitignored folder, never commit).

Method notes (what the data does and does not mean):
  - Function size: walk the comment lines 4 bytes each, re-sync at `loc_XXXXXXXX:` labels (same method as
    tools/codegen_repro/compare.py and tools/analysis/list_function.py).
  - Edges: a branch whose target is outside the function's own [start, end) range, or inside it but in another
    function that starts at or before the target (overlapping ranges: exception funclets). bl = call, b = tail (or
    a jump into another function), b<cond> = cond. Targets that are not a function start keep the raw target and
    the containing function. Indirect calls (bctrl) and jump-table/indirect jumps (bctr) are only counted.
  - Immediates: li/lis/addi/addis/ori/oris/andi./andis./xori/xoris/cmpwi/cmplwi/cmpdi/cmpldi/mulli/subfic/addic.
    Stored as u32 (signed ops sign-extended, unsigned ops zero-extended). addi with base r1 (stack frames) skipped.
  - lis pairs: a register loaded by lis and then used by addi/ori (op 'lis+addi'/'lis+ori') or as the base of a
    load/store (op 'lis+lwz', ...) gives a full 32-bit address. Tracked with a forward must-analysis over the
    function's control flow: the in-function branches are the generated `goto loc_X;` lines (incl. every case of a
    resolved bctr switch); at a label a lis value survives only if every predecessor (fall-through + each goto
    source) carries the same value; after an unconditional b/blr/bctr only gotos reach the next instruction; loops
    are iterated to a fixed point; volatile registers die at calls. A pair that only the old linear heuristic sees
    (lis value kept across labels and unconditional branches) is kept with a '?' suffix on its op (uncertain; q
    hides those unless --maybe). Registers changed by an indirect path the code does not show (none expected) are
    not modelled.
  - Strings: ASCII runs (>= 4 printable chars, NUL-terminated) and UTF-16BE runs (even address, >= 4 chars) outside
    the code range. A string reference = a lis pair landing exactly on a string start ('direct'), or a 'lis+lwz'
    whose image word is a pointer to a string start ('ptr'). Extra starts from certain lis+addi/lis+ori addresses:
    inside a run ('split'; not when every use of the built address is a float/vector load or store) or a 2-3 char
    string ('short'; not when any use is a float/vector access, nor 'XY' + two zero bytes that read as a float
    such as 1000.0f). A reference whose address register feeds a float/vector access gets how '...,fpvec'.
  - The build runs below normal priority and checks that the generated code, init.cpp, map and image did not change
    while it ran (a codegen or relink by another agent): it retries up to 3 times, else leaves the old index.
  - RTTI: MSVC layout, big-endian. TypeDescriptor = ".?AV/.?AU" name - 8; Complete Object Locator (COL) =
    word pointing at the TypeDescriptor, at COL+12, with signature 0 at COL+0; vtable = (word pointing at the COL) + 4;
    slots = following words that are registered function starts. Base classes from the COL's class hierarchy
    descriptor (+16 -> {sig, attr, count, pBaseArray}).
"""
import argparse
import bisect
import glob
import hashlib
import json
import multiprocessing
import os
import re
import sqlite3
import struct
import sys
import time

REPO = r'C:\Users\Jarrod\wwe2k14-recomp'
GEN = os.path.join(REPO, 'generated', 'default')
OUT_DIR = os.path.join(REPO, 'out', 'harness', 'index')
IMAGE = os.path.join(OUT_DIR, 'image.bin')
MAP = os.path.join(REPO, 'out', 'build', 'win-amd64-release', 'wwe2k14.map')
DB = os.path.join(OUT_DIR, 'index.db')
SCHEMA_VERSION = 1

IMAGE_BASE = 0x82000000
MAP_RE = re.compile(r'^\s*\{ 0x([0-9A-Fa-f]+), (\w+) \},?\s*$', re.M)
DEF = 'DEFINE_REX_FUNC('
# one match per line of interest: `// mnemonic operands` (an instruction), `loc_XXXXXXXX:` (a label) or a C++ line with
# `goto loc_XXXXXXXX;` (every in-function branch, incl. each case of a resolved bctr switch; attached to the
# instruction comment before it)
LINE_RE = re.compile(r'^[ \t]*(?://[ ]([^ \n]*)[ ]?([^\n]*)|loc_([0-9A-F]{8}):|[^\n/]*?\bgoto loc_([0-9A-F]{8});)', re.M)
MEM_RE = re.compile(r'^(r\d+|f\d+|v\d+),(-?(?:0x[0-9a-fA-F]+|\d+))\((r\d+)\)$')
FPVEC_PREFIX = ('lf', 'lv', 'stf', 'stv')   # float/vector loads and stores: an address they use is not a string
SIGNED_IMM = {'li', 'addi', 'addic', 'addic.', 'cmpwi', 'cmpdi', 'mulli', 'subfic', 'twi', 'tdi'}
UNSIGNED_IMM = {'ori', 'xori', 'andi.', 'cmplwi', 'cmpldi'}
SHIFTED_IMM = {'lis', 'addis', 'oris', 'xoris', 'andis.'}
NO_GPR_DEST_PREFIX = ('st', 'cmp', 'b', 'mt', 'tw', 'td', 'dcb', 'icb', 'sync', 'lwsync', 'eieio', 'fcmp', 'vcmp',
                      'nop', 'db16cyc', 'cctp')
VOLATILE = {'r0'} | {f'r{i}' for i in range(3, 13)}
COND_BRANCHES = {'beq', 'bne', 'blt', 'bgt', 'ble', 'bge', 'bnl', 'bng', 'bso', 'bns', 'bun', 'bnu', 'bdnz', 'bdz',
                 'bdnzt', 'bdnzf', 'bdzt', 'bdzf', 'bt', 'bf', 'bc'}
_MAPPING = None


def _init_worker(mapping):
    global _MAPPING
    _MAPPING = mapping
    lower_priority()   # inherited from the parent anyway; explicit in case the pool is started another way


def read_text(path):
    with open(path, 'rb') as f:
        return f.read().decode('utf-8', errors='replace')


def project_prefix(d):
    inits = glob.glob(os.path.join(d, '*_init.cpp'))
    if len(inits) != 1:
        raise SystemExit(f'build_index: expected one *_init.cpp in {d}, found {len(inits)}')
    return os.path.basename(inits[0])[:-len('_init.cpp')]


def load_mapping(d):
    prefix = project_prefix(d)
    mapping = {}
    for m in MAP_RE.finditer(read_text(os.path.join(d, f'{prefix}_init.cpp'))):
        mapping[m.group(2)] = int(m.group(1), 16)
    return prefix, mapping


def parse_int(s):
    return int(s, 0)


def branch_target(ops):
    last = ops.rsplit(',', 1)[-1].strip()
    if last.startswith('0x'):
        try:
            return int(last, 16)
        except ValueError:
            return None
    return None


def _meet(states):
    """Must-analysis meet: keep a register only when every incoming state has it with the same value."""
    s0 = states[0]
    if len(states) == 1:
        return dict(s0)
    out = {}
    for r, v in s0.items():
        for o in states[1:]:
            if o.get(r) != v:
                break
        else:
            out[r] = v
    return out


def decode(mn, ops):
    """One instruction -> (plain_imm, use, kills, lisdef, fpregs, branch).
    plain_imm: (value, op) recorded whatever the register state (None: none, or decided by `use`)
    use:       ('pair', src, v, mn, dest, plain_u) for addi/ori on a possible lis register (plain_u recorded when the
               pair is not certain), ('mem', base, disp, mn) for a load/store through a possible lis register
    kills:     registers this instruction overwrites (volatile ones at calls)
    lisdef:    (dest, value) for lis
    fpregs:    registers used as the address of a float/vector load or store
    branch:    None or (kind, target, is_ctr, is_terminator); kind 'call'/'tail'/'cond'/None"""
    plain = use = lisdef = branch = None
    kills = ()
    fpregs = ()
    if not mn:
        return plain, use, kills, lisdef, fpregs, branch
    if mn[0] == 'b':
        if mn == 'bctrl':
            return plain, use, tuple(VOLATILE), lisdef, fpregs, ('icall', None, True, False)
        if mn.endswith('ctr') or mn.endswith('ctrl'):
            return plain, use, kills, lisdef, fpregs, ('ijump', None, True, mn == 'bctr')
        if mn.endswith('lr') or mn.endswith('lrl'):
            return plain, use, kills, lisdef, fpregs, (None, None, False, mn == 'blr')
        t = branch_target(ops)
        if t is None:
            return plain, use, kills, lisdef, fpregs, None
        mn0 = mn.rstrip('+-')
        if mn0 == 'bl' or (mn0.endswith('l') and mn0[:-1] in COND_BRANCHES):
            return plain, use, tuple(VOLATILE), lisdef, fpregs, ('call', t, False, False)
        if mn0 == 'b':
            return plain, use, kills, lisdef, fpregs, ('tail', t, False, True)
        return plain, use, kills, lisdef, fpregs, ('cond', t, False, False)
    parts = ops.split(',') if ops else []
    if mn in SIGNED_IMM or mn in UNSIGNED_IMM or mn in SHIFTED_IMM:
        try:
            v = parse_int(parts[-1])
        except (ValueError, IndexError):
            v = None
        if v is not None:
            if mn in SHIFTED_IMM:
                u = (v << 16) & 0xFFFFFFFF
            elif mn in UNSIGNED_IMM:
                u = v & 0xFFFF if -0x8000 <= v < 0x10000 else v & 0xFFFFFFFF
            else:
                u = v & 0xFFFFFFFF
            src = parts[1] if len(parts) >= 3 else None
            dest = parts[0] if parts and parts[0].startswith('r') and mn[:3] not in ('cmp', 'tw', 'td') and mn[:2] not in ('tw', 'td') else None
            if mn in ('addi', 'addis') and src == 'r1':
                pass
            elif mn in ('addi', 'ori'):
                use = ('pair', src, v, mn, dest, u)
            else:
                plain = (u, mn)
            if dest:
                if mn == 'lis':
                    lisdef = (dest, u)
                else:
                    kills = (dest,)
            return plain, use, kills, lisdef, fpregs, branch
    k = []
    if len(parts) == 2 and '(' in parts[1]:
        mm = MEM_RE.match(ops)
        if mm:
            use = ('mem', mm.group(3), parse_int(mm.group(2)), mn)
            if mn.startswith(FPVEC_PREFIX):
                fpregs = (mm.group(3),)
            if mn.endswith('u'):
                k.append(mm.group(3))
    elif mn.startswith(FPVEC_PREFIX) and len(parts) == 3:
        fpregs = tuple(p for p in parts[1:] if p.startswith('r'))
    if parts and parts[0].startswith('r') and not mn.startswith(NO_GPR_DEST_PREFIX):
        k.append(parts[0])
    if mn.endswith('ux') and len(parts) == 3 and parts[1].startswith('r'):
        k.append(parts[1])
    return plain, use, tuple(k), lisdef, fpregs, branch


def _flow(dec, preds, term, goto_sources, prev_snap, base, sites):
    """One forward pass over a function body. Two register states:
      hi  = sound must-state: at a merge point (a label that a goto targets, or the instruction after an
            unconditional branch) a lis value survives only when every predecessor (fall-through + each goto source)
            carries the same value; back edges use the previous pass's snapshot (optimistic first pass, iterated
            to a fixed point by the caller).
      lin = the old linear heuristic (kept across labels): a pair it sees that hi does not is recorded with a
            '?' suffix (uncertain, hidden by default in q)."""
    hi, lin, full = {}, {}, {}
    snap = {}
    imms, drefs = [], []
    fpmark = set()
    for i, (plain, use, kills, lisdef, fpregs, branch) in enumerate(dec):
        if i == 0 or i in preds or term[i - 1]:
            states = [{}] if i == 0 else ([] if term[i - 1] else [hi])
            unseen_back = False
            for j in preds.get(i, ()):
                if j < i:
                    states.append(snap[j])
                else:
                    s = prev_snap.get(j)
                    if s is not None:
                        states.append(s)
                    else:
                        unseen_back = True   # first pass: optimistic (TOP), the identity of the meet
            if states:
                hi = _meet(states)
            elif unseen_back:
                # reached only by back edges not computed yet (a rotated loop entered by a jump past its head):
                # TOP is approximated by the state that flows in linearly; the next pass replaces it with the
                # back edges' real snapshots, and only a converged pass is kept, so the result does not depend on
                # this guess (a wrong guess can cost precision, never soundness)
                hi = dict(hi)
            else:
                hi = {}   # no known predecessor (unreachable, or reached in a way the code does not show)
            if i > 0 and term[i - 1]:
                full = {}
        site = sites[i]
        for r in fpregs:
            if r in full:
                fpmark.add(full[r])
        if plain is not None:
            imms.append((base, site, plain[0], plain[1]))
        newfull = None
        if use is not None:
            if use[0] == 'pair':
                _, src, v, mn, dest, u = use
                if src in hi or src in lin:
                    sure = src in hi
                    hv = hi[src] if sure else lin[src]
                    fv = (hv + v) & 0xFFFFFFFF if mn == 'addi' else (hv | (v & 0xFFFF))
                    op = 'lis+' + mn + ('' if sure else '?')
                    imms.append((base, site, fv, op))
                    drefs.append([base, site, fv, op, ''])
                    if dest:
                        newfull = (dest, len(drefs) - 1)
                    if not sure:
                        imms.append((base, site, u, mn))
                else:
                    imms.append((base, site, u, mn))
            else:
                _, b, disp, mn = use
                if b in hi or b in lin:
                    sure = b in hi
                    fv = ((hi[b] if sure else lin[b]) + disp) & 0xFFFFFFFF
                    op = 'lis+' + mn + ('' if sure else '?')
                    imms.append((base, site, fv, op))
                    drefs.append([base, site, fv, op, ''])
        for r in kills:
            hi.pop(r, None)
            lin.pop(r, None)
            full.pop(r, None)
        if lisdef is not None:
            hi[lisdef[0]] = lisdef[1]
            lin[lisdef[0]] = lisdef[1]
            full.pop(lisdef[0], None)
        if newfull is not None:
            full[newfull[0]] = newfull[1]
        if i in goto_sources:
            snap[i] = dict(hi)
    for k in fpmark:
        drefs[k][4] = 'fpvec'
    return imms, drefs, snap


def parse_file(args):
    """Worker: one recomp.N.cpp -> funcs, branches, imms, drefs."""
    path = args
    mapping = _MAPPING
    text = read_text(path)
    fname = os.path.basename(path)
    funcs, branches, imms, drefs = [], [], [], []
    stats = [0, 0, 0, 0]   # functions with back edges, their passes, functions not converged, gotos to no label
    pos = 0
    line_no = 1
    line_pos = 0
    while True:
        i = text.find(DEF, pos)
        if i < 0:
            break
        line_no += text.count('\n', line_pos, i)
        line_pos = i
        j = text.find(')', i)
        name = text[i + len(DEF):j]
        end = text.find('\n}\n', j)
        end = len(text) if end < 0 else end + 3
        body = text[i:end]
        pos = end
        if name in mapping:
            base = mapping[name]
        elif re.fullmatch(r'sub_[0-9A-Fa-f]{8}', name):
            base = int(name[4:], 16)
        else:
            continue
        addr = base
        resync = n_bctrl = n_bctr = 0
        sites, dec, gotos = [], [], []
        lab_idx = {}
        pending = []
        for m in LINE_RE.finditer(body):
            if m.group(4):
                if dec:
                    gotos.append((len(dec) - 1, int(m.group(4), 16)))
                continue
            if m.group(3):
                lab = int(m.group(3), 16)
                if lab != addr:
                    resync += 1
                    addr = lab
                pending.append(lab)
                continue
            if pending:
                for lab in pending:
                    lab_idx[lab] = len(dec)
                pending = []
            sites.append(addr)
            dec.append(decode(m.group(1), m.group(2).strip()))
            addr += 4
        fb = []
        term = []
        for k, d in enumerate(dec):
            br = d[5]
            t_flag = False
            if br is not None:
                kind, t, is_ctr, t_flag = br
                if kind == 'icall':
                    n_bctrl += 1
                elif kind == 'ijump':
                    n_bctr += 1
                elif kind is not None:
                    fb.append((sites[k], t, kind))
            term.append(t_flag)
        preds = {}
        back_sources = set()
        goto_sources = set()
        for src_i, tgt in gotos:
            k = lab_idx.get(tgt)
            if k is None:
                stats[3] += 1
                continue
            preds.setdefault(k, []).append(src_i)
            goto_sources.add(src_i)
            if src_i >= k:
                back_sources.add(src_i)
        prev = {}
        passes = 0
        while True:
            passes += 1
            f_imms, f_drefs, snap = _flow(dec, preds, term, goto_sources, prev, base, sites)
            if not back_sources or all(snap.get(j) == prev.get(j) for j in back_sources):
                break
            if passes >= 25:
                # not converged (not seen in practice): demote every pair of this function to uncertain
                stats[2] += 1
                f_imms = [(a, b, c, op + '?' if op.startswith('lis+') and not op.endswith('?') else op)
                          for a, b, c, op in f_imms]
                for d in f_drefs:
                    if not d[3].endswith('?'):
                        d[3] += '?'
                break
            prev = {j: snap[j] for j in back_sources if j in snap}
        if back_sources:
            stats[0] += 1
            stats[1] += passes
        imms += f_imms
        drefs += [tuple(d) for d in f_drefs]
        size = addr - base
        funcs.append((base, name, size, len(dec), fname, line_no, n_bctrl, n_bctr, resync))
        for site, t, kind in fb:
            # calls always (incl. recursion); other branches when the target is outside the own [start, end) range
            # (main() also keeps one that lands inside the own range but in ANOTHER function: overlapping ranges)
            branches.append((base, t, site, kind, 1 if base <= t < addr else 0))
    return funcs, branches, imms, drefs, stats


def scan_strings(img, code_lo, code_hi):
    """{guest addr: (enc, text)} for ASCII and UTF-16BE strings outside the code range."""
    out = {}
    regions = [(0, code_lo - IMAGE_BASE), (code_hi - IMAGE_BASE, len(img))]
    ascii_re = re.compile(rb'[\x20-\x7e\t\r\n]{4,}(?=\x00)')
    utf16_re = re.compile(rb'(?:\x00[\x20-\x7e]){4,}(?=\x00\x00)')
    for lo, hi in regions:
        seg = img[lo:hi]
        for m in ascii_re.finditer(seg):
            s = m.group(0).decode('ascii')
            if sum(c.isalnum() for c in s) * 2 < len(s):
                continue
            out[IMAGE_BASE + lo + m.start()] = ('ascii', s)
        for m in utf16_re.finditer(seg):
            if (lo + m.start()) & 1:
                continue
            s = m.group(0)[1::2].decode('ascii')
            a = IMAGE_BASE + lo + m.start()
            if a not in out:
                out[a] = ('utf16be', s)
    return out


def be32(img, a):
    o = a - IMAGE_BASE
    if o < 0 or o + 4 > len(img):
        return None
    return struct.unpack_from('>I', img, o)[0]


def scan_rtti(img, func_starts, code_lo, code_hi):
    """MSVC RTTI from the image: classes, vtables (+slots), base classes. See module docstring."""
    words = {}  # value -> list of addresses holding it (only for values we need, filled lazily below)
    n_words = len(img) // 4
    arr = struct.unpack('>%dI' % n_words, img[:n_words * 4])
    tds = {}
    for m in re.finditer(rb'\.\?A[VU][\x21-\x7e]+?@@\x00', img):
        td = IMAGE_BASE + m.start() - 8
        tds[td] = m.group(0)[:-1].decode('ascii')
    # index of word value -> addresses, restricted to TD addresses
    td_set = set(tds)
    cols = {}
    for idx, v in enumerate(arr):
        if v in td_set:
            col = IMAGE_BASE + idx * 4 - 12
            if be32(img, col) == 0:
                cols[col] = v
    col_set = set(cols)
    vtables = []
    for idx, v in enumerate(arr):
        if v in col_set:
            vt = IMAGE_BASE + idx * 4 + 4
            if code_lo <= vt < code_hi:
                continue
            slots = []
            a = vt
            while True:
                w = be32(img, a)
                if w is None or w not in func_starts:
                    break
                slots.append(w)
                a += 4
            vtables.append((vt, v, cols[v], slots))
    bases = []
    done = set()
    for col, td in sorted(cols.items()):
        if td in done:
            continue
        chd = be32(img, col + 16)
        if chd is None:
            continue
        cnt, barr = be32(img, chd + 8), be32(img, chd + 12)
        if cnt is None or barr is None or cnt > 64:
            continue
        for k in range(cnt):
            bcd = be32(img, barr + 4 * k)
            btd = be32(img, bcd) if bcd else None
            if btd in tds:
                bases.append((tds[td], k, tds[btd]))
                done.add(td)
    return tds, cols, vtables, bases, arr


def demangle(td_name):
    """'.?AVFoo@@' -> 'Foo'; nested '.?AVInner@Outer@@' -> 'Outer::Inner'. Templates left as-is."""
    s = td_name[4:]
    if s.endswith('@@'):
        s = s[:-2]
    if '?$' in s:
        return s
    parts = [p for p in s.split('@') if p]
    return '::'.join(reversed(parts))


def map_timestamp(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        for _, line in zip(range(10), f):
            if 'Timestamp is' in line:
                return line.strip()
    return None


def parse_map(path, sections=None):
    """Linker map -> [(rva, name, obj)]. With a `sections` list, also fills it with [rva_start, rva_end, name] per
    section (extent from the header's Start/Length table, RVA base from the symbols), so q can refuse an RVA that
    is outside every section instead of attributing it to the nearest symbol."""
    base = 0x140000000
    syms = []
    sec_len = {}
    sec_name = {}
    sec_rva = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            if 'Preferred load address is' in line:
                base = int(line.split()[-1], 16)
            m = re.match(r'\s*([0-9a-f]{4}):([0-9a-f]{8})\s+(\S+)\s+([0-9a-f]{16})\s+(?:f\s+)?(?:i\s+)?(\S+)?', line)
            if m:
                rva = int(m.group(4), 16) - base
                syms.append((rva, m.group(3), m.group(5) or ''))
                sec = int(m.group(1), 16)
                if sec and sec not in sec_rva:
                    sec_rva[sec] = rva - int(m.group(2), 16)
                continue
            m = re.match(r'\s*([0-9a-f]{4}):([0-9a-f]{8}) ([0-9a-f]{8})H\s+(\S+)', line)
            if m:
                sec = int(m.group(1), 16)
                sec_len[sec] = max(sec_len.get(sec, 0), int(m.group(2), 16) + int(m.group(3), 16))
                sec_name.setdefault(sec, m.group(4))
    if sections is not None:
        for sec, ln in sorted(sec_len.items()):
            if sec in sec_rva:
                sections.append([sec_rva[sec], sec_rva[sec] + ln, sec_name[sec]])
    return syms


def file_sig(path):
    st = os.stat(path)
    return {'path': path, 'size': st.st_size, 'mtime': int(st.st_mtime)}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def codegen_fingerprint(gen):
    try:
        with open(os.path.join(gen, 'codegen.stamp'), encoding='utf-8') as f:
            return json.load(f).get('fingerprint', '')
    except (OSError, ValueError):
        return ''


def source_sig(gen, prefix, map_path, image_path):
    """Everything the index is built from, cheaply (stat only + the codegen fingerprint). Taken before and after a
    build: a codegen or relink that ran in between (other agents' build.ps1) would leave a torn index."""
    sig = [codegen_fingerprint(gen)]
    paths = [os.path.join(gen, f'{prefix}_init.cpp')] + sorted(glob.glob(os.path.join(gen, f'{prefix}_recomp.*.cpp')))
    paths += [map_path, image_path]
    for p in paths:
        try:
            st = os.stat(p)
            sig.append((p, st.st_size, st.st_mtime_ns))
        except OSError:
            sig.append((p, None, None))
    return sig


def lower_priority():
    """Run below normal priority: a rebuild during someone's perf-measured game run must not steal CPU from it."""
    if sys.platform == 'win32':
        try:
            import ctypes
            k32 = ctypes.WinDLL('kernel32', use_last_error=True)
            k32.GetCurrentProcess.restype = ctypes.c_void_p
            k32.SetPriorityClass.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
            k32.SetPriorityClass.restype = ctypes.c_int
            if not k32.SetPriorityClass(k32.GetCurrentProcess(), 0x00004000):   # BELOW_NORMAL_PRIORITY_CLASS
                print(f'build_index: could not lower the priority (error {ctypes.get_last_error()})', file=sys.stderr)
        except (OSError, AttributeError):
            pass


def short_string_ok(img, a, raw):
    """A 2-3 character candidate at a code-taken address. Rejects big-endian float constants that look like text
    ('XY' + two zero bytes at an aligned address with X in '?'..'L' is a float in [0.5, 1e8) with a short
    mantissa, e.g. 1000.0f)."""
    if a % 4 == 0 and len(raw) == 2:
        o = a - IMAGE_BASE
        w = struct.unpack_from('>I', img, o)[0]
        if (w & 0xFFFF) == 0:
            f = abs(struct.unpack_from('>f', img, o)[0])
            if 0.5 <= f < 1e8:
                return False
    return True


def build(args, prefix, mapping, files, tmp):
    t0 = time.time()
    funcs, branches, imms, drefs = [], [], [], []
    st_back = st_passes = st_nonconv = st_nolabel = 0
    with multiprocessing.Pool(args.j, initializer=_init_worker, initargs=(mapping,)) as pool:
        for f, b, i, d, s in pool.imap_unordered(parse_file, files, chunksize=4):
            funcs += f
            branches += b
            imms += i
            drefs += d
            st_back += s[0]
            st_passes += s[1]
            st_nonconv += s[2]
            st_nolabel += s[3]
    t_parse = time.time() - t0
    funcs.sort()
    starts = [f[0] for f in funcs]
    func_set = set(starts)
    ends = {f[0]: f[0] + f[2] for f in funcs}
    names = {a: n for n, a in mapping.items()}
    # mapped addresses without a recompiled body (import thunks, helpers) still count as functions
    extra = [(a, n) for a, n in names.items() if a not in func_set]
    all_starts = sorted(set(starts) | {a for a, _ in extra})
    all_set = set(all_starts)

    def containing(a):
        k = bisect.bisect_right(starts, a) - 1
        if k >= 0 and a < ends[starts[k]]:
            return starts[k]
        return None

    def owner(a):
        k = bisect.bisect_right(all_starts, a) - 1
        return all_starts[k] if k >= 0 else None

    edges = []
    n_overlap_edges = 0
    for src, t, site, kind, in_range in branches:
        if in_range and kind != 'call':
            # inside the source's own [start, end): an internal branch, unless the range overlaps another function
            # that starts at or before the target (exception funclets, __savegprlr chains)
            if owner(t) == src:
                continue
            n_overlap_edges += 1
        if t in all_set:
            dst = t
        else:
            dst = containing(t)
        edges.append((src, dst, t, site, kind))

    # image-derived tables
    strings, strrefs, ptrs, rtti = {}, [], [], None
    img = None
    image_info = None
    code_lo = min(starts)
    code_hi = max(ends.values())
    n_short_rejected = 0
    if os.path.exists(args.image):
        with open(args.image, 'rb') as f:
            img = f.read()
        image_info = dict(file_sig(args.image), sha256=hashlib.sha256(img).hexdigest())
        strings = scan_strings(img, code_lo, code_hi)
        # pass 1: addresses taken by code (certain lis+addi / lis+ori) that are not a scanned string start: split a
        # run (e.g. a float byte glued in front of the string) or accept a short 2-3 char string. Not when every
        # use of the built address is a float/vector load or store (a constant, not text). Order-independent.
        run_starts = sorted(strings)
        cand = {}   # address -> [used other than as float/vector data, used as float/vector data]
        for fn, site, a, op, use in drefs:
            if op in ('lis+addi', 'lis+ori') and a not in strings:
                cu = cand.setdefault(a, [False, False])
                cu[1 if use == 'fpvec' else 0] = True
        added = {}
        for a in sorted(cand):
            if not (IMAGE_BASE <= a < IMAGE_BASE + len(img)) or code_lo <= a < code_hi:
                continue
            if not cand[a][0]:
                n_short_rejected += 1
                continue
            k = bisect.bisect_right(run_starts, a) - 1
            if k >= 0:
                st = run_starts[k]
                enc, text = strings[st]
                off = a - st
                if enc == 'ascii' and off < len(text):
                    added[a] = ('ascii', text[off:], 'split')
                    continue
                if enc == 'utf16be' and off % 2 == 0 and off // 2 < len(text):
                    added[a] = ('utf16be', text[off // 2:], 'split')
                    continue
            # short string: NUL-terminated, starts 4-aligned or after a NUL, has a letter/digit/%, not a float
            o = a - IMAGE_BASE
            e = img.find(bytes([0]), o, o + 4)
            if e > o + 1 and (a % 4 == 0 or img[o - 1] == 0):
                raw = img[o:e]
                if all(0x20 <= b < 0x7f for b in raw) and any(chr(b).isalnum() or b == 0x25 for b in raw):
                    if not cand[a][1] and short_string_ok(img, a, raw):
                        added[a] = ('ascii', raw.decode('ascii'), 'short')
                    else:
                        n_short_rejected += 1
        how_added = {a: v[2] for a, v in added.items()}
        strings.update({a: (v[0], v[1]) for a, v in added.items()})
        # pass 2: every reference (uncertain lis pairs keep their '?' op; q hides them unless --maybe)
        for fn, site, a, op, use in sorted(drefs):
            tag = ',fpvec' if use == 'fpvec' else ''
            if a in strings:
                strrefs.append((fn, site, a, how_added.get(a, 'direct') + tag, op))
            elif op.rstrip('?') == 'lis+lwz':
                w = be32(img, a)
                if w in strings:
                    strrefs.append((fn, site, w, 'ptr@%08X' % a, op))
        tds, cols, vtables, bases, arr = scan_rtti(img, all_set, code_lo, code_hi)
        string_set = set(strings)
        for idx, v in enumerate(arr):
            if v in all_set or v in string_set:
                where = IMAGE_BASE + idx * 4
                if code_lo <= where < code_hi:
                    continue
                ptrs.append((where, v, 'func' if v in all_set else 'str'))
        rtti = (tds, cols, vtables, bases)
    else:
        print(f'build_index: no image at {args.image}: strings/RTTI/data pointers skipped '
              f'(dump it with --wwe2k14_dump_image, see tools/q/README.md)', file=sys.stderr)

    sections = []
    syms = parse_map(args.map, sections) if os.path.exists(args.map) else []

    # write
    con = sqlite3.connect(tmp)
    c = con.cursor()
    c.executescript('''
        PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;
        CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
        CREATE TABLE funcs(addr INTEGER PRIMARY KEY, name TEXT, size INTEGER, insns INTEGER, file TEXT,
                           line INTEGER, n_bctrl INTEGER, n_bctr INTEGER, resync INTEGER, has_body INTEGER);
        CREATE TABLE edges(src INTEGER, dst INTEGER, target INTEGER, site INTEGER, kind TEXT);
        CREATE TABLE imms(func INTEGER, site INTEGER, value INTEGER, op TEXT);
        CREATE TABLE strings(addr INTEGER PRIMARY KEY, enc TEXT, text TEXT);
        CREATE TABLE strrefs(func INTEGER, site INTEGER, addr INTEGER, how TEXT, op TEXT);
        CREATE TABLE ptrs(at INTEGER, value INTEGER, kind TEXT);
        CREATE TABLE rtti_class(td INTEGER PRIMARY KEY, mangled TEXT, name TEXT);
        CREATE TABLE rtti_col(col INTEGER PRIMARY KEY, td INTEGER, offset INTEGER);
        CREATE TABLE rtti_vtable(vt INTEGER PRIMARY KEY, col INTEGER, td INTEGER, nslots INTEGER);
        CREATE TABLE rtti_slot(vt INTEGER, idx INTEGER, func INTEGER);
        CREATE TABLE rtti_base(cls TEXT, idx INTEGER, base TEXT);
        CREATE TABLE hostsyms(rva INTEGER, name TEXT, obj TEXT);
    ''')
    c.executemany('INSERT INTO funcs VALUES (?,?,?,?,?,?,?,?,?,1)', funcs)
    c.executemany('INSERT INTO funcs VALUES (?,?,0,0,NULL,NULL,0,0,0,0)', extra)
    c.executemany('INSERT INTO edges VALUES (?,?,?,?,?)', edges)
    c.executemany('INSERT INTO imms VALUES (?,?,?,?)', imms)
    c.executemany('INSERT INTO strings VALUES (?,?,?)', [(a, e, s) for a, (e, s) in strings.items()])
    c.executemany('INSERT INTO strrefs VALUES (?,?,?,?,?)', strrefs)
    c.executemany('INSERT INTO ptrs VALUES (?,?,?)', ptrs)
    if rtti:
        tds, cols, vtables, bases = rtti
        c.executemany('INSERT INTO rtti_class VALUES (?,?,?)', [(td, n, demangle(n)) for td, n in tds.items()])
        c.executemany('INSERT INTO rtti_col VALUES (?,?,?)', [(col, td, be32(img, col + 4)) for col, td in cols.items()])
        c.executemany('INSERT OR IGNORE INTO rtti_vtable VALUES (?,?,?,?)',
                      [(vt, col, td, len(sl)) for vt, col, td, sl in vtables])
        c.executemany('INSERT INTO rtti_slot VALUES (?,?,?)',
                      [(vt, k, fn) for vt, col, td, sl in vtables for k, fn in enumerate(sl)])
        c.executemany('INSERT INTO rtti_base VALUES (?,?,?)', [(demangle(a), k, demangle(b)) for a, k, b in bases])
    c.executemany('INSERT INTO hostsyms VALUES (?,?,?)', syms)
    c.executescript('''
        CREATE INDEX edges_src ON edges(src); CREATE INDEX edges_dst ON edges(dst);
        CREATE INDEX imms_value ON imms(value); CREATE INDEX imms_func ON imms(func);
        CREATE INDEX strrefs_func ON strrefs(func); CREATE INDEX strrefs_addr ON strrefs(addr);
        CREATE INDEX ptrs_value ON ptrs(value); CREATE INDEX ptrs_at ON ptrs(at);
        CREATE INDEX funcs_name ON funcs(name);
        CREATE INDEX rtti_slot_func ON rtti_slot(func); CREATE INDEX rtti_slot_vt ON rtti_slot(vt);
        CREATE INDEX hostsyms_rva ON hostsyms(rva); CREATE INDEX hostsyms_name ON hostsyms(name);
    ''')
    init_path = os.path.join(args.gen, f'{prefix}_init.cpp')
    n_pairs = sum(1 for x in imms if x[3].startswith('lis+'))
    n_unsure = sum(1 for x in imms if x[3].startswith('lis+') and x[3].endswith('?'))
    meta = {
        'schema': SCHEMA_VERSION,
        'built_at': time.strftime('%Y-%m-%d %H:%M:%S'),
        'gen_dir': args.gen,
        'default_gen': os.path.normcase(os.path.abspath(args.gen)) == os.path.normcase(os.path.abspath(GEN)),
        'codegen_fingerprint': codegen_fingerprint(args.gen),
        'init_cpp': dict(file_sig(init_path), sha256=sha256_file(init_path)),
        'recomp_files': len(files),
        'image': image_info,
        'map': dict(file_sig(args.map), sha256=sha256_file(args.map), timestamp=map_timestamp(args.map)) if syms else None,
        'map_sections': sections,
        'code_range': [code_lo, code_hi],
        'counts': {'funcs': len(funcs), 'mapped_without_body': len(extra), 'edges': len(edges), 'imms': len(imms),
                   'lis_pairs': n_pairs, 'lis_pairs_uncertain': n_unsure,
                   'strings': len(strings), 'strrefs': len(strrefs), 'ptrs': len(ptrs),
                   'rtti_classes': len(rtti[0]) if rtti else 0, 'rtti_vtables': len(rtti[2]) if rtti else 0,
                   'hostsyms': len(syms)},
        'flow': {'funcs_with_loops': st_back, 'loop_passes': st_passes, 'not_converged': st_nonconv,
                 'gotos_without_label': st_nolabel, 'edges_into_overlapping_funcs': n_overlap_edges,
                 'short_or_split_rejected': n_short_rejected},
    }
    meta['build_seconds'] = round(time.time() - t0, 1)
    meta['parse_seconds'] = round(t_parse, 1)
    c.executemany('INSERT INTO meta VALUES (?,?)', [(k, json.dumps(v)) for k, v in meta.items()])
    con.commit()
    con.close()
    return meta, image_info


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', default=GEN)
    ap.add_argument('--image', default=IMAGE)
    ap.add_argument('--map', default=MAP)
    ap.add_argument('--out', default=None,
                    help='default out/harness/index/index.db; with a --gen other than generated/default: '
                         'out/harness/index/index_<scratch name>.db (never overwrites the main index by accident)')
    ap.add_argument('-j', type=int, default=max(1, min(16, (os.cpu_count() or 4) - 2)))
    args = ap.parse_args()
    if args.out is None:
        if os.path.normcase(os.path.abspath(args.gen)) == os.path.normcase(os.path.abspath(GEN)):
            args.out = DB
        else:
            g = os.path.abspath(args.gen)
            tag = os.path.basename(os.path.dirname(g)) if os.path.basename(g) == 'default' else os.path.basename(g)
            args.out = os.path.join(OUT_DIR, 'index_' + re.sub(r'[^\w.-]', '_', tag) + '.db')
            print(f'build_index: --gen is not generated/default: writing {args.out} (pass --out to choose)',
                  file=sys.stderr)
    lower_priority()

    t0 = time.time()
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    tmp = f'{args.out}.{os.getpid()}.tmp'
    try:
        for attempt in range(1, 4):
            prefix, mapping = load_mapping(args.gen)
            sig0 = source_sig(args.gen, prefix, args.map, args.image)
            files = sorted(glob.glob(os.path.join(args.gen, f'{prefix}_recomp.*.cpp')),
                           key=lambda p: int(re.search(r'\.(\d+)\.cpp$', p).group(1)))
            if os.path.exists(tmp):
                os.remove(tmp)
            meta, image_info = build(args, prefix, mapping, files, tmp)
            if source_sig(args.gen, prefix, args.map, args.image) == sig0:
                break
            print(f'build_index: the generated code, map or image changed while building (a codegen or relink ran); '
                  f'{"retrying" if attempt < 3 else "giving up"} (attempt {attempt} of 3)', file=sys.stderr)
            if attempt < 3:
                time.sleep(15)
        else:
            raise SystemExit('build_index: sources kept changing during the build; the old index is left in place. '
                             'Rebuild once the codegen/relink has finished.')
        # a q.py reading the old index holds it open (Windows: no replace while open); queries take < 1 s, so retry
        for attempt in range(60):
            try:
                os.replace(tmp, args.out)
                break
            except PermissionError:
                if attempt == 59:
                    raise
                time.sleep(0.5)
    finally:
        if os.path.exists(tmp):
            try:
                os.remove(tmp)
            except OSError:
                pass
    print(f'index: {args.out}')
    print(f'  built in {round(time.time() - t0, 1)} s (last attempt {meta["build_seconds"]} s, parse '
          f'{meta["parse_seconds"]} s, {args.j} workers, below-normal priority)')
    print('  ' + ', '.join(f'{k} {v}' for k, v in meta['counts'].items()))
    print('  ' + ', '.join(f'{k} {v}' for k, v in meta['flow'].items()))
    print(f'  codegen fingerprint {meta["codegen_fingerprint"]}, image '
          f'{image_info["sha256"][:16] if image_info else "none"}, map {meta["map"]["sha256"][:16] if meta["map"] else "none"}')


if __name__ == '__main__':
    main()
