r"""Self-test of `q targetfmt` (S111 TARGETFMT): `py -I tools/q/qtargetfmt_selftest.py`   (0 game runs)

A. Format words (invented and real): 0x18280186 k_8_8_8_8 (as written), the same word with gamma signs on X / Y / Z
   (k_8_8_8_8_GAMMA: blends linear), 0x1828014F k_4_4_4_4 (no colour-target form), 0x2DA2ABA4 k_32_FLOAT, 0x1A20AB55
   k_16_16_16_16_EDRAM (fixed), format 62 / 63 (the EDRAM gamma / 7e3 forms).
B. Planted roles on an invented tree (a dict reader; no game code): our function found by its definition only (a call and a comment
   naming it are not), its variable's format, a depth copy skipped; the quantise step counted only when its variant is looked up
   and its id read again; a quantise line under #else needs the #if's define absent. Verdicts: a 4-bit destination beside an 8-bit
   image with the step OK and without it DIFF (LIQUID4BIT's shape); a gamma surface beside RGBA8 a blend_space DIFF; a 7e3 surface a
   target_format DIFF; a site with no constant UNSETTLED; two game formats against one image UNSETTLED; a DIFF with a DEPARTS
   marker on our format line DEPARTED.
C. Real data (needs the q index; else SKIP): the LIQUID4BIT RE-FIND. At its parent PARENT the sweat map's precision is a DIFF
   (game k_4_4_4_4, ours RGBA8); at FROZEN (the S111 tip this was built on) it is OK through yLiquidMap_copy_prev_q4. The scene
   colour's three fields are OK at both (game k_8_8_8_8 blends as written; ours RGBA8 the same).
D. Planted twins, each must be CAUGHT (a check above must fail under it): no_dest (precision read from the surface), no_gamma (the
   sign bits ignored), no_quant (our quantise step not read), quant_unused (a looked-up variant counted without its id read again).
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qtargetfmt as T   # noqa: E402

PARENT = 'a59900d3d^1'    # the LIQUID4BIT merge's first parent (the 8-bit sweat map)
FROZEN = 'cf927d4e3'      # the S111 tip this lane was cut from
FAILS, NOT_CAUGHT = [], []


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')
    return bool(cond)


# ------------------------------------------------------------------ A. format words
def part_a():
    d = T.decode(0x18280186)
    check('A 0x18280186 k_8_8_8_8', (d['name'], d['kind'], d['bits'], d['ch'], d['rt'], d['gamma']) == ('k_8_8_8_8', 'unorm', 8, 4, 'k_8_8_8_8', False), d)
    d = T.decode(0x18280186 | 0x7E00)
    check('A gamma signs X/Y/Z -> k_8_8_8_8_GAMMA', d['rt'] == 'k_8_8_8_8_GAMMA' and d['gamma'], d)
    check('A gamma blends linear', T.g_val('blend_space', d, []).startswith('linear'), d)
    d = T.decode(0x1828014F)
    check('A 0x1828014F k_4_4_4_4, resolve only', d['name'] == 'k_4_4_4_4' and d['bits'] == 4 and d['rt'] is None, d)
    d = T.decode(0x2DA2ABA4)
    check('A 0x2DA2ABA4 k_32_FLOAT', (d['name'], d['kind'], d['bits'], d['ch']) == ('k_32_FLOAT', 'float', 32, 1), d)
    d = T.decode(0x1A20AB55)
    check('A 0x1A20AB55 k_16_16_16_16_EDRAM fixed', (d['name'], d['kind'], d['rt']) == ('k_16_16_16_16_EDRAM', 'fixed', 'k_16_16_16_16'), d)
    d62, d63 = T.decode(62), T.decode(63)
    check('A format 62 gamma, 63 7e3', d62['gamma'] and d62['rt'] == 'k_8_8_8_8_GAMMA' and d63['kind'] == 'float7e3', (d62, d63))
    d = T.decode(0x3D)
    check('A an undecoded number has no name', d['name'] is None, d)


# ------------------------------------------------------------------ B. planted roles on an invented tree
SRC = r'''
// Foo::Make(int w) in a comment is not a definition
void Other() { Foo::Make(3); }
rdr::ImageId Foo::Make(int w) {
  rdr::ImageDesc c;
  c.width = uint16_t(w), c.format = rdr::Format::RGBA8;
  rdr::ImageDesc d = c;
  d.format = rdr::Format::Depth;
  s.fs_q = md::FindVariant("yEff_copy_q_fragment");
  s.fs_dead = md::FindVariant("yEff2_copy_q_fragment");
  return Draw(kVariants[s.fs_q]);
}
void Bar::Make() {
  rdr::ImageDesc c;
  c.format = rdr::Format::RGBA8;   // DEPARTS: habit 360_HABITS:15; a planted departure with its reason
}
'''
FRAG = r'''
void main() {
#ifdef FX_Q
  v = floor(v * 15.0 + 0.5) / 15.0;
#else
  v = round(v * 255.0) / 255.0;
#endif
}
'''
JSON = '{"effect": "%s", "fragment": [{"name": "base", "defines": []}, {"name": "copy_q", "defines": ["FX_Q"]}]}'
TREE = {'src/x/a.cpp': SRC, 'src/renderer/shaders/fx/yEff.frag': FRAG, 'src/renderer/shaders/fx/yEff.fx.json': JSON % 'yEff',
        'src/renderer/shaders/fx/yEff2.frag': FRAG, 'src/renderer/shaders/fx/yEff2.fx.json': JSON % 'yEff2',
        'docs/renderer/360_HABITS.md': '| # | habit |\n|---|---|\n| 15 | a planted habit row |\n'}


def read(rel):
    if rel not in TREE:
        raise OSError(f'{rel}: not in the planted tree')
    return TREE[rel]


def site(v, creator='surface'):
    return {'fn': 'sub_82000000', 'who': 'sub_82000000+10', 'creator': creator, 'reg': 'r5' if creator == 'surface' else 'r8',
            'value': v, 'text': f'0x{v:08X}' if v is not None else '[in_r3+12]', 'size': '64 x 64'}


SITES = {1: site(0x18280186), 2: site(0x1828014F, 'texture'), 3: site(0x18280186 | 0x7E00), 4: site(63), 5: site(None),
         6: site(0x1A20AB55)}
OURS = [('src/x/a.cpp', 'Foo::Make', 'c')]
ROLES = {
    'q4_step': dict(role='q4_step', surface=[1], dest=[2], ours=OURS, fx=['yEff'], pair='C', why='planted'),
    'q4_none': dict(role='q4_none', surface=[1], dest=[2], ours=OURS, fx=[], pair='C', why='planted'),
    'q4_unused': dict(role='q4_unused', surface=[1], dest=[2], ours=OURS, fx=['yEff2'], pair='C', why='planted: id never read again'),
    'gamma': dict(role='gamma', surface=[3], dest=[], ours=OURS, fx=[], pair='C', why='planted'),
    'float7e3': dict(role='float7e3', surface=[4], dest=[], ours=OURS, fx=[], pair='C', why='planted'),
    'noconst': dict(role='noconst', surface=[5], dest=[], ours=OURS, fx=[], pair='C', why='planted'),
    'several': dict(role='several', surface=[1, 6], dest=[], ours=OURS, fx=[], pair='C', why='planted'),
    'departed': dict(role='departed', surface=[4], dest=[], ours=[('src/x/a.cpp', 'Bar::Make', 'c')], fx=[], pair='C', why='planted'),
}


def verdicts(role, plant=''):
    T.PLANT = plant
    try:
        import qcodediff
        marks = qcodediff.Marks(read)
        rows, _facts, problems = T.role_rows(ROLES[role], SITES, read, marks)
    finally:
        T.PLANT = ''
    return {r['field']: r['verdict'] for r in rows}, problems, rows


def part_b():
    hit = T.find_function(SRC, 'Foo::Make')
    ok = hit is not None and 'rdr::ImageDesc c;' in hit[0][hit[1]:hit[2]]
    check('B the definition found, not the call or the comment', ok)
    got, p = T.ours_formats(read, 'src/x/a.cpp', 'Foo::Make', 'c')
    check('B the variable\'s format, the depth copy not counted', p is None and [g['format'] for g in got] == ['RGBA8'], (got, p))
    ql = T.quant_lines(FRAG)
    check('B quantise lines with their branch needs', [(n, need) for _l, n, need in ql] == [(15, {'FX_Q'}), (255, {'!FX_Q'})], ql)
    v, p, rows = verdicts('q4_step')
    check('B q4_step: the 4-bit destination beside RGBA8 + the 15-step quantise is OK', v == {'target_format': 'OK', 'blend_space': 'OK', 'precision': 'OK'} and not p, (v, p))
    check('B q4_step: the step carries its variant', any(q['variant'] == 'yEff_copy_q_fragment' for r in rows for q in r['quant']), rows[-1]['quant'])
    v, p, _ = verdicts('q4_none')
    check('B q4_none: no step = precision DIFF (LIQUID4BIT shape)', v['precision'] == 'DIFF' and v['target_format'] == 'OK', v)
    v, p, _ = verdicts('q4_unused')
    check('B q4_unused: a looked-up variant whose id is never read again does not count', v['precision'] == 'DIFF', v)
    v, p, _ = verdicts('gamma')
    check('B gamma surface beside RGBA8: blend_space DIFF', v['blend_space'] == 'DIFF' and v['target_format'] == 'OK', v)
    v, p, _ = verdicts('float7e3')
    check('B 7e3 surface beside RGBA8: target_format and precision DIFF', v['target_format'] == 'DIFF' and v['precision'] == 'DIFF', v)
    v, p, _ = verdicts('noconst')
    check('B a site with no constant: UNSETTLED', set(v.values()) == {'UNSETTLED'}, v)
    v, p, _ = verdicts('several')
    check('B two game formats, one image: target_format UNSETTLED', v['target_format'] == 'UNSETTLED', v)
    v, p, _ = verdicts('departed')
    check('B a DEPARTS marker on our format line: DEPARTED', v['target_format'] == 'DEPARTED', v)
    bad = dict(ROLES['q4_none'], ours=[('src/x/a.cpp', 'Nope::Make', 'c')])
    _r, _f, probs = T.role_rows(bad, SITES, read)
    check('B a role function not in our code is a PROBLEM', any('no definition of Nope::Make' in x for x in probs), probs)


# ------------------------------------------------------------------ C. real data: the LIQUID4BIT re-find
REAL = {}


def real_rows(ref):
    if ref not in REAL:
        import qostate as O
        rows, _facts, problems, _u = T.build(REAL['sites'], O.default_reader(ref), roles=[r for r in T.ROLES if r['role'] in ('sweat_map', 'scene_colour')])
        REAL[ref] = ({(r['role'], r['field']): r for r in rows}, problems)
    return REAL[ref]


def part_c():
    try:
        import q as qq
        if not os.path.exists(qq.DB):
            print(f'SKIP C: no q index at {qq.DB}')
            return False
        REAL['sites'] = T.default_sites(qq.connect(qq.DB))
    except Exception as e:   # noqa: BLE001  (the index or the decomp missing: said, then skipped)
        print(f'SKIP C: {e}')
        return False
    s = REAL['sites']
    check('C the game sites: sweat surface 0x18280186, map texture 0x1828014F',
          s.get(0x8279992C, {}).get('value') == 0x18280186 and s.get(0x827999A8, {}).get('value') == 0x1828014F,
          (s.get(0x8279992C), s.get(0x827999A8)))
    rows, probs = real_rows(PARENT)
    r = rows[('sweat_map', 'precision')]
    check(f'C RE-FIND at {PARENT}: sweat_map precision DIFF (game k_4_4_4_4, ours RGBA8)',
          r['verdict'] == 'DIFF' and r['game_values'] == ['k_4_4_4_4 (unorm4 x4)'] and r['ours_values'] == ['RGBA8 (unorm8 x4)'], (r['verdict'], r['game_values'], r['ours_values'], probs))
    rows, probs = real_rows(FROZEN)
    r = rows[('sweat_map', 'precision')]
    check(f'C at {FROZEN}: sweat_map precision OK through yLiquidMap_copy_prev_q4',
          r['verdict'] == 'OK' and any(q['variant'] == 'yLiquidMap_copy_prev_q4_fragment' for q in r['quant']), (r['verdict'], r['ours_values'], probs))
    for ref in (PARENT, FROZEN):
        rows, probs = real_rows(ref)
        v = {f: rows[('scene_colour', f)]['verdict'] for f in T.FIELDS}
        check(f'C at {ref}: scene_colour OK on all three fields (both blend as written)',
              v == {f: 'OK' for f in T.FIELDS} and rows[('scene_colour', 'blend_space')]['game_values'] == ['as written'], (v, probs))
    return True


# ------------------------------------------------------------------ D. planted twins
def twin(name, caught, detail):
    if caught:
        print(f'ok   planted twin caught: {name} ({detail})')
    else:
        NOT_CAUGHT.append(name)
        print(f'NOT CAUGHT planted twin: {name} ({detail})')


def part_d(real):
    v, _p, _ = verdicts('q4_none', 'no_dest')
    twin('no_dest', v['precision'] != 'DIFF', f'q4_none precision {v["precision"]} (the k_4_4_4_4 destination not read)')
    v, _p, _ = verdicts('gamma', 'no_gamma')
    twin('no_gamma', v['blend_space'] != 'DIFF', f'gamma blend_space {v["blend_space"]}')
    v, _p, _ = verdicts('q4_step', 'no_quant')
    twin('no_quant', v['precision'] != 'OK', f'q4_step precision {v["precision"]}')
    v, _p, _ = verdicts('q4_unused', 'quant_unused')
    twin('quant_unused', v['precision'] != 'DIFF', f'q4_unused precision {v["precision"]}')
    if real:
        import qostate as O
        T.PLANT = 'no_dest'
        try:
            rows, _f, _p, _u = T.build(REAL['sites'], O.default_reader(PARENT), roles=[r for r in T.ROLES if r['role'] == 'sweat_map'])
        finally:
            T.PLANT = ''
        pv = next(r['verdict'] for r in rows if r['field'] == 'precision')
        twin('no_dest on the real re-find', pv != 'DIFF', f'{PARENT} sweat_map precision {pv}')


def main():
    part_a()
    part_b()
    real = part_c()
    part_d(real)
    print(f'qtargetfmt selftest: {len(FAILS)} failed, {len(NOT_CAUGHT)} planted twins not caught' + ('' if real else ' (C skipped)'))
    if FAILS:
        return 1
    return 2 if NOT_CAUGHT else 0


if __name__ == '__main__':
    sys.exit(main())
