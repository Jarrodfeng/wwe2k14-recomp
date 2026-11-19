"""qtext: text-level questions about OUR code that the AST index cannot answer (S103 QX3 / QX4). All read this checkout's own files: always current.

    py tools/q/q.py ours line "[scene][maps][e2] plane made 3 (draws 4; ...)"     a log line (or just its tag / words): the src printf / Log site(s) that print it
                                                                                   and every tools\\ python / ps1 regex or string that parses / names it
    py tools/q/q.py ours lit 0x836F3940 | 980 | "gpu stamps check: PASS"           every src / tools / probes file that holds the literal (an integer is searched
                                                                                   word-bounded in its hex AND decimal form; other text as a fixed string)
    py tools/q/q.py ours lit VALUE --all [--tools]                                every line of every file (default 3 per file); --tools = scripts only, hits marked string / comment / code
    py tools/q/q.py ours includes FILE                                             the direct #include lines of a C++ file (resolved to the repo file where possible)
    py tools/q/q.py ours includers FILE                                            every C++ file that #includes FILE (direct)
FILE = a repo-relative path, an absolute one, or a unique basename.
"""
import os
import re
import sys
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qtools  # noqa: E402

CPP = ('.h', '.hpp', '.cpp', '.inl', '.cc', '.inc')
TEXT = CPP + ('.py', '.ps1', '.json', '.toml', '.txt', '.cmake', '.frag', '.vert', '.glsl', '.hlsl', '.cfg', '.bat')
MAX_BYTES = 1_500_000
SKIP = ('__pycache__', 'node_modules', '.git')
LIT_RX = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
SPEC = re.compile(r'%[-+ #0]*(?:\*|\d+)?(?:\.(?:\*|\d+))?(?:hh|h|ll|l|z|j|t|I64|I)?([diuxXfFgGeEsScp%])')
TAG = re.compile(r'^\s*((?:\[[^\]\n]+\]\s*)+)')


def walk(root, exts, tops=('src', 'tools', 'probes')):
    for top in tops:
        for dp, dn, fn in os.walk(os.path.join(root, top)):
            dn[:] = [d for d in dn if d not in SKIP and not d.startswith('.')]
            for f in fn:
                if f.endswith(exts):
                    fp = os.path.join(dp, f)
                    try:
                        if os.path.getsize(fp) > MAX_BYTES or os.sep + 'spv' + os.sep in fp:   # SPIR-V headers (up to 12 MB each) hold no hand-written text
                            continue
                    except OSError:
                        continue
                    yield fp


def rel_of(root, p):
    return os.path.relpath(p, root).replace('\\', '/')


def read_lines(p):
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            return f.read().split('\n')
    except OSError:
        return []


# ---------------------------------------------------------------- line

def sample_of(fmt):
    def sub(m):
        c = m.group(1)
        return {'%': '%', 'd': '1', 'i': '1', 'u': '1', 'x': '1f', 'X': '1F', 'f': '1.0', 'F': '1.0', 'g': '1', 'G': '1', 'e': '1.0e0', 'E': '1.0e0',
                's': 'x', 'S': 'x', 'c': 'x', 'p': '0x1'}.get(c, 'x')
    return SPEC.sub(sub, fmt).replace('{}', '1')


def unesc(s):
    return s.replace('\\n', '\n').replace('\\t', '\t').replace('\\"', '"').replace('\\\\', '\\')


def phrase_of(q):
    """-> (tag chain compacted or '', the longest run (<= 3 words) of plain words after it; numbers, = % ( ) { } ; split runs)"""
    m = TAG.match(q)
    tag = re.sub(r'\s+', '', m.group(1)) if m else ''
    rest = q[m.end():] if m else q
    runs, cur = [], []
    for w in rest.split():
        core = w.rstrip(',:.')
        if not re.fullmatch(r"[A-Za-z][A-Za-z_'-]*", core):
            if cur:
                runs.append(cur)
            cur = []
            continue
        cur.append(w)
        if len(cur) == 3 or w != core:               # a comma / colon ends a run, so the run is text the format really holds in one piece
            runs.append(cur)
            cur = []
    if cur:
        runs.append(cur)
    best = max(runs, key=lambda r: len(' '.join(r).rstrip(',:.')), default=[])
    return tag, ' '.join(best).rstrip(',:.')


CALL_RX = re.compile(r'(?i)\b[\w:]*(printf|print|log|puts|format|cout|cerr|trace|warn|debug|error|info|report|emit|fatal)[\w]*\s*(\(|<<)')
STR_RX = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def strip_comment(ln):
    """drop a // comment and one-line /* */ comments, leaving string literals alone"""
    if '/' not in ln:
        return ln
    out, i, n, instr = [], 0, len(ln), False
    while i < n:
        c = ln[i]
        if instr:
            out.append(c)
            if c == '\\' and i + 1 < n:
                out.append(ln[i + 1])
                i += 1
            elif c == '"':
                instr = False
        elif c == '"':
            instr = True
            out.append(c)
        elif c == '/' and ln[i:i + 2] == '//':
            break
        elif c == '/' and ln[i:i + 2] == '/*':
            j = ln.find('*/', i + 2)
            if j < 0:
                break
            i = j + 1
        else:
            out.append(c)
        i += 1
    return ''.join(out)


def balance(s):
    s = STR_RX.sub('""', s)
    return s.count('(') - s.count(')')


def print_statements(text):
    """every statement (call start line .. matching close paren) that is a print / log call and holds string literals -> [(line, fmt)]; comments dropped"""
    ls = [strip_comment(x) for x in text.split('\n')]
    out, i = [], 0
    while i < len(ls):
        ln = ls[i]
        if '"' not in ln or ln.lstrip().startswith('#'):
            i += 1
            continue
        a = i
        while a > 0 and i - a < 10 and (ls[a].lstrip().startswith('"') or balance(ls[a - 1]) > 0) and ls[a - 1].strip() and not ls[a - 1].lstrip().startswith('#'):
            a -= 1
        b, bal = a, balance(ls[a])
        while b < len(ls) - 1 and b - a < 30 and (bal > 0 or ls[b].rstrip().endswith('<<') or ls[b + 1].lstrip().startswith('"')):
            b += 1
            if ls[b].lstrip().startswith('#'):
                b -= 1
                break
            bal += balance(ls[b])
        stmt = '\n'.join(ls[a:b + 1])
        first = next((k for k in range(a, b + 1) if '"' in ls[k]), i)
        if CALL_RX.search(STR_RX.sub('""', stmt)):
            lits = [unesc(m.group(0)[1:-1]) for m in STR_RX.finditer(stmt)]
            out.append((first + 1, ''.join(lits)))
        i = max(b + 1, i + 1)
    return out


def fmt_regex(fmt, prefix=False):
    """a printf / {} format -> a list of regexes (one, or in prefix mode the first-k-parts ones) matching one printed line (None when the format is too generic to identify a line)"""
    f = fmt.rstrip('\n').replace('{}', '\x00')
    parts, pos, static = [], 0, 0
    for m in SPEC.finditer(f):
        piece = f[pos:m.start()]
        static += len(re.sub(r'\W', '', piece))
        parts.append(re.escape(piece).replace('\x00', '.*?'))
        c = m.group(1)
        parts.append('%' if c == '%' else (r'[-+0-9.eExXa-fA-FnaifINF]+' if c in 'diuxXfFgGeEp' else '.*?'))
        pos = m.end()
    tail = f[pos:]
    static += len(re.sub(r'\W', '', tail))
    parts.append(re.escape(tail).replace('\x00', '.*?'))
    if static < 8:
        return None
    try:
        if prefix:                    # a pasted line cut with "...": the regex of the first k parts, longest first (the caller wants one that reaches the cut)
            return [re.compile(re.sub(r'(\\s\+)+$', '', ''.join(parts[:k]).replace(r'\ ', r'\s+'))) for k in range(len(parts), 2, -1)]
        return [re.compile(''.join(parts).replace(r'\ ', r'\s+'))]
    except re.error:
        return None


def cpp_sites(root, tag, words, encl, line=''):
    """print / log call sites for a pasted line (whole-line regex of each format) or, failing that, for its tag / words (the needle inside a string literal)"""
    inner = re.findall(r'\[([^\]]+)\]', tag)
    toks = {x for x in re.findall(r'[A-Za-z]{5,}', line)} | set(inner)
    toks = sorted(toks, key=len, reverse=True)[:8]
    need_n = max(1, (len(toks) + 1) // 2)
    cands = []
    for p in walk(root, CPP):
        try:
            with open(p, encoding='utf-8', errors='replace') as f:
                text = f.read()
        except OSError:
            continue
        if toks and sum(t in text for t in toks) < need_n:
            continue
        if not toks and words not in text:
            continue
        rel = rel_of(root, p)
        for ln, fmt in print_statements(text):
            cands.append((rel, ln, fmt))
    m = TAG.match(line)
    rest = line[m.end():] if m else line
    sites = []
    if len(rest.split()) >= 3:
        cut = bool(re.search(r'(\.\.\.|…)$', line.strip()))
        flat = re.sub(r'\s+', ' ', re.sub(r'(\.\.\.|…)$', '', line.strip()).strip())
        for rel, ln, fmt in cands:
            pieces = SPEC.split(fmt.rstrip(chr(10)).replace('{}', '%s'))
            lead = pieces[0] if cut else max(pieces, key=len)
            if len(lead) < 4 or re.sub(r'\s+', ' ', lead) not in flat:
                continue
            ok = False
            for rx in fmt_regex(fmt, cut) or []:
                mm = rx.search(flat)
                if mm and (not cut or mm.end() >= len(flat) - 2):
                    ok = True
                    break
            if ok:
                sites.append({'file': rel, 'line': ln, 'func': encl(rel, ln), 'fmt': fmt[:400], 'tag_near': True, 'exact': True})
    if sites:
        return sites
    for rel, ln, fmt in cands:
        has_tag = bool(tag) and tag in re.sub(r'\s+', '', fmt)
        if (words and words in fmt and (has_tag or not tag)) or (not words and has_tag):
            sites.append({'file': rel, 'line': ln, 'func': encl(rel, ln), 'fmt': fmt[:400], 'tag_near': has_tag, 'exact': False})
    if not sites and tag:            # the words found nothing (a %s held them): the tag alone
        for rel, ln, fmt in cands:
            if tag in re.sub(r'\s+', '', fmt):
                sites.append({'file': rel, 'line': ln, 'func': encl(rel, ln), 'fmt': fmt[:400], 'tag_near': True, 'exact': False})
    sites.sort(key=lambda s: (not s['tag_near'], s['file'], s['line']))
    return sites


def cmd_line(root, q, encl):
    q = q.strip().strip('"\'')
    tag, words = phrase_of(q)
    if not tag and not words:
        return None
    sites = cpp_sites(root, tag, words, encl, q)
    samples = [sample_of(s['fmt']) for s in sites if s['fmt']]
    full_line = q if re.search(r'\d', q) else None
    idx = qtools.load(root)
    needle = words or tag
    parsers = []
    for rel, rec in idx.items():
        cands = [(ln, pat, 'regex') for ln, pat in rec['rxs']] + [(ln, v, 'string') for ln, v in rec['strs'] if len(v) < 400]
        seen = set()
        for ln, pat, kind in cands:
            if ln in seen:
                continue
            lit = re.sub(r'\\(.)', r'\1', pat)
            names = bool(words and (words in lit or words in pat)) or bool(tag and tag in re.sub(r'\s+', '', lit))
            matched = None
            if qtools.RX_LIKE.search(pat) or kind == 'regex':
                try:
                    with warnings.catch_warnings():
                        warnings.simplefilter('ignore')
                        rx = re.compile(pat)
                    matched = False
                    for smp in ([full_line] if full_line else []) + samples:
                        key = words if words and words in smp else tag
                        mm, at = rx.search(smp), smp.find(key)
                        runs = re.findall(r'[A-Za-z][A-Za-z0-9_ -]{4,}', lit)
                        if mm and at >= 0 and any(x in smp for x in runs) and mm.start() < at + len(key) and mm.end() > at:     # the match must cover the key words, not just any text
                            matched = True
                            break
                except re.error:
                    matched = None
            if matched or names:
                seen.add(ln)
                parsers.append({'file': rel, 'line': ln, 'how': 'regex matches the line' if matched else ('names it (regex)' if kind == 'regex' or qtools.RX_LIKE.search(pat) else 'names it (string)'),
                                'text': pat[:130]})
    parsers.sort(key=lambda r: (r['how'] != 'regex matches the line', r['file'], r['line']))
    return {'query': q, 'tag': tag, 'words': words, 'sites': sites, 'parsers': parsers}


def print_line(r, args):
    print(f'line {r["query"][:80]!r}: tag {r["tag"] or "-"}, key words {r["words"] or "-"}')
    print(f'  printed by ({len(r["sites"])}):')
    for s in r['sites'][:args.limit]:
        print(f'    {s["file"]}:{s["line"]}  in {s["func"] or "?"}{"   (whole line matches)" if s.get("exact") else ("" if s["tag_near"] else "   (tag not in the format)")}   "{s["fmt"][:110]}"')
    print(f'  parsed / named by tools ({len(r["parsers"])}):')
    for s in r['parsers'][:args.limit]:
        print(f'    {s["file"]}:{s["line"]}  {s["how"]}   {s["text"][:100]}')
    if len(r['parsers']) > args.limit:
        print(f'    ... {len(r["parsers"]) - args.limit} more (--limit)')


# ---------------------------------------------------------------- lit

def parse_int(q):
    q = q.strip()
    try:
        if re.fullmatch(r'0[xX][0-9a-fA-F\']+[uUlL]*', q):
            return int(q.rstrip('uUlL').replace("'", ''), 16), 'hex'
        if re.fullmatch(r'-?\d+', q):
            return int(q), 'dec'
    except ValueError:
        pass
    return None, None


def lit_patterns(q):
    v, kind = parse_int(q)
    if v is None:
        return [re.compile(re.escape(q.strip().strip('"')))], f'text {q!r}'
    pats = [re.compile(r"(?<![\w.])0[xX][0']*" + re.escape(format(v, 'x')) + r"[uUlL]*(?![\w.])", re.I)]
    if v >= 0:
        pats.append(re.compile(r"(?<![\w.])" + str(v) + r"(?:[uUlL]+|\.0+f?|f)?(?![\w.])"))
    else:
        pats.append(re.compile(r"(?<![\w])" + str(v) + r"(?![\w.])"))
    return pats, f'integer {v} (0x{v:X}), hex and decimal forms, word-bounded'


def line_kind(ln, ext, rx_list):
    """py / ps1 / bat: where the first match of the literal sits on its line: 'string' (inside quotes), 'comment' (after an unquoted #) or 'code'"""
    m = None
    for rx in rx_list:
        m = rx.search(ln)
        if m:
            break
    if not m:
        return ''
    if ext == '.bat' and ln.lstrip().lower().startswith(('rem ', '::')):
        return 'comment'
    q, i = '', 0
    while i < m.start():
        c = ln[i]
        if q:
            if c == '\\' and ext == '.py':
                i += 1
            elif c == q:
                q = ''
        elif c in '"\'':
            q = c
        elif c == '#' and ext in ('.py', '.ps1'):
            return 'comment'
        i += 1
    return 'string' if q else 'code'


def py_spans(text):
    """python: [(start, end, 'string' | 'comment')] absolute offsets from the tokenize module (docstrings and triple-quoted strings across lines, f-string text, # comments), or None when the file does not tokenize"""
    import io
    import tokenize
    starts = [0]
    for m in re.finditer('\n', text):
        starts.append(m.end())
    fm = getattr(tokenize, 'FSTRING_MIDDLE', -1)
    fs = getattr(tokenize, 'FSTRING_START', -1)
    fe = getattr(tokenize, 'FSTRING_END', -1)
    spans = []
    try:
        for t in tokenize.generate_tokens(io.StringIO(text).readline):
            if t.type == tokenize.COMMENT:
                kind = 'comment'
            elif t.type in (tokenize.STRING, fm, fs, fe):
                kind = 'string'
            else:
                continue
            (r0, c0), (r1, c1) = t.start, t.end
            if r0 - 1 < len(starts) and r1 - 1 < len(starts):
                spans.append((starts[r0 - 1] + c0, starts[r1 - 1] + c1, kind))
    except (tokenize.TokenError, IndentationError, SyntaxError):
        return None
    spans.sort()
    return spans


def ps1_spans(text):
    """PowerShell: [(start, end, 'string' | 'comment')] from a whole-file scan: <# block #>, # at a token start (a mid-word # is not a comment), "..." with backtick escapes and doubled quotes,
    '...' with doubled quotes, @" "@ / @' '@ here-strings (the closing mark on its own line start)"""
    spans, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '<' and text.startswith('<#', i):
            j = text.find('#>', i + 2)
            j = n if j < 0 else j + 2
            spans.append((i, j, 'comment'))
            i = j
            continue
        if c == '#' and (i == 0 or text[i - 1] in ' \t\r\n;({|,='):
            j = text.find('\n', i)
            j = n if j < 0 else j
            spans.append((i, j, 'comment'))
            i = j
            continue
        if c == '@' and i + 1 < n and text[i + 1] in '"\'':
            q = text[i + 1]
            k = i + 2
            while k < n and text[k] in ' \t\r':
                k += 1
            if k < n and text[k] == '\n':
                m = re.compile('\n' + q + '@').search(text, k)
                j = n if not m else m.end()
                spans.append((i, j, 'string'))
                i = j
                continue
        if c in '"\'':
            j = i + 1
            while j < n:
                d = text[j]
                if c == '"' and d == '`':
                    j += 2
                    continue
                if d == c:
                    if j + 1 < n and text[j + 1] == c:
                        j += 2
                        continue
                    break
                j += 1
            j = min(j + 1, n)
            spans.append((i, j, 'string'))
            i = j
            continue
        i += 1
    return spans


def span_kind(spans, off):
    import bisect
    k = bisect.bisect_right([s[0] for s in spans], off) - 1
    return spans[k][2] if k >= 0 and spans[k][0] <= off < spans[k][1] else 'code'


TOOL_EXTS = ('.py', '.ps1', '.bat', '.json', '.toml', '.txt', '.cfg', '.tsv', '.java', '.kt', '.cmake')


def cmd_lit(root, q, encl, limit=400, tools=False):
    pats, desc = lit_patterns(q)
    hits = []
    files = walk(root, TOOL_EXTS, ('tools', 'android', 'config', 'probes')) if tools else walk(root, TEXT)
    if tools:
        desc += '; tools / android / config / probes scripts only (py, ps1, bat, json ...)'
    for p in files:
        try:
            with open(p, encoding='utf-8', errors='replace') as f:
                text = f.read()
        except OSError:
            continue
        if not any(rx.search(text) for rx in pats):
            continue
        ls = text.split(chr(10))
        rel = rel_of(root, p)
        ext = os.path.splitext(rel)[1]
        spans, starts = False, None          # py / ps1: string / comment spans of the WHOLE file (tokenize / a whole-file state scan), computed at the first hit
        for i, ln in enumerate(ls, 1):
            ms = [m for m in (rx.search(ln) for rx in pats) if m]
            if ms:
                h = {'file': rel, 'line': i, 'func': encl(rel, i) if rel.endswith(CPP) else '', 'text': ln.strip()[:150]}
                if ext in ('.py', '.ps1'):
                    if spans is False:
                        spans = (py_spans(text) if ext == '.py' else ps1_spans(text))
                        starts = [0]
                        for mm in re.finditer('\n', text):
                            starts.append(mm.end())
                    if spans is not None:
                        h['kind'] = span_kind(spans, starts[i - 1] + min(m.start() for m in ms))
                    else:
                        h['kind'] = line_kind(ln, ext, pats)      # the file does not tokenize: the per-line guess
                elif ext == '.bat':
                    h['kind'] = line_kind(ln, ext, pats)
                hits.append(h)
                if len(hits) >= 20000:
                    break
    hits.sort(key=lambda h: (h['file'], h['line']))
    return {'query': q, 'how': desc, 'n': len(hits), 'files': len({h['file'] for h in hits}), 'hits': hits}


def print_lit(r, args):
    print(f'lit: {r["how"]}: {r["n"]} lines in {r["files"]} files')
    by = {}
    for h in r['hits']:
        by.setdefault(h['file'], []).append(h)
    shown = 0
    for f, hs in by.items():
        print(f'  {f}  ({len(hs)})')
        for h in (hs if getattr(args, 'all', False) else hs[:3]):
            print(f'    {h["line"]}: {h["text"][:110]}' + (f'   [in {h["func"]}]' if h['func'] else '') + (f'   <{h["kind"]}>' if h.get('kind') else ''))
        if not getattr(args, 'all', False) and len(hs) > 3:
            print(f'    ... {len(hs) - 3} more lines in this file (--all)')
        shown += 1
        if shown >= args.limit:
            print(f'  ... {len(by) - shown} more files (--limit; --all lists every line of the files shown)')
            break


# ---------------------------------------------------------------- includes

INC = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')


def find_cpp(root, spec):
    spec = spec.replace('\\', '/')
    r = root.replace('\\', '/').rstrip('/') + '/'
    if spec.startswith(r):
        spec = spec[len(r):]
    if os.path.isfile(os.path.join(root, spec)):
        return spec
    base = spec.split('/')[-1]
    hits = [rel_of(root, p) for p in walk(root, CPP) if os.path.basename(p) == base and rel_of(root, p).endswith(spec)]
    return hits[0] if len(hits) == 1 else (None if not hits else hits)


def resolve_inc(root, from_rel, kind, target, byname):
    t = target.replace('\\', '/')
    cands = []
    if kind == '"':
        cands.append(os.path.normpath(os.path.join(os.path.dirname(from_rel), t)).replace('\\', '/'))
    cands += [t, 'src/' + t, 'tools/' + t]
    for c in cands:
        if os.path.isfile(os.path.join(root, c)):
            return c
    hs = byname.get(os.path.basename(t))
    if hs:
        suf = [h for h in hs if h.endswith(t)]
        return (suf or hs)[0] if len(suf or hs) == 1 else (suf or hs)[0] + f' (+{len(suf or hs) - 1} same name)'
    return None


def by_name(root):
    d = {}
    for p in walk(root, CPP):
        d.setdefault(os.path.basename(p), []).append(rel_of(root, p))
    return d


def cmd_includes(root, rel):
    names = by_name(root)
    out = []
    for i, ln in enumerate(read_lines(os.path.join(root, rel)), 1):
        m = INC.match(ln)
        if m:
            out.append({'line': i, 'kind': m.group(1), 'target': m.group(2), 'resolved': resolve_inc(root, rel, m.group(1), m.group(2), names)})
    return {'file': rel, 'includes': out}


def print_includes(r, args):
    print(f'{r["file"]}: {len(r["includes"])} #include lines')
    for x in r['includes'][:args.limit * 4]:
        q = '"' if x['kind'] == '"' else '<'
        e = '>' if q == '<' else '"'
        print(f'  {x["line"]}: #include {q}{x["target"]}{e}' + (f'  -> {x["resolved"]}' if x['resolved'] else '  (system / not in the repo)'))


def cmd_includers(root, rel):
    names = by_name(root)
    base = os.path.basename(rel)
    out = []
    for p in walk(root, CPP):
        r2 = rel_of(root, p)
        if r2 == rel:
            continue
        for i, ln in enumerate(read_lines(p), 1):
            m = INC.match(ln)
            if m and os.path.basename(m.group(2)) == base:
                res = resolve_inc(root, r2, m.group(1), m.group(2), names)
                if res and res.split(' ')[0] == rel:
                    out.append({'file': r2, 'line': i, 'text': ln.strip()})
    out.sort(key=lambda x: (x['file'], x['line']))
    return {'file': rel, 'includers': out}


def print_includers(r, args):
    print(f'{r["file"]}: included directly by {len(r["includers"])} files')
    for x in r['includers'][:args.limit * 3]:
        print(f'  {x["file"]}:{x["line"]}  {x["text"]}')
