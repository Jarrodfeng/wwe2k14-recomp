r"""Self-test of `q pairs --coverage` (S111 PAIRCOV; tools/q/qpairs_cov.py). 0 game runs; a planted tree in a temp folder, read through
the production path: q ostate's collect (a reader over the tree), q pairs' table of that tree (qpairs.update), qpairs_cov.run.

    py -I tools\q\qpairs_cov_selftest.py

a  a one-block function with a citation comment in its body is PAIRED and not listed
b  a function of TWO blocks with the citation on the first block's lines: the first block PAIRED, the second (the planted unpaired
   block) UNPAIRED, listed with the first block's citation as its [L] fn-cite candidate
c  kTable rows: an `0x..` addr cell is PAIRED; a `sub_X +0xNN` addr cell is PAIRED with that function (qostate.table_facts reads both
   forms); a census-only cell is UNPAIRED with no [C] candidate
d  InstancedDraw stores: a no_blend-only store is NEVER-COMPARED; an alpha_test store of a function with no citation is UNPAIRED
e  OUTSIDE: a DrawState field store q ostate does not read (S111 OSTATEWIDE: a base whose type is not resolved, `auto& st = Pick();`) is
   listed with its function; a copy block (`rdr::DrawState st = other;`) is a site of its own (qostate_wide: read, UNPAIRED here)
f  PAIRED-NO-FACTS: a block whose only pair names a game function without q gstate facts
g  PLANTED TWIN: WWE_QPAIRSCOV_PLANT=any_fn_cite (every citation of a function stands for each of its blocks) must FAIL case b in a
   subprocess
h  S111 CDARMS: a bind's stores are one site per if-arm (tools/q/qcdarms.py): the wrestler arm PAIRED with the node its comment cites,
   not with the shadow pass its scope words (`colour pass only`) leave out; the material arm UNPAIRED with the citation as its [L]
   fn-cite candidate. Twin: WWE_QOSTATEWIDE_PLANT=no_arm (one store site per line, every citation of the function) must pair it.
Real line: `q pairs --coverage --no-game` on this tree exits 0 and prints its count line.
Exit 0 = all pass, 1 = a check failed, 2 = the planted twin was NOT caught.
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qostate as O     # noqa: E402
import qpairs_cov as C  # noqa: E402

FAILS = []
CPP = 'src/renderer_scene/syn_cov.cpp'
PS = O.PS

SYN_PS = r'''
#pragma once
namespace rdr_scene::pass_state {
enum class PassClass : uint8_t { Lit, Cut, Census, kCount };
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
    {"cut", 6, 128.f / 255.f, 6, 1, -1, 0, true, "stream census (GEQUAL 128); sub_8259E488 +0x74 (r6 = 128)"},
    {"census", 6, 1.f / 255.f, 6, 0, -1, 0, true, "stream census only"},
};
constexpr const PassState& Of(PassClass c) { return kTable[size_t(c)]; }
constexpr uint8_t ForwardZ(uint8_t vk_func) { return vk_func == 6 ? 3 : vk_func == 4 ? 1 : vk_func; }
inline PassClass ClassOf(const char* effect, bool hair_pass) {
  return PassClass::Lit;
}
}
'''
SYN_CPP = r'''
namespace rdr_scene {
void Syn::One(rdr::FrameBuilder& fb) {
  rdr::DrawState state{};
  state.depth_test = 1;   // game: sub_82000010 (the one-block draw)
  rdr::DrawOp op{};
  op.state = fb.State(state);
}
void Syn::Two(rdr::FrameBuilder& fb) {
  rdr::DrawState first{};
  first.depth_test = 1;   // game: sub_82000020 (the first block's draw)
  first.cull = 2;
  rdr::DrawOp op{};
  op.state = fb.State(first);
  rdr::DrawState second{};
  second.depth_write = 1;
  second.cull = 0;
  op.state = fb.State(second);
}
void Syn::NoFacts(rdr::FrameBuilder& fb) {
  rdr::DrawState s{};
  s.depth_test = 0;   // game: sub_82000099 (a game function with no q gstate fact)
  rdr::DrawOp op{};
  op.state = fb.State(s);
}
void Syn::Stores(rdr::FrameBuilder& fb) {
  InstancedDraw d;
  d.no_blend = true;
  Plain(d, fb);
}
void Syn::Tested(rdr::FrameBuilder& fb) {
  InstancedDraw e;
  e.alpha_test = 0;
  Plain(e, fb);
}
void Syn::Copy(const rdr::DrawState& other, rdr::FrameBuilder& fb) {
  rdr::DrawState st = other;
  st.cull = 2;
  rdr::DrawOp op{};
  op.state = fb.State(st);
}
const Bound& Syn::BindC(const Mat& mat, bool chr, int ps) {
  auto b = std::make_unique<Bound>();
  if (chr) {
    // game: sub_82000030 (the colour node); the shadow pass sub_82000040 does not take it (this bind serves the colour pass only)
    b->alpha_test = VariantHasTwin(ps);
    return *b;
  }
  b->alpha_test = mat.alpha_test;
  return *b;
}
void Syn::DrawC(const Mat& mat, rdr::FrameBuilder& fb, const rdr::DynamicState& dyn) {
  const Bound& b = BindC(mat, true, 3);
  rdr::DrawState state{};
  state.depth_test = 1;
  rdr::DrawOp op{};
  op.state = fb.State(state);
  op.dynamic = fb.Dynamic(b.alpha_test ? AlphaDynamic(dyn, pass_state::PassClass::Lit) : dyn);
}
void Syn::Unknown(rdr::FrameBuilder& fb) {
  auto& st = Pick(fb);
  st.depth_test = 1;
}
}
'''


def check(ok, what, detail=''):
    print(('PASS ' if ok else 'FAIL ') + what)
    if not ok:
        FAILS.append(what)
        if detail:
            print('     ' + str(detail).replace('\n', '\n     ')[:1500])


def tree():
    root = tempfile.mkdtemp(prefix='qpairscov_')
    for rel, text in ((PS, SYN_PS), (CPP, SYN_CPP)):
        p = os.path.join(root, rel.replace('/', os.sep))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, 'w', encoding='utf-8', newline='\n') as fh:
            fh.write(text)
    return root


def report(root):
    def read(rel):
        try:
            with open(os.path.join(root, rel.replace('/', os.sep)), encoding='utf-8') as fh:
                return fh.read()
        except OSError as e:
            raise O.OstateError(f'{rel}: not in the planted tree ({e})')
    facts, problems, _c = O.collect(read=read, files=[CPP])
    fns = sorted({f['fn'] for f in facts})
    have = {'sub_82000010', 'sub_82000020', 'sub_82BF5C58', 'sub_8259E488', 'sub_82000030', 'sub_82000040'}
    rep = C.run(facts, root, C.pair_rows_for(fns, root), {}, {}, set(), have, [], [], outside_rels=[CPP], passes={'sub_82000040': 'T_ShadowMap'})
    return rep, problems


def site(rep, fn, typ, nth=0):
    got = [s for s in rep['sites'] if s['fn'].endswith(fn) and s['type'] == typ]
    return got[nth] if len(got) > nth else None


def cases(rep, problems, only=None):
    st = lambda s: s and s['state']
    if only in (None, 'b'):
        a, b1, b2 = site(rep, 'Syn::One', 'block'), site(rep, 'Syn::Two', 'block', 0), site(rep, 'Syn::Two', 'block', 1)
        if only is None:
            check(st(a) == 'PAIRED' and ('sub_82000010' in {g for g, _x in a['pairs']}), 'a the one-block function is PAIRED with its citation', a)
        check(st(b1) == 'PAIRED', 'b the first block of Two (citation on its lines) is PAIRED', b1)
        check(st(b2) == 'UNPAIRED', 'b the second block of Two (planted unpaired) is UNPAIRED', b2)
        check(b2 is not None and any(c['game'] == 'sub_82000020' and c['conf'] == 'L' and c['src'] == 'fn-cite' for c in b2['candidates']),
              'b its candidate: the first block\'s citation, [L] fn-cite', b2 and b2['candidates'])
        if only:
            return
    lit, cut, cen = site(rep, 'kTable[lit]', 'table'), site(rep, 'kTable[cut]', 'table'), site(rep, 'kTable[census]', 'table')
    check(st(lit) == 'PAIRED', 'c the 0x addr cell row is PAIRED', lit)
    check(st(cut) == 'PAIRED' and 'sub_8259E488' in cut['game'], 'c the sub_X addr cell row is PAIRED with sub_8259E488 (qostate reads the sub_ form)', cut)
    check(st(cen) == 'UNPAIRED' and not any(c['conf'] == 'C' for c in cen['candidates']), 'c the census-only row is UNPAIRED, no [C] candidate', cen)
    s1, s2 = site(rep, 'Syn::Stores', 'store'), site(rep, 'Syn::Tested', 'store')
    check(st(s1) == 'NEVER-COMPARED', 'd the no_blend-only store is NEVER-COMPARED', s1)
    check(st(s2) == 'UNPAIRED', 'd the alpha_test store of an uncited function is UNPAIRED', s2)
    out = {(x['file'], x['fn'].split('::')[-1]): x['lines'] for x in rep['outside']}
    check(('src/renderer_scene/syn_cov.cpp', 'Unknown') in out and len(out) == 1, 'e the unresolved store is OUTSIDE (and nothing ostate read is)', rep['outside'])
    cp = site(rep, 'Syn::Copy', 'block')
    check(st(cp) == 'UNPAIRED' and cp['facts'][0].get('wide') == 'copy', 'e the copy block is a site of its own (q ostate wide), UNPAIRED', cp)
    nf = site(rep, 'Syn::NoFacts', 'block')
    check(st(nf) == 'PAIRED-NO-FACTS', 'f a pair whose game function has no gstate fact is PAIRED-NO-FACTS', nf)
    check(rep['counts']['PAIRED'] == 5 and rep['counts']['UNPAIRED'] == 6,
          'counts: 5 paired (One, Two first, lit, cut, the bind\'s wrestler arm), 6 unpaired (+ the copy, the material arm, DrawC)', rep['counts'])
    wr_ok, ma_ok, wr, ma = arms(rep)
    check(wr_ok, 'h the bind\'s wrestler arm: PAIRED with its comment\'s node only (the shadow pass out of the arm\'s scope)', wr)
    check(ma_ok, 'h the bind\'s material arm: UNPAIRED, the wrestler arm\'s citation its [L] fn-cite candidate', ma)


def arms(rep):
    """-> (the wrestler arm right, the material arm right, its sites, its sites)"""
    binds = [s for s in rep['sites'] if s['fn'].endswith('Syn::BindC') and s['type'] == 'store']
    wr = [s for s in binds if any('VariantHasTwin' in str(f['value']) for f in s['facts'])]
    ma = [s for s in binds if any('mat.alpha_test' in str(f['value']) for f in s['facts'])]
    return (len(wr) == 1 and wr[0]['state'] == 'PAIRED' and {g for g, _x in wr[0]['pairs']} == {'sub_82000030'},
            len(ma) == 1 and ma[0]['state'] == 'UNPAIRED' and any(c['game'] == 'sub_82000030' and c['src'] == 'fn-cite' for c in ma[0]['candidates']),
            wr, ma)


def main():
    if os.environ.get('WWE_QPAIRSCOV_CASE') == 'b':
        root = tree()
        try:
            rep, problems = report(root)
            cases(rep, problems, 'b')
        finally:
            shutil.rmtree(root, ignore_errors=True)
        return 1 if FAILS else 0
    root = tree()
    try:
        rep, problems = report(root)
        cases(rep, problems)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    env = dict(os.environ, WWE_QPAIRSCOV_PLANT='any_fn_cite', WWE_QPAIRSCOV_CASE='b')
    r = subprocess.run([sys.executable, '-I', os.path.abspath(__file__)], env=env, capture_output=True, text=True, timeout=300)
    caught = r.returncode != 0 and 'FAIL b the second block of Two' in r.stdout
    print(('PASS ' if caught else 'NOT CAUGHT ') + 'g planted twin any_fn_cite fails case b')
    root = tree()
    os.environ['WWE_QOSTATEWIDE_PLANT'] = 'no_arm'
    try:
        arm_caught = not arms(report(root)[0])[1]
    finally:
        os.environ['WWE_QOSTATEWIDE_PLANT'] = ''
        shutil.rmtree(root, ignore_errors=True)
    print(('PASS ' if arm_caught else 'NOT CAUGHT ') + 'h planted twin no_arm (one site per store line) pairs the material arm')
    caught = caught and arm_caught
    q = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'q.py'), 'pairs', '--coverage', '--no-game'], capture_output=True, text=True,
                       encoding='utf-8', errors='replace', timeout=600, cwd=os.path.dirname(os.path.dirname(HERE)))
    check(q.returncode == 0 and 'q pairs --coverage:' in q.stdout, 'real line: q pairs --coverage --no-game on this tree', (q.stdout + q.stderr)[-800:])
    print(f'{len(FAILS)} failed, {0 if caught else 1} planted twin not caught')
    return 2 if not caught else 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
