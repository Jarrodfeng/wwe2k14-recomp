// S111 PODDOOR unit (docs\research\s111\PODDOOR.md): the gm manager draws a set object in category 36 (sub_827C79A8: z write 0, blend code 0) when its key is 1500..1755 or its MdlAlpha is below 1
// (sub_827C90A8), else in category 23 (sub_827C7888: z write 1). Ours gives a category-36 row the node's object flags (gm_node_state.h GmNodeObjectFlags). 0 game runs, no files.
//   pwsh -NoProfile -File tools\weapons\build_gmglass_unit.ps1
// --plant = the rule before (the material's state for every gm row: the chamber glass in the opaque list writing depth): it must FAIL.
#include "../../src/renderer_scene/gm_node_state.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int g_fail = 0;
void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

namespace ss = rdr_scene::splat_state;
namespace gn = rdr_scene::gm_node;

}  // namespace

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::strcmp(argv[1], "--plant") == 0;
  const uint32_t kNode = uint32_t(rdr_scene::kObjNoDepthWrite) | uint32_t(rdr_scene::kObjNodeAlpha) | uint32_t(rdr_scene::kObjNodeLate);
  // run 015's rows: GMGA/0021 keys 1500 + 3 i (chem_glass02_ma), MdlAlpha 0.2 (near pods) / 0.9 (far pods)
  const uint32_t near_glass = gn::GmNodeObjectFlags(1500, 0.2f, plant), far_glass = gn::GmNodeObjectFlags(1521, 0.9f, plant);
  Check(near_glass == kNode && far_glass == kNode, "the chamber glass (keys 1500 / 1521 at 0.2 / 0.9): the category-36 node's flags");
  Check(!ss::DepthWriteOf(far_glass, true, true) && ss::NodeBlendOf(far_glass) == 1 && ss::LateNodeOf(far_glass), "the front end: no depth write, blended SRC_ALPHA / INV_SRC_ALPHA, after the wrestlers");
  Check(gn::GmNodeObjectFlags(1755, 1.f, plant) == kNode, "a tail key (1755) at MdlAlpha 1: category 36 too (sub_827C90A8's second loop takes every tail object)");
  Check(gn::GmNodeObjectFlags(1000, 0.5f, plant) == kNode, "a head key faded below 1: category 36 (the first loop)");
  Check(gn::GmNodeObjectFlags(1000, 1.f, plant) == 0u && gn::GmNodeObjectFlags(1255, 2.f, plant) == 0u, "a head key at MdlAlpha >= 1: category 23, the material's state (z write on)");
  Check(gn::GmNodeObjectFlags(1256, 1.f, plant) == 0u && gn::GmNodeObjectFlags(1756, 1.f, plant) == 0u, "keys outside the two ranges at alpha 1: no node flags");
  Check(gn::GmNodeObjectFlags(1000, std::nanf(""), plant) == 0u, "a NaN MdlAlpha on a head key stays in category 23 (the game's < 1 compare is false)");
  if (g_fail) std::printf("FAIL  %d check(s) failed%s\n", g_fail, plant ? " (planted twin: the material's state for every gm row)" : "");
  else std::printf("PASS  gmglass_unit\n");
  return g_fail ? 1 : 0;
}
