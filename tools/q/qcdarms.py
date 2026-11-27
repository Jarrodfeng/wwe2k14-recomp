r"""S111 CDARMS: OSTATEWIDE's two tool limits, read here and hooked into q ostate's wide reader (tools/q/qostate_wide.py), q codediff
(tools/q/qcodediff.py) and q pairs --coverage (tools/q/qpairs_cov.py).

1. A bind compared ARM BY ARM. Each store qostate_wide.bind_facts reads on a bind record (FrontEnd::BindMaterial's
   `b->alpha_test = ...`) carries the if-arm it sits in (arm_keys):
     arm       file:line of the innermost `if (..)` / `else` holding the store (braced or not; a loop / switch / lambda brace is passed
               through); no if-arm = the function's own first line (the code after an arm that returns: BindMaterial's material arm);
     attached  the lines of the comment block directly above the store (a comment that ends on the line above it);
     scope     a comment's scope words `<word> pass only` (SCOPE_RE) on those lines, on the store's line or on the arm's line / the line
               above it: the pass the arm serves (`this bind serves the colour pass only`).
   q codediff makes each arm its own unit (qcodediff.unit_of) paired only by the citations in scope of that arm (arm_scope: the
   cite_scope rule of CODEDIFF's review: a citation on the arm's line or the line above stands for every field; on a fact's line, the
   line above it or its attached block, for those facts' fields; anywhere else, nothing), and a fact's attached citations count as on
   its line for its nearest setter (qcodediff.keep_for). The scope words are read the way cite_scope reads an arm's `pass class X`:
   the arm stands only against the game functions of the pass its words name. A cited game function whose pass is known to be
   another one (db/ids/origin_table.json `technique`, by PASS_TECH) is out of the arm's scope (out_of_scope); a function of no known
   pass is kept (nothing refutes it); a scope word PASS_TECH does not know scopes nothing.
2. A value followed into OUR callers (climb). A DrawState that is a parameter of its function (Binder::InjectCrowd's
   `const rdr::DrawState& state`) is the state each caller passes: the call sites of the function (`NAME(` in the files the wide
   reader holds and, by git grep, in src at the ref; a declaration / definition is no call; the argument count must fit the parameter
   list, defaults counted), the argument at the parameter's place resolved in the caller's function by qostate_wide.Wide.resolve
   (a `{}` block, a copy, a member the game's device words fill, a parameter again: up its callers), depth-limited by
   qostate_wide.DEPTH (as gstate --inherit climbs the game's callers). The same value set from every caller = that value (each
   caller's entries, their conditions kept); different sets = UNSETTLED: a rule naming each caller and its values.
PLANT (selftest only): WWE_QOSTATEWIDE_PLANT = no_arm (the arm keys not written: one unit per function, OSTATEWIDE's) | no_climb (a
parameter stays a rule of ours); qcodediff.PLANT = one_arm (the arm not a unit) | arm_anywhere (an arm takes every citation of its
function) | no_scope_words (the scope words not read).
"""
import functools
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qconstdiff as CD   # noqa: E402
import qostate as O       # noqa: E402

SCOPE_RE = re.compile(r'\b([A-Za-z][\w-]*)\s+pass(?:es)?\s+only\b')
# a scope word -> the game pass techniques it names (db/ids/origin_table.json `technique` of the pass functions)
PASS_TECH = {'colour': ('T_Lambert',), 'color': ('T_Lambert',), 'lit': ('T_Lambert',),
             'shadow': ('T_ShadowMap', 'T_PlaneShadowMap'), 'shadow-map': ('T_ShadowMap',), 'rim': ('T_RimLightMap',),
             'z': ('T_ZPass',), 'depth': ('T_ZPass',)}
KNOWN_TECH = {t for ts in PASS_TECH.values() for t in ts}
PASSES_FILE = 'db/ids/origin_table.json'
CITE_SRC = re.compile(r'(?:comment|code|claim|seam-why) (.+):(\d+)')
NOT_CALL_PRE = re.compile(r'(?:[\w\]>]|(?<!&)&|\*)\s+$')
KEYWORD_PRE = re.compile(r'\b(?:return|else|case|co_return|co_await|throw|delete)\s+$')


def plant():
    return os.environ.get('WWE_QOSTATEWIDE_PLANT', '')


def _cd_plant():
    import qcodediff as CDF
    return CDF.PLANT


# ------------------------------------------------------------------ 1. the arm of a store (ours side)
def _prev(b, i):
    """the index of the last non-space character before i (-1: none)"""
    i -= 1
    while i >= 0 and b[i].isspace():
        i -= 1
    return i


def _open_paren(b, j):
    """b[j] == ')': the index of its '(' (-1: none)"""
    d = 0
    while j >= 0:
        if b[j] == ')':
            d += 1
        elif b[j] == '(':
            d -= 1
            if d == 0:
                return j
        j -= 1
    return -1


def _arm_head(b, j):
    """b[j] = the last character before a statement or a `{`: the index of the `if` / `else` that opens an arm there, else None"""
    if j >= 3 and b[j - 3:j + 1] == 'else' and (j < 4 or not (b[j - 4].isalnum() or b[j - 4] == '_')):
        return j - 3
    if j >= 0 and b[j] == ')':
        k = _open_paren(b, j)
        if k > 0:
            head = b[max(0, k - 40):k]
            m = re.search(r'\b(\w+)\s*$', head)
            if m and m.group(1) == 'if':
                return k - len(head) + m.start(1)
    return None


def arm_of(t, b, sp, pos):
    """-> the line of the if-arm holding the statement at pos (b = t with its strings blanked): a braceless `if (..) STMT` / `else
    STMT`, else the innermost `{` opened by an `if (..)` / `else` (other braces passed through), else the function's first line"""
    body = b.find('{', sp[0], sp[1])
    h = _arm_head(b, _prev(b, pos))
    if h is not None and h > body:
        return O.line_of(t, h)
    d, i = 0, pos - 1
    while i > body:
        ch = b[i]
        if ch == '}':
            d += 1
        elif ch == '{':
            if d == 0:
                h = _arm_head(b, _prev(b, i))
                if h is not None:
                    return O.line_of(t, h)
            else:
                d -= 1
        i -= 1
    return O.line_of(t, sp[0])


def attached(lines, ln):
    """the lines of the comment block that ends on the line above line ln (1-based), in order"""
    out, j = [], ln - 1
    while j >= 1 and lines[j - 1].lstrip().startswith(('//', '/*', '*')):
        out.append(j)
        j -= 1
    return sorted(out)


def scope_word(lines, cands):
    """-> (word, line) of the first `<word> pass only` on the candidate lines [(line, whole)]: a whole comment line, else the line's
    `//` comment; None"""
    for n, whole in cands:
        if not 1 <= n <= len(lines):
            continue
        s = lines[n - 1]
        part = s if whole else (s[s.find('//'):] if '//' in s else '')
        m = SCOPE_RE.search(part)
        if m:
            return m.group(1).lower(), n
    return None


def arm_keys(w, rel, sp, spos, ln):
    """the arm keys of a bind store at line ln (its statement at spos, its function at sp): {arm, attached, scope, scope_at}"""
    if plant() == 'no_arm':
        return {}
    import qostate_wide as W
    text, t, _src = w.text(rel)
    aln = arm_of(t, W._blank(t), sp, spos)
    lines = text.split('\n')
    att = attached(lines, ln)
    sc = scope_word(lines, [(x, True) for x in att] + [(ln, False), (aln, False), (aln - 1, False)])
    out = {'arm': f'{rel}:{aln}', 'attached': att or None}
    if sc:
        out.update(scope=sc[0], scope_at=f'{rel}:{sc[1]}')
    return out


# ------------------------------------------------------------------ 1. an arm's citations (the diff side)
def pass_techniques(read):
    """{sub_X: technique} of db/ids/origin_table.json's pass functions and submitters ({} when not read)"""
    try:
        d = json.loads(read(PASSES_FILE))
    except (OSError, ValueError, O.OstateError):
        return {}
    out = {}
    for r in d.get('passes', []) + d.get('submitters', []):
        if r.get('technique'):
            out[f"sub_{int(r['fn'], 16):08X}"] = r['technique']
    return out


def out_of_scope(g, facts, passes):
    """the arm's scope words name a pass and game function g's known pass is another one"""
    if not passes or _cd_plant() == 'no_scope_words':
        return False
    own = {t for f in facts if f.get('scope') for t in PASS_TECH.get(f['scope'], ())}
    tech = passes.get(g)
    return bool(own) and tech in KNOWN_TECH and tech not in own


def near_lines(f):
    """the lines a citation stands on for fact f: its own, the line above, its attached comment block"""
    n = int(f['at'].rpartition(':')[2])
    return {n, n - 1} | set(f.get('attached') or ())


def arm_scope(g, srcs, facts, passes=None):
    """-> [(source, fields)] of the pair sources (with game function g) that stand for one arm unit; fields None = every field"""
    if out_of_scope(g, facts, passes):
        return []
    if _cd_plant() == 'arm_anywhere':
        return [(s, None) for s in srcs]
    arms = {f['arm'] for f in facts if f.get('arm')}
    out = []
    for s in srcs:
        m = CITE_SRC.fullmatch(s)
        if not m:
            out.append((s, None))
            continue
        rel, ln = m.group(1), int(m.group(2))
        if any(a.rpartition(':')[0] == rel and int(a.rpartition(':')[2]) in (ln, ln + 1) for a in arms):
            out.append((s, None))
            continue
        fl = {f['field'] for f in facts if f['at'].rpartition(':')[0] == rel and ln in near_lines(f)}
        if fl:
            out.append((s, fl))
    return out


# ------------------------------------------------------------------ 2. a parameter: the state its callers pass (ours side)
def param_index(b, sp, name, var):
    """-> (place, parameters, required ones) of var in the parameter list of the function `name` at sp, else None"""
    body = b.find('{', sp[0], sp[1])
    if body < 0:
        return None
    m = re.search(r'(?<![\w])' + re.escape(name) + r'\s*\(', b[sp[0]:body])
    if m is None:
        return None
    op = sp[0] + m.end() - 1
    cl = CD._match(b, op, '(', ')')
    params = [p for p in O._depth_split(b[op + 1:cl - 1], ',') if p.strip()]
    for i, p in enumerate(params):
        mm = re.search(r'(\w+)\s*(?:\[[^\]]*\])?$', p.split('=')[0].strip())
        if mm and mm.group(1) == var:
            return i, len(params), sum(1 for x in params if '=' not in x)
    return None


@functools.lru_cache(maxsize=256)
def _grep_files(ref, name):
    import qostate_wide as W
    cmd = ['git', 'grep', '-l', '-E', r'\b' + name + r'\s*\('] + ([ref] if ref else []) + ['--', 'src']
    r = subprocess.run(cmd, cwd=O.REPO, capture_output=True, text=True, encoding='utf-8', errors='replace')
    rels = [ln.split(':', 1)[1] if ref else ln for ln in r.stdout.splitlines()] if r.returncode == 0 else []
    return tuple(x for x in rels if x.endswith(W.EXTS) and 'test' not in x.rpartition('/')[2].lower())


def call_sites(w, name, def_rel, def_sp):
    """-> [(file, function span, offset, [argument])] the calls of `name` in the files read and in src (git grep at the ref)"""
    import qostate_wide as W
    pat = re.compile(r'(?<![\w])' + re.escape(name) + r'\s*\(')
    out = []
    for rel in dict.fromkeys(list(w.files) + list(_grep_files(w.ref, name))):
        try:
            _text, t, src = w.text(rel)
        except (OSError, O.OstateError):
            continue
        b = W._blank(t)
        own_head = b.find('{', def_sp[0], def_sp[1]) if rel == def_rel else -1
        for m in pat.finditer(b):
            if rel == def_rel and def_sp[0] <= m.start() < own_head:
                continue   # the function's own head
            pre = b[max(0, m.start() - 80):m.start()]
            if NOT_CALL_PRE.search(pre) and not KEYWORD_PRE.search(pre):
                continue   # `uint32_t NAME(` / `T& NAME(`: a declaration or a definition
            op = m.end() - 1
            cl = CD._match(b, op, '(', ')')
            if re.match(r'\s*(?:(?:const|noexcept|override|final|mutable)\s*)*\{', b[cl:cl + 120]):
                continue   # `Q::NAME(..) {`: a qualified definition
            ln = O.line_of(t, m.start())
            csp = W.fn_span(t, src, ln, m.start())
            if csp is None:
                continue
            inner = t[op + 1:cl - 1]
            out.append((rel, csp, m.start(), [a.strip() for a in O._depth_split(inner, ',')] if inner.strip() else []))
    return out


def climb(w, rel, sp, var, fn, depth):
    """-> ({field: [entry]}, description) of parameter var of function fn (at sp in rel): the states its callers pass; None when var
    is no parameter of fn"""
    import qostate_wide as W
    t = w.text(rel)[1]
    name = fn.rpartition('::')[2]
    pi = param_index(W._blank(t), sp, name, var)
    if pi is None:
        return None
    idx, n_all, n_req = pi
    where = f'{var}, the parameter of {fn} @{rel}:{O.line_of(t, sp[0])}'
    if depth >= W.DEPTH:
        return w.named(f'{where} (its callers deeper than {W.DEPTH} calls not climbed)'), var
    results, other = [], 0
    for crel, csp, cpos, args in call_sites(w, name, rel, sp):
        if not n_req <= len(args) <= n_all:
            other += 1
            continue
        ct, csrc = w.text(crel)[1], w.text(crel)[2]
        cln = O.line_of(ct, cpos)
        got, desc = w.resolve(crel, csp, args[idx], cpos, depth + 1)
        results.append((f'{crel}:{cln} {csrc.name(cln)}', got, desc))
    if not results:
        return w.named(f'{where}: no caller of {name} found' + (f' ({other} call(s) of another argument count)' if other else '')), var
    out = {}
    for f in O.FIELDS:
        sets = [frozenset(str(x[0]) for x in got.get(f, ())) for _w, got, _d in results]
        if all(s == sets[0] for s in sets):
            out[f] = [x for _w, got, _d in results for x in got.get(f, ())]
        else:
            listing = '; '.join(f"{wh}: {', '.join(sorted(s)) or 'nothing'}" for (wh, _g, _d), s in zip(results, sets))
            out[f] = [(f'RULE: the callers of {name} pass different states ({listing})', None, None, 'copy', {})]
    return out, f'{where}: ' + '; '.join(f'{wh} passes {d}' for wh, _g, d in results)
