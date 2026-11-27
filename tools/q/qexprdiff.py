r"""q exprdiff: per GPU input, the EXPRESSION the game computes it from against OUR expression for the same input (S111 EXPRDIFF,
code-diff extractor 6; docs/research/s111/WHOLEDIFF_PLAN.md A.5 row 6). It replaces `q constdiff`'s verdict (one engine, rule 6).

    py tools/q/q.py exprdiff [--effect E] [--input GLOB] [--flag DIFF|GAME-ONLY|UNSETTLED|OK|PASS-NEVER ...] [--ref REF] [--all]
                             [--json|--facts]

The inputs, per lane:
  * every shader-constant lane the name table feeds (the join of `q constdiff`, tools/q/qconstdiff.py `collect`: per (effect, stage,
    register) the game's name-table rows with their lanes, ours' resolver branches). Game expression of a lane: GAME-MATERIAL = the
    material parameter `mat:NAME[i]`; GAME-CONST = the image constant (the declared chain, read every call); GAME-CODE = the code leaf
    `code:NAME[i]`, or, for a lane CODE_SLICES declares, the value `q slice --expr` reads at the game's setter (tools/q/qslice.py);
    GAME-NOREC / UNKNOWN rows are not inputs (the game never writes them). Ours expression: the branch text of ResolveChar / Resolve
    lane by lane (set4 / out[N] / memcpy, if / else arms and `?:` as phi, locals and the resolver's return-lambdas inlined); several
    live branches of one field are a phi in source order; a lane a branch leaves unwritten is the caller's zero.
  * every draw input DRAW_INPUTS declares (a GPU value not in the name table: the model draw's world rows): the game value is a slice
    at the setter's operand, ours the expression an anchor names in our code.
Leaves are bound into one space before the compare: a material parameter through OUR table of names (src/assets/material_floats.h
ApplyFloat, else jboy.h at an old ref: `mat.MEMBER[i]` = the parameter; the family's own spelling for the specular names) so
`mat:g_fHdLigLvl[0]` and `mt.hd_lig[0]` are one leaf; a declared input's game sub-expressions / ours names through its `bind`; an
image word below the code range end is its value.
The compare: both sides go to one normal form (a polynomial over atoms: + and * flattened and sorted, - as *-1, << k as * 2^k, a / const
as * 1/const; min / max / & / | / ^ sorted; a load is [its address in normal form]; a phi keeps its arms in order). Verdicts (field
`expr:<input>`): UNSETTLED when a game leaf is ?loop / ?vcall / ?rN (cut by the slicer) or ours reads a context leaf (c.*, our light
set ...) that no binding ties to the game's; GAME-ONLY LEAF when the game reads a leaf ours never does (the dropped factor: MATSIZE's
class; a literal where the game feeds the material is this); DIFF when a leaf differs (both sides read one the other does not) or the
operators / coefficients differ; OK when the normal forms are equal, or the two evaluate equal at every sample point (same leaves,
SAMPLES seeded points, phi = its first arm whose condition holds: `OK (by evaluation)` [L]); an ours phi whose arm equals the game's
is OK with the other arms named. PASS-NEVER (constdiff's class) = the model draw never picks a pass that reads it: not compared.
Each row carries constdiff's own flag for the register (`source`: DIFF / PARTIAL / CONST / PASS-NEVER / -), unchanged.
"""
import argparse
import collections
import fnmatch
import json
import math
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import qconstdiff as CD  # noqa: E402  the join (name table, resolvers, pass flags): one engine

REPO = CD.REPO
VERDICTS = ('DIFF', 'GAME-ONLY LEAF', 'UNSETTLED', 'OK', 'PASS-NEVER')
SAMPLES = 160
# selftest only: no_commute (no normal form: the text as written), no_offset (a load's offsets dropped), no_loop (cut leaves judged),
# no_factor (a missing game leaf not looked for), no_eval (no sample evaluation)
PLANT = os.environ.get('WWE_QEXPRDIFF_PLANT', '')

# GAME-CODE lanes whose value the slicer reads at the game's setter. (descriptor name, lane index): site, operand, the bindings that tie
# game sub-expressions to our names (each cited), constants the game reads from a field its constructor fills once.
CODE_SLICES = {
    ('g_f4RimLightDepth', 2): {
        'site': 'sub_827E9D90+E4', 'operand': '[mem]',
        'bind': {'[in_r3+384]': 'rim far', '[in_r3+380]': 'rim near'},
        'ours': {'c.rim_params[3]': 'rim far', 'c.rim_params[2]': 'rim near', 'd': None},
        'consts': {'[in_r3+52]': 1000.0},
        'why': 'z = G+308 (sub_827E9D90 +BC..+E4); r3 = the rim object + 20, so +380 / +384 = object +400 / +404 = the rim pass depth range'
               ' near / far (G+300 / +304, lanes x / y), +52 = object +72 = 1000 (sub_827EA7F0 +1C4): docs/research/s111/RIMGAME.md [C];'
               ' ours c.rim_params[2..3] = MapImage::params near / far (char_effects.cpp ResolveChar RimLightDepth)',
    },
}

# Draw inputs outside the name table. ours: (file, the anchor regex whose group 1 is our expression).
DRAW_INPUTS = (
    {'input': 'model draw palette row address (MATSIZE splat rows)',
     'site': 'sub_82C04DF0+525C', 'operand': 'r10',
     'bind_slices': {'palette word': ('sub_82C04DF0+5248', 'r10')},
     'bind': {'?r5': 'palette array'},
     'ours_file': 'src/scene_live/weapon_rows_core.h', 'ours_fn': 'OnModelCall',
     'anchor': r'\belse\s+palette\s*=\s*([^;]+);',
     'ours': {'palette_ptr': 'palette array', 'node': 'palette word'},
     'why': 'the row the model draw loads per palette word k (lvx128 at +525C; r6 = r5 stored at 0x82C04E14 stw r5,29044(r1), reloaded'
            ' at 0x82C0A01C: scene_live/arena_world_hook.h); the word = lwzu r10,4(r8) at +5248 (DESKMODEL / DESKSKIN [C]); ours'
            ' palette_ptr = the hook\'s r5, node = the part\'s palette word (weapon_rows_core.h OnModelCall); MATSIZE [C]: equal'},
)


class ExprError(Exception):
    pass


# ------------------------------------------------------------------ the tree
# ('num', v) ('leaf', key) ('op', o, a, b) ('neg', a) ('fn', name, (args)) ('ld', addr) ('phi', ((cond | None, value), ...))
# ('cmp', o, a, b) ('not', a) ('raw', text)
ZERO = ('num', 0.0)
CMP_OPS = ('<', '<=', '>', '>=', '==', '!=', '&&', '||')


class _P:
    """one Pratt parser for both notations: mode 'game' (the slicer's --expr text) or 'cpp' (our C++)"""
    PREC = {'?': 1, '||': 2, '&&': 3, '|': 4, '^': 5, '&': 6, '==': 7, '!=': 7, '<': 8, '<=': 8, '>': 8, '>=': 8,
            '<<': 9, '>>': 9, '+': 10, '-': 10, '*': 11, '/': 11}

    def __init__(self, toks, mode, env=None, lambdas=None):
        self.t, self.i, self.mode = toks, 0, mode
        self.env, self.lambdas = env or {}, lambdas or {}

    def peek(self, k=0):
        return self.t[self.i + k] if self.i + k < len(self.t) else ('end', None)

    def take(self, want=None):
        tok = self.peek()
        if want is not None and tok[1] != want:
            raise ExprError(f'expected {want!r} at token {self.i}: {tok}')
        self.i += 1
        return tok

    def expr(self, rbp=0):
        left = self.prefix()
        while True:
            k, v = self.peek()
            if k != 'op' or v not in self.PREC or self.PREC[v] <= rbp:
                return left
            self.take()
            if v == '?':
                a = self.expr(0)
                self.take(':')
                b = self.expr(0)
                left = ('phi', ((left, a), (None, b)))
                continue
            right = self.expr(self.PREC[v])
            left = ('cmp', v, left, right) if v in CMP_OPS else ('op', v, left, right)

    def prefix(self):
        k, v = self.take()
        if k == 'num':
            return self.postfix(('num', v))
        if k == 'img':
            return ('num', v)
        if k == 'op' and v == '-':
            a = self.expr(10.5)
            return ('num', -a[1]) if a[0] == 'num' else ('neg', a)
        if k == 'op' and v == '!':
            return ('not', self.expr(10.5))
        if k == 'op' and v == '*':          # cpp: a dereference = the leaf *x
            a = self.expr(10.5)
            return ('leaf', '*' + leaf_text(a)) if a[0] == 'leaf' else ('fn', 'deref', (a,))
        if k == 'op' and v == '&':
            return self.expr(10.5)
        if k == 'op' and v == '(':
            a = self.expr(0)
            self.take(')')
            return self.postfix(a)
        if k == 'op' and v == '[':          # game: a load
            a = self.expr(0)
            self.take(']')
            return ('ld', a)
        if k == 'id':
            return self.ident(v)
        raise ExprError(f'unexpected token {v!r}')

    def postfix(self, a):
        return a

    def ident(self, name):
        if self.mode == 'cpp':
            name = re.sub(r'^(?:\w+::)+', '', name)
            if self.peek()[1] == '<' and name in ('static_cast', 'reinterpret_cast', 'bit_cast', 'min', 'max'):
                while self.take()[1] != '>':
                    pass
        if self.peek()[1] == '(':
            self.take('(')
            if name == 'phi' and self.mode == 'game':
                raise ExprError('phi is cut out before parsing')
            args = []
            while self.peek()[1] != ')':
                args.append(self.expr(0))
                if self.peek()[1] == ',':
                    self.take(',')
            self.take(')')
            if self.mode == 'cpp' and name in ('float', 'double', 'static_cast', 'uint32_t', 'int32_t', 'int', 'size_t') and len(args) == 1:
                return self.member(args[0])
            if name in self.lambdas:
                ps, body = self.lambdas[name]
                env = dict(self.env)
                env.update(zip(ps, args))
                return parse_cpp(body, env, self.lambdas)
            return self.member(('fn', name, tuple(args)))
        if self.mode == 'cpp' and name in ('true', 'false'):
            return ('num', 1.0 if name == 'true' else 0.0)
        if self.mode == 'cpp' and name in self.env:
            return self.member(self.env[name])
        return self.member(('leaf', name))

    def member(self, a):
        """a postfix chain on a leaf: .x / ->x / [k] / (game) .r4"""
        while True:
            k, v = self.peek()
            if k == 'op' and v in ('.', '->') and self.peek(1)[0] == 'id':
                self.take()
                n = self.take()[1]
                a = ('leaf', leaf_text(a) + '.' + n)
            elif k == 'op' and v == '[' and self.mode == 'cpp':
                self.take()
                ix = self.expr(0)
                self.take(']')
                a = ('leaf', leaf_text(a) + '[' + (fnum(ix[1]) if ix[0] == 'num' else canon(ix)) + ']')
            else:
                return a


def leaf_text(a):
    return a[1] if a[0] == 'leaf' else canon(a)


def fnum(v):
    return str(int(v)) if float(v).is_integer() and abs(v) < 1e15 else f'{v:.9g}'


GAME_TOK = re.compile(r'\s*(?:(?P<img>(?:0x[0-9A-Fa-f]+|\d+(?:\.\d+)?(?:e[+-]?\d+)?))\[(?P<at>0x[0-9A-Fa-f]+)\]'
                      r'|(?P<num>0x[0-9A-Fa-f]+|\d+(?:\.\d+)?(?:e[+-]?\d+)?)'
                      r'|(?P<id>\?vcall\([^)]*\)|\?loop(?:\(any sign\))?|&?local\[sp\+\d+\]|[A-Za-z_?][\w#]*(?:@\+?[0-9A-Fa-f]+)?)'
                      r'|(?P<op><<|>>|<=|>=|==|!=|&&|\|\||[-+*/&|^<>!()\[\],:.]))')
CPP_TOK = re.compile(r'\s*(?:(?P<num>0[xX][0-9A-Fa-f]+[uUlL]*|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?[fFuUlL]*)'
                     r'|(?P<id>[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)'
                     r'|(?P<op>->|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/&|^<>!?()\[\],:.]))')


def _tokens(text, rx, mode):
    out, i = [], 0
    text = text.strip()
    while i < len(text):
        m = rx.match(text, i)
        if not m or m.end() == i:
            if text[i:].strip() == '':
                break
            raise ExprError(f'cannot read {text[i:i + 30]!r} ({mode})')
        i = m.end()
        if m.groupdict().get('img') is not None:
            out.append(('img', _num(m.group('img'))))
        elif m.lastgroup == 'num':
            out.append(('num', _num(m.group('num'))))
        else:
            out.append((m.lastgroup, m.group(m.lastgroup)))
    return out


def _num(s):
    s = re.sub(r'(?<=[\d.])[fFuUlL]+$', '', s) if not s.lower().startswith('0x') else re.sub(r'[uUlL]+$', '', s)
    return float(int(s, 16)) if s.lower().startswith(('0x', '-0x')) else float(s)


def _split_top(s, sep):
    out, d, cur, i = [], 0, '', 0
    while i < len(s):
        ch = s[i]
        if ch in '([':
            d += 1
        elif ch in ')]':
            d -= 1
        if d == 0 and s.startswith(sep, i):
            out.append(cur)
            cur, i = '', i + len(sep)
            continue
        cur += ch
        i += 1
    return out + [cur]


def parse_game(text):
    """the slicer's --expr text -> tree. A phi(...) is cut out first (its arms split at the top-level ' | ', each at its first ': ')."""
    text = text.strip()
    if text.endswith('...'):
        raise ExprError('the slice text is clipped (...)')
    holes = {}
    while True:
        m = re.search(r'\bphi\(', text)
        if not m:
            break
        j = CD._match(text, m.end() - 1, '(', ')')
        arms = []
        for part in _split_top(text[m.end():j - 1], ' | '):   # the leftmost phi( is the outermost: its arms parse their own phis
            if part.strip() == '...':
                arms.append((('raw', '...'), ('leaf', '?phi-more')))
                continue
            cv = _split_top(part, ': ')
            c, v = ('?', part) if len(cv) == 1 else (cv[0], ': '.join(cv[1:]))
            cond = None if c.strip() == '?' else _try(lambda: parse_game(c), ('raw', c.strip()))
            arms.append((cond, parse_game(v)))
        key = f'phi_hole_{len(holes)}'
        holes[key] = ('phi', tuple(arms))
        text = text[:m.start()] + key + text[j:]
    p = _P(_tokens(text, GAME_TOK, 'game'), 'game')
    t = p.expr(0)
    if p.peek()[0] != 'end':
        raise ExprError(f'trailing text in {text!r}')
    return _subst_holes(t, holes)


def _subst_holes(t, holes):
    if t[0] == 'leaf' and t[1] in holes:
        return holes[t[1]]
    return _map(t, lambda x: _subst_holes(x, holes))


def _try(f, dflt):
    try:
        return f()
    except ExprError:
        return dflt


def parse_cpp(text, env=None, lambdas=None):
    p = _P(_tokens(text, CPP_TOK, 'cpp'), 'cpp', env, lambdas)
    t = p.expr(0)
    if p.peek()[0] != 'end':
        raise ExprError(f'trailing text in {text!r}')
    return t


def _map(t, f):
    k = t[0]
    if k in ('op', 'cmp'):
        return (k, t[1], f(t[2]), f(t[3]))
    if k in ('neg', 'not', 'ld'):
        return (k, f(t[1]))
    if k == 'fn':
        return (k, t[1], tuple(f(a) for a in t[2]))
    if k == 'phi':
        return (k, tuple((None if c is None else f(c), f(v)) for c, v in t[1]))
    return t


# ------------------------------------------------------------------ the normal form
def _poly(t):
    """-> {monomial: coef}; a monomial = tuple of (atom text, exponent), sorted"""
    k = t[0]
    if k == 'num':
        return {(): t[1]} if t[1] else {}
    if k == 'neg':
        return _scale(_poly(t[1]), -1.0)
    if k == 'op':
        o = t[1]
        a, b = _poly(t[2]), _poly(t[3])
        if o == '+':
            return _add(a, b)
        if o == '-':
            return _add(a, _scale(b, -1.0))
        if o == '*':
            return _mul(a, b)
        if o == '/' and _is_const(b) and _cval(b):
            return _scale(a, 1.0 / _cval(b))
        if o == '/' and len(b) == 1:
            (mono, c), = b.items()
            if c:
                return _mul(a, {tuple((x, -e) for x, e in mono): 1.0 / c})
        if o == '<<' and _is_const(b):
            return _scale(a, 2.0 ** int(_cval(b)))
        args = [canon(t[2]), canon(t[3])]
        if o in ('&', '|', '^') and 'no_commute' not in PLANT:
            args.sort()
        return _atom(f'{o}(' + ','.join(args) + ')')
    if k == 'fn':
        name = t[1]
        if name in ('rotl32',) and len(t[2]) == 2 and t[2][1] == ZERO:
            return _poly(t[2][0])
        args = [canon(a) for a in t[2]]
        if name in ('min', 'max', 'fmin', 'fmax', 'fminf', 'fmaxf') and 'no_commute' not in PLANT:
            args.sort()
        return _atom(f'{name}(' + ','.join(args) + ')')
    if k == 'ld':
        return _atom(ld_key(t))
    if k == 'phi':
        return _atom('phi(' + ' | '.join(f"{'else' if c is None else canon(c)}: {canon(v)}" for c, v in t[1]) + ')')
    if k == 'cmp':
        return _atom(f'({canon(t[2])} {t[1]} {canon(t[3])})')
    if k == 'not':
        return _atom(f'!({canon(t[1])})')
    if k == 'raw':
        return _atom('raw(' + t[1] + ')')
    return _atom(t[1])


def ld_key(t):
    inner = canon(t[1])
    if 'no_offset' in PLANT:
        inner = re.sub(r'(?:^|\s[+-]\s)\d+(?:\.\d+)?(?=$|\s)', '', inner).strip() or inner
    return '[' + inner + ']'


def _atom(s):
    return {((s, 1),): 1.0}


def _is_const(p):
    return not p or (len(p) == 1 and () in p)


def _cval(p):
    return p.get((), 0.0)


def _scale(p, c):
    return {m: v * c for m, v in p.items() if v * c}


def _add(a, b):
    out = dict(a)
    for m, v in b.items():
        out[m] = out.get(m, 0.0) + v
        if abs(out[m]) < 1e-12:
            del out[m]
    return out


def _mul(a, b):
    out = {}
    for ma, va in a.items():
        for mb, vb in b.items():
            ex = collections.Counter()
            for x, e in ma + mb:
                ex[x] += e
            m = tuple(sorted((x, e) for x, e in ex.items() if e))
            out[m] = out.get(m, 0.0) + va * vb
            if abs(out[m]) < 1e-12:
                del out[m]
    return out


def _mono_text(m):
    return '*'.join(x if e == 1 else f'{x}^{e}' for x, e in m)


def _poly_text(p):
    if not p:
        return '0'
    parts = []
    for m in sorted(p, key=_mono_text):
        c = p[m]
        mt = _mono_text(m)
        if not mt:
            parts.append(fnum(round(c, 9)))
        elif abs(c - 1.0) < 1e-12:
            parts.append(mt)
        else:
            parts.append(f'{fnum(round(c, 9))}*{mt}')
    return ' + '.join(parts)


def _raw_text(t):
    """the tree as written (the no_commute plant: no normal form)"""
    k = t[0]
    if k == 'num':
        return fnum(t[1])
    if k in ('op', 'cmp'):
        return f'({_raw_text(t[2])} {t[1]} {_raw_text(t[3])})'
    if k == 'neg':
        return f'-{_raw_text(t[1])}'
    if k == 'ld':
        return '[' + _raw_text(t[1]) + ']'
    if k == 'fn':
        return t[1] + '(' + ','.join(_raw_text(a) for a in t[2]) + ')'
    if k == 'phi':
        return 'phi(' + ' | '.join(f"{'else' if c is None else _raw_text(c)}: {_raw_text(v)}" for c, v in t[1]) + ')'
    if k == 'not':
        return '!' + _raw_text(t[1])
    return str(t[1])


def canon(t):
    if 'no_commute' in PLANT:
        return _raw_text(t)
    return _poly_text(_poly(t))


def leaves(t, out=None):
    """the value leaves of a tree (a load is one leaf; a phi's conditions are not values)"""
    out = set() if out is None else out
    k = t[0]
    if k == 'leaf':
        out.add(t[1])
    elif k == 'ld':
        out.add(ld_key(t))
    elif k in ('op', 'cmp'):
        leaves(t[2], out)
        leaves(t[3], out)
    elif k in ('neg', 'not'):
        leaves(t[1], out)
    elif k == 'fn':
        for a in t[2]:
            leaves(a, out)
    elif k == 'phi':
        for _c, v in t[1]:
            leaves(v, out)
    return out


def cut_leaf(x):
    """a leaf the slicer could not follow: ?loop / ?vcall / ?rN / ?phi-more (also inside a load's address)"""
    return '?' in x


def is_context(x):
    return x.startswith('ours:')


# ------------------------------------------------------------------ evaluation (same leaves, sampled)
class _NoEval(Exception):
    pass


def _ev(t, env):
    k = t[0]
    if k == 'num':
        return t[1]
    if k in ('leaf',):
        return env[t[1]]
    if k == 'ld':
        return env[ld_key(t)]
    if k == 'neg':
        return -_ev(t[1], env)
    if k == 'not':
        return 0.0 if _ev(t[1], env) else 1.0
    if k in ('op', 'cmp'):
        o = t[1]
        if o == '&&':
            return 1.0 if (_ev(t[2], env) and _ev(t[3], env)) else 0.0
        if o == '||':
            return 1.0 if (_ev(t[2], env) or _ev(t[3], env)) else 0.0
        a, b = _ev(t[2], env), _ev(t[3], env)
        if o == '+':
            return a + b
        if o == '-':
            return a - b
        if o == '*':
            return a * b
        if o == '/':
            if b == 0:
                raise ZeroDivisionError
            return a / b
        if o == '<<':
            return a * 2.0 ** b
        cmpf = {'<': a < b, '<=': a <= b, '>': a > b, '>=': a >= b, '==': a == b, '!=': a != b}
        if o in cmpf:
            return 1.0 if cmpf[o] else 0.0
        raise _NoEval(o)
    if k == 'fn':
        n = t[1]
        args = [_ev(a, env) for a in t[2]]
        if n in ('min', 'fmin', 'fminf') and len(args) == 2:
            return min(args)
        if n in ('max', 'fmax', 'fmaxf') and len(args) == 2:
            return max(args)
        if n in ('abs', 'fabs', 'fabsf') and len(args) == 1:
            return abs(args[0])
        if n == 'rotl32' and len(args) == 2 and args[1] == 0:
            return args[0]
        raise _NoEval(n)
    if k == 'phi':
        for c, v in t[1]:
            if c is None or _ev(c, env):
                return _ev(v, env)
        raise _NoEval('no phi arm holds')
    raise _NoEval(k)


POOL = (-3000.0, -1500.0, -2.0, -1.0, -0.5, 0.0, 0.25, 0.5, 1.0, 2.0, 7.0, 999.0, 1000.0, 1001.0, 1500.0, 3000.0)


def same_by_eval(g, o):
    """True when g and o evaluate equal at every sample point (the same leaves, one value each); None = cannot be evaluated"""
    if 'no_eval' in PLANT:
        return None
    vals = leaves(g) | leaves(o)
    ks = sorted(vals | _cond_leaves(g) | _cond_leaves(o))
    rng = random.Random(20261127)
    hit = 0
    for _ in range(SAMPLES):
        # a leaf only a condition reads (a pointer / a switch: c.rim_params) is 0 a third of the time, so both of its arms are met
        env = {x: (0.0 if x not in vals and rng.random() < 0.34 else rng.choice(POOL) if rng.random() < 0.6 else rng.uniform(-3000, 3000))
               for x in ks}
        try:
            a, b = _ev(g, env), _ev(o, env)
        except ZeroDivisionError:
            continue
        except (_NoEval, KeyError):
            return None
        if not (math.isfinite(a) and math.isfinite(b)):
            continue
        hit += 1
        if not math.isclose(a, b, rel_tol=1e-6, abs_tol=1e-9):
            return False
    return True if hit else None


def _cond_leaves(t, out=None):
    out = set() if out is None else out
    if t[0] == 'phi':
        for c, v in t[1]:
            if c is not None:
                leaves(c, out)
                _cond_leaves(c, out)
            _cond_leaves(v, out)
    elif t[0] in ('op', 'cmp'):
        _cond_leaves(t[2], out)
        _cond_leaves(t[3], out)
    elif t[0] in ('neg', 'not', 'ld'):
        _cond_leaves(t[1], out)
    elif t[0] == 'fn':
        for a in t[2]:
            _cond_leaves(a, out)
    return out


# ------------------------------------------------------------------ the compare
RANK = {'OK': 4, 'DIFF': 3, 'GAME-ONLY LEAF': 2, 'UNSETTLED': 1}


def judge(g, o):
    """-> (verdict, why) of the game's tree g against ours o (None = ours writes nothing)"""
    if o is None:
        return 'GAME-ONLY LEAF', 'ours never writes it (no live branch: the register keeps the last draw\'s value)'
    gl = leaves(g)
    cut = sorted(x for x in gl if cut_leaf(x))
    if cut and 'no_loop' not in PLANT:
        return 'UNSETTLED', 'the game value has a leaf the slicer cut: ' + ', '.join(cut[:3])
    v, why = judge1(g, o)
    if v == 'OK' or o[0] != 'phi':
        return v, why
    for i, (c, arm) in enumerate(o[1]):
        av, aw = judge(g, arm)          # an arm that is itself a phi (an inlined lambda inside a branch) is searched in turn
        if av == 'OK':
            rest = [f"{'else' if cc is None else canon(cc)}: {canon(vv)}" for j, (cc, vv) in enumerate(o[1]) if j != i]
            return 'OK', (f"ours arm {'else' if c is None else canon(c)} equals the game's{'; ' + aw if aw else ''}; ours' other arm(s): "
                          + '; '.join(rest))
    return v, why


def judge1(g, o):
    cg, co = canon(g), canon(o)
    if cg == co:
        return 'OK', ''
    gl, ol = leaves(g), leaves(o)
    missing, extra = sorted(gl - ol), sorted(ol - gl)
    ctx = [x for x in extra if is_context(x)]
    if missing and 'no_factor' not in PLANT:
        if ctx:
            return 'UNSETTLED', (f"ours reads {', '.join(ctx[:3])} (our context: no binding ties it to the game's "
                                 f"{', '.join(missing[:3])})")
        if extra:
            return 'DIFF', f"leaf differs: the game reads {', '.join(missing[:3])}, ours {', '.join(extra[:3])}"
        return 'GAME-ONLY LEAF', f"the game reads {', '.join(missing[:4])}, ours never does (ours = {co})"
    if extra:
        return 'DIFF', f"ours-only leaf {', '.join(extra[:3])} (the game's = {cg})"
    s = same_by_eval(g, o)
    if s:
        return 'OK', f'(by evaluation: equal at {SAMPLES} sample points; the normal forms differ) [L]'
    return 'DIFF', describe(g, o) + ('' if s is None else '; unequal at a sample point')


def describe(g, o):
    pg, po = _poly(g), _poly(o)
    only_g = [m for m in pg if m not in po]
    only_o = [m for m in po if m not in pg]
    coef = [m for m in pg if m in po and not math.isclose(pg[m], po[m], rel_tol=1e-6)]
    parts = []
    if coef:
        parts.append('coefficient: ' + '; '.join(f'{_mono_text(m) or "constant"}: game {fnum(pg[m])}, ours {fnum(po[m])}' for m in coef[:3]))
    if only_g:
        parts.append('game term(s) ' + '; '.join(_poly_text({m: pg[m]}) for m in only_g[:3]))
    if only_o:
        parts.append('ours term(s) ' + '; '.join(_poly_text({m: po[m]}) for m in only_o[:3]))
    return 'operator / coefficient differs: ' + ('; '.join(parts) or f'game {canon(g)}, ours {canon(o)}')


# ------------------------------------------------------------------ leaf binding
def substitute(t, table):
    """replace every subtree whose normal form is a key of `table` (normal form -> tree)"""
    if table:
        c = canon(t) if t[0] not in ('num',) else None
        if c in table:
            return table[c]
    return _map(t, lambda x: substitute(x, table))


def bind_table(texts, parse):
    """{text: bound name | value} -> {normal form: tree}"""
    out = {}
    for txt, nm in texts.items():
        if nm is None:
            continue
        out[canon(parse(txt))] = ('num', float(nm)) if isinstance(nm, (int, float)) else ('leaf', 'bind:' + nm)
    return out


def ours_leaves_bound(t, effect, mat_bind):
    """ours leaves -> one space: mt.X = the material member (bound to the game's name by mat_bind); c.X / ls.X / ... = ours: context"""
    def f(x):
        if x[0] == 'leaf':
            s = x[1]
            if s.startswith('bind:'):
                return x
            if s.startswith(('mt.', 'mat.')):
                return ('leaf', 'mt:' + s.split('.', 1)[1])
            return ('leaf', 'ours:' + s)
        return _map(x, f)
    return f(t)


def load_mat_bind(read):
    """-> {family ('ych' | 'other'): {parameter name: ours member text}} from material_floats.h ApplyFloat (else jboy.h)"""
    text = None
    for p in ('src/assets/material_floats.h', 'src/assets/jboy.h'):
        try:
            text = read(p)
        except (OSError, subprocess.CalledProcessError, ExprError):
            continue
        if 'is("g_' in text:
            break
    if not text:
        raise ExprError('no material name table (material_floats.h / jboy.h) to bind the parameters')
    text = CD._mask(text)
    out = {'ych': {}, 'other': {}}

    def put(names, member, comps):
        for fam in ('ych', 'other'):
            nm = names[0] if (fam == 'ych' or len(names) == 1) else names[1]
            for i, m in enumerate(comps):
                out[fam].setdefault(f'{nm}[{i}]', m)
    three = {x for ln in text.split('\n') if re.search(r'\?\s*3\s*:\s*4', ln) for x in re.findall(r'dst\s*==\s*mat\.(\w+)', ln)}
    # the name test: is("N") / is(mat.ych_table ? "A" : "B") (material_floats.h), or pname == "N" (jboy.h before GUESTHAIR)
    nt = r'(?:is\((?:mat\.ych_table\s*\?\s*"(\w+)"\s*:\s*"(\w+)"|"(\w+)")\)|pname\s*==\s*"(\w+)")'

    def names_of(m):
        return [m.group(1), m.group(2)] if m.group(1) else [m.group(3) or m.group(4)]
    for m in re.finditer(nt + r'\s*\?\s*mat\.(\w+)', text):
        mem = m.group(5)
        put(names_of(m), mem, [f'{mem}[{i}]' for i in range(3 if mem in three else 4)])
    for m in re.finditer(nt + r'\s*\)\s*(?:\{\s*)?(?:for\s*\([^)]*\)\s*)?mat\.(\w+)(\[(\w+)\])?\s*=\s*(?:v\[|x\b|r\.f32)', text):
        mem, ix = m.group(5), m.group(7)
        if ix is not None and not ix.isdigit():      # a loop over the components: the whole vec4
            put(names_of(m), mem, [f'{mem}[{i}]' for i in range(4)])
        else:
            put(names_of(m), mem, [f'{mem}[{ix}]' if ix is not None else mem])
    for m in re.finditer(r'NameStarts\(n,\s*"(\w+_)"[^;]*?mat\.(\w+)\[', text):
        for i, s in enumerate('xyzw'):
            put([m.group(1) + s], m.group(2), [f'{m.group(2)}[{i}]'])
    return out


def game_mat_leaf(name, idx, effect, mat_bind):
    fam = 'ych' if effect.lower().startswith('ych') else 'other'
    m = mat_bind[fam].get(f'{name}[{idx}]')
    return ('leaf', 'mt:' + m) if m else ('leaf', f'mat:{name}[{idx}]')


# ------------------------------------------------------------------ ours: a resolver branch, lane by lane
LAMBDA_RE = re.compile(r'\bconst\s+auto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(([^)]*)\)\s*\{\s*return\s+([^;]+);\s*\}\s*;')


def resolver_lambdas(text):
    """{name: ([params], body)} of the resolver's one-statement return-lambdas (bake_pow), inlined at each call"""
    out = {}
    for m in LAMBDA_RE.finditer(text):
        ps = [re.sub(r'.*?(\w+)\s*$', r'\1', p) for p in m.group(2).split(',') if p.strip()]
        out[m.group(1)] = (ps, m.group(3))
    return out


def resolver_writers(text):
    """the resolver's lambdas (other than set4) whose body writes `out` (front_end.cpp `rows`): a call of one is an unsplit write"""
    out = set()
    for ml in re.finditer(r'\bconst\s+auto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(', text):
        lb = text.find('{', CD._match(text, ml.end() - 1, '(', ')'))
        if ml.group(1) != 'set4' and lb >= 0 and re.search(r'(?<![\w.])out\b', text[lb:CD._match(text, lb, '{', '}')]):
            out.add(ml.group(1))
    return out


def _clean(s):
    return re.sub(r'\b(?:assets|std|rdr_scene|sphere_join|live|change_colour)::', '', s)


class Branch:
    """walks one branch's statements: lanes {(vec4, lane): tree}; an `out` write it cannot split marks the branch `opaque`"""

    def __init__(self, lambdas, cond_free):
        self.lambdas = lambdas
        self.cond_free = cond_free
        self.opaque = None

    def run(self, text):
        lanes, _ret = self.block(self.stmts(_clean(text)), {}, {})
        return lanes

    def stmts(self, s):
        out, i = [], 0
        n = len(s)
        while i < n:
            while i < n and s[i] in ' \t\r\n;':
                i += 1
            if i >= n:
                break
            if s[i] == '{':
                j = CD._match(s, i, '{', '}')
                out.append(('block', self.stmts(s[i + 1:j - 1])))
                i = j
                continue
            m = re.match(r'(if|for|while)\s*\(', s[i:])
            if m:
                cp = i + m.end() - 1
                ce = CD._match(s, cp, '(', ')')
                head = s[cp + 1:ce - 1]
                body_end = self._one(s, ce)
                body = s[ce:body_end]
                if m.group(1) != 'if':
                    out.append(('opaque', s[i:body_end]))
                    i = body_end
                    continue
                els = None
                me = re.match(r'\s*else\b', s[body_end:])
                if me:
                    k = body_end + me.end()
                    ee = self._one(s, k)
                    els = s[k:ee]
                    body_end = ee
                out.append(('if', head, self.stmts(body), self.stmts(els) if els is not None else None))
                i = body_end
                continue
            j = self._one(s, i)
            out.append(('expr', s[i:j].strip().rstrip(';')))
            i = j
        return out

    @staticmethod
    def _one(s, i):
        while i < len(s) and s[i].isspace():
            i += 1
        if i < len(s) and s[i] == '{':
            return CD._match(s, i, '{', '}')
        m = re.match(r'(if|for|while)\s*\(', s[i:])
        if m:
            ce = CD._match(s, i + m.end() - 1, '(', ')')
            e = Branch._one(s, ce)
            me = re.match(r'\s*else\b', s[e:]) if m.group(1) == 'if' else None
            return Branch._one(s, e + me.end()) if me else e
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

    def block(self, sts, lanes, env):
        """-> (lanes, returned)"""
        lanes, env = dict(lanes), dict(env)
        for i, st in enumerate(sts):
            if st[0] == 'block':
                lanes, r = self.block(st[1], lanes, env)
                if r:
                    return lanes, True
            elif st[0] == 'opaque':
                self.unsplit(lanes, st[1])
            elif st[0] == 'if':
                cond = _try(lambda: parse_cpp(self.cond_free(st[1]), env, self.lambdas), ('raw', st[1]))
                tl, tr = self.block(st[2], lanes, env)
                rest = sts[i + 1:]
                if st[3] is not None:
                    el, er = self.block(st[3], lanes, env)
                else:
                    el, er = lanes, False
                if tr and not er:
                    el, er = self.block(rest, el, env)
                    return self.merge(cond, tl, el), True
                if er and not tr:
                    tl, tr = self.block(rest, tl, env)
                    return self.merge(cond, tl, el), True
                lanes = self.merge(cond, tl, el)
                if tr and er:
                    return lanes, True
            else:
                if self.expr_stmt(st[1], lanes, env):
                    return lanes, True
        return lanes, False

    def unsplit(self, lanes, text):
        """a write of `out` this walker cannot split (a loop, a helper, a memcpy of a computed source): every lane this path has not
        written yet takes it, as one ours leaf (`*` = the path's default lane)"""
        self.opaque = re.sub(r'\s+', ' ', text)[:60]
        lanes['*'] = ('leaf', 'ours?:' + self.opaque)

    @staticmethod
    def merge(cond, a, b):
        out = {}
        for k in set(a) | set(b):
            x, y = a.get(k, a.get('*', ZERO)), b.get(k, b.get('*', ZERO))
            out[k] = x if x == y else ('phi', ((cond, x), (None, y)))
        return out

    def expr_stmt(self, s, lanes, env):
        """one expression statement (top-level commas split); True = it returns"""
        if re.match(r'return\b', s):
            return True
        m = re.match(r'(?:const\s+)?(?:float|double|bool|int|uint32_t|auto)\s+(\w+)\s*(\[[^\]]*\])*\s*=\s*(.+)$', s, re.S)
        if m and not m.group(3).lstrip().startswith('['):
            nm, rhs = m.group(1), m.group(3)
            if m.group(2):
                vals = _split_top(rhs.strip().strip('{}'), ',') if rhs.strip().startswith('{') else []
                for k, v in enumerate(vals):
                    env[f'{nm}[{k}]'] = _try(lambda: parse_cpp(v, env, self.lambdas), ('leaf', f'{nm}[{k}]'))
                return False
            env[nm] = _try(lambda: parse_cpp(rhs, env, self.lambdas), ('leaf', nm))
            return False
        if re.match(r'(?:const\s+)?(?:float|double|bool|int|uint32_t|auto)\s+\w+', s):
            return False
        for part in _split_top(s, ','):
            p = part.strip()
            if any(re.match(re.escape(w) + r'\s*\(', p) for w in self.lambdas.get('__writers__', ())):
                self.unsplit(lanes, p)
                continue
            mm = re.match(r'set4\s*\((.*)\)$', p, re.S)
            if mm:
                args = _split_top(mm.group(1), ',')
                if len(args) == 5 and args[0].strip().isdigit():
                    for i, a in enumerate(args[1:]):
                        lanes[(int(args[0]), i)] = self.val(a, env)
                    continue
                self.unsplit(lanes, p)
                continue
            mm = re.match(r'out\[(\d+)\]\s*=(?!=)\s*(.+)$', p, re.S)
            if mm:
                n = int(mm.group(1))
                lanes[(n // 4, n % 4)] = self.val(mm.group(2), env)
                continue
            mm = re.match(r'memcpy\s*\(\s*out\s*,(.*)\)$', p, re.S)
            if mm:
                args = _split_top(mm.group(1), ',')
                src = _try(lambda: parse_cpp(args[0], env, self.lambdas), None)
                nb = _try(lambda: parse_cpp(args[1], env, self.lambdas), None) if len(args) > 1 else None
                if src is not None and src[0] == 'leaf' and nb is not None and nb[0] == 'num':
                    for j in range(int(nb[1]) // 4):
                        lanes[(j // 4, j % 4)] = ('leaf', f'{src[1]}[{j}]')
                    continue
                self.unsplit(lanes, p)
                continue
            if re.search(r'(?<![\w.])out\b', p):
                self.unsplit(lanes, p)
        return False

    def val(self, text, env):
        return _try(lambda: parse_cpp(text, env, self.lambdas), ('leaf', 'ours?:' + re.sub(r'\s+', ' ', text.strip())[:60]))


def cond_free(text):
    """a branch condition with its name compares made true (the field is the one asked for)"""
    t = re.sub(r'name\s*==\s*"\w+"', 'true', text)
    return t


def branch_expr(b, lambdas):
    """-> ({(vec4, lane): tree}, opaque text | None) of one live branch"""
    if b['kind'] == 'ZERO':
        return {}, None
    w = Branch(lambdas, cond_free)
    lanes = w.run(b['stmt'])
    return lanes, w.opaque


def ours_lane(brs, lane, lambdas):
    """the ours tree of one lane over the live branches of a field (a phi in source order); None = no live branch"""
    live = [b for b in (brs or []) if not b['plant'] and b['kind'] != 'FALSE']
    if not live:
        return None, []
    arms, at = [], []
    for b in live:
        lanes, opaque = branch_expr(b, lambdas)
        v = lanes.get(lane, lanes.get('*', ZERO))
        cond = _try(lambda: parse_cpp(_clean(cond_free(b['cond'])), {}, lambdas), ('raw', b['cond'])) if len(live) > 1 else None
        arms.append((cond, v))
        at.append(f"{b['path']}:{b['line']}")
    if len(arms) == 1:
        return arms[0][1], at
    return ('phi', tuple(arms[:-1]) + ((None, arms[-1][1]),)), at


# ------------------------------------------------------------------ the game side
class GameSlices:
    """q slice --expr at a site (tools/q/qslice.py; its own per-function cache): -> tree"""

    def __init__(self, con):
        import qslice
        import q as Q
        self.S, self.Q, self.con = qslice, Q, con
        self.src = None
        self.memo = {}
        self.ro_end = int(Q.meta(con).get('code_range', [0, 0])[1]) if con is not None else 0
        self.image = None

    def text(self, site, operand):
        k = (site, operand)
        if k not in self.memo:
            if self.src is None:
                self.src = self.S.QSource(self.con)
            fn, rel = self.S.parse_site(site)
            addr = self.Q.resolve(self.con, fn)[0]
            _root, expr, _head, _sl = self.S.slice_site(self.src, addr, rel, operand, depth=0, budget=40)
            self.memo[k] = expr
        return self.memo[k]

    def tree(self, site, operand):
        return self.image_words(parse_game(self.text(site, operand)))

    def image_words(self, t):
        """an absolute load below the code range end = the image's own word (a float constant)"""
        if t[0] == 'ld' and t[1][0] == 'num' and self.ro_end and t[1][1] < self.ro_end:
            if self.image is None:
                import qdecomp
                self.image = qdecomp.Image()
            w = self.image.word(int(t[1][1]))
            if w is not None:
                import struct
                return ('num', struct.unpack('>f', struct.pack('>I', w))[0])
        return _map(t, self.image_words)


# ------------------------------------------------------------------ the rows
def git_reader(ref):
    if not ref:
        return lambda p: open(os.path.join(REPO, p), encoding='utf-8').read()

    def rd(p):
        r = subprocess.run(['git', '-C', REPO, 'show', f'{ref}:{p}'], capture_output=True, text=True, encoding='utf-8', errors='replace')
        if r.returncode != 0:
            raise OSError(f'{ref}:{p}: {r.stderr.strip()}')
        return r.stdout
    return rd


def name_width(n):
    """the floats a name holds per register, from its type prefix: g_f / g_i / g_b + a capital = 1, g_f2 = 2, g_f3 = 3, else 4. A whole-vec4
    row (lane word 0) of a narrower name uploads the words after the value as its other lanes: those lanes are not a value the game
    names, so they are not compared [L: from the names, as qconstdiff.LANES reads x..z from the g_f3 names]"""
    if re.match(r'g_[fib][A-Z]', n):
        return 1
    m = re.match(r'g_f([23])[A-Za-z]', n)
    return int(m.group(1)) if m else 4


def lane_txt(k):
    return ('' if k[0] == 0 else f'[{k[0]}]') + 'xyzw'[k[1]]


def name_rows(con, read, k, game, mat_bind, problems):
    """the name-table inputs, lane by lane"""
    lam = {}
    for key, path, fre in CD.RESOLVERS:
        txt = CD._mask(read(path))
        lam[key] = resolver_lambdas(txt)
        lam[key]['__writers__'] = resolver_writers(txt)   # (not an identifier: never inlined)
    out = []
    for r in k.rows:
        brs = k.resolvers[r['resolver']].get(r['field'])
        src_flag = 'DIFF' if r['diff'] else 'PARTIAL' if r['partial'] else 'PASS-NEVER' if r['pass_never'] else 'CONST' if r['const_eq'] else '-'
        for gr in r['game_rows']:
            if gr['kind'] not in ('GAME-MATERIAL', 'GAME-CODE', 'GAME-CONST'):
                continue
            vals = gr.get('values') or []
            for li, ln in enumerate(gr['lanes']):
                vec, lane = ln
                inp = f"{r['effect']}:{r['stage']}.c{r['reg'] + vec}.{'xyzw'[lane]}"
                idx = 4 * (vec - (gr['reg'] - r['reg'])) + lane if gr['comp'] == 0 or gr['comp'] == 11 else 0
                if gr['comp'] == 0 and lane >= name_width(gr['name']):
                    k.past_width = getattr(k, 'past_width', 0) + 1
                    continue
                g, gwhy, gat = None, '', gr['site']
                if gr['kind'] == 'GAME-MATERIAL':
                    g = game_mat_leaf(gr['name'], idx, r['effect'], mat_bind)
                    gwhy = f"material parameter {gr['name']}[{idx}]" + (f" (ours: Material::{g[1][3:]})" if g[1].startswith('mt:') else ' (our Material table never reads it)')
                elif gr['kind'] == 'GAME-CONST':
                    g = ('num', float(vals[li])) if li < len(vals) else None
                    gwhy = 'the image constant (GAME-CONST chain)'
                else:
                    spec = CODE_SLICES.get((gr['name'], idx))
                    if spec:
                        try:
                            g = game.tree(spec['site'], spec['operand'])
                            g = substitute(g, bind_table(spec['bind'], parse_game))
                            g = substitute(g, bind_table(spec['consts'], parse_game))
                            gwhy, gat = f"q slice {spec['site']} {spec['operand']}: {game.text(spec['site'], spec['operand'])}", spec['site']
                        except Exception as e:  # noqa: BLE001 - a declared slice that no longer reads is a PROBLEM, never a value
                            problems.append(f"CODE_SLICES {gr['name']}[{idx}]: {type(e).__name__}: {e}")
                            g = None
                    if g is None:
                        g = ('leaf', f"code:{gr['name']}[{idx}]")
                        gwhy = gwhy or f"set by the game's code ({gr['why']}); no slice declared"
                if g is None:
                    continue
                if r['pass_never']:
                    v, why, o, at = 'PASS-NEVER', r['pass_never'], None, []
                else:
                    o, at = ours_lane(brs, ln, lam[r['resolver']])
                    if o is not None:
                        spec = CODE_SLICES.get((gr['name'], idx)) if gr['kind'] == 'GAME-CODE' else None
                        if spec:
                            o = substitute(o, bind_table({t: n for t, n in spec['ours'].items() if n}, parse_cpp))
                        o = ours_leaves_bound(o, r['effect'], mat_bind)
                    v, why = judge(g, o)
                out.append({'input': inp, 'field': f'expr:{inp}', 'effect': r['effect'], 'stage': r['stage'], 'reg': r['reg'] + vec,
                            'lane': 'xyzw'[lane], 'name': r['field'], 'game_name': gr['name'], 'game_kind': gr['kind'], 'source': src_flag,
                            'verdict': v, 'why': why, 'game': canon(g), 'game_why': gwhy, 'game_at': gat,
                            'ours': None if o is None else canon(o), 'ours_at': at, 'resolver': r['resolver'],
                            '_g': g, '_o': o, '_gn': (gr['name'], idx) if gr['kind'] == 'GAME-MATERIAL' else None})   # (_ keys: trees for --values, never printed)
    return out


def draw_rows(read, game, problems):
    out = []
    for d in DRAW_INPUTS:
        try:
            g = game.tree(d['site'], d['operand'])
            tbl = bind_table(d['bind'], parse_game)
            for nm, (site, opnd) in d.get('bind_slices', {}).items():
                tbl[canon(game.tree(site, opnd))] = ('leaf', 'bind:' + nm)
            g = substitute(g, tbl)
            text = CD._mask(read(d['ours_file']))
            m = re.search(d['anchor'], text)
            if not m:
                raise ExprError(f"{d['ours_file']}: the anchor {d['anchor']!r} matches nothing")
            line = text.count('\n', 0, m.start()) + 1
            o = parse_cpp(_clean(m.group(1)))
            o = substitute(o, bind_table(d['ours'], parse_cpp))
            o = ours_leaves_bound(o, '', {'ych': {}, 'other': {}})
            v, why = judge(g, o)
        except Exception as e:  # noqa: BLE001 - a declared input that no longer reads is a PROBLEM
            problems.append(f"DRAW_INPUTS {d['input']}: {type(e).__name__}: {e}")
            continue
        out.append({'input': d['input'], 'field': f"expr:{d['input']}", 'effect': '-', 'stage': 'vs', 'reg': None, 'lane': None,
                    'name': d['input'], 'game_name': d['site'], 'game_kind': 'GAME-SLICE', 'source': '-', 'verdict': v, 'why': why,
                    'game': canon(g), 'game_why': f"q slice {d['site']} {d['operand']}: {game.text(d['site'], d['operand'])}; {d['why']}",
                    'game_at': d['site'], 'ours': canon(o), 'ours_at': [f"{d['ours_file']}:{line} ({d['ours_fn']})"], 'resolver': '-'})
    return out


def collect(con, ref=None, k=None, game=None):
    """-> (rows, problems, k): k = the constdiff join (qconstdiff.collect) at the tree / ref"""
    read = git_reader(ref)
    if k is None:
        k = CD.collect(con, 'game', None if ref is None else read)
    problems = list(k.problems)
    game = game or GameSlices(con)
    mat_bind = load_mat_bind(read)
    rows = name_rows(con, read, k, game, mat_bind, problems) + draw_rows(read, game, problems)
    return rows, list(dict.fromkeys(problems)), k


def facts(rows):
    out = []
    for r in rows:
        out.append({'side': 'game', 'fn': r['game_at'], 'field': r['field'], 'value': r['game'], 'cond': None, 'at': r['game_at'],
                    'kind': 'expr', 'verdict': r['verdict']})
        out.append({'side': 'ours', 'fn': r['resolver'], 'field': r['field'], 'value': r['ours'], 'cond': None,
                    'at': (r['ours_at'] or [None])[0], 'kind': 'expr', 'verdict': r['verdict']})
    return out


def groups(rows):
    """one line per (verdict, our line(s), field name, lane, game name, why): the same fault in many effects is one lead"""
    g = collections.OrderedDict()
    for r in rows:
        key = (r['verdict'], tuple(r['ours_at']), r['name'], r['lane'], r['game_name'], r['why'])
        g.setdefault(key, []).append(r)
    return g


def input_match(r, pat):
    """--input: a glob over the input (effect:stage.cREG.lane), or a constant's name: exact (Reflect, Reflect.w, g_f4Ref_COL) or a prefix of it, whatever the effect"""
    if fnmatch.fnmatchcase(r['input'], pat):
        return True
    p = pat.lower()
    cands = [r['name'], f"{r['name']}.{r['lane']}" if r['lane'] else r['name'], r['game_name']]
    if any(c.lower().startswith(p) for c in cands if c):
        return True
    return any(ch in pat for ch in '*?[') and any(fnmatch.fnmatchcase(c, pat) for c in cands if c)


def values_for(groups_, read, con_values=None):
    """--values: {group key: qexprvals result} (the asset index; ours' Material defaults from our code at the ref)"""
    import qexprvals as V
    recs = V.Records(V.open_index())
    mb = load_mat_bind(read)
    try:
        defaults = V.material_defaults(CD._mask(read('src/assets/asset_model.h')))
    except OSError:
        defaults = {}
    ctx = V.Ctx(V.invert(mb), defaults)
    import sys as _s
    return {k: V.group_values(_s.modules[__name__], rs, recs, ctx) for k, rs in groups_.items()}


def main(argv, con):
    ap = argparse.ArgumentParser(prog='q exprdiff', description=__doc__.split('\n')[0])
    ap.add_argument('--effect', help='an effect name or a glob (yCh_*)')
    ap.add_argument('--input', help='an input (effect:stage.cREG.lane) or a glob, or a constant\'s name (exact or a prefix: Reflect, Reflect.w, g_f4Ref), or a draw input\'s name')
    ap.add_argument('--values', action='store_true', help='evaluate both sides on every record of the covered effects in the asset index: DIFF, N of M records differ')
    ap.add_argument('--flag', nargs='+', choices=VERDICTS + ('GAME-ONLY',), help='only these verdicts (default: DIFF, GAME-ONLY LEAF)')
    ap.add_argument('--all', action='store_true', help='every verdict')
    ap.add_argument('--ref', help='our code at this git ref (the game side is the game)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--facts', action='store_true', help='the rows as code-diff facts (JSON)')
    a = ap.parse_args(argv)
    rows, problems, _k = collect(con, a.ref)
    flags = {'GAME-ONLY LEAF' if f == 'GAME-ONLY' else f for f in (a.flag or ())} or (set(VERDICTS) if a.all else {'DIFF', 'GAME-ONLY LEAF'})
    sel = [r for r in rows if r['verdict'] in flags and (not a.effect or fnmatch.fnmatchcase(r['effect'], a.effect))
           and (not a.input or input_match(r, a.input))]
    tot = collections.Counter(r['verdict'] for r in rows)
    if a.facts:
        print(json.dumps(facts(sel), indent=1))
        return 2 if problems else 0
    gs = groups(sel)
    vals = values_for(gs, git_reader(a.ref)) if a.values else {}
    if a.json:
        out = {'ref': a.ref or 'the working tree', 'rows': [{k: v for k, v in r.items() if not k.startswith('_')} for r in sel], 'totals': dict(tot),
               'problems': problems}
        if a.values:
            out['values'] = [dict(verdict=k[0], field=k[2], lane=k[3], game_name=k[4], **{x: (sorted(y) if isinstance(y, set) else y) for x, y in v.items()})
                             for k, v in vals.items()]
        print(json.dumps(out, indent=1))
        return 2 if problems else 0
    for (v, at, name, lane, gname, why), rs in gs.items():
        effs = sorted({r['effect'] for r in rs})
        r = rs[0]
        print(f"{v:14s} {name}.{lane or '-'}  {gname} ({r['game_kind']})  [{len(rs)} input(s), {len(effs)} effect(s): "
              + ', '.join(effs[:3]) + (' ...' if len(effs) > 3 else '') + f"; constdiff {'/'.join(sorted({x['source'] for x in rs}))}]")
        print(f"               why:  {why}")
        print(f"               game: {r['game']}   <- {r['game_why'][:220]}")
        print(f"               ours: {r['ours']}   @ {', '.join(at) or 'no branch'}")
        if a.values:
            import qexprvals as V
            for ln in V.lines(vals[(v, at, name, lane, gname, why)], v):
                print('               ' + ln)
    nd = len({k for k in gs})
    print(f"totals ({a.ref or 'the working tree'}): {len(rows)} lanes; " + ', '.join(f'{v} {tot.get(v, 0)}' for v in VERDICTS)
          + f"; shown {len(sel)} in {nd} group(s) (verdict, our line, field, lane, game name); "
          f"{getattr(_k, 'past_width', 0)} lanes past their name's width not compared (name_width)")
    for p in problems:
        print('PROBLEM: ' + p)
    return 2 if problems else 0
