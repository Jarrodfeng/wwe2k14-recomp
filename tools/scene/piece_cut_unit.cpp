// S108 HAIR: unit of the piece draw rule (src\renderer_scene\piece_draw_rule.h) and its pass class (pass_state.h ClassOf), with planted twins (the old rules). Pure: no GPU, no game files.
//   pwsh -NoProfile -File tools\scene\build_piece_cut_unit.ps1
// The game's answer [C]: sub_8259E488 draws every SUBM piece at alpha ref r6 = 128 (+0x74; both callers pass 128: sub_8259FAE8 +0x328, sub_825A3908 +0x3D8 / +0x478) except the ids it compares at +0x98 .. +0x11C
// (1, 20000, 33, 20002, 21000, 21001, 21002, 21010, 21011, 21020, 21030, 30000, 30001, 30002 -> ref 1), and writes z write := 1 after every piece (+0x1B4). The stream census row "GEQUAL 128, dw 1" is pass_state.h see_through_cut.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "renderer_scene/pass_state.h"
#include "renderer_scene/piece_draw_rule.h"

namespace pd = rdr_scene::piece_draw;
namespace ps = rdr_scene::pass_state;

static int g_fail = 0;
static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fail;
}

// planted twins
static bool OldPieceCut(bool, int, bool) { return false; }                     // before S108: no piece mesh was ever cut (blended, untested, no depth write)
static bool ListMissing20002(int id) { return id != 20002 && pd::AlphaRefIsOne(id); }   // a list that lost the lid layer
static bool CutByEffectOnly(bool answers, int key, bool sorted) { (void)key; return answers && sorted; }   // keyed on "a translucent piece" alone: the lid layer and the body would be cut too

template <class CutFn, class RefOneFn>
static int RunCases(CutFn cut, RefOneFn ref_one, bool print) {
  int bad = 0;
  auto c = [&](bool ok, const char* what) { if (print) Check(ok, what); else bad += ok ? 0 : 1; };
  // the listing's 14 ref-1 ids, and their neighbours keep 128
  const int ones[] = {1, 20000, 33, 20002, 21000, 21001, 21002, 21010, 21011, 21020, 21030, 30000, 30001, 30002};
  bool all = true;
  for (int id : ones) all = all && ref_one(id);
  c(all, "the 14 ids of sub_8259E488 +0x98 .. +0x11C take alpha ref 1");
  const int keep[] = {2, 26, 27, 28, 29, 32, 34, 10000, 10010, 10500, 10501, 10999, 20001, 21003, 21012, 21031, 30003};
  bool none = true;
  for (int id : keep) none = none && !ref_one(id);
  c(none, "every other piece id keeps alpha ref 128 (hair 10000.., 10500 / 10501, 26-29, 32, 20001, the ids next to the list)");
  // the mesh rule
  c(cut(true, 10000, true), "a stock hair piece's translucent mesh (10000, yCh_Aniso_trans) is cut: GEQUAL 128 + depth write");
  c(cut(true, 29, true), "a translucent entry-29 piece is cut");
  c(!cut(true, 20002, true), "the lid / lash layer 20002 is not cut (ref 1)");
  c(!cut(true, 21011, true), "a 21000-range face piece is not cut (ref 1)");
  c(!cut(true, 0, true), "the body's own translucent mesh (src_key 0) is not cut (the body path, ref 1)");
  c(!cut(false, 10000, true), "a mesh that answers no piece row (not a wrestler pac entry, src_kind != 2) is not cut; a guest piece model carries its id in src_key (S111 GUESTKEY): the cases above are its cases");
  c(!cut(true, 10000, false), "an opaque piece mesh is not cut (the opaque node sets no alpha test)");
  return bad;
}

// S111 GUESTKEY: the S111 CAWHAIR guest route (GuestPieceCut: the id read from Object::pad beside a part-index key) is gone. A guest piece model carries its piece id in src_key, so RunCases is its rule;
// tools\scene\guest_model_unit.cpp CheckBlend builds one (a kObjDrawnByOwner piece, id 10000) and asks PieceCut, with the planted twins of the old stamp (43) and the old mask (66).

int main() {
  RunCases([](bool a, int k, bool s) { return pd::PieceCut(a, k, s); }, [](int id) { return pd::AlphaRefIsOne(id); }, true);
  Check(pd::kPieceCutRef > 0.5019f && pd::kPieceCutRef < 0.5020f, "the reference is 128/255");
  // the pass class: a cut is see_through_cut whatever the effect name; without it nothing changes
  Check(ps::ClassOf("yCh_Aniso_trans", true, true) == ps::PassClass::SeeThroughCut, "a cut hair pass is see_through_cut");
  Check(ps::ClassOf("yCh_trans", true, true) == ps::PassClass::SeeThroughCut, "a cut yCh_trans piece is see_through_cut (not see_through)");
  const ps::PassState& st = ps::Of(ps::PassClass::SeeThroughCut);
  Check(st.alpha_test_func == 6 && st.alpha_ref > 0.5019f && st.alpha_ref < 0.5020f && st.z_write == 1 && st.tested_blends && ps::ForwardZ(st.game_z_func) == 3, "see_through_cut = GEQUAL 128/255, z write 1, blended, LEQUAL in our forward Z");
  Check(ps::ClassOf("yCh_Aniso_trans", true) == ps::PassClass::Hair && ps::ClassOf("yCh_trans", true) == ps::PassClass::SeeThrough && ps::ClassOf("yCh_skin", false) == ps::PassClass::Lit, "without a cut the classes are as before");
  // planted twins: each must fail at least one case
  const int t1 = RunCases(OldPieceCut, [](int id) { return pd::AlphaRefIsOne(id); }, false);
  const int t2 = RunCases([](bool a, int k, bool s) { return a && s && k > 0 && !ListMissing20002(k); }, ListMissing20002, false);
  const int t3 = RunCases(CutByEffectOnly, [](int id) { return pd::AlphaRefIsOne(id); }, false);
  std::printf("planted twin old_rule (no cut): %d case(s) fail\nplanted twin list_missing_20002: %d case(s) fail\nplanted twin cut_by_effect_only: %d case(s) fail\n", t1, t2, t3);
  Check(t1 > 0, "planted twin old_rule is caught");
  Check(t2 > 0, "planted twin list_missing_20002 is caught");
  Check(t3 > 0, "planted twin cut_by_effect_only is caught");
  std::printf(g_fail ? "piece_cut_unit: FAIL (%d)\n" : "piece_cut_unit: PASS\n", g_fail);
  return g_fail ? 1 : 0;
}
