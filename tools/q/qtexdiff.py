r"""q texdiff: per texture slot a GAME shader samples, what the GAME binds there against what OURS binds (S111 TEXDIFF).

    py tools/q/q.py texdiff [--effect E] [--flag DIFF|UNSETTLED|MAPREC|HABIT|UNUSED|OK ...] [--json] [--ref REF]

One row per (effect, stage, sampler slot) of our fx registry (src/renderer/shaders/fx/*.fx.json: every variant's `samplers`) for the
effects whose materials our front end binds (yCh_* through the character switch, yBG_ / yProp_ / yDefault through the plain chains).
Flags:
  DIFF       the game binds something real there (a material texture by its sampler name, or code that names the sampler) and ours
             binds a stand-in (black / white / grey / a neutral map), nothing (the white default), or a material texture of a
             DIFFERENT sampler name (ROLE);
  UNSETTLED  the game's source is not known from the name table (a row no material record names and no code names: GAME-NOREC; or
             no row at that slot: NOROW, bound by code by slot number or not at all) and ours binds a stand-in: never silently OK;
  MAPREC     ours binds one of our runtime maps where some of the game's material records name a texture (the name binds it);
  HABIT      ours never binds a variant that reads the slot (the cube-specular slot: the non-cube twin is bound, a 360 bake dropped);
  UNUSED     the game never samples the slot for this effect: no material record of the player's files names the effect (NOUSE), or
             no record's technique has a pass whose pixel shader reads the slot (UNSAMPLED; S111 review: the yDefault slot-1 false alarm);
  OK         ours binds the material texture of the same sampler name, or one of our maps where the game's name table has no
             material source.
A material field ours binds that no sampler name loads (jboy.h) and no line of src writes is a stand-in (always empty), not OK.
Sources of truth (read on every call; nothing about the game committed here):
  ours  - the fx registry above: which variants read which slot, with their defines (a variant with FX_CHANGE_COLOR is a
          change-colour bind, one that reads slot 14 a wet bind, else a dry one: the role function the front end picks,
          front_end.cpp `switch (b->change_colour ? RoleOfChangeSlot(slotn) : wet ? RoleOfWetSlot(slotn) : RoleOfSlot(slotn))`);
        - char_effects.cpp: RoleOfSlot's switch, RoleOfWetSlot's ifs, RoleOfChangeSlot (+ change_colour.h MaskIndexOfSlot), CheckPs's
          cube slot (`if (slot == N) { cube = true`); each role function must keep exactly that shape (any other statement, such as an
          early return, is a PROBLEM: a slot's role would go unread);
        - front_end.cpp: the character switch's case bodies, the plain chains `if (slotn == N) image = ...` of every bind function,
          the vertex loops, the draw-time replacement line (`set.slots[slot].image = b.cube_slot[slot] == 1 ? ...`) and the liquid
          map's effect prefixes (the `liquid_of(` line); a case body is read for `tex(mat.F)` (a material texture), the stand-ins,
          `cube_slot[slotn] = K` / `liquid_slot` / `lm_image` (one of our maps);
        - jboy.h: which sampler names load into which Material field (`pname == "X" ... slot = &mat.F`): the ROLE check.
  game  - the name table sub_82BAAEE8 fills (qconstdiff.load_game: rows of bank 7 / 8 are the samplers, the register is the slot),
          per effect group (the common table + the effect's own sub-table, by its lowercased name);
        - the player's material records (asset index, table `material`, columns `samplers` and `technique_word`): GAME-MATERIAL = the
          effect's records that name the row's sampler with a texture AND whose technique samples the slot (count, the commonest
          textures); an fx effect with no record takes the records of the effect its name starts with once a trailing `LM` is cut
          (their techniques not checked), else it is NOUSE;
        - the player's effect files (tools/shaders/effect_table.py: _startup / car / edit pacs, every file's table merged): which
          pixel sampler slots each technique's passes read (the pass's shader by name in our fx registry);
        - code that names the sampler string (q index `strrefs`, any function but sub_82BAAEE8): GAME-CODE with the functions.
        - the model draw's pass flags (qconstdiff.load_pass_flags, sub_82BDA038; S111 CONSTPASS): a technique's pass the draw never
          picks for the effect (qconstdiff.reachable_passes) does not count.
        - S111 MUSCLEMAP: when the character switch goes through `muscle_map::Role` (muscle_map.h), slot kSlot of a fragment that
          defines the ReadsMuscle word, on a bind that is not change-colour, takes its role (ctx `dry+muscle` / `wet+muscle`), and
          `muscle_map::Image` binds its material field; any other shape of muscle_map.h is a PROBLEM.
Limits (docs/research/s111/TEXDIFF.md): the binder of a NOROW slot (shadow / rim / light-map page) is not read: ours binding one of
its maps there is OK, a stand-in UNSETTLED.
"""
import argparse
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
import qconstdiff as CD   # noqa: E402  (the name-table walker, the masks, the resolver routing: by import, never edited here)

REPO = CD.REPO
FX_DIR = CD.FX_DIR
CHAR = 'src/renderer_scene/char_effects.cpp'
FRONT = 'src/renderer_scene/front_end.cpp'
CC = 'src/scene_live/change_colour.h'
JBOY = 'src/assets/jboy.h'
MUSCLE = 'src/renderer_scene/muscle_map.h'   # S111 MUSCLEMAP: read only when the character switch goes through muscle_map::Role
SAMPLERS = 'src/assets/material_samplers.h'   # S111 UNIFY2: the sampler name -> field table jboy.h calls (absent at older refs: then jboy.h holds the chain)
SAMPLER_TYPES = (7, 8)          # a name-table row's word +24: 7 / 8 = sampler rows (qconstdiff.BANK_STAGES: the float banks)
STANDINS = ('black_cube_', 'neutral_normal_', 'black_', 'white_', 'grey_')
# our own variables the draw-time replacement line reads (front_end.cpp Record), named for the reader; an unlisted one prints as is
MAP_NAMES = {'cube': "the object's ambient cube (BuildAmbientCube)", 'rd_map': "the frame's shadow map (or the props map)",
             'plane_map': "the frame's plane shadow map", 'rim_map': "the frame's rim map", 'liquid': "the wrestler's liquid map",
             'lm': 'the light-map page (the stage page, else the material\'s own)'}
FLAGS = ('DIFF', 'UNSETTLED', 'MAPREC', 'HABIT', 'UNUSED', 'OK')
EMPTY_TEX = ('', '""')           # the asset index's spelling of a sampler record whose texture name is empty (no picture)


class TexDiffError(Exception):
    pass


def line_of(text, pos):
    return text.count('\n', 0, pos) + 1


# ------------------------------------------------------------------ ours: the fx registry
def load_samplers(fx_dir=None):
    """-> {(effect, stage, slot): {'kind', 'readers': [(variant, frozenset(defines), frozenset(slots))]}}."""
    out = {}
    for p in sorted(glob.glob(os.path.join(fx_dir or FX_DIR, '*.fx.json'))):
        with open(p, encoding='utf-8') as fh:
            d = json.load(fh)
        eff = d.get('effect') or os.path.basename(p).split('.')[0]
        for stage, sn in (('vertex', 'vs'), ('fragment', 'ps')):
            for v in d.get(stage, []):
                slots = frozenset(int(s['slot']) for s in v.get('samplers', []))
                for s in v.get('samplers', []):
                    e = out.setdefault((eff, sn, int(s['slot'])), {'kind': s.get('kind'), 'readers': []})
                    e['readers'].append((v.get('name'), frozenset(v.get('defines') or []), slots))
    return out


# ------------------------------------------------------------------ ours: the code
def _body(text, head_re, path):
    """-> (masked text, start, end) of the brace block that follows the first match of head_re."""
    ms = CD._mask(text)
    m = re.search(head_re, ms, re.M)
    if not m:
        raise TexDiffError(f'{path}: no line matches {head_re!r}')
    ob = ms.index('{', m.end() - 1 if ms[m.end() - 1] == '{' else m.end())
    return ms, ob, CD._match(ms, ob, '{', '}')


def parse_roles(char_text, cc_text):
    """-> {'dry': {slot: (role, line)}, 'dry_default': (role, line), 'wet': {...}, 'change': {...}, 'cube': {slot: line}}."""
    ms, a, b = _body(char_text, r'^CharSlot RoleOfSlot\(int slot\)\s*\{', CHAR)
    sw = re.fullmatch(r'\s*switch\s*\(\s*slot\s*\)\s*\{(.*)\}\s*', ms[a + 1:b - 1], re.S)
    if not sw or re.sub(r'(?:(?:case\s+\d+|default)\s*:\s*)+return\s+CharSlot::\w+\s*;', '', sw.group(1)).strip():
        raise TexDiffError(f'{CHAR}:{line_of(char_text, a)}: RoleOfSlot is no longer one `switch (slot)` of `case N: return CharSlot::X;` '
                           'arms (S111 review: another statement, such as an early return, would leave a slot\'s role unread)')
    dry = {}
    for m in re.finditer(r'((?:case\s+\d+\s*:\s*)+)return\s+CharSlot::(\w+)\s*;', ms[a:b]):
        for s in re.findall(r'case\s+(\d+)', m.group(1)):
            dry[int(s)] = (m.group(2), line_of(char_text, a + m.start()))
    md = re.search(r'default\s*:\s*return\s+CharSlot::(\w+)\s*;', ms[a:b])
    if not md:
        raise TexDiffError(f'{CHAR}: RoleOfSlot has no `default: return CharSlot::X;`')
    out = {'dry': dry, 'dry_default': (md.group(1), line_of(char_text, a + md.start()))}
    ms, a, b = _body(char_text, r'^CharSlot RoleOfWetSlot\(int slot\)\s*\{', CHAR)
    out['wet'] = {int(m.group(1)): (m.group(2), line_of(char_text, a + m.start()))
                  for m in re.finditer(r'if\s*\(\s*slot\s*==\s*(\d+)\s*\)\s*return\s+CharSlot::(\w+)\s*;', ms[a:b])}
    rest = re.sub(r'if\s*\(\s*slot\s*==\s*\d+\s*\)\s*return\s+CharSlot::\w+\s*;', '', ms[a + 1:b - 1])
    if ' '.join(rest.split()) != 'return RoleOfSlot(slot);':
        raise TexDiffError(f'{CHAR}:{line_of(char_text, a)}: RoleOfWetSlot is no longer `if (slot == N) return CharSlot::X;` lines then '
                           '`return RoleOfSlot(slot);`')
    ms, a, b = _body(char_text, r'^CharSlot RoleOfChangeSlot\(int slot\)\s*\{', CHAR)
    chg_line = line_of(char_text, a)
    if not re.fullmatch(r'const int k = (?:[\w:]+::)?MaskIndexOfSlot\(slot\); if \(k >= 0\) return CharSlot\(int\(CharSlot::ChangeMask1\) \+ k\); '
                        r'return RoleOfSlot\(slot\);', ' '.join(ms[a + 1:b - 1].split())):
        raise TexDiffError(f'{CHAR}:{chg_line}: RoleOfChangeSlot no longer maps MaskIndexOfSlot(slot) to ChangeMask1 + k, else RoleOfSlot(slot)')
    mi = re.search(r'MaskIndexOfSlot\(int slot\)\s*\{([^}]*)\}', CD._mask(cc_text))
    if not mi:
        raise TexDiffError(f'{CC}: no MaskIndexOfSlot body')
    if ' '.join(re.sub(r'slot\s*==\s*\d+\s*\?\s*\d+\s*:', '', mi.group(1)).split()) != 'return -1;':
        raise TexDiffError(f'{CC}: MaskIndexOfSlot is no longer `return slot == N ? K : ... : -1;`')
    out['change'] = {int(s): (f'ChangeMask{int(k) + 1}', chg_line) for s, k in re.findall(r'slot\s*==\s*(\d+)\s*\?\s*(\d+)', mi.group(1))}
    ms, a, b = _body(char_text, r'^PsBind CheckPs\(', CHAR)
    out['cube'] = {int(m.group(1)): line_of(char_text, a + m.start())
                   for m in re.finditer(r'if\s*\(\s*slot\s*==\s*(\d+)\s*\)\s*\{\s*cube\s*=\s*true', ms[a:b])}
    mt = re.search(r'CubeSibling\(f\)', CD._mask(char_text))
    out['twin_line'] = line_of(char_text, mt.start()) if mt else None
    return out


def classify(body, helpers=None):
    """-> {'fields', 'standins', 'maps'} read from one binding's text (a case body / a chain arm). helpers = {'ns::Fn': [fields]}: a
    helper call in the body binds those material fields (muscle_map::Image -> mat.muscle, parse_muscle)."""
    b = re.sub(r'\s+', ' ', body)
    fields = re.findall(r'\btex\(\s*mat\.(\w+(?:\[[^\]]*\])?)\s*\)', b)
    for h, fs in (helpers or {}).items():
        if re.search(r'\b' + re.escape(h) + r'\s*\(', b):
            fields += fs
    fields += re.findall(r'\bcube_of\(\s*mat\.(\w+)\s*\)', b)
    fields += re.findall(r'=\s*mat\.(\w+)\s*;', b)
    if 'MaskTextureOfSlot(' in b:
        fields.append('change_mask')
    maps = []
    for m in re.finditer(r'cube_slot\[slotn\]\s*=\s*([^;,]+)', b):
        maps += [int(k) for k in re.findall(r'\b(\d+)\b', m.group(1))]
    if re.search(r'\bliquid_slot\s*=', b):
        maps.append('liquid')
    if re.search(r'\blm_image\b', b):
        maps.append('lm')
    st = []
    rest = b
    for s in STANDINS:
        if re.search(r'(?<!\w)' + re.escape(s) + r'(?!\w)', rest):
            st.append(s)
            rest = re.sub(r'(?<!\w)' + re.escape(s) + r'(?!\w)', ' ', rest)
    return {'fields': list(dict.fromkeys(fields)), 'standins': st, 'maps': maps}


SWITCH_HEADS = (r'switch\s*\(\s*b->change_colour\s*\?\s*RoleOfChangeSlot\(slotn\)\s*:\s*wet\s*\?\s*RoleOfWetSlot\(slotn\)\s*:\s*RoleOfSlot\(slotn\)\s*\)\s*\{',
                r'switch\s*\(\s*muscle_map::Role\(\s*slotn\s*,\s*b->change_colour\s*,\s*wet\s*,\s*reads_muscle\s*\)\s*\)\s*\{')   # before / since S111 MUSCLEMAP


def switch_head(front_text):
    """-> 0 (the plain role chain) or 1 (S111 MUSCLEMAP: muscle_map::Role in front of it); any other head of the character switch is a TexDiffError."""
    ms = CD._mask(front_text)
    hits = [i for i, h in enumerate(SWITCH_HEADS) if re.search(h, ms)]
    if len(hits) != 1:
        raise TexDiffError(f'{FRONT}: the character switch is neither `switch (b->change_colour ? RoleOfChangeSlot(slotn) : wet ? '
                           'RoleOfWetSlot(slotn) : RoleOfSlot(slotn))` nor `switch (muscle_map::Role(slotn, b->change_colour, wet, reads_muscle))`'
                           + (' (both found)' if hits else ''))
    return hits[0]


def parse_muscle(front_text, muscle_text):
    """S111 MUSCLEMAP -> {'slot', 'role', 'define', 'line', 'fields'} from muscle_map.h: Role puts slot kSlot of a fragment that defines
    the ReadsMuscle word, on a bind that is not change-colour, on the role it returns, before the plain chain; Image binds tex(mat.F) else
    black. Every body must keep exactly that shape (else TexDiffError), and the front end's `reads_muscle` must come from ReadsMuscle."""
    ms = CD._mask(muscle_text)
    mk = re.search(r'\bkSlot\s*=\s*(\d+)\s*;', ms)
    mr = re.search(r'bool ReadsMuscle\(const char\* defines\)\s*\{\s*return HasDefine\(defines,\s*"(\w+)"\);\s*\}', ms)
    _m, a, b = _body(muscle_text, r'CharSlot Role\(int slot, bool change_colour, bool wet, bool muscle\)\s*\{', MUSCLE)
    role = re.fullmatch(r'if \(muscle && !change_colour && slot == kSlot\) return CharSlot::(\w+); return change_colour \? RoleOfChangeSlot\(slot\) : '
                        r'wet \? RoleOfWetSlot\(slot\) : RoleOfSlot\(slot\);', ' '.join(ms[a + 1:b - 1].split()))
    _m, c, d = _body(muscle_text, r'ImageT Image\(const assets::Material& mat, const TexFn& tex, ImageT none, ImageT black\)\s*\{', MUSCLE)
    img = re.fullmatch(r'const ImageT t = tex\(mat\.(\w+)\); return t != none \? t : black;', ' '.join(ms[c + 1:d - 1].split()))
    fe = re.search(r'\breads_muscle\s*=\s*[^;]*muscle_map::ReadsMuscle\(', CD._mask(front_text))
    if not (mk and mr and role and img and fe):
        raise TexDiffError(f'{MUSCLE}: muscle_map is no longer `kSlot = N`, ReadsMuscle = HasDefine(defines, "X"), Role = `if (muscle && '
                           '!change_colour && slot == kSlot) return CharSlot::R;` then the plain chain, Image = tex(mat.F) else black, with the '
                           f'front end\'s reads_muscle from ReadsMuscle ({FRONT})')
    return {'slot': int(mk.group(1)), 'role': role.group(1), 'define': mr.group(1), 'line': line_of(muscle_text, a), 'fields': [img.group(1)]}


def parse_char_switch(front_text, helpers=None):
    """-> {role: {'line', 'cls'}} from the character bind's role switch (SWITCH_HEADS)."""
    ms, a, b = _body(front_text, SWITCH_HEADS[switch_head(front_text)], FRONT)
    sw_line = line_of(front_text, a)
    labels = list(re.finditer(r'case\s+CharSlot::(\w+)\s*:', ms[a:b]))
    out, pending = {}, []
    for i, m in enumerate(labels):
        end = labels[i + 1].start() if i + 1 < len(labels) else (b - a - 1)
        body = ms[a + m.end():a + end]
        pending.append((m.group(1), line_of(front_text, a + m.start())))
        if not body.strip():
            continue
        cls = classify(body, helpers)
        for role, ln in pending:
            out[role] = {'line': ln, 'cls': cls}
        pending = []
    return out, sw_line


def _arm_end(s, i):
    while s[i].isspace():
        i += 1
    if s[i] == '{':
        return CD._match(s, i, '{', '}')
    d, j = 0, i
    while j < len(s):
        if s[j] in '({[':
            d += 1
        elif s[j] in ')}]':
            d -= 1
        elif s[j] == ';' and d == 0:
            return j + 1
        j += 1
    return j


def parse_chains(front_text):
    """-> [{'fn', 'line', 'arms': [{'slots', 'extra', 'line', 'cls'}], 'default_line'}] for every `rdr::ImageId image = white_;` followed by
    an `if (slotn ...` chain (the plain binds), and [{'fn', 'line', 'only', 'cls'}] for the vertex loops over `vv.sampler_count`."""
    ms = CD._mask(front_text)
    fns = [(m.start(), m.group(1)) for m in re.finditer(r'^[^\s#/][^\n;]*\bFrontEnd::(\w+)\([^\n]*\{\s*$', ms, re.M)]
    fn_at = lambda pos: max((f for f in fns if f[0] <= pos), default=(0, '?'))[1]
    chains = []
    for m in re.finditer(r'rdr::ImageId\s+image\s*=\s*white_\s*;\s*', ms):
        p = m.end()
        if not re.match(r'if\s*\(\s*slotn\b', ms[p:]):
            continue
        arms = []
        while True:
            mi = re.match(r'(else\s+)?if\s*\(', ms[p:])
            if not mi:
                break
            cp = p + mi.end() - 1
            ce = CD._match(ms, cp, '(', ')')
            cond = ms[cp + 1:ce - 1]
            ae = _arm_end(ms, ce)
            slots = [int(x) for x in re.findall(r'slotn\s*==\s*(\d+)', cond)]
            mr = re.search(r'slotn\s*>=\s*(\d+)\s*&&\s*slotn\s*<=\s*(\d+)', cond)
            if mr:
                slots += list(range(int(mr.group(1)), int(mr.group(2)) + 1))
            extra = re.sub(r'slotn\s*(?:==|>=|<=)\s*\d+', '', cond)
            extra = re.sub(r'^[\s&|()]+|[\s&|()]+$', '', extra)
            arms.append({'slots': slots, 'extra': ' '.join(extra.split()), 'line': line_of(front_text, p), 'body': ms[ce:ae],
                         'cls': classify(ms[ce:ae]), 'ref': re.search(r'\[\s*slotn\s*-\s*(\d+)\s*\]', ms[ce:ae])})
            mn = re.match(r'\s*else\b', ms[ae:])
            if not mn or not re.match(r'\s*else\s+if\s*\(', ms[ae:]):
                break
            p = ae + re.match(r'\s*', ms[ae:]).end()
        chains.append({'fn': fn_at(m.start()), 'line': line_of(front_text, m.start()), 'arms': arms})
    verts = []
    for m in re.finditer(r'for\s*\(\s*uint32_t\s+s\s*=\s*0\s*;\s*s\s*<\s*vv\.sampler_count\s*;\s*\+\+s\s*\)\s*\{', ms):
        e = CD._match(ms, m.end() - 1, '{', '}')
        body = ms[m.end():e]
        if 'set.slots' in body:      # the draw's own loop (Record): binds what the bind stored, not a bind
            continue
        mo = re.search(r'if\s*\(\s*slotn\s*!=\s*(\d+)\s*\)\s*continue', body)
        verts.append({'fn': fn_at(m.start()), 'line': line_of(front_text, m.start()), 'only': int(mo.group(1)) if mo else None,
                      'cls': classify(body)})
    return chains, verts


def parse_draw(front_text):
    """-> ({K: (source, conds, line)}, liquid {'prefixes', 'line', 'replace_line'}) from the draw (Record)."""
    ms = CD._mask(front_text)
    m = re.search(r'set\.slots\[slot\]\.image\s*=\s*b\.cube_slot\[slot\]\s*==\s*1[^;]*;', ms)
    if not m:
        raise TexDiffError(f'{FRONT}: no draw-time line `set.slots[slot].image = b.cube_slot[slot] == 1 ? ...`')
    ln = line_of(front_text, m.start())
    rep = {}
    for k in re.finditer(r'b\.cube_slot\[slot\]\s*==\s*(\d+)((?:\s*&&\s*[\w.>\-]+)*)\s*\)?\s*\?\s*\(?\s*(\w+)', m.group(0)):
        rep[int(k.group(1))] = (k.group(3), [c.strip() for c in k.group(2).split('&&') if c.strip()], ln)
    ml = re.search(r'^[^\n]*\bliquid_of\([^\n]*$', ms, re.M)
    if not ml:
        raise TexDiffError(f'{FRONT}: no `liquid_of(` line (the liquid map\'s effect gate)')
    mr = re.search(r'slot\s*==\s*int\(b\.liquid_slot\)\s*\)\s*set\.slots\[slot\]\.image\s*=\s*liquid_image', ms)
    liquid = {'prefixes': re.findall(r'rfind\("(\w+)",\s*0\)\s*==\s*0', ml.group(0)), 'line': line_of(front_text, ml.start()),
              'replace_line': line_of(front_text, mr.start()) if mr else None}
    return rep, liquid


def parse_jboy(jboy_text):
    """-> {field: [sampler names]} from the loader's `pname == "X" ... slot = &mat.F` lines (the old inline chain) and the shared table's
    `is("X") ... return &mat.F` lines (src/assets/material_samplers.h SlotOf, S111 UNIFY2)."""
    out = {}
    for ln in CD._mask(jboy_text).split('\n'):
        mf = re.search(r'(?:slot\s*=|\breturn)\s*&mat\.(\w+(?:\[\d+\])?)', ln)
        if mf:
            out.setdefault(mf.group(1), [])
            out[mf.group(1)] += [n for n in re.findall(r'(?:pname\s*==|\bis\()\s*"(\w+)"', ln) if n not in out[mf.group(1)]]
    return out


def default_reader(ref=None):
    def read(p):
        if ref:
            return subprocess.run(['git', '-C', REPO, 'show', f'{ref}:{p}'], capture_output=True, text=True, encoding='utf-8', check=True).stdout
        with open(os.path.join(REPO, p), encoding='utf-8') as fh:
            return fh.read()
    return read


# a src line that WRITES a Material field F: an assignment to `mat.F` / `mat.F[..]`, the ternary lvalue `(c ? mat.F : mat.G) = v`, or
# its address taken (`&mat.F`, the loader's `slot = &mat.F`), on a receiver our code names a Material by (`mat`, `mt`, `material`,
# `materials[i]`: a field of the same name on another struct, such as CharConstants' `cc.muscle`, is not a fill). Text, comments cut
# [L: a write through another name is not seen, and reads as never filled: a false DIFF, never a false OK].
RECV = r'(?:\bmat|\bmt|\bmaterial|\bmaterials\s*\[[^\]]*\])\s*\.\s*'
WRITE_RES = (RECV + r'{f}\b(?:\s*\[[^\]]*\])?\s*=(?!=)', RECV + r'{f}\b[^;=]*\)\s*=(?!=)', r'&\s*' + RECV + r'{f}\b')


def default_grep(ref=None):
    """-> grep(fields) -> {field: ['path:line', ...]}: the src lines that write each Material field (WRITE_RES), by `git grep` over src
    at ref (else the working tree, untracked files too)."""
    def grep(fields):
        out = {f: [] for f in fields}
        if not fields:
            return out
        cmd = ['git', '-C', REPO, 'grep', '-n', '-w']
        for f in fields:
            cmd += ['-e', f]
        cmd += ([ref] if ref else ['--untracked']) + ['--', 'src']
        r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8')
        if r.returncode not in (0, 1):
            raise TexDiffError(f'git grep over src failed: {r.stderr.strip()}')
        for ln in r.stdout.splitlines():
            parts = ln.split(':', 3) if ref else ln.split(':', 2)
            if len(parts) < (4 if ref else 3):
                continue
            path, num, text = parts[-3], parts[-2], parts[-1].split('//')[0]
            for f in fields:
                if any(re.search(p.format(f=re.escape(f)), text) for p in WRITE_RES):
                    out[f].append(f'{path}:{num}')
        return out
    return grep


def load_ours(read, grep=None):
    texts = {p: read(p) for p in (CHAR, FRONT, CC, JBOY)}
    try:
        texts[SAMPLERS] = read(SAMPLERS)   # S111 UNIFY2
    except (OSError, subprocess.CalledProcessError):
        texts[SAMPLERS] = ''
    if switch_head(texts[FRONT]) == 1:
        texts[MUSCLE] = read(MUSCLE)
    problems = [f'{p}: holds a merge-conflict marker (run with --ref HEAD)' for p, t in texts.items() if re.search(r'^<<<<<<< ', t, re.M)]
    roles = parse_roles(texts[CHAR], texts[CC])
    roles['muscle'] = parse_muscle(texts[FRONT], texts[MUSCLE]) if MUSCLE in texts else None
    mu = roles['muscle']
    sw, sw_line = parse_char_switch(texts[FRONT], {'muscle_map::Image': mu['fields']} if mu else None)
    if mu and mu['role'] not in sw:
        problems.append(f"{MUSCLE}:{mu['line']}: role {mu['role']} has no case in the character switch ({FRONT}:{sw_line})")
    chains, verts = parse_chains(texts[FRONT])
    rep, liquid = parse_draw(texts[FRONT])
    names = parse_jboy(texts[JBOY] + '\n' + texts[SAMPLERS])
    for ctx in ('dry', 'wet', 'change'):
        for s, (role, ln) in roles[ctx].items():
            if role not in sw:
                problems.append(f'{CHAR}:{ln}: role {role} has no case in the character switch ({FRONT}:{sw_line})')
    if roles['dry_default'][0] not in sw:
        problems.append(f'{CHAR}: the default role {roles["dry_default"][0]} has no case in the character switch')
    if not chains:
        problems.append(f'{FRONT}: no plain bind chain (`rdr::ImageId image = white_;` + `if (slotn == N) ...`) found')
    # S111 review: the material fields ours binds that no sampler name loads: which src lines fill them (none = always empty)
    used = [c['cls'] for c in sw.values()] + [a_['cls'] for ch in chains for a_ in ch['arms']] + [v['cls'] for v in verts]
    loaded = {re.sub(r'\[.*', '', k) for k in names}
    bases = sorted({re.sub(r'\[.*', '', f) for c in used for f in c['fields']} - loaded)
    filled = (grep or default_grep())(bases)
    return {'roles': roles, 'switch': sw, 'switch_line': sw_line, 'chains': chains, 'verts': verts, 'replace': rep, 'liquid': liquid,
            'names': names, 'filled': filled}, problems


# ------------------------------------------------------------------ ours: one slot's binding
def resolve_cls(cls, chr_, effect, ours, slot=None, ref=None):
    """-> {'kind': MATERIAL / MAP / STANDIN, 'fields', 'standins', 'maps' (named)} after the draw-time replacement."""
    maps = []
    for k in cls['maps']:
        if k == 'liquid':
            lq = ours['liquid']
            if lq['replace_line'] and any(effect.startswith(p) for p in lq['prefixes']):
                maps.append(MAP_NAMES['liquid'])
            continue
        if k == 'lm':
            maps.append(MAP_NAMES['lm'])
            continue
        r = ours['replace'].get(k)
        if r and all(c != 'b.chr' or chr_ for c in r[1]):
            maps.append(MAP_NAMES.get(r[0], r[0]))
    fields = []
    for f in cls['fields']:
        if ref is not None and slot is not None:
            f = re.sub(r'\[\s*slotn\s*-\s*(\d+)\s*\]', lambda m: f'[{slot - int(m.group(1))}]', f)
        fields.append(f)
    kind = 'MAP' if maps else 'MATERIAL' if fields else 'STANDIN'
    return {'kind': kind, 'fields': fields, 'standins': cls['standins'], 'maps': maps}


def ctx_of(readers):
    """-> {ctx: [variants]}: change = FX_CHANGE_COLOR, wet = reads slot 14, else dry (the role function the bind takes)."""
    out = {}
    for name, defs, slots in readers:
        c = 'change' if 'FX_CHANGE_COLOR' in defs else 'wet' if 14 in slots else 'dry'
        out.setdefault(c, []).append(name)
    return out


def ours_char(effect, slot, readers, ours):
    roles = ours['roles']
    if slot in roles['cube'] and all('FX_CUBE_SPECULAR' in d for _n, d, _s in readers):
        return [{'ctx': 'all', 'kind': 'TWIN', 'role': None, 'at': [f"{CHAR}:{roles['cube'][slot]}"] + ([f"{CHAR}:{roles['twin_line']}"] if roles['twin_line'] else []),
                 'variants': len(readers), 'fields': [], 'standins': [], 'maps': []}]
    out = []
    mu = roles.get('muscle')
    groups = {}
    for ctx, vs in ctx_of(readers).items():
        for name, defs, _s in readers:
            if name in vs:   # S111 MUSCLEMAP: muscle_map::Role takes slot kSlot of an FX_MUSCLE fragment of a non-change-colour bind first
                m = bool(mu) and ctx != 'change' and slot == mu['slot'] and mu['define'] in defs
                groups.setdefault((ctx, m), []).append(name)
    for (ctx0, m), vs in sorted(groups.items()):
        ctx = ctx0 + ('+muscle' if m else '')
        at = []
        if m:
            role, ln, where = mu['role'], mu['line'], MUSCLE
        elif ctx0 in ('wet', 'change') and slot in roles[ctx0]:
            (role, ln), where = roles[ctx0][slot], CHAR
        elif slot in roles['dry']:
            (role, ln), where = roles['dry'][slot], CHAR
        else:
            (role, ln), where = roles['dry_default'], CHAR
        at.append(f'{where}:{ln}')
        case = ours['switch'].get(role)
        if case is None:
            out.append({'ctx': ctx, 'kind': 'STANDIN', 'role': role, 'at': at, 'variants': len(vs), 'fields': [], 'standins': ['white_'], 'maps': []})
            continue
        at.append(f"{FRONT}:{case['line']}")
        r = resolve_cls(case['cls'], True, effect, ours)
        if 'liquid' in case['cls']['maps']:
            at.append(f"{FRONT}:{ours['liquid']['line']}")
        out.append(dict(r, ctx=ctx, role=role, at=at, variants=len(vs)))
    return out


def ours_front(effect, stage, slot, readers, ours):
    out = []
    if stage == 'vs':
        for v in ours['verts']:
            if v['only'] is not None and v['only'] != slot:
                out.append({'ctx': v['fn'], 'kind': 'STANDIN', 'role': None, 'at': [f"{FRONT}:{v['line']}"], 'variants': len(readers),
                            'fields': [], 'standins': ['white_'], 'maps': [], 'cond': []})
                continue
            out.append(dict(resolve_cls(v['cls'], False, effect, ours), ctx=v['fn'], role=None, at=[f"{FRONT}:{v['line']}"], variants=len(readers), cond=[]))
        return out
    for ch in ours['chains']:
        arms = [a for a in ch['arms'] if slot in a['slots']]
        main = [a for a in arms if not a['extra']]
        cond = [a for a in arms if a['extra']]
        if main:
            a = main[0]
            r = resolve_cls(a['cls'], False, effect, ours, slot, a['ref'])
            at = [f"{FRONT}:{a['line']}"]
        else:
            r = {'kind': 'STANDIN', 'fields': [], 'standins': ['white_'], 'maps': [], 'default': True}
            at = [f"{FRONT}:{ch['line']} (no arm: the white default)"]
        cs = []
        for a in cond:
            rc = resolve_cls(a['cls'], False, effect, ours, slot, a['ref'])
            cs.append({'when': a['extra'], 'at': f"{FRONT}:{a['line']}", 'kind': rc['kind'], 'fields': rc['fields'], 'maps': rc['maps'], 'standins': rc['standins']})
        out.append(dict(r, ctx=ch['fn'], role=None, at=at, variants=len(readers), cond=cs))
    return out


# ------------------------------------------------------------------ game
def sampler_rows(table, effect):
    """the sampler rows (bank 7 / 8) the game can bind for this effect: the common table + its own sub-table. The group pick is
    qconstdiff.group_rows' (copied: that function keeps the float banks only)."""
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
    return [r for b in bases for r in table['rows'] if r['base'] == b and r['type'] in SAMPLER_TYPES]


def sampler_names(path=None, source='game'):
    """-> ({effect: {name: {'n', 'tex': {texture: n}, 'tech': {technique word: {texture: n}}}}}, {effect: records},
    {effect: {technique word: records}}) from the asset index's `samplers` / `technique_word` columns; (None, None, None) without it.
    source (S111 MODINDEX, tools/q/qasset_mods.py where): 'game' = the game's own files (the default: the mods folder's rows left out), 'mods', 'mod:NAME' or 'all'."""
    if path is None:
        import qdecomp
        c = sorted(glob.glob(os.path.join(os.path.dirname(qdecomp.IMAGE), 'asset.s*.db')))
        path = c[-1] if c else None
    if not path or not os.path.exists(path):
        return None, None, None
    import qasset_mods
    sw, sa = qasset_mods.where(source)
    con = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
    names, totals, techs = {}, {}, {}
    try:
        for eff, tw, samp, n in con.execute('SELECT material.effect, material.technique_word, material.samplers, COUNT(*) FROM material JOIN entry ON material.entry_id = entry.id '
                                            'JOIN pach ON entry.pach_id = pach.id JOIN pacs ON pach.pac_id = pacs.id WHERE ' + sw +
                                            ' GROUP BY material.effect, material.technique_word, material.samplers', sa):
            totals[eff] = totals.get(eff, 0) + n
            tt = techs.setdefault(eff, {})
            tt[tw] = tt.get(tw, 0) + n
            try:
                ss = json.loads(samp or '{}')
            except ValueError:
                continue
            d = names.setdefault(eff, {})
            for k, t in (ss.items() if isinstance(ss, dict) else []):
                e = d.setdefault(k, {'n': 0, 'tex': {}, 'tech': {}})
                e['n'] += n
                e['tex'][t] = e['tex'].get(t, 0) + n
                et = e['tech'].setdefault(tw, {})
                et[t] = et.get(t, 0) + n
    finally:
        con.close()
    return names, totals, techs


def records_of(effect, names, totals):
    """-> (records {name: {...}}, total, why, own) the records that stand for an fx effect; records None when no record names it
    (S111 review: the old fallback to every record of the effect's group made effects no material uses look game-fed)."""
    if totals.get(effect):
        return names.get(effect, {}), totals[effect], f'{effect} records', True
    base = effect[:-2] if effect.endswith('LM') else effect
    alike = sorted(e for e in totals if base != effect and e.startswith(base))   # yBG_DefaultLM -> yBG_Default, yProp_UVScrolLM -> yProp_UVScroll
    if not alike:
        return None, 0, f'no material record of the player\'s files names {effect} (asset index, {sum(totals.values())} records)', False
    how = f'no {effect} record: the records of {", ".join(alike)}, their techniques not checked'
    merged = {}
    for e in alike:
        for k, v in names.get(e, {}).items():
            m = merged.setdefault(k, {'n': 0, 'tex': {}, 'tech': {}})
            m['n'] += v['n']
            for t, c in v['tex'].items():
                m['tex'][t] = m['tex'].get(t, 0) + c
    return merged, sum(totals[e] for e in alike), how, False


def technique_reads(fx_dir=None, flags=None, tables=None):
    """-> ({effect lower-case: {technique: frozenset(pixel sampler slots) or None}}, None), or (None, why) without the player's effect
    files. A technique's slots = the slots our fx registry gives the pixel shader of each of its passes, every file's table merged (a
    slot counts when ANY file's shader at any pass reads it); a shader the registry does not name makes the technique None (cannot
    tell: its records count as sampling every slot). flags = qconstdiff.load_pass_flags (S111 CONSTPASS): a pass the model draw never
    picks for the effect (not in qconstdiff.reachable_passes) is skipped; None = every pass counts. tables = effect_table.load_all()'s
    result when the caller has it (the selftest reads the files once)."""
    try:
        res = tables
        if res is None:
            sys.path.insert(0, os.path.join(REPO, 'tools', 'shaders'))
            import effect_table
            res = effect_table.load_all()
    except Exception as e:  # noqa: BLE001 - the player's files are an extra source; their absence is said, the filter is then off
        return None, f'{type(e).__name__}: {e}'
    slots = {}
    for p in sorted(glob.glob(os.path.join(fx_dir or FX_DIR, '*.fx.json'))):
        with open(p, encoding='utf-8') as fh:
            d = json.load(fh)
        eff = d.get('effect') or os.path.basename(p).split('.')[0]
        for v in d.get('fragment', []):
            slots[f"{eff}_{v.get('name')}_fragment"] = frozenset(int(s['slot']) for s in v.get('samplers', []))
    out = {}
    for e in res.values():
        d = out.setdefault(e['effect'].lower(), {})
        reach = CD.reachable_passes(e['effect'], flags) if flags else None
        for t in e['techniques']:
            cur = d.get(t['index'], frozenset())
            for p in t['passes']:
                if reach is not None and p['pass'] not in reach:
                    continue
                nm = (p['ps'] or {}).get('name')
                if cur is None or nm not in slots:
                    cur = None
                    break
                cur = cur | slots[nm]
            d[t['index']] = cur
    return (out, None) if out else (None, 'no effect table read from the player\'s files')


def code_refs(con, name):
    """functions other than the name table's filler that name this sampler string (q index strrefs)."""
    out = []
    for (a,) in con.execute('SELECT addr FROM strings WHERE text = ?', (name,)).fetchall():
        for f, site in con.execute('SELECT func, site FROM strrefs WHERE addr = ?', (a,)).fetchall():
            if f != CD.NAME_TABLE_FN and (f, site) not in out:
                out.append((f, site))
    return out


def game_slot(effect, stage, slot, table, names, totals, refs_of, techs=None, treads=None):
    """-> {'kind', 'rows': [...]} for one sampler slot of one effect. techs / treads (sampler_names / technique_reads): only the records
    whose own technique samples the slot count (S111 review); None = every record counts."""
    if stage != 'ps':
        return {'kind': 'NOROW', 'rows': [], 'why': 'a vertex sampler: the name table holds pixel sampler rows only'}
    rows = []
    rank = {'GAME-MATERIAL': 3, 'GAME-CODE': 2, 'GAME-NOREC': 1}
    best = None
    if names is None:
        recs, total, how, own = {}, 0, 'no asset index', False
    else:
        recs, total, how, own = records_of(effect, names, totals)
        if recs is None:
            return {'kind': 'NOUSE', 'rows': [], 'why': how + ': no material is drawn with this effect'}
    tr = (treads or {}).get(effect.lower()) if own else None
    sampled = None   # the technique words whose passes read the slot (a word the table lacks, or a shader we cannot name: counted)
    if tr is not None and (techs or {}).get(effect):
        sampled = {tw for tw in techs[effect] if not isinstance(tw, int) or tr.get(tw) is None or slot in tr[tw]}
        total = sum(techs[effect][tw] for tw in sampled)
        how += f', {total} of {totals[effect]} with a technique that samples s{slot}'
    for r in sampler_rows(table, effect):
        if r['reg'] != slot:
            continue
        nm = r['name'] or '?'
        rec = recs.get(nm)
        code = refs_of(nm)
        kinds = []
        tex, never = (rec['tex'] if rec else {}), 0
        if rec and sampled is not None:
            tex = {}
            for tw, tx in rec['tech'].items():
                for t, c in tx.items():
                    if tw in sampled:
                        tex[t] = tex.get(t, 0) + c
                    elif t not in EMPTY_TEX:
                        never += c
        real = sum(c for t, c in tex.items() if t not in EMPTY_TEX)   # a record naming the sampler with an empty texture binds no picture
        empty = sum(c for t, c in tex.items() if t in EMPTY_TEX)
        unseen = f'; {never} more name a texture under a technique that never samples s{slot}' if never else ''
        if real:
            top = sorted(((t, c) for t, c in tex.items() if t not in EMPTY_TEX), key=lambda kv: (-kv[1], kv[0]))[:3]
            kinds.append(('GAME-MATERIAL', f"{real} of {total} records ({how}" + (f'; {empty} more name it with an empty texture' if empty else '')
                          + unseen + '); textures ' + ', '.join(f'{t} x{c}' for t, c in top)))
        if code:
            kinds.append(('GAME-CODE', 'named by ' + ', '.join(f'sub_{f:08X} @{CD.hx(s)}' for f, s in code[:4])))
        if not kinds:
            kinds.append(('GAME-NOREC', (f'{empty} records name it with an empty texture only' if empty else 'no record names it')
                          + f' ({how}, {total} records{unseen}) and no code names the string: the material path binds no picture there'))
        k = max((x[0] for x in kinds), key=lambda x: rank[x])
        rows.append({'name': nm, 'row': CD.hx(r['addr']), 'site': CD.hx(r['site']), 'type': r['type'], 'kind': k, 'kinds': kinds})
        if best is None or rank[k] > rank[best]:
            best = k
    if sampled is not None and not total:
        return {'kind': 'UNSAMPLED', 'rows': rows, 'why': f"no record's technique samples s{slot}: the {effect} records' techniques "
                + ', '.join(f'{tw} (x{n})' for tw, n in sorted(techs[effect].items(), key=lambda kv: str(kv[0])))
                + ' have no pass the model draw picks whose pixel shader reads it (the player\'s effect files)'}
    if not rows:
        return {'kind': 'NOROW', 'rows': [], 'why': 'no sampler row at this slot in the effect\'s tables: bound by code by slot number, or not at all'}
    return {'kind': best, 'rows': rows}


# ------------------------------------------------------------------ the join
def judge(b, g, names, filled=None):
    """-> (flag, why) for one of ours' bindings against the game's slot. filled = {field: [src lines that write it]} (load_ours):
    a material field no sampler name loads and no line writes is always empty, so the binding is its stand-in."""
    if b['kind'] == 'TWIN':
        return 'HABIT', 'ours binds the non-cube twin (the slot is never read)'
    if g['kind'] in ('NOUSE', 'UNSAMPLED'):
        return 'UNUSED', g['why']
    fed = g['kind'] in ('GAME-MATERIAL', 'GAME-CODE')
    gnames = [r['name'] for r in g['rows'] if r['kind'] in ('GAME-MATERIAL', 'GAME-CODE')]
    dead = [f for f in b.get('fields') or [] if f not in names and not (filled or {}).get(re.sub(r'\[.*', '', f))]
    if b['kind'] == 'MATERIAL' and filled is not None and dead and len(dead) == len(b['fields']):
        b = dict(b, kind='STANDIN', standins=b['standins'], default=not b['standins'])
    if b['kind'] == 'STANDIN':
        what = 'nothing (the white default)' if b.get('default') else 'a stand-in (' + ', '.join(b['standins'] or ['?']) + ')'
        if b.get('fields'):
            what = f"mat.{'/'.join(b['fields'])}, which no sampler name loads and no src line writes: always empty, so {what}"
        if fed:
            return 'DIFF', f'ours binds {what}'
        return 'UNSETTLED', f'ours binds {what}; the game\'s source is {g["kind"]}'
    if b['kind'] == 'MAP':
        if g['kind'] == 'GAME-MATERIAL':
            return 'MAPREC', 'ours binds its map where material records name a texture'
        return 'OK', ''
    our = sorted({n for f in b['fields'] for n in names.get(f, [])})
    if fed and our and not any(n in our for n in gnames):
        return 'DIFF', f"ROLE: ours binds mat.{'/'.join(b['fields'])} (loaded from {', '.join(our)}), the game's name is {', '.join(gnames)}"
    if fed and not our and all(f not in names for f in b['fields']):
        by = sorted({x for f in b['fields'] for x in (filled or {}).get(re.sub(r'\[.*', '', f), [])})
        return 'OK', f"ours binds mat.{'/'.join(b['fields'])} (not loaded by a sampler name: filled by code" + (f" at {', '.join(by[:3])}" if by else '') + ')'
    return 'OK', ''


def build_rows(samplers, ours, table, names, totals, refs_of, techs=None, treads=None):
    out, skipped = [], {}
    for (eff, st, slot), s in sorted(samplers.items()):
        rk = CD.resolver_of(eff)
        if rk is None:
            skipped[eff] = skipped.get(eff, 0) + 1
            continue
        g = game_slot(eff, st, slot, table, names, totals, refs_of, techs, treads)
        binds = ours_char(eff, slot, s['readers'], ours) if rk == 'char' else ours_front(eff, st, slot, s['readers'], ours)
        worst = 'OK'
        for b in binds:
            b['flag'], b['why'] = judge(b, g, ours['names'], ours.get('filled'))
            if FLAGS.index(b['flag']) < FLAGS.index(worst):
                worst = b['flag']
        out.append({'effect': eff, 'stage': st, 'slot': slot, 'kind': s['kind'], 'path': rk, 'flag': worst, 'game': g, 'ours': binds,
                    'variants': len(s['readers'])})
    return out, skipped


def fmt_game(g):
    if not g['rows']:
        return f"{g['kind']} ({g['why']})"
    return (f"({g['why']}) " if g.get('why') else '') + '; '.join(
        f"{r['name']} (row {r['row']} @{r['site']}): " + '; '.join(f'{k} {w}' for k, w in r['kinds']) for r in g['rows'])


def fmt_bind(b):
    what = (', '.join(b['maps']) if b['kind'] == 'MAP' else 'mat.' + '/'.join(b['fields']) + (f" (else {', '.join(b['standins'])})" if b['standins'] else '')
            if b['kind'] == 'MATERIAL' else 'never read (the twin is bound)' if b['kind'] == 'TWIN' else ', '.join(b['standins']))
    role = f" {b['role']}" if b.get('role') else ''
    s = f"{b['ctx']}{role} -> {b['kind']} {what}  [{', '.join(b['at'])}; {b['variants']} variants]"
    for c in b.get('cond') or []:
        cw = ', '.join(c['maps']) if c['kind'] == 'MAP' else 'mat.' + '/'.join(c['fields']) if c['kind'] == 'MATERIAL' else ', '.join(c['standins'])
        s += f"; when {c['when']}: {c['kind']} {cw} [{c['at']}]"
    return s


def distinct(rows, flag='DIFF'):
    """-> {(slot, game names, ours lines, why): [effects]}: each departure once."""
    out = {}
    for r in rows:
        for b in r['ours']:
            if b['flag'] != flag:
                continue
            key = (r['stage'], r['slot'], tuple(x['name'] for x in r['game']['rows']) or (r['game']['kind'],), tuple(b['at']), b['why'])
            out.setdefault(key, []).append(f"{r['effect']}:{b['ctx']}")
    return out


def run(con, a, read=None, fx_dir=None):
    ours, problems = load_ours(read or default_reader(a.ref))
    table, _desc = CD.load_game(con)
    if not any(r['type'] in SAMPLER_TYPES for r in table['rows']):
        problems.append(f'name table: no sampler rows read from sub_{CD.NAME_TABLE_FN:08X}')
    names, totals, techs = sampler_names(source=getattr(a, 'source', 'game'))
    if names is None:   # said, not a PROBLEM (as q constdiff): every game row then reads GAME-NOREC / GAME-CODE
        print('NOTE: no asset index (q asset builds it): GAME-MATERIAL cannot be told from GAME-NOREC', file=sys.stderr)
    flags, ferr = CD.load_pass_flags(con)
    if flags is None:   # said, not a PROBLEM: every pass of a technique then counts (more slots sampled: never a false OK)
        print(f"NOTE: the model draw's pass flags were not read ({'; '.join(ferr)}): no pass is cut", file=sys.stderr)
    treads, terr = technique_reads(fx_dir, flags)
    if treads is None:   # said, not a PROBLEM: every record then counts as sampling every slot (the filter is off)
        print(f'NOTE: the player\'s effect files were not read ({terr}): records are not narrowed to the techniques that sample a slot', file=sys.stderr)
    cache = {}
    refs_of = lambda n: cache.setdefault(n, code_refs(con, n))
    rows, skipped = build_rows(load_samplers(fx_dir), ours, table, names, totals, refs_of, techs, treads)
    sel = rows
    if a.effect:
        sel = [r for r in sel if fnmatch.fnmatchcase(r['effect'], a.effect)]
    if a.flag:
        sel = [r for r in sel if any(b['flag'] in a.flag for b in r['ours'])]
    if a.json:
        print(json.dumps({'rows': sel, 'problems': problems}, indent=1, default=str))
    else:
        cur = None
        for r in sel:
            if r['effect'] != cur:
                cur = r['effect']
                print(f"== {cur}  ({'the character switch' if r['path'] == 'char' else 'the plain bind chains'})")
            print(f"{r['flag']:9s} {r['stage']} s{r['slot']:<2d} ({r['kind']})  game {r['game']['kind']}: {fmt_game(r['game'])}")
            for b in r['ours']:
                print(f"            ours {b['flag']:9s} {fmt_bind(b)}" + (f"  -- {b['why']}" if b['why'] else ''))
        tot = {f: sum(1 for r in sel if r['flag'] == f) for f in FLAGS}
        print(f"totals: {len(sel)} rows (of {len(rows)}); " + ', '.join(f'{f} {tot[f]}' for f in FLAGS)
              + '; game ' + ', '.join(f'{k} {v}' for k, v in sorted({k: sum(1 for r in sel if r['game']['kind'] == k) for k in {r['game']['kind'] for r in sel}}.items())))
        for flag in ('DIFF', 'UNSETTLED'):
            d = distinct(sel, flag)
            print(f'distinct {flag}: {len(d)}')
            for (st, slot, gn, at, why), effs in sorted(d.items()):
                print(f"  {st} s{slot} game {'/'.join(gn)}  ours {', '.join(at)}  {why}  ({len(effs)}: {', '.join(effs[:6])}{' ...' if len(effs) > 6 else ''})")
        unrow = sorted({n for e, d in (names or {}).items() if CD.resolver_of(e) for n in d if not any(r['name'] == n for r in table['rows'])})
        print(f"sampler names the records carry that no name-table row holds (never bound by name): {', '.join(unrow) or 'none'}")
        print(f"sources: name table {sum(1 for r in table['rows'] if r['type'] in SAMPLER_TYPES)} sampler rows (sub_{CD.NAME_TABLE_FN:08X}); material records "
              f"{sum((totals or {}).values())} (asset index); technique filter {'on: ' + str(len(treads)) + ' effects of the player' + chr(39) + 's effect files' if treads else 'OFF (' + str(terr) + ')'}; "
              f"our chains {', '.join(c['fn'] + ':' + str(c['line']) for c in ours['chains'])}; the character switch {FRONT}:{ours['switch_line']}")
        print(f"not bound through our material binds (not compared): {len(skipped)} effects, {sum(skipped.values())} slots")
        print(f"passes: {'only those the model draw can pick (qconstdiff.reachable_passes, the pass flags of sub_' + format(CD.PASS_FLAGS_FN, '08X') + ')' if flags else 'every pass of a technique (the pass flags were not read)'}"
              + (f"; the character switch goes through {MUSCLE}:{ours['roles']['muscle']['line']} (slot {ours['roles']['muscle']['slot']} of an "
                 f"{ours['roles']['muscle']['define']} fragment)" if ours['roles'].get('muscle') else ''))
    for p in problems:
        print('PROBLEM: ' + p)
    return 2 if problems else 0


def main(argv, con):
    ap = argparse.ArgumentParser(prog='q texdiff', description=__doc__.split('\n\n')[0])
    ap.add_argument('--effect', help='an effect name or a glob (yCh_*)')
    ap.add_argument('--flag', nargs='+', choices=FLAGS, help='only rows with a binding of these flags')
    ap.add_argument('--ref', help='read our code at this git ref (git show) instead of the working tree')
    import qasset_mods
    ap.add_argument('--source', type=qasset_mods.arg, default='game',
                    help='the asset index rows the material records come from: ' + qasset_mods.SOURCES + ' (S111 MODINDEX; default game: the mods folder left out)')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    try:
        return run(con, a)
    except (TexDiffError, CD.DiffError) as e:
        print(f'q texdiff: {e}', file=sys.stderr)
        return 2
