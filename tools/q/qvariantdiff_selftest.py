r"""Self-test of `q variantdiff` (S111 VARIANTDIFF): `py -I tools/q/qvariantdiff_selftest.py [--root DIR]`   (0 game runs)

A. Synthetic: the slot-set reader (parse_known), the game's pass rule (game_pass) on invented flags, the cube twin (twin_of) on an
   invented registry. Each must read back as written.
B. Real data (needs the q index with the generated listing, the player's pac\_startup.pac and the asset index; else SKIP), our code
   frozen at FROZEN (the S111 tip the cases were stated against), the cases RE-FOUND:
     1. the cube-twin substitution (S111 CONSTPASS review): yCh_TransTiling T0 / yCh_cloth_trans T0 draw p1 `cube_specular_fragment`
        in the game in every context (no entry +12 / +4 name: sub_82BDA038), ours binds the twin `base_fragment` through the table
        pick (char_effects.cpp:295 CubeSibling): HABIT, never DIFF / HOLE;
     3. the table pick (our only pick since S111 OLDCHAIN) has no DIFF and no HOLE at FROZEN;
     5. the hair class's shadow bit: yCh_trans T0 nospec in `m0 shadow` is UNSETTLED (ours p9 by !cl.hair, the game's row p11);
     6. (review) the editor tables edit.pac p7/0 / p9/0 bind on the table pick for every group and context.
   (2 and 4, the old chain's shadow and wet leads, went with the old chain: S111 OLDCHAIN.)
C. Planted twins on the same frozen text / game data, each must be CAUGHT (the check it targets must fail on it): CubeSibling removed
   (1 fails: HOLE), the game's E+4 / E+20 names given to yCh_cloth_trans (3 fails: a DIFF on the table path), the hair class's
   !cl.hair dropped (5 fails), a shape change that must be a PROBLEM (VariantDiffError): CharPassOf's +2 made +3; (review)
   CheckPs feeding no slot 7..10 (6 fails: the logo rows stop binding).
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qvariantdiff as V   # noqa: E402

FROZEN = 'ff0ee241c'      # the S111 tip the cases were stated against (char_effects.cpp:286 / :295, front_end.cpp:2662); its table pick is the one S111 OLDCHAIN keeps
FAILS, CAUGHT = [], []


def check(name, msg):
    """msg = None when the case holds, else why it does not."""
    if msg:
        FAILS.append(f'{name}: {msg}')
        print(f'FAIL {name}: {msg}')
    else:
        print(f'ok   {name}')


def twin(name, msg):
    """msg = the checker's failure on a planted case; None = the checker accepted it (a defect)."""
    if msg is None:
        CAUGHT.append(name)
        print(f'NOT CAUGHT planted twin: {name}')
    else:
        print(f'ok   planted twin caught: {name} ({msg})')


# ------------------------------------------------------------------ A. synthetic
def part_a():
    check('A parse_known', None if V.parse_known('(slot >= 0 && slot <= 3) || slot == 12 || slot == 15') == {0, 1, 2, 3, 12, 15} else 'wrong set')
    fl = {12: {'ych_a'}, 4: {'ych_a', 'ych_b'}, 20: {'ych_b'}}
    got = [V.game_pass('yCh_a', fl, 0, True, False, False)[0], V.game_pass('yCh_a', fl, 0, False, False, False)[0],
           V.game_pass('yCh_a', fl, 2, True, True, False)[0], V.game_pass('yCh_b', fl, 2, False, True, True)[0],
           V.game_pass('yCh_c', fl, 2, False, True, True)[0]]
    check('A game_pass', None if got == [1, 9, 11, 19, 1] else f'got {got}, want [1, 9, 11, 19, 1]')
    mk = lambda n, d, s: {'full': f'yCh_x_{n}_fragment', 'defines': frozenset(d), 'slots': s}
    frs = [mk('base', [], {0: '2d'}), mk('cube_specular', ['FX_CUBE_SPECULAR'], {0: '2d', 11: 'cube'}), mk('shadow', ['FX_SHADOW'], {12: '2d'})]
    reg = {'yCh_x': {'fragment': frs, 'vertex': [], 'by_name': {f['full']: f for f in frs}}}
    check('A twin_of', None if V.twin_of(reg, 'yCh_x', frs[1]) is frs[0] and V.twin_of(reg, 'yCh_x', frs[2]) is None else 'wrong twin')
    # the hair class's shadow contexts (review): UNSETTLED only when ours is exactly the game's row without the +2 term (or its cube twin);
    # any other variant without FX_SHADOW is a DIFF (the unfixed judge called every such variant UNSETTLED)
    env = mk('env_light', ['FX_ENV_LIGHT'], {0: '2d'})
    hreg = {'yCh_x': {'fragment': frs + [env], 'vertex': [], 'by_name': {f['full']: f for f in frs + [env]}}}
    hr = {'hair': {'ych_x'}, 'hair_noshadow': True, 'at': {'pass_call': 'front_end.cpp:1'}}
    hctx = {'shadow': True, 'm620': False}
    got = [V.judge('yCh_x', hctx, 11, (None, frs[2]['full']), {'ok': True, 'ps': ops, 'vs': None}, hreg, hr, grow_ns=(None, ns['full']))[0]
           for ops, ns in ((frs[0], frs[0]), (env, frs[0]), (frs[0], frs[1]))]
    check('A judge hair: the p-2 row / another variant / the p-2 cube twin', None if got == ['UNSETTLED', 'DIFF', 'UNSETTLED'] else f'got {got}, want [UNSETTLED, DIFF, UNSETTLED]')


# ------------------------------------------------------------------ B / C. real data
def frozen_reader(root, edits=None, ref=FROZEN):
    cache = {}

    def read(p):
        if p not in cache:
            cache[p] = subprocess.run(['git', '-C', root, 'show', f'{ref}:{p}'], capture_output=True, text=True, encoding='utf-8', check=True).stdout
        s = cache[p]
        for (ep, old, new) in (edits or []):
            if ep == p:
                if s.count(old) != 1:
                    raise SystemExit(f'plant anchor not unique in {p}: {old}')
                s = s.replace(old, new)
        return s
    return read


def group(rows, eff, tech, spec=None):
    return [x for x in rows if x['effect'] == eff and x['technique'] == tech and (spec is None or x['spec_pow'] == spec)]


def c1_twin(rows):
    for eff in ('yCh_TransTiling', 'yCh_cloth_trans'):
        gs = group(rows, eff, 0)
        if not gs:
            return f'no {eff} T0 group'
        for g in gs:
            for c in g['contexts']:
                if c['game_ps'] != f'{eff}_cube_specular_fragment' or c['game_pass'] != 1:
                    return f"{eff} {c['ctx']}: game p{c['game_pass']} {c['game_ps']}, want p1 cube_specular_fragment"
                if c['flag'] != 'HABIT' or c['ours_ps'] != f'{eff}_base_fragment':
                    return f"{eff} {c['ctx']}: {c['flag']} ours {c['ours_ps']} ({c['why']}), want HABIT base_fragment"
    return None


def c3_clean(rows):
    bad = [f"{x['effect']} T{x['technique']} {c['ctx']}: {c['flag']} {c['why']}" for x in rows for c in x['contexts'] if c['flag'] in ('DIFF', 'HOLE')]
    return f'{len(bad)} DIFF / HOLE contexts, first: {bad[0]}' if bad else None


def c5_hair(rows):
    gs = group(rows, 'yCh_trans', 0, False)
    cs = [c for c in gs[0]['contexts'] if c['ctx'] == 'm0 shadow'] if gs else []
    if not cs or cs[0]['flag'] != 'UNSETTLED' or cs[0]['ours_pass'] != 9 or cs[0]['game_pass'] != 11:
        return f"yCh_trans T0 nospec m0 shadow: {cs[0]['flag'] + ' ours p' + str(cs[0]['ours_pass']) + ' game p' + str(cs[0]['game_pass']) if cs else 'missing'}, want UNSETTLED ours p9 / game p11"
    return None


CASES = (('B1 the cube twin is HABIT', c1_twin), ('B3 the table pick has no DIFF / HOLE', c3_clean),
         ('B5 the hair shadow bit is UNSETTLED', c5_hair))


def part_bc(root):
    sys.path.insert(0, os.path.join(root, 'tools', 'q'))
    try:
        import q as qq
        con = qq.connect(qq.DB)
        flags, tables, groups = V.load_game(con)
    except Exception as e:  # noqa: BLE001
        print(f'SKIP B / C (no q index, listing, effect file or asset index: {e})')
        return
    fx = os.path.join(root, 'src', 'renderer', 'shaders', 'fx')

    def rows_of(edits=None, fl=None):
        rows, _r, problems = V.compute(frozen_reader(root, edits), fl or flags, tables, groups, fx)
        return rows, problems

    rows, problems = rows_of()
    check('B no PROBLEM at FROZEN', '; '.join(problems) or None)
    for name, fn in CASES:
        check(name, fn(rows))
    cs = V.CHAR
    fe = V.FRONT
    tp = V.TP
    plants = (
        ('C1 CubeSibling removed', c1_twin, [(cs, 'const int sib = CubeSibling(f);', 'const int sib = -1;')], None),
        ('C3 the game gives yCh_cloth_trans E+4', c3_clean, None, 'plant_flags'),
        ('C7 a pass cap of 0 in the table pick (review: the tool models char_effects.cpp\'s pass guard)', c3_clean,
         [(cs, 'if (pass < 0 || pass > 11) return', 'if (pass < 0 || pass > 0) return')], None),
        ('C5 the hair class drawn with the shadow bit', c5_hair,
         [(fe, 'technique_pick::CharPassOf(cl, objs[it.object].skin != nullptr, shadow_map && !cl.hair, mode, cm.spec_pow_prm)',
           'technique_pick::CharPassOf(cl, objs[it.object].skin != nullptr, shadow_map, mode, cm.spec_pow_prm)')], None),
    )
    for name, fn, edits, how in plants:
        fl = None
        if how == 'plant_flags':
            fl, err = V.CD.load_pass_flags(con, plant={'ych_aniso_trans': 'ych_cloth_trans'})
            if fl is None:
                twin(name, None)
                print(f'   (the planted flags were not read: {err})')
                continue
        prow, _p = rows_of(edits, fl)
        twin(name, fn(prow))
    for name, edits in (('C6 CharPassOf +2 made +3', [(tp, 'if (c.e4 && shadow_ok) pass += 2;', 'if (c.e4 && shadow_ok) pass += 3;')]),):
        try:
            rows_of(edits)
            twin(name, None)
        except V.VariantDiffError as e:
            twin(name, str(e)[:120])
    # B6 / C8 (review): the editor tables (edit.pac p7/0 logo rows, p9/0 change-colour rows: a CAW logo material / a recoloured body
    # draws from them, char_effects.cpp:275-282) bind for every group and context at FROZEN; a CheckPs that no longer feeds slots 7..10
    # (the logo layer reads slot 7 / 4) must leave rows unbound
    editor, eerr = V.load_editor_tables()
    if editor is None:
        print(f'SKIP B6 / C8 (no {V.EDIT}: {eerr})')
        return

    def c6_editor(edits=None):
        r, _p = V.parse_ours(frozen_reader(root, edits))
        ed = V.editor_picks(groups, editor, V.load_registry(fx), r, flags)
        bad = [x for x in ed if not x['ok']]
        if not ed:
            return 'no editor table context evaluated'
        return f"{len(bad)} of {len(ed)} editor contexts do not bind, first: {bad[0]['leaf']} {bad[0]['effect']} T{bad[0]['technique']} {bad[0]['ctx']}: {bad[0]['why']}" if bad else None
    check('B6 the editor tables bind on the table pick', c6_editor())
    twin('C8 CheckPs feeds no slot 7..10', c6_editor([(cs, 'const bool known = (slot >= 0 && slot <= 10) ||', 'const bool known = (slot >= 0 && slot <= 6) ||')]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--root', default=os.path.dirname(os.path.dirname(HERE)))
    a = ap.parse_args()
    root = os.path.abspath(a.root)
    part_a()
    part_bc(root)
    print(f'{len(FAILS)} failed, {len(CAUGHT)} planted twins not caught')
    return 2 if CAUGHT else 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
