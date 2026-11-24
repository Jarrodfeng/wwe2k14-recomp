r"""S110 lane FEED: list every site where our renderer's feed code reads the game's memory, one row per field read.

    py -I out\s110\feed\feed_fields.py [--tsv docs\research\s110\feed_fields.tsv]

Scope (mechanical): every .h / .cpp / .inc under the feed dirs (FEED_DIRS; unit tests excluded) plus each top-level src file
that includes a header from them. A read site is a call of one of the reader shapes in READERS (the guest-read helpers our
feed code uses), a register of a hook (ctx.rN / ctx.fN, writes excluded), a GuestBlock hull load, a TranslateVirtual / Host
host pointer, a physical read (PhysRead / ReadPhys / Phys / phys: texture and buffer bytes by physical address), or a
deref of a host copy (LoadBE32 / F32FromBe / BeFloat4 of a local buffer: the guest address is not on the line). Comments and
string literals are stripped. The address argument is split into base + constant offset (+ index terms); named constants
(k-prefixed or ALL_CAPS names only) are resolved from the constexpr / const definitions in the scanned files.
kind 'forward' = a read whose address is a bare parameter of the enclosing function at offset 0: a helper passing its
caller's address on (the caller's row is the field).

Writer status (from the q field index out\harness\index\fields.s2.db, read-only):
  K      absolute guest address: q field rows with root K at that address, rw W
  image  absolute address with no K writer whose word in the guest image (out\harness\index\image.bin) is non-zero:
         initialised XEX data (a constant table, or written only by a store q field does not model) [L]
  G      base loaded from a global word in the 40 lines above: rows with root g:GLOBAL (or argflow of it) at OFF, rw W
  T      base is a hook argument (ctx.rN of REX_HOOK_RAW(sub_X), directly or through a local assigned from it) whose type
         q field knows, or whose callers pass a global: rows of that type / global at OFF, rw W
  named  our source names a game writer next to the read (sub_XXXXXXXX with a write verb, same line or 3 above) [L]
  none   no writer known; `cand` = distinct game functions that write +OFF on ANY base (q field OFF --writes, unnarrowed)
Self-audit: every function in scope that calls a raw guest-read primitive (TranslateVirtual / SafeCopy / GuestMem()) and
is not a READERS shape is printed as an UNLISTED HELPER (its callers are not rows).
"""
import argparse
import os
import re
import sqlite3
import struct
import sys
from collections import Counter, defaultdict

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))   # tools/q -> the repo root
SRC = os.path.join(ROOT, 'src')
FEED_DIRS = ['scene_live', 'renderer_scene', 'renderer_adapter', 'renderer_game', 'renderer_host']
DB = os.path.join(ROOT, 'out', 'harness', 'index', 'fields.s2.db')
IMAGE = os.path.join(ROOT, 'out', 'harness', 'index', 'image.bin')
if not os.path.exists(DB):   # S110 FEEDKEY: a worktree has no index of its own: the main checkout's (build_fields.default_paths)
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import build_fields as _bf
        _g, IMAGE, DB = _bf.default_paths()
    except (SystemExit, ImportError, OSError):
        pass
IMAGE_BASE = 0x82000000   # tools\q\build_fields.py IMAGE_BASE

# name -> ({nargs: address arg index}, width rule, kind); width rule: int bytes | ('arg', i, mul) = args[i] * mul | 'hull' | ('argn', 4)
READERS = {
    'ReadWord': ({3: 1, 2: 0}, 4, 'field'),
    'Word': ({3: 1}, 4, 'field'),
    'Bytes': ({4: 1}, ('arg', 3, 1), 'field'),
    'ReadGuestByte': ({3: 1}, 1, 'field'),
    'ReadWordBE': ({3: 1}, 4, 'field'),
    'ReadFloatsBE': ({4: 1}, ('arg', 3, 4), 'field'),
    'ReadFloats': ({4: 1, 3: 0}, ('argn', 4), 'field'),
    'ReadFloat': ({3: 1, 2: 0}, 4, 'field'),
    'ReadWords': ({4: 1, 3: 0}, ('argn', 4), 'field'),
    'ReadGuest': ({3: 0}, ('arg', 2, 1), 'field'),
    'ReadGuestDirect': ({3: 0}, ('arg', 2, 1), 'field'),
    'ReadGuestRows': ({5: 0}, ('arg', 2, 0), 'field'),
    'ReadMatrices': ({3: 0}, ('arg', 1, 64), 'field'),
    'CompareGuest': ({3: 0}, ('arg', 2, 1), 'field'),
    'GuestU32': ({1: 0}, 4, 'field'),
    'SkelCopy': ({3: 0}, ('arg', 1, 1), 'field'),
    'SafeRead32': ({2: 0}, 4, 'field'),
    'read': ({3: 0}, ('arg', 2, 1), 'field'),
    'rd': ({2: 0, 3: 0}, 4, 'field'),
    'Ld32': ({2: 1, 1: 0}, 4, 'field'), 'Rd32': ({2: 1}, 4, 'field'), 'LdF': ({2: 1}, 4, 'field'), 'RdF': ({2: 1}, 4, 'field'),
    'Ld16': ({2: 1}, 2, 'field'), 'Rd16': ({2: 1}, 2, 'field'), 'Ld8': ({2: 1}, 1, 'field'), 'Rd8': ({2: 1}, 1, 'field'),
    'Ld64': ({2: 1}, 8, 'field'), 'ProbeWord': ({2: 1}, 4, 'field'), 'ProbeFloat': ({2: 1}, 4, 'field'),
    'Readable': ({3: 1}, ('arg', 2, 1), 'check'),
    'Host': ({2: 1}, 0, 'host_ptr'),
    'GuestRangePtr': ({2: 0}, ('arg', 1, 1), 'host_ptr'),
    'u32': ({1: 0}, 4, 'field'), 'f32': ({1: 0}, 4, 'field'), 'u16': ({1: 0}, 2, 'field'), 'u8': ({1: 0}, 1, 'field'),
    'words': ({3: 0}, ('arg', 1, 4), 'field'), 'bytes': ({3: 0}, ('arg', 1, 1), 'field'),
    'U8': ({2: 1}, 1, 'field'), 'U16': ({2: 1}, 2, 'field'),
    'Load': ({3: 1}, 'hull', 'hull'),
    'TranslateVirtual': ({1: 0}, 0, 'host_ptr'),
    'PhysRead': ({3: 0}, ('arg', 1, 1), 'phys'), 'ReadPhys': ({3: 0}, ('arg', 1, 1), 'phys'),
    'Phys': ({3: 0}, ('arg', 1, 1), 'phys'), 'phys': ({3: 0}, ('arg', 1, 1), 'phys'),
    'LoadBE32': ({1: 0}, 4, 'copy_deref'), 'F32FromBe': ({1: 0}, 4, 'copy_deref'), 'BeFloat4': ({2: 0, 1: 0}, 16, 'copy_deref'),
}
METHOD_ONLY = {'Word', 'Bytes', 'u32', 'f32', 'u16', 'u8', 'words', 'bytes', 'Load'}   # need a `.` or `->` before them
RAW_PRIMS = re.compile(r'\b(TranslateVirtual|SafeCopy|GuestMem\(\)|virtual_membase|membase\(\))')
CALL_RE = re.compile(r'(\.|->|::|\b)(' + '|'.join(sorted(READERS, key=len, reverse=True)) + r')\s*(<[^<>()]*>)?\s*\(')
REG_RE = re.compile(r'\bctx\.(r|f|v)(\d+)\b(\.(?:u64|u32|s64|s32|f64)\s*=(?!=))?')
HOOK_RE = re.compile(r'REX_HOOK\w*\s*\(\s*(sub_[0-9A-Fa-f]{8})')
MIDASM_RE = re.compile(r'\b(?:void|bool)\s+(\w+)\s*\(\s*PPCContext\s*&')
DEF_RE = re.compile(r'^\s*(?:template\s*<[^>]*>\s*)?(?:(?:inline|static|constexpr|virtual|extern|const|unsigned|struct)\s+)*'
                    r'[\w:<>,\*&\s]+?\b(\w+)\s*\(([^;{}]*)\)\s*(?:const\s*)?(?:noexcept\s*)?\{')
LAMBDA_RE = re.compile(r'\b(?:const\s+)?auto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(([^)]*)\)')
CONST_RE = re.compile(r'\b(?:constexpr|const)\s+(?:static\s+)?(?:std::)?(?:u?int(?:8|16|32|64)_t|unsigned|int|size_t|uint)\s+([^;(){}]+?);', re.S)
CONST_NAME = re.compile(r'^(?:k[A-Z0-9_]\w*|[A-Z][A-Z0-9_]{2,})$')
WRITE_VERB = re.compile(r'(writ|stor|fill|sets\b|set by|copies|stamp|builds|computes|updates)', re.I)
SUB_RE = re.compile(r'sub_[0-9A-Fa-f]{8}')
FAMILY_RULES = [   # (family, substrings of 'basename func' lower-case); first match wins; 'reader.h' is matched as the exact file name
    ('d3d_state', ['d3d_', 'binder', 'd3_record', 'd3_guest_core', 'const_override', 'bank_', 'draw_origin', 'tail_release', 'surface_role']),
    ('not_drawn', ['owner_timing', 'load_probe', 'span_timer', 'session_log', 'half2', 'vb_unlock', 'safe_guest_pages', 'rdr_switches',
                   'fp_probe']),
    ('readback', ['readback', 'thumb']),
    ('ui', ['quads2d', 'ui_', 'overlay']),
    ('crowd', ['crowd', 'card', 'audience', 'member_thin']),
    ('particle', ['particle', 'flash', 'stage_fx', 'stagefx', 'sprite', 'part_clock']),
    ('light', ['light', 'caster', 'sphere', 'prelight', 'rim', 'circle', 'shadow', 'lighting']),
    ('camera', ['camera', 'cam_', 'eye', 'cinecam']),
    ('material', ['material', 'colour', 'paint', 'tex', 'picture', 'liquid', 'gsf', 'wrk', 'sweat', 'tint', 'page_skip']),
    ('arena', ['arena', 'ring', 'plane', 'screen', 'movie', 'stage', 'weapon', 'rope', 'prop', 'sign']),
    ('model', ['wrestler', 'caw', 'attire', 'skin', 'model', 'piece', 'slot', 'bone', 'char', 'node', 'belt', 'own_rows', 'inputs']),
]


def scope_files():
    files = []
    for d in FEED_DIRS:
        for dp, _dn, fn in os.walk(os.path.join(SRC, d)):
            for f in fn:
                if f.endswith(('.h', '.cpp', '.inc')) and not f.endswith('_test.cpp'):
                    files.append(os.path.join(dp, f))
    inc = re.compile(r'#include\s+"(' + '|'.join(FEED_DIRS) + r')/')
    for f in os.listdir(SRC):
        p = os.path.join(SRC, f)
        if os.path.isfile(p) and f.endswith(('.h', '.cpp')):
            with open(p, encoding='utf-8', errors='replace') as fh:
                if inc.search(fh.read()):
                    files.append(p)
    return sorted(files)


def strip_code(lines):
    """comments and string literal contents removed, line count kept"""
    out, in_block = [], False
    for ln in lines:
        s, i, buf = ln, 0, []
        while i < len(s):
            if in_block:
                j = s.find('*/', i)
                if j < 0:
                    i = len(s)
                    break
                in_block, i = False, j + 2
                continue
            c = s[i]
            if s.startswith('//', i):
                break
            if s.startswith('/*', i):
                in_block, i = True, i + 2
                continue
            if c == '"':
                j = i + 1
                while j < len(s) and s[j] != '"':
                    j += 2 if s[j] == '\\' else 1
                buf.append('""')
                i = j + 1
                continue
            if c == "'":
                j = i + 1
                while j < len(s) and s[j] != "'":
                    j += 2 if s[j] == '\\' else 1
                buf.append("' '")
                i = j + 1
                continue
            buf.append(c)
            i += 1
        out.append(''.join(buf))
    return out


def split_args(text, start):
    """text[start] is just past '('; returns (list of args, end index) or (None, None)"""
    depth, cur, args, i = 0, [], [], start
    while i < len(text):
        c = text[i]
        if c in '([{':
            depth += 1
        elif c in ')]}':
            if depth == 0:
                args.append(''.join(cur).strip())
                return ([] if args == [''] else args), i
            depth -= 1
        if c == ',' and depth == 0:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    return None, None


def load_consts(files):
    raw = {}
    for p in files + [os.path.join(SRC, 'structs.h')]:
        if not os.path.exists(p):
            continue
        with open(p, encoding='utf-8', errors='replace') as fh:
            text = '\n'.join(strip_code(fh.read().splitlines()))
        for m in CONST_RE.finditer(text):
            for part in m.group(1).split(','):
                mm = re.match(r'\s*(\w+)\s*(?:=\s*(.+)|\{(.+)\})\s*$', part, re.S)
                if mm and CONST_NAME.match(mm.group(1)):
                    raw.setdefault(mm.group(1), (mm.group(2) or mm.group(3) or '').strip())
    vals = {}
    for _ in range(4):
        for k, e in raw.items():
            if k not in vals:
                v = eval_int(e, vals)
                if v is not None:
                    vals[k] = v
    return vals


def clean(e):
    e = re.sub(r'\b(?:static_cast<[^>]+>|u?int(?:8|16|32|64)_t|size_t|unsigned)\s*\(', '(', e)
    e = re.sub(r'\b(0[xX][0-9A-Fa-f]+|\d+)(?:ull|ULL|u|U|ll|LL|l|L)\b', r'\1', e)
    return re.sub(r'\s+', '', e)


def eval_int(e, consts):
    e = clean(e)
    if not e or not re.fullmatch(r'[\w\+\-\*\(\)<>|&~/]+', e):
        return None
    env = {}
    for n in re.findall(r'\b[A-Za-z_]\w*\b', e):
        if n not in consts:
            return None
        env[n] = consts[n]
    try:
        v = eval(e.replace('/', '//'), {'__builtins__': {}}, env)
    except Exception:
        return None
    return v & 0xFFFFFFFF if isinstance(v, int) else None


def split_terms(e):
    terms, depth, cur, sign = [], 0, [], '+'
    for c in e:
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c in '+-' and depth == 0 and cur and cur[-1] not in '-<>':
            terms.append((sign, ''.join(cur)))
            cur, sign = [], c
        else:
            cur.append(c)
    if cur:
        terms.append((sign, ''.join(cur)))
    return terms


def parse_addr(expr, consts):
    """-> (base, offset or None, indexed flag); base 'ABS' = no variable base (an absolute address, + idx when indexed)"""
    e = clean(expr)
    while e.startswith('(') and e.endswith(')') and split_args(e, 1)[1] == len(e) - 1:
        e = e[1:-1]
    v = eval_int(e, consts)
    if v is not None:
        return 'ABS', v, False
    base, off, idx = [], 0, False
    for sign, t in split_terms(e):
        tv = eval_int(t, consts)
        if tv is not None:
            off += tv if sign == '+' else -tv
        elif re.fullmatch(r'[A-Za-z_][\w\.]*(?:->\w+)*', t) and sign == '+':
            base.append(t)
        else:
            idx = True
    if not base:
        return ('ABS' if off else '?'), (off & 0xFFFFFFFF), idx
    return '+'.join(base), off, idx


def family(path, func):
    if os.path.basename(path) == 'reader.h':
        return 'd3d_state'
    key = (os.path.basename(path) + ' ' + (func or '')).lower()
    for fam, keys in FAMILY_RULES:
        if any(k in key for k in keys):
            return fam
    return 'other'


def param_names(sig):
    out = set()
    for p in (sig or '').split(','):
        m = re.search(r'(\w+)\s*(?:=[^,]*)?$', p.strip())
        if m:
            out.add(m.group(1))
    return out


class Writers:
    def __init__(self):
        self.ok = os.path.exists(DB)
        self.con = sqlite3.connect('file:' + DB.replace('\\', '/') + '?mode=ro', uri=True) if self.ok else None
        self.cache = {}
        self.img = open(IMAGE, 'rb').read() if os.path.exists(IMAGE) else b''
        if self.ok:
            tabs = {r[0] for r in self.con.execute("select name from sqlite_master where type='table'")}
            self.has_flow = 'argflow' in tabs
            self.root_id = {t: i for i, t in self.con.execute('select id, text from roots where text in (?, ?, ?, ?, ?, ?, ?, ?, ?)',
                                                                  ('K', 'a0', 'a1', 'a2', 'a3', 'a4', 'a5', 'a6', 'a7'))}

    def image_word(self, a):
        o = (a & ~3) - IMAGE_BASE
        return struct.unpack_from('>I', self.img, o)[0] if 0 <= o and o + 4 <= len(self.img) else None

    def q(self, key, sql, args):
        if key not in self.cache:
            self.cache[key] = self.con.execute(sql, args).fetchone()[0]
        return self.cache[key]

    def abs_(self, addr):
        return self.q(('K', addr), "select count(distinct r.func) from rows r where r.off=? and r.rw='W' and r.root=?",
                      (addr, self.root_id.get('K', -1)))

    def glob(self, g, off):
        gt = 'g:%08X' % g
        sql = ("select count(distinct r.func) from rows r join roots ro on ro.id=r.root where r.off=? and r.rw='W' and (ro.text=?"
               + (" or exists (select 1 from argflow af join roots gr on gr.id=af.groot where af.func=r.func and af.argroot=r.root and gr.text=?)"
                  if self.has_flow else '') + ')')
        return self.q(('G', g, off), sql, (off, gt, gt) if self.has_flow else (off, gt))

    def typed(self, func, argi, off):
        """-> (how, writer count): the hook's argument typed by a vtable, or passed a global by its callers"""
        rid = self.root_id.get('a%d' % argi)
        if rid is None:
            return '', 0
        vts = [v for (v,) in self.con.execute('select distinct vt from objtype where root=? and (func=? or func=0)', (rid, func))]
        if vts:
            sql = ("select count(distinct r.func) from rows r where r.off=? and r.rw='W' and exists (select 1 from objtype o where o.vt in (%s) "
                   "and o.root=r.root and (o.func=r.func or o.func=0))" % ','.join(str(v) for v in vts))
            n = self.q(('T', func, argi, off), sql, (off,))
            if n:
                return 'vt', n
        if self.has_flow:
            for (gt,) in self.con.execute('select gr.text from argflow af join roots gr on gr.id=af.groot where af.func=? and af.argroot=? limit 2',
                                          (func, rid)):
                m = re.match(r'g:([0-9A-F]{8})$', gt)
                if m:
                    n = self.glob(int(m.group(1), 16), off)
                    if n:
                        return gt, n
        return '', 0

    def cand(self, off):
        return self.q(('C', off), "select count(distinct func) from rows where off=? and rw='W'", (off,))


def find_global(code, i, var, consts):
    pat = [re.compile(r'\b' + re.escape(var) + r'\s*=\s*[\w:\.]*(?:u32|Ld32|Rd32|ReadU32|GuestU32)\s*\(\s*(?:\w+\s*,\s*)?([\w:x]+)\s*\)'),
           re.compile(r'ReadWord\s*\(\s*(?:\w+\s*,\s*)?([\w:x]+)\s*,\s*' + re.escape(var) + r'\s*\)')]
    for j in range(i, max(-1, i - 40), -1):
        for p in pat:
            m = p.search(code[j])
            if m:
                v = eval_int(m.group(1).split('::')[-1], consts)
                return v if v is not None and v >= 0x80000000 else None
    return None


def find_arg(code, i, var):
    """a local assigned from a hook register in the 40 lines above -> register number"""
    p = re.compile(r'\b' + re.escape(var) + r'\s*=\s*ctx\.r(\d+)\.u(?:32|64)\b')
    for j in range(i, max(-1, i - 40), -1):
        m = p.search(code[j])
        if m:
            return int(m.group(1))
    return None


def named_writer(orig, i):
    for j in range(max(0, i - 3), i + 1):
        t = orig[j]
        if SUB_RE.search(t) and WRITE_VERB.search(t) and ('//' in t or j < i):
            return True
    return False


def collect(W=None):
    """-> (rows, files, prim_funcs, Writers): the table rows (main() writes them as the tsv; q feed keys them, qfeedkey.py)"""
    files = scope_files()
    consts = load_consts(files)
    W = W or Writers()
    rows, prim_funcs = [], defaultdict(set)
    for p in files:
        rel = os.path.relpath(p, ROOT).replace('\\', '/')
        with open(p, encoding='utf-8', errors='replace') as fh:
            orig = fh.read().splitlines()
        code = strip_code(orig)
        func, hook, params = None, None, set()
        for i, ln in enumerate(code):
            m = DEF_RE.match(ln)
            if m and m.group(1) not in ('if', 'for', 'while', 'switch', 'return', 'catch'):
                func, params = m.group(1), param_names(m.group(2))
            else:
                lm = LAMBDA_RE.search(ln)
                if lm:
                    func, params = lm.group(1), param_names(lm.group(2))
            hm = HOOK_RE.search(ln)
            if hm:
                func, hook, params = hm.group(1), int(hm.group(1)[4:], 16), set()
            elif MIDASM_RE.search(ln) and not ln.rstrip().endswith(';'):
                hook = None
            if RAW_PRIMS.search(ln) and func:
                prim_funcs[func].add(rel)
            joined, k = ln, i
            while joined.count('(') > joined.count(')') and k < i + 6 and k + 1 < len(code):
                k += 1
                joined += ' ' + code[k]
            for cm in CALL_RE.finditer(ln):
                sep, name = cm.group(1), cm.group(2)
                if name in METHOD_ONLY and sep not in ('.', '->'):
                    continue
                if sep == '::' and name in ('read', 'rd', 'phys'):
                    continue
                pre = ln[:cm.start(2)]
                if sep != '.' and re.search(r'\b(?:bool|uint32_t|uint8_t\*|float|void|auto|int|static|inline)\s*[\w:]*$', pre.rstrip()[-40:]):
                    continue   # a definition, not a call
                args, _end = split_args(joined, cm.end())
                if args is None:
                    continue
                idxmap, wrule, kind = READERS[name]
                ai = idxmap.get(len(args))
                if ai is None or ai >= len(args):
                    continue
                base, off, indexed = parse_addr(args[ai], consts)
                if kind == 'field' and off == 0 and not indexed and base in params:
                    kind = 'forward'
                if isinstance(wrule, int):
                    width = str(wrule)
                elif wrule == 'hull':
                    width = 'hull'
                elif wrule[0] == 'arg':
                    if wrule[1] < len(args):
                        v = eval_int(args[wrule[1]], consts)
                        width = str(v * wrule[2]) if v is not None and wrule[2] else ('n=' + clean(args[wrule[1]]))
                    else:
                        width = '?'
                else:
                    width = '4*n'
                status, nw, cand = 'none', '', ''
                if base == 'ABS' and off is not None and off >= 0x80000000:
                    if W.ok and not indexed:
                        n = W.abs_(off)
                        status, nw = ('K' if n else 'none'), str(n)
                        if not n and W.image_word(off):
                            status = 'image'
                elif base not in ('?', 'ABS') and off is not None and kind in ('field', 'check', 'hull'):
                    rm = re.fullmatch(r'ctx\.r(\d+)\.u(?:32|64)', base)
                    reg = int(rm.group(1)) if rm else (find_arg(code, i, base) if re.fullmatch(r'\w+', base) else None)
                    if reg and hook and 3 <= reg <= 10 and W.ok:
                        how, n = W.typed(hook, reg - 3, off)
                        if n:
                            status, nw = 'T', '%d@%s(sub_%08X a%d)' % (n, how, hook, reg - 3)
                    if status == 'none' and re.fullmatch(r'\w+', base):
                        g = find_global(code, i, base, consts)
                        if g is not None and W.ok:
                            n = W.glob(g, off)
                            if n:
                                status, nw = 'G', '%d@g:%08X' % (n, g)
                    if status == 'none' and W.ok and kind == 'field':
                        cand = str(W.cand(off))
                if status == 'none' and kind != 'forward' and named_writer(orig, i):
                    status = 'named'
                addr_txt = ('0x%08X' % off + ('+idx' if indexed else '')) if base == 'ABS' and off is not None else (
                    base + ('+%d' % off if off else '') + ('+idx' if indexed else ''))
                rows.append(dict(file=rel, line=i + 1, func=func or '', reader=name, kind=kind, addr=addr_txt,
                                 base=base, off='' if off is None else str(off), width=width,
                                 family=family(rel, func), status=status, writers=nw, cand=cand))
            for rm in REG_RE.finditer(ln):
                if rm.group(3):
                    continue   # a write of the register (the hook's return), not a read
                rows.append(dict(file=rel, line=i + 1, func=func or '', reader='ctx.%s%s' % (rm.group(1), rm.group(2)), kind='reg',
                                 addr=('hook sub_%08X' % hook) if hook else 'mid-asm', base='reg', off='', width='reg',
                                 family=family(rel, func), status='hookarg', writers='', cand=''))
    seen, uniq = set(), []   # one row per (file, line, reader, addr): a line naming the same field twice is one read
    for r in rows:
        k = (r['file'], r['line'], r['reader'], r['addr'])
        if k not in seen:
            seen.add(k)
            uniq.append(r)
    return uniq, files, prim_funcs, W


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tsv', default=os.path.join(ROOT, 'docs', 'research', 's110', 'feed_fields.tsv'))
    a = ap.parse_args()
    rows, files, prim_funcs, _W = collect()
    os.makedirs(os.path.dirname(a.tsv), exist_ok=True)
    cols = ['file', 'line', 'func', 'reader', 'kind', 'addr', 'base', 'off', 'width', 'family', 'status', 'writers', 'cand']
    with open(a.tsv, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('\t'.join(cols) + '\n')
        for r in rows:
            fh.write('\t'.join(str(r[c]) for c in cols) + '\n')
    report(rows, files, prim_funcs, a.tsv)


def report(rows, files, prim_funcs, tsv):
    print('scope: %d files; rows: %d -> %s' % (len(files), len(rows), os.path.relpath(tsv, ROOT)))
    print('by kind:', dict(Counter(r['kind'] for r in rows)))
    fams = sorted({r['family'] for r in rows})
    print('\nfield reads (kind=field) per family: total / K / image / G / T / named / none  [no constant offset] {distinct (file, base, off)}')
    for f in fams + ['ALL']:
        rs = [r for r in rows if (f == 'ALL' or r['family'] == f) and r['kind'] == 'field']
        c = Counter(r['status'] for r in rs)
        unres = sum(1 for r in rs if r['base'] == '?' or r['off'] == '')
        dist = len({(r['file'], r['base'], r['off']) for r in rs})
        print('  %-10s %5d / %4d / %4d / %4d / %4d / %4d / %5d  [%d] {%d}'
              % (f, len(rs), c['K'], c['image'], c['G'], c['T'], c['named'], c['none'], unres, dist))
    print('\nother kinds per family:')
    for f in fams:
        c = Counter(r['kind'] for r in rows if r['family'] == f and r['kind'] != 'field')
        print('  %-10s %s' % (f, dict(c)))
    fs = [r for r in rows if r['kind'] == 'field']
    print('\ndistinct absolute addresses:', len({r['off'] for r in fs if r['base'] == 'ABS'}),
          '| indexed (+idx):', sum(1 for r in fs if r['addr'].endswith('+idx')))
    print('top readers:', Counter(r['reader'] for r in fs).most_common(30))
    un = sorted((f, sorted(p)) for f, p in prim_funcs.items() if f not in READERS)
    print('\nUNLISTED HELPERS (call a raw guest-read primitive; not a READERS shape): %d' % len(un))
    for f, p in un:
        print('  %s  %s' % (f, ', '.join(p)))


def verb(argv):
    """`q feed`: the table with a struct key and the game's writers per row (qfeedkey.py); `py tools\\q\\qfeed.py` alone rebuilds the plain tsv"""
    import qfeedkey
    return qfeedkey.run(argv)


if __name__ == '__main__':
    sys.exit(main())
