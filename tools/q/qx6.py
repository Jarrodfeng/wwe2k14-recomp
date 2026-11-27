"""qx6: four more answers (S103 QX3 lane): the q misses of 2026-11-18/19 turned into verbs. All read files directly: always current, no index to refresh.

    py tools/q/q.py asm FUNC|ADDR [FROM TO] [--into FROM TO]
        a per-address window of the generated listing of one game function: `ADDR  mnemonic operands`, branches marked `=> TARGET` (and `(out)` when the target
        is outside the function), every address that is a branch target marked `<= FROM,FROM...` (the source addresses). FUNC alone = the whole function (--limit
        caps it); an ADDR inside a function without FROM TO = the 12 instructions around it; FROM TO = that window.
        --into FROM TO: which branches enter [FROM,TO) from OUTSIDE it (conditional / unconditional / switch cases), with their source addresses, and whether
        the fall-through from the instruction before FROM enters it. Branches from OTHER functions are not listed (they are rare; `q callers` shows them).
    py tools/q/q.py ours hooks [ADDR|sub_X|NAME]
        every hook WE put on game code: REX_HOOK_RAW / REX_HOOK (src\\, incl. a #define macro that expands to REX_HOOK_RAW, invoked with the function) and the
        [[midasm_hook]] rows of config\\*.toml (address, name, registers, toml file:line, the src file:line that DEFINES the callback, and whether the generated
        code declares / calls it: `extern void NAME(`). ARG = a function (also lists mid-asm hooks INSIDE it), an address, or a hook / callback name; none = all.
    py tools/q/q.py ours text WORDS...
        lines of src / tools / config / probes / android (code, comments, strings, shaders) holding ANY of the words (S111 QGAPS; --and = ALL of them on one
        line; this module's cmd_text keeps any_=False = ALL for python callers), case-insensitive; an address word (82C1E840,
        0x82C1E840, sub_82C1E840) matches every spelling. C++ / py / ps1 hits are marked comment / string / code.
    q ours def|refs NAME on a name that is no C++ symbol falls back to the shader text (src\\**\\*.glsl / .frag / .vert / .hlsl / .fx.json): where a block field
        (Param1) is declared and used.
"""
import glob
import json
import os
import re
import sqlite3
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

SRC_EXT = ('.h', '.hpp', '.cpp', '.inl', '.cc', '.inc')
SHADER_EXT = ('.glsl', '.frag', '.vert', '.hlsl', '.comp', '.fx.json')
TEXT_EXT = SRC_EXT + ('.py', '.ps1', '.json', '.toml', '.txt', '.cmake', '.cfg', '.bat', '.md', '.java', '.kt', '.tsv') + SHADER_EXT
SKIP = ('__pycache__', 'node_modules', '.git', 'spv', 'spirv')
MAX_BYTES = 1_500_000


class XError(Exception):
    pass


def main_root(root):
    """the MAIN checkout (generated\\ and out\\ live there) from any worktree."""
    try:
        out = subprocess.run(['git', '-C', root, 'rev-parse', '--git-common-dir'], capture_output=True, text=True, timeout=20).stdout.strip()
        if out:
            cd = os.path.abspath(os.path.join(root, out))
            if os.path.basename(cd) == '.git':
                return os.path.dirname(cd)
    except (OSError, subprocess.SubprocessError):
        pass
    return os.path.abspath(root)


def walk(root, tops, exts):
    for top in tops:
        base = os.path.join(root, top)
        for dp, dn, fn in os.walk(base):
            dn[:] = [d for d in dn if d not in SKIP and not d.startswith('.')]
            for f in fn:
                if f.endswith(exts):
                    fp = os.path.join(dp, f)
                    try:
                        if os.path.getsize(fp) > MAX_BYTES:
                            continue
                    except OSError:
                        continue
                    yield fp


def rel_of(root, p):
    return os.path.relpath(p, root).replace('\\', '/')


def read_text(p):
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            return f.read()
    except OSError:
        return ''


# ================================================================= QX4: an address held in a named constant

ADDR_ARG_RE = re.compile(r'^(?:0x[0-9A-Fa-f]{5,8}|[0-9A-Fa-f]{8})$')


def is_addr_arg(arg):
    """a literal guest / hex address given where a symbol name is expected (0x836F38D0)"""
    a = arg.strip()
    return bool(ADDR_ARG_RE.match(a)) and a.lower().startswith(('0x8', '8'))


def _blank(text):
    """comments and string / char literals blanked (same length, newlines kept): an address inside a string or a comment is never a declaration"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        elif text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(c + ' ' * max(j - i - 2, 0) + (c if j - i >= 2 else ''))
            i = j
        else:
            out.append(c)
            i += 1
    return ''.join(out)


_INT_TYPES = r'(?:u?int(?:8|16|32|64)?_t|unsigned(?: int| long)?|int|long|size_t|uintptr_t|DWORD|uint32|u32)'
_CAST_PAREN = re.compile(r'^\(\s*' + _INT_TYPES + r'\s*\)\s*')
_CAST_CALL = re.compile(r'^(?:' + _INT_TYPES + r'|static_cast\s*<\s*' + _INT_TYPES + r'\s*>|reinterpret_cast\s*<\s*' + _INT_TYPES + r'\s*>)\s*\(\s*(.*)\)$', re.S)


def _is_value(expr, v):
    """the WHOLE expression is the value (casts and parentheses allowed): `0x836F38D0`, `(uint32_t)0x836f38d0u`, `uint32_t(0x836F38D0)`; not `0x836F38D0 + 4`, `Foo(0x836F38D0)`"""
    e = expr.strip()
    for _ in range(6):
        e = e.strip()
        if e.startswith('(') and e.endswith(')') and _balanced(e[1:-1]):
            e = e[1:-1]
            continue
        m = _CAST_PAREN.match(e)
        if m:
            e = e[m.end():]
            continue
        m = _CAST_CALL.match(e)
        if m:
            e = m.group(1)
            continue
        break
    m = re.fullmatch(r'0[xX]([0-9a-fA-F]+)[uUlL]*', e.strip())
    return bool(m) and int(m.group(1), 16) == v


def _balanced(s):
    d = 0
    for ch in s:
        d += ch in '({['
        d -= ch in ')}]'
        if d < 0:
            return False
    return d == 0


def _split_top(s):
    """split on commas at depth 0 -> [(offset, text)]"""
    parts, d, st = [], 0, 0
    for i, ch in enumerate(s):
        if ch in '({[':
            d += 1
        elif ch in ')}]':
            d -= 1
        elif ch == ',' and d == 0:
            parts.append((st, s[st:i]))
            st = i + 1
    parts.append((st, s[st:]))
    return parts


def _declarator(part, off, v):
    """`type NAME[N] = <init>` -> (name, offset of NAME) when the initialiser IS the value, or is a { list } with the value as an element"""
    k = -1
    for m in re.finditer(r'=', part):
        pre = part[m.start() - 1] if m.start() else ''
        nxt = part[m.start() + 1] if m.start() + 1 < len(part) else ''
        if pre in '=!<>' or nxt == '=':
            continue
        k = m.start()
        break
    if k < 0:
        return None
    left = part[:k]
    nm = re.search(r'([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*$', left)
    if not nm:
        return None
    init = part[k + 1:].strip()
    if init.startswith('{'):
        end = init.rfind('}')
        if end < 0 or not any(_is_value(el, v) for _, el in _split_top(init[1:end])):
            return None
    elif not _is_value(init, v):
        return None
    return nm.group(1), off + nm.start(1)


def addr_holders(root, arg):
    """-> [(name, file, line, text)]: every named constant whose WHOLE initialiser is the address (or one element of its { } list): a `constexpr / const / static`
    declaration (one of several declarators on a line, an array, any hex case, u / ul suffix, a (uint32_t) cast), an `enum` enumerator, a `#define NAME value`.
    Comments and strings are blanked first. NOT `x = Foo(0x836F38D0)`, `kN = 0x836F38D0 + 4`, `a = b + 0x836F38D0`."""
    a = arg.strip()
    v = int(a[2:] if a.lower().startswith('0x') else a, 16)
    low = '%x' % v
    out, seen = [], set()
    kw = re.compile(r'\b(constexpr|const|static|enum)\b')
    for fp in walk(root, ('src', 'probes', 'tools', 'android'), ('.h', '.hpp', '.cpp', '.inl', '.cc', '.inc')):
        raw = read_text(fp)
        if low not in raw.lower():
            continue
        t = _blank(raw)
        lines = raw.split('\n')
        rel = rel_of(root, fp)

        def add(name, pos):
            ln = t.count('\n', 0, pos) + 1
            k = (name, rel, ln)
            if k not in seen:
                seen.add(k)
                out.append((name, rel, ln, lines[ln - 1].strip()[:140]))
        for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]+([^\n]*)$', t, re.M):
            if _is_value(m.group(2), v):
                add(m.group(1), m.start(1))
        pos = 0
        while True:
            m = kw.search(t, pos)
            if not m:
                break
            i, d, is_enum = m.start(), 0, m.group(1) == 'enum'
            j = i
            ok = False
            while j < len(t):
                ch = t[j]
                if ch in '([':
                    d += 1
                elif ch in ')]':
                    d -= 1
                elif ch == '{':
                    before = t[i:j].rstrip()
                    if d == 0 and not (before.endswith('=') or before.endswith(',') or 'enum' in before or before.endswith(']')):
                        break              # a function / class body: not a declaration
                    d += 1
                elif ch == '}':
                    d -= 1
                elif ch == ';' and d <= 0:
                    ok = True
                    break
                if d < 0:
                    break
                j += 1
            stmt = t[i:j] if ok else ''
            if stmt and low in stmt.lower():
                if is_enum or re.match(r'enum\b', stmt):
                    b0, b1 = stmt.find('{'), stmt.rfind('}')
                    if b0 >= 0 and b1 > b0:
                        for off, part in _split_top(stmt[b0 + 1:b1]):
                            r = _declarator(part, 0, v)
                            if r:
                                add(r[0], i + b0 + 1 + off + r[1])
                else:
                    for off, part in _split_top(stmt):
                        r = _declarator(part, off, v)
                        if r:
                            add(r[0], i + r[1])
            pos = (j + 1) if ok else (i + len(m.group(0)))
    return out


# ================================================================= q asm

def parse_addr(s):
    m = re.fullmatch(r'(?:0x|sub_|loc_)?([0-9A-Fa-f]{1,8})', s.strip())
    if not m:
        raise XError(f'{s}: not an address (0x82..., sub_82..., 82...)')
    return int(m.group(1), 16)


def at_window(spec, start, size, ctx):
    """S111 QGAPS: `--at OFF` / `OFF..OFF2` (hex byte offsets from the function start, a leading + and 0x allowed) -> the [FROM, TO] address strings asm_answer takes.
    OFF alone = the instruction there +-ctx instructions, kept inside the function."""
    def off(s):
        m = re.fullmatch(r'\+?(?:0[xX])?([0-9A-Fa-f]+)', s.strip())
        if not m:
            raise XError(f'--at {spec}: not a hex offset (e.g. E00, +0xE00 or E00..E40)')
        v = int(m.group(1), 16) & ~3      # review S111: an unaligned offset = the instruction holding that byte
        if size and v >= size:
            raise XError(f'--at {spec}: +0x{v:X} is past the end of the function (size 0x{size:X}; its last instruction is +0x{max(size - 4, 0):X})')
        return v
    if '..' in spec:
        lo, hi = (off(x) for x in spec.split('..', 1))
        if hi < lo:
            raise XError('--at: the second offset must not be below the first')
    else:
        c = off(spec)
        lo, hi = max(0, c - 4 * ctx), min(max(size - 4, 0), c + 4 * ctx)
    return [f'{start + lo:X}', f'{start + hi:X}']


def asm_main(argv, con, resolve, root=None):
    import argparse
    import qdecomp
    ap = argparse.ArgumentParser(prog='q asm', description=__doc__.split('\n\n')[0])
    ap.add_argument('func')
    ap.add_argument('range', nargs='*', help='FROM TO (hex addresses)')
    ap.add_argument('--into', nargs=2, metavar=('FROM', 'TO'), help='branches that enter [FROM,TO) from outside it')
    ap.add_argument('--limit', type=int, default=None, help='rows shown (default 400; no cut when --range or --where is given)')
    ap.add_argument('--range', dest='range_spec', default=None, metavar='A..B', help='S111 QWHERE: the instructions from function offset A to B (+0x45C..+0x4C0, or game addresses), whole, no row cut (qdecomp_range.py)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--at', default=None, metavar='OFF[..OFF2]', help='S111 QGAPS: a window by hex byte offset from the function start: OFF = that instruction +-CTX, OFF..OFF2 = that range (`FUNC+OFF` = --at OFF)')
    ap.add_argument('--ctx', type=int, default=12, help='--at OFF: instructions either side (default 12)')
    import qfactcard
    qfactcard.add_arg(ap)
    ap.add_argument('--writes', default=None, metavar='rN|sp+D', help='S111 QARGS: only the instructions that write that register / store to that stack slot (qregs.py)')
    ap.add_argument('--reads', default=None, metavar='rN|sp+D', help='S111 QARGS: only the instructions that read that register / load that stack slot')
    a = ap.parse_args(argv)
    mo = re.fullmatch(r'(.+?)\+((?:0[xX])?[0-9A-Fa-f]+(?:\.\.\+?(?:0[xX])?[0-9A-Fa-f]+)?)', a.func.strip())
    if mo and a.at is None:      # FUNC+OFF
        a.func, a.at = mo.group(1), mo.group(2)
    if a.at is None and len(a.range) == 1 and a.range[0].startswith('+'):      # FUNC +OFF  /  FUNC +OFF..+OFF2 as separate words
        a.at, a.range = a.range[0], []
    fl = re.fullmatch(r'(wwe2k14_recomp\.\d+\.cpp):(\d+)', a.func.strip())
    if fl:       # a listing FILE:LINE -> the function whose body holds that line (the last DEFINE at or above it)
        hit = con.execute('SELECT name FROM funcs WHERE file = ? AND line <= ? ORDER BY line DESC LIMIT 1',
                          (fl.group(1), int(fl.group(2)))).fetchone()
        if hit is None:
            raise XError(f'{a.func}: no function of that listing file starts at or above that line')
        print(f'({a.func} is in {hit["name"]})')
        a.func = hit['name']
    addr, note = resolve(con, a.func)
    if note:
        print(f'({note})')
    f = con.execute('SELECT * FROM funcs WHERE addr = ?', (addr,)).fetchone()
    if f is None or not f['file']:
        raise XError(f'0x{addr:08X} has no generated body (import / helper)')
    gen = os.path.join(main_root(root or os.path.dirname(os.path.dirname(HERE))), 'generated', 'default')
    path = os.path.join(gen, f['file'])
    if not os.path.exists(path):
        raise XError(f'generated source {path} is missing (generated\\ is local only)')
    insns, labels, first, last = qdecomp.read_listing(path, f['line'], f['name'], f['addr'])
    if a.at is not None:
        if a.range or a.into:
            raise XError('--at / FUNC+OFF does not combine with FROM TO or --into')
        a.range = at_window(a.at, f['addr'], f['size'], a.ctx)
    if a.range_spec is not None:      # S111 QWHERE: --range A..B by function offset (or address) = the same window as --at A..B, never row-cut
        if a.range or a.into or a.at is not None:
            raise XError('--range does not combine with FROM TO, --at or --into')
        import qdecomp_range
        try:
            lo, hi = qdecomp_range.parse(a.range_spec, f['addr'])
        except qdecomp_range.RangeError as e:
            raise XError(str(e))
        lo = lo & ~3
        top = max(f['size'] - 4, 0) if f['size'] else hi
        a.range = [f'{f["addr"] + lo:X}', f'{f["addr"] + min(hi & ~3, top):X}']
    if a.limit is None:
        import qwhere
        a.limit = (1 << 30) if (a.range_spec is not None or qwhere.active()) else 400
    if a.writes or a.reads:
        import qregs
        return qregs.main(a, f, insns, con)
    r = asm_answer(f['name'], f['addr'], f['size'], f['file'], insns, labels, a.func, a.range, a.into, a.limit)
    if 'window' in r and qfactcard.mode_of(a) != 'none':      # S111 FACTCARD: the settled claims that cite an offset inside the window
        r['facts'] = qfactcard.window_marks(qfactcard.collect(f['addr'], con, f['size'], 'asm', qfactcard.mode_of(a)), r['window'][0] - f['addr'], r['window'][1] - f['addr'], qfactcard.mode_of(a))
    if a.json:
        print(json.dumps(r, indent=1))
    else:
        print_asm(r)
    return 0


def hook_jumps_into(main, lo, hi):
    """[[midasm_hook]] rows whose jump_address* lands in [lo,hi): the hook can move control there although no branch instruction does"""
    n = 0
    for p in glob.glob(os.path.join(main, 'config', '*.toml')):
        for m in re.finditer(r'^\s*jump_address\w*\s*=\s*(0x[0-9A-Fa-f]+)', read_text(p), re.M):
            if lo <= int(m.group(1), 16) < hi:
                n += 1
    return n


def _branches(insns, qdecomp):
    """-> [(source insn, target addr, kind)] for every direct branch and every switch case"""
    out = []
    for ins in insns:
        t = qdecomp.branch_target(ins)
        if t is not None and ins.mn.rstrip('+-') != 'bl':
            kind = 'uncond' if ins.mn.rstrip('+-') == 'b' else 'cond'
            out.append((ins, t, kind))
        for c in (ins.cases or []):
            out.append((ins, c, 'switch'))
    return out


def asm_answer(name, start, size, file, insns, labels, funcarg, rng, into, limit):
    import qdecomp
    end = start + size
    br = _branches(insns, qdecomp)
    into_map = {}
    for ins, t, kind in br:
        into_map.setdefault(t, []).append(ins.addr)
    by_addr = {i.addr: i for i in insns}
    r = {'func': name, 'start': start, 'end': end, 'file': file}
    if into:
        lo, hi = parse_addr(into[0]), parse_addr(into[1])
        if hi <= lo:
            raise XError('--into: TO must be above FROM')
        r['into'] = {'from': lo, 'to': hi, 'branches': [], 'fallthrough': None}
        for ins, t, kind in br:
            if lo <= t < hi and not (lo <= ins.addr < hi):
                r['into']['branches'].append({'src': ins.addr, 'target': t, 'kind': kind, 'text': f'{ins.mn} {ins.ops}'.strip(), 'line': ins.line})
        r['into']['branches'].sort(key=lambda b: (b['target'], b['src']))
        prev = by_addr.get(lo - 4)
        if prev is None:
            r['into']['fallthrough'] = 'no instruction before FROM in this function (it is the function entry or outside it)'
        elif prev.mn.rstrip('+-') in ('b', 'blr', 'bctr'):
            r['into']['fallthrough'] = f'no: {prev.addr:08X} is `{(prev.mn + " " + prev.ops).strip()}` (an unconditional end)'
        else:
            r['into']['fallthrough'] = f'YES: {prev.addr:08X} `{(prev.mn + " " + prev.ops).strip()}` falls through into {lo:08X}'
        r['into']['targets_inside'] = sorted({t for _, t, _ in br if lo <= t < hi})
        r['into']['unresolved_bctr'] = sum(1 for i in insns if i.mn.rstrip('+-') == 'bctr' and not i.cases)
        r['into']['hook_jumps'] = hook_jumps_into(main_root(os.path.dirname(os.path.dirname(HERE))), lo, hi)
        if lo < start or hi > end:
            r['warning'] = f'0x{lo:08X}..0x{hi:08X} is not inside {name} (0x{start:08X}..0x{end:08X}); only the part inside it can be answered'
        return r
    if len(rng) == 2:
        lo, hi = parse_addr(rng[0]), parse_addr(rng[1])
        if hi < lo:
            raise XError('FROM must not be above TO')
    elif len(rng) == 0 and re.fullmatch(r'(?:0x)?[0-9A-Fa-f]{8}', funcarg.strip()) and parse_addr(funcarg) != start:
        c = parse_addr(funcarg)
        lo, hi = c - 12 * 4, c + 12 * 4 + 4
    elif len(rng) == 0:
        lo, hi = start, end
    else:
        raise XError('give FROM and TO (both), or none')
    lines = []
    for ins in insns:
        if lo <= ins.addr < hi + (4 if len(rng) == 2 else 0) or (lo <= ins.addr <= hi and len(rng) != 2):
            t = qdecomp.branch_target(ins)
            row = {'addr': ins.addr, 'text': f'{ins.mn} {ins.ops}'.strip(), 'line': ins.line}
            if t is not None and ins.mn.rstrip('+-') != 'bl':
                row['to'] = t
                row['out'] = not (start <= t < end)
            if ins.cases:
                row['cases'] = list(ins.cases)
            if ins.addr in into_map:
                row['from'] = sorted(into_map[ins.addr])
            lines.append(row)
    r['window'] = [lo, hi]
    if lo < start or hi > end:
        r['warning'] = f'0x{lo:08X}..0x{hi:08X} is not inside {name} (0x{start:08X}..0x{end:08X}); rows outside it are not in this listing' + (' (the window holds none of it)' if not lines else '')
    r['rows'] = lines[:limit]
    r['truncated'] = max(0, len(lines) - limit)
    return r


def print_asm(r):
    print(f'{r["func"]}  0x{r["start"]:08X}..0x{r["end"]:08X}  ({r["file"]})')
    if r.get('warning'):
        print(f'WARNING: {r["warning"]}')
    if 'into' in r:
        i = r['into']
        print(f'branches into 0x{i["from"]:08X}..0x{i["to"]:08X} from outside it ({len(i["branches"])}); fall-through: {i["fallthrough"]}')
        for b in i['branches']:
            print(f'  {b["src"]:08X}  {b["text"]:<34} => {b["target"]:08X}  [{b["kind"]}]  (listing line {b["line"]})')
        if not i['branches']:
            print('  (none from inside this function; other functions: q callers)')
        print(f'branch targets inside the range: ' + (', '.join(f'{t:08X}' for t in i['targets_inside']) or 'none'))
        print(f'NOT LISTED (not visible in this listing): branches from OTHER functions (q callers); {i["unresolved_bctr"]} bctr in this function with no resolved switch table '
              f'(an indirect jump could land anywhere); {i["hook_jumps"]} [[midasm_hook]] jump_address row(s) landing in the range')
        return
    print(f'window 0x{r["window"][0]:08X}..0x{r["window"][1]:08X}')
    for row in r['rows']:
        s = f'{row["addr"]:08X}  {row["text"]}'
        s = f'{s:<56} [:{row["line"]}]'      # the listing line (read it with the listing file; `q show` refuses generated\)
        if 'to' in row:
            s += f'   => {row["to"]:08X}' + (' (out)' if row['out'] else '')
        if 'cases' in row:
            s += '   switch -> ' + ','.join(f'{c:08X}' for c in row['cases'][:12]) + (' ...' if len(row['cases']) > 12 else '')
        if 'from' in row:
            s += '   <= ' + ','.join(f'{x:08X}' for x in row['from'][:8]) + (' ...' if len(row['from']) > 8 else '')
        print(s)
    if r.get('truncated'):
        print(f'... {r["truncated"]} more (--limit N)')
    for _off, text in r.get('facts', []):
        print(text)


# ================================================================= q ours hooks

HOOK_CALL = re.compile(r'\bREX_HOOK(_RAW)?\s*\(\s*((?:__imp__)?[A-Za-z_]\w*)\s*[,)]')
DEFINE_RX = re.compile(r'^[ \t]*#[ \t]*define[ \t]+(\w+)\(([^)]*)\)', re.M)
FUNC_TOKEN = re.compile(r'^(?:__imp__)?sub_[0-9A-Fa-f]{8}$')
IDENT = re.compile(r'^[A-Za-z_]\w*$')
BUILDS = {1: 'ON', 2: 'OFF', 4: 'tool', 8: 'other'}


def _mask(text):
    """comments and string / char literal contents blanked (newlines and offsets kept): a commented-out or quoted REX_HOOK_RAW is not a hook"""
    import qx5
    return qx5.mask_all(text)


def _balanced_args(t, i):
    """t[i] == '(' -> (args list, index after the closing paren) or None"""
    depth, j, cur, out = 0, i, [], []
    while j < len(t):
        c = t[j]
        if c == '(':
            depth += 1
            if depth > 1:
                cur.append(c)
        elif c == ')':
            depth -= 1
            if depth == 0:
                out.append(''.join(cur).strip())
                return out, j + 1
            cur.append(c)
        elif c == ',' and depth == 1:
            out.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        j += 1
    return None


def _defines(masked):
    """-> {name: (params, body, offset)} of the function-like #defines of one masked text (continuation lines joined)"""
    out, spans = {}, []
    for m in DEFINE_RX.finditer(masked):
        j = m.end()
        while True:
            e = masked.find('\n', j)
            e = len(masked) if e < 0 else e
            if masked[j:e].rstrip().endswith('\\'):
                j = e + 1
                continue
            break
        body = masked[m.end():e].replace('\\\n', ' ')
        spans.append((m.start(), e))
        out.setdefault(m.group(1), ([p.strip() for p in m.group(2).split(',') if p.strip()], body, m.start(), e))
    return out, spans


def _hooking_macros(all_defs):
    """transitive: a macro whose body holds REX_HOOK*, or invokes a macro that does"""
    hook = {n for n, (ps, b, s, e) in all_defs.items() if re.search(r'\bREX_HOOK(_RAW)?\s*\(', b)}
    changed = True
    while changed:
        changed = False
        for n, (ps, b, s, e) in all_defs.items():
            if n not in hook and any(re.search(r'\b' + re.escape(h) + r'\s*\(', b) for h in hook):
                hook.add(n)
                changed = True
    return hook


def _expand(name, args, defs, hooking, depth=0):
    """the hook targets one invocation of a hooking macro expands to: parameters substituted, `a##b` pasted, nested hooking macros expanded (depth <= 6)"""
    params, body = defs[name][0], defs[name][1]
    sub = dict(zip(params, args))
    if sub:
        body = re.sub(r'\b(' + '|'.join(re.escape(p) for p in sub) + r')\b', lambda m: sub[m.group(1)], body)
    body = re.sub(r'\s*##\s*', '', body)
    out = [m.group(2) for m in HOOK_CALL.finditer(body)]
    if depth < 6:
        for n in hooking:
            for m in re.finditer(r'\b' + re.escape(n) + r'\s*\(', body):
                got = _balanced_args(body, m.end() - 1)
                if got and n in defs:
                    out += _expand(n, got[0], defs, hooking, depth + 1)
    return [t for t in out if IDENT.match(t)]


def _guards(masked):
    """-> list (per 1-based line index 0..n) of the enclosing #if conditions"""
    stack, per = [], []
    for ln in masked.split('\n'):
        m = re.match(r'^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b(.*)$', ln)
        if m:
            k, rest = m.group(1), m.group(2).strip()
            if k in ('if', 'ifdef', 'ifndef'):
                stack.append(('!' if k == 'ifndef' else '') + rest)
            elif k == 'elif' and stack:
                stack[-1] = 'elif ' + rest
            elif k == 'else' and stack:
                stack[-1] = '!(' + stack[-1] + ')'
            elif k == 'endif' and stack:
                stack.pop()
        per.append(list(stack))
    return per


def compiled_builds(main, rel):
    """the builds that compile `rel`: None (S110 QTEXT: no ours index, so no build tag; `q ours covers FILE` names the units that include a file)"""
    return None


def scan_hooks(root, main=None):
    """-> (src_hooks, midasm). src_hooks: dicts {target, kind, file, line, guards, builds, text}. Comments and strings are masked first; function-like macros that expand
    (transitively, with ## pastes) to REX_HOOK* are expanded at every invocation; tools\\ hits are tagged (not the game build); files no translation unit includes are tagged."""
    main = main or main_root(root)
    texts, masks, all_defs = {}, {}, {}
    for p in walk(root, ('src', 'tools', 'probes'), SRC_EXT):
        t = read_text(p)
        if 'HOOK' not in t:
            continue
        rel = rel_of(root, p)
        texts[rel] = t
        masks[rel] = _mask(t)
        for n, v in _defines(masks[rel])[0].items():
            all_defs.setdefault(n, v)
    hooking = _hooking_macros(all_defs)
    src_hooks = []
    cache = {}

    def builds_of(rel):
        if rel not in cache:
            cache[rel] = compiled_builds(main, rel)
        return cache[rel]

    for rel, m in masks.items():
        if not ('REX_HOOK' in m or any(h in m for h in hooking)):
            continue
        in_def = _defines(m)[1]
        starts = [0]
        for mm in re.finditer('\n', m):
            starts.append(mm.end())
        import bisect
        guards = None

        def emit(pos, target, kind):
            nonlocal guards
            if guards is None:
                guards = _guards(m)
            ln = bisect.bisect_right(starts, pos)
            g = [x for x in (guards[ln - 1] if ln - 1 < len(guards) else []) if x]
            src_hooks.append({'target': target, 'kind': kind, 'file': rel, 'line': ln, 'guards': g, 'builds': builds_of(rel), 'tools': rel.startswith('tools/'),
                              'text': texts[rel].split('\n')[ln - 1].strip()[:140]})

        def inside_define(pos):
            return any(s <= pos < e for s, e in in_def)
        for mm in HOOK_CALL.finditer(m):
            if inside_define(mm.start()):
                continue
            emit(mm.start(), mm.group(2), 'REX_HOOK' + ('_RAW' if mm.group(1) else ''))
        for n in sorted(hooking):
            for mm in re.finditer(r'(?<![\w#])' + re.escape(n) + r'\s*\(', m):
                if inside_define(mm.start()):
                    continue
                got = _balanced_args(m, mm.end() - 1)
                if not got:
                    continue
                for t in _expand(n, got[0], all_defs, hooking):
                    emit(mm.start(), t, f'macro {n} (-> REX_HOOK*)')
    src_hooks.sort(key=lambda h: (h['file'], h['line'], h['target']))
    midasm = []
    for p in sorted(glob.glob(os.path.join(root, 'config', '*.toml'))):
        ls = read_text(p).split('\n')
        rel = rel_of(root, p)
        cur = None
        for i, ln in enumerate(ls):
            s = ln.strip()
            if s.startswith('[[midasm_hook]]'):
                cur = {'toml': f'{rel}:{i + 1}', 'toml_file': rel, 'toml_line': i + 1}
                midasm.append(cur)
                continue
            if cur is None:
                continue
            if s.startswith('['):
                cur = None
                continue
            m = re.match(r'^(address|name|registers|return|return_on_true|return_on_false|jump_address|jump_address_on_true|jump_address_on_false|after_instruction)\s*=\s*(.+?)\s*(#.*)?$', s)
            if m:
                cur[m.group(1)] = m.group(2).strip().strip('"')
                if m.group(1) == 'name':
                    cur['toml'] = f'{rel}:{i + 1}'
    for h in midasm:
        try:
            h['addr'] = int(h.get('address', ''), 0)
        except ValueError:
            h['addr'] = None
    return src_hooks, midasm


def callback_defs(root, names):
    """-> {name: [(file, line)]} for `void NAME(` definitions (comments / strings masked; not declarations ending in `;`) in src / probes / tools C++"""
    out = {n: [] for n in names}
    if not names:
        return out
    rx = re.compile(r'\b(' + '|'.join(re.escape(n) for n in names) + r')\s*\(')
    for p in walk(root, ('src', 'probes', 'tools'), SRC_EXT):
        t = read_text(p)
        if not any(n in t for n in names):
            continue
        rel = rel_of(root, p)
        ls = _mask(t).split('\n')
        for i, ln in enumerate(ls):
            m = rx.search(ln)
            if m and re.search(r'\b(void|bool|int)\b[^;=]*$', ln[:m.start()]) and not ln.rstrip().endswith(';') and not re.search(r'[;=]\s*' + re.escape(m.group(1)), ln):
                out[m.group(1)].append((rel, i + 1))
    return out


def generated_decls(main, names):
    """-> {name: (file, line, calls)}: the first `extern <type> NAME(` declaration under generated\\default\\ and the number of lines calling NAME( in that file
    (`NAME(ctx.r3);`, `if (NAME())`, `x = NAME(...)`: any use that is not the declaration). Reads only the generated .cpp files, stopping when every name is found."""
    gen = os.path.join(main, 'generated', 'default')
    out = {}
    if not names or not os.path.isdir(gen):
        return out
    alt = '|'.join(re.escape(n) for n in names)
    rx = re.compile(r'extern\s+[A-Za-z_][\w:<>\s&\*]*?[\s\*&](' + alt + r')\(')
    for fn in sorted(os.listdir(gen)):
        if not fn.endswith('.cpp'):
            continue
        p = os.path.join(gen, fn)
        try:
            with open(p, encoding='utf-8', errors='replace') as f:
                t = f.read()
        except OSError:
            continue
        if 'extern' not in t:
            continue
        found = {}
        for m in rx.finditer(t):
            if m.group(1) not in found:
                found[m.group(1)] = t.count('\n', 0, m.start()) + 1
        for n, ln in found.items():
            if n in out:
                continue
            calls = len([1 for x in re.finditer(r'(?<![\w])' + re.escape(n) + r'\(', t)]) - len(re.findall(r'extern\s+[^;\n]*?\b' + re.escape(n) + r'\(', t))
            out[n] = (fn, ln, calls)
        if len(out) == len(names):
            break
    return out


def func_range(main, addr):
    """-> (start, end, name) of the game function containing addr, from the q index; None when no index"""
    import qpublish
    db = qpublish.newest(os.path.join(main, 'out', 'harness', 'index', 'index.db'))      # S110 QREBUILD review: a copy beside a held index.db is the one to read
    if not os.path.exists(db):
        return None
    try:
        con = sqlite3.connect(f'file:{db}?mode=ro', uri=True)
        r = con.execute('SELECT addr, size, name FROM funcs WHERE addr <= ? AND has_body = 1 ORDER BY addr DESC LIMIT 1', (addr,)).fetchone()
        con.close()
    except sqlite3.Error:
        return None
    if r and addr < r[0] + r[1]:
        return r[0], r[0] + r[1], r[2]
    return None


def cmd_hooks(root, arg, main=None, defs_decls=True):
    main = main or main_root(root)
    src_hooks, midasm = scan_hooks(root, main)
    arg = (arg or '').strip()
    want_addr, want_name, rng = None, None, None
    if arg:
        m = re.fullmatch(r'(?:0x|sub_|__imp__sub_)?([0-9A-Fa-f]{8})', arg)
        if m:
            want_addr = int(m.group(1), 16)
            rng = func_range(main, want_addr)
        else:
            want_name = arg.lower()

    def sel_src(h):
        if want_addr is not None:
            return re.search(f'{want_addr:08X}', h['target'], re.I) is not None
        if want_name:
            return want_name in h['target'].lower()
        return True

    def sel_mid(h):
        if want_addr is not None:
            return h['addr'] == want_addr
        if want_name:
            return want_name in h.get('name', '').lower() or want_name in h.get('address', '').lower()
        return True

    sh = [h for h in src_hooks if sel_src(h)]
    mh = [h for h in midasm if sel_mid(h)]
    if want_addr is not None and rng and rng[0] == want_addr:   # asked for a whole function: its mid-asm hooks inside it too
        mh = [h for h in midasm if h['addr'] is not None and rng[0] <= h['addr'] < rng[1]]
    elif want_addr is not None and rng:                           # an address INSIDE a function: also the function's own REX_HOOK
        sh += [h for h in src_hooks if f'{rng[0]:08X}' in h['target'].upper() and h not in sh]
    names = sorted({h['name'] for h in mh if h.get('name')})
    big = len(names) > 12
    defs = callback_defs(root, names) if defs_decls and not big else {}
    decls = generated_decls(main, names) if defs_decls and not big else {}
    gen_exists = os.path.isdir(os.path.join(main, 'generated', 'default'))
    for h in mh:
        n = h.get('name', '')
        h['defs'] = [f'{f}:{l}' for f, l in defs.get(n, [])]
        d = decls.get(n)
        if big:
            h['generated'] = 'not checked in a listing of many hooks (ask for one: q ours hooks NAME)'
            h['defs_checked'] = False
        else:
            h['defs_checked'] = True
            h['generated'] = (f'{d[0]}:{d[1]} declares it; {d[2]} call site(s)' if d else
                              ('not declared in generated\\default (the codegen has not been run with this hook)' if gen_exists else 'generated\\ absent here'))
        if h['addr'] is not None:
            fr = func_range(main, h['addr'])
            h['in_func'] = f'{fr[2]}+0x{h["addr"] - fr[0]:X}' if fr else ''
    return {'arg': arg, 'src': sh, 'midasm': mh, 'total_src': len(src_hooks), 'total_midasm': len(midasm)}


def _hook_tags(h):
    tags = []
    if h.get('tools'):
        tags.append('tools\\: not the game build')
    if h.get('builds') is not None:
        if not h['builds']:
            tags.append('NOT COMPILED: no translation unit includes this file (dead?)')
        elif not any(b in ('ON', 'OFF') for b in h['builds']):
            tags.append('compiled only in: ' + '/'.join(h['builds']))
        elif h['builds'] != ['OFF', 'ON'] and h['builds'] != ['ON', 'OFF']:
            tags.append('builds: ' + '/'.join(h['builds']))
    if h.get('guards'):
        tags.append('#if ' + ' && '.join(h['guards'])[:90])
    return tags


def print_hooks(r, limit=60):
    a = r['arg'] or 'all'
    print(f'hooks on game code ({a}): {len(r["src"])} REX_HOOK*, {len(r["midasm"])} [[midasm_hook]]   (all: {r["total_src"]} / {r["total_midasm"]})')
    if r['src'] and all(h.get('builds') is None for h in r['src']):
        print('  (no per-build tags: q ours has no index, so a hook in a file no unit compiles is NOT marked; `q ours covers FILE` = the units that include it)')
    for h in r['src'][:limit]:
        tags = _hook_tags(h)
        print(f'  {h["kind"].split(" ")[0]:<9} {h["target"]:<22} {h["file"]}:{h["line"]}' + (f'   via {h["kind"]}' if h['kind'].startswith('macro') else '') + (f'   [{"; ".join(tags)}]' if tags else ''))
    if len(r['src']) > limit:
        print(f'  ... {len(r["src"]) - limit} more (give an address / name)')
    for h in r['midasm'][:limit]:
        print(f'  midasm    {h.get("address", "?"):<10} {h.get("name", "?"):<30} regs {h.get("registers", "-")}   {h["toml"]}' + (f'   in {h["in_func"]}' if h.get('in_func') else ''))
        if h.get('defs_checked'):
            print(f'            callback defined: {", ".join(h["defs"]) if h["defs"] else "NOT FOUND in src / probes / tools"}; generated: {h["generated"]}')
        else:
            print(f'            ({h["generated"]})')
    if len(r['midasm']) > limit:
        print(f'  ... {len(r["midasm"]) - limit} more')
    if not r['src'] and not r['midasm']:
        print('  (no hook found: a REX_HOOK_RAW(sub_X) / REX_HOOK(sub_X, fn) in src, a macro (nested, ## pasted) that expands to one, or a [[midasm_hook]] row)')


# ================================================================= q ours text

WORD_ADDR = re.compile(r'^(?:0x|sub_|__imp__sub_|loc_)?([0-9A-Fa-f]{8})$')


def _word_rx(w):
    m = WORD_ADDR.match(w)
    if m and re.search(r'[0-9]', m.group(1)):
        return re.compile(r'(?<![0-9A-Fa-f])' + m.group(1) + r'(?![0-9A-Fa-f])', re.I)
    return re.compile(re.escape(w), re.I)


def _span_kind(ext, text, ln_start, pos, line):
    """comment / string / code for a hit inside one line (line-local heuristic; C++ block comments only when the line starts inside one is not tracked)"""
    s = line.lstrip()
    if ext in SRC_EXT:
        i = line.find('//')
        if s.startswith(('//', '/*', '*')) or (0 <= i <= pos):
            return 'comment'
        q = line.count('"', 0, pos)
        return 'string' if q % 2 == 1 else 'code'
    if ext in ('.py', '.ps1', '.toml', '.cmake', '.cfg'):
        i = line.find('#')
        if s.startswith('#') or (0 <= i <= pos and line.count('"', 0, i) % 2 == 0 and line.count("'", 0, i) % 2 == 0):
            return 'comment'
        return 'string' if (line.count('"', 0, pos) % 2 == 1 or line.count("'", 0, pos) % 2 == 1) else 'code'
    return ''


DOCS_PATHS = ('docs', ':(glob)*.md', '.claude', 'CMakeLists.txt', 'cmake')   # --docs: which docs / plans / briefs / agent definitions / build files mention a word
CODE_PATHS = ('src', 'tools', 'config', 'probes', 'android')


def _word_pat(w, regex):
    if regex:
        return w
    m = WORD_ADDR.match(w)
    return m.group(1) if (m and re.search(r'[0-9]', m.group(1))) else re.sub(r'([^A-Za-z0-9_ ])', lambda mm: chr(92) + mm.group(1), w)     # a space stays a space (a quoted phrase)


def _git_grep_lines(root, rxs, words, rev=None, docs=False, regex=False, any_=False, paths=None):
    """-> [(rel, line no, text)] of lines holding ALL words (ANY with any_), from `git grep` (tracked + untracked, never ignored files: a C++ line scan of 88 MB per call
    x many agents on the shared PC was the cost of the Python walk), or None when git cannot answer. The exact word rules are applied afterwards in Python.
    rev: search that revision (a branch / tag / commit; committed files only, `--untracked` does not combine with a revision); docs: the docs path set;
    regex: the words are Python regexes, passed through unescaped as -P (PCRE: \\d, (?:...) and the like, which POSIX -E misses or refuses; a git without PCRE
    exits 128 and the plain walk answers)."""
    if rev and rev.startswith('-'):
        raise XError(f'text: {rev!r} is not a revision (a REF starting with - would reach git as an option)')
    args = ['git', '-C', root, 'grep', '-n', '-I', '-i', '-P' if regex else '-E']
    if not rev:
        args.insert(4, '--untracked')
    for i, w in enumerate(words):
        if i:
            args.append('--or' if any_ else '--and')
        args += ['-e', _word_pat(w, regex)]
    if rev:
        args.append(rev)
    args += ['--'] + list(paths or (DOCS_PATHS if docs else CODE_PATHS))     # paths: S111 QGAPS, `--in FILE|FOLDER` searched whatever the code / docs split
    try:
        p = subprocess.run(args, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120)
    except (OSError, subprocess.SubprocessError):
        return None
    if p.returncode not in (0, 1):
        if rev:
            raise XError(f'text: git cannot search revision {rev!r}: {p.stderr.strip()[:200]}')
        return None
    out = []
    for ln in p.stdout.splitlines():
        if rev and ln.startswith(rev + ':'):
            ln = ln[len(rev) + 1:]
        m = re.match(r'^(.*?):(\d+):(.*)$', ln)
        if not m:
            continue
        rel = m.group(1)
        parts = rel.split('/')
        if any(x in SKIP for x in parts) or not rel.endswith(TEXT_EXT):
            continue
        out.append((rel, int(m.group(2)), m.group(3)))
    return out


def cmd_text(root, words, limit_files=60, per_file=4, allhits=False, rev=None, docs=False, regex=False, any_=False, paths=None):
    if not words:
        raise XError('text: give the words to look for (several words = any of them; --and: all on one line), e.g. q ours text PartsBlur   or   q ours text 82C1E840')
    if regex:
        try:
            rxs = [re.compile(w, re.I) for w in words]
        except re.error as e:
            raise XError(f'text: bad regex {e}')
    else:
        rxs = [_word_rx(w) for w in words]

    def line_hit(ln):
        ms = [m for m in (rx.search(ln) for rx in rxs) if m]
        return ms if (ms if any_ else len(ms) == len(rxs)) else None

    hits = []
    got = _git_grep_lines(root, rxs, words, rev=rev, docs=docs, regex=regex, any_=any_, paths=paths)
    if got is None and rev:
        raise XError(f'text: git cannot answer for revision {rev!r}')
    if got is not None:
        for rel, i, ln in got:
            ms = line_hit(ln)
            if ms:
                hits.append({'file': rel, 'line': i, 'kind': _span_kind(os.path.splitext(rel)[1], '', 0, min(m.start() for m in ms), ln), 'text': ln.strip()[:160]})
    else:   # no git: the plain walk
        for p in walk(root, ('docs', '.claude', 'cmake') if docs else CODE_PATHS, TEXT_EXT):
            t = read_text(p)
            if not (any if any_ else all)(rx.search(t) for rx in rxs):
                continue
            rel = rel_of(root, p)
            ext = os.path.splitext(rel)[1]
            for i, ln in enumerate(t.splitlines(), 1):
                ms = line_hit(ln)
                if ms:
                    hits.append({'file': rel, 'line': i, 'kind': _span_kind(ext, t, 0, min(m.start() for m in ms), ln), 'text': ln.strip()[:160]})
    hits.sort(key=lambda h: (h['file'], h['line']))
    return {'words': words, 'n': len(hits), 'files': len({h['file'] for h in hits}), 'hits': hits, 'per_file': 10**9 if allhits else per_file, 'limit_files': limit_files,
            'rev': rev, 'docs': docs, 'regex': regex, 'any': any_}


def print_text(r):
    print(f'text {(" | " if r.get("any") else " ").join(r["words"])}: {r["n"]} lines in {r["files"]} files' + (f' at {r["rev"]}' if r.get('rev') else '') + (' (docs)' if r.get('docs') else ''))
    import qcap   # S108 FD: one sizing rule; the trim line names the total and the exact call that gives the rest
    nshow = qcap.cut_n(r['files'], r['limit_files'], r['per_file'] >= 10**9)       # --all = every line of every file
    per = {}
    for h in r['hits']:
        per[h['file']] = per.get(h['file'], 0) + 1
    if not r['n'] and not r.get('regex'):
        print('  (whole-identifier match, any case; several words = ANY of them, --and = all on one line; -E = a regex such as viewport\\[[45]\\]; --docs = docs / root *.md / CMake; '
              '--in FILE|FOLDER searches that path whatever its type)')
    cur, shown, nf = None, 0, 0
    for h in r['hits']:
        if h['file'] != cur:
            cur, shown, nf = h['file'], 0, nf + 1
            if nf > nshow:
                print('  ' + qcap.trim_line(nshow, r['files'], 'files', r.get('again') or '--all'))
                return
        shown += 1
        if shown <= r['per_file']:
            print(f'  {h["file"]}:{h["line"]}' + (f' [{h["kind"]}]' if h['kind'] else '') + f'  {h["text"]}')
        elif shown == r['per_file'] + 1:
            print(f'  {h["file"]}: ... {per[h["file"]] - r["per_file"]} more lines in this file (--all)')


# ================================================================= shader names

def shader_hits(root, name, limit=40):
    """lines of src shader sources holding NAME as a word: kind decl (a type / field declaration of it, or a #define / uniform) or use"""
    rx = re.compile(r'(?<![A-Za-z0-9_])' + re.escape(name) + r'(?![A-Za-z0-9_])')
    decl = re.compile(r'^\s*(?:layout\s*\([^)]*\)\s*)?(?:uniform|const|in|out|flat|highp|mediump|lowp|float|u?int|bool|[iub]?vec[234]|mat[234]|[A-Za-z_]\w*)\s+(?:[A-Za-z_]\w*\s+)*' + re.escape(name) + r'\s*(?:\[[^\]]*\])?\s*[;=,]')
    hits = []
    for p in walk(root, ('src', 'tools', 'assets'), SHADER_EXT):
        t = read_text(p)
        if not rx.search(t):
            continue
        rel = rel_of(root, p)
        for i, ln in enumerate(t.split('\n'), 1):
            if rx.search(ln):
                kind = 'decl' if (decl.match(ln) or re.match(r'^\s*#\s*define\s+' + re.escape(name) + r'\b', ln)) else 'use'
                if rel.endswith('.fx.json'):
                    kind = 'field' if re.search(r'"' + re.escape(name) + r'"\s*:', ln) else 'use'
                hits.append({'file': rel, 'line': i, 'kind': kind, 'text': ln.strip()[:150]})
    hits.sort(key=lambda h: (h['kind'] != 'decl' and h['kind'] != 'field', h['file'], h['line']))
    return {'name': name, 'n': len(hits), 'hits': hits}


def print_shader_hits(r, limit=40):
    print(f'{r["name"]}: not in the C++ index (nor the branch text / tools index); shader text: {r["n"]} lines ({sum(1 for h in r["hits"] if h["kind"] in ("decl", "field"))} declarations)')
    for h in r['hits'][:limit]:
        print(f'  {h["file"]}:{h["line"]} [{h["kind"]}]  {h["text"]}')
    if r['n'] > limit:
        print(f'  ... {r["n"] - limit} more (--limit N)')
