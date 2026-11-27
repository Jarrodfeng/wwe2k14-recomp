r"""Self-test of `q ostate` (S111 OSTATE): `py -I tools/q/qostate_selftest.py`   (0 game runs; reads our code at two frozen refs)

A. Planted C++ (invented text, read through the production path qostate.collect with a dict reader):
   1. a conditional alpha test (`fb.Dynamic(tested ? AlphaDynamic(...) : dynamic)`, `tested` = the bind's `b.alpha_test`): atest from
      the material flag, the func / ref per pass class from AlphaDynamic's body and the class table, the untested arm's func 7;
      the CROWDATEST form (`d.alpha_test < 0 ? bool(b.alpha_test) : d.alpha_test != 0`): the material when the caller says -1, the
      caller's InstancedDraw::alpha_test otherwise;
   2. a kTable row: keyed pass_state::kTable[NAME] with its addr cell's game functions, the alpha ref as the game's raw byte, the z
      func through the table file's own ForwardZ;
   3. an InstancedDraw store: keyed by the enclosing function, under its for loop; a store to a variable that is no InstancedDraw
      is not one.
   Each has a planted twin that must be CAUGHT: (1) `tested` made a literal false (the material source must go); (2) the crowd row's
   game z func 6 -> 4 (ForwardZ: LESS, not LEQUAL) and its addr cell's address changed (the key's game function must go); (3) the
   draw variable's type changed (the store must go).
B. Real tree (git show; SKIP without the refs): at 13ded28c9^1 (the CROWDATEST parent) the crowd member draw
   FrontEnd::DrawInstanced tests alpha from the material flag (b.alpha_test) only, and LiveCrowd::Record states none; at 13ded28c9
   (the fix) LiveCrowd::Record states 0 for the members (the s.ngroups loop) and 1 for the far cards (the kTypes loop), and
   DrawInstanced reads the caller's InstancedDraw::alpha_test. Twin: the parent's Dynamic() condition made a plant (the material
   source must go). C. One evaluator: qrstdiff's ours_state / eval_val are qostate's.
D. S111 CDARMS: a bind's store prints its if-arm and its comment's scope words (`arm FILE:LINE; scope: the colour pass only @..`);
   twin: WWE_QOSTATEWIDE_PLANT=no_arm (the arm not written) must lose them.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qostate as O   # noqa: E402

PARENT, FIX = '13ded28c9^1', '13ded28c9'   # S111 CROWDATEST: the parent (the material flag) and the merge (the caller-stated test)
FAILS, CAUGHT = [], []


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, caught, detail=''):
    if not caught:
        CAUGHT.append(name)
        print(f'NOT CAUGHT planted twin: {name} {detail}')
    else:
        print(f'ok   planted twin caught: {name}')


# ------------------------------------------------------------------ A. planted C++
SYN_PS = r'''
#pragma once
namespace rdr_scene::pass_state {
enum class PassClass : uint8_t { Lit, Crowd, Sweat, kCount };
struct PassState {
  const char* name;
  uint8_t alpha_test_func;
  float alpha_ref;
  uint8_t game_z_func;
  int8_t z_write;
  int8_t cull;
  uint8_t blend_code;
  bool tested_blends;
  const char* addr;
};
constexpr PassState kTable[size_t(PassClass::kCount)] = {
    {"lit", 6, 1.f / 255.f, 6, -1, -1, 0, true, "0x82BF5C58 pass_lambert_draw"},
    {"crowd", 6, 192.f / 255.f, 6, 1, 0, 0, false, "0x82519508 audience_draw_entry, 0x825185B0 blocks"},
    {"sweat", 7, 0.f, 7, 0, 0, 0, false, "0x8279ACD8 sweat map"},
};
constexpr const PassState& Of(PassClass c) { return kTable[size_t(c)]; }
constexpr uint8_t ForwardZ(uint8_t vk_func) { return vk_func == 6 ? 3 : vk_func == 4 ? 1 : vk_func; }
inline PassClass ClassOf(const char* effect, bool hair_pass) {
  if (effect && std::strncmp(effect, "yAudience", 9) == 0) return PassClass::Crowd;
  return PassClass::Lit;
}
}
'''
SYN_CPP = r'''
namespace rdr_scene {
rdr::DynamicState AlphaDynamic(const rdr::DynamicState& base, pass_state::PassClass c, bool plant_old) {
  rdr::DynamicState d = base;
  d.system[8] = plant_old ? 0.5f : pass_state::Of(c).alpha_ref;
  d.system[9] = plant_old ? 6.f : float(pass_state::Of(c).alpha_test_func);
  return d;
}
uint32_t Syn::DrawCrowd(const InstancedDraw& d, rdr::FrameBuilder& fb) {
  rdr::DynamicState dynamic{};
  dynamic.system[9] = 7.f;
  const bool tested = b.alpha_test;
  rdr::DrawState state{};
  state.depth_test = 1;
  const pass_state::PassClass pcls = pass_state::ClassOf(mat.shader.c_str(), false);
  state.depth_compare = pass_state::ForwardZ(pass_state::Of(pcls).game_z_func);
  if (tested && pass_state::Of(pcls).tested_blends) state.blend[0].enable = 1;
  rdr::DrawOp op{};
  op.state = fb.State(state);
  op.dynamic = fb.Dynamic(tested ? AlphaDynamic(dynamic, pcls, options_.plant_old_alpha_ref) : dynamic);
  return 1;
}
void Syn::Record(rdr::FrameBuilder& fb) {
  for (int i = 0; i < n; ++i) {
    InstancedDraw d;
    d.alpha_test = 0;
    DrawCrowd(d, fb);
  }
  Other o;
  o.alpha_test = 1;
}
void Syn::Stage(rdr::FrameBuilder& fb) {
  for (int i = 0; i < n; ++i) {
    InstancedDraw d;
    d.alpha_test = 0;
    fe.DrawInstanced(d, ctx, fb, st);
  }
  for (int k = 0; k < m; ++k) {
    InstancedDraw d;
    d.alpha_test = 1;
    fe.DrawInstanced(d, ctx, fb, st);
  }
  InstancedDraw b;
  b.no_blend = true;
  fe.DrawInstanced(b, ctx, fb, st);
}
}
'''
CPP = 'src/renderer_scene/syn_ostate.cpp'


def syn(edits=None):
    files = {O.PS: SYN_PS, CPP: SYN_CPP}
    for rel, (old, new) in (edits or {}).items():
        if files[rel].count(old) != 1:
            raise SystemExit(f'planted edit not found once in {rel}: {old!r}')
        files[rel] = files[rel].replace(old, new)

    def read(rel):
        if rel not in files:
            raise O.OstateError(f'{rel}: not in the planted tree')
        return files[rel]
    facts, problems, _c = O.collect(read=read, files=[CPP])
    return facts, problems


def pick(facts, fn, field, kind=None):
    return [f for f in facts if f['fn'].endswith(fn) and f['field'] == field and (kind is None or f['kind'] == kind)]


def has(facts, fn, field, value, kind=None):
    return any(f['value'] == value for f in pick(facts, fn, field, kind))


def part_a():
    f, p = syn()
    check('A no problem', not p, str(p))
    mat = {'material': 'b.alpha_test'}
    check('A1 conditional alpha test: atest from the material flag', has(f, 'Syn::DrawCrowd', 'alpha_test', mat, 'alpha'),
          str(pick(f, 'Syn::DrawCrowd', 'alpha_test')))
    refs = {(x['value'], x.get('cond')) for x in pick(f, 'Syn::DrawCrowd', 'alpha_ref')}
    check('A1 the tested arm: ref per pass class (192 crowd, 1 lit)', refs == {(192, 'tested && pass class crowd'), (1, 'tested && pass class lit')}, str(refs))
    funcs = {(x['value'], x.get('cond')) for x in pick(f, 'Syn::DrawCrowd', 'alpha_func')}
    check('A1 the func: 6 when tested, 7 (always) on the other arm', funcs == {(6, 'tested'), (7, '!(tested)')}, str(funcs))
    zf = {(x['value'], x.get('cond')) for x in pick(f, 'Syn::DrawCrowd', 'z_func', 'set')}
    check('A1 the block: z func through ForwardZ, the same for both classes (no class cond)', zf == {(3, None)}, str(zf))
    fc, _ = syn({CPP: ('const bool tested = b.alpha_test;', 'const bool tested = d.alpha_test < 0 ? bool(b.alpha_test) : d.alpha_test != 0;')})
    at = {(str(x['value']), x.get('cond')) for x in pick(fc, 'Syn::DrawCrowd', 'alpha_test')}
    check('A1 the CROWDATEST form: the material when the caller says -1, else the caller', at == {
        (str(mat), 'd.alpha_test < 0'), (str({'caller': 'InstancedDraw::alpha_test', 'expr': 'd.alpha_test != 0'}), '!(d.alpha_test < 0)')}, str(at))
    ft, _ = syn({CPP: ('const bool tested = b.alpha_test;', 'const bool tested = false;')})
    twin('A1 `tested` made false: the material source goes', not has(ft, 'Syn::DrawCrowd', 'alpha_test', mat), str(pick(ft, 'Syn::DrawCrowd', 'alpha_test')))

    row = 'pass_state::kTable[crowd]'
    check('A2 a kTable row: the alpha ref as the raw byte 192', has(f, row, 'alpha_ref', 192, 'table'), str(pick(f, row, 'alpha_ref')))
    zr = pick(f, row, 'z_func', 'table')
    check('A2 the z func through ForwardZ: GEQUAL 6 -> LEQUAL 3, the game\'s 6 kept', zr and zr[0]['value'] == 3 and zr[0].get('game_vk') == 6, str(zr))
    check('A2 keyed by its addr cell: its game functions', zr and zr[0].get('game') == ['sub_82519508', 'sub_825185B0'] and 'audience_draw_entry' in zr[0]['addr'], str(zr))
    check('A2 z write / cull literals of the row', has(f, row, 'z_write', 1) and has(f, row, 'cull', 0) and has(f, 'pass_state::kTable[lit]', 'cull', {'rule': "the mesh's own (the file's winding rule)"}))
    check('A2 --fn by a game function the row names', any(x['fn'] == row for x in O.select(f, 'sub_82519508', 'zfunc')), str(O.select(f, 'sub_82519508', 'zfunc')))
    fz, _ = syn({O.PS: ('{"crowd", 6, 192.f / 255.f, 6,', '{"crowd", 6, 192.f / 255.f, 4,')})
    twin('A2 the crowd row given GREATER: z func LESS, not LEQUAL', not has(fz, row, 'z_func', 3) and has(fz, row, 'z_func', 1), str(pick(fz, row, 'z_func')))
    fa, _ = syn({O.PS: ('"0x82519508 audience_draw_entry', '"0x82519500 audience_draw_entry')})
    twin('A2 the addr cell changed: the key loses sub_82519508', not any('sub_82519508' in x.get('game', []) for x in pick(fa, row, 'z_func')), str(pick(fa, row, 'z_func')))

    st = pick(f, 'Syn::Record', 'alpha_test', 'caller')
    check('A3 an InstancedDraw store: keyed by the enclosing function, under its loop', len(st) == 1 and st[0]['value'] == 0
          and (st[0]['cond'] or '').startswith('for (') and st[0].get('store') == 'InstancedDraw::alpha_test' and st[0]['at'] == f'{CPP}:26', str(st))
    check('A3 a store to another type is not one', not any(x['value'] == 1 for x in pick(f, 'Syn::Record', 'alpha_test')), str(pick(f, 'Syn::Record', 'alpha_test')))
    fi, _ = syn({CPP: ('    InstancedDraw d;\n    d.alpha_test = 0;\n    DrawCrowd', '    OtherDraw d;\n    d.alpha_test = 0;\n    DrawCrowd')})
    twin('A3 the variable is no InstancedDraw: the store goes', not pick(fi, 'Syn::Record', 'alpha_test'), str(pick(fi, 'Syn::Record', 'alpha_test')))

    # S111 CDFOLLOW: one unit per InstancedDraw declaration; the -1 draw said; a kTable column read as a condition, per class
    sg = pick(f, 'Syn::Stage', 'alpha_test', 'caller')
    stored = sorted((x['value'], x.get('decl')) for x in sg if not x.get('left'))
    check('A4 each store keyed by its own InstancedDraw declaration (two `d`s, two units), each reaching DrawInstanced',
          stored == [(0, f'{CPP}:34'), (1, f'{CPP}:39')] and all(x.get('calls') == 'DrawInstanced' for x in sg), str(sg))
    left = [x for x in sg if x.get('left')]
    check('A4 a draw that reaches DrawInstanced with alpha_test never stored: a `left` fact (the material) at the call',
          len(left) == 1 and left[0]['decl'] == f'{CPP}:43' and left[0]['at'] == f'{CPP}:45' and 'material' in left[0]['value'], str(left))
    O.PLANT = 'one_unit'
    try:
        fo, _ = syn()
    finally:
        O.PLANT = ''
    twin('A4 every store keyed by its function only: no declaration', not any(x.get('decl') for x in pick(fo, 'Syn::Stage', 'alpha_test')))
    fs, _ = syn({CPP: ('  b.no_blend = true;\n', '  b.alpha_test = 1;\n')})
    twin('A4 the bake states its test: the left fact goes', not [x for x in pick(fs, 'Syn::Stage', 'alpha_test') if x.get('left')])
    bl = [(x['value'], x.get('cond')) for x in pick(f, 'Syn::DrawCrowd', 'blend_enable', 'set')]
    check("A5 a kTable column read as a condition (tested_blends: lit true, crowd false) splits per pass class: the blend arm is the lit class's",
          len(bl) == 1 and bl[0][0] == 1 and 'pass class lit' in (bl[0][1] or ''), str(bl))
    ft2, _ = syn({O.PS: ('6, 1, 0, 0, false, "0x82519508', '6, 1, 0, 0, true, "0x82519508')})
    bl2 = [(x['value'], x.get('cond')) for x in pick(ft2, 'Syn::DrawCrowd', 'blend_enable', 'set')]
    twin('A5 the column the same for every class: no class tag', len(bl2) == 1 and 'pass class' not in (bl2[0][1] or ''), str(bl2))


def part_dead():
    """S111 PARTZ2: a kTable row no draw reads (ClassOf never returns it, no PassClass::<it> anywhere) carries `dead`"""
    f, _p = syn()
    sw = pick(f, 'pass_state::kTable[sweat]', 'z_func', 'table')
    check('A6 a row ClassOf never returns and nothing names: its facts carry `dead`', sw and all(x.get('dead') for x in sw)
          and not any(x.get('dead') for x in pick(f, 'pass_state::kTable[crowd]', 'z_func', 'table')), str(sw))
    fr, _p = syn({CPP: ('  Other o;\n', '  Other o = Of(PassClass::Sweat);\n')})
    twin('A6 the row named by a draw: not dead', not any(x.get('dead') for x in pick(fr, 'pass_state::kTable[sweat]', 'z_func', 'table')))


# ------------------------------------------------------------------ B. the real tree at the CROWDATEST refs
def edited(ref, edits):
    base = O.default_reader(ref)

    def read(rel):
        t = base(rel)
        for old, new in edits.get(rel, ()):
            if t.count(old) != 1:
                raise SystemExit(f'planted edit not found once in {rel} at {ref}: {old!r}')
            t = t.replace(old, new)
        return t
    return read


def part_b():
    try:
        for ref in (PARENT, FIX):
            subprocess.run(['git', 'cat-file', '-e', f'{ref}^{{commit}}'], cwd=O.REPO, check=True, capture_output=True)
    except Exception as e:  # noqa: BLE001 - no history here
        print(f'SKIP B (no CROWDATEST refs: {e})')
        return
    mat = {'material': 'b.alpha_test'}
    fp, pp = O.collect(PARENT)[:2]
    check('B parent: no problem', not pp, str(pp))
    di = pick(fp, 'FrontEnd::DrawInstanced', 'alpha_test')
    check('B1 parent: the crowd member draw tests alpha from the material flag only', [x['value'] for x in di] == [mat] and di[0]['cond'] is None
          and 'scene_crowd (instanced)' in di[0].get('kinds', []), str(di))
    check('B1 parent: LiveCrowd::Record states no alpha test (every draw of it a `left` -1 fact)',
          all(x.get('left') for x in pick(fp, 'LiveCrowd::Record', 'alpha_test')), str(pick(fp, 'LiveCrowd::Record', 'alpha_test')))
    ff, pf = O.collect(FIX)[:2]
    check('B fix: no problem', not pf, str(pf))
    lc = [x for x in pick(ff, 'LiveCrowd::Record', 'alpha_test', 'caller') if not x.get('left')]
    mem = [x for x in lc if 's.ngroups' in (x['cond'] or '')]
    cards = [x for x in lc if 'kTypes' in (x['cond'] or '')]
    check('B2 fix: the members stated 0 by the caller (the s.ngroups loop)', len(mem) == 1 and mem[0]['value'] == 0
          and mem[0]['at'].startswith('src/renderer_scene/live_crowd.cpp:'), str(lc))
    check('B2 fix: the far cards stated 1 (the kTypes loop)', [x['value'] for x in cards] == [1], str(cards))
    di = pick(ff, 'FrontEnd::DrawInstanced', 'alpha_test')
    check('B2 fix: DrawInstanced reads the caller\'s InstancedDraw::alpha_test, the material only at -1',
          any(isinstance(x['value'], dict) and x['value'].get('caller') == 'InstancedDraw::alpha_test' for x in di)
          and all(x['cond'] for x in di), str(di))
    ft = O.collect(PARENT, read=edited(PARENT, {O.FE: [('fb.Dynamic(b.alpha_test ? AlphaDynamic(', 'fb.Dynamic(g_plant_ostate ? AlphaDynamic(')]}))[0]
    twin('B the parent\'s Dynamic() condition made a plant: the material source goes',
         not has(ft, 'FrontEnd::DrawInstanced', 'alpha_test', mat), str(pick(ft, 'FrontEnd::DrawInstanced', 'alpha_test')))


def part_c():
    import qrstdiff as R
    check('C one evaluator: qrstdiff.ours_state / eval_val / parse_table are qostate\'s',
          R.ours_state is O.ours_state and R.eval_val is O.eval_val and R.parse_table is O.parse_table and R.RstDiffError is O.OstateError)


BIND_EDIT = {CPP: ('void Syn::Stage(rdr::FrameBuilder& fb) {\n', '\n'.join([
    'const Bound& Syn::BindD(const Mat& mat, bool chr, int ps) {',
    '  auto b = std::make_unique<Bound>();',
    '  if (chr) {',
    '    // the translucent node sub_8259FAE8 tests it (this bind serves the colour pass only)',
    '    b->alpha_test = VariantHasTwin(ps);',
    '    return *b;',
    '  }',
    '  b->alpha_test = mat.alpha_test;',
    '  return *b;',
    '}',
    'void Syn::DrawD(const Mat& mat, rdr::FrameBuilder& fb, const rdr::DynamicState& dyn) {',
    '  const Bound& b = BindD(mat, true, 3);',
    '  rdr::DrawState state{};',
    '  state.depth_test = 1;',
    '  rdr::DrawOp op{};',
    '  op.state = fb.State(state);',
    '  op.dynamic = fb.Dynamic(b.alpha_test ? AlphaDynamic(dyn, pass_state::PassClass::Lit, false) : dyn);',
    '}',
    'void Syn::Stage(rdr::FrameBuilder& fb) {', '']))}


def bind_lines(plant=''):
    os.environ['WWE_QOSTATEWIDE_PLANT'] = plant
    try:
        f, _p = syn(BIND_EDIT)
    finally:
        os.environ['WWE_QOSTATEWIDE_PLANT'] = ''
    return [O.fact_line(x) for x in pick(f, 'Syn::BindD', 'alpha_test', 'bind')]


def part_d():
    ls = bind_lines()
    text = SYN_CPP.replace(*BIND_EDIT[CPP])
    arm_ln = text[:text.index('  if (chr) {\n    // the translucent')].count('\n') + 1
    check('D a bind store prints its if-arm and its comment\'s scope words',
          len(ls) == 2 and any(f'arm {CPP}:{arm_ln}; scope: the colour pass only @{CPP}:{arm_ln + 1}' in x and 'VariantHasTwin' in x for x in ls)
          and any('mat.alpha_test' in x and 'arm ' in x and 'scope' not in x for x in ls), '\n'.join(ls))
    tw = bind_lines('no_arm')
    twin('D PLANT no_arm: the arm and the scope go from the line', tw and not any('arm ' in x for x in tw), '\n'.join(tw))


def main():
    part_a()
    part_dead()
    part_b()
    part_c()
    part_d()
    print(f'{len(FAILS)} failed, {len(CAUGHT)} planted twins not caught')
    return 2 if CAUGHT else 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
