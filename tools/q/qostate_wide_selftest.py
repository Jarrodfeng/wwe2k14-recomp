r"""Self-test of q ostate's wide reader (S111 OSTATEWIDE, tools/q/qostate_wide.py): `py -I tools/q/qostate_wide_selftest.py`   (0 game runs)

A. Planted C++ (invented text, read through the production path qostate.collect with a dict reader, the hook in collect included):
   1. a copy block `rdr::DrawState state = fixed.state;` (fixed a Fixed, Fixed::state decoded from the device words by DecodeIt): every
      field inherits the game's word; the velocity arm's stores (a range-for zeroing the blends, the write mask, depth test) add under
      their condition; an unconditional store (depth write 0) replaces the base;
   2. a reference store `rdr::DrawState& st`: depth compare 3, cull 0 under `far`;
   3. a pointer store `rdr::DrawState* s`: `s->stencil_test = 0`;
   4. set aside with a reason: a decoder's own stores (the game's state), a tween range `out.own.blend[0]` (Tween), an `Item`-like
      record's `it.depth_write`;
   5. a bind record: BindIt's `b->alpha_test` stores (a rule under `chr`, the material flag otherwise) are facts of BindIt, and the
      draw's alpha fact that reads `b.alpha_test` (b a Bound) carries them as `bind`.
   Planted twins that must be CAUGHT: (1) the copy's source made a member of a struct nobody decodes (the game's word must go); the
   decoder's device read removed (the base must stop being the game's); (2) / (3) the variable's type made a non-DrawState record (the
   stores must go, set aside); (5) the char arm removed (the bind link must lose the rule). PLANT twins (WWE_QOSTATEWIDE_PLANT): no_override
   (the base survives an unconditional store), no_arrow (the pointer store goes), any_type (the tween range is read), no_decoder (the
   decoder's stores become facts).
C. S111 CDARMS (tools/q/qcdarms.py), planted C++ through the same path:
   6. a bind's stores each carry their if-arm: a braced `if (chr) {` arm (with the comment block above the store and its scope words
      `colour pass only`), a braceless `if (ps < 0) STMT` arm, the function's own line for the store after an arm that returns;
   7. a copy of a member that a function fills from its PARAMETER (`in.state = state`) is the state that function's callers pass: two
      calls passing the game's decoded word = the game's word; a caller passing another state = UNSETTLED, both callers named.
   Twins: PLANT no_arm (no arm keys), no_climb (the parameter stays a rule).
B. Real tree (git show; SKIP without the refs): GINHERIT's ALPHAWREST re-find: at 22f306696^1 FrontEnd::BindMaterial's wrestler arm
   (`IsCharEffect(mat.shader)`) binds alpha_test from the material flag mat.alpha_test; at 22f306696 from a rule of ours (VariantHasTwin),
   never the flag; FrontEnd::Record's alpha fact names the binds. The working tree: Binder::BindDraw's copy of fixed.state is the game's
   word (DrawFixed::state, decoded by rdr_reader::DecodeFixed) and DecodeFixed's stores are set aside.
Exit 0 = all pass, 1 = a check failed, 2 = a planted twin was NOT caught.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qostate as O   # noqa: E402

FAILS, MISSED = [], []
CPP = 'src/renderer_scene/zz_ostatewide_plant.cpp'
PARENT, FIX = '22f306696^1', '22f306696'

SYN_PS = r'''
#pragma once
namespace rdr_scene::pass_state {
enum class PassClass : uint8_t { Lit, kCount };
struct PassState {
  const char* name;
  uint8_t alpha_test_func;
};
inline constexpr PassState kTable[1] = {
    {"lit", 6},
};
inline PassClass ClassOf(int x) { return PassClass::Lit; }
}
'''

PLANT = r'''
namespace rdr_scene {
struct Fixed { rdr::DrawState state{}; uint8_t alpha_test = 0; };
struct Loose { rdr::DrawState state{}; };
struct Tween { uint16_t offset = 0, bytes = 0; };
struct Own { Tween blend[4]; };
struct Out { Own own; };
struct Rec { int depth_write = 0; int depth_compare = 0; int cull = 0; int stencil_test = 0; };
struct Bound { bool alpha_test = false; int program = 0; };

inline void DecodeIt(const uint8_t* base, uint32_t dev, Fixed& f) {
  const uint32_t dc = db::Ld32(base, dev + kDevDepthControl);
  f.state.depth_test = uint8_t(dc & 1);
  f.state.cull = 2;
}

void Binder::DrawIt(const Fixed& fixed, const Loose& loose, rdr::FrameBuilder& builder) {
  rdr::DrawState state = fixed.state;
  if (velocity_) {
    for (auto& b : state.blend) b = rdr::BlendState{};
    state.blend[0].write_mask = 0xF;
    state.depth_test = 1;
  }
  state.depth_write = 0;
  op.state = builder.State(state);
}

void Tune(rdr::DrawState& st, bool far) {
  st.depth_compare = 3;
  if (far) st.cull = 0;
}

void Patch(rdr::DrawState* s) {
  s->stencil_test = 0;
}

void Other(Out& out, Rec& it) {
  out.own.blend[0] = {16, 48};
  it.depth_write = 1;
}

const Bound& BindIt(const Mat& mat, bool chr, int ps) {
  auto b = std::make_unique<Bound>();
  if (chr) {
    b->alpha_test = VariantHasTwin(ps);
    b->program = ProgramFor(1, ps, b->alpha_test);
  } else {
    b->alpha_test = mat.alpha_test;
  }
  return *b;
}

void DrawRec(const Mat& mat, rdr::FrameBuilder& fb, const rdr::DynamicState& dyn) {
  const Bound& b = BindIt(mat, true, 3);
  rdr::DrawState state{};
  state.depth_test = 1;
  op.state = fb.State(state);
  op.dynamic = fb.Dynamic(b.alpha_test ? AlphaDynamic(dyn, 0) : dyn);
}
}  // namespace rdr_scene
'''


def check(name, cond, detail=''):
    if not cond:
        FAILS.append(f'{name}: {detail}')
        print(f'FAIL {name} {detail}')
    else:
        print(f'ok   {name}')


def twin(name, caught, detail=''):
    if not caught:
        MISSED.append(name)
        print(f'NOT CAUGHT planted twin: {name} {detail}')
    else:
        print(f'ok   planted twin caught: {name}')


def run(text, plant=''):
    os.environ['WWE_QOSTATEWIDE_PLANT'] = plant
    try:
        files = {CPP: text, O.PS: SYN_PS}

        def read(rel):
            if rel in files:
                return files[rel]
            raise O.OstateError(f'{rel}: not planted')
        return O.collect(read=read, files=[CPP])
    finally:
        os.environ['WWE_QOSTATEWIDE_PLANT'] = ''


def of(facts, fn, field, kind=None):
    return [f for f in facts if f['fn'].endswith(fn) and f['field'] == field and (kind is None or f['kind'] == kind)]


def vals(fs):
    return sorted((O.value_text(f['value']), f.get('cond') or '') for f in fs)


def aside_of(counts, needle):
    return [a for a in counts.get('aside', ()) if needle in a[2]]


def part_a():
    facts, _p, c = run(PLANT)
    # 1. the copy block
    ze = of(facts, 'DrawIt', 'z_enable')
    check('A1 copy: z_enable = the game\'s word (copy) and 1 under velocity_',
          vals(ze) == [('1', 'velocity_'), ('game', '')] and any(f['kind'] == 'copy' and 'DecodeIt' in f.get('base_src', '') for f in ze), vals(ze))
    zw = of(facts, 'DrawIt', 'z_write')
    check('A1 copy: an unconditional store replaces the base (z_write 0 only)', vals(zw) == [('0', '')], vals(zw))
    cm = of(facts, 'DrawIt', 'colour_write_mask')
    check('A1 copy: the range-for zero then the mask 0xF under velocity_', vals(cm) == [('0', 'velocity_'), ('15', 'velocity_'), ('game', '')], vals(cm))
    check('A1 copy: one block key (the declaration line)', len({f['block'] for f in of(facts, 'DrawIt', 'z_enable')}) == 1
          and all(f.get('wide') == 'copy' for f in ze))
    # 2. the reference store
    zf, cu = of(facts, '::Tune', 'z_func'), of(facts, '::Tune', 'cull')
    check('A2 ref: z_func 3, cull 0 when far', vals(zf) == [('3', '')] and vals(cu) == [('0', 'far')], (vals(zf), vals(cu)))
    # 3. the pointer store
    se = of(facts, '::Patch', 'stencil_enable')
    check('A3 pointer: stencil_enable 0', vals(se) == [('0', '')], vals(se))
    # 4. set aside
    check('A4 the decoder\'s stores are set aside as the game\'s, no fact of DecodeIt',
          len(aside_of(c, 'the game\'s own state')) == 2 and not [f for f in facts if f['fn'].endswith('DecodeIt')], c.get('aside'))
    check('A4 the tween range and the record are set aside, not read', aside_of(c, 'out is Out.own is Own') and aside_of(c, 'it is Rec')
          and not [f for f in facts if f['fn'].endswith('::Other')], c.get('aside'))
    # 5. the bind record
    bi = of(facts, 'BindIt', 'alpha_test', 'bind')
    check('A5 bind: BindIt binds a rule under chr, the material flag otherwise',
          vals(bi) == [('material mat.alpha_test', '!(chr)'), ('rule VariantHasTwin(ps)', 'chr')], vals(bi))
    al = [f for f in of(facts, 'DrawRec', 'alpha_test', 'alpha') if isinstance(f['value'], dict) and 'material' in f['value']]
    check('A5 bind: the draw\'s alpha fact names both binds', al and len(al[0].get('bind', ())) == 2
          and any('VariantHasTwin' in b for b in al[0]['bind']), [f.get('bind') for f in al])
    check('A counts: wide units and bind stores counted', c.get('wide_units', 0) >= 3 and c.get('bind_stores') == 2, {k: c.get(k) for k in ('wide_units', 'bind_stores')})

    # twins
    f2, _p, _c = run(PLANT.replace('rdr::DrawState state = fixed.state;', 'rdr::DrawState state = loose.state;'))
    v2 = vals(of(f2, 'DrawIt', 'z_enable'))
    twin('T1 copy of a member nobody decodes: not the game\'s word', ('game', '') not in v2 and any('Loose::state' in x[0] for x in v2), v2)
    f3, _p, _c = run(PLANT.replace('  const uint32_t dc = db::Ld32(base, dev + kDevDepthControl);\n', '  const uint32_t dc = dev;\n'))
    twin('T1 the decoder\'s device read removed: the base is no longer the game\'s', ('game', '') not in vals(of(f3, 'DrawIt', 'z_enable'))
         and of(f3, 'DecodeIt', 'cull'), vals(of(f3, 'DrawIt', 'z_enable')))
    f4, _p, c4 = run(PLANT.replace('void Tune(rdr::DrawState& st', 'void Tune(Rec& st').replace('void Patch(rdr::DrawState* s)', 'void Patch(Rec* s)'))
    twin('T2/T3 a non-DrawState record: the ref / pointer stores go (set aside)', not of(f4, '::Tune', 'z_func') and not of(f4, '::Patch', 'stencil_enable')
         and aside_of(c4, 'st is Rec') and aside_of(c4, 's is Rec'), c4.get('aside'))
    f5, _p, _c = run(PLANT.replace('  if (chr) {\n    b->alpha_test = VariantHasTwin(ps);\n    b->program = ProgramFor(1, ps, b->alpha_test);\n  } else {\n'
                                   '    b->alpha_test = mat.alpha_test;\n  }\n', '  b->alpha_test = mat.alpha_test;\n'))
    al5 = [f for f in of(f5, 'DrawRec', 'alpha_test', 'alpha') if isinstance(f['value'], dict) and 'material' in f['value']]
    twin('T5 the char arm removed: the bind link is the material flag only', al5 and al5[0].get('bind') and not any('VariantHasTwin' in b for b in al5[0]['bind']),
         [f.get('bind') for f in al5])
    for plant, test in (('no_override', lambda fs, cc: vals(of(fs, 'DrawIt', 'z_write')) != [('0', '')]),
                        ('no_arrow', lambda fs, cc: not of(fs, '::Patch', 'stencil_enable')),
                        ('any_type', lambda fs, cc: bool([f for f in fs if f['fn'].endswith('::Other')])),
                        ('no_decoder', lambda fs, cc: bool([f for f in fs if f['fn'].endswith('DecodeIt')]))):
        fp, _p, cp = run(PLANT, plant)
        twin(f'PLANT {plant}', test(fp, cp))


def have(ref):
    return subprocess.run(['git', 'rev-parse', '--verify', '-q', ref + '^{commit}'], cwd=O.REPO, capture_output=True).returncode == 0


def part_b():
    if not (have(PARENT) and have(FIX)):
        print(f'SKIP B: {PARENT} / {FIX} not in this clone')
        return
    for ref, want_mat in ((PARENT, True), (FIX, False)):
        facts = O.collect(ref)[0]
        bm = [f for f in facts if f['fn'] == 'rdr_scene::FrontEnd::BindMaterial' and f['kind'] == 'bind'
              and 'IsCharEffect(mat.shader)' in (f.get('cond') or '') and f.get('store') == 'Bound::alpha_test']
        mats = [f for f in bm if isinstance(f['value'], dict) and 'mat.alpha_test' in str(f['value'].get('material', ''))]
        check(f'B {ref}: the wrestler arm binds alpha_test from ' + ('the material flag' if want_mat else 'a rule (not the flag)'),
              bm and (bool(mats) if want_mat else not mats), vals(bm))
        rec = [f for f in facts if f['fn'] == 'rdr_scene::FrontEnd::Record' and f['kind'] == 'alpha' and f['field'] == 'alpha_test' and f.get('bind')]
        check(f'B {ref}: FrontEnd::Record\'s alpha fact names the binds', rec and any('BindMaterial' in b for b in rec[0]['bind']), len(rec))
        if ref == PARENT:
            twin('B twin: the parent\'s flag is not the tip\'s', bool(mats))
    facts, _p, c = O.collect(None)
    bd = [f for f in facts if f['fn'].endswith('Binder::BindDraw') and f['field'] == 'z_enable']
    check('B working tree: BindDraw\'s copy of fixed.state is the game\'s word (DrawFixed::state, DecodeFixed)',
          any(f['kind'] == 'copy' and O.value_text(f['value']) == 'game' and 'DecodeFixed' in f.get('base_src', '') for f in bd), vals(bd))
    check('B working tree: DecodeFixed\'s stores set aside as the game\'s', aside_of(c, 'DecodeFixed reads the guest device words'), len(c.get('aside', ())))
    cd = [f for f in facts if f['fn'].endswith('CardsDrawer::Draw') and f['field'] == 'z_enable']
    check('B working tree (CDARMS): CardsDrawer::Draw\'s copy of CrowdInject::state is the game\'s word, through InjectCrowd\'s caller BindDraw',
          any(O.value_text(f['value']) == 'game' and 'Binder::BindDraw passes state' in f.get('base_src', '') for f in cd), vals(cd))
    bm = [f for f in facts if f['fn'] == 'rdr_scene::FrontEnd::BindMaterial' and f['kind'] == 'bind']
    check('B working tree (CDARMS): BindMaterial\'s wrestler colour arm is its own arm, scoped to the colour pass; the material arm another',
          len({f.get('arm') for f in bm}) >= 3 and any(f.get('scope') == 'colour' and 'BindTests' in str(f['value']) for f in bm)
          and any('mat.alpha_test' in str(f['value']) and not f.get('scope') for f in bm), [(f['at'], f.get('arm'), f.get('scope')) for f in bm])


ARMS = r'''
namespace rdr_scene {
const Bound& BindTwo(const Mat& mat, bool chr, int ps) {
  auto b = std::make_unique<Bound>();
  if (chr) {
    b->debug = false;
    // the translucent node sub_83002000 tests it; the map pass sub_83002100 turns it off (this bind serves the colour pass only)
    b->alpha_test = VariantHasTwin(ps);
    return *b;
  }
  if (ps < 0) b->alpha_test = false;
  b->alpha_test = mat.alpha_test;
  return *b;
}
void DrawTwo(const Mat& mat, rdr::FrameBuilder& fb, const rdr::DynamicState& dyn) {
  const Bound& b = BindTwo(mat, true, 3);
  rdr::DrawState state{};
  state.depth_test = 1;
  op.state = fb.State(state);
  op.dynamic = fb.Dynamic(b.alpha_test ? AlphaDynamic(dyn, 0) : dyn);
}
struct CrowdIn { rdr::DrawState state; };   // (no `{}`: a member initialiser reads as a block)
uint32_t Hook::TakeCdarms(CrowdIn& in, rdr::FrameBuilder& builder) {
  rdr::DrawState state = in.state;
  state.cull = 0;
  op.state = builder.State(state);
  return 1;
}
uint32_t Binder::PassCdarms(const rdr::DrawState& state, rdr::FrameBuilder& builder, bool far = false) {
  CrowdIn in;
  in.state = state;
  return hook_->TakeCdarms(in, builder);
}
void Binder::GoCdarms(const Fixed& fixed, rdr::FrameBuilder& builder) {
  rdr::DrawState st = fixed.state;
  uint32_t n = PassCdarms(st, builder);
  n += PassCdarms(st, builder, true);
}
}  // namespace rdr_scene
'''
OTHER_CALLER = r'''
namespace rdr_scene {
void Binder::OtherCdarms(rdr::FrameBuilder& builder) {
  rdr::DrawState zero{};
  zero.depth_test = 1;
  PassCdarms(zero, builder);
}
}
'''


def line_no(text, needle):
    return text[:text.index(needle)].count('\n') + 1


def part_c():
    text = PLANT + ARMS
    facts, _p, _c = run(text)
    bt = {f['at'].rpartition(':')[2]: f for f in of(facts, 'BindTwo', 'alpha_test', 'bind')}
    chr_ln, brace_ln = line_no(text, '    b->alpha_test = VariantHasTwin(ps);\n    return'), line_no(text, 'if (ps < 0) b->alpha_test')
    mat_ln, fn_ln = line_no(text, '  b->alpha_test = mat.alpha_test;\n  return'), line_no(text, 'const Bound& BindTwo(')
    c6, b6, m6 = bt.get(str(chr_ln)), bt.get(str(brace_ln)), bt.get(str(mat_ln))
    check('C6 the braced arm: its `if (chr) {` line, the comment block above the store, its scope words (colour)',
          c6 and c6.get('arm') == f'{CPP}:{line_no(text, "  if (chr) {\n    b->debug")}' and c6.get('attached') == [chr_ln - 1]
          and c6.get('scope') == 'colour' and c6.get('scope_at') == f'{CPP}:{chr_ln - 1}', c6)
    check('C6 the braceless arm: the `if (ps < 0)` line itself', b6 and b6.get('arm') == f'{CPP}:{brace_ln}' and not b6.get('scope'), b6)
    check('C6 the store after an arm that returns: the function\'s own line', m6 and m6.get('arm') == f'{CPP}:{fn_ln}' and not m6.get('attached'), m6)
    fp, _p, _c = run(text, 'no_arm')
    twin('C6 PLANT no_arm: no arm keys', not any(f.get('arm') for f in of(fp, 'BindTwo', 'alpha_test', 'bind')))
    tk = of(facts, 'TakeCdarms', 'z_enable')
    check('C7 a member filled from a parameter: the callers pass the game\'s decoded word (both calls of GoCdarms)',
          vals(tk) == [('game', '')] and 'GoCdarms passes st' in tk[0].get('base_src', ''), [(vals(tk), f.get('base_src')) for f in tk])
    check('C7 the function\'s own store still overrides the base (cull 0)', vals(of(facts, 'TakeCdarms', 'cull')) == [('0', '')], vals(of(facts, 'TakeCdarms', 'cull')))
    f2, _p, _c = run(text + OTHER_CALLER)
    t2 = of(f2, 'TakeCdarms', 'z_enable')
    check('C7 a caller passing another state: UNSETTLED (a rule), both callers named',
          len(t2) == 1 and 'pass different states' in vals(t2)[0][0] and 'GoCdarms' in vals(t2)[0][0] and 'OtherCdarms' in vals(t2)[0][0], vals(t2))
    f3, _p, _c = run(text, 'no_climb')
    t3 = of(f3, 'TakeCdarms', 'z_enable')
    twin('C7 PLANT no_climb: the parameter stays a rule (not followed)', t3 and all('not followed' in x[0] for x in vals(t3)), vals(t3))


def main():
    part_a()
    part_c()
    part_b()
    print(f'qostate_wide_selftest: {len(FAILS)} failed, {len(MISSED)} planted twins not caught')
    return 1 if FAILS else 2 if MISSED else 0


if __name__ == '__main__':
    sys.exit(main())
