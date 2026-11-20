#!/usr/bin/env python3
r"""S103 WGD: the whole-game choice diff (docs\agents\choice_diff.md): for every mesh of every match arena, in the entrance and the match look, the game's own rule against what
our renderer REALLY picked (tools\harvest\choice_ours.py), the mismatches grouped by class. Offline, no game running.

  py -I tools\harvest\choice_diff.py [--ours DIR] [--arenas all|4,44] [--looks entrance,match] [--out-dir DIR] [--name choice_diff] [--sheet]

The game's side, per family (marks: [C] read in the game's code / files, [L] reasoning):
  stage  (pac keys 0..399): tools\harvest\choice_predict.py, the frozen read rule (1335 / 1337 and 1422 / 1422 on sealed arenas) [C]; its ASSUMED inputs are named per class.
  screen (800-809): the screens pass sub_82C0CA08 draws every movie key with the model's movie effect ([model+464] = yMovieDefault, set by sub_82BDB460 +0x1B24), whatever the
         file's effect: record 6 at +0x89C / sub_82BB1320 takes technique = (method arg r4 != 0) and pass = (model+540 == 1) [C listing]; both passes 0 / 1 of technique 0 are
         yMovieDefault_base_vertex + yMovieDefault_base_fragment, and the captured screen draws took the base fragment (docs\renderer\UNCLAIMED_MODEL_PARTS.md row 0x82C0F4BC) [C stream]:
         one pair. The r4 argument's writer is unread: technique 1 (alpha_movie fragment) would be the other.
  ring   (951-982): the prop rule (src\renderer_scene\technique_pick.h: the file's technique word +0x90, the effect table's pass 0) [L for the ring: the ring model's selector /
         model+540 / camera_1164 inputs are unread]; blend / alpha / depth write / cull not compared.
  flash  (31000): yFlashDefault (sub_82BDB460's fixed effect), pass [U]: judged against the effect's non-skinned pairs; blend / alpha / depth write / cull not compared.
  other  (props of other pacs: ring steps, desks): no game rule row in this version: not compared.
Compared directly: vs and ps variant names and their fx.json defines; samplers (address u / v / w, mag / min, mip) per bound slot against the effect's sampler table
(tools\shaders\effect_table.py sampler_table, the same mapping as tools\scene\sampler_compare.py). Through a stated mapping: the effect family (screen: yMovieDefault <-> our
yBG_Default; flash: yFlashDefault <-> the flash part's effect), blend (the game's stage pass blends EVERY draw SRC_ALPHA / INV_SRC_ALPHA: equal to our opaque draw where the
output alpha is 1 [L]), alpha test (the game's GEQUAL 1/255 on every stage draw: equal to our untested draw where the output alpha is never 0 [L]; a reference above 1 must match),
depth write (stage), cull as faces drawn and the front face of the base and the `r` mirrored copy (stage; SCU: the game's side settled in CUL.md / CUL_ADV.md). Not compared: the texture bound per slot (the predictor emits roles, never textures).
Writes OUT_DIR\NAME.txt (the class table: count, one example mesh, the inputs it rests on; per arena), NAME.json (the matrix + every mismatched row) and, with --sheet, NAME.png
(tools\harvest\choice_sheet.py). Names, counts and our variant names only. Exit 0 written (mismatches are FINDINGS, never a failure); 2 bad input."""
import argparse
import collections
import glob
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'arena'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'shaders'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'scene'))

MOVIE_VS, MOVIE_PS = 'yMovieDefault_base_vertex', 'yMovieDefault_base_fragment'
SCREEN_BASIS = 'C: sub_82C0CA08 record 6 (tech = r4 != 0, pass = model+540 == 1) + the stream took the base fragment; r4 writer unread'
FLASH_PAIRS = [('yFlashDefault_base_vertex', 'yFlashDefault_base_fragment'), ('yFlashDefault_shadow_vertex', 'yFlashDefault_shadow_fragment')]
FAMILY_MAP = {'screen': ('yMovieDefault', {'yBG_Default'}), 'flash': ('yFlashDefault', None)}   # game fixed effect -> the effects of ours that stand for it (None = any)
SAMPLER_FIELDS = ('u', 'v', 'w', 'mag', 'min', 'mip')
SAMPLER_STATS = collections.Counter()   # how many (mesh, look, slot) sampler states were compared (reported: a class-free run must show it compared something)
LEGACY_SHOW = {'entrance': (802, 804, 805, 807, 809), 'match': (800, 801, 805, 806, 808)}   # src\renderer_scene\stage_show.h LegacyShow (WGD2 section 3): an offline record carries no show bits
SCREEN_OFF_STAGE = (301, 302, 304)   # hidden while the arena has the entrance stage 801 / 802 (docs\formats\arena_slot_flags.md; rf_c_core.cpp the show rule)
# the predictor's ASSUMED inputs a field of a stage row rests on (choice_predict.py ASSUMED)
ASSUMED_PAIR = ['phase_word', 'registry', 'alias_table', 'model604', 'model616', 'model620', 'camera_1164', 'rec9_bit80', 'draw_gate']
ASSUMED_BLEND = ['inherited_state', 'blend_sep_off', 'param_bound']
ASSUMED_CULL = ['cull_k']   # SCU: cull_t (0) and cull_entry_mode (2) are SETTLED in choice_predict.py (CUL.md / CUL_ADV.md [C]); cull_k only matters while t != 0


# ---- the joiner (pure: plain dicts in, classes out; the unit enters here) --------------------------------------------------------------
def blend_kind(text):
    if text is None:
        return None
    if text == 'off':
        return 'off'
    col = text.split(' a:')[0]
    if 'SRC_ALPHA/ONE_MINUS_SRC_ALPHA/ADD' in col:
        return 'alpha'
    if 'SRC_ALPHA/ONE/ADD' in col:
        return 'add'
    return 'other(%s)' % col.replace('on ', '')


def alpha_ref(text):
    """'GEQUAL 0.00392157' -> ('GEQUAL', 1); 'off' -> None"""
    if not text or text == 'off':
        return None
    fn, _, v = text.partition(' ')
    return fn, int(round(float(v) * 255))


def define_delta(game, ours):
    """'+D' = only ours, '-D' = only the game's"""
    g, o = set(game or []), set(ours or [])
    return ' '.join(['+' + d for d in sorted(o - g)] + ['-' + d for d in sorted(g - o)])


def compare_stage(fam, side, g_name, o_name, g_defs, o_defs, g_eff, o_eff):
    """one shader stage (vs / ps) -> a class label or None"""
    if g_name == o_name:
        return None
    if fam in FAMILY_MAP:
        fixed, ours_ok = FAMILY_MAP[fam]
        if g_eff == fixed and ours_ok is not None and o_eff not in ours_ok:
            return '%s %s family: ours %s (game %s)' % (fam, side, o_eff or '(none)', fixed)
        d = define_delta(g_defs, o_defs)
        return ('%s %s defines %s' % (fam, side, d)) if d else None   # the mapped family with equal defines is equal
    d = define_delta(g_defs, o_defs)
    if d:
        return '%s %s defines %s' % (fam, side, d)
    if g_eff != o_eff:
        return '%s %s effect %s -> ours %s' % (fam, side, g_eff, o_eff)
    return '%s %s variant (same defines)' % (fam, side)


def alpha_one(o, g):
    """-> (True, '') when our draw's output alpha is PROVEN 1 [C: compile_shaders.py alpha_meta / fx_bg.glsl]: the vertex stage's alpha = MdlAlpha * MatAmbCol.w *
    MatDifCol.w * the vertex colour's alpha (vs out_alpha 1), the pixel stage's = that (ps out_alpha 1) or the diffuse texture's alpha times it (ps out_alpha 2: the texture
    must be alpha-free, diffuse_alpha_clear 0); the vertex colours' alpha from the file (g['vtx_alpha_min']). Else (False, the first input that is not proven)."""
    r = o.get('raw') or {}
    if r.get('vs_oa') != 1:
        return False, 'vertex variant alpha not provable (out_alpha %s)' % r.get('vs_oa')
    if o.get('out_alpha') not in (1, 2):
        return False, 'pixel variant alpha not provable (out_alpha %s)' % o.get('out_alpha')
    if o.get('out_alpha') == 2 and (r.get('dal') is None or r['dal'] > 0):
        return False, 'the diffuse texture carries alpha'
    if r.get('mdl_a') != 1 or r.get('amb_w') != 1 or r.get('dif_w') != 1:
        return False, 'MdlAlpha / MatAmbCol.w / MatDifCol.w not 1'
    if r.get('vc', 255) != 255:
        va = (g or {}).get('vtx_alpha_min')
        if va is None:
            return False, 'vertex colour alpha unread'
        if va < 255:
            return False, 'vertex colour alpha below 255'
    return True, ''


def classify(g, o):
    """g = the game's row, o = ours (either may be None) -> (classes [(field, label, assumed list)], not_compared [(field, label)]).
    field: row (presence), pair (vs / ps), blend, alpha, zwrite, cull, sampler. Every path that does not compare a field appends a not-compared entry: nothing is skipped silently."""
    cls, nc = [], []
    fam = (g or o)['family']
    if o is None:
        if g.get('look_hidden'):
            nc.append(('row', '%s not drawn in this look (%s, both sides)' % (fam, g['look_hidden'])))
        elif g.get('problem'):
            nc.append(('row', '%s game side: %s' % (fam, g['problem'])))
        elif fam == 'screen' and g['slot'] not in LEGACY_SHOW.get(g['look'], ()):
            nc.append(('row', 'screen key %d not in this look\'s show set (ours: LegacyShow; the game\'s per-window show bits are not ported)' % g['slot']))
        elif fam == 'ring':
            nc.append(('row', 'ring key %d not drawn by our record at rest (which ring keys the game draws per match type is not ported)' % g['slot']))
        elif g.get('cycle_group') is not None:
            nc.append(('row', 'stage timed-cycle slot not drawn at the record\'s time (the group check counts members drawn)'))
        else:
            cls.append(('row', '%s ours lacks the mesh' % fam, []))
        return cls, nc
    if g is None:
        if fam == 'other':
            nc.append(('row', 'other parts (props of other pacs): no game rule row in this version'))
        else:
            cls.append(('row', '%s ours draws a mesh the game side has no row for' % fam, []))
        return cls, nc
    if g.get('look_hidden'):
        cls.append(('row', '%s ours draws a mesh the game hides in this look (%s)' % (fam, g['look_hidden']), []))
    if g.get('problem'):
        nc.append(('pair', '%s game side: %s' % (fam, g['problem'])))
        return cls, nc
    if o.get('path') in ('unbound', 'skinned_unwired'):
        cls.append(('row', '%s ours not drawn (%s)' % (fam, o['path']), []))
        return cls, nc
    rests = g.get('assumed', [])
    pair_assumed = [x for x in rests if x in ASSUMED_PAIR or x in ('outer_selector', 'shadow_module', 'shadow_gate_pass') or fam != 'stage']
    pair_differs = False
    for side in ('vs', 'ps'):
        options = g.get('options') or [(g['vs'], g['ps'], g['vs_defines'], g['ps_defines'], g['vs_effect'], g['ps_effect'])]
        labels = []
        for op in options:
            gi = 0 if side == 'vs' else 1
            labels.append(compare_stage(fam, side, op[gi], o[side], op[2 + gi], o[side + '_defines'], op[4 + gi], o[side + '_effect']))
        if None not in labels:
            rl = o.get('rule') or {}
            if fam == 'stage' and labels[0].endswith('defines -FX_SHADOW') and rl.get('shadow') == 1 and rl.get('pass_used') != rl.get('pass_rule'):
                nc.append(('pair', 'stage %s shadow pass: no floor map in the offline frame (the rule took the pass without +2, d1 shadow 1)' % side))
                continue
            pair_differs = True
            cls.append(('pair', labels[0], pair_assumed + (['flash pass [U]'] if fam == 'flash' else []) +
                        (['screen technique: sub_82C0CA08 arg r4 writer unread (technique 0 seen in the stream)'] if fam == 'screen' else [])))
    if g.get('compare_states'):
        one, why = alpha_one(o, g)
        gb, ob = blend_kind(g.get('blend')), blend_kind(o.get('blend'))
        if gb is None or ob is None:
            nc.append(('blend', '%s blend: the %s side has no value' % (fam, 'game' if gb is None else 'our')))
        elif gb == 'alpha' and ob == 'off':
            if not one:   # equal only where our output alpha is proven 1
                cls.append(('blend', '%s blend [L] game alpha-blend / ours opaque: output alpha not proven 1 (%s)' % (fam, why), [x for x in rests if x in ASSUMED_BLEND] or ['inherited_state']))
        elif gb != ob:
            cls.append(('blend', '%s blend game %s / ours %s' % (fam, gb, ob), [x for x in rests if x in ASSUMED_BLEND] or ['inherited_state']))
        ga, oa = alpha_ref(g.get('alpha')), alpha_ref(o.get('alpha'))
        if g.get('alpha') is None or o.get('alpha') is None:
            nc.append(('alpha', '%s alpha test: the %s side has no value' % (fam, 'game' if g.get('alpha') is None else 'our')))
        elif ga is None:
            if oa is not None:
                cls.append(('alpha', '%s alpha test: ours tests, game off' % fam, []))
        elif oa is None:
            if ga[1] > 1:
                cls.append(('alpha', '%s alpha ref game %s %d / ours untested' % (fam, ga[0], ga[1]), []))
            elif not one:   # GEQUAL 1/255 drops only alpha 0: equal to no test only where the output alpha is proven 1
                cls.append(('alpha', '%s alpha test [L] game %s %d / ours untested: output alpha not proven 1 (%s)' % (fam, ga[0], ga[1], why), []))
        elif oa != ga and not (ga[1] <= 1 and oa[1] <= 1 and ga[0] == oa[0]):
            cls.append(('alpha', '%s alpha ref game %s %d / ours %s %d' % (fam, ga[0], ga[1], oa[0], oa[1]), []))
        if g.get('zwrite') is None or o.get('zwrite') is None or o.get('zwrite') < 0:
            nc.append(('zwrite', '%s depth write: the %s side has no value' % (fam, 'game' if g.get('zwrite') is None else 'our')))
        elif int(g['zwrite']) != int(o['zwrite']):
            cls.append(('zwrite', '%s depth write game %d / ours %d' % (fam, g['zwrite'], o['zwrite']), []))
        faces = g.get('faces')
        if faces is None or o.get('cull') is None or o.get('cull') < 0:
            nc.append(('cull', '%s cull: the %s side has no face rule' % (fam, 'game' if faces is None else 'our')))
        elif faces == 'none':
            cls.append(('cull', '%s cull: the game does not draw the mesh (rule_cull: no sub-pass) / ours draws it' % fam, ASSUMED_CULL))
        else:
            of = 'both' if o['cull'] == 0 else 'one'
            if of != faces:
                gdesc = 'one sub-pass, back faces culled (+0x8C bit 2, entry winding 2)' if faces == 'one' else 'both faces (cull off)'
                cls.append(('cull', '%s cull: game %s / ours %s' % (fam, gdesc, 'two-sided' if of == 'both' else 'one face culled'), ASSUMED_CULL))
            elif faces == 'one':
                # SCU: the winding as our front face (the game's value 6 = cull back + front CW = our front_face 1 for mode 2; mode 1 = front CCW = 0), the base copy and the `r` copy
                want = {2: 1, 1: 0}
                if g.get('cull_mode') in want and o.get('ff') is not None and o['ff'] >= 0 and o['ff'] != want[g['cull_mode']]:
                    cls.append(('cull', '%s cull front face: game %s / ours %s (base copy)' % (fam, 'CW' if want[g['cull_mode']] else 'CCW', 'CW' if o['ff'] else 'CCW'), ASSUMED_CULL))
                gm, om = g.get('copy_mode'), o.get('ff_mirror')
                if gm is not None and om is None:
                    nc.append(('cull', '%s cull: the `r` copy has no line of ours (not drawn in this look)' % fam))
                elif gm is None and om is not None:
                    cls.append(('cull', '%s cull: ours draws a mirrored copy the game does not (no `r` token)' % fam, ASSUMED_CULL))
                elif gm is not None:
                    if (o.get('cull_mirror') or 0) == 0:
                        cls.append(('cull', '%s cull: game culls the `r` copy / ours draws it two-sided' % fam, ASSUMED_CULL))
                    elif om != want.get(gm, -1):
                        cls.append(('cull', '%s cull front face: game %s / ours %s (the `r` copy)' % (fam, 'CW' if want.get(gm) else 'CCW', 'CW' if om else 'CCW'), ASSUMED_CULL))
    else:
        nc.append(('states', '%s blend / alpha / depth write / cull: no game rule ported' % fam))
    if not g.get('samplers'):
        nc.append(('sampler', '%s samplers: no game sampler table for this row' % fam))
    elif pair_differs:
        nc.append(('sampler', '%s samplers: the pair differs (another effect\'s table)' % fam))
    elif any(f == 'pair' for f, _n in nc):
        nc.append(('sampler', '%s samplers: the pair was not compared (the game\'s pair is not the one drawn)' % fam))
    else:
        bad = collections.Counter()
        for slot, exp in g['samplers'].items():
            got = (o.get('samplers') or {}).get(str(slot))
            if exp is None:
                nc.append(('sampler', '%s sampler slot %s: the game\'s table has no entry' % (fam, slot)))
                continue
            if got is None:
                cls.append(('sampler', '%s sampler slot %s: ours binds nothing' % (fam, slot), []))
                continue
            gotd = dict(zip(SAMPLER_FIELDS, got))
            SAMPLER_STATS['slots compared'] += 1
            for k in SAMPLER_FIELDS:
                if exp[k] != gotd[k]:
                    bad['slot %s %s' % (slot, k)] += 1
        if bad:
            cls.append(('sampler', '%s sampler %s' % (fam, ', '.join(sorted(bad))), []))
    return cls, nc


def join(game_rows, our_rows):
    """-> (list of (key, g, o, classes, not_compared), duplicate ours keys, duplicate game keys). key = (arena, look, file, mesh). A duplicate key on either side is
    counted (the first row is kept) and reported, never dropped silently."""
    G, gdup = {}, 0
    for g in game_rows:
        k = (g['arena'], g['look'], g['file'], g['mesh'])
        if k in G:
            gdup += 1
            continue
        G[k] = g
    O, dup = {}, 0
    for o in our_rows:
        k = (o['arena'], o['look'], o['file'], o['mesh'])
        if k in O:
            dup += 1
            continue
        O[k] = o
    out = []
    for k in sorted(set(G) | set(O), key=lambda x: (x[0], x[1], str(x[2]), x[3])):
        g, o = G.get(k), O.get(k)
        cls, nc = classify(g, o)
        out.append((k, g, o, cls, nc))
    return out, dup, gdup


def cycle_classes(joined):
    """[L] a timed-cycle group (entries 60010-60025) shows one member at a time: per (arena, look, group) the members our record drew; a count other than 1 is a class."""
    drawn = collections.defaultdict(set)
    members = collections.defaultdict(set)
    for k, g, o, cls, nc in joined:
        if g and g.get('cycle_group') is not None:
            gk = (k[0], k[1], g['cycle_group'])
            members[gk].add(g['slot'])
            if o is not None:
                drawn[gk].add(g['slot'])
    out = []
    for gk in sorted(members):
        n = len(drawn[gk])
        if n != 1:
            out.append((gk, 'stage timed-cycle group [L] draws %d of %d members at the record\'s time (one expected)' % (n, len(members[gk]))))
    return out, len(members)


def tabulate(joined):
    """-> (classes: label -> dict(count, example, assumed, arenas, meshes, field), not_compared Counter, headline per family)."""
    classes = {}
    ncs = collections.Counter()
    head = collections.defaultdict(collections.Counter)

    def add(label, field, assumed, k, g, o):
        c = classes.setdefault(label, dict(count=0, example=None, assumed=set(), arenas=collections.Counter(), meshes=set(), field=field))
        c['count'] += 1
        c['arenas'][k[0]] += 1
        c['meshes'].add((k[0], k[2], k[3]))
        c['assumed'].update(assumed)
        if c['example'] is None:
            c['example'] = dict(arena=k[0], look=k[1], file=k[2], mesh=k[3], effect=(o or g or {}).get('effect'), game=g and [g.get('vs'), g.get('ps')], ours=o and [o.get('vs'), o.get('ps'), o.get('path')],
                                rule_why=(o or {}).get('rule', {}).get('why') if o else None)
    for k, g, o, cls, nc in joined:
        fam = (g or o)['family']
        h = head[fam]
        h['rows'] += 1
        for f, n in nc:
            ncs[n] += 1
        both = g is not None and o is not None and not g.get('problem') and o.get('path') not in ('unbound', 'skinned_unwired')
        if both:
            h['pairs compared'] += 1
            pair_cls = [c for c in cls if c[0] == 'pair']
            pair_nc = [n for n in nc if n[0] == 'pair']
            if pair_nc:
                h['pairs with a field not compared'] += 1
            elif pair_cls:
                h['pairs differ'] += 1
            else:
                h['pairs equal'] += 1
            if not cls and not nc:
                h['rows equal in every field'] += 1
            elif nc and not cls:
                h['rows with no class but a field not compared'] += 1
        for f, label, assumed in cls:
            add(label, f, assumed, k, g, o)
        if cls:
            h['rows with a class'] += 1
    cyc, ngroups = cycle_classes(joined)
    for (a, look, grp), label in cyc:
        add(label, 'row', ['the cycle rule [L]: one member shown at a time'], (a, look, 'cycle group %s' % grp, -1), None, None)
    head['stage']['cycle groups checked'] = ngroups
    return classes, ncs, head


def headline(head):
    """the sentence per family the text table and the picture print, word for word"""
    out = []
    for fam in sorted(head):
        h = head[fam]
        out.append('%s: %d rows joined; %d pairs compared: %d equal, %d differ, %d with a pair field not compared; %d rows equal in every field, %d rows with a class' % (
            fam, h['rows'], h['pairs compared'], h['pairs equal'], h['pairs differ'], h['pairs with a field not compared'], h['rows equal in every field'], h['rows with a class']))
    return out


# ---- the game's side from the player's files ------------------------------------------------------------------------------------------
def extra_records(arena_id):
    """-> (raw, arena): raw = (slot, mesh) -> {name, w90} for the screen and ring keys (the predictor's record words); arena = arena_files' load with positions and the
    extra keys (its mesh colours give the vertex alpha our loader uploads: the S0 colour word's byte 0, src\\assets\\jboy.h:581)."""
    import arena_files as af
    raw = {}
    orig = af.parse_jboy

    def wrap(slot, blob, with_positions=True):
        out = orig(slot, blob, with_positions)
        if slot >= 400:
            body = blob[8:8 + af.be32(blob, 4)]
            mtab = af.be32(body, 0x14)
            for mi in range(af.be32(body, 0x10)):
                rec = mtab + mi * af.MESH_REC
                raw[(slot, mi)] = dict(name=af._cstr(body[rec + 0x7C:rec + 0x7C + 16]), w90=af.be32(body, rec + 0x90))
        return out
    af.parse_jboy = wrap
    try:
        af._CACHE.pop((arena_id, True, True), None)
        arena = af.load_arena(arena_id, with_positions=True, extra_keys=True)
    finally:
        af.parse_jboy = orig
        af._CACHE.pop((arena_id, True, True), None)   # (49 arenas with positions: not kept)
    return raw, arena


def vertex_alpha_min(arena):
    """(slot, mesh) -> the lowest vertex-colour alpha byte our loader uploads for that mesh (None: no colour stream read)"""
    out = {}
    for slot, meshes in arena.slots.items():
        for mi, m in enumerate(meshes):
            out[(slot, mi)] = int(m.colour[:, 0].min()) if m.colour is not None and len(m.colour) else None
    return out


def cycle_groups(arena_id, cp):
    """slot -> its timed-cycle group (the entry id 60010-60025: (slot, ticks) pairs; choice_predict.cycled_slots' reader, kept per group)"""
    import re
    out = {}
    for k, d in cp._entries(arena_id, lambda x: 60010 <= x <= 60025).items():
        nums = [int(x) for x in re.findall(r'-?\d+', d.decode('cp932', 'replace'))]
        for s in nums[0::2]:
            out.setdefault(s, k)
    return out


class GameSide:
    def __init__(self):
        import choice_ours
        import choice_predict as cp
        import effect_table
        import sampler_compare
        self.cp, self.sc = cp, sampler_compare
        self.fx = choice_ours.FxDefines()
        self.eff = cp.Effects()
        self.samp, _ = effect_table.sampler_table(effect_table.load_all())
        self.fxj = cp.FxJson()
        self.notes = collections.Counter()   # not-compared entries that have no row (the flash part)

    def _samplers(self, effect, vs, ps):
        fxj = self.fxj
        slots = set(fxj.get(vs)['samplers']) | set(fxj.get(ps)['samplers'])
        return {str(s): self.sc.expected(self.samp, effect, s) for s in sorted(slots)}

    def _row(self, arena, look, slot, mesh, fam, effect, vs, ps, **kw):
        r = dict(arena=arena, look=look, file='bg%02d.pac#%d' % (arena, slot), slot=slot, mesh=mesh, family=fam, effect=effect, vs=vs, ps=ps,
                 vs_defines=self.fx.defines(vs), ps_defines=self.fx.defines(ps), vs_effect=self.fx.effect(vs), ps_effect=self.fx.effect(ps))
        r.update(kw)
        return r

    def rows(self, arena, looks):
        cp = self.cp
        out = []
        pcache = {}
        table, _B = cp.predict(arena, looks, {}, cache=pcache)
        praw = pcache[arena][1]   # (slot, mesh) -> the record words (w8c: the cull bit 2)
        texts = cp.slot_texts(arena)
        ex, A = extra_records(arena)
        valpha = vertex_alpha_min(A)
        cyc = cycle_groups(arena, cp)
        if 31000 in A.ents:
            self.notes['flash part (pac key 31000): never a scene mesh of ours (front_end.cpp AddModelImpl skips "crowd flash" parts: the viewer\'s flash pass)'] += len(looks)
        has_ent_stage = any(s in (801, 802) for s, _m in ex)
        for m in table['meshes']:
            toks = set(texts.get(m['slot'], []))
            bit2 = bool(praw.get((m['slot'], m['mesh']), {}).get('w8c', 0) & 2)
            for look in looks:
                lk = m['looks'][look]
                g = m['game'][look]
                hidden = 'e token' if (look == 'match' and 'e' in toks and 'g' not in toks) else 'g token' if (look == 'entrance' and 'g' in toks and 'e' not in toks) else None
                if hidden is None and m['slot'] in SCREEN_OFF_STAGE and has_ent_stage:
                    hidden = 'screen-off stage while 801 / 802 exist'
                common = dict(look_hidden=hidden, cycle_group=cyc.get(m['slot']), bit2=bit2, vtx_alpha_min=valpha.get((m['slot'], m['mesh'])))
                if not lk['choices']:
                    out.append(self._row(arena, look, m['slot'], m['mesh'], 'stage', m['effect'], '', '', problem=(g or {}).get('problem', 'no prediction'), **common))
                    continue
                ch = lk['choices'][0]
                assumed = list(ASSUMED_PAIR)
                if g.get('draw_group') or g.get('cycled'):
                    assumed.append('outer_selector')
                if g.get('shadow'):
                    assumed += ['shadow_module', 'shadow_gate_pass']
                cull = g.get('cull') or {}
                # choice_predict.rule_cull: two sub-passes or cull off = both faces; one sub-pass with cull = one face; no sub-pass (drawn False) = the game does not draw it
                faces = ('both' if (cull.get('subpasses') == 2 or cull.get('cull') is False) else 'one' if cull.get('subpasses') == 1 else
                         'none' if cull.get('subpasses') == 0 else None)
                gef = g.get('effect') or ''
                out.append(self._row(arena, look, m['slot'], m['mesh'], 'stage', m['effect'], ch['vs'], ch['ps'], blend=ch['blend'], alpha=ch['alpha'], zwrite=g.get('zwrite'),
                                     faces=faces, cull_mode=cull.get('mode'), copy_mode=cull.get('copy_mode'), compare_states=True, assumed=assumed + ASSUMED_BLEND + ASSUMED_CULL, cycled=bool(g.get('cycled')),
                                     samplers=self._samplers(gef, ch['vs'], ch['ps']), basis='C: the read rule (choice_predict.py)', **common))
        for (slot, mi), rec in sorted(ex.items()):
            for look in looks:
                if 800 <= slot <= 809:
                    out.append(self._row(arena, look, slot, mi, 'screen', rec['name'], MOVIE_VS, MOVIE_PS, basis=SCREEN_BASIS, samplers=None, vtx_alpha_min=valpha.get((slot, mi))))
                elif 951 <= slot <= 982:
                    e = self.eff.get(rec['name'].lower())
                    if e is None:
                        out.append(self._row(arena, look, slot, mi, 'ring', rec['name'], '', '', problem='effect %s not in the registry' % rec['name']))
                        continue
                    p, _rs, _ss, probs = cp.rule_pass_blocks(cp.Branches(), e, rec['w90'], 0)
                    if p is None:
                        out.append(self._row(arena, look, slot, mi, 'ring', rec['name'], '', '', problem='; '.join(probs)))
                        continue
                    vs, ps = (p['vs'] or {}).get('name') or '', (p['ps'] or {}).get('name') or ''
                    out.append(self._row(arena, look, slot, mi, 'ring', rec['name'], vs, ps, basis='L: the prop rule (technique word, pass 0)', assumed=['ring selector / model540 / camera_1164 [U]'],
                                         samplers=self._samplers(e['name'], vs, ps)))
        return out


def flash_rows(our_rows, fx):
    """the flash part has no file-side reader here: its game rows are made for the meshes our record drew (yFlashDefault's non-skinned pairs, pass [U])"""
    out = []
    for o in our_rows:
        if o['family'] == 'flash':
            opts = [(v, p, fx.defines(v), fx.defines(p), fx.effect(v), fx.effect(p)) for v, p in FLASH_PAIRS]
            out.append(dict(arena=o['arena'], look=o['look'], file=o['file'], slot=o['slot'], mesh=o['mesh'], family='flash', effect='yFlashDefault', vs=FLASH_PAIRS[0][0], ps=FLASH_PAIRS[0][1],
                            vs_defines=[], ps_defines=[], vs_effect='yFlashDefault', ps_effect='yFlashDefault', options=opts, basis='L: yFlashDefault, pass [U]'))
    return out


def game_stamp(arena_id):
    """what the game side of an arena depends on: the arena pac, the effect file and the tools that read them (size + mtime each)"""
    import arena_files as af
    paths = [os.path.join(af.pac.GAME, 'pac', 'bg', 'bg%02d.pac' % arena_id), os.path.join(af.pac.GAME, 'pac', '_startup.pac'), os.path.abspath(__file__),
             os.path.join(HERE, 'choice_predict.py'), os.path.join(HERE, 'choice_ours.py'), os.path.join(ROOT, 'tools', 'arena', 'arena_files.py'),
             os.path.join(ROOT, 'tools', 'shaders', 'effect_table.py'), os.path.join(ROOT, 'tools', 'scene', 'sampler_compare.py')]
    return ['%s:%d:%d' % (os.path.basename(p), os.path.getsize(p), int(os.path.getmtime(p))) if os.path.exists(p) else '%s:missing' % os.path.basename(p) for p in paths]


def load_ours(d, arenas, looks):
    rows, missing = [], []
    for a in arenas:
        for lk in looks:
            f = os.path.join(d, 'a%02d_%s.jsonl' % (a, lk))
            if not os.path.exists(f):
                missing.append(os.path.basename(f))
                continue
            rows += [json.loads(x) for x in open(f, encoding='utf-8') if x.strip()]
    return rows, missing


def cull_accounting(joined):
    """the stage meshes whose record word +0x8C has bit 2 (the game culls them with one sub-pass): how many there are in the files, and where each one went"""
    per = {}
    for k, g, o, cls, nc in joined:
        if not g or g['family'] != 'stage' or not g.get('bit2'):
            continue
        mk = (k[0], k[2], k[3])
        if any(c[0] == 'cull' for c in cls):
            out = 'in the cull class'
        elif o is not None and not g.get('problem') and o.get('path') not in ('unbound', 'skinned_unwired'):
            out = 'compared, cull equal'
        else:
            out = 'not compared: ' + (nc[0][1] if nc else 'ours not drawn (%s)' % (o or {}).get('path'))
        per.setdefault(mk, set()).add(out)
    tally = collections.Counter()
    for mk, outs in per.items():   # a mesh counts once: the cull class if either look has it, else compared, else its first reason
        tally['in the cull class' if 'in the cull class' in outs else 'compared, cull equal' if 'compared, cull equal' in outs else sorted(outs)[0]] += 1
    return len(per), tally


def report(joined, dup, gdup, arenas, looks, args_text, notes=None):
    classes, ncs, head = tabulate(joined)
    for n, v in (notes or {}).items():
        ncs[n] += v
    if gdup:
        ncs['game side: duplicate key (the first row kept)'] += gdup
    order = sorted(classes, key=lambda c: (-classes[c]['count'], c))
    heads = headline(head)
    nbit2, ctally = cull_accounting(joined)
    L = ['# choice_diff: the game\'s rule against our real draw, every mesh of %d match arenas x %s (%s)' % (len(arenas), '/'.join(looks), args_text),
         '# a class is a FINDING (game != ours by the stated rule); "rests on" = the predictor\'s ASSUMED inputs the game\'s side of that field depends on; [L] = a mapping not proven per row.',
         '# a row with a field not compared is never counted equal ("pairs equal" = both shaders compared and equal).', '']
    L += heads
    L += ['sampler slots compared %d; duplicate rows: ours %d, game %d; stage timed-cycle groups checked %d' % (SAMPLER_STATS['slots compared'], dup, gdup, head['stage']['cycle groups checked']),
          'stage meshes with the cull bit (+0x8C bit 2) in the files: %d distinct; %s' % (nbit2, '; '.join('%s %d' % kv for kv in sorted(ctally.items(), key=lambda kv: -kv[1]))), '',
          '## mismatch classes (%d): count (meshes x looks) | distinct meshes | arenas | example | rests on' % len(order)]
    for c in order:
        x = classes[c]
        e = x['example']
        L.append('%6d | %5d | %2d | %s' % (x['count'], len(x['meshes']), len(x['arenas']), c))
        L.append('         e.g. a%02d %s %s mesh %s effect %s: game %s / ours %s%s' % (e['arena'], e['look'], e['file'], e['mesh'], e['effect'], e['game'], e['ours'],
                                                                                 (' (rule: %s)' % e['rule_why']) if e.get('rule_why') else ''))
        if x['assumed']:
            L.append('         rests on: %s' % ', '.join(sorted(x['assumed'])))
    L += ['', '## not compared (count, reason)']
    for n, v in sorted(ncs.items(), key=lambda kv: -kv[1]):
        L.append('%6d  %s' % (v, n))
    L += ['', '## per arena (classes with a count; the picture: choice_diff.png)']
    for a in arenas:
        cells = ['%s=%d' % (order.index(c) + 1, classes[c]['arenas'][a]) for c in order if classes[c]['arenas'].get(a)]
        L.append('a%02d  %s' % (a, ' '.join(cells) or '-'))
    L.append('')
    L.append('## class numbers: ' + '; '.join('%d %s' % (i + 1, c) for i, c in enumerate(order)))
    matrix = dict(arenas=arenas, classes=order, counts={c: {str(a): classes[c]['arenas'].get(a, 0) for a in arenas} for c in order},
                  examples={c: classes[c]['example'] for c in order}, totals={c: classes[c]['count'] for c in order}, meshes={c: len(classes[c]['meshes']) for c in order},
                  not_compared=dict(ncs), headline=heads, head={f: dict(h) for f, h in head.items()})
    return '\n'.join(L) + '\n', matrix


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--ours', default=os.path.join(ROOT, 'out', 'wgd', 'ours'))
    ap.add_argument('--arenas', default='all')
    ap.add_argument('--looks', default='entrance,match')
    ap.add_argument('--out-dir', default=os.path.join(ROOT, 'out', 'wgd'))
    ap.add_argument('--name', default='choice_diff')
    ap.add_argument('--sheet', action='store_true')
    ap.add_argument('--run-ours', action='store_true', help='first run choice_ours.py for these arenas / looks into --ours (the gate lane\'s rig check)')
    ap.add_argument('--fail-on', action='append', default=[], help='exit 1 when a class starts with this text (a guard on a fixed class, e.g. "screen ")')
    ap.add_argument('--game-cache', default=os.path.join(ROOT, 'out', 'wgd', 'game_cache'), help='where the game side is cached per arena (scratch; "" = no cache)')
    ap.add_argument('--against', help='the reference arm\'s NAME.json: list the classes this run adds or grows (a planted twin\'s re-find)')
    a = ap.parse_args()
    import choice_ours
    arenas = choice_ours.MATCH_ARENAS if a.arenas == 'all' else [int(x) for x in a.arenas.split(',')]
    looks = a.looks.split(',')
    if a.run_ours:
        import subprocess
        rc = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'choice_ours.py'), '--arenas', ','.join(str(x) for x in arenas), '--looks', ','.join(looks), '--out', a.ours]).returncode
        if rc != 0:
            print('FAIL choice_ours.py exit %d' % rc)
            return 1
    ours, missing = load_ours(a.ours, arenas, looks)
    if missing:
        print('ours missing (run choice_ours.py first): %s' % ', '.join(missing[:8]))
        return 2
    gs = None
    game, notes = [], collections.Counter()
    for ar in arenas:   # the game's side depends on the player's files and these tools only: cached per arena (8-12 s each to make), keyed on their stamps
        rows, nts = None, None
        cf = os.path.join(a.game_cache, 'a%02d_%s.json' % (ar, '_'.join(looks))) if a.game_cache else None
        stamp = game_stamp(ar)
        if cf and os.path.exists(cf):
            try:
                c = json.load(open(cf, encoding='utf-8'))
                if c.get('stamp') == stamp:
                    rows, nts = c['rows'], c['notes']
            except (OSError, ValueError, KeyError):
                rows = None
        if rows is None:
            gs = gs or GameSide()
            before = collections.Counter(gs.notes)
            rows = gs.rows(ar, looks)
            nts = dict(gs.notes - before)
            if cf:
                os.makedirs(a.game_cache, exist_ok=True)
                json.dump(dict(stamp=stamp, rows=rows, notes=nts), open(cf, 'w', encoding='utf-8'))
        game += rows
        notes.update(nts)
    gs = gs or GameSide()
    game += flash_rows(ours, gs.fx)
    joined, dup, gdup = join(game, ours)
    summ = json.load(open(os.path.join(a.ours, 'ours_summary.json'))) if os.path.exists(os.path.join(a.ours, 'ours_summary.json')) else {}
    text, matrix = report(joined, dup, gdup, arenas, looks, 'ours env %s%s' % (summ.get('env') or '{}', ', every movie key shown' if summ.get('show_all_screens') else ''), notes)
    os.makedirs(a.out_dir, exist_ok=True)
    open(os.path.join(a.out_dir, a.name + '.txt'), 'w', encoding='utf-8').write(text)
    matrix['rows'] = [dict(key=list(k), classes=[c[1] for c in cls], game=g and {x: g.get(x) for x in ('vs', 'ps', 'blend', 'alpha', 'zwrite', 'faces', 'basis')},
                           ours=o and {x: o.get(x) for x in ('vs', 'ps', 'path', 'blend', 'alpha', 'zwrite', 'cull', 'effect')}) for k, g, o, cls, nc in joined if cls]
    json.dump(matrix, open(os.path.join(a.out_dir, a.name + '.json'), 'w'), indent=1, sort_keys=True)
    print(text[:6000])
    if a.sheet:
        import choice_sheet
        choice_sheet.draw(matrix, os.path.join(a.out_dir, a.name + '.png'), title='%s: game rule vs our draw (%s)' % (a.name, 'ours env %s' % (summ.get('env') or 'none')))
    print('wrote %s.txt / .json%s under %s' % (a.name, ' / .png' if a.sheet else '', a.out_dir))
    if a.against:   # a planted arm against its reference arm: the classes the plant adds or grows (the re-find)
        ref = json.load(open(a.against))
        rt = ref.get('totals', {})
        lines = ['', '## against %s: classes new or grown in this run (this run / the reference)' % a.against]
        for c in sorted(matrix['classes'], key=lambda c: -(matrix['totals'][c] - rt.get(c, 0))):
            if matrix['totals'][c] > rt.get(c, 0):
                lines.append('%6d / %6d  (+%d, %d distinct meshes)  %s' % (matrix['totals'][c], rt.get(c, 0), matrix['totals'][c] - rt.get(c, 0), matrix['meshes'][c], c))
        gone = [c for c in rt if c not in matrix['totals']]
        lines.append('classes of the reference absent here: %s' % ('; '.join(gone) or 'none'))
        txt = '\n'.join(lines) + '\n'
        print(txt)
        open(os.path.join(a.out_dir, a.name + '.txt'), 'a', encoding='utf-8').write(txt)
    hit = [c for c in matrix['classes'] for p in a.fail_on if c.startswith(p)]
    if hit:
        print('FAIL a guarded class is present: %s' % '; '.join('%s (%d)' % (c, matrix['totals'][c]) for c in hit))
        return 1
    if a.fail_on:
        print('PASS no class starts with %s' % ' / '.join(repr(p) for p in a.fail_on))
    return 0


if __name__ == '__main__':
    sys.exit(main())
