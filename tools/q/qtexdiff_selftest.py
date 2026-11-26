r"""Self-test of `q texdiff` (S111 TEXDIFF): `py -I tools/q/qtexdiff_selftest.py [--root DIR]`   (0 game runs)

A. Synthetic text (invented, no game code): RoleOfSlot's cases + default, RoleOfWetSlot's ifs, RoleOfChangeSlot through a
   MaskIndexOfSlot, CheckPs's cube slot; the character switch (grouped cases, a material texture with a stand-in fallback, a map
   slot, the liquid slot); a plain chain (a range arm, a conditional arm, the white default); the draw-time replacement line; the
   loader's name -> field lines. Each must read back as written.
B. Real data (needs generated/, the q index and the asset index; else SKIP), our code frozen at FROZEN (the S111 tip the two cases
   were stated against): the name table's sampler rows (g_mMusclePrm = slot 2 at row 0x8392AB00, g_mBSSPrm = slot 14 at 0x8392AC80),
   and the two cases RE-FOUND:
     1. slot 2 (g_mMusclePrm, GAME-MATERIAL) of yCh_skin_flex: DIFF, ours black at front_end.cpp:1424 (RoleOfSlot's default,
        char_effects.cpp:120 at FROZEN);
     2. slot 14 (g_mBSSPrm) of yCh_gear's wet bind: UNSETTLED (no record and no code names g_mBSSPrm: GAME-NOREC), ours black at
        front_end.cpp:1408 behind the liquid map's yCh_skin gate (front_end.cpp:2700);
   with slot 0 of yCh_skin OK and slot 14 of yCh_skin's wet bind OK (the liquid map).
   S111 review: the game side keeps only the records whose own technique samples the slot, and an effect no record names is
   UNUSED: yDefault slot 1 (texSpecularMap, the D5 / D6 false alarm: 5 records name it, under techniques 0 / 1 whose passes never
   read slot 1) and yCh_Anisotropic slot 8 (no record names the effect) are UNUSED; yCh_Aniso_trans slot 8 / 9 and yCh_trans slot
   10 (technique 1 only) stay DIFF; slot 4 of yCh_Aniso_trans (mat.logo4, filled by code) stays OK.
C. Planted twins on the same frozen text, each must be CAUGHT (the check it targets must fail on it):
   the FlatBlack case bound to a material field the loader fills from g_mMusclePrm (the muscle DIFF must go; S111 review: a field
   nothing fills is a stand-in, so the loader line is planted too), the liquid gate widened to every yCh_ (the gear
   UNSETTLED must go), the Diffuse case bound to white (a NEW DIFF at slot 0), slot 0's role made Normal (a ROLE DIFF at slot 0),
   a role with no case in the switch (a PROBLEM); S111 review: an early return in RoleOfSlot (a PROBLEM, not a silent OK at slot
   0), slot 8 bound to a material field nothing fills (still DIFF, not OK), yDefault technique 1 made to read slot 1 (slot 1 no
   longer UNUSED), a yCh_Anisotropic record naming g_mHairNoisePrm (slot 8 no longer UNUSED).
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qtexdiff as T   # noqa: E402

FROZEN = 'd2195d5fe'      # the S111 session tip the two cases were stated against (front_end.cpp:1424 / :1408 / :2700, char_effects.cpp:121)
FRONT, CHAR = T.FRONT, T.CHAR
FAILS, CAUGHT = [], []


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, msg):
    """msg = the checker's failure message on a planted case; None = the checker accepted it (a defect)."""
    if msg is None:
        CAUGHT.append(name)
        print(f'NOT CAUGHT planted twin: {name}')
    else:
        print(f'ok   planted twin caught: {name} ({msg})')


# ------------------------------------------------------------------ A. synthetic text
SYN_CHAR = r'''
CharSlot RoleOfSlot(int slot) {
  switch (slot) {
    case 0: return CharSlot::Diffuse;
    case 8: case 9: return CharSlot::FlatGrey;   // a comment with case 5: return CharSlot::Bogus;
    default: return CharSlot::FlatBlack;
  }
}
PsBind CheckPs(int f, bool alpha_test, int pass, std::string& why) {
  for (uint32_t s = 0; s < n; ++s) {
    if (slot == 11) { cube = true; continue; }
  }
}
  f = CubeSibling(f);
CharSlot RoleOfWetSlot(int slot) {
  if (slot == 14) return CharSlot::LiquidMap;
  return RoleOfSlot(slot);
}
CharSlot RoleOfChangeSlot(int slot) {
  const int k = live::change_colour::MaskIndexOfSlot(slot);
  if (k >= 0) return CharSlot(int(CharSlot::ChangeMask1) + k);
  return RoleOfSlot(slot);
}
'''
SYN_CC = 'inline int MaskIndexOfSlot(int slot) { return slot == 7 ? 0 : slot == 2 ? 1 : -1; }\n'
SYN_FRONT = r'''
const Bound& FrontEnd::BindMaterial(ModelData& m, int material) {
      rdr::ImageId image = white_;
      switch (b->change_colour ? RoleOfChangeSlot(slotn) : wet ? RoleOfWetSlot(slotn) : RoleOfSlot(slotn)) {
        case CharSlot::ChangeMask1: case CharSlot::ChangeMask2: {
          image = tex(change_colour_draw::MaskTextureOfSlot(mat, slotn)) != rdr::kNoImage ? black_ : black_;
          image = change_colour_draw::MaskTextureOfSlot(mat, slotn);
          break;
        }
        case CharSlot::LiquidMap: image = black_, b->liquid_slot = uint8_t(slotn); break;
        case CharSlot::Diffuse: image = tex(mat.diffuse) != rdr::kNoImage ? tex(mat.diffuse) : white_; break;
        case CharSlot::FlatGrey: image = grey_; break;
        case CharSlot::FlatBlack: image = black_; break;
        case CharSlot::RimMap: image = black_, b->cube_slot[slotn] = 2; break;
      }
    rdr::ImageId image = white_;
    if (slotn == 0) image = w.diffuse ? tex(mat.diffuse) : white_;
    else if (slotn >= 1 && slotn <= 3) image = tex(mat.layer[slotn - 1]);
    else if (slotn == 12 && w.screen_shadow) image = white_, b->cube_slot[slotn] = 5;
    b->slot_image[slotn] = image;
}
void FrontEnd::Record(int x) {
  const MapImage* liquid = (is_char && m.materials[size_t(me.material)].shader.rfind("yCh_skin", 0) == 0) ? liquid_of(o) : nullptr;
      set.slots[slot].image = b.cube_slot[slot] == 1 ? (cube ? cube->image : b.slot_image[slot]) : (b.cube_slot[slot] == 5 && plane_map) ? plane_map->image : (b.cube_slot[slot] == 2 && b.chr && rim_map) ? rim_map->image : b.slot_image[slot];
      if (liquid_image != rdr::kNoImage && slot == int(b.liquid_slot)) set.slots[slot].image = liquid_image;
}
'''
SYN_JBOY = r'''
        if (pname == "g_mDiffusePrm" || pname == "sampDiffusePrm") slot = &mat.diffuse;
        else if (pname == "sampDiffuse1Prm" && uv_layers) slot = &mat.layer[0];
'''


def part_a():
    r = T.parse_roles(SYN_CHAR, SYN_CC)
    check('A RoleOfSlot cases (a comment is not a case)', {k: v[0] for k, v in r['dry'].items()} == {0: 'Diffuse', 8: 'FlatGrey', 9: 'FlatGrey'}, str(r['dry']))
    check('A RoleOfSlot default', r['dry_default'][0] == 'FlatBlack', str(r['dry_default']))
    check('A RoleOfWetSlot', {k: v[0] for k, v in r['wet'].items()} == {14: 'LiquidMap'}, str(r['wet']))
    check('A RoleOfChangeSlot via MaskIndexOfSlot', {k: v[0] for k, v in r['change'].items()} == {7: 'ChangeMask1', 2: 'ChangeMask2'}, str(r['change']))
    check('A CheckPs cube slot + the twin line', list(r['cube']) == [11] and r['twin_line'] is not None, f"{r['cube']} {r['twin_line']}")
    sw, _ln = T.parse_char_switch(SYN_FRONT)
    check('A switch: grouped cases share one body', sw['ChangeMask1']['cls'] == sw['ChangeMask2']['cls'] and 'change_mask' in sw['ChangeMask1']['cls']['fields'],
          str(sw.get('ChangeMask1')))
    check('A switch: material + fallback', sw['Diffuse']['cls']['fields'] == ['diffuse'] and sw['Diffuse']['cls']['standins'] == ['white_'], str(sw['Diffuse']))
    check('A switch: stand-in only', sw['FlatBlack']['cls'] == {'fields': [], 'standins': ['black_'], 'maps': []}, str(sw['FlatBlack']))
    check('A switch: liquid / map slots', sw['LiquidMap']['cls']['maps'] == ['liquid'] and sw['RimMap']['cls']['maps'] == [2], f"{sw['LiquidMap']} {sw['RimMap']}")
    chains, _v = T.parse_chains(SYN_FRONT)
    check('A one plain chain (the switch loop is not one)', len(chains) == 1 and chains[0]['fn'] == 'BindMaterial', str([c['fn'] for c in chains]))
    arms = chains[0]['arms'] if chains else []
    check('A chain arms: slots / extra', [(a['slots'], a['extra']) for a in arms] == [([0], ''), ([1, 2, 3], ''), ([12], 'w.screen_shadow')],
          str([(a['slots'], a['extra']) for a in arms]))
    rep, liquid = T.parse_draw(SYN_FRONT)
    check('A draw replacement', {k: (v[0], v[1]) for k, v in rep.items()} == {1: ('cube', []), 5: ('plane_map', ['plane_map']), 2: ('rim_map', ['b.chr', 'rim_map'])},
          str(rep))
    check('A liquid gate prefixes + replacement line', liquid['prefixes'] == ['yCh_skin'] and liquid['replace_line'] is not None, str(liquid))
    names = T.parse_jboy(SYN_JBOY)
    check('A loader names', names == {'diffuse': ['g_mDiffusePrm', 'sampDiffusePrm'], 'layer[0]': ['sampDiffuse1Prm']}, str(names))
    ours = {'roles': r, 'switch': sw, 'chains': chains, 'verts': [], 'replace': rep, 'liquid': liquid, 'names': names}
    gw = T.ours_char('yCh_gear', 14, [('blood', frozenset(['FX_BLOOD']), frozenset([0, 14]))], ours)
    sk = T.ours_char('yCh_skin', 14, [('bss', frozenset(['FX_BSS']), frozenset([0, 14]))], ours)
    check('A the liquid map reaches yCh_skin only', gw[0]['kind'] == 'STANDIN' and sk[0]['kind'] == 'MAP', f'{gw} {sk}')
    l2 = T.ours_front('yBG_UVScroll', 'ps', 2, [('uv3', frozenset(), frozenset([0, 2]))], ours)
    check('A a range arm binds layer[slot - 1]', l2[0]['fields'] == ['layer[1]'], str(l2))
    l12 = T.ours_front('yBG_Default', 'ps', 12, [('shadow', frozenset(['FX_SHADOW']), frozenset([0, 12]))], ours)
    check('A a conditional arm leaves the white default, listed', l12[0].get('default') and l12[0]['cond'] and l12[0]['cond'][0]['kind'] == 'MAP', str(l12))
    g = {'kind': 'GAME-MATERIAL', 'rows': [{'name': 'g_mMusclePrm', 'kind': 'GAME-MATERIAL'}]}
    check('A judge: stand-in vs material = DIFF', T.judge({'kind': 'STANDIN', 'standins': ['black_']}, g, names)[0] == 'DIFF')
    check('A judge: other name = DIFF (ROLE)', T.judge({'kind': 'MATERIAL', 'fields': ['diffuse'], 'standins': []}, g, names)[1].startswith('ROLE'))
    check('A judge: stand-in vs NOREC = UNSETTLED', T.judge({'kind': 'STANDIN', 'standins': ['black_']}, {'kind': 'GAME-NOREC', 'rows': []}, names)[0] == 'UNSETTLED')
    dead = {'kind': 'MATERIAL', 'fields': ['hair_noise'], 'standins': ['grey_']}
    check('A judge: a field no name loads and no line writes = its stand-in (DIFF)', T.judge(dead, g, names, {'hair_noise': []})[0] == 'DIFF')
    check('A judge: a field code fills = OK', T.judge(dead, g, names, {'hair_noise': ['src/x.h:1']})[0] == 'OK')
    check('A judge: NOUSE / UNSAMPLED = UNUSED', T.judge({'kind': 'STANDIN', 'standins': ['grey_']}, {'kind': 'UNSAMPLED', 'rows': [], 'why': 'w'}, names)[0] == 'UNUSED')
    early = SYN_CHAR.replace('  switch (slot) {\n    case 0:', '  if (slot == 0) return CharSlot::FlatBlack;\n  switch (slot) {\n    case 0:', 1)
    try:
        T.parse_roles(early, SYN_CC)
        bad = None
    except T.TexDiffError as e:
        bad = str(e)
    check('A an early return in RoleOfSlot is a PROBLEM (never a role read past it)', bad is not None and early != SYN_CHAR, bad or 'accepted')


# ------------------------------------------------------------------ B / C. real data
def frozen_reader(root, edits=None, ref=FROZEN):
    """our code at ref (None = the working tree under root), with planted edits."""
    cache = {}

    def read(p):
        if p not in cache and ref is None:
            with open(os.path.join(root, p), encoding='utf-8') as fh:
                cache[p] = fh.read()
        if p not in cache:
            cache[p] = subprocess.run(['git', '-C', root, 'show', f'{ref}:{p}'], capture_output=True, text=True, encoding='utf-8',
                                      check=True).stdout
        s = cache[p]
        for (ep, old, new) in (edits or []):
            if ep == p:
                if s.count(old) != 1:
                    raise SystemExit(f'plant anchor not unique in {p}: {old}')
                s = s.replace(old, new)
        return s
    return read


def row(rows, eff, slot, stage='ps'):
    hits = [r for r in rows if r['effect'] == eff and r['stage'] == stage and r['slot'] == slot]
    return hits[0] if hits else None


def expect(rows, eff, slot, ctx, flag, at=(), game=None):
    """None when (eff, slot)'s `ctx` binding has `flag` and names every `at`; else the reason."""
    r = row(rows, eff, slot)
    if r is None:
        return f'no {eff} ps s{slot} row'
    bs = [b for b in r['ours'] if b['ctx'] == ctx]
    if not bs:
        return f"{eff} s{slot}: no {ctx} binding ({[b['ctx'] for b in r['ours']]})"
    b = bs[0]
    if b['flag'] != flag:
        return f"{eff} s{slot} {ctx}: {b['flag']} ({b['why']}), want {flag}"
    miss = [a for a in at if a not in b['at']]
    if miss:
        return f"{eff} s{slot} {ctx}: {b['at']} lacks {miss}"
    if game and [(g['name'], g['kind']) for g in r['game']['rows']] != [game]:
        return f"{eff} s{slot}: game rows {[(g['name'], g['kind']) for g in r['game']['rows']]}, want {[game]}"
    return None


def no_diff(rows, eff, slot):
    r = row(rows, eff, slot)
    if r is None:
        return f'no {eff} ps s{slot} row'
    bad = [f"{b['ctx']}: {b['why']}" for b in r['ours'] if b['flag'] == 'DIFF']
    return f'{eff} s{slot} DIFF: {bad}' if bad else None


def part_bc(root):
    sys.path.insert(0, os.path.join(root, 'tools', 'q'))
    try:
        import q as qq
        con = qq.connect(qq.DB)
        table, _desc = T.CD.load_game(con)
    except Exception as e:  # noqa: BLE001
        print(f'SKIP B / C (no q index or generated listing: {e})')
        return
    names, totals, techs = T.sampler_names()
    if names is None:
        print('SKIP B / C (no asset index: GAME-MATERIAL cannot be told)')
        return
    fx = os.path.join(root, 'src', 'renderer', 'shaders', 'fx')
    try:
        sys.path.insert(0, os.path.join(root, 'tools', 'shaders'))
        import effect_table
        tables = effect_table.load_all()
    except Exception as e:  # noqa: BLE001
        print(f'SKIP B / C (the player\'s effect files were not read: {e})')
        return
    flags, ferr = T.CD.load_pass_flags(con)
    check('B the model draw\'s pass flags read (sub_82BDA038)', flags is not None, str(ferr))
    treads, terr = T.technique_reads(fx, flags, tables)
    if treads is None:
        print(f'SKIP B / C (no technique table: {terr})')
        return
    sr = {r['name']: r for r in table['rows'] if r['type'] in T.SAMPLER_TYPES}
    mu, bs = sr.get('g_mMusclePrm'), sr.get('g_mBSSPrm')
    check('B name table: sampler rows read', len(sr) >= 25, str(len(sr)))
    check('B g_mMusclePrm = slot 2, row 0x8392AB00 @0x82BACDE8', mu and (mu['reg'], mu['addr'], mu['site']) == (2, 0x8392AB00, 0x82BACDE8), str(mu))
    check('B g_mBSSPrm = slot 14, row 0x8392AC80 @0x82BAD070', bs and (bs['reg'], bs['addr'], bs['site']) == (14, 0x8392AC80, 0x82BAD070), str(bs))
    samplers = T.load_samplers(os.path.join(root, 'src', 'renderer', 'shaders', 'fx'))
    cache = {}
    refs_of = lambda n: cache.setdefault(n, T.code_refs(con, n))

    grep = T.default_grep(FROZEN)

    def rows_of(edits=None, g=None, ref=FROZEN):
        """g = (names, totals, techs, treads) to plant the game side; ref None = the working tree (the tip); a PROBLEM the parse
        raises comes back as the problems."""
        try:
            ours, probs = T.load_ours(frozen_reader(root, edits, ref), grep if ref else T.default_grep())
        except T.TexDiffError as e:
            return [], [str(e)]
        nm, tt, tc, tr = g or (names, totals, techs, treads)
        rows, _sk = T.build_rows(samplers, ours, table, nm, tt, refs_of, tc, tr)
        return rows, probs

    rows, probs = rows_of()
    check('B frozen code parses with no problem', probs == [], str(probs))
    case1 = lambda rs: expect(rs, 'yCh_skin_flex', 2, 'dry', 'DIFF', (f'{CHAR}:120', f'{FRONT}:1424'), ('g_mMusclePrm', 'GAME-MATERIAL'))
    case2 = lambda rs: expect(rs, 'yCh_gear', 14, 'wet', 'UNSETTLED', (f'{FRONT}:1408', f'{FRONT}:2700'), ('g_mBSSPrm', 'GAME-NOREC'))
    m = case1(rows)
    check('B RE-FOUND 1: yCh_skin_flex slot 2 (g_mMusclePrm) DIFF, black at front_end.cpp:1424', m is None, m or '')
    m = case2(rows)
    check('B RE-FOUND 2: yCh_gear slot 14 (g_mBSSPrm) UNSETTLED, black at front_end.cpp:1408 behind :2700', m is None, m or '')
    m = no_diff(rows, 'yCh_skin', 0)
    check('B yCh_skin slot 0 (the material diffuse) is not DIFF', m is None, m or '')
    m = expect(rows, 'yCh_skin', 14, 'wet', 'OK')
    check('B yCh_skin slot 14 wet = the liquid map, OK', m is None, m or '')
    d5 = lambda rs: expect(rs, 'yDefault', 1, 'BindMaterial', 'UNUSED')
    m = d5(rows)
    check('B the D5 false alarm: yDefault slot 1 (texSpecularMap) UNUSED, no record\'s technique samples it', m is None, m or '')
    an = lambda rs: expect(rs, 'yCh_Anisotropic', 8, 'dry', 'UNUSED')
    m = an(rows)
    check('B yCh_Anisotropic slot 8 UNUSED (no record names the effect)', m is None, m or '')
    m = expect(rows, 'yCh_Aniso_trans', 8, 'dry', 'DIFF', (f'{FRONT}:1423',), ('g_mHairNoisePrm', 'GAME-MATERIAL'))
    check('B D2: yCh_Aniso_trans slot 8 (g_mHairNoisePrm) DIFF, grey at front_end.cpp:1423', m is None, m or '')
    m = expect(rows, 'yCh_Aniso_trans', 9, 'dry', 'DIFF', (f'{FRONT}:1423',), ('g_mHairShiftPrm', 'GAME-MATERIAL'))
    check('B D3: yCh_Aniso_trans slot 9 (g_mHairShiftPrm) DIFF', m is None, m or '')
    m = expect(rows, 'yCh_trans', 10, 'dry', 'DIFF', (f'{FRONT}:1427',))
    tr10 = row(rows, 'yCh_trans', 10)
    env = [g_ for g_ in (tr10 or {}).get('game', {}).get('rows', []) if g_['name'] == 'g_mEnvLPrm']
    txt = env[0]['kinds'][0][1] if env else ''
    check('B D4: yCh_trans slot 10 DIFF on the technique that samples it only (the others counted apart)',
          m is None and 'with a technique that samples s10' in txt and 'never samples s10' in txt, (m or '') + ' ' + txt)
    m = expect(rows, 'yCh_Aniso_trans', 4, 'dry', 'OK')
    check('B yCh_Aniso_trans slot 4 (mat.logo4, filled by code) OK', m is None, m or '')
    m = expect(rows, 'yDefault', 12, 'BindMaterial', 'UNUSED')
    check('B the pass cut (S111 CONSTPASS): yDefault slot 12 UNUSED, ydefault sets no shadow word so no shadow pass is drawn', m is None, m or '')
    # the tip (the working tree): S111 MUSCLEMAP's muscle_map::Role in front of the role chain; skipped on a tree that predates it
    trows, tprobs = rows_of(ref=None)
    mus = os.path.exists(os.path.join(root, T.MUSCLE))
    if mus:
        check('B tip parses with no problem (the muscle_map::Role switch)', tprobs == [], str(tprobs))
        m = expect(trows, 'yCh_skin_flex', 2, 'dry+muscle', 'OK', (f'{T.MUSCLE}:{T.parse_muscle(frozen_reader(root, ref=None)(T.FRONT), frozen_reader(root, ref=None)(T.MUSCLE))["line"]}',),
                   ('g_mMusclePrm', 'GAME-MATERIAL'))
        check('B tip: D1 yCh_skin_flex slot 2 OK through muscle_map (mat.muscle from g_mMusclePrm)', m is None, m or '')
        m = expect(trows, 'yCh_skin_flex', 2, 'change', 'OK')
        check('B tip: a change-colour bind keeps slot 2 as Mask4 (not the muscle map)', m is None, m or '')
        if os.path.exists(os.path.join(root, 'src', 'renderer_scene', 'env_cube.h')):   # S111 ENVCUBE: slot 10 = the material's g_mEnvLPrm cube (mat.env_cube)
            for eff in ('yCh_Reflect', 'yCh_trans', 'yCh_TransTiling'):
                for ctx in ('dry', 'wet'):
                    m = expect(trows, eff, 10, ctx, 'OK')
                    check(f'B tip: D4 {eff} slot 10 {ctx} OK through env_cube (mat.env_cube from g_mEnvLPrm)', m is None, m or '')
    else:
        print('SKIP B tip checks (no muscle_map.h in this tree)')

    rs, _p = rows_of([(FRONT, 'case CharSlot::FlatBlack: image = black_; break;', 'case CharSlot::FlatBlack: image = tex(mat.muscle); break;'),
                      (T.JBOY, 'else if (pname == "g_mMaskPrm") slot = &mat.mask;',
                       'else if (pname == "g_mMaskPrm") slot = &mat.mask;\n        else if (pname == "g_mMusclePrm") slot = &mat.muscle;')])
    twin('C the FlatBlack case bound to a material field, muscle still DIFF', case1(rs))
    rs, _p = rows_of([(FRONT, 'shader.rfind("yCh_skin", 0) == 0 && (!char_b', 'shader.rfind("yCh_", 0) == 0 && (!char_b')])
    twin('C the liquid gate widened to every yCh_, gear slot 14 still UNSETTLED', case2(rs))
    rs, _p = rows_of([(FRONT, 'case CharSlot::Diffuse: image = tex(mat.diffuse) != rdr::kNoImage ? tex(mat.diffuse) : white_; break;',
                       'case CharSlot::Diffuse: image = white_; break;')])
    twin('C the Diffuse case bound to white, no new DIFF at slot 0', no_diff(rs, 'yCh_skin', 0))
    rs, _p = rows_of([(CHAR, 'case 0: return CharSlot::Diffuse;', 'case 0: return CharSlot::Normal;')])
    twin('C slot 0 role made Normal, no ROLE DIFF at slot 0', no_diff(rs, 'yCh_skin', 0))
    _r, p3 = rows_of([(CHAR, 'case 1: return CharSlot::Normal;', 'case 1: return CharSlot::NoSuchRole;')])
    twin('C a role with no case in the switch accepted', p3[0] if p3 else None)
    _r, p4 = rows_of([(CHAR, 'CharSlot RoleOfSlot(int slot) {', 'CharSlot RoleOfSlot(int slot) { if (slot == 0) return CharSlot::FlatBlack;')])
    twin('C an early return in RoleOfSlot accepted (slot 0 silently OK)', p4[0] if p4 else None)
    rs, _p = rows_of([(FRONT, 'case CharSlot::FlatGrey: image = grey_; break;',
                       'case CharSlot::FlatGrey: image = tex(mat.hair_noise) != rdr::kNoImage ? tex(mat.hair_noise) : grey_; break;')])
    twin('C slot 8 bound to a field nothing fills, judged OK', expect(rs, 'yCh_Aniso_trans', 8, 'dry', 'OK'))
    yd = dict(treads.get('ydefault', {}))
    yd[1] = frozenset([0, 1])
    tr2 = dict(treads)
    tr2['ydefault'] = yd
    rs, _p = rows_of(g=(names, totals, techs, tr2))
    twin('C yDefault technique 1 made to read slot 1, slot 1 still UNUSED', d5(rs))
    nm2 = dict(names, yCh_Anisotropic={'g_mHairNoisePrm': {'n': 1, 'tex': {'hair_noise': 1}, 'tech': {0: {'hair_noise': 1}}}})
    rs, _p = rows_of(g=(nm2, dict(totals, yCh_Anisotropic=1), dict(techs, yCh_Anisotropic={0: 1}), treads))
    twin('C a yCh_Anisotropic record naming g_mHairNoisePrm, slot 8 still UNUSED', an(rs))
    f2 = {w: set(v) for w, v in flags.items()}
    f2.setdefault(4, set()).add('ydefault')
    rs, _p = rows_of(g=(names, totals, techs, T.technique_reads(fx, f2, tables)[0]))
    twin('C ydefault planted into the shadow word, slot 12 still UNUSED', expect(rs, 'yDefault', 12, 'BindMaterial', 'UNUSED'))
    if mus:
        rs, _p = rows_of([(T.FRONT, 'case CharSlot::MuscleMap: image = muscle_map::Image(mat, tex, rdr::kNoImage, black_); break;',
                           'case CharSlot::MuscleMap: image = black_; break;')], ref=None)
        twin('C tip: the MuscleMap case bound to black, D1 still OK', expect(rs, 'yCh_skin_flex', 2, 'dry+muscle', 'OK'))
        _r, p5 = rows_of([(T.MUSCLE, 'if (muscle && !change_colour && slot == kSlot) return CharSlot::MuscleMap;',
                           'if (muscle && !change_colour && slot == kSlot) return CharSlot::MuscleMap;\n  if (slot == 0) return CharSlot::FlatBlack;')], ref=None)
        twin('C tip: an extra statement in muscle_map::Role accepted', p5[0] if p5 else None)
        _r, p6 = rows_of([(T.FRONT, 'switch (muscle_map::Role(slotn, b->change_colour, wet, reads_muscle)) {', 'switch (muscle_map::Role(slotn, b->change_colour, wet, true)) {')], ref=None)
        twin('C tip: an unknown head of the character switch accepted', p6[0] if p6 else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(HERE)))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    T.REPO = root
    T.FX_DIR = os.path.join(root, 'src', 'renderer', 'shaders', 'fx')
    part_a()
    part_bc(root)
    print(f'{len(FAILS)} failed, {len(CAUGHT)} planted twins not caught')
    return 2 if CAUGHT else 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
