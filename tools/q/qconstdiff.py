r"""q constdiff: per GAME shader input our renderer feeds, where the GAME gets the value against where OURS gets it (S111 CONSTDIFF).

    py tools/q/q.py constdiff [--effect E] [--kind LITERAL|OURS|GAME|UNFED|PASS-NEVER] [--diff] [--partial] [--json]

One row per (effect, stage, register) that our by-name resolvers feed (the fx.json constants of every effect whose draws go through
ResolveChar or the front end's Resolve). Columns: the field, the GAME's source, OURS's source, and the flag DIFF when the game takes
the value from a material record or from code but ours is a LITERAL (a constant in our code, zero included) or UNFED (no branch:
the resolver returns false), or when a game row that writes only some lanes (x / y / z / w alone) gets literals in every one of them
from ours (DIFF by lane). PARTIAL (not a DIFF): some lanes of a game-fed register literal in ours, or a live literal arm beside the fed
one. Totals per flag at the end.

Sources of truth (nothing guessed, nothing committed from the game):
  ours  - src/renderer/shaders/fx/*.fx.json: effect, stage, field, register, count (our registry of the game's shaders);
        - the resolvers' own text: every `if (...)` whose condition names `name == "X"` in ResolveChar
          (src/renderer_scene/char_effects.cpp) and Resolve (src/renderer_scene/front_end.cpp); a branch's kind is read from
          the roots its body reads (ROOTS below: `mt` = the material file values = GAME, `c` / `ls` / ... = our context = OURS,
          none = LITERAL); a branch gated by a dev plant (a getenv switch or a *Plant() call in its condition) is skipped;
        - which resolver an effect goes to: IsCharEffect (char_effects.cpp: "yCh_" prefix -> ResolveChar), else Resolve for the
          front end's classes (FRONT_PREFIXES).
        A body root missing from ROOTS is a PROBLEM (exit 2): the map is checked against the code on every call.
  game  - the shader-constant name table filled by sub_82BAAEE8 (its listing: copy name -> u16 register +16, count +18,
          lanes +20, bank +24), with its per-effect sub-tables (picked by the effect's lowercased name: a full 16-byte
          compare or a 3-character prefix, both read from the same listing); a row feeds only the stage its bank names
          (BANK_STAGES; bool banks and samplers are skipped) and only the lanes its +20 word names (LANES);
        - the model's descriptor list filled by sub_82BDB460 (47 names, array 0x8392E5A0): those names are set by the model
          draw code -> GAME-CODE;
        - the player's own material records (out/harness/index/asset.s*.db, `q asset`): a name the effect's records carry
          -> GAME-MATERIAL (with the record count); an effect with no record in the files takes the other effects' records;
        - `q const` literal writers of the register (qconst scan cache): listed (GAME-WRITER), never deciding the kind: a literal
          register write is not tied to an effect;
        - a table row that is neither -> GAME-NOREC (model create writes a row only for a parameter a material record names:
          s111 MAT1, 0x82BDC898; the other by-name lookups are not joined); GAME-TABLE only with no asset index; no row -> UNKNOWN;
        - GAME-CONST (S111 RIMGAME): a code-fed name whose source the game's code fills from IMAGE CONSTANTS (GAME_CONST: the
          declared chain, sink -> its one writer -> the writer's object constructor, each read in docs/research/s111/RIMGAME.md).
          The four values are read from the two listings and the image on every call (const_walk), never written here; ours'
          literal lanes are compared with them: equal = not a DIFF (CONST), unequal = a DIFF by lane. A chain that no longer
          resolves is a PROBLEM.
  passes - PASS-NEVER (S111 CONSTPASS, docs/research/s111/CONSTPASS.md): a row whose register is read only by fx.json variants that
          the model draw never picks for that effect is not a DIFF / PARTIAL / UNFED, whatever ours feeds. The pass the draw picks is
          base (0 / 1) plus the PASS_TERMS adds, each allowed only when model create's sub_82BDA038 stores 1 in that entry word for
          the effect's lowercased name; the name lists are read from its listing on every call (pass_flags: 32 copied names, no hand
          table). Which passes use a variant comes from the player's effect files (tools/shaders/effect_table.py: every technique's
          passes, all blobs of an effect merged); without them no row is marked (said in the sources line). A wrestler pass whose
          pixel shader reads the cube-specular slot 11 is drawn by OUR table pick with its non-cube twin (char_effects.cpp
          CubeSibling), so the twin counts as drawn in that pass (cube_twins); a pass with a shader the registry cannot name marks
          nothing of its effect. The map is cached beside the q index by its inputs' size and mtime (effect_passes; ~7 s cold).
Notation and limits: docs/research/s111/CONSTDIFF.md.
"""
import argparse
import fnmatch
import glob
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
REPO = os.path.dirname(os.path.dirname(HERE))
FX_DIR = os.path.join(REPO, 'src', 'renderer', 'shaders', 'fx')

NAME_TABLE_FN = 0x82BAAEE8      # fills the shader-constant name table (32-byte rows)
DESC_FN = 0x82BDB460            # model creation: copies the descriptor names into DESC_ARRAY
DESC_ARRAY, DESC_COUNT = 0x8392E5A0, 47
COPY_FN = 0x82C35118            # copy_string_max15_into_zeroed_16byte_buffer
STRNCMP_FN = 0x82C34BF8         # strncmp_null_safe_sign_result
# A row's word +24 is the BANK the draw records it into (sub_82C04DF0 0x82C050B0 / 0x82C052A0 / 0x82C0548C): 1 -> VS float (op 21,
# sub_82BB27D8), 3 -> PS float (tag 28, sub_82BB1CF8), 5 -> both banks (tag 35, sub_82C1A998). 2 / 4 = bool banks and 7 / 8 = samplers:
# not float registers, never matched to an fx.json constant.
BANK_STAGES = {1: ('vs',), 3: ('ps',), 5: ('vs', 'ps')}
# A row's word +20 is which LANES of the register it writes (the same switch, PS arm 0x82C052A8-0x82C05484): 0 = the whole vec4(s)
# (count of them); 1 / 2 = a float / an int into x (y..w padded); 3..6 = a float into x / y / z / w alone; 7..10 = an int into x..w alone;
# 11 = x..z (sub_82BB21F0; [L] from the g_f3 names beside a w row of the same register).
LANES = {0: (0, 1, 2, 3), 1: (0,), 2: (0,), 3: (0,), 4: (1,), 5: (2,), 6: (3,), 7: (0,), 8: (1,), 9: (2,), 10: (3,), 11: (0, 1, 2)}
LANE_RANK = {'GAME': 3, 'OURS': 2, '?': 1.5, 'LITERAL': 1}

# (resolver key, our file, the line that opens the function)
RESOLVERS = (
    ('char', 'src/renderer_scene/char_effects.cpp', r'^bool ResolveChar\(const std::string& name'),
    ('front', 'src/renderer_scene/front_end.cpp', r'^bool Resolve\(const std::string& name'),
)
FRONT_PREFIXES = ('yBG_', 'yProp_', 'yDefault')

# the roots a resolver body reads -> the source kind; None = not a value source (the output, a switch, a loop helper).
# A root not listed is a PROBLEM: add it here with its kind after reading what it is.
ROOTS = {
    'mt': 'GAME',              # const assets::Material& mt = *c.mat: the material record's own values (the model file)
    'c': 'OURS',               # the draw context (CharCtx / Ctx): our rule, or a record value our code forwarded
    'ls': 'OURS',              # the light set the context carries
    'key_world': 'OURS',       # char_effects.cpp: the key light rotated by our camera rule
    'sp': 'GAME',              # front_end.cpp UVScroll: sp = mt.scroll + ...
    'mm': 'OURS',              # front_end.cpp: mm = c.bones[...]
    'm': 'OURS',               # char_effects.cpp: m = c.bones[...]
    'out': None, 'g_wrk_live': None, 'g_table_pick_all': None, 'absent': None, 'std': None,
}
KIND_RANK = {'GAME': 3, 'OURS': 2, 'LITERAL': 1}
GAME_FED = ('GAME-MATERIAL', 'GAME-CODE')
# GAME-CONST: a descriptor value the game's code fills from image constants. The chain is declared (each link read from the
# listings: docs/research/s111/RIMGAME.md, seam shadows_rim); the VALUES are read by const_walk on every call.
#   glob / off: where the draw's setter reads the vec4 (G = [glob], G + off); writer: the one function that stores it there;
#   ctor: the constructor of the writer's object (the writer runs on a node of that object: its vtable is found in the ctor).
GAME_CONST = {
    'g_f4RimLightParam': {'glob': 0x8392DDA4, 'off': 320, 'writer': 0x827E9D90, 'ctor': 0x827EA7F0,
                          'why': 'the skin setter 0x82BDD108 uploads [0x8392DDA4]+320 or the model block copy of it (0x82BD7B50 +560..+58C);'
                                 ' q field 320 --writes: one G-rooted writer'},
}
# PASS-NEVER (S111 CONSTPASS): the pass index the model draw sub_82C04DF0 draws a part with = base (1 when model+540 == 1, else 0:
# 0x82C05878 / 0x82C058AC) + these adds, in order sub_82BD4F88 (0x82C05A7C) then sub_82BD5EE0 (0x82C05B08). Each add needs its word of
# the part's 24-byte entry at [model+480] to be set; model create sub_82BDB460 +B40..+B54 starts every entry at +0 = 1, +4..+20 = 0,
# and sub_82BDA038 stores 1 there after the effect's name matches one of the names it copies in (read by pass_flags).
#   (entry word, the values it can add, where)
PASS_TERMS = (
    (12, (4, 8), 'sub_82BD4F88 +24..+64: +4 when model+616 == 1, +8 when 616 == 2 or (616 == 0 and the +16 word null or its target < 0);'
                 ' 616 >= 3 adds nothing'),
    (4, (2,), 'sub_82BD5EE0 +44..+64: +2, the shadow passes (when sub_82BA7868 also answers non-zero)'),
    (8, (4,), 'sub_82BD5EE0 +68..+90: +4 (when sub_82BCC3B0 also answers non-zero)'),
    (20, (16,), 'sub_82BD5EE0 +94..+B8: +16 (when model+620 == 1)'),
)
PASS_FLAGS_FN = 0x82BDA038      # model create's flag setter: copies PASS_FLAGS_COUNT names, compares the part's effect name with them
PASS_FLAGS_COUNT = 32
LOOKUP_FN = 0x82756710          # fills [r3+4] with the record a handle names: the effect name the compares read (sp+344 -> sp+348)
UNNAMED = '?unnamed'            # effect_passes key: the passes holding a shader the fx.json registry cannot name (its constants unknown)


class DiffError(Exception):
    pass


def hx(a):
    return f'0x{a:08X}'


def name_key(n):
    """g_f4EyePos -> EyePos (the same stripping as qconst.name_key)."""
    n = re.sub(r'^g_', '', n)
    m = re.match(r'^(?:f[1-4]|i[1-4]?|b[1-4]?|f|i|b|mat[0-9]?|mtx|fv|iv|fm[0-9]+)(?=[A-Z0-9])', n)
    return n[m.end():] if m else n


# ------------------------------------------------------------------ ours: the fx registry
def load_fields(fx_dir=None):
    """-> {(effect, stage, register): {'field', 'count', 'variants'}} from the committed fx.json (variants merged; 'variants' = the
    '<effect>_<variant>_<stage>' names that read the register, the names effect_table gives a pass's shaders)."""
    fx_dir = fx_dir or FX_DIR
    out = {}
    for p in sorted(glob.glob(os.path.join(fx_dir, '*.fx.json'))):
        with open(p, encoding='utf-8') as fh:
            d = json.load(fh)
        eff = d.get('effect') or os.path.basename(p).split('.')[0]
        for stage, sn in (('vertex', 'vs'), ('fragment', 'ps')):
            for v in d.get(stage, []):
                for c in v.get('constants', []):
                    k = (eff, sn, int(c['register']))
                    e = out.setdefault(k, {'field': c['field'], 'count': int(c.get('count', 1)), 'variants': set()})
                    e['count'] = max(e['count'], int(c.get('count', 1)))
                    e['variants'].add(f"{eff}_{v.get('name')}_{stage}")
    return out


def resolver_of(effect):
    if effect.startswith('yCh_'):
        return 'char'
    if effect.startswith(FRONT_PREFIXES):
        return 'front'
    return None


# ------------------------------------------------------------------ ours: the resolvers' text
def _mask(text):
    """Comments -> spaces (same length, newlines kept); strings kept."""
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
            while j < n and text[j] != ch:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(ch)
            i += 1
    return ''.join(out)


def _match(s, i, op, cl):
    """index just past the bracket that closes s[i] == op."""
    d = 0
    j = i
    while j < len(s):
        ch = s[j]
        if ch in '"\'':
            k = j + 1
            while k < len(s) and s[k] != ch:
                k += 2 if s[k] == '\\' else 1
            j = k + 1
            continue
        if ch == op:
            d += 1
        elif ch == cl:
            d -= 1
            if d == 0:
                return j + 1
        j += 1
    raise DiffError(f'unbalanced {op}{cl} from offset {i}')


def _stmt_end(s, i):
    """end of the statement starting at s[i] (a block or up to ';' at depth 0), with a following `else` branch."""
    while s[i].isspace():
        i += 1
    if s[i] == '{':
        j = _match(s, i, '{', '}')
    else:
        d, j = 0, i
        while j < len(s):
            ch = s[j]
            if ch in '({[':
                d += 1
            elif ch in ')}]':
                d -= 1
            elif ch == ';' and d == 0:
                j += 1
                break
            j += 1
    m = re.match(r'\s*else\b', s[j:])
    if m:
        k = j + m.end()
        mi = re.match(r'\s*if\s*\(', s[k:])
        if mi:
            k = _match(s, k + mi.end() - 1, '(', ')')
        return _stmt_end(s, k)
    return j


NAME_RE = re.compile(r'name\s*==\s*"(\w+)"')
ROOT_RE = re.compile(r'(?<![\w.>])([A-Za-z_]\w*)\s*(?:\.|->|\[)')   # a root: not itself a member (after . or ->)
BARE_RE = re.compile(r'(?<![\w.>])([A-Za-z_]\w*)\s*(?=[,)])')       # an identifier passed as an argument


def body_kind(body, roots_seen):
    """-> (kind, roots): GAME / OURS / LITERAL / ZERO / FALSE from the roots the body reads."""
    b = re.sub(r'\b(?:\w+::)+', '', body)              # namespaces / scopes are not value roots
    b = re.sub(r'"(?:[^"\\]|\\.)*"', '""', b)
    roots = []
    for m in ROOT_RE.finditer(b):
        r = m.group(1)
        if r in ('if', 'for', 'while', 'return', 'sizeof', 'static_cast', 'reinterpret_cast', 'float', 'uint32_t', 'int', 'size_t'):
            continue
        if b[m.end() - 1] == '.' and re.match(r'\d', b[m.start():]):
            continue
        roots.append(r)
        roots_seen.add(r)
    roots += [m.group(1) for m in BARE_RE.finditer(b) if ROOTS.get(m.group(1))]   # a root passed whole to a helper: FillX(out, mt)
    kinds = [ROOTS.get(r) for r in roots if r in ROOTS]
    if 'GAME' in kinds:
        return 'GAME', roots
    if 'OURS' in kinds:
        return 'OURS', roots
    inner = re.sub(r'[{}\s;]', '', b)
    if inner in ('returntrue', ''):
        return 'ZERO', roots
    if inner == 'returnfalse':
        return 'FALSE', roots
    return 'LITERAL', roots


NUMLIT_RE = re.compile(r'(?<![\w.])(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?[fFuUlL]*')
LOCAL_RE = re.compile(r'\b(?:const\s+)?(?:float|auto|bool|int|uint32_t|double)\s+(\w+)\s*(?:\[[^\]]*\]\s*)?=\s*([^;]+);')


def _split_args(s):
    out, d, cur = [], 0, ''
    for ch in s:
        if ch in '([{':
            d += 1
        elif ch in ')]}':
            d -= 1
        if ch == ',' and d == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    return out + [cur]


def arg_kind(t, local=None):
    """the source of ONE written value: GAME / OURS from its roots (or a local built from them), LITERAL when it is only numbers, else '?'."""
    ks = [ROOTS.get(m.group(1)) for m in ROOT_RE.finditer(t)] + [ROOTS.get(m.group(1)) for m in BARE_RE.finditer(t + ')')]
    ks += [local[n] for n in re.findall(r'(?<![\w.>])([A-Za-z_]\w*)\b', t) if local and n in local]
    if 'GAME' in ks:
        return 'GAME'
    if 'OURS' in ks:
        return 'OURS'
    return 'LITERAL' if re.fullmatch(r'[\s+\-*/()]*', NUMLIT_RE.sub('', t)) else '?'


def branch_lanes(stmt, kind, writers=()):
    """-> {(vec4, lane): kind} of the values a branch writes, or None when it writes `out` some other way (a memcpy, a loop, a helper):
    then its lanes are not split. A lane a split branch does not write is the caller's zero (the block is zeroed before the resolver:
    front_end.cpp `block.assign(bytes, 0)`), so it is LITERAL."""
    if kind == 'ZERO':
        return {}
    if any(re.search(r'(?<![\w.])' + re.escape(w) + r'\s*\(', stmt) for w in writers):
        return None   # a lambda of the resolver that writes `out` itself (front_end.cpp `rows`): not split
    b = re.sub(r'\b(?:\w+::)+', '', stmt)
    b = re.sub(r'"(?:[^"\\]|\\.)*"', '""', b)
    local = {m.group(1): arg_kind(m.group(2)) for m in LOCAL_RE.finditer(b)}
    lanes, spans = {}, []

    def put(key, k):
        if LANE_RANK[k] > LANE_RANK.get(lanes.get(key), 0):
            lanes[key] = k
    for m in re.finditer(r'\bset4\s*\(', b):
        e = _match(b, m.end() - 1, '(', ')')
        args = _split_args(b[m.end():e - 1])
        if len(args) != 5 or not args[0].strip().isdigit():
            return None
        for i, a in enumerate(args[1:]):
            put((int(args[0]), i), arg_kind(a, local))
        spans.append((m.start(), e))
    for m in re.finditer(r'(?<![\w.])out\[(\d+)\]\s*=(?!=)\s*', b):
        d, j = 0, m.end()
        while j < len(b) and not (d == 0 and b[j] in ',;'):
            d += b[j] in '([{'
            d -= b[j] in ')]}'
            j += 1
        n = int(m.group(1))
        put((n // 4, n % 4), arg_kind(b[m.end():j], local))
        spans.append((m.start(), j))
    rest = ''.join(' ' if any(s <= i < e for s, e in spans) else ch for i, ch in enumerate(b))
    if re.search(r'(?<![\w.])out\b', rest):
        return None
    return lanes


def lit_value(t):
    """the value of a written argument made only of numeric literals (`0.83f`, `1.f / 3.f`), else None."""
    s = t.strip()
    if not re.search(r'\d', s) or not re.fullmatch(r'[\d.eE+\-*/() fFuUlL]*', s):
        return None
    s = re.sub(r'(?<=[\d.])[fFuUlL]+', '', s)
    try:
        return float(eval(s, {'__builtins__': {}}, {}))   # noqa: S307 - the text is digits and arithmetic only (checked above)
    except Exception:  # noqa: BLE001
        return None


def branch_values(stmt):
    """-> {(vec4, lane): value or None} of the set4 arguments / out[N] writes of a branch (lit_value of each)."""
    b = re.sub(r'\b(?:\w+::)+', '', stmt)
    vals = {}
    for m in re.finditer(r'\bset4\s*\(', b):
        args = _split_args(b[m.end():_match(b, m.end() - 1, '(', ')') - 1])
        if len(args) == 5 and args[0].strip().isdigit():
            for i, a in enumerate(args[1:]):
                vals[(int(args[0]), i)] = lit_value(a)
    for m in re.finditer(r'(?<![\w.])out\[(\d+)\]\s*=(?!=)\s*', b):
        d, j = 0, m.end()
        while j < len(b) and not (d == 0 and b[j] in ',;'):
            d += b[j] in '([{'
            d -= b[j] in ')]}'
            j += 1
        n = int(m.group(1))
        vals[(n // 4, n % 4)] = lit_value(b[m.end():j])
    return vals


def literal_arm(stmt):
    """True when the branch holds a set4 of four literals (an if / else arm: `if (live) set4(0, mt...); else set4(0, 0.f, 0.f, 1.f, 1.f)`)."""
    b = re.sub(r'\b(?:\w+::)+', '', stmt)
    for m in re.finditer(r'\bset4\s*\(', b):
        args = _split_args(b[m.end():_match(b, m.end() - 1, '(', ')') - 1])
        if len(args) == 5 and all(arg_kind(a) == 'LITERAL' for a in args[1:]):
            return True
    return False


def parse_resolver(text, func_re, path):
    """-> (branches {field: [{kind, line, plant, roots}]}, problems [str]) for the function opening at `func_re` in `text`."""
    m = re.search(func_re, text, re.M)
    if not m:
        raise DiffError(f'{path}: no line matches {func_re!r}')
    ms = _mask(text)
    ob = ms.index('{', m.end())
    end = _match(ms, ob, '{', '}')
    body = ms[ob:end]
    base_line = text.count('\n', 0, ob) + 1
    plants = set(re.findall(r'static\s+const\s+bool\s+(\w+)\s*=\s*std::getenv', body))
    writers = set()   # the resolver's own lambdas that write `out` (set4 is read lane by lane; any other one hides its lanes)
    for ml in re.finditer(r'\bconst\s+auto\s+(\w+)\s*=\s*\[[^\]]*\]\s*\(', body):
        lb = body.find('{', _match(body, ml.end() - 1, '(', ')'))
        if ml.group(1) != 'set4' and lb >= 0 and re.search(r'(?<![\w.])out\b', body[lb:_match(body, lb, '{', '}')]):
            writers.add(ml.group(1))
    out, problems, seen = {}, [], set()
    for mi in re.finditer(r'\bif\s*\(', body):
        cp = mi.end() - 1
        ce = _match(body, cp, '(', ')')
        cond = body[cp:ce]
        names = NAME_RE.findall(cond)
        if not names:
            continue
        se = _stmt_end(body, ce)
        stmt = body[ce:se]
        line = base_line + body.count('\n', 0, mi.start())
        plant = bool(re.search(r'\w*Plant\w*\s*\(\s*\)', cond)) or any(re.search(r'\b' + re.escape(p) + r'\b', cond) for p in plants)
        kind, roots = body_kind(stmt, seen)
        lanes = branch_lanes(stmt, kind, writers)
        for nm in names:
            out.setdefault(nm, []).append({'kind': kind, 'line': line, 'plant': plant, 'roots': sorted(set(roots)), 'path': path, 'lanes': lanes,
                                           'lit_arm': kind in ('GAME', 'OURS') and literal_arm(stmt), 'values': branch_values(stmt)})
    for r in sorted(seen):
        if r not in ROOTS:
            problems.append(f'{path}: the resolver body reads `{r}` (a root ROOTS does not map: read it, add it with its kind)')
    return out, problems


def ours_kind(brs):
    """-> (kind, [branches used]) for one field: the strongest non-plant branch; none = UNFED."""
    live = [b for b in (brs or []) if not b['plant'] and b['kind'] != 'FALSE']
    if not live:
        return 'UNFED', []
    best = max(live, key=lambda b: KIND_RANK.get('LITERAL' if b['kind'] == 'ZERO' else b['kind'], 0))
    k = 'LITERAL' if best['kind'] == 'ZERO' else best['kind']
    return k, live


def lane_kind(live, key):
    """the strongest source any live branch writes into lane `key` = (vec4, lane); a branch whose lanes are not split gives its whole kind."""
    best = None
    for b in live:
        whole = 'LITERAL' if b['kind'] == 'ZERO' else b['kind']
        k = whole if b.get('lanes') is None else b['lanes'].get(key, 'LITERAL')
        if best is None or LANE_RANK[k] > LANE_RANK[best]:
            best = k
    return best or 'LITERAL'


def lane_values(live, key):
    """the literal values the live branches write into lane `key` (None = a literal we cannot read: an unsplit branch); a split branch
    that leaves the lane unwritten gives the caller's zero (see branch_lanes)."""
    out = []
    for b in live:
        if b['kind'] == 'ZERO':
            out.append(0.0)
        elif b.get('lanes') is None:
            if b['kind'] == 'LITERAL':
                out.append(None)
        elif b['lanes'].get(key, 'LITERAL') == 'LITERAL':
            out.append(b.get('values', {}).get(key, 0.0) if key in b.get('values', {}) else 0.0)
    return out


def lane_name(key):
    k, i = key
    return ('' if k == 0 else f'[{k}]') + 'xyzw'[i]


def load_resolvers(read=None):
    read = read or (lambda p: open(os.path.join(REPO, p), encoding='utf-8').read())
    res, problems = {}, []
    for key, path, fre in RESOLVERS:
        b, p = parse_resolver(read(path), fre, path)
        res[key] = b
        problems += p
    return res, problems


# ------------------------------------------------------------------ game: the listing walk
def _walk(insns):
    """Constant-propagating walk over a function: copies {dst: (src, site)}, stores {addr: (value, site, base, mn)},
    compares [(site, mode, buf, n)], rebases [(site, base, last_compare)], r31_at {insn addr: base}, branches [(site, target)]."""
    regs, copies, stores, cmps, rebases, r31_at, brs = {}, {}, {}, [], [], {}, []
    last_cmp = None
    for x in insns:
        ops = [o.strip() for o in x.ops.split(',')] if x.ops else []
        r31_at[x.addr] = regs.get('r31')
        mn = x.mn
        if mn == 'bl':
            tgt = int(ops[0], 16) if ops else None
            if tgt == COPY_FN and isinstance(regs.get('r3'), int) and isinstance(regs.get('r4'), int):
                copies[regs['r3']] = (regs['r4'], x.addr)
            elif tgt == STRNCMP_FN:
                last_cmp = (x.addr, 'prefix', regs.get('r4'), regs.get('r5'))
                cmps.append(last_cmp)
            for r in ('r3', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12'):
                regs[r] = None
            continue
        if mn == 'b' and ops:
            try:
                brs.append((x.addr, int(ops[0], 16)))
            except ValueError:
                pass
            continue
        if mn == 'lis':
            regs[ops[0]] = (int(ops[1], 0) & 0xFFFF) << 16
        elif mn == 'li':
            regs[ops[0]] = int(ops[1], 0) & 0xFFFFFFFF
        elif mn == 'addi':
            b = regs.get(ops[1])
            regs[ops[0]] = ((b + int(ops[2], 0)) & 0xFFFFFFFF) if isinstance(b, int) else None
            if ops[0] == 'r31' and isinstance(regs['r31'], int):
                rebases.append((x.addr, regs['r31'], last_cmp))
        elif mn == 'mr':
            regs[ops[0]] = regs.get(ops[1])
        elif mn in ('sth', 'stw'):
            m = re.match(r'(-?\d+)\((r\d+)\)', ops[1])
            if m and isinstance(regs.get(m.group(2)), int):
                a = (regs[m.group(2)] + int(m.group(1))) & 0xFFFFFFFF
                stores[a] = (regs.get(ops[0]), x.addr, regs.get('r31'), mn)
        elif mn == 'ld':
            m = re.match(r'(-?\d+)\((r\d+)\)', ops[1]) if len(ops) > 1 else None
            if m and isinstance(regs.get(m.group(2)), int) and m.group(1) == '0' and m.group(2) != 'r31' and regs[m.group(2)] in copies:
                last_cmp = (x.addr, 'full', regs[m.group(2)], 16)
                cmps.append(last_cmp)
            regs[ops[0]] = None
        elif ops and re.match(r'^r\d+$', ops[0]) and not mn.startswith(('st', 'cmp', 'b')):
            regs[ops[0]] = None
    return {'copies': copies, 'stores': stores, 'cmps': cmps, 'rebases': rebases, 'r31_at': r31_at, 'branches': brs}


def game_table(insns, strings):
    """-> {'rows': [...], 'groups': [{base, mode, text, n, site}], 'alias': {base: base}} from sub_82BAAEE8's instructions."""
    w = _walk(insns)
    rows = []
    for dst, (src, site) in sorted(w['copies'].items()):
        st = w['stores'].get(dst + 16)
        if st is None or st[3] != 'sth' or not isinstance(st[0], int):
            continue
        get = lambda o: (w['stores'].get(dst + o) or (None,))[0]
        rows.append({'addr': dst, 'name': strings(src), 'name_at': src, 'site': site, 'reg': st[0], 'count': get(18),
                     'comp': get(20), 'type': get(24), 'base': st[2]})
    bufs = {dst: strings(src) for dst, (src, _s) in w['copies'].items()}
    groups, alias = [], {}
    for site, base, cmp in w['rebases']:
        g = {'base': base, 'site': site, 'mode': 'common', 'text': None, 'n': None}
        if cmp is not None:
            g['mode'], g['text'], g['n'] = cmp[1], bufs.get(cmp[2]), (cmp[3] if cmp[1] == 'prefix' else 16)
        groups.append(g)
        # a rebase that branches into another group's fill code before storing anything shares that group's rows
        for bsite, tgt in w['branches']:
            if site < bsite <= site + 48 and not any(site < s[1] <= bsite for s in w['stores'].values()):
                tb = w['r31_at'].get(tgt)
                if isinstance(tb, int) and tb != base:
                    alias[base] = tb
                break
    return {'rows': rows, 'groups': groups, 'alias': alias}


def group_rows(table, effect):
    """-> [rows] the game can bind for this effect: the common rows, then the effect's own sub-table (by its lowercased name)."""
    lc = effect.lower()
    bases = []
    common = [g for g in table['groups'] if g['mode'] == 'common']
    if common:
        bases.append(common[0]['base'])
    for g in table['groups']:
        if g['mode'] == 'full' and g['text'] and lc == g['text'].lower():
            bases.append(g['base'])
            break
        if g['mode'] == 'prefix' and g['text'] and isinstance(g['n'], int) and lc[:g['n']] == g['text'].lower()[:g['n']]:
            bases.append(g['base'])
            break
    bases = [table['alias'].get(b, b) for b in bases]
    return [r for b in bases for r in table['rows'] if r['base'] == b and r['type'] in BANK_STAGES]


def descriptor_names(insns, strings):
    w = _walk(insns)
    out = {}
    for dst, (src, site) in w['copies'].items():
        if DESC_ARRAY <= dst < DESC_ARRAY + 16 * DESC_COUNT:
            out[strings(src)] = (dst - DESC_ARRAY) // 16
    return out


def const_walk(insns):
    """-> {(base, off): value} of the stores of one function, walked in listing order (branches are not followed: a declared
    chain's stores sit on its fall-through path). Bases: ('arg',) = r3 at entry, ('stack',) = r1, ('abs',) = an absolute address,
    ('mem', A) = the pointer loaded from A. Values: an int (address / immediate), a base tuple + offset, ('img', A) = a float loaded
    from the image word A, ('fld', base, off) = a field loaded from a base, ('pair', a, b) = two words moved by ld / std.
    A bl / bctrl clobbers r0, r3..r12 and f0..f13 (the save helper called right after mflr does not)."""
    gpr = {'r3': ('arg', 0), 'r1': ('stack', 0)}
    fpr, stores = {}, {}
    prev = None

    def key(r, d):
        b = gpr.get(r)
        if isinstance(b, int):
            return (('abs',), (b + d) & 0xFFFFFFFF)
        if isinstance(b, tuple) and b[0] in ('arg', 'stack', 'mem'):
            return (b[:-1], b[-1] + d)
        return None

    def mem(op):
        m = re.match(r'(-?\d+)\((r\d+)\)', op)
        return key(m.group(2), int(m.group(1))) if m else None
    for x in insns:
        ops = [o.strip() for o in x.ops.split(',')] if x.ops else []
        mn = x.mn
        if mn in ('bl', 'bctrl', 'blrl'):
            if not (mn == 'bl' and prev is not None and prev.mn == 'mflr'):
                for r in ['r0'] + [f'r{i}' for i in range(3, 13)]:
                    gpr.pop(r, None)
                for i in range(14):
                    fpr.pop(f'f{i}', None)
        elif mn == 'lis':
            gpr[ops[0]] = (int(ops[1], 0) & 0xFFFF) << 16
        elif mn == 'li':
            gpr[ops[0]] = int(ops[1], 0) & 0xFFFFFFFF
        elif mn == 'addi':
            b = gpr.get(ops[1])
            gpr[ops[0]] = ((b + int(ops[2], 0)) & 0xFFFFFFFF) if isinstance(b, int) else (b[:-1] + (b[-1] + int(ops[2], 0),)) if isinstance(b, tuple) else None
        elif mn == 'mr':
            gpr[ops[0]] = gpr.get(ops[1])
        elif mn == 'fmr':
            fpr[ops[0]] = fpr.get(ops[1])
        elif mn == 'lwz':
            k = mem(ops[1])
            gpr[ops[0]] = ('mem', k[1], 0) if k and k[0] == ('abs',) else stores.get(k) if k else None
        elif mn == 'stw':
            k = mem(ops[1])
            if k:
                stores[k] = gpr.get(ops[0])
        elif mn == 'lfs':
            k = mem(ops[1])
            fpr[ops[0]] = None if k is None else ('img', k[1]) if k[0] == ('abs',) else stores[k] if k in stores else ('fld',) + k
        elif mn == 'stfs':
            k = mem(ops[1])
            if k:
                stores[k] = fpr.get(ops[0])
        elif mn == 'ld':
            k = mem(ops[1])
            gpr[ops[0]] = ('pair', stores.get(k), stores.get((k[0], k[1] + 4))) if k else None
        elif mn == 'std':
            k = mem(ops[1])
            v = gpr.get(ops[0])
            if k:
                stores[k], stores[(k[0], k[1] + 4)] = (v[1], v[2]) if isinstance(v, tuple) and v[0] == 'pair' else (None, None)
        elif ops and not mn.startswith(('st', 'cmp', 'fcmp', 'b', 'mt')):
            if re.match(r'^r\d+$', ops[0]):
                gpr[ops[0]] = None
            elif re.match(r'^f\d+$', ops[0]):
                fpr[ops[0]] = None
        prev = x
    return stores


def game_const(writer_insns, ctor_insns, writer, glob, off, image):
    """-> ([x, y, z, w] floats, why) for the vec4 `writer` stores at [glob] + off, each lane traced to an image constant (directly,
    or through a field of the writer's object that the constructor stores from one); (None, the reason) when a lane does not resolve."""
    ws = const_walk(writer_insns)
    lanes = [ws.get((('mem', glob), off + 4 * i)) for i in range(4)]
    if any(ln is None for ln in lanes):
        return None, f'sub_{writer:08X} stores no traced vec4 at [{hx(glob)}]+{off}: {lanes}'
    cs = const_walk(ctor_insns)
    nodes = sorted({(o, v, k) for (base, o), v in cs.items() if base == ('arg',) and isinstance(v, int) for k in range(8)
                    if image.word(v + 4 * k) == writer})
    if len(nodes) != 1:
        return None, f'the constructor stores {len(nodes)} vtables holding sub_{writer:08X} (want 1): {nodes}'
    node, vt, slot = nodes[0]
    vals, src = [], []
    for i, ln in enumerate(lanes):
        at = None
        if ln[0] == 'fld' and ln[1] == ('arg',):
            at = f'object +{node + ln[2]}'
            ln = cs.get((('arg',), node + ln[2]))
        if not (isinstance(ln, tuple) and ln[0] == 'img'):
            return None, f'lane {"xyzw"[i]} ({at or "a writer value"}) is not an image constant in the constructor: {ln}'
        w = image.word(ln[1])
        if w is None:
            return None, f'lane {"xyzw"[i]}: image word {hx(ln[1])} not readable'
        vals.append(struct.unpack('>f', struct.pack('>I', w))[0])
        src.append(f'{at} = {hx(ln[1])}' if at else hx(ln[1]))
    return vals, (f"= ({', '.join(f'{v:g}' for v in vals)}): " + ', '.join(src) + f' (constructor stores), copied by sub_{writer:08X}'
                  f' (vtable {hx(vt)} slot {slot} at object +{node}) to [{hx(glob)}]+{off}')


def material_names(path=None, source='game'):
    """-> ({effect: {name: records}}, {effect: records}) from the asset index (the player's own files); (None, None) when absent.
    The index lives beside the q index of the main checkout (qdecomp.IMAGE's folder), as every q game-side tool reads it.
    source (S111 MODINDEX, tools/q/qasset_mods.py where): 'game' = the game's own files (the default: the mods folder's rows left out), 'mods', 'mod:NAME' or 'all'."""
    if path is None:
        import qdecomp
        c = sorted(glob.glob(os.path.join(os.path.dirname(qdecomp.IMAGE), 'asset.s*.db')))
        path = c[-1] if c else None
    if not path or not os.path.exists(path):
        return None, None
    import qasset_mods
    sw, sa = qasset_mods.where(source)
    con = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    names, totals = {}, {}
    try:
        for eff, params, n in con.execute('SELECT material.effect, material.params, COUNT(*) FROM material JOIN entry ON material.entry_id = entry.id '
                                          'JOIN pach ON entry.pach_id = pach.id JOIN pacs ON pach.pac_id = pacs.id WHERE ' + sw +
                                          ' GROUP BY material.effect, material.params', sa):
            totals[eff] = totals.get(eff, 0) + n
            try:
                ps = json.loads(params or '{}')
            except ValueError:
                continue
            d = names.setdefault(eff, {})
            for k in ps:
                d[k] = d.get(k, 0) + n
    finally:
        con.close()
    return names, totals


def const_writers(con):
    """-> ([(stage, reg, count, func)], None) literal-register writers from the q const scan (cached); (None, why) when it cannot run."""
    try:
        import qconst
        d = qconst.get_scan(con, types.SimpleNamespace(no_cache=False, window=qconst.WINDOW, raw=False, root=None))
    except Exception as e:  # noqa: BLE001 - the scan is an extra source; its absence is said in the totals
        return None, str(e)
    out = []
    for h in d['hits']:
        if h['reg']['kind'] == 'literal':
            c = h['count']['value'] if h['count']['kind'] == 'literal' else 1
            out.append((h['stage'], h['reg']['value'], max(c, 1), h['func']))
    return out, None


def _insns(con, a):
    import qdecomp
    f = con.execute('SELECT * FROM funcs WHERE addr = ?', (a,)).fetchone()
    if f is None:
        raise DiffError(f'{hx(a)} is not in the q index')
    return qdecomp.read_listing(os.path.join(qdecomp.GEN, f['file']), f['line'], f['name'], f['addr'])[0]


def load_consts(con, image=None):
    """-> ({name: {'values', 'why'}}, problems) for every GAME_CONST chain."""
    import qdecomp
    image = image or qdecomp.Image()
    out, problems = {}, []
    for nm, d in GAME_CONST.items():
        try:
            v, why = game_const(_insns(con, d['writer']), _insns(con, d['ctor']), d['writer'], d['glob'], d['off'], image)
        except (DiffError, qdecomp.DecompError) as e:
            v, why = None, str(e)
        if v is None:
            problems.append(f'GAME-CONST {nm}: {why}')
        else:
            out[nm] = {'values': v, 'why': f"{why}; {d['why']}"}
    return out, problems


# ------------------------------------------------------------------ PASS-NEVER (S111 CONSTPASS)
def pass_flags(insns, strings):
    """-> ({entry word: {names whose match stores 1 there}}, [the copied names]) from sub_82BDA038's instructions, one linear walk.
    The names are copied into 16-byte buffers (COPY_FN: r3 the buffer, r4 the string); a buffer's address is kept in a register or a
    stack slot; LOOKUP_FN called with r3 = sp+N leaves the effect name's pointer in the slot sp+N+4. A test is two 8-byte compares
    (ld / cmpld) of the name with a buffer: the one on word +8 is followed by `beq T` (the match path starts at T) or by `bne` (the
    match path is the next instruction). A match path runs straight to its first branch; a `stw` in it of a register holding 1, at
    offset W, sets entry word W for that name."""
    regs, slots, copies, tests, stores = {}, {}, {}, [], {}
    pending = None
    addrs = [x.addr for x in insns]
    for x in insns:
        ops = [o.strip() for o in x.ops.split(',')] if x.ops else []
        mn = x.mn
        m = re.match(r'(-?\d+)\((r\d+)\)', ops[1]) if len(ops) > 1 else None
        if mn.startswith('b'):
            if pending is not None and mn in ('beq', 'bne') and ops:
                tests.append((pending, int(ops[-1], 16) if mn == 'beq' else None, x.addr))
            pending = None
        if mn == 'bl':
            tgt = int(ops[0], 16) if ops else None
            r3, r4 = regs.get('r3'), regs.get('r4')
            if tgt == COPY_FN and isinstance(r3, int) and isinstance(r4, int):
                copies[r3] = r4
            elif tgt == LOOKUP_FN and isinstance(r3, tuple) and r3[0] == 'sp':
                slots[r3[1] + 4] = ('name',)
            for r in ['r0'] + [f'r{i}' for i in range(3, 13)]:
                regs.pop(r, None)
            continue
        if mn.startswith('b'):
            continue
        if mn == 'lis':
            regs[ops[0]] = (int(ops[1], 0) & 0xFFFF) << 16
        elif mn == 'li':
            regs[ops[0]] = ('imm', int(ops[1], 0))
        elif mn == 'addi':
            b, k = regs.get(ops[1]) if ops[1] != 'r1' else ('sp', 0), int(ops[2], 0)
            regs[ops[0]] = ((b + k) & 0xFFFFFFFF) if isinstance(b, int) else ('sp', b[1] + k) if isinstance(b, tuple) and b[0] == 'sp' else None
        elif mn == 'mr':
            regs[ops[0]] = regs.get(ops[1])
        elif mn == 'stw' and m:
            if m.group(2) == 'r1':
                slots[int(m.group(1))] = regs.get(ops[0])
            else:
                stores[x.addr] = (int(m.group(1)), regs.get(ops[0]))
        elif mn == 'lwz' and m:
            regs[ops[0]] = slots.get(int(m.group(1))) if m.group(2) == 'r1' else None
        elif mn == 'ld' and m:
            b = regs.get(m.group(2))
            regs[ops[0]] = ('buf', b, int(m.group(1))) if isinstance(b, int) else ('nm', int(m.group(1))) if b == ('name',) else None
        elif mn == 'cmpld' and len(ops) == 3:
            a, b = regs.get(ops[1]), regs.get(ops[2])
            buf = [v for v in (a, b) if isinstance(v, tuple) and v[0] == 'buf' and v[2] == 8]
            pending = buf[0][1] if buf and ('nm', 8) in (a, b) else None
        elif ops and re.match(r'^r\d+$', ops[0]) and not mn.startswith(('st', 'cmp')):
            regs[ops[0]] = None
    flags = {}
    for buf, tgt, at in tests:
        name = strings(copies.get(buf))
        i = addrs.index(tgt) if tgt in addrs else addrs.index(at) + 1 if tgt is None else None
        while name and i is not None and i < len(insns) and not insns[i].mn.startswith('b'):
            w, v = stores.get(insns[i].addr, (None, None))
            if v == ('imm', 1):
                flags.setdefault(w, set()).add(name)
            i += 1
    return flags, [strings(copies[a]) for a in sorted(copies)]


def load_pass_flags(con, plant=None):
    """-> ({entry word: {names}} or None, problems) from sub_82BDA038. plant = {string text: new text} (the selftest's renamed strings)."""
    import qdecomp

    def strings(a):
        r = con.execute('SELECT text FROM strings WHERE addr = ?', (a,)).fetchone() if isinstance(a, int) else None
        t = r['text'] if r else None
        return (plant or {}).get(t, t)
    try:
        flags, names = pass_flags(_insns(con, PASS_FLAGS_FN), strings)
    except (DiffError, qdecomp.DecompError) as e:
        return None, [f'pass flags: {e}']
    problems = []
    if len(names) != PASS_FLAGS_COUNT or None in names:
        problems.append(f'pass flags: {len(names)} names copied by sub_{PASS_FLAGS_FN:08X} ({names.count(None)} unreadable), expected {PASS_FLAGS_COUNT}')
    gone = [w for w, _adds, _why in PASS_TERMS if not flags.get(w)]
    if gone:
        problems.append(f'pass flags: no name of sub_{PASS_FLAGS_FN:08X} sets entry word(s) {gone} (read: {sorted(flags)})')
    return (None if problems else flags), problems


def cube_twins(fx_dir=None):
    """-> {'<effect>_<cube variant>_fragment': '<effect>_<twin>_fragment'} for the wrestler (ResolveChar) effects. OUR table pick binds a
    row whose pixel shader reads the cube-specular slot 11 with the same effect's fragment whose defines are the row's minus
    FX_CUBE_SPECULAR (src/renderer_scene/char_effects.cpp ChooseCharEffectTable -> CheckPs / CubeSibling: the 360 bake dropped), so
    the twin is drawn in every pass the cube variant is; the same rule, read from the same fx.json."""
    fx_dir = fx_dir or FX_DIR
    out = {}
    for p in sorted(glob.glob(os.path.join(fx_dir, '*.fx.json'))):
        with open(p, encoding='utf-8') as fh:
            d = json.load(fh)
        eff = d.get('effect') or os.path.basename(p).split('.')[0]
        if resolver_of(eff) != 'char':
            continue
        frs = d.get('fragment', [])
        for v in frs:
            if not any(s.get('slot') == 11 for s in v.get('samplers') or []):
                continue
            want = sorted(x for x in v.get('defines') or [] if x != 'FX_CUBE_SPECULAR')
            tw = next((w for w in frs if w is not v and sorted(w.get('defines') or []) == want), None)
            if tw:
                out[f"{eff}_{v.get('name')}_fragment"] = f"{eff}_{tw.get('name')}_fragment"
    return out


def _passes_key(effect_table, shaderlib):
    """The inputs effect_passes reads, as (path, size, mtime_ns): the player's three effect files, our fx.json registry, the code."""
    fs = [os.path.join(shaderlib.GAME, rel.replace('/', os.sep)) for rel in effect_table.SHADER_FILES]
    fs += sorted(glob.glob(os.path.join(effect_table.FX_DIR, '*.fx.json'))) + sorted(glob.glob(os.path.join(FX_DIR, '*.fx.json')))
    fs += [effect_table.__file__, shaderlib.__file__, os.path.join(os.path.dirname(os.path.dirname(effect_table.__file__)), 'wm30', 'pac.py'),
           os.path.abspath(__file__)]
    key = []
    for f in fs:
        try:
            st = os.stat(f)
            key.append([os.path.abspath(f), st.st_size, st.st_mtime_ns])
        except OSError:
            key.append([os.path.abspath(f), None, None])
    return json.dumps(key)


def effect_passes(cache_path=None):
    """-> ({effect: {'<effect>_<variant>_<stage>': {pass indices}}}, None) from the player's effect files (tools/shaders/effect_table.py,
    every file's blobs of an effect merged), plus OUR cube twins (cube_twins: drawn in the cube variant's passes) and, under UNNAMED,
    the passes holding a shader the registry cannot name; (None, why) when the files cannot be read. Cached by its inputs' size and
    mtime (_passes_key) in cache_path (default: beside the q index, one file per checkout): ~7 s cold, a JSON read warm."""
    try:
        sys.path.insert(0, os.path.join(REPO, 'tools', 'shaders'))
        import effect_table
        import shaderlib
        key = _passes_key(effect_table, shaderlib)
        if cache_path is None:
            import hashlib
            import qdecomp
            cache_path = os.path.join(os.path.dirname(qdecomp.IMAGE),
                                      f"effect_passes.{hashlib.sha1(os.path.abspath(REPO).lower().encode()).hexdigest()[:10]}.json")
        try:
            with open(cache_path, encoding='utf-8') as fh:
                c = json.load(fh)
            if c.get('key') == key:
                return {e: {n: set(ps) for n, ps in d.items()} for e, d in c['passes'].items()}, None
        except (OSError, ValueError, KeyError, AttributeError, TypeError):
            pass
        res = effect_table.load_all()
    except Exception as e:  # noqa: BLE001 - the player's files are an extra source; their absence is said in the sources line
        return None, f'{type(e).__name__}: {e}'
    twins = cube_twins()
    out = {}
    for e in res.values():
        d = out.setdefault(e['effect'], {})
        for t in e['techniques']:
            for p in t['passes']:
                for side in ('vs', 'ps'):
                    nm = (p[side] or {}).get('name')
                    for n in (nm, twins.get(nm)) if nm else (UNNAMED,):
                        if n:
                            d.setdefault(n, set()).add(p['pass'])
    if os.path.isdir(os.path.dirname(cache_path) or '.'):
        tmp = f'{cache_path}.{os.getpid()}.tmp'
        try:
            with open(tmp, 'w', encoding='utf-8') as fh:
                json.dump({'key': key, 'passes': {e: {n: sorted(ps) for n, ps in d.items()} for e, d in out.items()}}, fh)
            os.replace(tmp, cache_path)
        except OSError:
            pass
    return out, None


def reachable_passes(effect, flags):
    """-> the pass indices the model draw can pick for this effect: base 0 / 1 plus every PASS_TERMS add whose word its name sets."""
    n, sums = effect.lower(), {0, 1}
    for word, adds, _why in PASS_TERMS:
        if n in flags.get(word, ()):
            sums = {s + a for s in sums for a in (0,) + adds}
    return sums


def pass_never(effect, variants, passes, flags):
    """-> the reason (text) when every variant that reads the register is used only by passes the model draw never picks for this
    effect; None when one of them is drawn, or cannot be told (no flags / no effect files / a variant no pass of the files names / a
    drawn pass holding a shader the registry cannot name, whose constants are unknown)."""
    if not passes or not flags or not variants or effect not in passes:
        return None
    reach, used = reachable_passes(effect, flags), []
    if passes[effect].get(UNNAMED, set()) & reach:
        return None
    for v in sorted(variants):
        ps = passes[effect].get(v)
        if not ps or ps & reach:
            return None
        used.append(f"{v} (p{'/'.join(str(p) for p in sorted(ps))})")
    words = [w for w, _adds, _why in PASS_TERMS if effect.lower() in flags.get(w, ())]
    return (f"'{effect.lower()}' sets entry word(s) {words or 'none'} in sub_{PASS_FLAGS_FN:08X}, so the model draw picks passes "
            f"{sorted(reach)} only; the register is read by " + ', '.join(used))


def load_game(con):
    insns = lambda a: _insns(con, a)

    def strings(a):
        if not isinstance(a, int):
            return None
        r = con.execute('SELECT text FROM strings WHERE addr = ?', (a,)).fetchone()
        return r['text'] if r else None

    return game_table(insns(NAME_TABLE_FN), strings), descriptor_names(insns(DESC_FN), strings)


# ------------------------------------------------------------------ the join
def game_source(effect, stage, reg, field, count, table, desc, mats, totals, writers, consts=None):
    """-> {'kind', 'rows': [...]} for one register of one effect's stage."""
    cand = [r for r in group_rows(table, effect) if isinstance(r['reg'], int) and reg <= r['reg'] < reg + max(count, 1)
            and stage in BANK_STAGES[r['type']]]
    by_name = [r for r in cand if name_key(r['name'] or '') == field]
    rows = []
    best = None
    rank = {'GAME-MATERIAL': 3, 'GAME-CODE': 2, 'GAME-CONST': 2, 'GAME-NOREC': 1, 'GAME-TABLE': 1}
    for r in cand:
        nm = r['name'] or '?'
        if consts and nm in consts:
            k, why = 'GAME-CONST', consts[nm]['why'] + (f'; model descriptor #{desc[nm]} ({hx(DESC_FN)})' if nm in desc else '')
        elif nm in desc:
            k, why = 'GAME-CODE', f'model descriptor #{desc[nm]} ({hx(DESC_FN)})'
        elif mats is not None and mats.get(effect, {}).get(nm):
            k, why = 'GAME-MATERIAL', f'{mats[effect][nm]} of {totals.get(effect, 0)} records'
        elif mats is not None and not totals.get(effect) and any(d.get(nm) for d in mats.values()):
            # the player's files hold no record of this effect: a name other effects' records carry is still a material parameter
            k, why = 'GAME-MATERIAL', f'no {effect} records; {sum(d.get(nm, 0) for d in mats.values())} records of other effects carry it'
        elif mats is not None:
            # model create writes a table row only for a parameter a material record names (s111 MAT1: 0x82BDC898 [C]); the
            # descriptor list is the other writer. Not joined: the other by-name lookups (sub_8250C1D0, sub_82513D78, sub_82C21660, sub_834179A8).
            k, why = 'GAME-NOREC', ('named, not a descriptor, and ' + (f'no {effect} record names it' if totals.get(effect) else 'no record of any effect names it')
                                    + ': the material path never writes it')
        else:
            k, why = 'GAME-TABLE', 'named, writer not decided (no asset index)'
        k0 = r['reg'] - reg
        lanes = [(k0 + j, i) for j in range(max(r['count'] or 1, 1)) for i in LANES.get(r['comp'], (0, 1, 2, 3)) if k0 + j < max(count, 1)]
        rows.append({'name': nm, 'row': hx(r['addr']), 'site': hx(r['site']), 'reg': r['reg'], 'comp': r['comp'], 'lanes': lanes,
                     'kind': k, 'why': why, 'match': 'name' if r in by_name else 'register',
                     'values': consts[nm]['values'] if k == 'GAME-CONST' else None})
        if best is None or rank[k] > rank[best]:
            best = k
    # `q const` literal writers: a literal register in SOME function of the game, not tied to an effect, so they never decide the kind
    fns = sorted({fn for st, wr, wc, fn in (writers or []) if (st == stage or st == 'vsps') and wr <= reg < wr + wc})
    if fns:
        rows.append({'name': None, 'row': None, 'site': hx(fns[0]), 'reg': reg, 'comp': None, 'kind': 'GAME-WRITER',
                     'why': f'q const: {len(fns)} literal writer(s) of {stage} c{reg} in any effect: ' + ', '.join(f'sub_{f:08X}' for f in fns[:3])
                     + (' ...' if len(fns) > 3 else ''), 'match': 'register'})
    return {'kind': best or 'UNKNOWN', 'rows': rows}


def build_rows(fields, resolvers, game, mats, totals, writers, consts=None, passes=None, flags=None):
    table, desc = game
    out, skipped = [], {}
    for (eff, st, reg), f in sorted(fields.items()):
        rk = resolver_of(eff)
        if rk is None:
            skipped[eff] = skipped.get(eff, 0) + 1
            continue
        brs = resolvers[rk].get(f['field'])
        ok, live = ours_kind(brs)
        g = game_source(eff, st, reg, f['field'], f['count'], table, desc, mats, totals, writers, consts)
        never = pass_never(eff, f.get('variants'), passes, flags)   # S111 CONSTPASS: the game never draws a pass that reads it
        # per lane: a game-fed row whose every lane ours writes as a literal is a DIFF even when the branch reads a root elsewhere
        # (set4(0, mt.x, 1.f, ...)); some of its lanes literal = PARTIAL; a fed branch beside a live literal arm = PARTIAL too.
        dl, part = [], []
        for gr in g['rows']:
            if gr['kind'] not in GAME_FED or ok == 'UNFED':
                continue
            lit = [ln for ln in gr['lanes'] if lane_kind(live, ln) == 'LITERAL']
            if lit and len(lit) == len(gr['lanes']):
                dl.append(f"{''.join(lane_name(x) for x in lit)} ({gr['name']}, {gr['kind']})")
            elif lit:
                part.append(f"{''.join(lane_name(x) for x in lit)} literal in {gr['name']} ({gr['kind']}, the game writes {''.join(lane_name(x) for x in gr['lanes'])})")
        # GAME-CONST: each literal lane of ours against the game's constant (a lane ours computes is not compared, as for a fed row)
        ceq = []
        for gr in g['rows']:
            if gr['kind'] != 'GAME-CONST' or ok == 'UNFED':
                continue
            bad, same = [], []
            for ln, gv in zip(gr['lanes'], gr['values'] if len(gr['values']) == len(gr['lanes']) else [None] * len(gr['lanes'])):
                if lane_kind(live, ln) != 'LITERAL':
                    continue
                ovs = lane_values(live, ln)
                if gv is not None and ovs and all(v is not None and math.isclose(v, gv, rel_tol=1e-6, abs_tol=1e-12) for v in ovs):
                    same.append(f'{lane_name(ln)} {gv:g}')
                else:
                    bad.append(f"{lane_name(ln)}: ours {'/'.join('?' if v is None else f'{v:g}' for v in ovs)}, the game's {gv:g}")
            if bad:
                dl.append(f"{gr['name']} (GAME-CONST): " + '; '.join(bad))
            elif same:
                ceq.append(f"{', '.join(same)} ({gr['name']})")
        arms = [b for b in live if b['kind'] in ('LITERAL', 'ZERO') or b.get('lit_arm')]
        if g['kind'] in GAME_FED and ok in ('GAME', 'OURS') and arms:
            part.append('a literal arm beside the fed one: ' + ', '.join(f"{b['path']}:{b['line']}" for b in arms))
        whole = (g['kind'] in GAME_FED and ok in ('LITERAL', 'UNFED')) or (g['kind'] == 'GAME-CONST' and ok == 'UNFED')
        out.append({'effect': eff, 'stage': st, 'reg': reg, 'count': f['count'], 'field': f['field'], 'resolver': rk,
                    'ours': ok, 'ours_at': [f"{b['kind']} {b['path']}:{b['line']}" for b in live],
                    'plants': [f"{b['path']}:{b['line']}" for b in (brs or []) if b['plant']],
                    'game': g['kind'], 'game_rows': g['rows'],
                    'diff': (whole or bool(dl)) and not never, 'diff_lanes': [] if (whole or never) else dl,
                    'partial': [] if (whole or dl or never) else part, 'const_eq': [] if (whole or dl or never) else ceq,
                    'pass_never': never})
    return out, skipped


def fmt_game(r):
    if not r['game_rows']:
        return 'UNKNOWN'
    parts = []
    for g in r['game_rows']:
        if g['name']:
            parts.append(f"{g['kind']} {g['name']} (row {g['row']} @{g['site']}, by {g['match']}; {g['why']})")
        else:
            parts.append(f"{g['kind']} ({g['why']})")
    return '; '.join(parts)


def run(con, a, read=None, fx_dir=None):
    if getattr(a, 'byreg', False):   # S111 BYREG: the effects ours feeds BY REGISTER (tools/q/qconstdiff_reg.py)
        import qconstdiff_reg
        return qconstdiff_reg.run(con, a, read, fx_dir)
    fields = load_fields(fx_dir)
    resolvers, problems = load_resolvers(read)
    game = load_game(con)
    table, desc = game
    if len(desc) != DESC_COUNT:
        problems.append(f'descriptor list: {len(desc)} names copied into {hx(DESC_ARRAY)} by sub_{DESC_FN:08X}, expected {DESC_COUNT}')
    if not table['rows']:
        problems.append(f'name table: no rows read from sub_{NAME_TABLE_FN:08X}')
    mats, totals = material_names(source=getattr(a, 'source', 'game'))
    writers, werr = const_writers(con)
    consts, cprobs = load_consts(con)
    problems += cprobs
    flags, fprobs = load_pass_flags(con)
    problems += fprobs
    passes, perr = effect_passes()
    rows, skipped = build_rows(fields, resolvers, game, mats, totals, writers, consts, passes=passes, flags=flags)
    sel = rows
    if a.effect:
        sel = [r for r in sel if fnmatch.fnmatchcase(r['effect'], a.effect)]
    if a.kind == 'PASS-NEVER':
        sel = [r for r in sel if r['pass_never']]
    elif a.kind:
        sel = [r for r in sel if r['ours'] == a.kind and not r['pass_never']]
    if a.diff:
        sel = [r for r in sel if r['diff']]
    if a.partial:
        sel = [r for r in sel if r['partial']]
    if a.json:
        print(json.dumps({'rows': sel, 'problems': problems}, indent=1))
    else:
        cur = None
        for r in sel:
            if r['effect'] != cur:
                cur = r['effect']
                print(f'== {cur}  (resolver {"ResolveChar" if r["resolver"] == "char" else "Resolve (front end)"})')
            flag = 'DIFF ' if r['diff'] else 'PART ' if r['partial'] else 'NEVER' if r['pass_never'] else '     '
            regs = f"c{r['reg']}" + (f"+{r['count']}" if r['count'] > 1 else '')
            ours = ', '.join(r['ours_at']) if r['ours_at'] else 'no branch (the resolver returns false)'
            print(f"{flag}{r['stage']} {regs:8s} {r['field']:22s} ours {r['ours']:7s} {ours}")
            print(f"           game {fmt_game(r)}" + (f"   [dev plants skipped: {', '.join(r['plants'])}]" if r['plants'] else ''))
            if r['diff_lanes']:
                print(f"           DIFF by lane: ours writes a literal where the game writes " + '; '.join(r['diff_lanes']))
            for p in r['partial']:
                print(f"           PARTIAL: {p}")
            for p in r['const_eq']:
                print(f"           CONST: ours' literal equals the game's image constant: {p}")
            if r['pass_never']:
                print(f"           PASS-NEVER: {r['pass_never']}")
        tot = {}
        for r in sel:
            if not r['pass_never']:
                tot[r['ours']] = tot.get(r['ours'], 0) + 1
        nnever = sum(1 for r in sel if r['pass_never'])
        gt = {}
        for r in sel:
            gt[r['game']] = gt.get(r['game'], 0) + 1
        nd = sum(1 for r in sel if r['diff'])
        nl = sum(1 for r in sel if r['diff_lanes'])
        npart = sum(1 for r in sel if r['partial'])
        dl = {a for r in sel if r['diff'] for a in (r['ours_at'] or [f"UNFED {r['resolver']}:{r['field']}"])}
        print(f"totals: {len(sel)} rows (of {len(rows)}); DIFF {nd} ({nl} of them by lane; at {len(dl)} distinct ours lines / unfed fields); PARTIAL {npart}; "
              f"PASS-NEVER {nnever}; ours (PASS-NEVER rows not counted) "
              + ', '.join(f'{k} {tot.get(k, 0)}' for k in ('LITERAL', 'OURS', 'GAME', 'UNFED'))
              + '; game ' + ', '.join(f'{k} {v}' for k, v in sorted(gt.items())))
        print(f"sources: name table {len(table['rows'])} rows in {len(table['groups'])} groups (sub_{NAME_TABLE_FN:08X}); descriptor list {len(desc)} names; "
              + (f"material records {sum(totals.values())} (asset index)" if totals else 'material records: NO asset index (q asset builds it): GAME-MATERIAL cannot be told from GAME-TABLE')
              + ('; q const writers ' + (str(len(writers)) if writers is not None else f'not available ({werr})'))
              + ('; pass gates: ' + (f"{sum(len(v) for v in flags.values())} name matches in sub_{PASS_FLAGS_FN:08X}, the passes of {len(passes)} effects"
                                     " (the player's effect files)" if flags and passes else f"not available ({perr or 'see the PROBLEM line'}): no row is PASS-NEVER")))
        print(f"not fed by a by-name resolver (not compared): {len(skipped)} effects, {sum(skipped.values())} registers")
    for p in problems:
        print('PROBLEM: ' + p)
    return 2 if problems else 0


def main(argv, con, resolve=None):
    ap = argparse.ArgumentParser(prog='q constdiff', description=__doc__.split('\n\n')[0])
    ap.add_argument('--effect', help='an effect name or a glob (yCh_*)')
    ap.add_argument('--kind', choices=('LITERAL', 'OURS', 'GAME', 'UNFED', 'PASS-NEVER'),
                    help='only rows whose OURS source is this kind (a PASS-NEVER row is in none of them); PASS-NEVER = the rows whose pass the game never draws')
    ap.add_argument('--diff', action='store_true', help='only DIFF rows')
    ap.add_argument('--partial', action='store_true', help='only PARTIAL rows (some lanes literal in a game-fed register, or a literal arm)')
    import qasset_mods
    ap.add_argument('--source', type=qasset_mods.arg, default='game',
                    help='the asset index rows the material records come from: ' + qasset_mods.SOURCES + ' (S111 MODINDEX; default game: the mods folder left out)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--byreg', action='store_true', help='the effects OUR code feeds by register (live maps, post, particles) against the '
                    "game's writer on each effect's code path (S111 BYREG, tools/q/qconstdiff_reg.py)")
    a = ap.parse_args(argv)
    try:
        return run(con, a)
    except DiffError as e:
        print(f'q constdiff: {e}', file=sys.stderr)
        return 2
