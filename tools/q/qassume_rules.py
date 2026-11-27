r"""qassume_rules (S111 SWEEP1): the mechanical patterns of `q assume` (kinds A-F; docs\research\s111\SWEEP_PLAN.md step 1).

Each rule reads ONE file through a `View` (qsym.Src: raw lines, comment / literal masked code lines, enclosing spans) and returns hits
    {'kind': 'A'..'F', 'sub': 'A1'.., 'line': first line, 'end': last line, 'sym': declared identifier or '', 'text': the matched code, 'why': the short reason}
The caller (qassume.py) adds the file, the enclosing name, the game addresses near the place and the seam rows that cite it.

THE KINDS (a row is a place where our code MAY hard-code a choice the game makes at run time; a reader sorts it):
  A  literal game-id / key tables:  A1 a brace list of >= 4 id-like numbers (a table), A2 a switch with >= 3 numeric case labels, A3 one statement comparing
     against >= 3 id-like numbers (an id-range / key-set test)
  B  count rules:  B1 a count word compared with a literal (2..99), B2 a count word compared with a Need-style name / call, B3 a function named like a
     Need-style rule (Need / Expect / Full / Whole / Complete / Required ...)
  C  caller / address gates:  C1 a return-address / caller word in a comparison, C2 a comparison (or case) against a game address (0x82.. / 0x83.. or a
     constant whose value is one)
  D  keep-one / pick-one / first-match:  D1 a first-match chain (>= 2 `if (x) return x;` in a function), D2 a ternary `x ? x : y`, D3 a keep-one /
     latch / prefer / family word in a declaration or a function / type name, D4 a comment that says "wins" / "first match" / "keeps one" / "falls back to"
  E  assumption comments:  E1 [U] / [L] marks, E2 until / assume / fitted / guess, E3 never / always next to a condition
  F  switch-gated forks:  F1 a cvar / environment switch read inside a condition, F2 a switch variable defined from the environment, F3 a planted twin
     (a bool member whose comment says twin / plant / old rule)
Precision matters more than recall for A-D: every pattern below is as narrow as its known fault allowed (see qassume_selftest.py).
"""
import bisect
import re
import sys
import os

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import qx5  # noqa: E402

# ------------------------------------------------------------------------------------------------------------------ numbers

SIZE_LITS = {128, 256, 320, 360, 480, 512, 576, 600, 640, 720, 768, 800, 900, 1024, 1080, 1200, 1280, 1440, 1600, 1920, 2048, 2160, 2560, 3840, 4096, 8192, 16384, 32768, 65535, 65536}
NUM_RX = re.compile(r'(?<![\w.])(?:0[xX]([0-9A-Fa-f]{1,8})|(\d{1,9}))(?:[uU][lL]{0,2}|[lL]{1,2}[uU]?)?(?![\w.])')
FLOAT_RX = re.compile(r'(?<![\w])\d+\.\d*(?:[eE][-+]?\d+)?[fF]?(?![\w.])|(?<![\w.])\d+[fF]\b|\d[eE][-+]?\d')
ADDR_RX = re.compile(r'(?<![\w.])0[xX](8[23][0-9A-Fa-f]{6})(?:[uU][lL]{0,2})?(?![\w.])')
ADDR_NEAR_RX = re.compile(r'sub_[0-9A-Fa-f]{8}|(?<![\w.])0[xX]8[23][0-9A-Fa-f]{6}(?![0-9A-Fa-f])')


def id_like(m):
    """a NUM_RX match -> True when it reads like a game id / key (not a size, not a small number)"""
    if m.group(1) is not None:
        v = int(m.group(1), 16)
        return 0x100 <= v <= 0xFFFF and v not in SIZE_LITS and not 0xD800 <= v <= 0xDFFF or 0x82000000 <= v <= 0x83FFFFFF
    v = int(m.group(2))
    return 100 <= v <= 99999 and v not in SIZE_LITS and v != 255


def id_lits(text):
    return [m for m in NUM_RX.finditer(text) if id_like(m)]


# ------------------------------------------------------------------------------------------------------------------ the view of one file

class View:
    """one file: raw lines, code lines (comments and literal insides blanked), comment text per line, statements, enclosing names"""

    def __init__(self, src):
        self.src = src
        self.rel = src.rel
        self.raw = [l.rstrip('\r') for l in src.lines]
        src.masked
        self.code = [l.rstrip('\r') for l in src.mlines]
        n = len(self.raw)
        self.cmt = [''] * (n + 2)                       # 1-based: the comment text on each line
        text = src.text
        nl = [m.start() for m in re.finditer('\n', text)]
        for m in qx5.CMT.finditer(text):
            s = m.group(0)
            if s[0] != '/':
                continue
            ln = bisect.bisect_left(nl, m.start())      # 0-based line of the match start
            for k, part in enumerate(s.split('\n')):
                if ln + k + 1 < len(self.cmt):
                    self.cmt[ln + k + 1] += ' ' + part.rstrip('\r')
        self._stmts = None
        self._offs = None

    # 1-based accessors (a line past the end is empty)
    def c(self, ln):
        return self.code[ln - 1] if 1 <= ln <= len(self.code) else ''

    def r(self, ln):
        return self.raw[ln - 1] if 1 <= ln <= len(self.raw) else ''

    def n(self):
        return len(self.raw)

    def stmts(self):
        """[(first line, last line, code text)]: code lines joined until one ends in ; { } or : (at most 8 lines); preprocessor lines are skipped"""
        if self._stmts is None:
            out, cur, first = [], [], 0
            for i, l in enumerate(self.code, 1):
                s = l.strip()
                if not s or s.startswith('#'):
                    if cur:
                        out.append((first, i - 1, ' '.join(cur)))
                        cur = []
                    continue
                if not cur:
                    first = i
                cur.append(s)
                if s.endswith((';', '{', '}', ':')) or len(cur) >= 8:
                    out.append((first, i, ' '.join(cur)))
                    cur = []
            if cur:
                out.append((first, len(self.code), ' '.join(cur)))
            self._stmts = out
        return self._stmts

    def rawcode(self, a, b):
        """the raw text of lines a..b with each line's comment cut off (literals kept), joined"""
        parts = []
        for k in range(a, b + 1):
            s = self.r(k)
            c = self.cmt[k].strip() if k < len(self.cmt) else ''
            if c:
                cut = s.find(c.split(' ')[0]) if c else -1
                if cut >= 0:
                    s = s[:cut]
            parts.append(s.strip())
        return re.sub(r'\s+', ' ', ' '.join(p for p in parts if p)).strip()

    def func_name(self, ln):
        f = self.src.func_at(ln)
        return (f['qual'] or f['name']) if f else None

    def func_span(self, ln):
        f = self.src.func_at(ln)
        return (f['start'], f['end']) if f else None

    def scope_name(self, ln):
        """-> (name, how): the enclosing function; else the enclosing type / namespace; else the next function / type that STARTS within 14 lines below (a comment above it); else file scope"""
        f = self.src.func_at(ln)
        if f is not None:
            return (f['qual'] or f['name']), 'func'
        lab = self.src.label(ln)
        spans = self.src.spans
        best = None
        for sp in spans:
            if sp['kind'] in ('func', 'type') and ln < sp['start'] <= ln + 14:
                if best is None or sp['start'] < best['start']:
                    best = sp
        if best is not None and lab in ('(file scope)',) or best is not None and lab.startswith('(in ') and best['start'] > ln:
            return (best['qual'] or best['name']), 'next'
        if lab.startswith('(in '):
            return lab[4:-1], 'type'
        return '(file scope)', 'file'


def hit(kind, sub, line, end, text, why, sym=''):
    return {'kind': kind, 'sub': sub, 'line': line, 'end': end, 'text': re.sub(r'\s+', ' ', text).strip(), 'why': why, 'sym': sym}


def _lastname(head):
    m = re.search(r'(\w+)\s*(?:\[[^\]]*\]\s*)*(?:=\s*|\(\s*)?$', head.rstrip())
    return m.group(1) if m else ''


# ------------------------------------------------------------------------------------------------------------------ A

def rule_a1(v):
    """a brace list (no ';' inside, not an enum) holding >= 4 id-like numbers and no floats: a literal table of game ids / keys / addresses"""
    code = '\n'.join(v.code)
    nlpos = [m.start() for m in re.finditer('\n', code)]

    def line_of(p):
        return bisect.bisect_left(nlpos, p) + 1
    stack, groups = [], []
    for m in re.finditer(r'[{};]', code):
        ch, p = m.group(), m.start()
        if ch == '{':
            stack.append([p, False])
        elif ch == ';':
            for e in stack:
                e[1] = True
        elif stack:
            st = stack.pop()
            if not st[1]:
                groups.append((st[0], p))
    out, cover = [], []
    for a, b in sorted(groups, key=lambda g: (g[0], -g[1])):
        if cover and a < cover[-1]:
            continue
        body = code[a:b + 1]
        head = code[max(0, a - 160):a]
        if re.search(r'\benum\b[^;{}]*$', head) or FLOAT_RX.search(body):
            continue
        lits = id_lits(body)
        vals = {int(m.group(1), 16) if m.group(1) is not None else int(m.group(2)) for m in lits}
        if len(vals) < 4:        # distinct values: {255, 255, 255, 255} / a column of one buffer size is not a table of ids
            continue
        if sum(1 for x in vals if x % 16 == 0) >= 0.9 * len(vals) and not any(x >= 0x82000000 for x in vals):   # register / struct offsets, not ids
            continue
        if re.search(r'\)\s*(?:const\s*)?(?:noexcept\s*)?(?:->\s*[\w:<>&\* ]+)?\s*$', head) and not re.search(r'[=,(:]\s*$|\breturn\s*$|\w\s*$', head):
            continue
        cover.append(b)
        sym = _lastname(re.sub(r'\s*\{?\s*$', '', head.split(';')[-1].split('{')[-1].split('}')[-1]).rstrip(' =\t\n'))
        la, lb = line_of(a), line_of(b)
        shown = ', '.join(m.group(0) for m in lits[:8]) + (' ...' if len(lits) > 8 else '')
        out.append(hit('A', 'A1', la, lb, f'{len(lits)} id-like numbers: {shown}', 'a literal table of ids / keys / addresses', sym))
    return out


def rule_a2(v):
    """a switch with >= 3 numeric `case N:` labels (id-like numbers), clustered within 15 lines"""
    cases = []
    for i, l in enumerate(v.code, 1):
        for m in re.finditer(r'\bcase\s+(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\s*:', l):
            mm = NUM_RX.match(m.group(1))
            if mm and id_like(mm):
                cases.append((i, m.group(1)))
    out, cur = [], []
    for c in cases + [(10 ** 9, '')]:
        if cur and c[0] - cur[-1][0] > 15:
            if len(cur) >= 3:
                out.append(hit('A', 'A2', cur[0][0], cur[-1][0], f'{len(cur)} case labels: ' + ', '.join(x[1] for x in cur[:8]) + (' ...' if len(cur) > 8 else ''),
                               'a switch over literal game ids / keys'))
            cur = []
        cur.append(c)
    return out


CMP_RX = re.compile(r'(?<![<>=!\-+*/&|^%])(==|!=|>=|<=|<|>)(?![<>=])\s*(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*(?![\w.])|(?<![\w.])(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\s*(==|!=|>=|<=|<|>)(?![<>=])')


def _cmp_lits(text):
    n, vals = 0, []
    for m in CMP_RX.finditer(text):
        tok = m.group(2) or m.group(3)
        mm = NUM_RX.match(tok)
        if mm and id_like(mm):
            n += 1
            vals.append(tok)
    return n, vals


def rule_a3(v):
    """one statement comparing against >= 3 id-like numbers: a hard-coded id / key set or range"""
    out = []
    for a, b, t in v.stmts():
        if 'static_assert' in t or t.startswith('case ') and t.count('case') < 3:
            continue
        n, vals = _cmp_lits(t)
        if n >= 3:
            out.append(hit('A', 'A3', a, b, v.rawcode(a, b)[:220], f'{n} comparisons with id-like numbers ({", ".join(vals[:6])})'))
            continue
        if t.endswith('{') or t.startswith(('case ', '#')) or FLOAT_RX.search(t) or '?' not in t or '{' in t:
            continue
        ids = {m.group(0).rstrip('uUlL') for m in id_lits(t)}
        if len(ids) >= 5:       # a ternary ladder / an argument list spelling out five or more ids (BeltTwin's 349 / 399 / 451 ... runs)
            out.append(hit('A', 'A4', a, b, v.rawcode(a, b)[:220], f'{len(ids)} distinct id-like numbers in one statement'))
    return out


# ------------------------------------------------------------------------------------------------------------------ B

COUNTWORD = r'(?:\b(?:have|got|seen|ncalls|nrows|ninst|ninstances|n_calls|n_rows|n_inst|n_instances|num_calls|num_rows|n_draws|ndraws|n_pieces)\b(?:\s*\[[^\]]*\])?|\b\w+_(?:calls|rows|instances|draws|pieces|records|models|batches)\b|\.\s*n_calls\b|\.\s*n_rows\b|\b(?:calls|rows|draws|instances|pieces|records|models|nodes|batches|meshes)_?\s*\.\s*size\s*\(\s*\))'
NEEDNAME = r'(?:(?:\w+::)*\b(?:Need|Needs|Needed|NeedCount|Want|Wants|Expect|Expects|Expected|ExpectedCount|FullCount|Quota)\w*\s*\(|\bk(?:Need|Want|Expect|FullCount|Complete|Required)\w*)'
B1_RX = re.compile(r'(' + COUNTWORD + r')\s*(==|!=|>=|<=|<|>)\s*(\d+)[uUlL]*(?![\w.])|(?<![\w.])(\d+)[uUlL]*\s*(==|!=|>=|<=|<|>)\s*(' + COUNTWORD + r')')
B2_RX = re.compile(r'(' + COUNTWORD + r')\s*(==|!=|>=|<=|<|>)\s*(' + NEEDNAME + r')|(' + NEEDNAME + r')\s*\)?\s*(==|!=|>=|<=|<|>)\s*(' + COUNTWORD + r')')
B3_NAME = re.compile(r'(?:^|::)(?:(?:Need|Needed|Expect|Expected|Want|Wanted|Required)(?:Count|Calls|Rows|Draws|Pieces|Instances)?|\w*(?:Need|Expected|Required)(?:Count|Calls|Rows|Draws|Pieces))$')


def after_for(t):
    """a `for (...) body` statement -> its body (a loop bound is not a rule); any other statement unchanged"""
    m = re.match(r'for\s*\(', t)
    if not m:
        return t
    depth, i = 1, m.end()
    while i < len(t) and depth:
        depth += (t[i] == '(') - (t[i] == ')')
        i += 1
    return t[i:].strip()


def rule_b(v):
    out = []
    for a, b, t in v.stmts():
        t = after_for(t)
        if 'static_assert' in t:
            continue
        m2 = B2_RX.search(t)
        if m2:
            out.append(hit('B', 'B2', a, b, v.rawcode(a, b)[:220], 'a count compared with a Need-style name / call'))
            continue
        m1 = B1_RX.search(t)
        if m1:
            lit = int(m1.group(3) or m1.group(4))
            if 2 <= lit <= 99:
                out.append(hit('B', 'B1', a, b, v.rawcode(a, b)[:220], f'a count compared with the literal {lit}'))
    for sp in v.src.spans:
        if sp['kind'] == 'func' and B3_NAME.search(sp['qual'] or sp['name'] or ''):
            out.append(hit('B', 'B3', sp['start'], sp['open'], v.c(sp['start']).strip() or sp['name'], 'a Need-style function: a count the game decides', sym=sp['name']))
    return out


# ------------------------------------------------------------------------------------------------------------------ C

_CALLER = r'(?:[\w:]+(?:\.|->))*(?:lr|ra|ret_addr|retaddr|return_addr|return_address|caller|caller_lr|callsite|call_site|pass_site|guest_lr)'
_CMP = r'(?:==|!=|<=|>=|(?<![<>-])<(?![<>])|(?<![<>-])>(?![<>=]))'
CALLER_WORD = re.compile(r'(?<![\w.>])' + _CALLER + r'(?![\w(])\)?\s*' + _CMP + r'|(?<=[\s)\]])' + _CMP + r'\s*(?:uint32_t\s*\(\s*)?(?<![\w.>])' + _CALLER + r'(?![\w(])')


def addr_consts(views):
    """names of constants (k... / UPPER_CASE style) whose value is a 0x82.. / 0x83.. address in every definition found, over all files: {name}"""
    addr, other = set(), set()
    rx = re.compile(r'\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\s*[;,]|#\s*define\s+([A-Za-z_]\w*)\s+(0[xX][0-9A-Fa-f]+|\d+)')
    for v in views:
        for l in v.code:
            for m in rx.finditer(l):
                nm, val = (m.group(1), m.group(2)) if m.group(1) else (m.group(3), m.group(4))
                (addr if re.fullmatch(r'0[xX]8[23][0-9A-Fa-f]{6}', val) else other).add(nm)
    return {n for n in addr - other if len(n) > 3 and re.match(r'k[A-Z]|[A-Z][A-Z0-9_]{3,}$', n)}


def rule_c(v, consts):
    out = []
    cmp_ = _CMP
    const_alt = '|'.join(sorted((re.escape(n) for n in consts), key=len, reverse=True)) or '(?!x)x'
    addr = r'(?:0[xX]8[23][0-9A-Fa-f]{6}[uUlL]*|\b(?:' + const_alt + r')\b)'
    c2 = re.compile(cmp_ + r'\s*' + addr + r'(?!\s*>)|(?<![<(,])\s' + addr + r'\s*' + cmp_ + r'|\bcase\s+' + addr + r'\s*:')
    for a, b, t in v.stmts():
        t = after_for(t)
        if t.startswith('#') or re.match(r'(?:constexpr|static\s+constexpr|inline\s+constexpr|const)\b', t) and 'if' not in t:
            continue
        w = CALLER_WORD.search(t)
        if w:
            out.append(hit('C', 'C1', a, b, v.rawcode(a, b)[:220], f'a caller / call-site / return-address compare `{w.group(0).strip()}`'))
            continue
        m = c2.search(t)
        if m:
            out.append(hit('C', 'C2', a, b, v.rawcode(a, b)[:220], 'a comparison against a game address / an address constant'))
    return out


# ------------------------------------------------------------------------------------------------------------------ D

IF_RET = re.compile(r'^\s*(?:\}\s*)?(?:else\s+)?if\s*\((.*)\)\s*return\s+([A-Za-z_]\w*(?:\.\w+|->\w+)?)\s*;')
KEEPWORDS = {'prefer', 'preferred', 'family', 'picked', 'chosen', 'winner'}
DECL_RX = re.compile(r'(?:^\s*|[;{}(,]\s*|\)\s*)(?:static\s+|const\s+|constexpr\s+|inline\s+|mutable\s+)*(?:bool|int|unsigned|int8_t|int16_t|int32_t|int64_t|uint8_t|uint16_t|uint32_t|uint64_t|size_t|float|auto)\s*[&*]?\s+([A-Za-z_]\w*)\s*(?:=|;|\[|\{|,)')
NAME_FUNC_RX = re.compile(r'^(?:Pick|Choose|Prefer|KeepOne|FirstMatch|PickOne|OnlyOne)[A-Z]?\w*$|Preferred|FirstMatch|PickOne|KeepOne|OnlyOne')
NAME_TYPE_RX = re.compile(r'Latch$|Family|Fallback|Preferred|FirstMatch|PickOne|KeepOne|OnlyOne')
D4_RX = re.compile(r'\b(?:wins?\b|first[- ]match|first wins|last wins|keeps? only|only one of|one of the two|picks? one|fall(?:s|ing)? back to (?:the|a|an|one|our|its)\b)', re.I)


def _toks(name):
    s = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', name).lower()
    return [t for t in re.split(r'_+', s) if t]


def rule_d(v):
    out = []
    # D1: first-match chains
    chain = []
    for i, l in enumerate(v.code, 1):
        m = IF_RET.match(l)
        if m and m.group(2) not in ('true', 'false', 'nullptr', 'NULL', 'void') and re.search(r'(?<![\w])' + re.escape(m.group(2).split('.')[0].split('-')[0]) + r'(?![\w])', m.group(1)):
            if chain and (i - chain[-1][0] > 6 or v.func_name(i) != chain[-1][2]):
                if len(chain) >= 2:
                    out.append(hit('D', 'D1', chain[0][0], chain[-1][0], ' | '.join(v.c(x[0]).strip() for x in chain)[:220], f'a first-match chain of {len(chain)} early returns'))
                chain = []
            chain.append((i, m.group(2), v.func_name(i)))
    if len(chain) >= 2:
        out.append(hit('D', 'D1', chain[0][0], chain[-1][0], ' | '.join(v.c(x[0]).strip() for x in chain)[:220], f'a first-match chain of {len(chain)} early returns'))
    # D2: x ? x : y  /  x >= 0 ? x : y
    for a, b, t in v.stmts():
        m = re.search(r'(?<![\w.])([A-Za-z_]\w*)\s*(?:>=\s*0\s*|!=\s*-1\s*|>\s*-1\s*)\?\s*\1\s*:', t)
        if m:
            out.append(hit('D', 'D2', a, b, v.rawcode(a, b)[:220], 'a ternary that keeps the first answer else replaces it'))
    # D3: names
    for i, l in enumerate(v.code, 1):
        if 'return' in l[:12] or l.lstrip().startswith(('if', 'for', 'while', 'case', '//')):
            continue
        for m in DECL_RX.finditer(l):
            tk = _toks(m.group(1))
            ws = [t for t in tk if t in KEEPWORDS]
            if ws and 'queue' not in tk and 'k' not in tk[:1]:
                out.append(hit('D', 'D3', i, i, l.strip()[:220], f'a declaration named for a keep / pick / latch rule ({"/".join(ws)})', sym=m.group(1)))
    for sp in v.src.spans:
        nm = (sp['name'] or '').split('::')[-1].split('(')[0]
        if v.rel.startswith(('src/assets/view/', 'src/renderer/')):
            continue      # the viewer's UI and the Vulkan device: pick-one there is not a game choice
        if sp['kind'] in ('func', 'type') and nm and (NAME_FUNC_RX if sp['kind'] == 'func' else NAME_TYPE_RX).search(nm):
            out.append(hit('D', 'D3', sp['start'], sp['open'], v.c(sp['start']).strip() or nm, f'a {"function" if sp["kind"] == "func" else "type"} named for a keep / pick / latch / fallback rule', sym=nm))
    # D4: comments
    for i in range(1, v.n() + 1):
        c = v.cmt[i]
        if c and D4_RX.search(c):
            m = D4_RX.search(c)
            out.append(hit('D', 'D4', i, i, c.strip()[:220], f'a comment says `{m.group(0).strip()}`'))
    return out


# ------------------------------------------------------------------------------------------------------------------ E

E_TAG = re.compile(r'\[(U|L)\b')
E_WORD = re.compile(r'\b(until|assum(?:e|es|ed|ing|ption|ptions)|fitted|guess(?:ed|es)?|never|always)\b', re.I)
E_COND = re.compile(r'\b(?:if|else|while|switch|case|return)\b|\?|==|!=|&&|\|\|')


def rule_e(v):
    """comment blocks (consecutive lines with comment text) holding a mark or an assumption word; one row per block and sub-kind"""
    out = []
    n = v.n()
    i = 1
    while i <= n:
        if not v.cmt[i].strip():
            i += 1
            continue
        j = i
        while j + 1 <= n and v.cmt[j + 1].strip():
            j += 1
        # the block i..j; code on its own lines (a comment-only block) or beside it
        text = re.sub(r'\s+', ' ', re.sub(r'/\*+|\*+/|//+', ' ', ' '.join(v.cmt[k].strip() for k in range(i, j + 1)))).strip()
        tags = sorted({m.group(0) for m in E_TAG.finditer(text)})
        words = sorted({m.group(1).lower() for m in E_WORD.finditer(text)})
        if tags or words:
            adj_code = ' '.join(v.c(k).strip() for k in range(i, j + 3) if v.c(k).strip())
            first = next((k for k in range(i, j + 1) if E_TAG.search(v.cmt[k]) or E_WORD.search(v.cmt[k])), i)
            if tags:
                out.append(hit('E', 'E1', first, j, _clip(text, E_TAG), 'a [U] / [L] mark: ' + ' '.join(tags)))
            strong = [w for w in words if w not in ('never', 'always')]
            if strong:
                out.append(hit('E', 'E2', first, j, _clip(text, E_WORD, strong), 'an assumption word: ' + ', '.join(strong)))
            weak = [w for w in words if w in ('never', 'always')]
            if weak and j - i < 3 and E_COND.search(adj_code):
                out.append(hit('E', 'E3', first, j, _clip(text, E_WORD, weak), 'next to a condition: ' + ', '.join(weak)))
        i = j + 1
    return out


def _clip(text, rx, only=None):
    """~200 chars of the comment around the first match"""
    for m in rx.finditer(text):
        if only is None or (m.lastindex and m.group(1).lower() in only) or only is None:
            a = max(0, m.start() - 90)
            return ('...' if a else '') + text[a:m.end() + 110]
    return text[:200]


# ------------------------------------------------------------------------------------------------------------------ F

SWITCH_READ = re.compile(r'REXCVAR_GET\s*\(\s*(\w+)\s*\)|(?:std::)?getenv\s*\(\s*"([^"]*)"')
SWITCH_NAME = re.compile(r'rdr|scene|render', re.I)
F_COND = re.compile(r'(?:\bif|\bwhile)\s*\(|\?|&&|\|\|')


def rule_f(v):
    out = []
    for a, b, t in v.stmts():
        if not SWITCH_READ.search(t):
            continue
        raw = ' '.join(v.r(k).strip() for k in range(a, b + 1))
        names = []
        for m in SWITCH_READ.finditer(raw):
            names.append(m.group(1) or m.group(2))
        nm = ', '.join(dict.fromkeys(x for x in names if x))
        if not SWITCH_NAME.search(nm):
            continue          # an application / probe / log switch, not a choice between two ways of drawing
        if re.search(r'getenv', t) and re.search(r'^\s*(?:static\s+)?(?:const\s+|constexpr\s+)*(?:bool|int|auto|float|double|std::string)\s+\w+\s*=', t):
            out.append(hit('F', 'F2', a, b, v.rawcode(a, b)[:220], f'a switch variable read from the environment ({nm})', sym=nm))
        elif F_COND.search(t) and not re.match(r'(?:inline|static|constexpr)?\s*(?:bool|int|int64_t|auto)\s+\w+\s*\(\s*\)\s*\{', t):
            out.append(hit('F', 'F1', a, b, v.rawcode(a, b)[:220], f'a switch read inside a condition ({nm})', sym=nm))
    return out


ALL_KINDS = 'ABCDEFG'


def scan(v, consts, kinds):
    """every hit of the wanted kinds in one View"""
    out = []
    if 'A' in kinds:
        out += rule_a1(v) + rule_a2(v) + rule_a3(v)
    if 'B' in kinds:
        out += rule_b(v)
    if 'C' in kinds:
        out += rule_c(v, consts)
    if 'D' in kinds:
        out += rule_d(v)
    if 'E' in kinds:
        out += rule_e(v)
    if 'F' in kinds:
        out += rule_f(v)
    return out
