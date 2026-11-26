r"""q variantdiff: per wrestler material, the shader the GAME draws it with in each pass beside the one OURS binds (S111 VARIANTDIFF).

    py tools/q/q.py variantdiff [--effect E] [--pac P] [--flag DIFF|HOLE|UNSETTLED|HABIT|OK ...] [--contexts] [--json] [--ref REF]

`q constdiff` / `q texdiff` diff what goes INTO a shader; this diffs WHICH shader (fx registry variant) each yCh_ material ends up
drawing with. One group per (effect, technique word, spec-power record) of the player's material records (the asset index, table
`material`), evaluated in every scene context the pass rule reads: the model mode word (0 / 2: the only two the game makes, seam
material_switches 0x825A20A0 / 0x825A1D48) and whether the frame has a shadow map; plus, for an effect whose part entry word +20 is set,
the +16 term (model +620 == 1, open).
  GAME  - the pass = base 1 (a wrestler model is skinned) + step A (sub_82BD4F88: entry +12 set -> mode 0: +0 with a spec-power
          record, else +8; mode 2: +8) + step B (sub_82BD5EE0: +2 with entry +4 and a shadow map; +16 with entry +20 and model +620 == 1),
          the entry words read from sub_82BDA038's listing on every call (qconstdiff.load_pass_flags); the shader pair = that
          (technique, pass) row of the player's pac\_startup.pac effect table (tools/shaders/effect_table.py, named by our fx registry).
  OURS  - parsed from our code with file:line (a changed shape is a PROBLEM, exit 2): technique_pick.h CharClassOf / CharPassOf (the
          class lists, the pass rule), front_end.cpp's CharPassOf call (the hair class's shadow bit) and its table bind, and
          char_effects.cpp ChooseCharEffectTable (the row, CheckPs: the cube row -> CubeSibling twin, the wrinkle switch, the slots it
          can feed, the shadow slot outside a shadow pass; CheckVs). The table pick is our only pick (S111 OLDCHAIN): a row it cannot
          bind is a counted hole, drawn nothing.
Flags (worst over a group's contexts):
  DIFF      ours draws another variant than the game's row (the defines that differ are named: shadow, cube, wet terms, muscle...);
  HOLE      the table pick does not bind the row: ours draws nothing for it (front_end.cpp Record, counted char_hole_draws), a bug lane;
  UNSETTLED the game's pass is not settled for the context (the +16 term, the hair class's shadow bit) or the game has no row;
  HABIT     ours binds the cube row's non-cube twin (defines minus FX_CUBE_SPECULAR): the slot-11 bake is a 360 habit, the analytic
            lobes give its value (docs/research/s103/CUB.md, [C]); an accepted replace, not a DIFF;
  OK        the game's row.
Limits (docs/research/s111/VARIANTDIFF.md): rigid parts (ours takes the VS of row p & ~1) are not evaluated; the change-colour and logo
editor tables are checked for binding only (a hole there is listed); a material is never alpha-tested here (every yCh fragment has its
alpha twin).
"""
import argparse
import collections
import fnmatch
import glob
import json
import os
import re
import sqlite3
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qconstdiff as CD   # noqa: E402  (the comment mask, the brace matcher, the pass flags of sub_82BDA038: by import)

REPO = CD.REPO
FX_DIR = CD.FX_DIR
TP = 'src/renderer_scene/technique_pick.h'
CHAR = 'src/renderer_scene/char_effects.cpp'
FRONT = 'src/renderer_scene/front_end.cpp'
MODEL_H = 'src/assets/asset_model.h'
STARTUP = 'pac/_startup.pac'
FLAGS = ('DIFF', 'HOLE', 'UNSETTLED', 'HABIT', 'OK')
MODES = (0, 2)          # the model mode word +616 the game makes for a character (seam material_switches 0x825A20A0: never 1 or 3)
WORDS = {'FX_SHADOW': 'shadow', 'FX_CUBE_SPECULAR': 'cube', 'FX_BSS': 'wet', 'FX_BLOOD_TINT': 'wet', 'FX_SWEAT': 'wet', 'FX_BLOOD': 'wet',
         'FX_BLOOD_BUMP': 'blood bump', 'FX_WRINKLES': 'wrinkles', 'FX_MUSCLE': 'muscle', 'FX_ENV_LIGHT': 'env light', 'FX_LOGO': 'logo',
         'FX_SPHERE_OCC': 'sphere occ', 'FX_ID_MAP': 'id map', 'FX_WRINKLE_LEVEL': 'wrinkle level', 'FX_VIEW_NORMAL': 'view normal',
         'FX_SKINNED': 'skinned', 'FX_CHANGE_COLOR': 'change colour', 'FX_CAMO': 'camo', 'FX_SS': 'ss'}


class VariantDiffError(Exception):
    pass


def line_of(text, pos):
    return text.count('\n', 0, pos) + 1


def lit(s):
    """a regex for the code text s: each space of s matches any run of white space (or none)."""
    return r'\s*'.join(re.escape(p) for p in s.split())


# ------------------------------------------------------------------ ours: the code
def _body(ms, head_re, path):
    m = re.search(head_re, ms, re.M)
    if not m:
        raise VariantDiffError(f'{path}: no line matches {head_re!r}')
    ob = ms.index('{', m.end())
    return ob, CD._match(ms, ob, '{', '}')


def _find(ms, text, pat, path, what, a=0, b=None, need=True):
    """-> (match, line) of pat in ms[a:b]; a missing anchor is a VariantDiffError (need) or (None, None)."""
    m = re.compile(pat, re.S).search(ms, a, len(ms) if b is None else b)
    if not m:
        if need:
            raise VariantDiffError(f'{path}: {what} is no longer `{pat}` (read the code, then this parser)')
        return None, None
    return m, line_of(text, m.start())


def _strs(s):
    return re.findall(r'"(\w+)"', s)


def parse_known(expr):
    """the slot set of `slot == N || (slot >= A && slot <= B) || ...`."""
    out = set()
    for a, b in re.findall(r'slot\s*>=\s*(\d+)\s*&&\s*slot\s*<=\s*(\d+)', expr):
        out |= set(range(int(a), int(b) + 1))
    out |= {int(n) for n in re.findall(r'slot\s*==\s*(\d+)', expr)}
    return out


def parse_ours(read):
    """-> (rules, problems): every rule of our wrestler variant choice, each with its file:line."""
    texts = {p: read(p) for p in (TP, CHAR, FRONT, MODEL_H)}
    problems = [f'{p}: holds a merge-conflict marker (run with --ref HEAD)' for p, t in texts.items() if re.search(r'^<<<<<<< ', t, re.M)]
    r, at = {}, {}
    # technique_pick.h: the class lists and the pass rule
    t = texts[TP]
    ms = CD._mask(t)
    a, b = _body(ms, r'^inline CharClass CharClassOf\(', TP)
    m, at['e12'] = _find(ms, t, r'static const char\* const kNames\[\] = \{([^}]*)\};', TP, 'the entry +12 name list', a, b)
    r['e12'] = set(_strs(m.group(1)))
    _m, _ln = _find(ms, t, lit('for (const char* n : kNames) c.e12 = c.e12 || lower == n;'), TP, 'the +12 test', a, b)
    m, at['e4'] = _find(ms, t, r'c\.e4 = c\.e12((?:\s*\|\|\s*lower == "\w+")*)\s*;', TP, 'the entry +4 rule', a, b)
    r['e4_extra'] = set(_strs(m.group(1)))
    m, at['hair'] = _find(ms, t, r'c\.hair = ((?:\s*lower == "\w+"\s*(?:\|\|)?)+);', TP, 'the hair class', a, b)
    r['hair'] = set(_strs(m.group(1)))
    m, at['e20'] = _find(ms, t, r'c\.e20 = lower == "(\w+)";', TP, 'the entry +20 rule', a, b, need=False)   # S111 TECHPASS (absent before it: no +16 term)
    r['e20'] = {m.group(1)} if m else set()
    a, b = _body(ms, r'^inline int CharPassOf\(', TP)
    for key, s in (('base', 'const int base = model_skinned ? 1 : 0;'), ('e12gate', 'if (c.e12) {'),
                   ('mode0', 'if (mode == 0) pass = spec_pow_ok ? base : base + 8;'), ('mode1', 'else if (mode == 1) pass = base + 4;'),
                   ('mode2', 'else if (mode == 2) pass = base + 8;'), ('mode3', 'else pass = base;'), ('shadow', 'if (c.e4 && shadow_ok) pass += 2;')):
        _m, at['pass_' + key] = _find(ms, t, lit(s), TP, f'CharPassOf ({key})', a, b)
    m, at['pass_e20'] = _find(ms, t, lit('if (c.e20 && model620 == 1) pass += 16;'), TP, 'CharPassOf (the +16 term)', a, b, need=False)   # S111 TECHPASS
    r['e20_term'] = m is not None
    # char_effects.cpp: the table pick, its checks, the twin
    t = texts[CHAR]
    ms = CD._mask(t)
    a, b = _body(ms, r'^CharChoice ChooseCharEffectTable\(', CHAR)
    m, at['guard'] = _find(ms, t, r'if \(pass < 0(?: \|\| pass > (\d+))?\) return', CHAR, 'the pass guard', a, b)   # S111 OLDCHAIN review: modelled exactly (table_pick)
    r['pass_max'] = int(m.group(1)) if m.group(1) else None
    _m, at['row'] = _find(ms, t, lit('const read_rule::PassShaders* row = technique_pick::RowOf(*table, effect, technique, uint32_t(p));'), CHAR, 'the table row', a, b)
    _m, at['rigid'] = _find(ms, t, lit('skinned ? row : technique_pick::RowOf(*table, effect, technique, uint32_t(p & ~1));'), CHAR, 'the rigid VS row', a, b)
    _m, at['checkps_call'] = _find(ms, t, lit('PsBind r = CheckPs(f, alpha_test, p, why);'), CHAR, 'the CheckPs call', a, b)
    m, at['twin'] = _find(ms, t, lit('if (r == PsBind::Cube) {') + r'.*?' + lit('const int sib = CubeSibling(f);') + r'.*?'
                          + lit('f = sib, r = CheckPs(f, alpha_test, p, why)'), CHAR, 'the cube twin', a, b, need=False)
    r['twin'] = m is not None
    _m, at['notok'] = _find(ms, t, lit('if (r != PsBind::Ok) {'), CHAR, 'the fallback on a blocked row', a, b)
    _m, at['checkvs_call'] = _find(ms, t, lit('if (!CheckVs(v, f, skinned, why)) return'), CHAR, 'the CheckVs call', a, b)
    a, b = _body(ms, r'^PsBind CheckPs\(', CHAR)
    _m, at['cube'] = _find(ms, t, lit('if (slot == 11) { cube = true; continue; }'), CHAR, 'CheckPs: the cube slot', a, b)
    m, at['ps_known'] = _find(ms, t, r'const bool known = ([^;]*);', CHAR, 'CheckPs: the slots it feeds', a, b)
    r['ps_known'] = parse_known(m.group(1))
    m, _ln = _find(ms, t, r'if \(!known \|\| \(kind != 0 && !\(\(([^)]*)\) && kind == 1\)\)\) other = true;', CHAR, 'CheckPs: the cube kinds', a, b)
    r['ps_cube_ok'] = parse_known(m.group(1))
    _m, at['ps_slot12'] = _find(ms, t, lit('if (slot == 12 && !(pass & 2)) other = true;'), CHAR, 'CheckPs: the shadow slot', a, b)
    _m, at['ps_cube_ret'] = _find(ms, t, lit('if (cube) return'), CHAR, 'CheckPs: the cube answer first', a, b)
    _m, at['ps_wrinkles'] = _find(ms, t, lit('if (Has(fdef, "FX_WRINKLES") && !assets::g_wrk_live.load('), CHAR, 'CheckPs: the wrinkle switch', a, b)
    a, b = _body(ms, r'^bool CheckVs\(', CHAR)
    m, at['vs_subset'] = _find(ms, t, r'if \(!Subset\(vdef, \{([^}]*)\}\)\) return', CHAR, 'CheckVs: the vertex options', a, b)
    r['vs_subset'] = set(_strs(m.group(1)))
    _m, _ln = _find(ms, t, lit('if (Has(vdef, "FX_SKINNED") != skinned) return'), CHAR, 'CheckVs: the skin flag', a, b)
    _m, _ln = _find(ms, t, lit('if (kVariants[v].sampler_count) return'), CHAR, 'CheckVs: no vertex samplers', a, b)
    # front_end.cpp: the table pick's pass and its bind (S111 OLDCHAIN: a row it cannot bind is a hole; there is no other pick to read)
    t = texts[FRONT]
    ms = CD._mask(t)
    a, b = _body(ms, r'^bool FrontEnd::Record\(', FRONT)
    m, at['pass_call'] = _find(ms, t, r'const int pass = technique_pick::CharPassOf\(cl, ([^,]*), ([^,]*), mode, cm\.spec_pow_prm(?:, (\w+))?\);', FRONT, 'the CharPassOf call', a, b)
    r['m620_arg'] = m.group(3) is not None   # S111 TECHPASS: the model +620 word handed to the pass rule (absent before it)
    sh = ' '.join(m.group(2).split())
    if sh not in ('shadow_map', 'shadow_map && !cl.hair'):
        raise VariantDiffError(f"{FRONT}:{at['pass_call']}: the shadow argument of CharPassOf is `{sh}` (known: shadow_map [&& !cl.hair])")
    r['hair_noshadow'] = '!cl.hair' in sh
    _m, at['char_b'] = _find(ms, t, lit('if (cb.char_table && !cb.debug) char_b = &cb;'), FRONT, 'the table bind (any other answer is the hole)', a, b)
    for key, path, name in (('wrk_live', MODEL_H, 'g_wrk_live'),):
        t = texts[path]
        m, at[key] = _find(CD._mask(t), t, r'inline std::atomic<bool> ' + name + r'\{(true|false)\};', path, f'the default of {name}')
        r[key] = m.group(1) == 'true'
    r['at'] = {k: f'{TP if k.startswith(("e12", "e20", "e4", "hair", "pass_")) and k != "pass_call" else CHAR if k in CHAR_KEYS else MODEL_H if k == "wrk_live" else FRONT}:{v}'
               for k, v in at.items() if v is not None}
    return r, problems


CHAR_KEYS = {'guard', 'row', 'rigid', 'checkps_call', 'twin', 'notok', 'checkvs_call', 'cube', 'ps_known', 'ps_slot12', 'ps_cube_ret', 'ps_wrinkles',
             'vs_subset'}


def default_reader(ref=None):
    def read(p):
        if ref:
            return subprocess.run(['git', '-C', REPO, 'show', f'{ref}:{p}'], capture_output=True, text=True, encoding='utf-8', check=True).stdout
        with open(os.path.join(REPO, p), encoding='utf-8') as fh:
            return fh.read()
    return read


# ------------------------------------------------------------------ ours: the fx registry
def load_registry(fx_dir=None):
    """-> {effect: {'fragment': [variant...], 'vertex': [...], 'by_name': {full name: variant}}}; a variant = {'full', 'defines', 'slots'
    {slot: kind}, 'samplers'} in the fx.json order (the order our chooser walks)."""
    out = {}
    for p in sorted(glob.glob(os.path.join(fx_dir or FX_DIR, 'yCh_*.fx.json'))):
        with open(p, encoding='utf-8') as fh:
            d = json.load(fh)
        eff = d.get('effect') or os.path.basename(p).split('.')[0]
        e = out.setdefault(eff, {'fragment': [], 'vertex': [], 'by_name': {}})
        for stage in ('fragment', 'vertex'):
            for v in d.get(stage, []):
                x = {'full': f"{eff}_{v['name']}_{stage}", 'defines': frozenset(v.get('defines') or []),
                     'slots': {int(s['slot']): s.get('kind') for s in v.get('samplers', [])}}
                e[stage].append(x)
                e['by_name'][x['full']] = x
    return out


def twin_of(reg, eff, f):
    """CubeSibling: the fragment of the same effect whose defines are f's minus FX_CUBE_SPECULAR; None = none."""
    want = f['defines'] - {'FX_CUBE_SPECULAR'}
    return next((g for g in reg[eff]['fragment'] if g is not f and g['defines'] == want), None)


# ------------------------------------------------------------------ game
def _key(paths):
    key = []
    for f in paths:
        try:
            st = os.stat(f)
            key.append([os.path.abspath(f), st.st_size, st.st_mtime_ns])
        except OSError:
            key.append([os.path.abspath(f), None, None])
    return json.dumps(key)


def load_tables(cache_path=None):
    """-> ({effect: [[(vs full name or None, ps full name or None) per pass] per technique]}, None) from the player's pac/_startup.pac
    (the table our table pick reads: live_assets.cpp PublishEffectTable), yCh_ effects; (None, why) when it cannot be read. Cached by
    its inputs' size and mtime beside the q index."""
    try:
        sys.path.insert(0, os.path.join(REPO, 'tools', 'shaders'))
        import effect_table
        import shaderlib
        key = _key([os.path.join(shaderlib.GAME, STARTUP.replace('/', os.sep))] + sorted(glob.glob(os.path.join(effect_table.FX_DIR, '*.fx.json')))
                   + [effect_table.__file__, shaderlib.__file__, os.path.abspath(__file__)])
        if cache_path is None:
            import hashlib
            import qdecomp
            cache_path = os.path.join(os.path.dirname(qdecomp.IMAGE),
                                      f"variant_rows.{hashlib.sha1(os.path.abspath(REPO).lower().encode()).hexdigest()[:10]}.json")
        try:
            with open(cache_path, encoding='utf-8') as fh:
                c = json.load(fh)
            if c.get('key') == key:
                return {e: [[tuple(p) for p in t] for t in ts] for e, ts in c['tables'].items()}, None
        except (OSError, ValueError, KeyError, AttributeError, TypeError):
            pass
        res = effect_table.load_all(files=[STARTUP])
    except Exception as e:  # noqa: BLE001 - the player's file is the game side; its absence is a PROBLEM said by the caller
        return None, f'{type(e).__name__}: {e}'
    out = {}
    for e in res.values():
        if not e['effect'].startswith('yCh_') or e['effect'] in out:
            continue
        techs = [[None] * 0 for _ in e['techniques']]
        for i, t in enumerate(e['techniques']):
            techs[i] = [((p['vs'] or {}).get('name'), (p['ps'] or {}).get('name')) for p in sorted(t['passes'], key=lambda p: p['pass'])]
        out[e['effect']] = techs
    if os.path.isdir(os.path.dirname(cache_path) or '.'):
        tmp = f'{cache_path}.{os.getpid()}.tmp'
        try:
            with open(tmp, 'w', encoding='utf-8') as fh:
                json.dump({'key': key, 'tables': out}, fh)
            os.replace(tmp, cache_path)
        except OSError:
            pass
    return out, None


EDIT = 'pac/edit/edit.pac'
EDITOR_LEAVES = ('p7/0', 'p9/0')   # live_assets.cpp:109-110: p7/0 = the CAW logo rows, p9/0 = the Superstar Threads change-colour rows


def load_editor_tables(cache_path=None):
    """-> ({leaf: {effect lower-case: [[(vs, ps) per pass] per technique]}}, None) of the editors' tables in the player's pac/edit/edit.pac
    (ChooseCharEffectTable takes the p9/0 row for a recoloured body and the p7/0 row for a CAW logo material when the table names the
    effect, char_effects.cpp:275-282); (None, why) when the file cannot be read. Cached as load_tables."""
    try:
        sys.path.insert(0, os.path.join(REPO, 'tools', 'shaders'))
        import effect_table
        import shaderlib
        key = _key([os.path.join(shaderlib.GAME, EDIT.replace('/', os.sep))] + sorted(glob.glob(os.path.join(effect_table.FX_DIR, '*.fx.json')))
                   + [effect_table.__file__, shaderlib.__file__, os.path.abspath(__file__)])
        if cache_path is None:
            import hashlib
            import qdecomp
            cache_path = os.path.join(os.path.dirname(qdecomp.IMAGE),
                                      f"variant_rows_edit.{hashlib.sha1(os.path.abspath(REPO).lower().encode()).hexdigest()[:10]}.json")
        try:
            with open(cache_path, encoding='utf-8') as fh:
                c = json.load(fh)
            if c.get('key') == key:
                return {lf: {e: [[tuple(p) for p in t] for t in ts] for e, ts in tab.items()} for lf, tab in c['tables'].items()}, None
        except (OSError, ValueError, KeyError, AttributeError, TypeError):
            pass
        res = effect_table.load_all(files=[EDIT])
    except Exception as e:  # noqa: BLE001 - the player's file is the game side; its absence is said by the caller
        return None, f'{type(e).__name__}: {e}'
    out = {lf: {} for lf in EDITOR_LEAVES}
    for e in res.values():
        lc = e['effect'].lower()
        if e['leaf'] not in out or not lc.startswith('ych_') or lc in out[e['leaf']]:
            continue
        out[e['leaf']][lc] = [[((p['vs'] or {}).get('name'), (p['ps'] or {}).get('name')) for p in sorted(t['passes'], key=lambda p: p['pass'])]
                              for t in e['techniques']]
    if os.path.isdir(os.path.dirname(cache_path) or '.'):
        tmp = f'{cache_path}.{os.getpid()}.tmp'
        try:
            with open(tmp, 'w', encoding='utf-8') as fh:
                json.dump({'key': key, 'tables': out}, fh)
            os.replace(tmp, cache_path)
        except OSError:
            pass
    return out, None


def family_of(path):
    """the pac family of a pac path: pac/ch (the stock wrestlers), pac/edit (created-wrestler parts), dlc/<pack>, ..."""
    p = path.replace('\\', '/')
    parts = p.split('/')
    if parts[0] == 'dlc' and len(parts) > 2:
        return 'dlc/' + parts[1]
    return '/'.join(parts[:2]) if len(parts) > 2 else parts[0]


def load_records(path=None, pac=None):
    """-> ({(effect, technique word, spec-power): {'n', 'families': {family: n}, 'pacs': {stem: n}}}, None) from the asset index's yCh_
    material records; spec-power = the record names g_mSpecPowPrm with a texture (the game's entry +16, [L] for a runtime word);
    (None, why) without the index. pac = a pac stem / path substring the records must come from."""
    if path is None:
        import qdecomp
        c = sorted(glob.glob(os.path.join(os.path.dirname(qdecomp.IMAGE), 'asset.s*.db')))
        path = c[-1] if c else None
    if not path or not os.path.exists(path):
        return None, 'no asset index (q asset builds it)'
    con = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    out = {}
    try:
        q = ("SELECT m.effect, m.technique_word, m.samplers, p.path FROM material m JOIN entry e ON e.id = m.entry_id "
             "JOIN pach h ON h.id = e.pach_id JOIN pacs p ON p.id = h.pac_id WHERE m.effect LIKE 'yCh!_%' ESCAPE '!'")
        for eff, tw, samp, pp in con.execute(q):
            if pac and pac.lower() not in pp.lower().replace('\\', '/'):
                continue
            try:
                ss = json.loads(samp or '{}')
            except ValueError:
                ss = {}
            sp = isinstance(ss, dict) and ss.get('g_mSpecPowPrm') not in (None, '', '""')
            g = out.setdefault((eff, int(tw or 0), bool(sp)), {'n': 0, 'families': {}, 'pacs': {}})
            g['n'] += 1
            fam, stem = family_of(pp), os.path.splitext(os.path.basename(pp))[0]
            g['families'][fam] = g['families'].get(fam, 0) + 1
            g['pacs'][stem] = g['pacs'].get(stem, 0) + 1
    finally:
        con.close()
    return out, None


def game_pass(eff, flags, mode, spec, shadow, m620):
    """the game's pass (sub_82BD4F88 then sub_82BD5EE0, base 1) and the terms it took."""
    n = eff.lower()
    p, why = 1, ['base 1']
    if n in flags.get(12, ()):
        if mode == 0 and not spec:
            p, why = p + 8, why + ['+8 (E+12, mode 0, no spec-power record)']
        elif mode == 2:
            p, why = p + 8, why + ['+8 (E+12, mode 2)']
    if shadow and n in flags.get(4, ()):
        p, why = p + 2, why + ['+2 (E+4, shadow map)']
    if m620 and n in flags.get(20, ()):
        p, why = p + 16, why + ['+16 (E+20, model +620 == 1)']
    return p, why


def row_of(tables, eff, tech, p):
    ts = tables.get(eff)
    if ts is None or tech >= len(ts) or p >= len(ts[tech]):
        return None
    return ts[tech][p]


# ------------------------------------------------------------------ ours: the choice
def ours_term16(eff, r):
    """S111 TECHPASS: ours adds the +16 term for this effect when model +620 == 1 (CharClassOf's e20 line, CharPassOf's +16 line, the call hands the word)."""
    return bool(eff.lower() in r.get('e20', ()) and r.get('e20_term') and r.get('m620_arg'))


def ours_pass(eff, r, mode, spec, shadow, m620=False):
    n = eff.lower()
    e12 = n in r['e12']
    e4 = e12 or n in r['e4_extra']
    hair = n in r['hair']
    p = 1
    if e12:
        p = 1 + (0 if spec else 8) if mode == 0 else 1 + 4 if mode == 1 else 1 + 8 if mode == 2 else 1
    if e4 and shadow and not (hair and r['hair_noshadow']):
        p += 2
    if m620 and ours_term16(eff, r):
        p += 16
    return p


def table_pick(eff, tech, p, tables, reg, r):
    """ChooseCharEffectTable for a skinned part -> {'ok', 'vs', 'ps', 'twin', 'why', 'at'}."""
    if p < 0 or (r['pass_max'] is not None and p > r['pass_max']):   # the pass guard of ChooseCharEffectTable, as parsed (S111 OLDCHAIN review: the tool cannot disagree with the code)
        return {'ok': False, 'why': f'pass {p} refused by the pass guard' + (f' (cap {r["pass_max"]})' if r['pass_max'] is not None else ''), 'at': [r['at']['guard']]}
    at = [r['at']['row']]
    row = row_of(tables, eff, tech, p)
    if row is None or not row[0] or not row[1]:
        return {'ok': False, 'why': f'no table row for technique {tech} pass {p}' if row is None else 'the table\'s shader is not named by the registry', 'at': at}
    e = reg.get(eff)
    f = e and e['by_name'].get(row[1])
    v = e and e['by_name'].get(row[0])
    if not f or not v:
        return {'ok': False, 'why': f"the table's {'pixel' if not f else 'vertex'} shader is not in the registry", 'at': at}
    twin = False
    if 11 in f['slots']:
        if not r['twin']:
            return {'ok': False, 'why': 'reads the cube-specular slot 11 (no twin rule)', 'at': at + [r['at']['cube']]}
        sib = twin_of(reg, eff, f)
        if sib is None:
            return {'ok': False, 'why': 'the cube row has no non-cube sibling in the registry', 'at': at + [r['at']['twin']]}
        f, twin = sib, True
        at.append(r['at']['twin'])
    if 'FX_WRINKLES' in f['defines'] and not r['wrk_live']:
        return {'ok': False, 'why': 'reads the wrinkle map (g_wrk_live off)', 'at': at + [r['at']['ps_wrinkles']]}
    for s, k in f['slots'].items():
        if s == 11:
            return {'ok': False, 'why': 'the twin still reads slot 11', 'at': at + [r['at']['cube']]}
        if s not in r['ps_known'] or (k != '2d' and s not in r['ps_cube_ok']):
            return {'ok': False, 'why': f'a sampler slot the front end cannot feed (s{s})', 'at': at + [r['at']['ps_known']]}
        if s == 12 and not (p & 2):
            return {'ok': False, 'why': 'a shadow-map reader in a pass with no shadow map', 'at': at + [r['at']['ps_slot12']]}
    if not v['defines'] <= r['vs_subset'] or 'FX_SKINNED' not in v['defines'] or v['slots']:
        return {'ok': False, 'why': 'the vertex variant has an option / a sampler this path does not feed', 'at': at + [r['at']['vs_subset']]}
    return {'ok': True, 'vs': v, 'ps': f, 'twin': twin, 'why': '', 'at': at}


# ------------------------------------------------------------------ the join
def short(eff, x):
    return x['full'][len(eff) + 1:] if x and x['full'].startswith(eff + '_') else (x['full'] if x else '-')


def define_diff(game, ours):
    """'ours +FX_SHADOW (shadow), -FX_CUBE_SPECULAR (cube)' for the defines ours adds / lacks against the game's."""
    plus = sorted(ours['defines'] - game['defines'])
    minus = sorted(game['defines'] - ours['defines'])
    w = lambda d: f"{d} ({WORDS[d]})" if d in WORDS else d
    return ', '.join([f'+{w(d)}' for d in plus] + [f'-{w(d)}' for d in minus]) or 'same defines, another variant'


def judge(eff, ctx, gp, grow, ours, reg, r, path='table', grow_ns=None):
    """-> (flag, why) for one context. grow_ns = the game's row of the same (technique, pass) without the +2 shadow term (gp - 2), for
    the hair class: ours is UNSETTLED only when it is exactly that row (or its cube twin), else DIFF."""
    e = reg.get(eff, {'by_name': {}})
    gps = e['by_name'].get(grow[1]) if grow else None
    gvs = e['by_name'].get(grow[0]) if grow else None
    if ctx.get('m620') and not ours_term16(eff, r):
        return 'UNSETTLED', (f"the game's +16 pass p{gp} ({short(eff, gps)}) needs model +620 == 1: +620 = the character's +5064 = its dim predicate "
                             'sub_8259F0E0 (+5076 = info id <=u 9999, +5072 = a CASD file for an id above 9999: sub_825A20A0 +0x134..+0x14C), copied by '
                             'sub_825A1E38 before every draw [C]; the value a stock / created wrestler holds is [L] (the info id); '
                             'ours records the term only (front_end.cpp CharPassOf call)')
    if grow is None or gps is None:
        return 'UNSETTLED', f'the game has no named row for pass p{gp}'
    if path == 'table' and not ours.get('ok'):
        return 'HOLE', f"the table pick does not bind ({ours['why']}): ours draws nothing for it (a counted hole, front_end.cpp Record: char_hole_draws) " \
                       f"where the game draws {short(eff, gps)}"
    ops, ovs = (ours['ps'], ours['vs'])
    if ops is None:
        return 'DIFF', 'ours finds no variant'
    vs_note = '' if ovs is gvs else f"; vs ours {short(eff, ovs)} vs the game's {short(eff, gvs)}: {define_diff(gvs, ovs) if gvs else '?'}"
    if ops is gps:
        return ('OK', '') if not vs_note else ('DIFF', 'the pixel shader is the game\'s' + vs_note)
    tw = twin_of(reg, eff, gps) if 11 in gps['slots'] else None
    if tw is ops:
        return ('HABIT' if not vs_note else 'DIFF'), f"ours binds {short(eff, ops)}, the non-cube twin of the game's {short(eff, gps)} (the slot-11 bake dropped: CUB.md)" + vs_note
    n = eff.lower()
    if path == 'table' and n in r['hair'] and r['hair_noshadow'] and ctx['shadow'] and 'FX_SHADOW' in gps['defines'] and 'FX_SHADOW' not in ops['defines']:
        alt = e['by_name'].get(grow_ns[1]) if grow_ns else None   # the game's own row without the +2 term: ours must be exactly it (or its cube twin)
        if alt is not None and (ops is alt or (11 in alt['slots'] and twin_of(reg, eff, alt) is ops)):
            return 'UNSETTLED', (f"hair: ours never adds the +2 shadow pass ({r['at']['pass_call']}: !cl.hair) and draws the game's p{gp - 2} row; "
                                 "the game's +2 is the per-thread shadow slot (sub_82BA7868 ignores the part), which the character's translucent "
                                 "draw sub_8259FAE8 sets to 0 (+0x18C) before it draws, so a part drawn there gets no +2 [C]; that every hair part is "
                                 f"drawn in that pass is [L] (the captures agree, YCH.md section 1): ours {short(eff, ops)}, the game's row {short(eff, gps)}")
    return 'DIFF', f"ours {short(eff, ops)} where the game draws {short(eff, gps)}: {define_diff(gps, ops)}" + vs_note


def contexts(eff, r, flags):
    n = eff.lower()
    out = []
    for mode in MODES:
        for shadow in (True, False):
            out.append({'mode': mode, 'shadow': shadow, 'm620': False})
    if n in flags.get(20, ()):
        for shadow in (True, False):
            out.append({'mode': 0, 'shadow': shadow, 'm620': True})
    return out


def ctx_name(c):
    return f"m{c['mode']} {'shadow' if c['shadow'] else 'noshadow'}" + (' m620=1' if c['m620'] else '')


def worst(flags_):
    return min(flags_, key=FLAGS.index) if flags_ else 'OK'


def evaluate(groups, tables, reg, r, flags):
    """-> rows: one per material group, with its contexts on the table pick (our only pick: S111 OLDCHAIN)."""
    rows = []
    for (eff, tech, spec), g in sorted(groups.items()):
        cs = []
        for c in contexts(eff, r, flags):
            gp, gwhy = game_pass(eff, flags, c['mode'], spec, c['shadow'], c['m620'])
            grow = row_of(tables, eff, tech, gp)
            op = ours_pass(eff, r, c['mode'], spec, c['shadow'], c['m620'])
            tp = table_pick(eff, tech, op, tables, reg, r)
            fl, why = judge(eff, c, gp, grow, tp, reg, r, grow_ns=row_of(tables, eff, tech, gp - 2) if c['shadow'] and gp >= 2 else None)
            cs.append({'ctx': ctx_name(c), 'game_pass': gp, 'game_terms': gwhy, 'game_vs': grow[0] if grow else None, 'game_ps': grow[1] if grow else None,
                       'ours_pass': op, 'ours_vs': tp['vs']['full'] if tp.get('ok') else None,
                       'ours_ps': tp['ps']['full'] if tp.get('ok') else None,
                       'ours_path': 'table' if tp.get('ok') else 'hole', 'flag': fl, 'why': why,
                       'at': tp['at'] + ([] if tp.get('ok') else [r['at']['char_b']])})
        rows.append({'effect': eff, 'technique': tech, 'spec_pow': spec, 'records': g['n'], 'families': g['families'], 'pacs': g['pacs'],
                     'flag': worst([c['flag'] for c in cs]), 'contexts': cs})
    return rows


def editor_picks(groups, editor, reg, r, flags):
    """-> one dict per (editor table, group, context): ChooseCharEffectTable on that table with OUR pass (a logo / recoloured material
    draws from it), 'ok' False = the row does not bind (a hole). The +16 term does not change the pick, so those contexts are left out."""
    out = []
    for leaf, tab in sorted(editor.items()):
        for (eff, tech, spec), g in sorted(groups.items()):
            if eff.lower() not in tab:
                continue
            one = {eff: tab[eff.lower()]}
            for c in contexts(eff, r, flags):
                if c['m620']:
                    continue
                op = ours_pass(eff, r, c['mode'], spec, c['shadow'])
                tp = table_pick(eff, tech, op, one, reg, r)
                out.append({'leaf': leaf, 'effect': eff, 'technique': tech, 'spec_pow': spec, 'ctx': ctx_name(c), 'pass': op, 'ok': tp['ok'],
                            'ps': tp['ps']['full'] if tp['ok'] else None, 'why': tp['why'], 'records': g['n']})
    return out


def distinct(rows, key='contexts', flags_=('DIFF', 'HOLE', 'UNSETTLED', 'HABIT')):
    """-> {(flag, why): [(effect, technique, spec, ctx, records)]}: each departure once."""
    out = collections.OrderedDict()
    for row in rows:
        for c in row[key]:
            if c['flag'] in flags_:
                out.setdefault((c['flag'], c['why']), []).append((row['effect'], row['technique'], row['spec_pow'], c['ctx'], row['records'], row['families']))
    return out


def fam_text(fams):
    return ', '.join(f'{k} {v}' for k, v in sorted(fams.items(), key=lambda kv: (-kv[1], kv[0])))


def compute(read, flags, tables, groups, fx_dir=None, effect=None):
    """-> (rows, rules, problems): our rules parsed from read(path), joined with the game's pass flags / effect table and the records."""
    r, problems = parse_ours(read)
    reg = load_registry(fx_dir)
    missing = sorted({e for (e, _t, _s) in groups if e not in reg or e not in tables})
    for e in missing:
        problems.append(f"{e}: material records name it but {'our fx registry' if e not in reg else 'the startup effect table'} has no such effect")
    groups = {k: v for k, v in groups.items() if k[0] not in missing and (not effect or fnmatch.fnmatchcase(k[0], effect))}
    return evaluate(groups, tables, reg, r, flags), r, problems


def load_game(con, pac=None):
    """-> (flags, tables, groups): the pass flags of sub_82BDA038, the startup effect table, the material records; a missing one raises."""
    flags, ferr = CD.load_pass_flags(con)
    if flags is None:
        raise VariantDiffError("the model draw's pass flags (sub_82BDA038) were not read: " + '; '.join(ferr))
    tables, terr = load_tables()
    if tables is None:
        raise VariantDiffError(f"the player's {STARTUP} effect table was not read: {terr}")
    groups, gerr = load_records(pac=pac)
    if groups is None:
        raise VariantDiffError(gerr)
    return flags, tables, groups


def run(con, a, read=None, fx_dir=None):
    flags, tables, groups = load_game(con, a.pac)
    rows, r, problems = compute(read or default_reader(a.ref), flags, tables, groups, fx_dir, a.effect)
    sel = [x for x in rows if not a.flag or x['flag'] in a.flag]
    editor, eerr = load_editor_tables()
    ed = None if editor is None else editor_picks({k: v for k, v in groups.items() if not a.effect or fnmatch.fnmatchcase(k[0], a.effect)},
                                                  editor, load_registry(fx_dir), r, flags)
    if a.json:
        print(json.dumps({'rows': sel, 'rules': {k: (sorted(v) if isinstance(v, set) else v) for k, v in r.items()}, 'problems': problems,
                          'editor': ed, 'editor_error': eerr}, indent=1, default=str))
    else:
        report(sel, rows, r, flags, a)
        if ed is None:
            print(f"the editor tables ({EDIT}) were not read: {eerr}")
        else:
            bad = [x for x in ed if not x['ok']]
            print(f"the editor tables ({EDIT} {' / '.join(EDITOR_LEAVES)}: a CAW logo material / a recoloured body, {CHAR}:275-282): "
                  f"{len(ed)} contexts, {len(bad)} do not bind (holes)")
            for x in bad:
                print(f"  HOLE      {x['leaf']} {x['effect']} T{x['technique']} {'spec' if x['spec_pow'] else 'nospec'} {x['ctx']} p{x['pass']}: {x['why']} ({x['records']} records)")
    for p in problems:
        print('PROBLEM: ' + p)
    return 2 if problems else 0


def report(sel, rows, r, flags, a):
    cur = None
    for x in sel:
        if x['effect'] != cur:
            cur = x['effect']
            print(f'== {cur}')
        print(f"{x['flag']:9s} T{x['technique']} {'spec' if x['spec_pow'] else 'nospec'}  {x['records']} records ({fam_text(x['families'])})")
        for c in x['contexts']:
            if a.contexts or c['flag'] != 'OK':
                print(f"    {c['flag']:9s} {c['ctx']:22s} game p{c['game_pass']} {short(x['effect'], {'full': c['game_ps']}) if c['game_ps'] else '-'}"
                      f"  ours p{c['ours_pass']} {c['ours_path']} {short(x['effect'], {'full': c['ours_ps']}) if c['ours_ps'] else '-'}"
                      + (f"  -- {c['why']}" if c['why'] else '') + f"  [{', '.join(c['at'])}]")
    print('totals (records by the group\'s worst flag, the default table path):')
    effs = sorted({x['effect'] for x in sel})
    for e in effs:
        xs = [x for x in sel if x['effect'] == e]
        by = {f: sum(x['records'] for x in xs if x['flag'] == f) for f in FLAGS}
        fam = {}
        for x in xs:
            if x['flag'] in ('DIFF', 'HOLE'):
                for k, v in x['families'].items():
                    fam[k] = fam.get(k, 0) + v
        print(f"  {e:18s} {sum(x['records'] for x in xs):6d} records: " + ', '.join(f'{f} {by[f]}' for f in FLAGS if by[f])
              + (f"; DIFF / HOLE by family: {fam_text(fam)}" if fam else ''))
    for title, key in (('the table pick (our only pick)', 'contexts'),):
        d = distinct(sel, key)
        print(f'distinct departures on {title}: {len(d)}')
        for (fl, why), hits in d.items():
            recs = sum(h[4] for h in {(h[0], h[1], h[2]): h for h in hits}.values())
            fam = {}
            for h in {(h[0], h[1], h[2]): h for h in hits}.values():
                for k, v in h[5].items():
                    fam[k] = fam.get(k, 0) + v
            where = sorted({f"{h[0]} T{h[1]} {'spec' if h[2] else 'nospec'}" for h in hits})
            ctxs = sorted({h[3] for h in hits})
            print(f"  {fl:9s} {why}\n            groups {', '.join(where[:6])}{' ...' if len(where) > 6 else ''}; contexts {', '.join(ctxs)}; "
                  f"{recs} records ({fam_text(fam)})")
    print(f"sources: the pass flags of sub_{CD.PASS_FLAGS_FN:08X} (E+12 {len(flags.get(12, ()))} names, E+4 {len(flags.get(4, ()))}, E+20 {len(flags.get(20, ()))}); "
          f"the player's {STARTUP} effect table; the asset index's yCh_ material records{' from pacs matching ' + repr(a.pac) if a.pac else ''}; "
          f"our rules {r['at']['pass_call']} (pass), {r['at']['row']} (row), {r['at']['twin'] if r['twin'] else 'NO cube twin rule'} (twin), "
          f"{r['at']['char_b']} (the table bind; any other answer is a hole)")
    print(f"switches (defaults read from the code): live wrinkles {'on' if r['wrk_live'] else 'OFF'} ({r['at']['wrk_live']})")


def main(argv, con):
    ap = argparse.ArgumentParser(prog='q variantdiff', description=__doc__.split('\n\n')[0])
    ap.add_argument('--effect', help='an effect name or a glob (yCh_skin*)')
    ap.add_argument('--pac', help='only the material records of pacs whose path holds this (a stem like ch139, or pac/ch)')
    ap.add_argument('--flag', nargs='+', choices=FLAGS, help='only groups with this flag')
    ap.add_argument('--contexts', action='store_true', help='print the OK contexts too')
    ap.add_argument('--ref', help='read our code at this git ref (git show) instead of the working tree')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    try:
        return run(con, a)
    except (VariantDiffError, CD.DiffError) as e:
        print(f'q variantdiff: {e}', file=sys.stderr)
        return 2
