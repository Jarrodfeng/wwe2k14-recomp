// S111 CHAMBERFADE unit (docs\research\s111\CHAMBERFADE.md): the Elimination Chamber's category-35 / 36 node (sub_826F8F88) blends every piece by its MdlAlpha ([model+252]) with an alpha test GEQUAL 1/255,
// and writes the near-wall fade there (the front wall k 7: 0 when the camera is within 2000 of the fade point). Ours: rf_props.cpp sets the node's flags (stage_node_state.h StageNodeObjectFlags) and then the
// tint (StageNodeTint) on the tint rf_props' general rule gave. 0 game runs, no files.
//   pwsh -NoProfile -File tools\weapons\build_chamberfade_unit.ps1
// --plant = the rule before (MdlAlpha 0 leaves tint 1: the front wall drawn solid): it must FAIL.
#include "../../src/renderer_scene/stage_node_state.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

int g_fail = 0;
bool g_plant = false;
void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

namespace W = rdr_scene::weapons;
namespace sn = rdr_scene::stage_node;

// The return addresses of the slot-13 submits (bctrl + 4) [C listings]: sub_826F8F88 +0x564 (k 7, category 36), +0x6C4 (k 5), +0x4FC (k 4 / 6), +0x7BC (k 8); sub_826F8958 +0x2E8 (category 3).
constexpr uint32_t kLrWall = 0x826F8F88 + 0x568, kLrGlass = 0x826F8F88 + 0x6C8, kLrFrame = 0x826F8F88 + 0x500, kLrSpot = 0x826F8F88 + 0x7C0, kLrCat3 = 0x826F8958 + 0x2EC;

uint32_t OwnRow(uint32_t k, uint32_t lr) { return W::PackStageFlags(k, true, false, W::OwnCallerClass(lr)); }

// rf_props.cpp ApplyWeaponRows for an own stage row: the general tint rule (:345), then the node's flags (:346) and StageNodeTint.
float TintOf(uint32_t k, uint32_t lr, float mdl_alpha) {
  float tint = 1.f;
  if (mdl_alpha > 0.f && mdl_alpha < 1.f) tint = mdl_alpha;
  const uint32_t flags = sn::StageNodeObjectFlags(OwnRow(k, lr), 6, k);
  return sn::StageNodeTint(flags, mdl_alpha, tint, g_plant);
}

}  // namespace

int main(int argc, char** argv) {
  g_plant = argc > 1 && std::strcmp(argv[1], "--plant") == 0;
  Check(TintOf(7, kLrWall, 0.f) == 0.f, "the front wall k 7 at MdlAlpha 0 (camera within 2000): tint 0, blended to nothing like the game's GEQUAL 1/255 test");
  Check(TintOf(7, kLrWall, 0.35f) == 0.35f, "k 7 mid-fade 0.35: tint 0.35");
  Check(TintOf(4, kLrFrame, 0.2f) == 0.2f && TintOf(6, kLrFrame, 0.2f) == 0.2f, "the near frames / doors at the floored fade 0.2");
  Check(TintOf(5, kLrGlass, 0.9f) == 0.9f, "the far pod glass at 0.9");
  Check(TintOf(4, kLrFrame, 1.f) == 1.f && TintOf(7, kLrWall, 1.5f) == 1.f, "MdlAlpha 1 (the far sides) and above (a shot floor > 100 %): solid");
  Check(TintOf(8, kLrSpot, 0.5f) == 0.5f, "the spotlight k 8 (additive) at 0.5");
  Check(TintOf(7, kLrWall, std::nanf("")) == 1.f, "a NaN MdlAlpha keeps the general rule's tint");
  Check(TintOf(4, kLrCat3, 0.f) == 1.f, "a piece the category-3 node draws (no node state) at 0: the general rule, tint 1");
  Check(sn::StageNodeTint(0u, 0.f, 1.f) == 1.f, "a row without node flags: untouched");
  if (g_fail) std::printf("FAIL  %d check(s) failed%s\n", g_fail, g_plant ? " (planted twin: MdlAlpha 0 leaves tint 1)" : "");
  else std::printf("PASS  chamberfade_unit\n");
  return g_fail ? 1 : 0;
}
