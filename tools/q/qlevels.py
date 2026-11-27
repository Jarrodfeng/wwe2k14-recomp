r"""q codediff --field levels: per texture list the GAME builds, the loader's level count in force when it is built (the word
0x83629CA8), against the count OUR loader gives the same family's texture directory (S111 LEVELSDIFF; the levels extractor of the code
diff, docs/research/s111/WHOLEDIFF_PLAN.md A.5.1).

    py tools/q/q.py codediff --field levels [--fn NAME | --game sub_X] [--flag V ...] [--ref REF] [--json] [--facts]

GAME (glevels). The word has one writer, sub_82BD42E0 (`q field 0x83629CA8 --writes --values`), and one getter, sub_82BD42F0. A FAMILY
is a caller of the writer that appends a texture list (a direct call of APPENDS: the list append sub_82BA9D68 and the surface list
sub_82BAA568, kind in r6; the element build sub_82BA9EF8, kind in r9; kind 0 2D, 1 cube, 2 volume: CUBELEVELS chain step 4), plus every
list-appending function our code cites (its count is the caller's). Each function is read from its decomp (qdecomp: the call events in
address order, each with its block path = the decomp's open braces, and its argument registers), as events: a SET (r3: a constant,
`reg ROOT`, or the getter's value = a RESTORE of what was in force at that read), a SAVE (the getter), a FIND (sub_82C351E8, r4 = the
PACH key), a PER-NAME lookup (sub_827FDB78: the word from a 32-byte row's byte 31, a MipmapData table), an APPEND. The count in force at
an append: a set replaces the earlier values of the blocks it encloses; the last one whose block encloses the append holds, and every
later one (in a block the append is not in: a path it may not take) adds [L tool: the block stands in for the path condition]; no
set = INHERITED, taken one level up the call graph where the callers are read families (else it stays inherited). An append's KEYS are
the constant keys found since the previous append (none since: the previous append's keys).
OURS (olevels). Every TexSet call of src: AddDir (the file's chain, or per name when its third argument is given), AddDirLevels (the
count argument: a literal, a constexpr name resolved through its definition, a rule function whose one return is read arm by arm, a
parameter; a lambda's parameter is read at each call of the lambda), AddDirNamed (per name). n >= 15 = the whole chain (a 16384 side has
15 levels; game_levels.h GameLevels clamps), 0 = the file's chain. Its KEYS: `== K`, `id >= A && id <= B`, `pac::Find / Get(.., K)` on
its line and the line above, a lambda call's other arguments, and the keys >= 100 that `pac::Find / Get` names in its function. Its
CITES: the game functions named on its line, the line above, the definitions of the names its count reads (with their comment block),
its function's leading comment block and its file's header block. The UPLOAD RULE: game_levels.h Relevel's first refusal that tests
`faces`, evaluated for a 2D texture (faces 1), a cube (6 square faces) and a volume (ours keeps no depth: faces 1) [C tool]; a list
kind the rule refuses uploads the file's chain whatever its count (CUBELEVELS). No Relevel in the tree: no count is ever uploaded.
JOIN, by load family: a directory of ours pairs with a game list when it cites the family function, or when they share a key >= 100
(a key below 100 is an entry id every model PACH has: it only narrows); a pair whose two sides both have keys and share none is
dropped. One row per game list: the best of its pairs (OK, UNSETTLED, DIFF, GAME-ONLY; the others listed beside it). An unpaired list
whose count is set is GAME-ONLY; a counted directory of ours that pairs nothing is OURS-ONLY (low).
VERDICTS (field `levels`): DIFF (both count, the values differ; the game per name, ours flat), GAME-ONLY (the game counts, ours leaves
the file's chain: no count, a 0 arm, a kind the upload refuses, no pair), UNSETTLED (an inherited count; a non-constant game count
against a constant or an uncited rule of ours; several game values of which ours gives one), OURS-ONLY, OK (the same constant, the
whole chain both sides, per name both sides, a rule of ours that cites the builder of a non-constant game count).
"""
import argparse
import collections
import fnmatch
import functools
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qcodediff as C   # noqa: E402  (the verdict order, CodediffError)
import qostate as O     # noqa: E402  (the reader at a ref, TextSrc, the masker)

REPO = O.REPO
FIELD = 'levels'
SETTER, GETTER = 0x82BD42E0, 0x82BD42F0                     # the word's one writer / its getter (q field 0x83629CA8 --writes)
FINDS = {0x82C351E8: 'r4'}                                  # pach_table_find_index_by_key16: r4 = the key
APPENDS = {0x82BA9D68: 'r6', 0x82BAA568: 'r6', 0x82BA9EF8: 'r9'}   # list append / surface list / element build: the kind register
PER_NAME = {0x827FDB78}                                     # the MipmapData row lookup (ARENALEVELS step 2)
KIND_NAME = {0: '2D', 1: 'cube', 2: 'volume', None: 'kind ?'}
KIND_FACES = {0: 1, 1: 6, 2: 1}                             # the faces our Texture holds for a list kind (no depth: a volume reads as 2D)
WHOLE_FROM = 15                                             # an ours count >= this is the whole chain (16384 = 15 levels)
FILE, WHOLE, TABLE, INHERIT = 'file', 'whole', 'per-name', 'inherited'
BEST = ('OK', 'UNSETTLED', 'DIFF', 'GAME-ONLY')             # the best pair of a list first
NUM_RE = re.compile(r'-?(?:0x[0-9A-Fa-f]+|\d+)')
SUB_RE = re.compile(r'\bsub_([0-9A-Fa-f]{8})\b')
TEXSET_RE = re.compile(r'(?:\.|->)\s*(AddDirLevels|AddDirNamed|AddDir)\s*\(')
TEXSET_FILES = r'(\.|->)[[:space:]]*AddDir(Levels|Named)?[[:space:]]*\('
LINE_RE = re.compile(r'^(\s*)\+([0-9A-F]+)\s+call#[0-9A-F]+\s+sub_([0-9A-F]{8})\b')
# selftest only: a planted twin its selftest must see fail ('no_cube' the upload rule's kind refusal ignored, 'no_keys' keys never
# narrow a pair, 'no_inherit' an inherited count read as set, 'no_unpaired' an unpaired list not reported, 'no_paths' every set adds,
# 'no_table' the per-name lookup not read)
PLANT = os.environ.get('WWE_QLEVELS_PLANT', '')


class LevelsError(C.CodediffError):
    pass


def hx(a):
    return f'sub_{a:08X}'


def vtext(v):
    if isinstance(v, tuple):
        if v[0] == 'reg':
            return f'reg {v[1]}'
        if v[0] == 'rule':
            return f"rule {v[1]} -> {', '.join(vtext(x) for x in v[2])}"
        return ' '.join(str(x) for x in v)
    return str(v)


# ------------------------------------------------------------------ the game
def events_of(d, fn):
    """a decomp dict -> [{off, depth, kind, tgt, regs}] for the calls that matter, in address order"""
    sites = d['summary']['sites'] if isinstance(d, dict) else {}
    out = []
    path, nxt = [], 0
    for line in (d['lines'] if isinstance(d, dict) else ()):
        s = re.sub(r'\s*//.*$', '', line).strip()
        m = LINE_RE.match(line)
        if not m:
            if s.startswith('}') and path:
                path.pop()
            if s.endswith('{') and not s.startswith(';'):
                nxt += 1
                path.append(nxt)
            continue
        depth, off, tgt = tuple(path), int(m.group(2), 16), int(m.group(3), 16)
        regs = next((s.get('regs') or {} for s in sites.get(f'{tgt:08X}', []) if s['site'] == fn + off), {})
        if tgt == SETTER:
            kind = 'set'
        elif tgt == GETTER:
            kind = 'get'
        elif tgt in FINDS:
            kind = 'find'
        elif tgt in APPENDS:
            kind = 'append'
        elif tgt in PER_NAME and PLANT != 'no_table':
            kind = 'pername'
        else:
            kind = 'call'
        out.append({'off': off, 'path': depth, 'kind': kind, 'tgt': tgt, 'regs': regs, 'line': line.strip()})
    out.sort(key=lambda e: e['off'])
    return out


def set_value(e, local_ret):
    """a SET event's value: an int, ('ret', getter off), ('reg', text)"""
    v = (e['regs'].get('r3') or '').strip()
    if NUM_RE.fullmatch(v):
        return int(v, 0)
    root = local_ret(v) if v.startswith('L') else None
    m = re.fullmatch(r'ret#([0-9A-F]+)', root or v)
    if m:
        return ('ret', int(m.group(1), 16))
    return ('reg', v or '?')


def _norm_game(v):
    return WHOLE if v == 0 else v


def _encloses(p, q):
    """block path p is q or one of q's enclosing blocks"""
    return p is not None and q is not None and tuple(q[:len(p)]) == tuple(p)


def in_force(evs, upto, path, memo=None):
    """the set of values the word may hold at offset `upto` for a consumer in block `path` (a tuple of the decomp's open blocks).
    A set replaces the earlier entries of the blocks it encloses; at the consumer the last entry whose block encloses it holds, and
    every later entry (a set in a block the consumer is not in: a path it may not take) adds its values [L tool]"""
    memo = {} if memo is None else memo
    key = (upto, tuple(path))
    if key in memo:
        return memo[key]
    memo[key] = {INHERIT}          # a cycle (a restore of a read after it) reads as inherited
    gets = {e['off']: e for e in evs if e['kind'] == 'get'}
    stack = []                     # [(block path, {values})] in address order
    for e in evs:
        if e['off'] >= upto:
            break
        if e['kind'] == 'set':
            v = e['val']
            if isinstance(v, tuple) and v[0] == 'ret' and v[1] in gets:
                vals = set(in_force(evs, v[1], gets[v[1]]['path'], memo))
            elif isinstance(v, tuple) and v[0] == 'ret':
                vals = {('reg', f'ret#{v[1]:X}')}
            else:
                vals = {_norm_game(v)}
            p = tuple(e['path'])
        elif e['kind'] == 'pername' and PLANT != 'no_table':
            vals, p = {TABLE}, tuple(e['path']) + ('lookup',)     # the lookup sets the word only for a name its table holds
        else:
            continue
        if PLANT == 'no_paths':
            stack.append((None, vals))
            continue
        stack = [(sp, sv) for sp, sv in stack if not _encloses(p, sp)] + [(p, vals)]
    last = max((i for i, (sp, _sv) in enumerate(stack) if _encloses(sp, path)), default=-1)
    out = set(stack[last][1]) if last >= 0 else {INHERIT}
    for _sp, sv in stack[last + 1:]:
        out |= sv
    memo[key] = out
    return out


def lists_of(fn, evs):
    """the APPEND events of one function -> [{fn, at, off, callee, kind, keys, vals, depth, set_here}]"""
    for e in evs:
        if e['kind'] == 'set':
            e['val'] = e.get('val', ('reg', '?'))
    out, keys_since, prev_keys = [], set(), set()
    memo = {}
    any_set = any(e['kind'] == 'set' for e in evs)
    for e in evs:
        if e['kind'] == 'find':
            k = (e['regs'].get(FINDS[e['tgt']]) or '').strip()
            if NUM_RE.fullmatch(k):
                keys_since.add(int(k, 0))
        elif e['kind'] == 'append':
            kr = (e['regs'].get(APPENDS[e['tgt']]) or '').strip()
            kind = int(kr, 0) if NUM_RE.fullmatch(kr) else None
            keys = keys_since or prev_keys
            vals = in_force(evs, e['off'], e['path'], memo)
            out.append({'fn': hx(fn), 'at': f'0x{fn + e["off"]:08X}', 'off': e['off'], 'callee': hx(e['tgt']), 'kind': kind,
                        'keys': sorted(keys), 'vals': vals, 'path': e['path'], 'set_here': any_set})
            prev_keys, keys_since = set(keys), set()
    return out


def resolve_inherited(lists, evs_by, callers_of):
    """an INHERITED count -> the counts in force at the callers' call sites (one level; a caller not read keeps it inherited)"""
    for li in lists:
        if INHERIT not in li['vals'] or PLANT == 'no_inherit':
            continue
        fn = int(li['fn'][4:], 16)
        cs = callers_of(fn)
        got, via = set(), []
        for c in cs:
            evs = evs_by.get(c)
            sites = [e for e in (evs or ()) if e['tgt'] == fn]
            if not sites:
                got.add(INHERIT)
                continue
            for s in sites:
                v = in_force(evs, s['off'], s['path'])
                got |= v
                via.append(f'{hx(c)} +0x{s["off"]:X}: {", ".join(sorted(vtext(x) for x in v))}')
        if not cs:
            got.add(INHERIT)
        li['vals'] = (li['vals'] - {INHERIT}) | got
        if via:
            li['inherited_from'] = via
    if PLANT == 'no_inherit':
        for li in lists:
            if li['vals'] == {INHERIT}:
                li['vals'] = {3}


class GameDb:
    """the q db: call edges and decomps"""
    def __init__(self, con=None):
        import q as Q
        self.con = con or Q.connect(Q.DB)
        self.cache = {}

    def callers(self, fn):
        return sorted({s for (s,) in self.con.execute("SELECT DISTINCT src FROM edges WHERE dst = ? AND kind IN ('call', 'tail')", (fn,))})

    def callees(self, fn):
        return {d for (d,) in self.con.execute("SELECT DISTINCT dst FROM edges WHERE src = ? AND kind IN ('call', 'tail')", (fn,))}

    def events(self, fn):
        if fn not in self.cache:
            import qdecomp
            import qgstate
            try:
                d = qdecomp.decompile(self.con, fn)
            except qdecomp.DecompError as e:
                raise LevelsError(f'{hx(fn)}: decomp refused: {e}')
            evs = events_of(d, fn)
            for e in evs:
                if e['kind'] == 'set':
                    e['val'] = set_value(e, lambda root, d=d: qgstate._local_ret(d, root))
            self.cache[fn] = evs
        return self.cache[fn]


def game_lists(db, cited=()):
    """-> (lists, families {fn: why}, problems): every list of the families (module doc), counts resolved one level up"""
    problems = []
    fams = {}
    for f in db.callers(SETTER):
        if db.callees(f) & set(APPENDS):
            fams[f] = 'sets the count word and appends a list'
    for f in sorted(set(cited) - set(fams) - set(APPENDS) - {SETTER, GETTER} - set(FINDS) - PER_NAME):
        if db.callees(f) & set(APPENDS):
            fams[f] = 'appends a list; cited by our code (the count is its caller\'s)'
    evs_by = {}
    for f in set(fams) | set(db.callers(SETTER)):
        try:
            evs_by[f] = db.events(f)
        except LevelsError as e:
            problems.append(str(e))
    lists = []
    for f in sorted(fams):
        if f in evs_by:
            lists += lists_of(f, evs_by[f])
    resolve_inherited(lists, evs_by, db.callers)
    return lists, fams, problems


# ------------------------------------------------------------------ ours
class Tree:
    """our src at a ref (None = the working tree): read a file, grep the tree (git grep, never a checkout)"""
    def __init__(self, ref=None):
        self.ref = ref
        self.read = O.default_reader(ref)
        self._texts = {}

    def text(self, rel):
        if rel not in self._texts:
            self._texts[rel] = self.read(rel)
        return self._texts[rel]

    def grep(self, pattern, paths=('src',)):
        """-> [(rel, line no)] of lines matching the extended regex"""
        args = ['git', 'grep', '-n'] + ([] if self.ref else ['--untracked']) + ['-E', pattern] + ([self.ref] if self.ref else []) + ['--'] + list(paths)
        r = subprocess.run(args, cwd=REPO, capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode not in (0, 1):
            raise LevelsError(f'git grep: {r.stderr.strip()[:200]}')
        out = []
        for ln in r.stdout.splitlines():
            if self.ref:
                ln = ln.split(':', 1)[1] if ':' in ln else ln
            parts = ln.split(':', 2)
            if len(parts) >= 2 and parts[1].isdigit():
                out.append((parts[0], int(parts[1])))
        return out


class DictTree(Tree):
    """the selftest's tree: {rel: text}"""
    def __init__(self, files):
        self.ref = None
        self.files = files
        self._texts = dict(files)
        self.read = self._read

    def _read(self, rel):
        if rel not in self.files:
            raise FileNotFoundError(rel)
        return self.files[rel]

    def grep(self, pattern, paths=('src',)):
        rx = re.compile(pattern.replace('[[:space:]]', r'\s'))
        return [(rel, i + 1) for rel, t in sorted(self.files.items()) for i, ln in enumerate(t.split('\n')) if rx.search(ln)]


def _args(t, i):
    """the top-level arguments of the call whose '(' is at t[i] -> ([(text, start)], end after ')')"""
    depth, start, out = 0, i + 1, []
    j = i
    while j < len(t):
        c = t[j]
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
            if depth == 0:
                out.append((t[start:j], start))
                return out, j + 1
        elif c == ',' and depth == 1:
            out.append((t[start:j], start))
            start = j + 1
        j += 1
    return out, len(t)


def _ternary(e):
    """top-level `c ? a : b` -> (a, b) or None"""
    depth, q = 0, -1
    for i, c in enumerate(e):
        if c in '([{':
            depth += 1
        elif c in ')]}':
            depth -= 1
        elif c == '?' and depth == 0 and q < 0:
            q = i
        elif c == ':' and depth == 0 and q >= 0 and e[i - 1:i + 1] != '::' and e[i:i + 2] != '::':
            return e[q + 1:i].strip(), e[i + 1:].strip()
    return None


def _strip(e):
    e = e.strip()
    while e.startswith('(') and e.endswith(')') and _args(e, 0)[1] == len(e):
        e = e[1:-1].strip()
    m = re.fullmatch(r'(?:uint32_t|int|uint8_t|unsigned)\s*\((.*)\)', e, re.S)
    return m.group(1).strip() if m else e


def _lines(text):
    return text.split('\n')


def comment_block(lines, ln):
    """the `//` lines right above line ln (1-based) and the line itself"""
    out = [lines[ln - 1]] if 0 < ln <= len(lines) else []
    i = ln - 2
    while i >= 0 and lines[i].lstrip().startswith('//'):
        out.append(lines[i])
        i -= 1
    return out


def header_block(lines):
    out = []
    for ln in lines:
        if ln.lstrip().startswith('//'):
            out.append(ln)
        elif ln.strip() == '' and not out:
            continue
        else:
            break
    return out


class Resolver:
    """names of our code -> their definitions (constexpr values, one-return rule functions), by git grep over src"""
    def __init__(self, tree):
        self.tree = tree

    @functools.lru_cache(maxsize=None)
    def const(self, name, near):
        last = name.split('::')[-1]
        hits = self.tree.grep(r'constexpr[^;=(]*\b' + re.escape(last) + r'[[:space:]]*=')
        if not hits:
            return None
        rel, ln = next(((r, n) for r, n in hits if r == near), hits[0])
        line = _lines(self.tree.text(rel))[ln - 1]
        m = re.search(r'\b' + re.escape(last) + r'\s*=\s*([^;]+);', line)
        return (m.group(1).strip(), rel, ln) if m else None

    @functools.lru_cache(maxsize=None)
    def rule(self, name, near):
        last = name.split('::')[-1]
        hits = self.tree.grep(r'^[[:space:]]*(inline|constexpr|static)[^;=]*[[:space:]]' + re.escape(last) + r'[[:space:]]*\(')
        if not hits:
            return None
        rel, ln = next(((r, n) for r, n in hits if r == near), hits[0])
        ls = _lines(self.tree.text(rel))
        head = ls[ln - 1]
        pm = re.search(r'\b' + re.escape(last) + r'\s*\(([^)]*)\)', head)
        params = [re.sub(r'=.*$', '', p).strip().split()[-1].lstrip('&*') for p in (pm.group(1).split(',') if pm and pm.group(1).strip() else []) if p.strip()]
        body = ' '.join(ls[ln - 1:ln + 5])
        m = re.search(r'\breturn\s+([^;]+);', body)
        return (m.group(1).strip() if m else None, params, rel, ln)


def resolve(expr, res, near, params=(), seen=None, cites=None):
    """our count expression -> FILE | WHOLE | int | ('per-name', e) | ('rule', name, [leaves]) | ('param', name) | ('expr', e)"""
    seen = set() if seen is None else seen
    cites = [] if cites is None else cites
    e = _strip(expr)
    if re.fullmatch(r'\d+[uU]?', e):
        n = int(e.rstrip('uU'))
        return FILE if n == 0 else WHOLE if n >= WHOLE_FROM else n
    if re.fullmatch(r'[A-Za-z_][\w:]*', e):
        if e in params:
            return ('param', e)
        if e in seen:
            return ('expr', e)
        seen.add(e)
        d = res.const(e, near)
        if d:
            val, rel, ln = d
            cites.append((rel, ln))
            return resolve(val, res, rel, (), seen, cites)
        return ('param', e)
    t = _ternary(e)
    if t:
        a = resolve(t[0], res, near, params, seen, cites)
        b = resolve(t[1], res, near, params, seen, cites)
        return ('rule', '?:', _leaves(a) + _leaves(b))
    m = re.fullmatch(r'([A-Za-z_][\w:]*)\s*\((.*)\)', e, re.S)
    if m and m.group(1).split('::')[-1] not in seen:
        name = m.group(1)
        r = res.rule(name, near)
        if r and r[0]:
            body, ps, rel, ln = r
            cites.append((rel, ln))
            inner = resolve(body, res, rel, tuple(ps), seen | {name.split('::')[-1]}, cites)
            return ('rule', name.split('::')[-1], _leaves(inner))
        return ('rule', name.split('::')[-1], [('expr', e)])
    return ('expr', e)


def _leaves(v):
    return list(v[2]) if isinstance(v, tuple) and v[0] == 'rule' else [v]


def _int_of(tok, res, near, depth=0):
    """a key token (a literal or a constexpr name) -> its int, else None (a key, not a count: no whole-chain reading)"""
    e = _strip(tok)
    if re.fullmatch(r'\d+[uU]?', e):
        return int(e.rstrip('uU'))
    if depth < 4 and re.fullmatch(r'[A-Za-z_][\w:]*', e):
        d = res.const(e, near)
        if d:
            return _int_of(d[0], res, d[1], depth + 1)
    return None


KEY_EQ = re.compile(r'==\s*([A-Za-z_][\w:]*|\d+)[uU]?\b')
KEY_RANGE = re.compile(r'([\w.>-]+)\s*>=\s*([A-Za-z_][\w:]*|\d+)[uU]?\s*&&\s*\1\s*<=\s*([A-Za-z_][\w:]*|\d+)[uU]?')
KEY_PAC = re.compile(r'pac::(?:Find|Get)\s*\(([^;]*)')


def _pac_keys(text, res, near):
    out = set()
    for m in KEY_PAC.finditer(text):
        a, _end = _args(m.group(0), m.group(0).index('('))
        if a:
            k = _int_of(a[-1][0], res, near)
            if k is not None:
                out.add(k)
    return out


def line_keys(text, res, near):
    out = set()
    for m in KEY_RANGE.finditer(text):
        lo, hi = _int_of(m.group(2), res, near), _int_of(m.group(3), res, near)
        if lo is not None and hi is not None and 0 <= hi - lo <= 100:
            out |= set(range(lo, hi + 1))
    for m in KEY_EQ.finditer(text):
        k = _int_of(m.group(1), res, near)
        if k is not None:
            out.add(k)
    return out | _pac_keys(text, res, near)


def _lambda_of(masked, span, pos):
    """the lambda whose body holds pos inside the function span -> (name, [param names], end of its definition) or None"""
    a, b = span
    best = None
    for m in re.finditer(r'\bauto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(', masked[a:b]):
        p0 = a + m.end() - 1
        ps, after = _args(masked, p0)
        bo = masked.find('{', after)
        if bo < 0 or bo > pos:
            continue
        _x, be = _args(masked, bo)
        if bo < pos < be:
            names = [re.sub(r'=.*$', '', p).strip().split()[-1].lstrip('&*') for p, _s in ps if p.strip()]
            best = (m.group(1), names, be)
    return best


def ours_facts(tree):
    """-> (facts, problems): one fact per TexSet call of our src (a lambda's count parameter: one per call of the lambda)"""
    res = Resolver(tree)
    files = sorted({rel for rel, _ln in tree.grep(TEXSET_FILES)})
    facts, problems = [], []
    for rel in files:
        if not rel.endswith(('.h', '.cpp', '.inc', '.inl', '.hpp')):
            continue
        try:
            text = tree.text(rel)
        except (OSError, O.OstateError) as e:
            problems.append(f'{rel}: {e}')
            continue
        masked = O.CD._mask(text)
        lines, mlines = _lines(text), _lines(masked)
        src = None
        for m in TEXSET_RE.finditer(masked):
            meth = m.group(1)
            ln = O.line_of(masked, m.start())
            args, _end = _args(masked, m.end() - 1)
            if src is None:
                src = O.TextSrc(rel, text)
            span = src.span(ln) or (0, len(masked))
            fn = src.name(ln) or '(file scope)'
            fstart = O.line_of(masked, span[0])
            near_cites = [lines[ln - 1], lines[ln - 2] if ln >= 2 else '']
            near_cites += comment_block(lines, fstart) + header_block(lines)
            keys = line_keys(mlines[ln - 1] + '\n' + (mlines[ln - 2] if ln >= 2 else ''), res, rel)
            keys |= {k for k in _pac_keys(masked[span[0]:span[1]], res, rel) if k >= 100}
            base = {'side': 'ours', 'field': FIELD, 'fn': fn, 'cond': None, 'method': meth}
            if meth == 'AddDir':
                per = len(args) >= 3 and args[2][0].strip() not in ('nullptr', 'NULL', '0')
                v = (TABLE, args[2][0].strip()) if per else FILE
                facts.append(dict(base, value=v, at=f'{rel}:{ln}', kind='per-name' if per else 'file', keys=sorted(keys),
                                  cites=_cites(near_cites, ())))
                continue
            if len(args) < 2:
                problems.append(f'{rel}:{ln}: {meth} with {len(args)} argument(s)')
                continue
            cexpr = args[1][0].strip()
            if meth == 'AddDirNamed':
                cs = []
                resolve(cexpr.split('.')[0], res, rel, cites=cs)
                facts.append(dict(base, value=(TABLE, cexpr), at=f'{rel}:{ln}', kind='per-name', keys=sorted(keys),
                                  cites=_cites(near_cites, _def_lines(tree, cs))))
                continue
            lam = _lambda_of(masked, span, m.start())
            if lam and _strip(cexpr) in lam[1]:
                name, params, lend = lam
                idx = params.index(_strip(cexpr))
                for cm in re.finditer(r'\b' + re.escape(name) + r'\s*\(', masked[lend:span[1]]):
                    cpos = lend + cm.end() - 1
                    cargs, _e = _args(masked, cpos)
                    if idx >= len(cargs):
                        continue
                    cln = O.line_of(masked, cpos)
                    cs = []
                    v = resolve(cargs[idx][0], res, rel, cites=cs)
                    ks = set(keys) | {k for j, (a, _s) in enumerate(cargs) if j != idx for k in [_int_of(a, res, rel)] if k is not None}
                    ks |= line_keys(mlines[cln - 1], res, rel)
                    facts.append(dict(base, value=v, at=f'{rel}:{cln}', kind=f'lambda {name}', keys=sorted(ks),
                                      cites=_cites(near_cites + [lines[cln - 1], lines[cln - 2] if cln >= 2 else ''], _def_lines(tree, cs)),
                                      texset=f'{rel}:{ln}'))
                continue
            cs = []
            v = resolve(cexpr, res, rel, cites=cs)
            facts.append(dict(base, value=v, at=f'{rel}:{ln}', kind='count', keys=sorted(keys), cites=_cites(near_cites, _def_lines(tree, cs))))
    return facts, problems


def _def_lines(tree, cs):
    out = []
    for rel, ln in cs:
        try:
            out += comment_block(_lines(tree.text(rel)), ln)
        except (OSError, O.OstateError):
            continue
    return out


def _cites(lines, extra):
    return sorted({'sub_' + m.group(1).upper() for ln in list(lines) + list(extra) for m in SUB_RE.finditer(ln)})


# ------------------------------------------------------------------ the upload rule
def upload_rule(tree, rel='src/renderer_scene/game_levels.h'):
    """-> {present, refused: {kind}, why}: game_levels.h Relevel's first refusal that tests `faces`, per list kind"""
    try:
        text = tree.text(rel)
    except (OSError, O.OstateError):
        return {'present': False, 'refused': set(KIND_FACES), 'why': f'no {rel} at this tree: no count of ours is uploaded'}
    masked = O.CD._mask(text)
    m = re.search(r'\binline\s+bool\s+Relevel\s*\(', masked)
    if not m:
        return {'present': False, 'refused': set(KIND_FACES), 'why': f'no Relevel in {rel}: no count of ours is uploaded'}
    bo = masked.find('{', m.end())
    _a, be = _args(masked, bo)
    body = masked[bo:be]
    locs = {n: e for n, e in re.findall(r'const\s+bool\s+(\w+)\s*=\s*([^;]+);', body)}
    cond = None
    for im in re.finditer(r'\bif\s*\(', body):
        a, end = _args(body, im.end() - 1)
        if 'faces' in a[0][0] and re.match(r'\s*return\s+false\s*;', body[end:]):
            cond = a[0][0]
            break
    if cond is None:
        return {'present': True, 'refused': set(), 'why': 'Relevel tests no faces: every kind is relevelled'}
    refused, why = set(), []
    for kind, faces in KIND_FACES.items():
        try:
            r = _eval_refusal(cond, locs, faces)
        except Exception as e:  # noqa: BLE001 - an expression this reader cannot evaluate is said, never guessed
            return {'present': True, 'refused': set(), 'unread': True, 'why': f'Relevel refusal `{cond.strip()}` not evaluated ({type(e).__name__})'}
        if r and PLANT != 'no_cube':
            refused.add(kind)
            why.append(f'{KIND_NAME[kind]} (faces {faces}) refused')
    return {'present': True, 'refused': refused, 'cond': ' '.join(cond.split()),
            'why': ('Relevel `' + ' '.join(cond.split()) + '`: ' + ('; '.join(why) if why else 'no kind refused'))}


def _eval_refusal(cond, locs, faces):
    vals = {'t.faces': str(faces), 't.width': '64', 't.height': '64', 't.game_levels': '3', 't.format': "'bc1'",
            'assets::TexFormat::kUnknown': "'unknown'"}

    def tr(e, depth=0):
        for k in sorted(vals, key=len, reverse=True):
            e = e.replace(k, vals[k])
        for n, x in locs.items():
            if depth < 4:
                e = re.sub(r'\b' + n + r'\b', '(' + tr(x, depth + 1) + ')', e)
        e = e.replace('!=', ' <> ').replace('&&', ' and ').replace('||', ' or ')
        e = re.sub(r'!(?!=)', ' not ', e).replace(' <> ', ' != ')
        return e
    py = tr(cond)
    if re.search(r'[A-Za-z_]\w*(?:\.|::)', py) or re.search(r'\b(?!and\b|or\b|not\b)[A-Za-z_]\w*\b(?!\')', py.replace("'bc1'", '').replace("'unknown'", '')):
        raise ValueError(py)
    return bool(eval(py, {'__builtins__': {}}, {}))   # noqa: S307 - only digits, quotes, comparisons and and / or / not reach here


# ------------------------------------------------------------------ the join
def pairs_for(li, facts):
    """the facts of ours that pair with game list li -> [(fact, source)]"""
    out = []
    lk = set(li['keys'])
    for f in facts:
        fk = set(f['keys'])
        src = None
        if li['fn'] in f['cites']:
            src = f'cites {li["fn"]}'
        elif {k for k in lk & fk if k >= 100}:
            src = 'key ' + ', '.join(str(k) for k in sorted(k for k in lk & fk if k >= 100))
        if not src:
            continue
        if lk and fk and not (lk & fk) and PLANT != 'no_keys':
            continue
        out.append((f, src))
    return out


def judge(gvals, ov, cited, kind, rule):
    """one game list's count set against one fact of ours -> (verdict, why)"""
    real = set(gvals) - {INHERIT}
    gt = ', '.join(sorted(vtext(v) for v in gvals))
    if not real:
        return 'UNSETTLED', 'the game sets no count here: the caller\'s (an inherited count)'
    if not rule['present']:
        return 'GAME-ONLY', f'the game counts {gt}; {rule["why"]}'
    if kind in rule['refused'] and ov != FILE:
        return 'GAME-ONLY', f'the game counts this {KIND_NAME[kind]} list ({gt}); ours carries {vtext(ov)} but its upload rule refuses a {KIND_NAME[kind]} ({rule["why"]}): the file\'s chain'
    if kind in (1, 2) and rule.get('unread') and ov != FILE:
        return 'UNSETTLED', f'a {KIND_NAME[kind]} list; {rule["why"]}'
    if ov == FILE:
        return 'GAME-ONLY', f'the game counts {gt}; ours leaves the file\'s chain'
    oper = isinstance(ov, tuple) and ov[0] == TABLE
    orule = isinstance(ov, tuple) and ov[0] in ('rule', 'param', 'expr')
    leaves = ov[2] if isinstance(ov, tuple) and ov[0] == 'rule' else [ov]
    lconst = {x for x in leaves if x == WHOLE or isinstance(x, int)}
    if TABLE in real:
        if oper:
            return 'OK', 'per name both sides (the game\'s MipmapData lookup; ours a per-name rule)'
        if orule:
            return 'UNSETTLED', f'the game per name ({gt}); ours a rule {vtext(ov)}'
        return 'DIFF', f'the game overrides per name ({gt}); ours flat {vtext(ov)}'
    if any(isinstance(v, tuple) for v in real):
        if (orule or oper) and cited:
            return 'OK', f'a non-constant count both sides; our rule {vtext(ov)} cites the builder (its values are its unit\'s)'
        if FILE in leaves and not lconst:
            return 'GAME-ONLY', f'the game counts {gt}; an arm of ours gives the file\'s chain'
        return 'UNSETTLED', f'the game\'s count is not a constant ({gt}); ours {vtext(ov)}'
    if oper:
        return 'DIFF', f'the game builds flat ({gt}); ours per name'
    if isinstance(ov, int) or ov == WHOLE:
        if INHERIT in gvals:
            return 'UNSETTLED', f'the game: {gt} (partly the caller\'s); ours {vtext(ov)}'
        if real == {ov}:
            return 'OK', f'{vtext(ov)} both sides'
        if ov in real:
            return 'UNSETTLED', f'the game builds this list at several counts ({gt}); ours {vtext(ov)}'
        return 'DIFF', f'the game {gt}; ours {vtext(ov)}'
    if FILE in leaves and not (lconst & real):
        return 'GAME-ONLY', f'the game counts {gt}; an arm of ours ({ov[1]}) gives the file\'s chain'
    if lconst & real:
        return ('OK', f'an arm of our rule {ov[1]} gives the game\'s {gt} (the rule cites the builder)') if cited else \
            ('UNSETTLED', f'an arm of our rule {ov[1]} gives {gt}, the rule does not cite the builder')
    if leaves and all(x == WHOLE or isinstance(x, int) or x == FILE for x in leaves):
        return 'DIFF', f'the game {gt}; our rule {ov[1]} gives {", ".join(vtext(x) for x in leaves)}'
    return 'UNSETTLED', f'the game {gt}; ours {vtext(ov)}'


def ofact_row(f):
    return {'value': vtext(f['value']), 'cond': f.get('cond'), 'at': f['at'], 'kind': f['kind']}


def run(lists, facts, rule, fams=None):
    """THE join -> rows in the code-diff schema (field `levels`)"""
    fams = fams or {}
    rows, used = [], set()
    for li in lists:
        ps = pairs_for(li, facts)
        game_vals = [{'value': ', '.join(sorted(vtext(v) for v in li['vals'])), 'at': li['at']}]
        lst = {'at': li['at'], 'callee': li['callee'], 'kind': KIND_NAME[li['kind']], 'keys': li['keys']}
        if li.get('inherited_from'):
            lst['inherited_from'] = li['inherited_from']
        if not ps:
            if li['vals'] == {INHERIT}:
                v, why = 'UNSETTLED', 'the game sets no count here (inherited) and no texture directory of ours pairs with it'
            elif PLANT == 'no_unpaired':
                continue
            else:
                v, why = 'GAME-ONLY', ('the game counts ' + ', '.join(sorted(vtext(x) for x in li['vals']))
                                       + '; no texture directory of ours pairs with this list (none cites ' + li['fn'] + ', no shared key >= 100)')
            rows.append({'fn': '-', 'block': None, 'game': li['fn'], 'field': FIELD, 'verdict': v, 'why': why, 'pair': [],
                         'ours': [], 'game_vals': game_vals, 'family': 'scene', 'list': lst, 'others': []})
            continue
        judged = []
        for f, src in ps:
            vd, why = judge(li['vals'], f['value'], li['fn'] in f['cites'], li['kind'], rule)
            judged.append((BEST.index(vd), vd, why, f, src))
            used.add(f['at'])
        judged.sort(key=lambda x: (x[0], x[3]['at']))
        _r, vd, why, f, src = judged[0]
        others = [f"{v2} {f2['fn']} @{f2['at']} ({s2}): {w2}" for _i, v2, w2, f2, s2 in judged[1:]]
        rows.append({'fn': f['fn'], 'block': None, 'game': li['fn'], 'field': FIELD, 'verdict': vd, 'why': why, 'pair': [src],
                     'ours': [ofact_row(f)], 'game_vals': game_vals, 'family': 'scene', 'list': lst, 'others': others})
    for f in facts:
        if f['at'] in used or f['value'] == FILE:
            continue
        rows.append({'fn': f['fn'], 'block': None, 'game': '-', 'field': FIELD, 'verdict': 'OURS-ONLY',
                     'why': 'a counted directory of ours that pairs with no game list (it cites ' + (', '.join(f['cites'][:3]) or 'no game function') + ')',
                     'pair': [], 'ours': [ofact_row(f)], 'game_vals': [], 'family': 'scene', 'list': None, 'others': []})
    rank = {v: i for i, v in enumerate(C.VERDICTS)}
    rows.sort(key=lambda r: (rank[r['verdict']], r['fn'] == '-', r['game'], (r['list'] or {}).get('at', ''), r['fn']))
    return rows


def collect(ref=None, db=None):
    """-> (rows, problems, info) for our tree at ref (None = the working tree)"""
    tree = Tree(ref)
    facts, problems = ours_facts(tree)
    rule = upload_rule(tree)
    db = db or GameDb()
    cited = {int(c[4:], 16) for f in facts for c in f['cites']}
    lists, fams, p2 = game_lists(db, cited)
    rows = run(lists, facts, rule, fams)
    return rows, problems + p2, {'ours_facts': len(facts), 'lists': len(lists), 'families': len(fams), 'upload': rule['why'],
                                 'facts': facts, 'game_lists': lists}


# ------------------------------------------------------------------ the entry
def row_text(r):
    li = r.get('list') or {}
    where = f" list {li['at']} {li['kind']} via {li['callee']}" + (f" keys {', '.join(str(k) for k in li['keys'])}" if li.get('keys') else '') if li else ''
    lines = [f"{r['verdict']:10s} {r['fn']} vs {r['game']}  levels [{where.strip()}]", f"           {r['why']}"]
    lines.append('           game: ' + ('; '.join(f"{g['value']} @{g['at']}" for g in r['game_vals']) or 'no list'))
    for x in li.get('inherited_from', ())[:4]:
        lines.append(f'             inherited from {x}')
    lines.append('           ours: ' + ('; '.join(f"{o['value']} @{o['at']} ({o['kind']})" for o in r['ours']) or 'no directory of ours'))
    if r['pair']:
        lines.append('           pair: ' + ', '.join(r['pair']))
    for o in r.get('others', ())[:6]:
        lines.append(f'           also: {o}')
    if len(r.get('others', ())) > 6:
        lines.append(f"           also: (+{len(r['others']) - 6} more pairs; --json)")
    return '\n'.join(lines)


def _jsonable(x):
    if isinstance(x, set):
        return sorted(vtext(v) for v in x)
    if isinstance(x, tuple):
        return list(x)
    return str(x)


def main(argv):
    ap = argparse.ArgumentParser(prog='q codediff --field levels', description=__doc__.split('\n')[0])
    ap.add_argument('--field', default=FIELD)
    ap.add_argument('--fn', help='one of our functions (qualified name, a glob, the last name part)')
    ap.add_argument('--game', help='one game function sub_X')
    ap.add_argument('--flag', nargs='+', choices=C.VERDICTS, help='only these verdicts')
    ap.add_argument('--ref', help='our code at this git ref')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--facts', action='store_true', help='the two sides as code-diff facts (JSON), no join')
    ap.add_argument('--no-rank', action='store_true', help='(accepted for q codediff; levels rows are not call-site ranked)')
    a = ap.parse_args(argv)
    if a.field != FIELD:
        raise LevelsError(f'--field {a.field}: this extractor answers --field {FIELD} only')
    if a.ref and a.ref.startswith('-'):
        raise LevelsError(f'--ref {a.ref!r}: a REF starting with - would reach git as an option')
    rows, problems, info = collect(a.ref)
    if a.facts:
        out = [{'side': 'game', 'fn': li['fn'], 'field': FIELD, 'value': sorted(vtext(v) for v in li['vals']), 'cond': None, 'at': li['at'],
                'kind': 'list', 'list_kind': KIND_NAME[li['kind']], 'keys': li['keys']} for li in info['game_lists']]
        out += [dict({k: v for k, v in f.items()}, value=ofact_row(f)['value']) for f in info['facts']]
        print(json.dumps(out, indent=1, default=_jsonable))
        return 2 if problems else 0
    sel = []
    for r in rows:
        if a.fn and not (a.fn == r['fn'] or fnmatch.fnmatchcase(r['fn'], a.fn) or r['fn'].endswith('::' + a.fn)):
            continue
        if a.game and r['game'].lower() != 'sub_' + a.game.lower().replace('sub_', '').replace('0x', ''):
            continue
        if a.flag and r['verdict'] not in a.flag:
            continue
        sel.append(r)
    tot = collections.Counter(r['verdict'] for r in rows)
    if a.json:
        print(json.dumps({'ref': a.ref or 'the working tree', 'rows': sel, 'totals': dict(tot), 'problems': problems,
                          'info': {k: v for k, v in info.items() if k not in ('facts', 'game_lists')}}, indent=1, default=_jsonable))
        return 2 if problems else 0
    for r in sel:
        print(row_text(r))
    print(f"totals levels ({a.ref or 'the working tree'}): {len(rows)} rows over {info['lists']} game lists of {info['families']} families; "
          + ', '.join(f'{v} {tot.get(v, 0)}' for v in C.VERDICTS if v != 'DEPARTED') + (f'; shown {len(sel)}' if len(sel) != len(rows) else ''))
    print(f"sources: {info['ours_facts']} TexSet calls of ours; the count word's setter callers (decomp); upload rule: {info['upload']}")
    for p in problems:
        print('PROBLEM: ' + p)
    return 2 if problems else 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except (LevelsError, O.OstateError) as e:
        print(f'q codediff --field levels: {e}', file=sys.stderr)
        sys.exit(2)
