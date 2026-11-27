r"""q constdiff --byreg: the shader inputs OUR code feeds BY REGISTER, beside the game's writer of that register (S111 BYREG).

    py tools/q/q.py constdiff --byreg [--effect E] [--diff] [--partial] [--json]

`q constdiff` compares the effects our by-name resolvers feed; this compares the rest: the draws OUR code builds itself and fills
by register number. One row per (effect, stage, register) of the fx registry that an ours site below feeds (variants merged, as
q constdiff). Flags: DIFF (the game writes the register from code or a material and ours writes a literal in every lane the game
writes, zero included; or a lane the game fills from an image constant gets another literal), PARTIAL (some of those lanes literal,
or a literal arm beside a fed one), CONST (ours' literals equal the game's image constants), OK, PASS-NEVER (the model draw never
picks a pass that reads it, as q constdiff), UNSETTLED (no writer of that register found on the effect's code path: not compared).

Sources of truth (nothing guessed, nothing committed from the game):
  ours, three families, each parsed from the code on every call (a site the parser cannot read is a PROBLEM, exit 2):
    live_maps  - every `md::PutRegisters(block, VAR, REG, DATA, N)` in src/renderer_scene/live_map_*.cpp. VAR is bound to the variants
                 its `kVariants[...]` index names (through `X = md::FindVariant("name")` of the same file, and a local `int` chosen by
                 a ?:); unbound (a lambda's parameter, a function's argument) = every FindVariant name of the file. PutRegisters writes
                 only a range that STARTS at REG (live_maps_internal.h), min(N, its count) vec4s. A variant some site writes keeps
                 zero in every other register (each block is made zero-filled before the puts).
    post       - PostPass::Record (src/renderer_scene/post_pass.cpp): each `Regs X;` is a zeroed pixel bank (Regs() memset, the struct
                 at the top of the file); `X.Set(REG, D)` / `X.SetN(REG, D, N)` fill it (a REG that is a for loop's variable takes the
                 loop's range); `pass(..., VS, PS, X, ...)` draws the variant(s) the last `need(NAME, PS)` named, copying every range the
                 variant declares from the bank: an unset register is zero for that pass. A NAME that is not a string literal is
                 followed to its string literals (locals and the file's functions it is built from): the candidates are the registry
                 variants whose name is the literal effect prefix plus words all found in those literals.
    particles  - LiveParticles::Record (src/renderer_scene/live_particles.cpp): the float arrays PARTICLE_BLOCKS names, each a whole
                 constant block of one variant (checked: the array's size equals the variant's ranges packed in fx.json order, 16 bytes
                 a vec4, and the array is pushed with PushConstants); writes `A[i] = v`, `std::memcpy(A + i, src, bytes)`, aliases
                 `float* p = A + i`, loop variables expanded; an unwritten float is the array's zero.
    A data argument is LITERAL lane by lane when it names a local array initialised with numbers (`const float k[4] = {1.f, ...}`),
    a literal number, or the zero fill; anything else (a context field, a computed local) is OURS.
  game, per effect: the effect's CODE PATH = the functions that load the effect's name string (q index strrefs: a string equal to the
    effect's name, case folded), the slots of every vtable those functions store (fields index vstores) or build the address of
    (a lis + addi immediate where the index's table catalog starts a table, read to the catalog's length; a stored one to its first
    non-function word), and their callees to CALL_DEPTH calls (the constant setters themselves left out). The writers are
    the `q const` literal-register setter calls on that path (qconst scan cache) that cover the register in the stage (a vs+ps setter
    counts for both; a setter of one lane writes that lane only) AND start at a register where one of the effect's variants of that
    stage starts a range (the game writes a constant at its declared register: a write starting elsewhere is another shader's
    layout on the same path, e.g. the HDR chain's `r4 = 16 count 16` beside the combine's c20; the row counts them as left out).
    A lane whose data is an image word (`([0x8218B8CC], ...)`) is GAME-CONST (the value read from the image, every call); any other
    data is GAME-CODE. A lane ours computes against a GAME-CONST lane is not compared (as q constdiff) and the row says so (NOTE).
    An effect whose name string is loaded by model create (sub_82BDB460) is drawn by the model draw, which binds by NAME: its game
    source is q constdiff's own (the name table, descriptors, material records, GAME-CONST chains) and PASS-NEVER applies.
Limits: docs/research/s111/BYREG.md.
"""
import fnmatch
import glob
import itertools
import json
import math
import os
import re
import sqlite3
import struct
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qconstdiff as Q   # noqa: E402

LIVE_MAPS_GLOB = 'src/renderer_scene/live_map_*.cpp'
POST = ('src/renderer_scene/post_pass.cpp', r'^bool PostPass::Record\(')
PARTICLES = ('src/renderer_scene/live_particles.cpp', r'^void LiveParticles::Record\(')
# the particle constant arrays -> the variant whose whole block each one is (checked against the text and the registry on every call)
PARTICLE_BLOCKS = {'block': 'yParticle_base_vertex', 'sb': 'ySoftParticle_base_vertex', 'fsb': 'ySoftParticle_base_fragment'}
CALL_DEPTH = 3   # review (S111 BYREG): the combine's c20 / c21 / c27 / c29 / c30 writers sit at depth 3 (driver sub_82C1E4C0 -> sub_82C1E1D0 -> sub_82C1C768 -> sub_82C1C1A0 / sub_82C1B668)
RANK = {'GAME': 3, 'OURS': 2, 'LITERAL': 1}
WHAT_LANES = {'x': (0,), 'y': (1,), 'z': (2,), 'w': (3,)}
FLAGS = ('DIFF', 'PARTIAL', 'CONST', 'OK', 'PASS-NEVER', 'UNSETTLED')
UNSEEN = 'unseen'        # hit_values: a lane of a scalar-store write that no store fills


class RegError(Exception):
    pass


# ------------------------------------------------------------------ the fx registry, per variant
def load_variants(fx_dir=None):
    """-> {'<effect>_<variant>_<vertex|fragment>': {'effect', 'stage', 'consts': [(field, reg, count)]}} in fx.json order."""
    out = {}
    for p in sorted(glob.glob(os.path.join(fx_dir or Q.FX_DIR, '*.fx.json'))):
        with open(p, encoding='utf-8') as fh:
            d = json.load(fh)
        eff = d.get('effect') or os.path.basename(p).split('.')[0]
        for stage, sn in (('vertex', 'vs'), ('fragment', 'ps')):
            for v in d.get(stage, []):
                out[f"{eff}_{v.get('name')}_{stage}"] = {'effect': eff, 'stage': sn,
                                                        'consts': [(c['field'], int(c['register']), int(c.get('count', 1))) for c in v.get('constants', [])]}
    return out


def float_slots(var):
    """-> [(register, lane)] of a variant's packed block, float by float (ranges in fx.json order, 16 bytes a vec4)."""
    return [(reg + k, i) for _f, reg, cnt in var['consts'] for k in range(cnt) for i in range(4)]


# ------------------------------------------------------------------ text helpers
def line_of(ms, at):
    return ms.count('\n', 0, at) + 1


def func_body(text, func_re, path):
    """-> (masked text, body start, body end) of the function opening at func_re."""
    m = re.search(func_re, text, re.M)
    if not m:
        raise RegError(f'{path}: no line matches {func_re!r}')
    ms = Q._mask(text)
    ob = ms.index('{', m.end())
    return ms, ob, Q._match(ms, ob, '{', '}')


def call_args(ms, open_at):
    """the arguments of the call whose '(' is at ms[open_at]."""
    e = Q._match(ms, open_at, '(', ')')
    return [a.strip() for a in Q._split_args(ms[open_at + 1:e - 1])]


def _elements(inner):
    """the scalar elements of a brace initialiser (nested braces flattened)."""
    flat = inner.replace('{', ' ').replace('}', ' ')
    return [e for e in Q._split_args(flat) if e.strip()]


def array_init(ms, name, at, lo=0):
    """-> [(kind, value)] per float of the last declaration `NAME[..] = {...}` above `at` (zeros for `{}`), or None when the last
    one above has no initialiser (`float NAME[4];`, filled by code) or none is found."""
    last = None
    for m in re.finditer(r'(?<![\w.>])' + re.escape(name) + r'\s*((?:\[[^\]]*\]\s*)+)(=\s*\{|;|,|\))', ms[lo:at]):
        last = m
    if last is None or not last.group(2).startswith('='):
        return None
    ob = lo + last.end() - 1
    cb = Q._match(ms, ob, '{', '}')
    head = ms[max(ms.rfind(';', 0, lo + last.start()), ms.rfind('{', 0, lo + last.start()), ms.rfind('}', 0, lo + last.start())) + 1:lo + last.start()]
    if not re.search(r'\b(?:const|constexpr)\b', head) and _written_after(ms, name, cb, at):
        return None   # filled by code after its initialiser (a memcpy into it, an element store, passed to a function)
    els = _elements(ms[ob + 1:cb - 1])
    if not els:
        dims = [Q.lit_value(d) for d in re.findall(r'\[([^\]]*)\]', last.group(1))]
        n = math.prod(int(d) for d in dims) if all(d is not None for d in dims) else 64
        return [('LITERAL', 0.0)] * n
    out = []
    for e in els:
        v = Q.lit_value(e)
        out.append(('LITERAL', v) if v is not None else ('OURS', None))
    return out


READ_ONLY_CALLS = ('PutRegisters', 'Set', 'SetN', 'PushConstants', 'memcpy', 'sizeof')


def _enclosing_call(ms, pos):
    """the name of the call whose argument list holds ms[pos], or None."""
    d = 0
    for j in range(pos - 1, max(pos - 4000, -1), -1):
        ch = ms[j]
        if ch in ')]}':
            d += 1
        elif ch in '([{':
            if d == 0:
                if ch != '(':
                    return None
                m = re.search(r'([A-Za-z_]\w*)\s*$', ms[max(0, j - 80):j])
                return m.group(1) if m else None
            d -= 1
        elif ch == ';' and d == 0:
            return None
    return None


def _written_after(ms, name, start, at):
    """True when the array NAME is written between start and at: a memcpy into it, an element store, or passed to a call that may fill it."""
    seg = ms[start:at]
    n = re.escape(name)
    if re.search(r'\bmemcpy\s*\(\s*&?\s*' + n + r'\b', seg):
        return True
    if re.search(r'(?<![\w.>])' + n + r'\s*(?:\[[^\]]*\]\s*)+(?:[+\-*/]?=)(?!=)', seg):
        return True
    for m in re.finditer(r'(?<![\w.>])' + n + r'\b(?!\s*\[)', seg):
        fn = _enclosing_call(ms, start + m.start())
        if fn and fn not in READ_ONLY_CALLS:
            return True
    return False


def data_values(ms, expr, at, lo=0):
    """-> [(kind, value)] per float of a data argument (`k`, `k + 16`, `&k[0][0]` of a literal-initialised local array), or None = OURS."""
    e = expr.strip()
    v = Q.lit_value(e)
    if v is not None:
        return [('LITERAL', v)]
    e = e.lstrip('&').strip()
    e = re.sub(r'(\[\s*0\s*\])+$', '', e)
    m = re.fullmatch(r'([A-Za-z_]\w*)(?:\s*\+\s*(\d+))?', e)
    if not m:
        return None
    arr = array_init(ms, m.group(1), at, lo)
    return None if arr is None else arr[int(m.group(2) or 0):]


def func_start(ms, at):
    """-> the '{' of the function body that holds ms[at] (the outermost enclosing brace that follows a ')': a function, not a
    namespace or a class), or 0. A data name is looked up from there: an earlier function's array of the same name is another array."""
    stack, i = [], 0
    while i < at:
        ch = ms[i]
        if ch in '"\'':
            j = i + 1
            while j < at and ms[j] != ch:
                j += 2 if ms[j] == '\\' else 1
            i = j + 1
            continue
        if ch == '{':
            stack.append(i)
        elif ch == '}' and stack:
            stack.pop()
        i += 1
    for ob in stack:
        if re.search(r'\)\s*(?:const|override|noexcept|final|mutable|\s)*(?:->\s*[\w:<>*&\s]+)?$', ms[max(0, ob - 200):ob]):
            return ob
    return 0


def lanes_of(vals, nfloats):
    """-> [(kind, value)] of nfloats floats from data_values (None = OURS; a float past the array's end is OURS)."""
    if vals is None:
        return [('OURS', None)] * nfloats
    return [vals[i] if i < len(vals) else ('OURS', None) for i in range(nfloats)]


# ------------------------------------------------------------------ ours: live maps (PutRegisters)
PUT_RE = re.compile(r'\bmd::PutRegisters\s*\(')
FIND_RE = re.compile(r'([\w.>-]+)\s*=\s*md::FindVariant\(\s*"(\w+)"\s*\)')


def _bind(ms, var, at, finds):
    """the FindVariant names a Variant& VAR at `at` is bound to (its kVariants[...] index), [] = unbound."""
    last = None
    for m in re.finditer(r'Variant&\s+' + re.escape(var) + r'\s*=\s*(?:rdr::fx::)?kVariants\[([^\]]+)\]', ms[:at]):
        last = m
    if not last:
        return []
    return _names_of(ms, last.group(1), last.start(), finds, 0)


def _names_of(ms, expr, at, finds, depth):
    out = []
    for ident in re.findall(r'[A-Za-z_]\w*', expr):
        if ident in finds:
            out += finds[ident]
        elif depth < 2:
            last = None
            for m in re.finditer(r'\bint\s+' + re.escape(ident) + r'\s*=\s*([^;]+);', ms[:at]):
                last = m
            if last:
                out += _names_of(ms, last.group(1), last.start(), finds, depth + 1)
    return list(dict.fromkeys(out))


def find_names(ms):
    """{member or local: [FindVariant names]} of `X = md::FindVariant("name")` in a file."""
    finds = {}
    for m in FIND_RE.finditer(ms):
        finds.setdefault(re.split(r'\.|->', m.group(1))[-1], []).append(m.group(2))
    return finds


def parse_live_maps(path, text, variants, gfinds=None):
    """-> (sites, problems). A site: {'family', 'path', 'line', 'variant', 'writes': {(reg, lane): (kind, value, line)}, 'data'}.
    gfinds: the FindVariant names of every live map file (the hook's members are assigned in one file and used in another);
    a file's own assignments come first."""
    ms = Q._mask(text)
    finds = {k: list(v) for k, v in (gfinds or {}).items()}
    for k, v in find_names(ms).items():
        finds[k] = v
    allv = list(dict.fromkeys(n for ns in find_names(ms).values() for n in ns))
    sites, problems = [], []
    for m in PUT_RE.finditer(ms):
        args = call_args(ms, m.end() - 1)
        line = line_of(ms, m.start())
        if len(args) != 5 or not args[2].isdigit():
            problems.append(f'{path}:{line}: a PutRegisters call the parser cannot read (want 5 arguments, a literal register): {args}')
            continue
        var, reg, data, n = args[1], int(args[2]), args[3], args[4]
        cands = _bind(ms, var, m.start(), finds) or allv
        nvec = int(n) if n.isdigit() else None
        vals = data_values(ms, data, m.start(), func_start(ms, m.start()))
        hit = False
        for vn in cands:
            v = variants.get(vn)
            if v is None:
                problems.append(f'{path}:{line}: FindVariant("{vn}") names no fx.json variant')
                continue
            rng = [(f, r, c) for f, r, c in v['consts'] if r == reg]
            if not rng:
                continue
            hit = True
            cnt = rng[0][2] if nvec is None else min(nvec, rng[0][2])
            lanes = lanes_of(vals, cnt * 4)
            sites.append({'family': 'live_maps', 'path': path, 'line': line, 'variant': vn,
                          'writes': {(reg + j // 4, j % 4): lanes[j] + (line,) for j in range(cnt * 4)}, 'data': data})
        if not hit:
            sites.append({'family': 'live_maps', 'path': path, 'line': line, 'variant': None, 'writes': {}, 'data': data,
                          'note': f'no candidate variant ({", ".join(cands) or "none"}) has a range starting at c{reg}: writes nothing'})
    return sites, problems


# ------------------------------------------------------------------ ours: the post chain (Regs)
def _string_lits(s):
    return re.findall(r'"((?:[^"\\]|\\.)*)"', s)


def _name_lits(ms, expr, at, depth=0):
    """the string literals a non-literal variant name is built from: its locals' definitions and the file's functions it calls."""
    lits = _string_lits(expr)
    if depth > 3:
        return lits
    for ident in re.findall(r'(?<![\w.:])([A-Za-z_]\w*)\b', expr):
        last = None
        for m in re.finditer(r'\b(?:std::string|auto)\s+' + re.escape(ident) + r'\s*=\s*([^;]+);', ms[:at]):
            last = m
        if last:
            lits += _name_lits(ms, last.group(1), last.start(), depth + 1)
            continue
        for m in re.finditer(r'\b(?:\w+::)*' + re.escape(ident) + r'\s*\([^;{)]*\)\s*(?:const\s*)?\{', ms):
            ob = m.end() - 1
            lits += _string_lits(ms[ob:Q._match(ms, ob, '{', '}')])
    return lits


def _compose(name, lits, effects):
    """True when a variant name = an effect's literal prefix '<effect>_' + words each found in the literals."""
    words = {w for t in lits for w in t.split('_') if w}
    for t in lits:
        if t.endswith('_') and t[:-1] in effects and name.startswith(t):
            rest = [w for w in name[len(t):].split('_') if w]
            return bool(rest) and all(w in words for w in rest)
    return False


def _for_ranges(ms, lo, hi):
    """[(var, a, b, start, end)] of the `for (int V = A; V < B; ++V)` loops in ms[lo:hi] (body span start..end)."""
    out = []
    for m in re.finditer(r'\bfor\s*\(\s*(?:int|uint32_t|size_t)\s+(\w+)\s*=\s*([^;]+);\s*\1\s*<\s*([^;]+);[^)]*\)', ms[lo:hi]):
        a, b = Q.lit_value(m.group(2)), Q.lit_value(m.group(3))
        if a is None or b is None:
            continue
        s = lo + m.end()
        out.append((m.group(1), int(a), int(b), s, Q._stmt_end(ms, s)))
    return out


def _set_regs(ms, arg, at, loops):
    """the registers a Set's first argument names: a literal, a constexpr int of the file, or a for loop's variable; None = unreadable."""
    if re.fullmatch(r'\d+', arg):
        return [int(arg)]
    if re.fullmatch(r'[A-Za-z_]\w*', arg):
        loop = [lp for lp in loops if lp[0] == arg and lp[3] <= at < lp[4]]
        if loop:
            return list(range(loop[-1][1], loop[-1][2]))
        const = re.search(r'\bconstexpr\s+int\s+' + re.escape(arg) + r'\s*=\s*(\d+)\s*;', ms)
        if const:
            return [int(const.group(1))]
    return None


def parse_post(path, text, variants):
    ms, lo, hi = func_body(text, POST[1], path)
    effects = {v['effect'] for v in variants.values()}
    loops = _for_ranges(ms, lo, hi)
    ev = []
    for m in re.finditer(r'\bRegs\s+([\w\s,]+);', ms[lo:hi]):
        ev.append((lo + m.start(), 'decl', [x.strip() for x in m.group(1).split(',')], None))
    for m in re.finditer(r'(?<![\w.])(\w+)\.(SetN|Set)\s*\(', ms[lo:hi]):
        ev.append((lo + m.start(), 'set', (m.group(1), m.group(2)), lo + m.end() - 1))
    for m in re.finditer(r'(?<![\w.])(need|pass)\s*\(', ms[lo:hi]):
        ev.append((lo + m.start(), m.group(1), None, lo + m.end() - 1))
    ev.sort(key=lambda x: x[0])
    banks, ps_of, sites, problems = {}, {}, [], []
    for at, kind, x, op in ev:
        if kind == 'decl':
            for n in x:
                banks[n] = {}
            continue
        args = call_args(ms, op)
        line = line_of(ms, at)
        if kind == 'need':
            if len(args) != 2:
                problems.append(f'{path}:{line}: a need() call the parser cannot read: {args}')
                continue
            lits = _string_lits(args[0])
            names = [t for t in lits if t in variants]
            if not lits:
                bl = _name_lits(ms, args[0], at)
                names = sorted(n for n in variants if n.endswith('_fragment') and _compose(n, bl, effects))
            if not names:
                problems.append(f'{path}:{line}: need({args[0]}) names no registry variant')
            ps_of[args[1]] = (names, line)
        elif kind == 'set':
            bank, fn = x
            if bank not in banks:
                continue   # not a Regs bank (another object's Set)
            if (fn == 'Set' and len(args) != 2) or (fn == 'SetN' and len(args) != 3):
                problems.append(f'{path}:{line}: a {bank}.{fn}() call the parser cannot read: {args}')
                continue
            regs = _set_regs(ms, args[0], at, loops)
            if regs is None:
                problems.append(f'{path}:{line}: {bank}.{fn}({args[0]}, ...): the register is not a literal, a constexpr or a for loop variable')
                continue
            nv = Q.lit_value(args[2]) if fn == 'SetN' else 1
            nvec = int(nv) if nv is not None else 1
            lanes = lanes_of(data_values(ms, args[1], at, lo), 4 * nvec)
            for r0 in regs:
                for j in range(4 * nvec):
                    banks[bank][(r0 + j // 4, j % 4)] = lanes[j] + (line,)
        elif kind == 'pass':
            if len(args) < 6:
                continue   # the lambda's own definition (`const auto pass = [&](...)`)
            psv, bank = args[4], args[5]
            if bank not in banks:
                problems.append(f'{path}:{line}: pass(..., {bank}, ...): not a Regs bank declared above')
                continue
            if psv not in ps_of:
                problems.append(f'{path}:{line}: pass(..., {psv}, ...): no need(..., {psv}) above')
                continue
            for vn in ps_of[psv][0]:
                w = {}
                for _f, reg, cnt in variants[vn]['consts']:
                    for k in range(cnt):
                        for i in range(4):
                            w[(reg + k, i)] = banks[bank].get((reg + k, i)) or ('LITERAL', 0.0, line)
                sites.append({'family': 'post', 'path': path, 'line': line, 'variant': vn, 'writes': w,
                              'data': f'bank {bank}; the variant from need() at line {ps_of[psv][1]}; an unset register is the bank\'s zero'})
    return sites, problems


# ------------------------------------------------------------------ ours: particles (float arrays)
def _eval_index(expr, consts, env):
    """-> [int] the values of an index expression over the loop variables in env {var: range}; None = not evaluable."""
    e = re.sub(r'sizeof\s*\(\s*float\s*\)', '4', expr)
    for k, v in consts.items():
        e = re.sub(r'\b' + re.escape(k) + r'\b', str(v), e)
    free = sorted(set(re.findall(r'[A-Za-z_]\w*', e)))
    if any(f not in env for f in free):
        return None
    out = []
    for combo in itertools.product(*[env[f] for f in free]):
        t = e
        for f, val in zip(free, combo):
            t = re.sub(r'\b' + re.escape(f) + r'\b', str(val), t)
        t = re.sub(r'(?<=[\d.])[fFuU]+\b', '', t)
        if not re.fullmatch(r'[\d\s+\-*/()%]*', t):
            return None
        try:
            out.append(int(eval(t, {'__builtins__': {}}, {})))   # noqa: S307 - digits and arithmetic only (checked above)
        except Exception:  # noqa: BLE001
            return None
    return out


def parse_particles(path, text, variants, blocks=None):
    blocks = PARTICLE_BLOCKS if blocks is None else blocks
    ms, lo, hi = func_body(text, PARTICLES[1], path)
    problems = []
    consts = {}
    for m in re.finditer(r'\bconstexpr\s+uint32_t\s+([^;]+);', ms):
        for k, v in re.findall(r'(\w+)\s*=\s*(\d+)', m.group(1)):
            consts[k] = int(v)
    sizes, state, first_line = {}, {}, {}
    for name, vn in blocks.items():
        var = variants.get(vn)
        d = re.search(r'\bfloat\s+' + re.escape(name) + r'\s*\[([^\]]+)\]', ms[lo:hi])
        if var is None or d is None:
            problems.append(f'{path}: particle block {name} -> {vn}: ' + ('no such registry variant' if var is None else f'no `float {name}[...]` in the function'))
            continue
        n = _eval_index(d.group(1), consts, {})
        slots = float_slots(var)
        if not n or n[0] != len(slots):
            problems.append(f'{path}: particle block {name}[{d.group(1)}] = {n} floats, but {vn} packs {len(slots)} (its fx.json ranges)')
            continue
        if not re.search(r'PushConstants\(\s*' + re.escape(name) + r'\b', ms[lo:hi]):
            problems.append(f'{path}: particle block {name} is never pushed with PushConstants in the function')
            continue
        sizes[name] = n[0]
        state[name] = [[] for _ in range(n[0])]
        first_line[name] = line_of(ms, lo + d.start())
    loops = _for_ranges(ms, lo, hi)

    def as_offset(expr):
        """`&A[i]` -> `A + (i)` (the same address); anything else unchanged."""
        mm = re.fullmatch(r'\s*&\s*(\w+)\s*\[(.+)\]\s*', expr, re.S)
        return f'{mm.group(1)} + ({mm.group(2)})' if mm else expr
    aliases = {}
    for m in re.finditer(r'\bfloat\s*\*\s*(\w+)\s*=\s*([^;]+);', ms[lo:hi]):
        rhs = as_offset(m.group(2))
        ma = re.fullmatch(r'\s*(\w+)\s*(?:\+\s*(.+))?', rhs, re.S)
        if ma and ma.group(1) in state:
            aliases[m.group(1)] = (ma.group(1), (ma.group(2) or '0').strip())
        elif any(re.search(r'\b' + re.escape(b) + r'\b', rhs) for b in state):
            problems.append(f'{path}:{line_of(ms, lo + m.start())}: float* {m.group(1)} = {rhs.strip()}: a pointer into a block the parser cannot read')

    def names_block(expr):
        return any(re.search(r'\b' + re.escape(b) + r'\b', expr) for b in list(state) + list(aliases))

    def target(expr, at):
        """'A + i' / '&A[i]' / 'A' / an alias -> (block, [index values]); (None, None) when it is not a block."""
        env = {lp[0]: range(lp[1], lp[2]) for lp in loops if lp[3] <= at < lp[4]}
        mm = re.fullmatch(r'\s*(\w+)\s*(?:\+\s*(.+))?', as_offset(expr), re.S)
        if not mm:
            return None, None
        nm, off = mm.group(1), (mm.group(2) or '0').strip()
        if nm in aliases:
            b, base = aliases[nm]
            off = f'({base}) + ({off})'
            nm = b
        if nm not in state:
            return None, None
        return nm, _eval_index(off, consts, env)

    def nbytes(expr, at):
        e = expr.strip()
        mm = re.fullmatch(r'sizeof\s*\(?\s*(\w+)\s*\)?', e)
        if mm and mm.group(1) != 'float':
            nm = mm.group(1)
            if nm in sizes:
                return 4 * sizes[nm]
            arr = array_init(ms, nm, at, lo)
            return 4 * len(arr) if arr else None
        v = _eval_index(e, consts, {})
        return v[0] if v and len(v) == 1 else None

    events = []
    for m in re.finditer(r'(?<![\w.])(\w+)\s*\[([^\]]+)\]\s*(\+=|-=|\*=|=)(?!=)\s*', ms[lo:hi]):
        events.append((lo + m.start(), 'w', m))
    for m in re.finditer(r'\bstd::memcpy\s*\(', ms[lo:hi]):
        events.append((lo + m.start(), 'cpy', m))
    events.sort(key=lambda x: x[0])
    for at, kind, m in events:
        line = line_of(ms, at)
        if kind == 'w':
            nm = m.group(1)
            if (nm not in state and nm not in aliases) or re.match(r'\s*\{', ms[lo + m.end():]):
                continue   # not a block, or a declaration's initialiser
            b, idx = target(f'{nm} + ({m.group(2)})', at)
            if b is None:
                continue
            if idx is None:
                problems.append(f'{path}:{line}: {nm}[{m.group(2)}]: an index the parser cannot evaluate')
                continue
            j, d = lo + m.end(), 0
            while j < hi and not (d == 0 and ms[j] in ',;'):
                d += ms[j] in '([{'
                d -= ms[j] in ')]}'
                j += 1
            v = Q.lit_value(ms[lo + m.end():j]) if m.group(3) == '=' else None
            k = ('LITERAL', v) if v is not None else ('OURS', None)
            for i in idx:
                if 0 <= i < sizes[b]:
                    state[b][i].append(k + (line,))
        else:
            args = call_args(ms, lo + m.end() - 1)
            if len(args) != 3:
                continue
            b, idx = target(args[0], at)
            if b is None:
                if names_block(args[0]):   # a write into a block the parser cannot place: never skipped (its floats would read as the zero)
                    problems.append(f'{path}:{line}: memcpy({args[0]}, ...): a destination in a block the parser cannot read')
                continue
            n = nbytes(args[2], at)
            if idx is None or n is None or len(idx) != 1:
                problems.append(f'{path}:{line}: memcpy({args[0]}, ..., {args[2]}): a destination or size the parser cannot evaluate')
                continue
            sb, sidx = target(args[1], at)
            if sb is not None and sidx and len(sidx) == 1:
                src = [max(state[sb][sidx[0] + q], key=lambda t: RANK[t[0]])[:2] if state[sb][sidx[0] + q] else ('LITERAL', 0.0)
                       for q in range(n // 4)]
            else:
                src = lanes_of(data_values(ms, args[1], at, lo), n // 4)
            for q in range(n // 4):
                if 0 <= idx[0] + q < sizes[b]:
                    state[b][idx[0] + q].append(src[q] + (line,))
    sites = []
    for b, vn in blocks.items():
        if b not in state:
            continue
        for (reg, lane), got in zip(float_slots(variants[vn]), state[b]):
            for kind, val, line in (got or [('LITERAL', 0.0, first_line[b])]):
                sites.append({'family': 'particles', 'path': path, 'line': line, 'variant': vn, 'writes': {(reg, lane): (kind, val, line)},
                              'data': f'{b}[] ' + ('written' if got else 'never written: the array\'s zero')})
    return sites, problems


def load_ours(read, variants, live_paths):
    sites, problems = [], []
    gfinds = {}
    for p in live_paths:
        for k, v in find_names(Q._mask(read(p))).items():
            gfinds.setdefault(k, []).extend(x for x in v if x not in gfinds.get(k, []))
    for p in live_paths:
        s, pr = parse_live_maps(p, read(p), variants, gfinds)
        sites += s
        problems += pr
    for parse, (p, _re) in ((parse_post, POST), (parse_particles, PARTICLES)):
        s, pr = parse(p, read(p), variants)
        sites += s
        problems += pr
    # live maps: a variant some site writes keeps zero in every register no site writes (the block is zero-filled)
    written = {}
    for s in sites:
        if s['family'] == 'live_maps' and s['variant']:
            written.setdefault(s['variant'], (s['path'], s['line'], set()))[2].update(s['writes'])
    for vn, (p, ln, keys) in written.items():
        miss = {(r + k, i): ('LITERAL', 0.0, ln) for _f, r, c in variants[vn]['consts'] for k in range(c) for i in range(4) if (r + k, i) not in keys}
        if miss:
            sites.append({'family': 'live_maps', 'path': p, 'line': ln, 'variant': vn, 'writes': miss,
                          'data': 'zero (no PutRegisters of this register; the block is zero-filled)'})
    return sites, problems


# ------------------------------------------------------------------ game: the effect's code path
def effect_paths(con, effects, img, setters, fcon=None, depth=CALL_DEPTH):
    """-> {effect: {'roots', 'vtables', 'funcs': {func: how}, 'model': bool}} (see the module doc)."""
    import qvtable
    catalog = qvtable.has_tables(con)
    out = {}
    for e in effects:
        roots = sorted({r['func'] for s in con.execute('SELECT addr FROM strings WHERE lower(text) = lower(?)', (e,))
                        for r in con.execute('SELECT func FROM strrefs WHERE addr = ?', (s['addr'],))})
        model = Q.DESC_FN in roots
        roots = [r for r in roots if r not in (Q.DESC_FN, Q.NAME_TABLE_FN)]
        stored = {v[0] for f in roots for v in fcon.execute('SELECT DISTINCT vt FROM vstores WHERE func = ?', (f,))} if fcon else set()
        # a table the loader builds the address of (lis + addi) where the index's table catalog starts one: the object's other
        # vtables (a draw node's table beside the class's, which the field index does not record as a store); read to the
        # catalog's length (the next table may start right after it), a stored vtable to its first non-function word
        built = {r[0]: r[1] for f in roots for r in con.execute("SELECT DISTINCT i.value, t.len FROM imms i JOIN tables t ON t.start = i.value "
                                                                "WHERE i.func = ? AND i.op = 'lis+addi'", (f,))} if catalog else {}
        vts = sorted(stored | set(built))
        funcs = {f: f'loads "{e}"' for f in roots}
        for vt in vts:
            for slot, _at, tgt in qvtable.read_table(con, img, vt, limit=qvtable.MAX_SLOTS if vt in stored else built[vt]):
                funcs.setdefault(tgt, f'slot {slot} of vtable {Q.hx(vt)}')
        front = list(funcs)
        for _d in range(depth):
            nxt = []
            for f in front:
                for r in con.execute('SELECT DISTINCT dst FROM edges WHERE src = ? AND dst IS NOT NULL', (f,)):
                    c = r[0]
                    if c not in funcs and c not in setters:
                        funcs[c] = f'called by sub_{f:08X}'
                        nxt.append(c)
            front = nxt
        out[e] = {'roots': roots, 'vtables': vts, 'funcs': funcs, 'model': model}
    return out


def hit_lanes(h):
    return WHAT_LANES.get(h['what'].split()[-1], (0, 1, 2, 3))


def _const_of(p, img):
    """a data term's value: a number the scan folded in (`2`, `-1`) or one image word (`[0x8218B8CC]`); None = from code."""
    num = Q.lit_value(p)
    if num is not None:
        return num
    m = re.fullmatch(r'\s*\[0x([0-9A-Fa-f]{8})\]\s*', p)
    w = img.word(int(m.group(1), 16)) if m else None
    return struct.unpack('>f', struct.pack('>I', w))[0] if w is not None else None


def hit_values(h, img):
    """-> [4 floats or None] per lane of a one-vec4 write: the data as a tuple `(a, b, c, d)`, or as the stack stores the scan saw
    (`sp+84[f32] = X; sp+88[f32] = Y` with r5 = sp+84: lane = (slot - r5) / 4; a lane no scalar store fills is UNSEEN: the
    stack's leftover, not compared). A term with alternatives (`X | (a later store on some paths) Y`) is from code."""
    t = (h.get('data') or '').strip()
    out = [None] * 4
    if t.startswith('(') and t.endswith(')'):
        parts = Q._split_args(t[1:-1])
        return [_const_of(p, img) for p in parts] if len(parts) == 4 else out
    m0 = re.fullmatch(r'sp\+(\d+)', (h.get('r5') or '').strip())
    if m0 and re.match(r'sp\+\d+\[f32\] = ', t):
        base = int(m0.group(1))
        out = [UNSEEN] * 4
        for part in t.split('; '):
            mm = re.fullmatch(r'sp\+(\d+)\[f32\] = (.+)', part.strip())
            if not mm:
                continue
            off = int(mm.group(1)) - base
            if off % 4 == 0 and 0 <= off // 4 < 4:
                out[off // 4] = None if '|' in mm.group(2) else _const_of(mm.group(2), img)
    return out


def any_writers(stage, reg, count, hits, limit=3):
    """-> [text] the literal-register writes of registers reg..reg+count-1 of the stage in ANY function (q const): a hint for an
    UNSETTLED row, never deciding it (a literal register write is not tied to an effect)."""
    out = []
    for h in hits:
        if h['reg']['kind'] != 'literal' or not (h['stage'] == stage or h['stage'] == 'vsps'):
            continue
        hr = h['reg']['value']
        hc = h['count']['value'] if h['count']['kind'] == 'literal' else 1
        if hr < reg + count and reg < hr + max(hc, 1):
            out.append(f"sub_{h['func']:08X} ({h.get('func_name') or '?'}) {Q.hx(h['site'])} r4 = {hr} count {hc}; data {(h.get('data') or '')[:70]}")
    return out


def path_writers(stage, reg, count, path, hits, img, starts=None, left=None):
    """-> [game row] the literal-register writes on the effect's code path covering registers reg..reg+count-1 of the stage
    (a write whose lanes are part image constants, part code gives one row of each). starts: the registers where the effect's
    variants of this stage start a range; a write starting elsewhere is another layout's (appended to `left`, not joined)."""
    rows = []
    for h in hits:
        if h['func'] not in path['funcs'] or h['reg']['kind'] != 'literal' or not (h['stage'] == stage or h['stage'] == 'vsps'):
            continue
        hr = h['reg']['value']
        hc = h['count']['value'] if h['count']['kind'] == 'literal' else 1
        ks = [hr + j - reg for j in range(max(hc, 1)) if reg <= hr + j < reg + count]
        if not ks:
            continue
        if starts is not None and hr not in starts:
            if left is not None:
                left.append(f"sub_{h['func']:08X} {Q.hx(h['site'])} r4 = {hr} count {hc}")
            continue
        vals = hit_values(h, img) if hc == 1 else [None] * 4
        why = (f"sub_{h['func']:08X} ({path['funcs'][h['func']]}) {Q.hx(h['site'])} {h['stage']} {h['what']} r4 = {hr} count {hc}"
               f"; data {(h.get('data') or '')[:90]}")
        code = [(k, i) for k in ks for i in hit_lanes(h) if vals[i] is None]
        const = [((k, i), vals[i]) for k in ks for i in hit_lanes(h) if vals[i] is not None and vals[i] is not UNSEEN]
        if code:
            rows.append({'name': None, 'kind': 'GAME-CODE', 'lanes': code, 'values': None, 'site': Q.hx(h['site']), 'func': Q.hx(h['func']), 'why': why})
        if const:
            rows.append({'name': None, 'kind': 'GAME-CONST', 'lanes': [c[0] for c in const], 'values': [c[1] for c in const],
                         'site': Q.hx(h['site']), 'func': Q.hx(h['func']), 'why': why})
    return rows


# ------------------------------------------------------------------ the join
def _best(ents):
    return max(ents, key=lambda x: RANK[x['kind']])


def compare(lanes, grows):
    """-> (diff, partial, const_eq) lists of text for one row: ours' lanes {key: [entries]} against the game rows."""
    best = {k: _best(v)['kind'] for k, v in lanes.items()}
    diff, part, ceq, note = [], [], [], []
    for gr in grows:
        gl = [ln for ln in gr['lanes'] if ln in lanes]
        if gr['kind'] == 'GAME-CONST':
            bad, same = [], []
            for ln, gv in zip(gr['lanes'], gr['values'] or [None] * len(gr['lanes'])):
                if ln in lanes and best[ln] != 'LITERAL':
                    note.append(f"{Q.lane_name(ln)} ours computes it where the game writes the image constant {'?' if gv is None else f'{gv:g}'} "
                                f"(not compared): {gr['why']}")
                if ln not in lanes or best[ln] != 'LITERAL':
                    continue
                ovs = [x['value'] for x in lanes[ln] if x['kind'] == 'LITERAL']
                if gv is not None and all(v is not None and math.isclose(v, gv, rel_tol=1e-6, abs_tol=1e-12) for v in ovs):
                    same.append(f'{Q.lane_name(ln)} {gv:g}')
                else:
                    bad.append(f"{Q.lane_name(ln)}: ours {'/'.join('?' if v is None else f'{v:g}' for v in ovs)}, the game's "
                               + ('?' if gv is None else f'{gv:g}'))
            if bad:
                diff.append(f"{gr['why']}: " + '; '.join(bad))
            elif same:
                ceq.append(', '.join(same))
            continue
        lit = [ln for ln in gl if best[ln] == 'LITERAL']
        if lit and len(lit) == len(gl):
            diff.append(f"{''.join(Q.lane_name(x) for x in lit)} literal in ours where the game writes: {gr['why']}")
        elif lit:
            part.append(f"{''.join(Q.lane_name(x) for x in lit)} literal in ours of the lanes {''.join(Q.lane_name(x) for x in gl)} the game writes: {gr['why']}")
        arms = sorted({x['at'] for ln in gl if best[ln] != 'LITERAL' for x in lanes[ln] if x['kind'] == 'LITERAL'})
        if arms:
            part.append('a literal arm beside the fed one: ' + ', '.join(arms))
    return diff, sorted(set(part)), ceq, sorted(set(note))


def build_rows(sites, variants, fields, paths, hits, img, game=None, mats=None, totals=None, consts=None, passes=None, flags=None):
    """-> rows, one per (effect, stage, start register) of the fx registry that an ours site feeds."""
    fidx = {}
    for (eff, st, reg), f in fields.items():
        fidx.setdefault((eff, st), []).append((reg, f['count']))
    starts = {}
    for v in variants.values():
        starts.setdefault((v['effect'], v['stage']), set()).update(r for _f, r, _c in v['consts'])
    byfield = {}
    for s in sites:
        if not s['variant']:
            continue
        v = variants[s['variant']]
        rr = fidx.get((v['effect'], v['stage']), [])
        for (r, i), kv in s['writes'].items():
            for reg, cnt in rr:
                if reg <= r < reg + cnt:
                    byfield.setdefault((v['effect'], v['stage'], reg), []).append(
                        {'key': (r - reg, i), 'kind': kv[0], 'value': kv[1], 'family': s['family'], 'at': f"{s['path']}:{kv[2] if len(kv) > 2 else s['line']}"})
                    break
    rows = []
    for (eff, st, reg), ents in sorted(byfield.items()):
        f = fields[(eff, st, reg)]
        p = paths.get(eff) or {'roots': [], 'vtables': [], 'funcs': {}, 'model': False}
        left = []
        if p['model'] and game is not None:
            g = Q.game_source(eff, st, reg, f['field'], f['count'], game[0], game[1], mats, totals, None, consts)
            grows = [{**gr, 'why': f"{gr['name']} (row {gr['row']} @{gr['site']}, by {gr['match']}; {gr['why']})"} for gr in g['rows']
                     if gr['kind'] in Q.GAME_FED + ('GAME-CONST',)]
            src = 'the model draw (by name: q constdiff\'s sources)'
        else:
            grows = path_writers(st, reg, f['count'], p, hits, img, starts.get((eff, st), set()), left)
            src = f"code path: {len(p['roots'])} loader(s), {len(p['vtables'])} vtable(s), {len(p['funcs'])} functions"
        lanes = {}
        for en in ents:
            lanes.setdefault(en['key'], []).append(en)
        best = {k: _best(v)['kind'] for k, v in lanes.items()}
        ours = max(best.values(), key=lambda k: RANK[k])
        never = Q.pass_never(eff, f.get('variants'), passes, flags) if p['model'] else None
        diff, part, ceq, note = compare(lanes, grows)
        flag = ('PASS-NEVER' if never else 'UNSETTLED' if not grows else 'DIFF' if diff else 'PARTIAL' if part else 'CONST' if ceq else 'OK')
        at = sorted({f"{_best(v)['kind']} {_best(v)['at']}" for v in lanes.values()})
        hints = any_writers(st, reg, f['count'], hits) if flag == 'UNSETTLED' else []
        rows.append({'effect': eff, 'stage': st, 'reg': reg, 'count': f['count'], 'field': f['field'], 'hints': hints,
                     'families': sorted({en['family'] for en in ents}), 'ours': ours, 'ours_at': at,
                     'lanes': {f'{k[0]}.{"xyzw"[k[1]]}': best[k] for k in sorted(best)},
                     'game': src, 'game_rows': grows, 'flag': flag, 'diff': diff, 'partial': part, 'const_eq': ceq,
                     'computed_vs_const': note, 'left_out': left,
                     'pass_never': never, 'loaders': [Q.hx(r) for r in p['roots']]})
    return rows


def load_game_side(con, effects, source='game'):
    """-> (paths, hits, img, game, mats, totals, consts, passes, flags, problems)."""
    import qconst
    import qdecomp
    import qvtable
    problems = []
    d = qconst.get_scan(con, types.SimpleNamespace(no_cache=False, window=qconst.WINDOW, raw=False, root=None))
    setters = {v['addr'] for v in d['setters'].values()}
    img = qvtable.Img(qdecomp.IMAGE)
    fcon = None
    try:
        import build_fields as bf
        _g, _i, fdb = bf.default_paths(None, None)
        if os.path.exists(fdb):
            fcon = sqlite3.connect(f'file:{fdb}?mode=ro', uri=True)
    except Exception as e:  # noqa: BLE001
        problems.append(f'fields index: {e}')
    if fcon is None:
        problems.append("fields index (vstores) not found: the effects' vtables cannot be read")
    try:
        paths = effect_paths(con, effects, img, setters, fcon)
    finally:
        if fcon is not None:
            fcon.close()
    game = Q.load_game(con)
    mats, totals = Q.material_names(source=source)
    consts, cprobs = Q.load_consts(con)
    flags, fprobs = Q.load_pass_flags(con)
    passes, _perr = Q.effect_passes()
    return paths, d['hits'], img, game, mats, totals, consts, passes, flags, problems + cprobs + fprobs


def run(con, a, read=None, fx_dir=None, live_paths=None):
    read = read or (lambda p: open(os.path.join(Q.REPO, p), encoding='utf-8').read())
    if live_paths is None:
        live_paths = sorted(os.path.relpath(p, Q.REPO).replace(os.sep, '/') for p in glob.glob(os.path.join(Q.REPO, LIVE_MAPS_GLOB)))
    variants = load_variants(fx_dir)
    fields = Q.load_fields(fx_dir)
    try:
        sites, problems = load_ours(read, variants, live_paths)
    except (RegError, Q.DiffError) as e:
        print(f'q constdiff --byreg: {e}', file=sys.stderr)
        return 2
    effects = sorted({variants[s['variant']]['effect'] for s in sites if s['variant']})
    paths, hits, img, game, mats, totals, consts, passes, flags, gprobs = load_game_side(con, effects, getattr(a, 'source', 'game'))
    rows = build_rows(sites, variants, fields, paths, hits, img, game, mats, totals, consts, passes, flags)
    return report(rows, sites, fields, a, problems + gprobs)


def report(rows, sites, fields, a, problems):
    sel = rows
    if getattr(a, 'effect', None):
        sel = [r for r in sel if fnmatch.fnmatchcase(r['effect'], a.effect)]
    if getattr(a, 'kind', None) == 'PASS-NEVER':
        sel = [r for r in sel if r['pass_never']]
    elif getattr(a, 'kind', None):
        sel = [r for r in sel if r['ours'] == a.kind and not r['pass_never']]
    if getattr(a, 'diff', False):
        sel = [r for r in sel if r['flag'] == 'DIFF']
    if getattr(a, 'partial', False):
        sel = [r for r in sel if r['flag'] == 'PARTIAL']
    fed = {r['effect'] for r in rows}
    other = sorted({e for (e, _s, _r) in fields if Q.resolver_of(e) is None and e not in fed})
    if getattr(a, 'json', False):
        print(json.dumps({'rows': sel, 'not_fed_by_ours': other, 'problems': problems}, indent=1, default=str))
    else:
        cur = None
        for r in sel:
            if r['effect'] != cur:
                cur = r['effect']
                print(f"== {cur}  (game: {r['game']}; loaders {', '.join(r['loaders']) or 'none'})")
            regs = f"c{r['reg']}" + (f"+{r['count']}" if r['count'] > 1 else '')
            print(f"{r['flag']:10s} {r['stage']} {regs:8s} {r['field']:22s} ours {r['ours']:7s} {', '.join(r['ours_at'])}  [{'/'.join(r['families'])}]")
            for g in r['game_rows'][:4]:
                print(f"           game {g['kind']}: {g['why']}")
            if len(r['game_rows']) > 4:
                print(f"           game ... {len(r['game_rows']) - 4} more writers (--json)")
            for x in r['diff']:
                print(f'           DIFF: {x}')
            for x in r['partial']:
                print(f'           PARTIAL: {x}')
            for x in r['const_eq']:
                print(f"           CONST: ours' literal equals the game's image constant: {x}")
            for x in r['computed_vs_const']:
                print(f'           NOTE: {x}')
            if r['left_out']:
                print(f"           (left out: {len(r['left_out'])} writer(s) on the path whose start register is no range start of this effect: "
                      f"another layout's: {'; '.join(r['left_out'][:2])}{' ...' if len(r['left_out']) > 2 else ''})")
            if r['pass_never']:
                print(f"           PASS-NEVER: {r['pass_never']}")
            for x in r['hints'][:3]:
                print(f'           (hint, not joined: a literal writer of this register in some function: {x})')
            if len(r['hints']) > 3:
                print(f"           (hint: {len(r['hints']) - 3} more such writers, --json)")
        for fam in sorted({f for r in rows for f in r['families']}):
            fr = [r for r in sel if fam in r['families']]
            tot = {}
            for r in fr:
                tot[r['flag']] = tot.get(r['flag'], 0) + 1
            print(f"totals {fam}: {len(fr)} rows; " + ', '.join(f'{k} {tot.get(k, 0)}' for k in FLAGS))
        dl = sorted({x for r in sel if r['flag'] == 'DIFF' for x in r['ours_at'] if x.startswith('LITERAL')})
        print(f"totals: {len(sel)} rows (of {len(rows)}); DIFF {sum(r['flag'] == 'DIFF' for r in sel)} at {len(dl)} distinct ours lines; "
              f"ours sites {sum(1 for s in sites if s['variant'])} (+{sum(1 for s in sites if not s['variant'])} that write nothing)")
        print(f"not fed by ours by register (drawn from the stream's own bank, or not drawn by ours): {len(other)} effects: {', '.join(other)}")
    for p in problems:
        print('PROBLEM: ' + p)
    return 2 if problems else 0
