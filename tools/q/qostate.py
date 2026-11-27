r"""q ostate: per function of OURS, the render state it sets, as code-diff facts (S111 OSTATE, piece 3 of the code-diff tool: the ours-side
render-state extractor; docs/research/s111/WHOLEDIFF_PLAN.md A.3).

    py tools/q/q.py ostate [--fn NAME] [--field F] [--json] [--facts] [--ref REF]

One evaluator (rule 6): the small C++ reader `q rstdiff` compares with lives HERE (Ctx, eval_cond, eval_val, assign_targets, statements,
parse_table, ours_state, effect_rows); tools/q/qrstdiff.py imports it. On top of it this tool reads three inputs rstdiff never compared:
  (a) the rows of src/renderer_scene/pass_state.h kTable, one fact per (row, field), keyed `pass_state::kTable[NAME]` and carrying the
      row's `addr` cell and the game functions it names (alpha func / ref, z func through ForwardZ as rstdiff mirrors it, z write, cull,
      blend code);
  (b) the alpha test of a draw: the `fb.Dynamic(EXPR)` that follows a `rdr::DrawState X{};` block in the same function. A ternary whose
      arm calls AlphaDynamic / alpha_dyn = tested when its condition holds (atest = that condition, read to its sources: the material
      flag `Material::alpha_test` / the bind's `b.alpha_test`, a caller-stated `InstancedDraw::alpha_test`, or a rule of ours such as
      FrontEnd::Record's `at_on` terms); the tested arm's func / ref are AlphaDynamic's `system[9]` / `system[8]` per pass class
      (pass_state's alpha_test_func / alpha_ref), the other arm's the base state's `system[9]` (7 = always = no test). A plain
      `Dynamic(VAR)` reads VAR's `system[9]` / `system[8]` (func 7 = test off);
  (c) the caller stores of `InstancedDraw` fields (`VAR.alpha_test = `, `.no_blend = `, VAR declared an InstancedDraw), by text, keyed
      by the enclosing function AND the variable's declaration (`decl`: one unit per draw), with the if / for conditions the store sits
      under; a declared draw that reaches DrawInstanced with alpha_test never stored is a `left` fact (the -1: the material's flag).
Every `rdr::DrawState X{};` block in qrstdiff.COVER_DIRS is read: a block `q rstdiff` has in its KINDS with that kind's anchors (its game
words, known conditions, helpers), any other from its start to the first `State(X)` that consumes it (else its scope's end). A block
that reads `pass_state::Of(pcls)` is read once per pass class ClassOf can return; a fact the same for every class carries no class.
The fact form is `q gstate --facts`' (tools/q/qgstate.py `facts`): {side, fn, field, value, cond, at, kind} + extras. side = ours; fn =
the enclosing function as `q assume` names it (qsym's qualified name); field = gstate's setter key where the game has one (alpha_test,
alpha_func, alpha_ref, z_enable, z_write, z_func, blend_enable, blend_code, cull, colour_write_mask, stencil_enable), else ours'
own (blend_op / blend_src / blend_dst, no_blend); value = an int, else {'material': EXPR} / {'caller': 'InstancedDraw::F'} /
{'game': EXPR} (our draw reads the game's own word) / {'rule': EXPR} (a rule of ours); cond = None (unconditional) or the live
conditions; at = file:line; kind = set | default (the `X{}` zero, kept until an unconditional set) | table | caller | alpha.
Units are OURS (the join converts): compares are VkCompareOp numbers (z_func forward: the game's reversed GEQUAL is our LEQUAL 3), cull
is a VkCullModeFlags literal, alpha_ref is 0..255 (the game's raw byte). --field takes gstate's key or rstdiff's short name (atest,
afunc, aref, ztest, zwrite, zfunc, blend, cmask, stencil). --ref reads the tree at a git ref through `git show` (no checkout).
"""
import argparse
import fnmatch
import functools
import glob
import itertools
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qconstdiff as CD   # noqa: E402

REPO = CD.REPO
FIELDS = ('ztest', 'zwrite', 'zfunc', 'blend', 'blend_op', 'blend_src', 'blend_dst', 'cull', 'cmask', 'stencil')
OURS_FIELD = {'depth_test': 'ztest', 'depth_write': 'zwrite', 'depth_compare': 'zfunc', 'cull': 'cull', 'stencil_test': 'stencil',
              'blend[0].enable': 'blend', 'blend[0].color_op': 'blend_op', 'blend[0].src_color': 'blend_src',
              'blend[0].dst_color': 'blend_dst', 'blend[0].write_mask': 'cmask'}
WHOLE_BLEND = ('blend', 'blend_op', 'blend_src', 'blend_dst', 'cmask')   # `X.blend[0] = Y` sets these from Y (the mask: a rule of ours)
# Effect-file render-state rows (device offset = 4 x render state row) that this tool compares (fill mode, offset 52, is not one).
FX_ROWS = {40: 'ztest', 44: 'zfunc', 48: 'zwrite', 56: 'cull', 60: 'blend', 72: 'blend_src', 76: 'blend_dst', 80: 'blend_op',
           96: 'atest', 100: 'aref', 104: 'afunc', 108: 'stencil', 212: 'cmask'}
PLANT_RE = re.compile(r'plant|PLANT|faults_\s*\.')
START_RE = r'rdr::DrawState\s+(?P<var>\w+)\s*\{\s*\}\s*;'
GAME = 'GAME'
PS = 'src/renderer_scene/pass_state.h'
FE = 'src/renderer_scene/front_end.cpp'

# the fact field of each rstdiff field: `q gstate`'s setter key where the game has one (its schema wins: WHOLEDIFF_PLAN.md A.2)
FIELD_KEY = {'ztest': 'z_enable', 'zwrite': 'z_write', 'zfunc': 'z_func', 'blend': 'blend_enable', 'blend_op': 'blend_op',
             'blend_src': 'blend_src', 'blend_dst': 'blend_dst', 'cull': 'cull', 'cmask': 'colour_write_mask', 'stencil': 'stencil_enable',
             'atest': 'alpha_test', 'afunc': 'alpha_func', 'aref': 'alpha_ref'}
TABLE_FIELD = {'alpha_test_func': 'alpha_func', 'alpha_ref': 'alpha_ref', 'game_z_func': 'z_func', 'z_write': 'z_write', 'cull': 'cull',
               'blend_code': 'blend_code'}
INST_FIELDS = ('alpha_test', 'no_blend')   # the InstancedDraw fields a caller states (front_end.h rdr_scene::InstancedDraw; S111 CARDCULL removed cull_none)
ALPHA_CALL_RE = re.compile(r'\b(?:AlphaDynamic|alpha_dyn)\s*\(')
SOURCE_RE = re.compile(r'\b(\w+)\s*(?:\.|->)\s*alpha_test\b')


class OstateError(Exception):
    pass


class MissingAtRef(OstateError):
    """default_reader: the path is not in the ref read (`git show REF:PATH` on a commit that lacks it)."""
    def __init__(self, msg, ref, rel):
        super().__init__(msg)
        self.ref, self.rel = ref, rel


@functools.lru_cache(maxsize=None)
def born_after(ref, rel):
    """True when REL's first commit is not in REF's history: REF is older than the file (S111 CASTCULL review: the selftest's
    frozen refs predate caster_cull.h). A file never committed, or one REF's history holds (deleted or renamed since), is False."""
    r = subprocess.run(['git', 'log', '--diff-filter=A', '--format=%H', '--', rel], cwd=REPO, capture_output=True, text=True)
    adds = r.stdout.split() if r.returncode == 0 else []
    if not adds:
        return False
    a = subprocess.run(['git', 'merge-base', '--is-ancestor', adds[-1], ref], cwd=REPO, capture_output=True)
    return a.returncode == 1


# ------------------------------------------------------------------ ours: the DrawState block, parsed
def default_reader(ref=None):
    def read(rel):
        if ref:
            r = subprocess.run(['git', 'show', f'{ref}:{rel}'], cwd=REPO, capture_output=True, text=True, encoding='utf-8', errors='replace')
            if r.returncode != 0:
                msg = f'git show {ref}:{rel}: {r.stderr.strip()}'
                if subprocess.run(['git', 'cat-file', '-e', f'{ref}^{{commit}}'], cwd=REPO, capture_output=True).returncode == 0:
                    raise MissingAtRef(msg, ref, rel)   # the ref is a commit: the path is not in it
                raise OstateError(msg)
            return r.stdout
        with open(os.path.join(REPO, rel.replace('/', os.sep)), encoding='utf-8', errors='replace') as fh:
            return fh.read()
    return read


def line_of(text, pos):
    return text.count('\n', 0, pos) + 1


def _depth_split(s, sep):
    """s split at top-level occurrences of sep (outside () [] {})."""
    out, d, cur, i = [], 0, [], 0
    while i < len(s):
        ch = s[i]
        if ch in '"\'':   # a string or char literal: kept whole (a kTable row's address text holds commas)
            j = i + 1
            while j < len(s) and s[j] != ch:
                j += 2 if s[j] == '\\' else 1
            cur.append(s[i:j + 1])
            i = j + 1
            continue
        if ch in '([{':
            d += 1
        elif ch in ')]}':
            d -= 1
        if d == 0 and s.startswith(sep, i):
            out.append(''.join(cur))
            cur, i = [], i + len(sep)
            continue
        cur.append(ch)
        i += 1
    out.append(''.join(cur))
    return out


def split_ternary(s):
    """'c ? a : b' at top level -> (c, a, b) (right associative), else None."""
    d, q, qpos = 0, 0, -1
    for i, ch in enumerate(s):
        if ch in '([{':
            d += 1
        elif ch in ')]}':
            d -= 1
        elif d == 0 and ch == '?':
            if qpos < 0:
                qpos = i
            else:
                q += 1
        elif d == 0 and ch == ':' and qpos >= 0 and not (s[i - 1:i] == ':' or s[i + 1:i + 2] == ':'):
            if q == 0:
                return s[:qpos].strip(), s[qpos + 1:i].strip(), s[i + 1:].strip()
            q -= 1
    return None


def strip_parens(s):
    s = s.strip()
    while s.startswith('(') and CD._match(s, 0, '(', ')') == len(s):
        s = s[1:-1].strip()
    m = re.fullmatch(r'(?:u?int\d+_t|float|bool|int)\s*\((.*)\)', s, re.S)
    return strip_parens(m.group(1)) if m and CD._match(s, s.index('('), '(', ')') == len(s) else s


class Ctx:
    """What an expression of one ours block can be resolved against."""
    def __init__(self, texts, block, known=None, game_words=(), table=None, classes=()):
        self.texts, self.block, self.known = texts, block, known or {}
        self.game_words = [re.compile(w) for w in game_words]
        self.table, self.classes = table or {}, classes

    def is_game(self, s):
        return any(w.search(s) for w in self.game_words)

    def local(self, name):
        m = None
        for m in re.finditer(r'\b(?:const\s+)?(?:bool|auto|int|uint\d+_t|float)\s+' + re.escape(name) + r'\s*=\s*([^;]+);', self.block):
            pass
        return m.group(1) if m else None

    def const(self, name):
        for t in self.texts:
            for m in re.finditer(r'constexpr[^;\n]*?\b' + re.escape(name) + r'\s*=\s*(0[xX][0-9a-fA-F]+|\d+)[uU]?\b', t):
                return int(m.group(1), 0)
        return None

    def helper(self, name):
        for t in self.texts:
            m = re.search(r'\b' + re.escape(name) + r'\s*\(([^()]*)\)\s*(?:const\s*)?\{\s*return\s+([^;]+);\s*\}', t)
            if m:
                params = [p.strip().split()[-1].lstrip('&*') for p in m.group(1).split(',') if p.strip()]
                return params, m.group(2)
        return None


def eval_cond(c, ctx, env, depth=0):
    """-> True / False / None (unknown)."""
    c = strip_parens(c)
    ors = _depth_split(c, '||')
    if len(ors) > 1:
        vs = [eval_cond(x, ctx, env, depth) for x in ors]
        return True if True in vs else False if all(v is False for v in vs) else None
    ands = _depth_split(c, '&&')
    if len(ands) > 1:
        vs = [eval_cond(x, ctx, env, depth) for x in ands]
        return False if False in vs else True if all(v is True for v in vs) else None
    if c.startswith('!') and not c.startswith('!='):
        v = eval_cond(c[1:], ctx, env, depth)
        return None if v is None else not v
    if PLANT_RE.search(c):
        return False
    for k, v in ctx.known.items():
        if re.fullmatch(k, c):
            return v
    m = re.fullmatch(r'(?:\w+::)*Of\s*\(\s*\w+\s*\)\s*\.\s*(\w+)', c)
    if m and ctx.table and ctx.classes:   # S111 CDFOLLOW: a kTable column read as a condition (tested_blends), per pass class as a value is
        vals = {_truth(ctx.table[k].get(m.group(1))) for k in ctx.classes if k in ctx.table}
        return vals.pop() if len(vals) == 1 and None not in vals else None
    m = re.fullmatch(r'(.+?)\s*(==|!=)\s*(.+)', c, re.S)
    if m:
        a, b = eval_val(m.group(1), ctx, env, depth + 1), eval_val(m.group(3), ctx, env, depth + 1)
        if len(a) == 1 and len(b) == 1 and all(isinstance(x, int) for x in a | b):
            return (a == b) == (m.group(2) == '==')
        return None
    if c in env and len(env[c]) == 1:
        return bool(next(iter(env[c])))
    loc = ctx.local(c) if depth < 8 else None
    if loc is not None:
        return eval_cond(loc, ctx, env, depth + 1)
    return None


def _truth(v):
    """a kTable cell -> True / False (a bool column's `true` / `false`, an int), else None"""
    if isinstance(v, bool) or isinstance(v, int):
        return bool(v)
    return {'true': True, 'false': False}.get(str(v).strip())


def eval_val(expr, ctx, env=None, depth=0):
    """-> a set of ints, GAME, or 'RULE: <expr>'."""
    env = env or {}
    s = strip_parens(re.sub(r'\s+', ' ', expr))
    if depth > 12:
        return {f'RULE: {s}'}
    t = split_ternary(s)
    if t:
        c, a, b = t
        cv = eval_cond(c, ctx, env, depth)
        if cv is True:
            return eval_val(a, ctx, env, depth + 1)
        if cv is False:
            return eval_val(b, ctx, env, depth + 1)
        loc = ctx.local(c.strip()) if re.fullmatch(r'\w+', c.strip()) else None
        if ctx.is_game(c) or (loc and ctx.is_game(loc)):
            return {GAME}
        return eval_val(a, ctx, env, depth + 1) | eval_val(b, ctx, env, depth + 1)
    m = re.fullmatch(r'(0[xX][0-9a-fA-F]+|\d+)[uU]?', s)
    if m:
        return {int(m.group(1), 0)}
    if s in ('true', 'false'):
        return {1 if s == 'true' else 0}
    if s in env:
        return set(env[s])
    if ctx.is_game(s):
        return {GAME}
    m = re.fullmatch(r'(?:\w+::)*Of\s*\(\s*\w+\s*\)\s*\.\s*(\w+)', s)
    if m and ctx.table:
        vals = {ctx.table[c][m.group(1)] for c in ctx.classes if c in ctx.table and m.group(1) in ctx.table[c]}
        return vals or {f'RULE: {s}'}
    m = re.fullmatch(r'(?:\w+::)*(\w+)\s*\((.*)\)', s, re.S)
    if m and CD._match(s, s.index('('), '(', ')') == len(s):
        h = ctx.helper(m.group(1))
        if h:
            params, body = h
            args = [a for a in _depth_split(m.group(2), ',') if a.strip()]
            if len(args) == len(params):
                sets = [eval_val(a, ctx, env, depth + 1) for a in args]
                if all(all(isinstance(v, int) for v in x) for x in sets):
                    out = set()
                    for combo in itertools.islice(itertools.product(*[sorted(x) for x in sets]), 64):
                        out |= eval_val(body, ctx, dict(zip(params, ({v} for v in combo))), depth + 1)
                    return out
                if all(x == {GAME} for x in sets):
                    return {GAME}
            return eval_val(body, ctx, {}, depth + 1)   # an argument not known: the body with its parameters unbound (both arms of a test on them)
    if re.fullmatch(r'\w+', s):
        v = ctx.const(s)
        if v is not None:
            return {v}
        loc = ctx.local(s)
        if loc is not None:
            return eval_val(loc, ctx, env, depth + 1)
    return {f'RULE: {s}'}


CASE_RE = re.compile(r'^\s*(?:(?:case\s+(?:[^:;]|::)+?|default)\s*:(?!:)\s*)+')   # `case 1:` / `case Mode::Add:` / `default:` before an arm's statement
CHAIN_RE = re.compile(r'^\s*[\w.\[\]\s]+?=(?!=)(.*)$', re.S)              # `a = b = 1`: the value is the last right-hand side
ASSIGN_RE = re.compile(r'^\s*(?P<var>\w+)\s*\.\s*(?P<path>\w+(?:\s*\[\s*0\s*\])?(?:\s*\.\s*\w+)?)\s*=(?!=)\s*(?P<expr>.+?)\s*$', re.S)
WHOLE_RE = re.compile(r'^\s*(?P<var>\w+)\s*=(?!=)\s*(?P<expr>.+?)\s*$', re.S)   # `b = Y` (a whole BlendState through its reference)


def assign_targets(part, var, aliases):
    """-> ([the DrawState paths one statement part assigns], the value expression). Every target of a chain `a = b = 1` is one
    (the value is the last right-hand side; a target that is not our state, another object or a local, is passed over); a
    reference `b` of `X.blend[0]` stands for it (`b.enable = 1`, a whole `b = Y`). S111 RSTCOVER-A review: the first reader kept
    only the first target of a chain and dropped `auto&` references and whole assignments through one."""
    paths, expr = [], part
    while True:
        m = ASSIGN_RE.match(expr)
        w = None if m else WHOLE_RE.match(expr)
        if m and (m.group('var') == var or m.group('var') in aliases):
            p = re.sub(r'\s+', '', m.group('path'))
            paths.append(p if m.group('var') == var else 'blend[0].' + p)
            expr = m.group('expr')
        elif w and w.group('var') in aliases:
            paths.append('blend[0]')
            expr = w.group('expr')
        elif CHAIN_RE.match(expr):
            expr = CHAIN_RE.match(expr).group(1)
        else:
            return paths, expr


def statements(s, i, e, conds, out):
    """Walk s[i:e]: out += (start, text, conds) per simple statement; conds = [(cond text, taken)]."""
    while i < e:
        while i < e and s[i].isspace():
            i += 1
        if i >= e:
            break
        if s[i] == '{':
            j = CD._match(s, i, '{', '}')
            statements(s, i + 1, j - 1, conds, out)
            i = j
            continue
        if s[i] in '};':
            i += 1
            continue
        m = re.compile(r'(if|for|while|switch)\s*\(').match(s, i)
        if m:
            k = CD._match(s, m.end() - 1, '(', ')')
            cond = s[m.end():k - 1].strip() if m.group(1) == 'if' else f'{m.group(1)} ({s[m.end():k - 1].strip()})'
            j = _one_stmt(s, k)
            statements(s, k, j, conds + [(cond, True)], out)
            i = j
            me = re.compile(r'\s*else\b').match(s, i)
            if me and m.group(1) == 'if':
                j2 = _one_stmt(s, me.end())
                statements(s, me.end(), j2, conds + [(cond, False)], out)
                i = j2
            continue
        j = _one_stmt(s, i)
        out.append((i, s[i:j], conds))
        i = j
    return out


def _one_stmt(s, i):
    """end of the one statement at s[i] (a block, an if with its else, or up to ';' at depth 0)."""
    while i < len(s) and s[i].isspace():
        i += 1
    if i < len(s) and s[i] == '{':
        return CD._match(s, i, '{', '}')
    m = re.compile(r'(if|for|while|switch)\s*\(').match(s, i)
    if m:
        k = CD._match(s, m.end() - 1, '(', ')')
        j = _one_stmt(s, k)
        me = re.compile(r'\s*else\b').match(s, j)
        return _one_stmt(s, me.end()) if me and m.group(1) == 'if' else j
    d, j = 0, i
    while j < len(s):
        ch = s[j]
        if ch in '({[':
            d += 1
        elif ch in ')}]':
            d -= 1
        elif ch == ';' and d == 0:
            return j + 1
        j += 1
    return j


def parse_table(ps_text):
    """pass_state.h kTable -> {class name: {field: value}} (the PassState fields in declaration order)."""
    t = CD._mask(ps_text)
    m = re.search(r'struct\s+PassState\s*\{(.*?)\};', t, re.S)
    k = re.search(r'kTable\s*\[[^\]]*\]\s*=\s*\{', t)
    if not m or not k:
        raise OstateError(f'{PS}: no struct PassState / kTable')
    names = [re.split(r'[\s*]+', d.strip())[-1] for d in m.group(1).split(';') if d.strip()]
    body = t[k.end():CD._match(t, k.end() - 1, '{', '}') - 1]
    out = {}
    for rm in re.finditer(r'\{([^{}]*)\}', body):
        vals = [v.strip() for v in _depth_split(rm.group(1), ',')]
        if len(vals) != len(names):
            continue
        row = {}
        for n, v in zip(names, vals):
            mm = re.fullmatch(r'-?(0[xX][0-9a-fA-F]+|\d+)', v)
            row[n] = int(v, 0) if mm else v.strip('"') if v.startswith('"') else v
        row['_pos'] = k.end() + rm.start()   # (S111 OSTATE: the row's offset in the file, for its fact's file:line)
        out[row['name']] = row
    return out


def builder_mask(o, read):
    """S111 RSTCOVER-B: -> the write-mask values the kind's blend builder assigns (o['blend_builder']: its file, a regex for the
    function, its output variable): every `VAR.write_mask = EXPR` in the function's body, evaluated; None when the kind names
    no builder, the builder is not found, or a value is a rule of ours."""
    b = o.get('blend_builder')
    if not b:
        return None
    try:
        t = CD._mask(read(b['file']))
    except (OSError, OstateError):
        return None
    m = re.search(b['fn'], t)
    k = t.find('{', m.end()) if m else -1
    if k < 0:
        return None
    body = t[k + 1:CD._match(t, k, '{', '}') - 1]
    ctx = Ctx([t], body)
    vals = set()
    for a in re.finditer(r'\b' + re.escape(b['var']) + r'\s*\.\s*write_mask\s*=(?!=)\s*([^,;]+)', body):
        vals |= eval_val(a.group(1), ctx)
    return vals if vals and all(isinstance(v, int) for v in vals) else None


def _blank_strings(t):
    """t with every string / char literal's inside blanked (same length): a brace in a literal is not a scope"""
    return re.sub(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])+\'', lambda m: m.group(0)[0] + ' ' * (len(m.group(0)) - 2) + m.group(0)[-1], t)


def scope_end(t, pos):
    """-> the offset of the `}` closing the brace scope that holds pos (len(t) at file scope)"""
    b = _blank_strings(t)
    d = 0
    for i in range(pos - 1, -1, -1):
        if b[i] == '}':
            d += 1
        elif b[i] == '{':
            if d == 0:
                return CD._match(b, i, '{', '}') - 1
            d -= 1
    return len(t)


def find_block(t, o):
    """-> (the START_RE match, the block's end offset) of one ours block o: after o['after'] (a regex) or at o['from'] (an offset);
    up to o['end'] (a regex), or (S111 OSTATE, o['end'] None) the first `State(X)` that consumes it, else its scope's end."""
    if 'from' in o:
        pos = o['from']
    else:
        a = re.search(o['after'], t) if o.get('after') else None
        if o.get('after') and not a:
            raise OstateError(f"{o['file']}: anchor /{o['after']}/ not found")
        pos = a.end() if a else 0
    sm = re.compile(START_RE).search(t, pos)
    if not sm:
        raise OstateError(f"{o['file']}: no `rdr::DrawState X{{}};` after /{o.get('after')}/")
    if o.get('end'):
        em = re.compile(o['end']).search(t, sm.end())
        if not em:
            raise OstateError(f"{o['file']}: end /{o['end']}/ not found after line {line_of(t, sm.start())}")
        return sm, em.start()
    se = scope_end(t, sm.start())
    um = re.compile(r'\bState\s*\(\s*' + re.escape(sm.group('var')) + r'\s*\)').search(t, sm.end(), se)
    return sm, (um.start() if um else se)


def ours_state(kind, o, read):
    """-> ({field: {'vals': {value: [file:line]}, 'cond': [file:line], 'when': {file:line: cond}}}, start line) for one ours block of a kind."""
    text = read(o['file'])
    t = CD._mask(text)
    sm, end = find_block(t, o)
    var = sm.group('var')
    texts = [t]
    for h in o.get('helpers', []):
        try:
            texts.append(CD._mask(read(h)))
        except MissingAtRef as e:   # S111 CASTCULL: a helper added after an older ref read (the selftest's frozen commits) names nothing there
            if not born_after(e.ref, e.rel):   # (review) the ref's history holds it, or it was never committed: a real missing file
                raise OstateError(f'{o["file"]}: helper {h} is missing at {e.ref}, which is not older than its first commit ({e})') from e
    table = None
    if kind.get('classes'):
        table = parse_table(read(PS))
    ctx = Ctx(texts, t[sm.end():end], o.get('known'), o.get('game_words', ()), table, kind.get('classes', ()))
    st = {f: {'vals': {0: [f"{o['file']}:{line_of(t, sm.start())} ({var}{{}})"]}, 'cond': [], 'when': {}} for f in FIELDS}
    aliases = set(re.findall(r'(?:rdr::BlendState|auto)\s*&\s*(\w+)\s*=\s*' + re.escape(var) + r'\s*\.\s*blend\s*\[\s*0\s*\]\s*;', t[sm.end():end]))
    for pos, stmt, conds in statements(t, sm.end(), end, [], []):
        live = []
        skip = False
        for c, taken in conds:
            v = eval_cond(c, ctx, {})
            if v is None:
                live.append(c if taken else f'!({c})')
            elif v != taken:
                skip = True
        if skip:
            continue
        for part in _depth_split(stmt.rstrip().rstrip(';'), ','):
            paths, expr = assign_targets(CASE_RE.sub('', part, count=1), var, aliases)
            where = f"{o['file']}:{line_of(t, pos + stmt.find(part.strip()[:20]) if part.strip()[:20] in stmt else pos)}"
            for path in paths:
                fields = WHOLE_BLEND if path == 'blend[0]' else (OURS_FIELD[path],) if path in OURS_FIELD else ()
                vals = eval_val(expr, ctx) if fields else set()
                for f in fields:
                    if f == 'cmask' and path == 'blend[0]':   # the whole blend copied: its write mask is the copy's builder's, not a game word
                        vals = builder_mask(o, read) or {f"RULE: {expr.strip()} (the whole blend state)"}   # S111 RSTCOVER-B: the builder, when the kind names it
                    if not live:
                        st[f]['vals'] = {}
                        st[f]['cond'] = []
                        st[f]['when'] = {}
                    else:
                        st[f]['cond'].append(f'{where} when {" && ".join(live)}')
                        st[f]['when'][where] = " && ".join(live)
                    for v in vals:
                        st[f]['vals'].setdefault(v, []).append(where)
    return st, line_of(t, sm.start())


# ------------------------------------------------------------------ the effect files: per-pass render-state rows
def effect_rows(cache_path=None):
    """-> ({'passes': n, 'fill_only': n, 'empty': n, 'rows': [(effect, tech, pass, field, value)]}, None) from every pass's
    render-state block (pass+12: counts at +16 / +20 / +24, 8-byte (offset, value) entries from +28); (None, why) when unread."""
    try:
        sys.path.insert(0, os.path.join(REPO, 'tools', 'shaders'))
        import effect_table as E
        import shaderlib
        key = CD._passes_key(E, shaderlib) + os.path.abspath(__file__)
        if cache_path is None:
            import hashlib
            import qdecomp
            cache_path = os.path.join(os.path.dirname(qdecomp.IMAGE),
                                      f"effect_rstates.{hashlib.sha1(os.path.abspath(REPO).lower().encode()).hexdigest()[:10]}.json")
        try:
            with open(cache_path, encoding='utf-8') as fh:
                c = json.load(fh)
            if c.get('key') == key:
                return c['res'], None
        except (OSError, ValueError, KeyError, TypeError):
            pass
        import pac
        res = {'passes': 0, 'fill_only': 0, 'empty': 0, 'rows': [], 'other': 0}
        for rel in E.SHADER_FILES:
            data = pac.load(os.path.join(shaderlib.GAME, rel.replace('/', os.sep)))
            for _leaf, dec in pac.entries(data):
                table = shaderlib.effect_table(dec) if b'\x10\x2a\x11' in dec else None
                for name, off, size in table or ():
                    blob = dec[off:off + size]
                    body = blob[12:12 + E.u32(blob, 4)]
                    tab, cnt = E.u32(body, E.T_OFF), E.u32(body, E.T_COUNT)
                    for ti in range(cnt if 0 < cnt < 4096 else 0):
                        tt = E.u32(body, tab + 4 * ti)
                        for p in range(E.u32(body, tt + 4) if tt + 8 <= len(body) else 0):
                            rs = E.u32(body, E.u32(body, tt + 16 + 4 * p) + 12)
                            res['passes'] += 1
                            n = sum(E.u32(body, rs + k) for k in (16, 20, 24)) if rs + 28 <= len(body) else 0
                            ents = [(E.u32(body, rs + 28 + 8 * i), E.u32(body, rs + 32 + 8 * i)) for i in range(min(n, 200))]
                            if not ents:
                                res['empty'] += 1
                            elif all(o == 52 for o, _v in ents):
                                res['fill_only'] += 1
                            for o, v in ents:
                                if o in FX_ROWS:
                                    res['rows'].append([name, ti, p, FX_ROWS[o], v])
                                elif o != 52:
                                    res['other'] += 1
        if os.path.isdir(os.path.dirname(cache_path) or '.'):
            tmp = f'{cache_path}.{os.getpid()}.tmp'
            try:
                with open(tmp, 'w', encoding='utf-8') as fh:
                    json.dump({'key': key, 'res': res}, fh)
                os.replace(tmp, cache_path)
            except OSError:
                pass
        return res, None
    except Exception as e:  # noqa: BLE001 - the player's files are an extra source; their absence is said in the sources line
        return None, f'{type(e).__name__}: {e}'


# ================================================================== S111 OSTATE: the facts
def _fact(fn, field, value, cond, at, kind, **extra):
    f = {'side': 'ours', 'fn': fn, 'field': field, 'value': value, 'cond': cond or None, 'at': at, 'kind': kind}
    f.update({k: v for k, v in extra.items() if v is not None})
    return f


def val_of(v):
    """an evaluator value -> the fact value: an int, {'game': ...}, {'rule': EXPR}"""
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, int):
        return v
    if v == GAME:
        return {'game': True}
    if isinstance(v, str) and v.startswith('RULE: '):
        return {'rule': v[6:]}
    return {'rule': str(v)}


def _and(*cs):
    cs = [c for c in cs if c]
    return ' && '.join(cs) if cs else None


def _floats(expr):
    """`7.f` / `7.0f` -> `7` (our dynamic words are floats; the evaluator reads integer literals)"""
    return re.sub(r'(?<![\w.])(\d+)\.0*f?(?![\w.])', r'\1', expr)


def ref_raw(v):
    """a kTable alpha_ref cell (`192.f / 255.f`, `0.f`) or an evaluated int -> the game's raw byte 0..255, else None"""
    if isinstance(v, int):
        return v
    s = str(v).strip()
    m = re.fullmatch(r'(\d+(?:\.\d*)?)f?\s*/\s*255(?:\.0*)?f?', s)
    if m:
        return int(round(float(m.group(1))))
    m = re.fullmatch(r'(\d+(?:\.\d*)?)f?', s)
    if m:
        return int(round(float(m.group(1)) * 255))
    return None


def _ref_val(v):
    r = ref_raw(v)
    return r if r is not None else val_of(v)


# ------------------------------------------------------------------ the source text: files, functions
class TextSrc:
    """qsym.Src over a given text (a file at a git ref): the enclosing names `q assume` gives (qassume_rules.View.scope_name)"""
    def __init__(self, rel, text):
        import qsym
        self.src = qsym.Src(REPO, rel, text)
        self._view = None
        self.offs = [0]
        for m in re.finditer('\n', text):
            self.offs.append(m.end())

    def name(self, ln):
        f = self.src.func_at(ln)
        if f is not None:
            return f['qual'] or f['name']
        if self._view is None:
            import qassume_rules
            self._view = qassume_rules.View(self.src)
        return self._view.scope_name(ln)[0]

    def span(self, ln):
        """-> (start offset, end offset) of the function holding line ln, else None"""
        f = self.src.func_at(ln)
        if f is None:
            return None
        a = self.offs[f['start'] - 1] if f['start'] - 1 < len(self.offs) else 0
        b = self.offs[f['end']] if f['end'] < len(self.offs) else len(self.src.text)
        return a, b


def candidate_files(ref, read, dirs):
    """the .h / .cpp files of dirs that name a DrawState or an InstancedDraw: at ref (git grep), else the working tree"""
    pat = r'rdr::DrawState|InstancedDraw'
    if ref:
        if ref.startswith('-'):
            raise OstateError(f'--ref {ref!r}: a REF starting with - would reach git as an option')
        r = subprocess.run(['git', 'grep', '-l', '-E', pat, ref, '--'] + list(dirs), cwd=REPO, capture_output=True, text=True,
                           encoding='utf-8', errors='replace')
        if r.returncode not in (0, 1):
            raise OstateError(f'git grep at {ref}: {r.stderr.strip()[:200]}')
        out = [ln.split(':', 1)[1] for ln in r.stdout.splitlines() if ':' in ln]
    else:
        out = []
        for d in dirs:
            for p in sorted(glob.glob(os.path.join(REPO, d.replace('/', os.sep), '*.*'))):
                rel = os.path.relpath(p, REPO).replace(os.sep, '/')
                try:
                    if re.search(pat, read(rel)):
                        out.append(rel)
                except (OSError, OstateError):
                    continue
    return sorted(r for r in out if r.endswith(('.cpp', '.h')))


# ------------------------------------------------------------------ (a) the pass_state.h kTable rows
def class_names(ps_text):
    """-> (enum names in order, {enum name: row name}, [the row names ClassOf can return])"""
    t = CD._mask(ps_text)
    rows = list(parse_table(ps_text))
    em = re.search(r'enum\s+class\s+PassClass\b[^{]*\{([^}]*)\}', t)
    enums = [x.strip() for x in em.group(1).split(',') if x.strip() and x.strip() != 'kCount'] if em else []
    by_enum = dict(zip(enums, rows))
    cm = re.search(r'\bClassOf\s*\([^)]*\)\s*\{', t)
    ret = []
    if cm:
        body = t[cm.end():CD._match(t, cm.end() - 1, '{', '}') - 1]
        for r in re.findall(r'return\s+PassClass::(\w+)\s*;', body):
            if r in by_enum and by_enum[r] not in ret:
                ret.append(by_enum[r])
    return enums, by_enum, ret


def live_rows(ps_text, texts):
    """S111 PARTZ2: -> {row name: why} of the kTable rows NO draw can read: ClassOf never returns the row's class and no text names
    `PassClass::<it>` (texts: our src files' text). A row of those is compared nowhere (q codediff: moot)."""
    enums, by_enum, ret = class_names(ps_text)
    named = set()
    for t in texts:
        named |= set(re.findall(r'\bPassClass::(\w+)', CD._mask(t)))
    return {by_enum[e]: f'PassClass::{e}: ClassOf never returns it and no file of ours names it' for e in enums
            if e in by_enum and by_enum[e] not in ret and e not in named}


def src_texts(ref, read, files):
    """the texts live_rows scans: every src file naming PassClass:: (git grep, at ref or the working tree); files = a planted list"""
    if files is not None:
        out = []
        for rel in list(files) + [PS]:
            try:
                out.append(read(rel))
            except (OSError, OstateError):
                pass
        return out
    r = subprocess.run(['git', 'grep', '-h', '-o', '-E', r'PassClass::\w+'] + ([ref] if ref else []) + ['--', 'src'], cwd=REPO,
                       capture_output=True, text=True, encoding='utf-8', errors='replace')
    return [r.stdout]


def table_facts(read, dead=None):
    """(a) one fact per (kTable row, field), keyed pass_state::kTable[NAME], with the row's addr cell and the game functions it names;
    dead = live_rows' {row: why}: such a row's facts carry `dead` (no draw reads it)"""
    dead = dead or {}
    text = read(PS)
    t = CD._mask(text)
    table = parse_table(text)
    ctx = Ctx([t], '')
    out = []
    for name, row in table.items():
        at = f'{PS}:{line_of(t, row["_pos"])}'
        addr = str(row.get('addr', ''))
        # the cell names a game function as 0x8XXXXXXX or sub_8XXXXXXX (S111 PAIRCOV: see_through_cut's sub_ form was never paired)
        game = list(dict.fromkeys('sub_' + a.upper() for a in re.findall(r'\b(?:0x|sub_)([0-9A-Fa-f]{8})\b', addr)))
        common = {'addr': addr, 'game': game, **({'dead': dead[name]} if name in dead else {})}
        fn = f'pass_state::kTable[{name}]'
        for col, field in TABLE_FIELD.items():
            if col not in row:
                continue
            v = row[col]
            if col == 'alpha_ref':
                out.append(_fact(fn, field, _ref_val(v), None, at, 'table', unit='0..255', src=str(v), **common))
            elif col == 'game_z_func':   # the game's reversed compare -> ours, through pass_state.h's own ForwardZ (rstdiff's mirror)
                fz = eval_val(f'ForwardZ({v})', ctx)
                fv = next(iter(fz)) if len(fz) == 1 else None
                out.append(_fact(fn, field, val_of(fv) if fv is not None else {'rule': f'ForwardZ({v})'}, None, at, 'table',
                                 unit='vk compare, forward', game_vk=v, **common))
            elif col in ('z_write', 'cull') and v == -1:
                out.append(_fact(fn, field, {'rule': 'decided by the draw' if col == 'z_write' else "the mesh's own (the file's winding rule)"},
                                 None, at, 'table', src='-1', **common))
            elif col == 'blend_code' and v == 0xFF:
                out.append(_fact(fn, field, {'rule': 'from the emitter / sprite'}, None, at, 'table', src='0xFF', **common))
            else:
                unit = {'alpha_test_func': 'vk compare', 'cull': 'vk cull mode', 'blend_code': 'game recorder code'}.get(col)
                out.append(_fact(fn, field, val_of(v), None, at, 'table', unit=unit, **common))
    return out


# ------------------------------------------------------------------ (b) the alpha test of a block
def eval_bool(c, ctx, inst_vars, depth=0):
    """-> [(value, cond)]: value 0 / 1, {'material': EXPR} (Material::alpha_test / the bind's copy), {'caller': 'InstancedDraw::alpha_test',
    'expr': EXPR} or {'rule': EXPR}; cond the ternary arms taken. A local is read through when its terms reach a source, else named."""
    c = strip_parens(re.sub(r'\s+', ' ', c))
    if depth > 8:
        return [({'rule': c}, None)]
    t = split_ternary(c)
    if t:
        cc, a, b = t
        cv = eval_cond(cc, ctx, {})
        if cv is True:
            return eval_bool(a, ctx, inst_vars, depth + 1)
        if cv is False:
            return eval_bool(b, ctx, inst_vars, depth + 1)
        return ([(v, _and(cc, k)) for v, k in eval_bool(a, ctx, inst_vars, depth + 1)]
                + [(v, _and(f'!({cc})', k)) for v, k in eval_bool(b, ctx, inst_vars, depth + 1)])
    for sep, absorb, unit in (('||', 1, 0), ('&&', 0, 1)):
        parts = _depth_split(c, sep)
        if len(parts) > 1:
            out = []
            for x in parts:
                got = eval_bool(x, ctx, inst_vars, depth + 1)
                if got == [(absorb, None)]:
                    return [(absorb, None)]
                out += [g for g in got if g != (unit, None)]
            return out or [(unit, None)]
    v = eval_cond(c, ctx, {})
    if v is not None:
        return [(int(v), None)]
    m = SOURCE_RE.search(c)
    if m:
        if m.group(1) in inst_vars:
            return [({'caller': 'InstancedDraw::alpha_test', 'expr': c}, None)]
        return [({'material': c}, None)]
    if re.fullmatch(r'\w+', c):
        loc = ctx.local(c)
        if loc is not None:
            got = eval_bool(loc, ctx, inst_vars, depth + 1)
            if all(isinstance(g, int) for g, _k in got) or any(isinstance(g, dict) and ('material' in g or 'caller' in g) for g, _k in got):
                return got
    return [({'rule': c}, None)]


def _last_word(text, var, idx):
    """the last `VAR.system[idx] = EXPR` in text -> EXPR, else None"""
    m = None
    for m in re.finditer(r'\b' + re.escape(var) + r'\s*\.\s*system\s*\[\s*' + str(idx) + r'\s*\]\s*=(?!=)\s*([^,;]+)', text):
        pass
    return m.group(1) if m else None


def _alpha_body(texts):
    """AlphaDynamic's `d.system[8] = X; d.system[9] = Y;` -> (ref expr, func expr), else (None, None)"""
    for t in texts:
        m = re.search(r'\bAlphaDynamic\s*\([^)]*\)\s*\{', t)
        if m:
            body = t[m.end():CD._match(t, m.end() - 1, '{', '}') - 1]
            vm = re.search(r'\b(\w+)\s+=\s+base\s*;', body)
            var = vm.group(1) if vm else 'd'
            return _last_word(body, var, 8), _last_word(body, var, 9)
    return None, None


def alpha_facts(fn, rel, t, block_start, block_end, fspan, ctx_texts, table, classes, inst_vars, game_words=()):
    """(b) -> [fact] for one block's draw: the first `Dynamic(EXPR)` after the block (before the next block of the function), else the
    last one before the block in the function (a dynamic state pushed once for the draws that follow it)"""
    lo, hi = fspan
    nxt = re.compile(START_RE).search(t, block_end, hi)
    stop = nxt.start() if nxt else hi
    dyn_re = re.compile(r'\bDynamic\s*\(')
    dm = dyn_re.search(t, block_end, stop)
    if not dm:
        before = list(dyn_re.finditer(t, lo, block_start))
        dm = before[-1] if before else None
    if not dm:   # the draw's dynamic state is pushed by a caller (an id parameter): said, not read across the call
        return [_fact(fn, 'alpha_test', {'rule': 'no Dynamic() in the function: the dynamic state is its caller\'s'}, None,
                      f'{rel}:{line_of(t, block_start)}', 'alpha')]
    k = CD._match(t, dm.end() - 1, '(', ')')
    expr = t[dm.end():k - 1].strip()
    at = f'{rel}:{line_of(t, dm.start())}'
    fctx = Ctx(ctx_texts, t[lo:dm.start()], None, game_words, table, classes)
    out = []
    tern = split_ternary(strip_parens(re.sub(r'\s+', ' ', expr)))
    if tern and (ALPHA_CALL_RE.search(tern[1]) or ALPHA_CALL_RE.search(tern[2])):
        cc, a, b = tern
        tested_first = bool(ALPHA_CALL_RE.search(a))
        on_cond, off_cond = (cc, f'!({cc})') if tested_first else (f'!({cc})', cc)
        for v, kc in (eval_bool(on_cond, fctx, inst_vars)):
            out.append(_fact(fn, 'alpha_test', v, kc, at, 'alpha', src=on_cond))
        ref_e, func_e = _alpha_body(ctx_texts)
        for field, e, conv in (('alpha_func', func_e, val_of), ('alpha_ref', ref_e, _ref_val)):
            if e is None:
                out.append(_fact(fn, field, {'rule': 'AlphaDynamic (its body not read)'}, on_cond, at, 'alpha'))
                continue
            for v in sorted(eval_val(_floats(e), fctx) if field == 'alpha_func' else eval_val(e, fctx), key=str):
                out.append(_fact(fn, field, conv(v), on_cond, at, 'alpha', src='AlphaDynamic: ' + e.strip(),
                                 unit='vk compare' if field == 'alpha_func' else '0..255'))
        base = (b if tested_first else a).strip()
        fe = _last_word(t[lo:dm.start()], base, 9) if re.fullmatch(r'\w+', base) else None
        if fe is not None:
            for v in sorted(eval_val(_floats(fe), fctx), key=str):
                out.append(_fact(fn, 'alpha_func', val_of(v), off_cond, at, 'alpha', src=f'{base}.system[9] = {fe.strip()}', unit='vk compare'))
        return out
    base = strip_parens(expr)
    if not re.fullmatch(r'\w+', base):
        return [_fact(fn, 'alpha_test', {'rule': expr}, None, at, 'alpha')]
    fe, re_ = _last_word(t[lo:dm.start()], base, 9), _last_word(t[lo:dm.start()], base, 8)
    if fe is None:   # a parameter: the caller's DynamicState; else its zero (function 0): either way said, not guessed
        head = t[lo:t.find('{', lo, dm.start()) if t.find('{', lo, dm.start()) >= 0 else dm.start()]
        why = (f'{base} is a parameter: the caller\'s DynamicState' if re.search(r'\bDynamicState\s*&\s*' + re.escape(base) + r'\b', head)
               else f'{base}.system[9] not set in the function (the DynamicState zero)')
        return [_fact(fn, 'alpha_test', {'rule': why}, None, at, 'alpha')]
    for v in sorted(eval_val(_floats(fe), fctx), key=str):
        fv = val_of(v)
        out.append(_fact(fn, 'alpha_func', fv, None, at, 'alpha', src=f'{base}.system[9] = {fe.strip()}', unit='vk compare'))
        out.append(_fact(fn, 'alpha_test', (0 if fv == 7 else 1) if isinstance(fv, int) else fv, None, at, 'alpha',
                         src=f'{base}.system[9] = {fe.strip()} (7 = always: no test)'))
    if re_ is not None:
        for v in sorted(eval_val(_floats(re_), fctx), key=str):
            out.append(_fact(fn, 'alpha_ref', _ref_val(v), None, at, 'alpha', src=f'{base}.system[8] = {re_.strip()}', unit='0..255'))
    return out


# ------------------------------------------------------------------ (c) the InstancedDraw stores of the callers
INST_DECL_RE = re.compile(r'\bInstancedDraw\s*&?\s*(\w+)\s*(?:[;={),]|$)')


def inst_vars_of(text):
    return set(INST_DECL_RE.findall(text))


INST_DECL_FULL = re.compile(r'\bInstancedDraw\s*(?:&\s*)?(\w+)\s*(?:=\s*([^;{}]+?))?\s*;')   # a local (or member) declaration, its initialiser
DRAW_CALL = r'\bDrawInstanced\s*\(\s*{var}\b'
LEFT = 'Material::alpha_test (the caller leaves InstancedDraw::alpha_test at -1: the draw follows each material\'s own flag)'
PLANT = os.environ.get('WWE_QOSTATE_PLANT', '')   # selftest only: 'one_unit' every store keyed by its function (no declaration)


def caller_facts(rel, t, src):
    """(c) -> [fact] per `VAR.alpha_test|no_blend = EXPR` (VAR an InstancedDraw of the function), with its if / for conditions.
    S111 CDFOLLOW (CODEDIFF review finding 3): each store is keyed by its VARIABLE's declaration (`decl` = file:line of the nearest
    `InstancedDraw VAR` above it in the function): one unit per draw, compared on the fields it states. A declared VAR that reaches
    `DrawInstanced(VAR` with no alpha_test store between its declaration and the call states the -1 (the material's flag): a fact
    `left` at the call, so the unit is reported, not skipped. `calls` names the draw function the unit reaches."""
    out = []
    walked = {}

    def info(sp):
        if sp not in walked:
            ftext = t[sp[0]:sp[1]]
            body = ftext.find('{')
            stmts = statements(t, sp[0] + body + 1, sp[1] - 1, [], []) if body >= 0 else []
            decls = [(sp[0] + d.start(), d.group(1), (d.group(2) or '').strip()) for d in INST_DECL_FULL.finditer(ftext)]
            walked[sp] = (ftext, stmts, decls)
        return walked[sp]

    def cond_at(stmts, pos):
        for p, stmt, conds in stmts:
            if p <= pos < p + len(stmt):
                return _and(*[c if taken else f'!({c})' for c, taken in conds])
        return None

    def unit_of(decls, var, pos, end):
        """-> (decl offset, initialiser, unit end) of the declaration of var that holds pos, else None"""
        mine = [d for d in decls if d[1] == var]
        before = [d for d in mine if d[0] < pos]
        if not before or PLANT == 'one_unit':
            return None
        d = before[-1]
        nxt = [x[0] for x in mine if x[0] > d[0]]
        return d[0], d[2], (nxt[0] if nxt else end)

    hits = list(re.finditer(r'\b(\w+)\s*\.\s*(' + '|'.join(INST_FIELDS) + r')\s*=(?!=)\s*([^,;]+)', t))
    stated = set()   # (decl offset, var) of the units with an alpha_test store
    for m in hits:
        ln = line_of(t, m.start())
        sp = src.span(ln)
        if sp is None:
            continue
        ftext, stmts, decls = info(sp)
        if m.group(1) not in inst_vars_of(ftext):
            continue
        cond = cond_at(stmts, m.start())
        ctx = Ctx([t], ftext)
        name, field, expr = src.name(ln), m.group(2), m.group(3).strip()
        at = f'{rel}:{ln}'
        store = f'InstancedDraw::{field}'
        u = unit_of(decls, m.group(1), m.start(), sp[1])
        keyed = {}
        if u:
            keyed['decl'] = f'{rel}:{line_of(t, u[0])}'
            if re.search(DRAW_CALL.format(var=re.escape(m.group(1))), t[u[0]:u[2]]):
                keyed['calls'] = 'DrawInstanced'
            if field == 'alpha_test':
                stated.add((u[0], m.group(1)))
        for v in sorted(eval_val(expr, ctx), key=str):
            fv = val_of(v)
            if field == 'alpha_test':
                val = {'material': LEFT} if fv == -1 else fv
                out.append(_fact(name, 'alpha_test', val, cond, at, 'caller', store=store, src=expr, left=True if fv == -1 else None, **keyed))
            else:   # S103 P7: the tick-blend tag, not the blend state
                out.append(_fact(name, 'no_blend', fv, cond, at, 'caller', store=store, src=expr, what='never tagged for the tick blend (not the blend state)', **keyed))
    for d in INST_DECL_FULL.finditer(t):   # the units that reach a draw with alpha_test left at -1
        ln = line_of(t, d.start())
        sp = src.span(ln)
        if sp is None or PLANT == 'one_unit' or (d.start(), d.group(1)) in stated:
            continue
        _ftext, stmts, decls = info(sp)
        u = unit_of(decls, d.group(1), d.start() + 1, sp[1])
        if not u or u[0] != d.start():
            continue
        init = u[1]
        for c in re.finditer(DRAW_CALL.format(var=re.escape(d.group(1))), t[u[0]:u[2]]):
            pos = u[0] + c.start()
            copied = init and not re.fullmatch(r'InstancedDraw\s*\{\s*\}', init)
            val = {'material': LEFT + (f' (a copy of {init}, which no store of the function sets)' if copied else '')}
            if copied and re.search(r'\b' + re.escape(init) + r'\s*\.\s*alpha_test\s*=(?!=)', t[sp[0]:u[0]]):
                val = {'rule': f'InstancedDraw::alpha_test copied from {init} (its own store, above)'}
            out.append(_fact(src.name(ln), 'alpha_test', val, cond_at(stmts, pos), f'{rel}:{line_of(t, pos)}', 'caller',
                             store='InstancedDraw::alpha_test', src='not stated (-1)', left=True, decl=f'{rel}:{ln}', calls='DrawInstanced'))
    return out


# ------------------------------------------------------------------ the blocks
def _kinds_blocks():
    """the ours blocks of qrstdiff's KINDS: [(o, [kind name (ctx)])], one o per (file, after, end, known)"""
    import qrstdiff
    seen = {}
    for k in qrstdiff.KINDS:
        for o in k['ours']:
            key = (o['file'], o.get('after'), o.get('end'), json.dumps(o.get('known') or {}, sort_keys=True, default=str))
            label = k['kind'] + (f" ({o['ctx']})" if o.get('ctx') else '')
            if key in seen:
                seen[key][1].append(label)
            else:
                seen[key] = (o, [label])
    return list(seen.values()), qrstdiff.COVER_DIRS


def block_facts(o, kinds, read, cache, ps_info):
    """-> (facts, start line) for one block: the DrawState fields (ours_state) and the alpha test, per pass class when it reads one"""
    rel = o['file']
    if rel not in cache:
        text = read(rel)
        cache[rel] = (text, CD._mask(text), TextSrc(rel, text))
    text, t, src = cache[rel]
    sm, end = find_block(t, o)
    start = line_of(t, sm.start())
    fn = src.name(start)
    fspan = src.span(start) or (0, len(t))
    table, enums_ret = ps_info
    region = t[sm.end():end]
    dm = re.compile(r'\bDynamic\s*\(').search(t, end, fspan[1])
    alpha_text = t[end:dm.end() + 200] if dm else ''
    per_class = bool(table) and bool(re.search(r'\bOf\s*\(', region) or ALPHA_CALL_RE.search(alpha_text))
    classes = enums_ret if per_class else [None]
    texts = [t]
    for h in o.get('helpers', []) + ([PS] if PS not in o.get('helpers', []) else []):
        try:
            texts.append(CD._mask(read(h)))
        except (OSError, OstateError):
            pass
    inst = inst_vars_of(t[fspan[0]:fspan[1]])
    merged = {}
    for c in classes:
        kind = {'classes': [c]} if c else {}
        oo = dict(o, helpers=list(dict.fromkeys(o.get('helpers', []) + [PS])))
        st, _s = ours_state(kind, oo, read)
        got = []
        for f in FIELDS:
            for v, ws in st[f]['vals'].items():
                for w in sorted(set(ws)):
                    at = w.split(' ')[0]
                    got.append(_fact(fn, FIELD_KEY[f], val_of(v), st[f]['when'].get(w), at, 'default' if w.endswith('{})') else 'set'))
        got += alpha_facts(fn, rel, t, sm.start(), end, fspan, texts, table if c else None, [c] if c else (), inst, o.get('game_words', ()))
        for g in got:
            key = json.dumps(g, sort_keys=True, default=str)
            merged.setdefault(key, (g, []))[1].append(c)
    out = []
    for g, cs in merged.values():
        g = dict(g)
        if cs != classes and cs != [None]:
            g['cond'] = _and(g['cond'], f"pass class {'|'.join(cs)}")
            g['classes'] = cs
        if o.get('known'):   # one block read under a kind's known conditions (the 2D row in the scene / on the final target)
            g['cond'] = _and(g['cond'], f"[{o.get('ctx') or json.dumps(o['known'], default=str)}]")
        g['block'] = f'{rel}:{start}'
        if kinds:
            g['kinds'] = kinds
        out.append(g)
    return out, start


def collect(ref=None, read=None, files=None):
    """-> (facts, problems, counts) for the tree at ref (None = the working tree); read / files override the reader and the file list"""
    planted = files   # a planted file list (the selftest's dict reader): live_rows scans those, not git
    read = read or default_reader(ref)
    kblocks, dirs = _kinds_blocks()
    given = files   # S111 OSTATEWIDE: the caller's own list (None: the wide reader walks its own dirs)
    files = files if files is not None else candidate_files(ref, read, dirs)
    facts, problems = [], []
    cache = {}
    try:
        ps_text = read(PS)
        table = parse_table(ps_text)
        ps_info = (table, class_names(ps_text)[2])
        facts += table_facts(read, live_rows(ps_text, src_texts(ref, read, planted)))
    except (OSError, OstateError) as e:
        problems.append(f'{PS}: {e}')
        ps_info = ({}, [])
    covered = set()
    n_blocks = 0
    for o, kinds in kblocks:
        if o['file'] not in files:
            continue
        try:
            got, start = block_facts(o, kinds, read, cache, ps_info)
        except (OSError, OstateError, CD.DiffError) as e:
            problems.append(f"{', '.join(kinds)}: {e}")
            continue
        if (o['file'], start) in covered and not o.get('known'):
            continue
        covered.add((o['file'], start))
        facts += got
        n_blocks += 1
    n_stores = 0
    for rel in files:
        try:
            if rel not in cache:
                text = read(rel)
                cache[rel] = (text, CD._mask(text), TextSrc(rel, text))
        except (OSError, OstateError) as e:
            problems.append(f'{rel}: {e}')
            continue
        _text, t, src = cache[rel]
        for m in re.finditer(START_RE, t):
            if (rel, line_of(t, m.start())) in covered:
                continue
            try:
                got, start = block_facts({'file': rel, 'from': m.start(), 'end': None}, [], read, cache, ps_info)
            except (OSError, OstateError, CD.DiffError) as e:
                problems.append(f'{rel}:{line_of(t, m.start())}: {e}')
                continue
            covered.add((rel, start))
            facts += got
            n_blocks += 1
        got = caller_facts(rel, t, src)
        n_stores += len(got)
        facts += got
    import qostate_wide as W   # S111 OSTATEWIDE: copy blocks, DrawState stores by reference / pointer, the bind records (tools/q/qostate_wide.py)
    got, wp, wc = W.collect(ref, read, given, cache, facts)
    return facts + got, problems + wp, {'blocks': n_blocks, 'stores': n_stores, 'files': len(files), **wc}


# ------------------------------------------------------------------ the entry
ALIASES = {'atest': 'alpha_test', 'afunc': 'alpha_func', 'aref': 'alpha_ref', 'ztest': 'z_enable', 'zwrite': 'z_write', 'zfunc': 'z_func',
           'blend': 'blend_enable', 'cmask': 'colour_write_mask', 'stencil': 'stencil_enable'}


def select(facts, fn=None, field=None):
    want = ALIASES.get(field, field) if field else None
    out = []
    for f in facts:
        if want and f['field'] != want:
            continue
        if fn and not (fn == f['fn'] or fnmatch.fnmatchcase(f['fn'], fn) or f['fn'].endswith('::' + fn) or fn in f.get('game', [])):
            continue
        out.append(f)
    return out


def value_text(v):
    if isinstance(v, dict):
        for k in ('material', 'caller', 'game', 'rule'):
            if k in v:
                return f'{k} {v[k]}' if v[k] is not True else k
        if 'restore' in v:
            return f'restore {v["restore"]}'
    return str(v)


def fact_line(f):
    extra = []
    if f.get('store'):
        extra.append(f['store'])
    if f.get('unit'):
        extra.append(f['unit'])
    if f.get('game'):
        extra.append('game ' + ', '.join(f['game']))
    extra += [f"{k} {f[k] if isinstance(f[k], str) else '; '.join(f[k])}" for k in ('base', 'base_src', 'bind') if f.get(k)]   # S111 OSTATEWIDE
    if f.get('arm'):   # S111 CDARMS (tools/q/qcdarms.py): a bind store's if-arm and its comment's scope words
        extra.append(f"arm {f['arm']}" + (f"; scope: the {f['scope']} pass only @{f['scope_at']}" if f.get('scope') else ''))
    cond = f'  when {f["cond"]}' if f.get('cond') else ''
    return f"  {f['field']} = {value_text(f['value'])}  @{f['at']}  ({f['kind']}{'; ' + '; '.join(extra) if extra else ''}){cond}"


def main(argv):
    ap = argparse.ArgumentParser(prog='q ostate', description=__doc__.split('\n')[0])
    ap.add_argument('--fn', help="one of our functions (qualified name, a glob, the last name part) or a game sub_X a kTable row names")
    ap.add_argument('--field', help="a fact field (gstate's key: alpha_test, z_func, ...) or rstdiff's name (atest, afunc, aref, ztest, zfunc, ...)")
    ap.add_argument('--ref', help='read our code at this git ref (git show) instead of the working tree')
    ap.add_argument('--json', action='store_true', help='the facts, the problems and the counts as JSON')
    ap.add_argument('--facts', action='store_true', help="the code-diff fact form only (q gstate --facts' document shape)")
    a = ap.parse_args(argv)
    facts, problems, counts = collect(a.ref)
    sel = select(facts, a.fn, a.field)
    where = a.ref or 'the working tree'
    if a.facts:
        print(json.dumps({'ref': where, 'facts': sel}, indent=1, default=str))
        return 0
    if a.json:
        print(json.dumps({'ref': where, 'facts': sel, 'problems': problems, 'counts': counts}, indent=1, default=str))
        return 2 if problems else 0
    fns = list(dict.fromkeys(f['fn'] for f in sel))
    print(f"q ostate: {len(sel)} facts (of {len(facts)}) in {len(fns)} functions at {where}; {counts['blocks']} DrawState blocks, "
          f"{counts['stores']} InstancedDraw stores, {counts['files']} files"
          + (f"; --fn {a.fn}" if a.fn else '') + (f"; --field {a.field}" if a.field else ''))
    for name in fns:
        rows = [f for f in sel if f['fn'] == name]
        blocks = sorted({f['block'] for f in rows if f.get('block')})
        kinds = sorted({k for f in rows for k in f.get('kinds', [])})
        print(f"== {name}" + (f"  (block {', '.join(blocks)})" if blocks else '') + (f"  [rstdiff {', '.join(kinds)}]" if kinds else ''))
        for f in rows:
            print(fact_line(f))
    for p in problems:
        print('PROBLEM: ' + p)
    return 2 if problems else 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv[1:]))
    except OstateError as e:
        print(f'q ostate: {e}', file=sys.stderr)
        sys.exit(2)
