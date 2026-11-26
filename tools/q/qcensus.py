r"""qcensus (S103 lane QCF): which line of OUR code bumps the counter behind a census / log field.

    py tools/q/q.py ours census FIELD [--in PATH]
        FIELD is the word(s) a log line prints (black_no_3d, pic_why, "named but never drawn", shown_without_row). The chain it prints:
          field -> the format literal that holds it (file:line, the enclosing function)
                -> the argument expression that literal prints for it (printf: the Nth arg of the format; `" field " + to_string(x)` / `<< x`: the
                   expression right after it)
                -> the counter (member / global / array element: c.never_fed_bit[b] -> never_fed_bit), a plain local is expanded through the
                   statements that assign it in the same function (depth 3)
                -> every write to that counter in src / probes / tools: ++x, x++, --x, x += ..., x = ..., x[i] ..., .store / .fetch_* / .exchange, with
                   file:line and the enclosing function, and the declaration line(s).
        --in PATH keeps only the writers whose file path contains PATH (a member name like `frames` is declared in many structs: the writers are NAME matches).
    Text level, like `q ours str`: this checkout's own files, always current, no index, no worktree remap.
    What it handles: printf-style formats (adjacent literals, a literal split over lines, PRIu64 style macros, %% and `*` widths), string concatenation
      (`" field " + std::to_string(expr)`, `+ n(expr)`, `<< expr`), a multi-line statement, a counter reached through a local, a counter named only inside a
      loop that builds the text (the near-name fallback: marked `by name`, UNCONFIRMED).
    Trust rules: a printf spec is taken as the field's value only when the text between the field and it is empty or one of `: = ( [`; a spec right BEFORE the field
      (`%u ops`, `%llu drawn`) is a value-first label and is used when the next spec is not adjacent; both adjacent, or neither, = `candidate (UNCONFIRMED)`, never
      `prints:`. A concat piece is trusted when no text follows the field in its literal. A local is expanded only when declared before the site in a block holding it,
      and only through statements before the site; a member / global is never expanded; a local that cannot be expanded (a subscripted array `kinds[3]`, a scalar) is
      the counter, its writers limited to its function. At most 15 format literals are analysed (src first); the rest are listed.
    What it does NOT: a format held in a variable / returned by a function (the args are elsewhere), a literal inside a #define (the macro is named, its uses are
      not expanded), a counter written through a pointer / reference handed to another function, a write by memset / std::fill, a member name shared by several
      structs (all writers by that name are listed: --in narrows them). A counter resolved by name (not from an argument) is marked, never presented as fact.
Selftest: py -I tools/q/qcensus_selftest.py
"""
import bisect
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qtext  # noqa: E402


SITE_CAP = 15


class CError(Exception):
    pass


SPEC_RX = re.compile(r'%[-+ #0]*(\*|\d+)?(?:\.(\*|\d+))?(?:hh|h|ll|l|L|z|j|t|I64|I)?([diuoxXfFgGeEaAsScpn%])')
KEYWORDS = {'unsigned', 'signed', 'long', 'int', 'short', 'char', 'float', 'double', 'bool', 'const', 'static_cast', 'reinterpret_cast', 'const_cast',
            'dynamic_cast', 'sizeof', 'true', 'false', 'nullptr', 'this', 'auto', 'uint8_t', 'uint16_t', 'uint32_t', 'uint64_t', 'int8_t', 'int16_t',
            'int32_t', 'int64_t', 'size_t', 'void', 'return', 'else', 'new', 'delete', 'typename', 'struct', 'class', 'volatile', 'static', 'inline', 'constexpr',
            'if', 'for', 'while', 'switch', 'case', 'default', 'break', 'continue', 'do', 'goto', 'throw', 'noexcept', 'operator', 'ull', 'llu', 'f', 'u'}
ACCESSORS = {'load', 'size', 'get', 'value', 'count', 'empty', 'c_str', 'data', 'length', 'at', 'front', 'back', 'begin', 'end', 'first', 'second'}
CHAIN_RX = re.compile(r'(?<![\w.])(?:::)?[A-Za-z_]\w*(?:\s*(?:::|\.|->)\s*[A-Za-z_]\w*|\s*\[(?:[^\[\]]|\[[^\[\]]*\])*\])*')
CTRL = {'if', 'for', 'while', 'switch', 'catch', 'else', 'do', 'try', 'return'}


# ------------------------------------------------------------------ text preparation

def strip_all(text, blank_strings=False):
    """the file with // and /* */ comments replaced by spaces (newlines kept, so offsets and lines stay); char literals blanked ('x'); string literals kept
    (or their contents blanked with blank_strings)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == '//':
            j = text.find('\n', i)
            j = n if j < 0 else j
            seg = text[i:j]
            # a comment continued with a trailing backslash keeps going
            while seg.endswith('\\') and j < n:
                k = text.find('\n', j + 1)
                k = n if k < 0 else k
                seg += text[j:k]
                j = k
            out.append(re.sub(r'[^\n]', ' ', seg))
            i = j
        elif two == '/*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif c == 'R' and text[i + 1:i + 2] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == '_')):
            m = re.match(r'R"([^(\s]*)\(', text[i:])
            if m:
                end = text.find(')' + m.group(1) + '"', i + m.end())
                j = n if end < 0 else end + len(m.group(1)) + 2
                seg = text[i:j]
                out.append(re.sub(r'[^\n"]', ' ', seg) if blank_strings else seg)
                i = j
            else:
                out.append(c)
                i += 1
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"' and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n) if j < n and text[j] == '"' else j
            seg = text[i:j]
            out.append('"' + ' ' * max(0, len(seg) - 2) + '"' if blank_strings and len(seg) >= 2 else seg)
            i = j
        elif c == "'":
            j = i + 1
            while j < n and text[j] != "'" and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            j = min(j + 1, n)
            seg = text[i:j]
            if re.fullmatch(r"'(?:[^'\\\n]|\\.)*'", seg) and len(seg) <= 6:
                out.append("'" + 'x' * (len(seg) - 2) + "'")      # a digit separator (1'000) is not matched: left as it is below
                i = j
            else:
                out.append(c)
                i += 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def line_starts(text):
    st = [0]
    for m in re.finditer('\n', text):
        st.append(m.end())
    return st


def line_of(starts, off):
    return bisect.bisect_right(starts, off)      # 1-based


def unesc(s):
    return re.sub(r'\\(.)', lambda m: {'n': '\n', 't': '\t', 'r': '\r'}.get(m.group(1), m.group(1)), s)


LIT = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
GAP_OK = re.compile(r'^\s*(?:PRI[A-Za-z0-9]+\s*)?$')


def literal_runs(code):
    """-> [(start, end, joined_text, n_literals)]: adjacent string literals (only whitespace / a PRIxx macro between them) are ONE literal to the compiler"""
    runs, cur = [], None
    for m in LIT.finditer(code):
        if cur and GAP_OK.match(code[cur[1]:m.start()]):
            gap = code[cur[1]:m.start()].strip()
            cur = (cur[0], m.end(), cur[2] + ('llu' if gap else '') + unesc(m.group(1)), cur[3] + 1)
            runs[-1] = cur
        else:
            cur = (m.start(), m.end(), unesc(m.group(1)), 1)
            runs.append(cur)
    return runs


# ------------------------------------------------------------------ scopes (the enclosing function, from the text)

def scopes(code):
    """-> [(start_line, end_line, qualified_name)] for every function body (outermost; lambdas and blocks belong to the function around them)"""
    starts = line_starts(code)
    stack, funcs = [], []
    b = 0                          # start of the current header (after the last ; { })
    n = len(code)
    i = 0
    depth_paren = 0
    while i < n:
        c = code[i]
        if c == '"':
            j = i + 1
            while j < n and code[j] != '"' and code[j] != '\n':
                j += 2 if code[j] == '\\' else 1
            i = j + 1
            continue
        if c == '#' and (i == 0 or code[i - 1] == '\n' or code[:i].rsplit('\n', 1)[-1].strip() == ''):
            j = i
            while True:
                k = code.find('\n', j)
                k = n if k < 0 else k
                if k > 0 and code[k - 1] == '\\' and k < n:
                    j = k + 1
                    continue
                break
            i = k
            b = k
            continue
        if c == '(':
            depth_paren += 1
        elif c == ')':
            depth_paren = max(0, depth_paren - 1)
        elif c == ';' and depth_paren == 0:
            b = i + 1
        elif c == '{':
            header = code[b:i]
            kind, name = classify_header(header, [e for e in stack])
            stack.append((kind, name, line_of(starts, i), header))
            b = i + 1
            depth_paren = 0
        elif c == '}':
            if stack:
                kind, name, ln, _h = stack.pop()
                if kind == 'func' and not any(e[0] == 'func' for e in stack):
                    types = [e[1] for e in stack if e[0] == 'type']
                    q = name if (not types or '::' in name) else '::'.join(types) + '::' + name
                    funcs.append((ln, line_of(starts, i), q))
            b = i + 1
            depth_paren = 0
        i += 1
    return funcs


def classify_header(header, stack):
    h = re.sub(r'\s+', ' ', header).strip()
    if any(e[0] == 'func' for e in stack):
        return 'block', ''
    m = re.match(r'^(?:template\s*<.*>\s*)?(?:typedef\s+)?(?:struct|class|union)\s+(?:\[\[[^\]]*\]\]\s*)?([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^({]*)?$', h)
    if m:
        return 'type', m.group(1)
    if re.search(r'\bnamespace\b', h) or re.match(r'^extern\s+"C"', h):
        return 'ns', ''
    if re.search(r'\benum\b', h) or h == '' or '=' in h.split('(')[0]:
        return 'block', ''
    first = re.match(r'[A-Za-z_]\w*', h)
    if first and first.group(0) in CTRL:
        return 'block', ''
    m = re.search(r'((?:[~A-Za-z_]\w*\s*(?:<[^<>()]*>\s*)?::\s*)*(?:operator\s*\S+?|~?[A-Za-z_]\w*))\s*\(', h)
    if m and m.group(1) not in CTRL:
        return 'func', re.sub(r'\s+', '', m.group(1))
    return 'block', ''


def func_at(funcs, ln):
    best = None
    for s, e, q in funcs:
        if s <= ln <= e and (best is None or s >= best[0]):
            best = (s, e, q)
    return best


# ------------------------------------------------------------------ expression -> counters

def split_args(code, pos):
    """code[pos] is just after the format literal. -> [arg text] when `, a, b, c)` follows, [] when `)` / nothing follows, None when no argument list is there"""
    j = pos
    n = len(code)
    while j < n and code[j].isspace():
        j += 1
    if j >= n or code[j] != ',':
        return None if (j < n and code[j] not in ')') else []
    args, depth, cur, k = [], 0, [], j + 1
    while k < n:
        c = code[k]
        if c == '"':
            m = LIT.match(code, k)
            if m:
                cur.append(m.group(0))
                k = m.end()
                continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            if depth == 0:
                break
            depth -= 1
        elif c == ';' and depth == 0:
            break
        elif c == ',' and depth == 0:
            args.append(''.join(cur).strip())
            cur = []
            k += 1
            continue
        elif c == '<' and code[k:k + 2] == '<<' and depth == 0:
            pass
        cur.append(c)
        k += 1
    args.append(''.join(cur).strip())
    return [a for a in args if a != '']


def concat_expr(code, pos):
    """the expression right after a literal that ends a `" field " + expr` / `<< expr` piece, up to the next `+ "text"` / `<< "text"` / `;` / `,` / unmatched `)`"""
    j = pos
    n = len(code)
    while j < n and code[j].isspace():
        j += 1
    if code[j:j + 1] == '+':
        j += 1
    elif code[j:j + 2] == '<<':
        j += 2
    else:
        return None
    k, depth, cur = j, 0, []
    while k < n:
        c = code[k]
        if c == '"' and LIT.match(code, k):
            m = LIT.match(code, k)
            cur.append(m.group(0))
            k = m.end()
            continue
        if c in '([{':
            depth += 1
        elif c in ')]}':
            if depth == 0:
                break
            depth -= 1
        elif depth == 0 and c in ';,':
            break
        elif depth == 0 and (c == '+' or code[k:k + 2] == '<<'):
            rest = code[k + (1 if c == '+' else 2):].lstrip()
            if rest[:1] == '"' or rest[:1] == "'":
                break
        cur.append(c)
        k += 1
    e = ''.join(cur).strip()
    return e or None


def counters_of(expr):
    """the counter chains an expression reads: [(chain text, member name, has_object)]; casts, std:: and plain function calls dropped; `.load()` style accessors
    stripped; `x.method()` that is not an accessor is a call result (dropped)"""
    res = []
    e = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', expr)
    for m in CHAIN_RX.finditer(e):
        ch = m.group(0)
        if ch.startswith('::'):
            ch = ch[2:]
        after = e[m.end():].lstrip()
        parts = [p for p in re.split(r'\s*(?:::|\.|->)\s*', re.sub(r'\[(?:[^\[\]]|\[[^\[\]]*\])*\]', lambda x: '\0' * len(x.group(0)), ch)) if p]
        names = [re.sub('\0', '', p).strip() for p in parts]
        names = [x for x in names if x]
        if not names:
            continue
        if names[0] == 'std' or names[0] in KEYWORDS:
            continue
        if after.startswith('('):
            last = names[-1]
            if len(names) > 1 and last in ACCESSORS:
                ch = re.sub(r'\s*(?:\.|->)\s*' + re.escape(last) + r'$', '', ch)
                names = names[:-1]
            else:
                continue                                  # a function / call result
        mem = names[-1]
        if mem in KEYWORDS or re.fullmatch(r'k[A-Z]\w*', mem) and len(names) == 1 and False:
            continue
        res.append((ch.strip(), mem, len(names) > 1))
    seen, out = set(), []
    for c in res:
        if c[0] not in seen:
            seen.add(c[0])
            out.append(c)
    return out


LOCAL_TYPE = r'(?:\b(?!return\b|else\b|case\b|new\b|delete\b|throw\b|co_return\b|goto\b)[A-Za-z_][\w:]*(?:<[^;(){}]*>)?[\s*&]+)'


def block_of(text, pos):
    """(open, close) offsets of the innermost {} block of `text` that holds pos ((0, len) when none)"""
    depth, i = 0, pos - 1
    while i >= 0:
        c = text[i]
        if c == '}':
            depth += 1
        elif c == '{':
            if depth == 0:
                break
            depth -= 1
        i -= 1
    if i < 0:
        return 0, len(text)
    depth, j, n = 0, i + 1, len(text)
    while j < n:
        c = text[j]
        if c == '{':
            depth += 1
        elif c == '}':
            if depth == 0:
                break
            depth -= 1
        j += 1
    return i, j


def local_info(fn_code, name, site_off):
    """-> (declared, [(rhs text, offset)]): `name` is a local when a declaration of it BEFORE site_off sits in a block that holds site_off (the nearest one counts);
    the statements assigning it are only those between that declaration and site_off (never a later one, never a sibling block's). A member is never a local."""
    decl_off = None
    for m in re.finditer(r'(?<![\w.>:])' + re.escape(name) + r'\b', fn_code[:site_off]):
        pre = fn_code[max(0, m.start() - 160):m.start()]
        post = fn_code[m.end():m.end() + 300]
        if re.search(LOCAL_TYPE + r'$', pre) and typeish_before(pre) and re.match(r'\s*(?:=(?!=)|\{|;|,|\)|\[|:)', post):
            b = block_of(fn_code, m.start())
            if b[0] <= site_off <= b[1]:
                decl_off = m.start()
    if decl_off is None:
        return False, []
    rhs = []
    for m in re.finditer(r'(?<![\w.>:])' + re.escape(name) + r'\b', fn_code[decl_off:site_off]):
        st = decl_off + m.end()
        post = fn_code[st:st + 300]
        asg = re.match(r'\s*(?:\+=|-=|\|=|=(?!=))\s*', post)
        if asg:
            k, depth, cur = asg.end(), 0, []
            while k < len(post):
                c = post[k]
                if c in '([{':
                    depth += 1
                elif c in ')]}':
                    if depth == 0:
                        break
                    depth -= 1
                elif c in ';,' and depth == 0:
                    break
                cur.append(c)
                k += 1
            rhs.append((''.join(cur), decl_off + m.start()))
        else:
            br = re.match(r'\s*\{([^;]*)\}', post)
            if br and decl_off + m.start() == decl_off:
                rhs.append((br.group(1), decl_off))
    return True, rhs


def resolve_counters(expr, fn_code, site_off, depth=3, seen=None, top=True):
    """-> [(chain, member, how, scoped)]. A plain name is a LOCAL only when local_info says so (declared before the site in a block holding it); it is expanded through the
    statements before the site that assign it; a local that cannot be expanded, and a subscripted local array (kinds[3]), is a counter SCOPED to its function (scoped=True).
    A member / global is never expanded (holes_ += pending does not make `pending` the counter)."""
    seen = seen if seen is not None else set()
    out = []
    for chain, mem, has_obj in counters_of(expr):
        if not has_obj and fn_code is not None:
            declared, rhs = local_info(fn_code, mem, site_off)
            if declared:
                is_arr = '[' in chain
                sub = []
                if rhs and not is_arr and mem not in seen and depth > 0:
                    seen.add(mem)
                    for r, off in rhs:
                        for c, m2, how, sc in resolve_counters(r, fn_code, off, depth - 1, seen, False):
                            sub.append((c, m2, how if how.startswith('via') else f'via local {mem}', sc))
                if sub:
                    out.extend(sub)
                elif is_arr or top:
                    out.append((chain, mem, 'local array' if is_arr else 'local', True))
                continue
        out.append((chain, mem, 'member' if has_obj else 'global / field', False))
    seen2, res = set(), []
    for c in out:
        if (c[0], c[1], c[3]) not in seen2:
            seen2.add((c[0], c[1], c[3]))
            res.append(c)
    return res


# ------------------------------------------------------------------ the census

class Files:
    """lazy per-file cache of the comment-stripped text, the string-blanked code, line starts and function scopes"""

    def __init__(self, root):
        self.root = root
        self.c = {}
        self.wcache = {}

    def get(self, p):
        if p not in self.c:
            with open(p, encoding='utf-8', errors='replace') as f:
                raw = f.read()
            self.c[p] = {'raw': raw, 'code': None}
        return self.c[p]

    def code(self, p):                  # comments out, strings kept
        e = self.get(p)
        if e['code'] is None:
            e['code'] = strip_all(e['raw'])
            e['starts'] = line_starts(e['code'])
        return e

    def blank(self, p):                 # comments out, string contents blanked
        e = self.code(p)
        if 'blank' not in e:
            e['blank'] = strip_all(e['raw'], blank_strings=True)
        return e

    def funcs(self, p):
        e = self.blank(p)
        if 'funcs' not in e:
            e['funcs'] = scopes(e['blank'])
        return e['funcs']


def cpp_files(root, tops=('src', 'probes', 'tools')):
    return list(qtext.walk(root, qtext.CPP, tops))


def define_of(code, off):
    """the #define NAME whose (backslash-continued) body holds offset off, or None"""
    ls = code.rfind('\n', 0, off) + 1
    first = ls
    while True:
        prev_end = first - 1
        if prev_end > 0 and code[prev_end - 1] == '\\':
            first = code.rfind('\n', 0, prev_end - 1) + 1
        else:
            break
    m = re.match(r'\s*#\s*define\s+(\w+)', code[first:first + 200])
    return m.group(1) if m else None


def find_sites(root, field, fl, files):
    fl_low = field
    sites = []
    word = re.compile((r'(?<![\w])' if re.match(r'\w', field) else '') + re.escape(field) + (r'(?![\w])' if re.search(r'\w$', field) else ''))
    inlit = re.compile(r'"[^"\n]*' + re.escape(field) + r'[^"\n]*"')
    for p in files:
        e = fl.get(p)
        if fl_low not in e['raw'] or not inlit.search(e['raw']):      # B3: the field must sit inside a string literal (a literal split in two is missed here: `str` finds those)
            continue
        e = fl.code(p)
        code, starts = e['code'], e['starts']
        rel = qtext.rel_of(root, p)
        for s, en, joined, nlit in literal_runs(code):
            if not word.search(joined):
                continue
            ln = line_of(starts, s)
            fn = func_at(fl.funcs(p), ln)
            site = {'file': rel, 'path': p, 'line': ln, 'func': fn[2] if fn else '', 'fn_range': (fn[0], fn[1]) if fn else None, 'literals': nlit,
                    'format': joined.replace('\n', '\\n'), 'end': en, 'define': define_of(code, s)}
            sites.append(site)
    return sites


ALLOWED_GAP = {'', ':', '=', '(', '['}


def spec_arg_index(specs, k):
    idx = 0
    for st, m in specs[:k]:
        idx += 1 + (1 if m.group(1) == '*' else 0) + (1 if m.group(2) == '*' else 0)
    m = specs[k][1]
    return idx + (1 if m.group(1) == '*' else 0) + (1 if m.group(2) == '*' else 0)


def analyse_site(site, field, fl):
    """-> [(chain, member, how, scope)]. site['confirmed'] is False when the field -> argument step is a guess (no argument is then printed as traced)."""
    p = site['path']
    e = fl.code(p)
    code = e['code']
    blank = fl.blank(p)['blank']
    joined = site['format'].replace('\\n', '\n')
    wm = re.search((r'(?<![\w])' if re.match(r'\w', field) else '') + re.escape(field) + (r'(?![\w])' if re.search(r'\w$', field) else ''), joined)
    pos = wm.start() if wm else joined.find(field)
    site['notes'] = []
    site['confirmed'] = True
    if site['define']:
        site['notes'].append(f'inside #define {site["define"]}: the macro is not expanded here; its uses name the real arguments (q ours refs {site["define"]})')
        return []
    tail = pos + len(field)
    specs = [(m.start(), m) for m in SPEC_RX.finditer(joined) if m.group(3) != '%']
    after = [i for i, (st, m) in enumerate(specs) if st >= tail]
    before = [i for i, (st, m) in enumerate(specs) if st < pos]
    arg_expr = None
    how = None
    prev_ok = False
    pk = None
    if before:
        pk = before[-1]
        pend = specs[pk][0] + len(specs[pk][1].group(0))
        prev_ok = joined[pend:pos].strip() == ''          # `%u ops`: a value written BEFORE its label
    nxt = after[0] if after else None
    next_ok = nxt is not None and joined[tail:specs[nxt][0]].strip() in ALLOWED_GAP   # `label %u`, `label: %u`, `label (%u`
    k, mode = None, None
    if nxt is not None or prev_ok:
        if next_ok and not prev_ok:
            k, mode = nxt, 'next spec'
        elif prev_ok and not next_ok:
            k, mode = pk, 'value-first: the spec just BEFORE the field'
        elif nxt is not None:
            k = nxt
            mode = 'AMBIGUOUS: a spec sits right before AND right after the field' if prev_ok and next_ok else 'the next spec is not adjacent to the field (text "' + joined[tail:specs[nxt][0]].strip()[:30] + '" between)'
            site['confirmed'] = False
        else:
            k, mode = pk, 'value-first: the spec just BEFORE the field'
    if k is not None:
        idx = spec_arg_index(specs, k)
        args = split_args(code, site['end'])
        if args is None:
            site['notes'].append('the format literal is not followed by an argument list (the arguments are given elsewhere)')
        elif idx < len(args):
            arg_expr = args[idx]
            how = f'printf spec #{idx + 1} ({specs[k][1].group(0)}) of {len(args)} args; {mode}'
        else:
            site['notes'].append(f'spec #{idx + 1} but only {len(args)} arguments follow')
    else:
        arg_expr = concat_expr(code, site['end'])
        how = 'concatenated: the expression after the literal' if arg_expr else None
        if arg_expr and joined[tail:].strip() not in ALLOWED_GAP:
            site['confirmed'] = False
            how = 'concatenated, but text follows the field in the literal ("' + joined[tail:].strip()[:30] + '"): the expression after the literal may belong to that text'
        if arg_expr is None and re.search(r'\w', joined[tail:]):
            site['notes'].append('the field is followed by more text in the same literal and no spec: no value is printed right after it')
    if arg_expr and not site['confirmed']:
        site['notes'].append('UNCONFIRMED: the field -> argument step is ambiguous; the argument below is the candidate, not a traced fact')
    site['arg'] = arg_expr
    site['how'] = how
    fr = site['fn_range']
    fn_code = None
    fstart = 0
    if fr:
        st = e['starts']
        fstart = st[fr[0] - 1]
        fn_code = blank[fstart:(st[fr[1]] if fr[1] < len(st) else len(blank))]
    scope = (site['file'], fr) if fr else None
    res = []
    if arg_expr:
        res = resolve_counters(arg_expr, fn_code, site['end'] - fstart)
        res = [(c, m, ('' if site['confirmed'] else 'UNCONFIRMED candidate: ') + h, scope if sc else None) for c, m, h, sc in res]
    if not res:
        # near-name fallbacks: chains whose member name contains the field text, (1) in the statements right after the literal (a loop that prints the value on
        # the next line), then (2) anywhere in the format's own function. Always marked: no argument was traced to them.
        key = re.sub(r'[^A-Za-z0-9_]+', '_', field).strip('_').lower()
        for tier, text, label in (('near', blank[site['end']:site['end'] + 700], 'in the statements right after the literal'), ('fn', fn_code, 'in the same function')):
            if not text or not key or res:
                continue
            found, seen = [], set()
            for m in CHAIN_RX.finditer(text):
                for chain, mem, ho in counters_of(m.group(0)):
                    if key in mem.lower() and mem not in seen:
                        seen.add(mem)
                        found.append((chain, mem, f'BY NAME (unconfirmed): a name {label} that contains the field text; no argument was traced to it', None))
            res = found
            if res:
                site['notes'].append(f'no argument expression resolved: the counter below is a name match {label}, not a traced argument')
    return res


WRITE_AFTER = [
    ('inc', re.compile(r'\s*(\+\+|--)')),
    ('add', re.compile(r'\s*(\+=|-=|\*=|/=|\|=|&=|\^=|<<=|>>=)')),
    ('store', re.compile(r'\s*(?:\.|->)\s*(store|fetch_add|fetch_sub|fetch_or|fetch_and|fetch_xor|exchange|compare_exchange_\w+)\s*\(')),
    ('set', re.compile(r'\s*=(?!=)')),
]
SUBSCRIPT = re.compile(r'(?:\s*\[(?:[^\[\]]|\[[^\[\]]*\])*\])*')
DECL_BEFORE = re.compile(r'^\s*(?:(?:static|inline|constexpr|const|mutable|volatile|extern|thread_local)\s+)*[A-Za-z_][\w:]*(?:<[^;(){}]*>)?[\s*&]+'
                         r'(?:[A-Za-z_]\w*(?:\s*\[[^\]]*\])?\s*(?:=\s*[^,;]*)?,\s*)*$')
PRE_INC = re.compile(r'(\+\+|--)\s*(?:[A-Za-z_][\w:]*(?:\s*\[(?:[^\[\]]|\[[^\[\]]*\])*\])*\s*(?:\.|->)\s*)*$')


BUILTIN_T = {'int', 'bool', 'float', 'double', 'char', 'short', 'long', 'unsigned', 'signed', 'auto', 'size_t', 'void'}


def typeish_word(w):
    return w in BUILTIN_T and w != 'auto' or w.endswith('_t') or '::' in w or w[:1].isupper()


def typeish_before(before):
    """the text before a name ends with a type token (uint64_t, std::atomic<..>, CamelCase, a builtin) and whitespace / * / &: `struct S { uint64_t NAME` is a declaration, `y = a * NAME` is not"""
    m = re.search(r'(?<![\w.>])([A-Za-z_][\w:]*)(?:<[^;(){}]*>)?[\s*&]+$', before)
    if not m:
        return False
    w = m.group(1)
    return w in BUILTIN_T or w.endswith('_t') or '::' in w or w[:1].isupper() or '<' in before[m.start():]


def decl_snip(line, name):
    t = line.strip()
    i = t.find(name)
    if len(t) <= 150 or i < 0:
        return t[:150]
    a = max(0, i - 60)
    return ('...' if a else '') + t[a:a + 150] + '...'


def func_text(root, fl, rel, ln):
    """the blanked body text of the function holding line ln of rel (or '')"""
    p = os.path.join(root, rel.replace('/', os.sep))
    fn = func_at(fl.funcs(p), ln)
    if not fn:
        return ''
    b = fl.blank(p)
    st = b['starts']
    return b['blank'][st[fn[0] - 1]:(st[fn[1]] if fn[1] < len(st) else len(b['blank']))]


def type_of_var(text, var):
    """the declared type of the local / loop variable `var` in a function body (`const PictureObs& q`), or None (auto / not found)"""
    for m in re.finditer(r'(?<![\w.>])([A-Za-z_][\w:]*)(?:<[^;(){}]*>)?\s*[&*]*\s+' + re.escape(var) + r'\s*(?=[:=;,)({\[])', text):
        t = m.group(1)
        if t not in KEYWORDS and t not in ('auto', 'return', 'else', 'case', 'new', 'delete', 'throw') and typeish_word(t):
            return t
    return None


def feeders_of(root, writers, fl, files, only_in=None, cap=12):
    """the index of an array counter (`pic_why[q.why < 9 ? q.why : 8]`) names a member of an object `q`: who sets that member. The object's declared type T is read from the
    writer's function (`const PictureObs& q`); only member writes inside functions that name T are listed (a bare `.why` is far too common a name: with no type found,
    the writes in the same folder). -> [{member, from, type, writers, n}]"""
    out = []
    done = set()
    dirs = {os.path.dirname(w['file']) for w in writers}
    for w in writers:
        for chain, mem, has_obj in counters_of(w['sub'].strip('[]')) if w['sub'] else []:
            if not has_obj or mem in done:
                continue
            done.add(mem)
            obj = re.split(r'\s*(?:\.|->)\s*', re.sub(r'\[[^\]]*\]', '', chain))[0]
            typ = type_of_var(func_text(root, fl, w['file'], w['line']), obj)
            ws, _d = writers_of(root, mem, fl, files, only_in)
            ws = [x for x in ws if x['member'] and x['kind'] in ('set', 'inc', 'add', 'store')]
            if typ:
                ws = [x for x in ws if re.search(r'(?<![\w])' + re.escape(typ.split('::')[-1]) + r'(?![\w])', func_text(root, fl, x['file'], x['line']))]
            else:
                ws = [x for x in ws if os.path.dirname(x['file']) in dirs]
            if ws:
                out.append({'member': mem, 'from': f'{w["file"]}:{w["line"]}', 'type': typ, 'writers': ws[:cap], 'n': len(ws)})
    return out


def writers_of(root, name, fl, files, only_in=None):
    ck = (name, only_in)
    if ck in fl.wcache:
        return fl.wcache[ck]
    fl.wcache[ck] = r = _writers_of(root, name, fl, files, only_in)
    return r


def _writers_of(root, name, fl, files, only_in=None):
    writes, decls = [], []
    rx = re.compile(r'(?<![\w])' + re.escape(name) + r'(?![\w])')
    for p in files:
        e = fl.get(p)
        if name not in e['raw'] or not rx.search(e['raw']):
            continue
        rel = qtext.rel_of(root, p)
        if only_in and only_in not in rel:
            continue
        b = fl.blank(p)
        code = b['blank']
        starts = b['starts']
        rawlines = e['raw'].split('\n')
        for m in rx.finditer(code):
            s, en = m.start(), m.end()
            ln = line_of(starts, s)
            ls = starts[ln - 1]
            before = code[ls:s]
            pre_member = re.search(r'(?:\.|->)\s*$', before) is not None
            tail = code[en:en + 400]
            sub = SUBSCRIPT.match(tail)
            t2 = tail[sub.end():] if sub else tail
            kind = None
            for k, rxk in WRITE_AFTER:
                mm = rxk.match(t2)
                if mm:
                    if k == 'set' and tail[sub.end():][mm.end():mm.end() + 1] == '=':
                        continue
                    kind = k
                    break
            if kind is None and PRE_INC.search(before):
                kind = 'inc'
            is_decl = (not pre_member) and (DECL_BEFORE.match(before) is not None or typeish_before(before)) and re.match(r'\s*(?:\[|=|;|,|\{|\)|:)', tail) is not None
            if is_decl:
                decls.append({'file': rel, 'line': ln, 'text': decl_snip(rawlines[ln - 1], name)})
                continue
            if kind is None:
                continue
            fn = func_at(fl.funcs(p), ln)
            rhs = ''
            if kind in ('set', 'add', 'store'):
                mm = dict(WRITE_AFTER)[kind].match(t2)
                rhs = re.sub(r'\s+', ' ', t2[mm.end():mm.end() + 70].split(';')[0]).strip()
            writes.append({'file': rel, 'line': ln, 'kind': kind, 'func': fn[2] if fn else '', 'text': rawlines[ln - 1].strip()[:150], 'rhs': rhs,
                           'member': pre_member, 'sub': (sub.group(0).strip() if sub else '')})
    uniq = {(w['file'], w['line'], w['kind']): w for w in writes}
    return sorted(uniq.values(), key=lambda w: (w['file'], w['line'])), sorted({(d['file'], d['line']): d for d in decls}.values(), key=lambda d: (d['file'], d['line']))


def cmd_census(root, field, only_in=None, limit=40):
    field = field.strip()
    if len(field) >= 2 and field[0] == field[-1] == '"':
        field = field[1:-1]
    if not field:
        raise CError('census: give the field a log line prints (black_no_3d, pic_why, "named but never drawn")')
    files = cpp_files(root)
    fl = Files(root)
    sites = find_sites(root, field, fl, files)
    result = {'field': field, 'sites': [], 'run': None}
    sites.sort(key=lambda x: (not x['file'].startswith('src/'), x['file'], x['line']))
    result['more_sites'] = max(0, len(sites) - SITE_CAP)       # B3: only the first SITE_CAP are analysed (src first); the rest are counted
    result['skipped_sites'] = [f'{x["file"]}:{x["line"]}' for x in sites[SITE_CAP:SITE_CAP + 12]]
    for site in sites[:SITE_CAP]:
        chains = analyse_site(site, field, fl)
        counters = []
        for chain, mem, how, scope in chains:
            w, d = writers_of(root, mem, fl, files, only_in)
            if scope:
                lo, hi = scope[1]
                w = [x for x in w if x['file'] == scope[0] and lo <= x['line'] <= hi]
                d = [x for x in d if x['file'] == scope[0] and lo <= x['line'] <= hi]
                how += f' (a local of {site["func"] or "its function"}: writers limited to that function)'
            counters.append({'chain': chain, 'name': mem, 'how': how, 'writers': w, 'decls': d, 'feeders': [] if scope else feeders_of(root, w, fl, files, only_in)})
        result['sites'].append({'file': site['file'], 'line': site['line'], 'func': site['func'], 'format': site['format'], 'arg': site.get('arg'),
                                'how': site.get('how'), 'confirmed': site.get('confirmed', True), 'notes': site.get('notes', []), 'counters': counters})
    if not sites:
        w, d = writers_of(root, field, fl, files, only_in) if re.fullmatch(r'[A-Za-z_]\w*', field) else ([], [])
        if w or d:
            result['sites'].append({'file': '(no format literal)', 'line': 0, 'func': '', 'format': field, 'arg': None, 'how': None,
                                    'notes': ['no string literal holds it as a word: taken as a COUNTER NAME (the log may print it under another word: try `q ours census` with that word)'],
                                    'counters': [{'chain': field, 'name': field, 'how': 'by name (the field was given as the counter itself)', 'writers': w, 'decls': d,
                                                  'feeders': feeders_of(root, w, fl, files, only_in)}]})
    result['sites'].sort(key=lambda s: (not s['arg'], not s.get('confirmed', True), not s['counters'], s['file'], s['line']))
    return result


def short_fmt(fmt, field, w=150):
    i = fmt.find(field)
    a = max(0, i - 40)
    return ('...' if a else '') + fmt[a:a + w] + ('...' if a + w < len(fmt) else '')


def print_census(r, args):
    lim = getattr(args, 'limit', 30)
    print(f'census "{r["field"]}": {len(r["sites"])} format literal(s) holding it')
    if not r['sites']:
        print('  no string literal in src / probes / tools holds it. `q ours str` / `q ours lit` search other spellings; a field built from pieces '
              '("hol" "es") only matches when the literals are adjacent.')
    for i, s in enumerate(r['sites'], 1):
        print(f'  [{i}] {s["file"]}:{s["line"]}' + (f'  in {s["func"]}' if s['func'] else ''))
        print(f'      format:  "{short_fmt(s["format"], r["field"])}"')
        if s['arg'] and s.get('confirmed', True):
            print(f'      prints:  {s["arg"][:150]}   <{s["how"]}>')
        elif s['arg']:
            print(f'      candidate (UNCONFIRMED, not traced):  {s["arg"][:150]}   <{s["how"]}>')
        for n in s['notes']:
            print(f'      note:    {n}')
        if not s['counters']:
            print('      counter: not resolved (see the notes)')
        for c in s['counters']:
            print(f'      counter: {c["name"]}   [{c["chain"]}; {c["how"]}]')
            for d in c['decls'][:4]:
                print(f'        declared  {d["file"]}:{d["line"]}   {d["text"]}')
            if len(c['decls']) > 1:
                print(f'        ({len(c["decls"])} declarations of the name {c["name"]}: the writers below are matches by NAME, not by struct; --in PATH narrows them)')
            for fd in c.get('feeders', []):
                print(f'        index fed by .{fd["member"]} (the subscript of the write at {fd["from"]}); ' + (f'writes of .{fd["member"]} inside functions that name its type {fd["type"]}' if fd['type'] else f'type not found: writes of .{fd["member"]} in the same folder(s)') + f' ({fd["n"]}):')
                for x in fd['writers']:
                    print(f'          {x["file"]}:{x["line"]}  {x["kind"]:5s}' + (f'  in {x["func"]}' if x['func'] else '') + f'\n              {x["text"]}')
            if not c['writers'] and any(re.search(r'\b(?:constexpr|const)\b', d['text']) for d in c['decls']):
                print(f'        writers:  none: it is a constant. `q ours refs {c["name"]}` lists who reads it')
            elif not c['writers']:
                print('        writers:  none found (no ++ / += / = / .store in src / probes / tools: a member copied whole, written by a memset, or through a reference to another function)')
            else:
                print(f'        writers ({len(c["writers"])}):')
            for w in c['writers'][:lim * 2]:
                print(f'          {w["file"]}:{w["line"]}  {w["kind"]:5s}' + (f'  in {w["func"]}' if w['func'] else '') + f'\n              {w["text"]}')
            if len(c['writers']) > lim * 2:
                print(f'          ... {len(c["writers"]) - lim * 2} more (--limit / --in)')
    if r.get('more_sites'):
        print(f'  ... {r["more_sites"]} more format literal(s) not analysed (the first {SITE_CAP} are, src first): ' + ', '.join(r['skipped_sites']) + (' ...' if r['more_sites'] > len(r['skipped_sites']) else '') + ' - narrow the field')
    return 0
