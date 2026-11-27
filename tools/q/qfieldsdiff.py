r"""q fieldsdiff: per pair (OUR function, the GAME function it mirrors), the record fields the game function READS against the
fields ours reads, joined by (record, offset) (S111 FIELDSDIFF, code-diff extractor 5; docs/research/s111/WHOLEDIFF_PLAN.md A.5 row 5).

    py tools/q/q.py fieldsdiff [--fn NAME | --game sub_X] [--flag GAME-ONLY ...] [--ref REF] [--min-overlap N] [--json] [--facts]
    py tools/q/q.py codediff --field reads [the same options]        (the same rows: codediff hands --field reads to this tool)

Why a verb of its own (codediff only forwards `--field reads`): a GAME-ONLY READ is a lead, not a fault (the game reads bookkeeping
fields ours never needs), so these rows must not fail check.py's `codediff` row, whose DIFF / GAME-ONLY rows do.

The game side, per game function: `q decomp`'s own summary of the function (tools/q/qdecomp.py `decompile`: the `fields read` list,
each a base expression, an offset and a width / kind), plus the COPY-THROUGH reads `--fields` does not list: a constant-size memcpy (a
callee whose db name holds `memcpy`) into a stack local, then a load of that local (also through a second local copy, byte by byte, as
the placement loader swaps its count), is a read of the source base at the copied bytes' offset (a byte-reversed pair = a
little-endian read). A memcpy of a whole buffer into another base makes the destination an alias of the source. A read used inside
`extsh` / `extsb` is signed. Each read carries its USE in the body: gate (an if / loop condition), arg (a call argument), copy (stored
on), read (other). The walk is straight-line over the printed body (a local's provenance does not follow branches) [L].
The ours side, per function of ours (qsym's spans over the file at the tree / --ref): every READER CALL with a constant offset
(`Rd16(b, 2)`, `Rd32(base, mgr + 124)`, `Be32(b + 0x18)`, `ReadWord(&f, obj + kCamNear, v)`, `read32(c + kOffCount)`; the width from
the reader's name: 8 / 16 / 32 / 64, F = f32, ReadWord = 32; a leading s or a trailing s = signed), a cast load `*(const uint32_t*)(p + 8)`,
and a constant subscript of a byte buffer (`b[0]`, the name declared uint8_t / char / std::vector<uint8_t>). `int16_t(...)` /
`static_cast<int8_t>(...)` around a read = signed; `std::bit_cast<float>(...)` = f32. A constant offset is a literal, a constexpr /
const name (the file, else any src file: the k-constants), or + - * << of those. A guest address >= 0x80000000 is the global word K.
Not read: struct members by type (a mirror struct's member whose comment cites the game offset): ours bases are plain variables;
reads inside OUR callees (a helper such as crowd_latch_reader.h PropElement that reads [elt+160] for its caller): one function's own
body only, so such a read shows as GAME-ONLY READ (the whole-tree sort counts these as tool hits).
The pairs: `q pairs` precise MIRRORS rows (comment / code / claim / seam-why; tools/q/qpairs_rel.py: a watcher's rows are not pairs)
whose game side is a function, plus the named records of
tools/q/fieldsdiff_records.json (each row binds a game base to an ours variable and is itself a pair).
The join, per pair: a record row binds its bases (bind `record`); the global word K binds when both read one common address (`global`);
any other ours base binds to the game base with the most offsets read at the same width, when that is >= --min-overlap (2) and no
other game base ties (`overlap N` [L]: an inference, ranked after the record / global binds). An unbound base gives no row.
Verdicts per (record, offset): GAME-ONLY READ (the game reads it, ours never does: the missed input; ranked by its use, gate / arg
first), WIDTH (both read it, the bytes differ, or below 4 bytes the sign: same_width), OURS-ONLY READ (low: the game may read it in a callee), OK.
A row is a code-diff fact pair: --facts prints {side, fn, field: `read RECORD+OFF`, value: width, cond, at, kind: read}.
"""
import argparse
import ast
import collections
import fnmatch
import functools
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
RECORDS = os.path.join(HERE, 'fieldsdiff_records.json')
VERDICTS = ('GAME-ONLY READ', 'WIDTH', 'OURS-ONLY READ', 'OK')
USES = ('gate', 'arg', 'copy', 'read', 'ptr', 'local')
PRECISE = ('comment', 'code', 'claim', 'seam-why')
MIN_OVERLAP = 2
# selftest only: no_copy_through (memcpy'd locals are not reads), no_sign (extsh ignored), bind_any (overlap binds on 0 matches)
PLANT = os.environ.get('WWE_QFIELDSDIFF_PLANT', '')


class FieldsdiffError(Exception):
    pass


def kind_width(w):
    """decomp's width word (u8 / s16 / f32 / u32 byteswapped ...) -> (bytes, 'u' | 's' | 'f')"""
    m = re.match(r'([usf])(8|16|32|64)', w or '')
    if not m:
        return None, None
    return int(m.group(2)) // 8, m.group(1)


def wtext(n, k):
    return f'{k}{8 * n}' if n else '?'


# ------------------------------------------------------------------ the game side
CALL_RE = re.compile(r'^\s*\+([0-9A-F]+)\s+call#[0-9A-F]+ sub_([0-9A-F]{8})(?: \([^()]*\))?\((.*)\)\s*$')
LLOAD_RE = re.compile(r'^\s*\+([0-9A-F]+)\s+[rf]\d+ = (\w+)(?: byteswapped)? local\[sp\+(\d+)\]\s*$')
LSTORE_RE = re.compile(r'^\s*\+([0-9A-F]+)\s+local\[sp\+(\d+)\] = (.+?)\s+\((\w+)\)\s*$')
SITE_RE = re.compile(r'^\s*\+([0-9A-F]+)\s')
NAME_RE = re.compile(r'^L\d+@\+[0-9A-F]+$')
STACK_RE = re.compile(r'^sp\+(\d+)$')


def split_addr(a):
    """'in_r3+4' -> ('in_r3', 4); '([in_r3+8] + r30@8258F898)+2' -> ('([in_r3+8] + r30@8258F898)', 2); '0x8376AB34' -> ('K', addr)"""
    a = a.strip()
    if re.fullmatch(r'0x[0-9A-Fa-f]+', a) and int(a, 16) >= 0x80000000:
        return 'K', int(a, 16)
    m = re.fullmatch(r'(.+?)([+-])(0x[0-9A-Fa-f]+|\d+)', a)
    if m and m.group(1)[-1] not in '+-*/ (':
        v = int(m.group(3), 0)
        return m.group(1), v if m.group(2) == '+' else -v
    return a, 0


def _args(s):
    out, d, cur = {}, 0, ''
    for ch in s + ',':
        if ch in '([':
            d += 1
        elif ch in ')]':
            d -= 1
        if ch == ',' and d == 0:
            k, _, v = cur.strip().partition('=')
            if v:
                out[k.strip()] = v.strip()
            cur = ''
            continue
        cur += ch
    return out


def _use_of(lines, tok):
    """the strongest use of a value token in the body: gate > arg > copy > read > ptr"""
    best = None
    pat = re.compile(re.escape(tok) + r'(?![\w+@])')
    for ln in lines:
        if not pat.search(ln):
            continue
        s = ln.strip()
        body = re.sub(r'^\+[0-9A-F]+\s+', '', s)
        if s.startswith(('if (', 'while (', '} while (')) or re.match(r'^\+[0-9A-F]+\s+if \(', s):
            u = 'gate'
        elif 'call#' in s and pat.search(s.split('(', 1)[-1]):
            u = 'arg'
        elif re.match(r'^local\[sp\+\d+\] = ', body) and pat.search(body.split(' = ', 1)[1]):
            u = 'local'
        elif re.match(r'^\[.*\] = ', body) and pat.search(body.split(' = ', 1)[1]):
            u = 'copy'
        elif ('[' + tok) in ln or ('[(' in ln and tok in ln and '[' + tok + ']' not in ln):
            u = 'ptr'
        else:
            u = 'read'
        if best is None or USES.index(u) < USES.index(best):
            best = u
    return best or 'read'


def _signed(lines, tok):
    if PLANT == 'no_sign':
        return False
    pat = re.compile(r'\bexts[bh]\(\(?' + re.escape(tok) + r'(?![\w+@])')
    return any(pat.search(ln) for ln in lines)


def game_reads(lines, summary, memcpy, ro_end=0):
    """-> [{base, off, n, k, use, at, via}] the reads of one decompiled game function (lines = the printed body, summary =
    decompile()'s summary, memcpy = the callee addresses (hex, 8 digits) that are a memcpy, ro_end = the end of the image's read-only part:
    the q index's code_range end; a K read below it is a constant of the image)"""
    out = []
    alias = {}
    stk = {}        # stack byte offset -> (base, off) of the byte it holds, or None
    vals = {}       # a loaded local's name (L87@+EC) -> its bytes' provenance
    loads = []      # (name, site, n, k)

    def src_byte(src, i):
        sm = STACK_RE.match(src)
        if sm:
            return stk.get(int(sm.group(1)) + i)
        b, o = split_addr(src)
        b = alias.get(b, b)
        return (b, o + i)
    for ln in lines:
        m = CALL_RE.match(ln)
        if m and m.group(2) in memcpy:
            a = _args(m.group(3))
            dst, src, n = a.get('r3', ''), a.get('r4', ''), a.get('r5', '')
            dm = STACK_RE.match(dst)
            if dm and re.fullmatch(r'\d+', n) and src and PLANT != 'no_copy_through':
                bytes_ = [src_byte(src, i) for i in range(int(n))]
                for i, p in enumerate(bytes_):
                    stk[int(dm.group(1)) + i] = p
            elif dst and not dm and src and not STACK_RE.match(src):
                b, o = split_addr(src)
                if o == 0:
                    alias[dst] = alias.get(b, b)
            continue
        m = LLOAD_RE.match(ln)
        if m:
            n, k = kind_width(m.group(2))
            off = int(m.group(3))
            name = f'L{off}@+{m.group(1)}'
            vals[name] = [stk.get(off + i) for i in range(n or 0)]
            loads.append((name, m.group(1), n, k))
            continue
        m = LSTORE_RE.match(ln)
        if m:
            off = int(m.group(2))
            n, _k = kind_width(m.group(4))
            v = m.group(3).strip()
            src = vals.get(v) if NAME_RE.match(v) else None
            for i in range(n or 0):
                stk[off + i] = src[i] if src and i < len(src) else None
    for name, site, n, k in loads:
        prov = vals.get(name) or []
        if not prov or any(p is None for p in prov) or len({p[0] for p in prov}) != 1:
            continue
        offs = [p[1] for p in prov]
        lo = min(offs)
        if sorted(offs) != list(range(lo, lo + len(offs))):
            continue
        swapped = len(offs) > 1 and offs[0] > offs[-1]
        sk = 's' if k != 'f' and _signed(lines, name) else k
        use = _use_of(lines, name)
        if use == 'local':
            continue    # only moved into another local (a byte shuffle): its read is the later load of that local
        out.append({'base': prov[0][0], 'off': lo, 'n': n, 'k': sk, 'use': use, 'at': '+' + site,
                    'via': 'copy-through' + (' (byte-reversed: little-endian)' if swapped else '')})
    for r in summary.get('reads', ()):
        n, k = kind_width(r['w'])
        if not n:
            continue
        b, o = split_addr(r['addr'])
        if b == 'K' and o < ro_end:
            continue    # a word of the image's read-only part (a float / table constant): a literal, constdiff's domain, not a field
        b = alias.get(b, b)
        tok = '[' + r['addr'] + ']'
        sk = 's' if k == 'u' and _signed(lines, tok) else k
        site = next((SITE_RE.match(ln).group(1) for ln in lines if tok in ln and SITE_RE.match(ln)), None)
        out.append({'base': b, 'off': o, 'n': n, 'k': sk, 'use': _use_of(lines, tok), 'at': '+' + site if site else '',
                    'via': 'field' + (f' x{r["n"]}' if r.get('n', 1) > 1 else '')})
    return out


class Game:
    """the game side over the q index: decompile (cached) + the memcpy callees by db name"""
    def __init__(self):
        import q as Q
        import qdecomp
        self.D = qdecomp
        self.con = Q.connect(Q.DB)
        dbi = qdecomp.db_index()
        self.memcpy = {f'{a:08X}' for a, e in dbi.items() if 'memcpy' in str(e.get('name') or '').lower()}
        self.ro_end = int(Q.meta(self.con).get('code_range', [0, 0])[1])
        self.cache = {}

    def reads(self, g):
        """g = 'sub_XXXXXXXX' -> [reads] or None (not a function with a body)"""
        if g not in self.cache:
            try:
                r = self.D.decompile(self.con, int(g[4:], 16))
                self.cache[g] = game_reads(r['lines'], r['summary'], self.memcpy, self.ro_end)
            except (self.D.DecompError, ValueError, OSError):
                self.cache[g] = None
        return self.cache[g]


# ------------------------------------------------------------------ the ours side
def mask(text):
    """comments and string / char literals -> spaces (same offsets)"""
    out, i, n = [], 0, len(text)
    while i < n:
        ch = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(c if c == '\n' else ' ' for c in text[i:j]))
            i = j
        elif ch in '"\'':
            j = i + 1
            while j < n and text[j] != ch and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(ch + ' ' * (j - i - 1) + (text[j] if j < n else ''))
            i = j + 1
        else:
            out.append(ch)
            i += 1
    return ''.join(out)


def match_close(s, i):
    """s[i] is '(' / '[' / '{' -> the index just past its partner (len(s) if unclosed)"""
    op = s[i]
    cl = {'(': ')', '[': ']', '{': '}', '<': '>'}[op]
    d = 0
    for j in range(i, len(s)):
        if s[j] == op:
            d += 1
        elif s[j] == cl:
            d -= 1
            if d == 0:
                return j + 1
    return len(s)


def split_top(s, seps=','):
    out, d, cur = [], 0, ''
    for ch in s:
        if ch in '([{':
            d += 1
        elif ch in ')]}':
            d -= 1
        if ch in seps and d == 0:
            out.append(cur)
            cur = ''
            continue
        cur += ch
    out.append(cur)
    return [x.strip() for x in out]


READER_RE = re.compile(r'(?:safe|guest|host|try)?(rd|read|load|be|le|u|s|i|f|rds|rdu|rdf|reads|readu|readf|readbe|readle|loadbe|loadle|readguest)'
                       r'(8|16|32|64|f)(s)?(?:guarded|safe|nonote|be|le)?')
TYPE_W = {'uint8_t': (1, 'u'), 'int8_t': (1, 's'), 'uint16_t': (2, 'u'), 'int16_t': (2, 's'), 'uint32_t': (4, 'u'), 'int32_t': (4, 's'),
          'uint64_t': (8, 'u'), 'int64_t': (8, 's'), 'float': (4, 'f'), 'double': (8, 'f')}


def reader_width(name, tmpl=None):
    """a reader function's name (and its template argument) -> (bytes, kind) or None"""
    n = name.lower().replace('_', '')
    if n in ('loadandswap', 'load') and tmpl:
        return TYPE_W.get(tmpl.strip('<> ').split('::')[-1].replace('const', '').strip())
    if n == 'readword':
        return 4, 'u'
    m = READER_RE.fullmatch(n)
    if not m:
        return None
    pre, w, s = m.groups()
    if w == 'f' or pre in ('f', 'rdf', 'readf'):
        return 4, 'f'
    k = 's' if s or pre in ('s', 'i', 'rds', 'reads') else 'u'
    return int(w) // 8, k


class Consts:
    """constexpr / const integer names: the function's file first, then every src file of the tree (one pass, lazily)"""
    def __init__(self, root):
        self.root, self._all = root, None

    @staticmethod
    def scan(text):
        d = {}
        for m in re.finditer(r'\b(?:constexpr|const)\s+(?:static\s+)?(?:inline\s+)?[\w:<>]+\s+(\w+)\s*(?:=\s*|\{\s*)(0[xX][0-9a-fA-F]+|\d+)[uUlL]*\s*\}?\s*[;,]', text):
            d.setdefault(m.group(1), int(m.group(2), 0))
        for m in re.finditer(r'\benum\b[^{;]*\{([^{}]*)\}', text):
            for e in re.finditer(r'\b(\w+)\s*=\s*(0[xX][0-9a-fA-F]+|\d+)[uU]?\b', m.group(1)):
                d.setdefault(e.group(1), int(e.group(2), 0))
        return d

    def all(self):
        if self._all is None:
            self._all = {}
            for p in glob.glob(os.path.join(self.root, 'src', '**', '*.*'), recursive=True):
                if not p.endswith(('.h', '.cpp', '.inc', '.hpp', '.inl')):
                    continue
                try:
                    with open(p, encoding='utf-8', errors='replace') as fh:
                        t = fh.read()
                except OSError:
                    continue
                if 'const' in t:
                    for k, v in self.scan(t).items():
                        self._all.setdefault(k, v)
        return self._all


CONST_NAME_RE = re.compile(r'k[A-Z0-9]\w*|[A-Z][A-Z0-9_]{2,}')


def const_of(expr, local, consts):
    """a constant expression of ours -> int, else None"""
    e = expr.strip()
    if not e:
        return None
    e = re.sub(r'\b(?:u?int\d+_t|size_t|unsigned|int|long)\s*\(', '(', e)
    e = re.sub(r'static_cast<[^<>]*>\s*', '', e)

    def name(m):
        w = m.group(0)
        if re.fullmatch(r'0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*', w):
            return str(int(re.sub(r'[uUlL]+$', '', w), 0))
        last = w.split('::')[-1]
        v = local.get(last)
        if v is None and CONST_NAME_RE.fullmatch(last):    # the tree-wide index only for constant-shaped names (a loop's `i` is not one)
            v = consts.all().get(last)
        if v is None:
            raise KeyError(w)
        return str(v)
    try:
        e = re.sub(r'(?:\w+::)*\w+', name, e)
    except KeyError:
        return None
    if not re.fullmatch(r'[\d\s+\-*()<>]*', e):
        return None
    try:
        tree = ast.parse(e, mode='eval')
    except SyntaxError:
        return None
    ok = (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant, ast.Add, ast.Sub, ast.Mult, ast.LShift, ast.USub, ast.UAdd)
    if not all(isinstance(nd, ok) for nd in ast.walk(tree)):
        return None
    try:
        v = eval(compile(tree, '<const>', 'eval'), {'__builtins__': {}})   # noqa: S307 - a whitelisted arithmetic tree
    except Exception:   # noqa: BLE001
        return None
    return v if isinstance(v, int) else None


IDENT = r'[A-Za-z_]\w*(?:(?:\.|->)[A-Za-z_]\w*)*'
HOST_BASES = {'base', 'mem', 'host', 'read', 'rd', 'read32', 'g_base', 'guest'}   # a reader's first argument that is the memory, not the record


CAST_HEAD_RE = re.compile(r'(?:static_cast|reinterpret_cast|std::bit_cast)\s*<[^<>]*>\s*(?=\()|(?:std::)?(?:u?int\d+_t|size_t|uintptr_t)\s*(?=\()')
CCAST_RE = re.compile(r'\(\s*(?:const\s+)?[\w:]+\s*(?:const\s*)?\**\s*\)\s*(?=\S)')


def strip_casts(s):
    """`uint32_t(X)`, `static_cast<T>(X)`, `(T*)X`, `(X)` -> X (repeatedly)"""
    s = s.strip()
    while s:
        m = CAST_HEAD_RE.match(s)
        if m and match_close(s, m.end()) == len(s):
            s = s[m.end() + 1:-1].strip()
            continue
        m = CCAST_RE.match(s)
        if m and not re.fullmatch(r'\(\s*\d+\s*\)', m.group(0).strip()):
            s = s[m.end():].strip()
            continue
        if s.startswith('(') and match_close(s, 0) == len(s):
            s = s[1:-1].strip()
            continue
        break
    return s


def nested_base(t, local, consts):
    """a term that is itself a reader call of a global word (`Rd32(base, 0x83930050)`) -> '[0x83930050]' (decomp's name for the
    pointer loaded from that word), else None"""
    m = CALLSITE_RE.match(t)
    if not m or not reader_width(m.group(2), m.group(3)) or match_close(t, m.end() - 1) != len(t):
        return None
    args = [a for a in split_top(t[m.end():-1]) if a]
    v = const_of(args[-1], local, consts) if args else None
    return f'[0x{v:08X}]' if v is not None and v >= 0x80000000 else None


def parse_addr(arg, local, consts):
    """an address / pointer expression of ours -> (base, off) with a constant off, else None. `&X[E]`, `X.data() + E`, `X + E - F`"""
    s = strip_casts(arg)
    m = re.fullmatch(r'&\s*(' + IDENT + r')\s*\[(.+)\]', s, re.S)
    if m:
        v = const_of(m.group(2), local, consts)
        return (m.group(1), v) if v is not None else None
    terms, cur, d, sign, i = [], '', 0, '+', 0
    while i < len(s):
        ch = s[i]
        if ch in '([{':
            d += 1
        elif ch in ')]}':
            d -= 1
        if d == 0 and ch in '+-' and not s.startswith('->', i) and cur.strip():
            terms.append((sign, cur.strip()))
            sign, cur = ch, ''
            i += 1
            continue
        cur += ch
        i += 1
    terms.append((sign, cur.strip()))
    off, base = 0, None
    for sg, t in terms:
        v = const_of(t, local, consts)
        if v is not None:
            off += v if sg == '+' else -v
            continue
        t2 = re.sub(r'\.data\(\)$', '', strip_casts(t))
        nb = None if re.fullmatch(IDENT, t2) else nested_base(t2, local, consts)
        if sg != '+' or base is not None or not (nb or re.fullmatch(IDENT, t2)):
            return None
        base = nb or t2
    if base is None:
        return ('K', off) if off >= 0x80000000 else None
    return base, off


BYTEBUF_RE = re.compile(r'(?:\bu?int8_t|\bchar|std::vector<\s*u?int8_t\s*>|std::byte|std::span<\s*(?:const\s+)?u?int8_t\s*>)\s*(?:const\s*)?[*&]*\s*(?:const\s+)?(\w+)\s*[,)=;\[{]')
CALLSITE_RE = re.compile(r'\b((?:\w+::)*)(\w+)\s*(<[^<>()]*>)?\s*\(')
CASTLOAD_RE = re.compile(r'\*\s*\(\s*(?:const\s+)?(?:volatile\s+)?(u?int(?:8|16|32|64)_t|float|double)\s*(?:const\s*)?\*\s*\)\s*\(')
WRAP_RE = re.compile(r'(?:static_cast<\s*(u?int(?:8|16|32)_t)\s*>|\b(int(?:8|16|32)_t)|std::bit_cast<\s*(float)\s*>)\s*\(\s*$')


def _wrapper(body, start, n, k):
    """(n, k) of a read at body[start:] adjusted by the cast it sits in: int16_t( .. ) = signed, std::bit_cast<float>( .. ) = f32"""
    m = WRAP_RE.search(body[max(0, start - 40):start])
    if not m:
        return n, k
    t = m.group(1) or m.group(2)
    if m.group(3):
        return n, 'f'
    tw = TYPE_W.get(t)
    if tw and tw[0] == n:
        return n, tw[1]
    return n, k


def ours_reads(body, first_line, local_consts, consts):
    """-> [{base, off, n, k, at (line), expr}] the constant-offset reads of one function body of ours (text masked)"""
    out = []
    bufs = set(BYTEBUF_RE.findall(body))

    def line(pos):
        return first_line + body.count('\n', 0, pos)
    for m in CALLSITE_RE.finditer(body):
        name, tmpl = m.group(2), m.group(3)
        rw = reader_width(name, tmpl)
        if not rw:
            continue
        before = body[max(0, m.start() - 30):m.start()]
        if re.search(r'(?:inline|auto|uint\d+_t|int\d*_t|bool|float|void|static)\s+$', before):
            continue    # a definition, not a call
        op = m.end() - 1
        end = match_close(body, op)
        args = [a for a in split_top(body[op + 1:end - 1]) if a]
        hit = None
        for a in args:
            if re.search(r'[+\-&]|\.data\(\)', a):
                p = parse_addr(a, local_consts, consts)
                if p:
                    hit = p
                    break
        if hit is None and len(args) >= 2 and re.fullmatch(IDENT, strip_casts(args[1])) and (
                args[0].startswith('&') or strip_casts(args[0]) in HOST_BASES):
            hit = (strip_casts(args[1]), 0)     # ReadWord(read, a, out) / Rd32(base, a): the guest address itself, +0
        if hit is None and len(args) >= 2 and re.fullmatch(IDENT, strip_casts(args[0])):
            v = const_of(args[1], local_consts, consts)
            if v is not None:
                hit = ('K', v) if v >= 0x80000000 else (strip_casts(args[0]), v)
        if hit is None and len(args) == 1:
            a = strip_casts(args[0])
            if re.fullmatch(IDENT, a):
                if a in bufs:       # a byte pointer read at +0 (`Be32(h)`); a plain name may be an offset (`u32(off)`): not a read
                    hit = (a, 0)
            else:
                v = const_of(a, local_consts, consts)
                if v is not None:
                    hit = ('K', v) if v >= 0x80000000 else (f'{name}()', v)
        if hit is None:
            continue
        n, k = _wrapper(body, m.start(), *rw)
        out.append({'base': hit[0], 'off': hit[1], 'n': n, 'k': k, 'at': line(m.start()), 'expr': ' '.join(body[m.start():end].split())[:90]})
    for m in CASTLOAD_RE.finditer(body):
        op = m.end() - 1
        end = match_close(body, op)
        p = parse_addr(body[op:end], local_consts, consts)
        if p:
            n, k = TYPE_W[m.group(1)]
            out.append({'base': p[0], 'off': p[1], 'n': n, 'k': k, 'at': line(m.start()), 'expr': ' '.join(body[m.start():end].split())[:90]})
    for m in re.finditer(r'(?<![\w&.>])(' + IDENT + r')\s*\[', body):
        base = m.group(1)
        if base.split('.')[-1].split('>')[-1] not in bufs and base not in bufs:
            continue
        op = m.end() - 1
        end = match_close(body, op)
        if re.match(r'\s*=[^=]', body[end:]) or re.match(r'\s*(?:\+\+|--|[+\-*/|&^]=)', body[end:]):
            continue    # a store
        v = const_of(body[op + 1:end - 1], local_consts, consts)
        if v is None:
            continue
        n, k = _wrapper(body, m.start(), 1, 'u')
        out.append({'base': base, 'off': v, 'n': n, 'k': k, 'at': line(m.start()), 'expr': ' '.join(body[m.start():end].split())[:90]})
    return out


class Tree:
    """our code at the working tree (root = REPO) or a git ref's archive (qcodediff.ref_root)"""
    def __init__(self, root):
        self.root = root
        self.consts = Consts(root)
        self._files = {}

    def text(self, rel):
        if rel not in self._files:
            try:
                with open(os.path.join(self.root, rel), encoding='utf-8', errors='replace') as fh:
                    self._files[rel] = fh.read().replace('\r\n', '\n')
            except OSError:
                self._files[rel] = None
        return self._files[rel]

    @functools.lru_cache(maxsize=None)
    def funcs(self, rel):
        """[(qual, start line, end line)] of the file's function spans"""
        import qsym
        t = self.text(rel)
        if t is None:
            return []
        s = qsym.Src(self.root, rel, t)
        return [(sp['qual'] or sp['name'], sp['start'], sp['end']) for sp in s.spans if sp['kind'] == 'func' and (sp['qual'] or sp['name'])]

    @functools.lru_cache(maxsize=None)
    def file_consts(self, rel):
        t = self.text(rel)
        return Consts.scan(mask(t)) if t else {}

    def reads(self, rel, fn):
        """the reads of every span of `fn` in `rel` (an overload / a split body: all of them)"""
        t = self.text(rel)
        if t is None:
            return None
        lines = t.split('\n')
        got, found = [], False
        for q, a, b in self.funcs(rel):
            if not name_matches(q, fn):
                continue
            found = True
            body = mask('\n'.join(lines[a - 1:b]))
            loc = dict(self.file_consts(rel))
            loc.update(Consts.scan(body))
            for r in ours_reads(body, a, loc, self.consts):
                r['at'] = f'{rel}:{r["at"]}'
                got.append(r)
        return got if found else None


def name_matches(qual, want):
    qs = [x for x in re.sub(r'\(.*$', '', qual).split('::') if x]
    ws = [x for x in want.split('::') if x]
    return bool(ws) and len(qs) >= len(ws) and qs[-len(ws):] == ws


# ------------------------------------------------------------------ the join
def load_records(path=RECORDS):
    with open(path, encoding='utf-8') as fh:
        return json.load(fh).get('records', [])


def bind(ours, game, fixed, min_overlap=MIN_OVERLAP):
    """-> [(ours base, game base, how)] for one pair. ours / game: [reads]; fixed: [(ours base, game base, record name)]"""
    ob = collections.defaultdict(dict)
    gb = collections.defaultdict(dict)
    for r in ours:
        ob[r['base']].setdefault(r['off'], r)
    for r in game:
        gb[r['base']].setdefault(r['off'], r)
    out, used_o, used_g = [], set(), set()
    for o, g, name in fixed:
        out.append((o, g, f'record {name}'))
        used_o.add(o)
        used_g.add(g)
    if 'K' in ob and 'K' in gb and 'K' not in used_o and set(ob['K']) & set(gb['K']):
        out.append(('K', 'K', 'global'))
        used_o.add('K')
        used_g.add('K')
    for o in sorted(ob):
        if o in used_o or o == 'K':
            continue
        score = []
        for g in gb:
            if g in used_g or g == 'K':
                continue
            s = sum(1 for off, r in ob[o].items() if off in gb[g] and gb[g][off]['n'] == r['n'])
            score.append((s, g))
        if not score:
            continue
        score.sort(key=lambda x: -x[0])
        need = 0 if PLANT == 'bind_any' else min_overlap
        if score[0][0] < max(need, 1 if PLANT != 'bind_any' else 0):
            continue
        if len(score) > 1 and score[1][0] == score[0][0] and PLANT != 'bind_any':
            continue    # two game bases tie: no inference
        out.append((o, score[0][1], f'overlap {score[0][0]}'))
        used_o.add(o)
        used_g.add(score[0][1])
    return out


def same_width(a, b):
    """(bytes, kind) pairs that read the same field: the same bytes, and below 4 bytes the same sign (a byte / halfword extends by
    it); a whole word read as u32 / s32 / f32 is one value seen two ways (ours reads a word, then bit_casts it)"""
    return a[0] == b[0] and (a[0] >= 4 or a[1] == b[1])


def judge(fn, game_fn, ours, game, binds, fields=None, sources=()):
    rows = []
    for o, g, how in binds:
        orr = [r for r in ours if r['base'] == o]
        grr = [r for r in game if r['base'] == g]
        rec = how.split(' ', 1)[1] if how.startswith('record ') else ('K' if how == 'global' else f'{g} ~ {o}')
        offs = sorted({r['off'] for r in orr} | {r['off'] for r in grr})
        for off in offs:
            go = [r for r in grr if r['off'] == off]
            oo = [r for r in orr if r['off'] == off]
            if go and not oo:
                v = 'GAME-ONLY READ'
                use = min((r['use'] for r in go), key=USES.index)
                why = f'the game reads {g}+{off} ({wtext(go[0]["n"], go[0]["k"])}, {use}); ours never reads {o}+{off}'
            elif oo and not go:
                v, use = 'OURS-ONLY READ', None
                why = f'ours reads {o}+{off} ({wtext(oo[0]["n"], oo[0]["k"])}); the game function never does (it may in a callee)'
            else:
                gw = {(r['n'], r['k']) for r in go}
                ow = {(r['n'], r['k']) for r in oo}
                use = min((r['use'] for r in go), key=USES.index)
                if any(same_width(a, b) for a in gw for b in ow):
                    v, why = 'OK', ''
                else:
                    v = 'WIDTH'
                    why = (f"the game reads {g}+{off} as {'/'.join(sorted(wtext(*x) for x in gw))}, ours {o}+{off} as "
                           f"{'/'.join(sorted(wtext(*x) for x in ow))}")
            fname = (fields or {}).get(str(off)) if how.startswith('record ') else None
            rows.append({'fn': fn, 'game': game_fn, 'record': rec, 'bind': how, 'off': off, 'verdict': v, 'use': use, 'why': why,
                         'name': fname, 'game_reads': go, 'ours_reads': oo, 'pair': list(sources)})
    return rows


def rank_key(r):
    bk = 0 if r['bind'].startswith('record') else 1 if r['bind'] == 'global' else 2
    ov = -int(r['bind'].split()[1]) if r['bind'].startswith('overlap') else 0
    return (VERDICTS.index(r['verdict']), USES.index(r['use']) if r['use'] in USES else len(USES), bk, ov, r['fn'], r['game'], r['off'])


def pair_rows(root):
    """{(file, our fn): {sub_X: [source]}} of q pairs' precise rows whose game side is a function-shaped address"""
    import qpairs
    con, _info = qpairs.update(root)
    try:
        out = collections.defaultdict(lambda: collections.defaultdict(list))
        for r in con.execute("SELECT src, ours, kind, file, line, game FROM pairs WHERE kind = 'func' AND rel = 'MIRRORS'"):   # S111 PAIRMEAN: ports only
            if r['src'] not in PRECISE or not r['game'].startswith('82'):
                continue
            s = f"{r['src']} {r['file']}:{r['line']}"
            lst = out[(r['file'], r['ours'])]['sub_' + r['game']]
            if s not in lst:
                lst.append(s)
        return out
    finally:
        con.close()


def ref_root(ref):
    if not ref:
        return REPO
    import qcodediff
    try:
        return qcodediff.ref_root(ref)
    except qcodediff.CodediffError as e:
        raise FieldsdiffError(str(e)) from e


def find_fn_files(tree, fn):
    """the src files where our function `fn` has a body (a record row names a function, not a file)"""
    tail = fn.split('::')[-1]
    hits = []
    for p in glob.glob(os.path.join(tree.root, 'src', '**', '*.*'), recursive=True):
        if not p.endswith(('.h', '.cpp', '.inc', '.hpp', '.inl')):
            continue
        rel = os.path.relpath(p, tree.root).replace(os.sep, '/')
        t = tree.text(rel)
        if t and re.search(r'\b' + re.escape(tail) + r'\s*\(', t) and any(name_matches(q, fn) for q, _a, _b in tree.funcs(rel)):
            hits.append(rel)
    return hits


def collect(ref=None, fn=None, game_fn=None, min_overlap=MIN_OVERLAP, game=None, records=None, pairs=None, tree=None):
    """-> (rows, problems, info) for the tree at ref (None = the working tree)"""
    tree = tree or Tree(ref_root(ref))
    game = game or Game()
    records = load_records() if records is None else records
    problems = []
    pairs = pair_rows(tree.root) if pairs is None else pairs
    fixed = collections.defaultdict(list)          # (file, fn, sub_X) -> [(ours base, game base, record)]
    fields = {}
    for rec in records:
        for o in rec.get('ours', ()):
            files = find_fn_files(tree, o['fn'])
            if not files:
                problems.append(f"record {rec['name']}: {o['fn']} has no body in this tree")
            for rel in files:
                for g in rec.get('game', ()):
                    fixed[(rel, o['fn'], g['fn'])].append((o['base'], g['base'], rec['name']))
                    pairs.setdefault((rel, o['fn']), collections.defaultdict(list))
                    srcs = pairs[(rel, o['fn'])][g['fn']]
                    if f"record {rec['name']}" not in srcs:
                        srcs.append(f"record {rec['name']}")
                    fields[(o['fn'], g['fn'])] = rec.get('fields') or {}
    rows, n_ours, n_pairs, n_game = [], 0, 0, set()
    for (rel, ofn), gs in sorted(pairs.items()):
        if fn and not (fn == ofn or fnmatch.fnmatchcase(ofn, fn) or name_matches(ofn, fn)):
            continue
        ours = tree.reads(rel, ofn)
        if ours is None or not (ours or any(r2 == rel and f2 == ofn for r2, f2, _g in fixed)):
            continue    # no body here, or no read and no record binds it (a record row's function is judged with no read at all)
        n_ours += 1 if ours else 0
        for g, srcs in sorted(gs.items()):
            if game_fn and g.lower() != 'sub_' + game_fn.lower().replace('sub_', '').replace('0x', ''):
                continue
            greads = game.reads(g)
            if greads is None:
                continue
            n_pairs += 1
            n_game.add(g)
            fx = [x for (r2, f2, g2), lst in fixed.items() if r2 == rel and f2 == ofn and g2 == g for x in lst]
            rows += judge(ofn, g, ours, greads, bind(ours, greads, fx, min_overlap), fields.get((ofn, g)), srcs)
    rows.sort(key=rank_key)
    return rows, problems, {'ours_functions_with_reads': n_ours, 'pairs_judged': n_pairs, 'game_functions': len(n_game)}


def facts(rows):
    """the rows as code-diff facts (the q gstate --facts form)"""
    out = []
    for r in rows:
        for side, lst in (('game', r['game_reads']), ('ours', r['ours_reads'])):
            for x in lst:
                out.append({'side': side, 'fn': r['game'] if side == 'game' else r['fn'], 'field': f"read {r['record']}+{r['off']}",
                            'value': wtext(x['n'], x['k']), 'cond': None, 'at': x['at'], 'kind': 'read',
                            'use': x.get('use'), 'verdict': r['verdict']})
    return out


def row_text(r):
    nm = f" ({r['name']})" if r.get('name') else ''
    lines = [f"{r['verdict']:14s} {r['fn']} vs {r['game']}  {r['record']}+{r['off']}{nm}  [{r['bind']}" + (f", {r['use']}" if r['use'] else '') + ']']
    if r['why']:
        lines.append(f"               {r['why']}")
    lines.append('               game: ' + ('; '.join(f"{wtext(x['n'], x['k'])} {x['use']} @{x['at']} ({x['via']})" for x in r['game_reads']) or 'never reads it'))
    lines.append('               ours: ' + ('; '.join(f"{wtext(x['n'], x['k'])} @{x['at']} `{x['expr']}`" for x in r['ours_reads']) or 'never reads it'))
    lines.append('               pair: ' + ', '.join(r['pair'][:3]) + (f" (+{len(r['pair']) - 3})" if len(r['pair']) > 3 else ''))
    return '\n'.join(lines)


def main(argv):
    ap = argparse.ArgumentParser(prog='q fieldsdiff', description=__doc__.split('\n')[0])
    ap.add_argument('--fn', help='one of our functions (qualified name, a glob, the last name part)')
    ap.add_argument('--game', help='one game function sub_X')
    ap.add_argument('--flag', nargs='+', choices=VERDICTS + ('GAME-ONLY', 'OURS-ONLY'), help='only these verdicts')
    ap.add_argument('--ref', help='our code (and its pairs) at this git ref')
    ap.add_argument('--min-overlap', type=int, default=MIN_OVERLAP, help='an inferred bind needs this many offsets read at the same width (2)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--facts', action='store_true', help='the rows as code-diff facts (JSON)')
    a = ap.parse_args(argv)
    flags = {f + ' READ' if f in ('GAME-ONLY', 'OURS-ONLY') else f for f in (a.flag or ())}
    rows, problems, info = collect(a.ref, a.fn, a.game, a.min_overlap)
    sel = [r for r in rows if not flags or r['verdict'] in flags]
    tot = collections.Counter(r['verdict'] for r in rows)
    if a.facts:
        print(json.dumps(facts(sel), indent=1))
        return 0
    if a.json:
        print(json.dumps({'ref': a.ref or 'the working tree', 'rows': sel, 'totals': dict(tot), 'problems': problems, 'info': info}, indent=1, default=str))
        return 0
    for r in sel:
        print(row_text(r))
    print(f"totals ({a.ref or 'the working tree'}): {len(rows)} rows over {len({(r['fn'], r['game']) for r in rows})} bound pairs "
          f"({info['pairs_judged']} pairs judged, {info['ours_functions_with_reads']} of our functions with a constant-offset read, "
          f"{info['game_functions']} game functions); " + ', '.join(f'{v} {tot.get(v, 0)}' for v in VERDICTS)
          + (f'; shown {len(sel)}' if len(sel) != len(rows) else ''))
    for p in problems:
        print('PROBLEM: ' + p)
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except FieldsdiffError as e:
        print(f'q fieldsdiff: {e}', file=sys.stderr)
        sys.exit(2)
