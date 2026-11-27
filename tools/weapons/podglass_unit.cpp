// S111 PODGLASS unit (docs\research\s111\PODGLASS.md): the Elimination Chamber's category-35 / 36 node (sub_826F8F88) draws its pieces blended with z write 0; ours tags its own rows by the caller of the slot-13
// dispatcher (weapon_rows.h OwnCallerClass -> kStageLrOwnNode) and gives them the node's object flags (stage_node_state.h StageNodeObjectFlags). 0 game runs, no files.
//   pwsh -NoProfile -File tools\weapons\build_podglass_unit.ps1
// --plant = the rule before (the material's state for every stage row): it must FAIL. --plant-order = the see-through order before the review (the node's alpha draws in rank 0, before the
// wrestlers' translucent pieces: a chain fence drawn with no depth write, then a wrestler's hair over it): it must FAIL.
// S111 CHAMBERGLASS: --plant-stage-a = the ranks before CHAMBERGLASS (the node's alpha draws before every additive piece, the arena's `a` group among them, which the game draws in
// category 35, before CDTA's 36): it must FAIL.
#include "../../src/renderer_scene/stage_node_state.h"
#include "../../src/renderer_scene/see_through_order.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_fail;
}

namespace W = rdr_scene::weapons;
namespace ss = rdr_scene::splat_state;
namespace sn = rdr_scene::stage_node;

// The return addresses of the slot-13 submits (bctrl + 4) in the two chamber draws [C listings]: sub_826F8F88 +0x6C4 (the glass, category 36), +0x1C0 (a fence, category 35); sub_826F8958 +0x2E8 (a door, category 3).
constexpr uint32_t kLrGlass = 0x826F8F88 + 0x6C8, kLrFence = 0x826F8F88 + 0x1C4, kLrCat3 = 0x826F8958 + 0x2EC;

// As the tracker packs an own stage call of piece k made from `lr` (weapon_rows_core.h).
uint32_t OwnRow(uint32_t k, uint32_t lr) { return W::PackStageFlags(k, true, false, W::OwnCallerClass(lr)); }

}  // namespace

int main(int argc, char** argv) {
  const bool plant = argc > 1 && std::strcmp(argv[1], "--plant") == 0;
  rdr_scene::g_plant_late_node_off.store(argc > 1 && std::strcmp(argv[1], "--plant-order") == 0);
  rdr_scene::g_plant_late_before_additive.store(argc > 1 && std::strcmp(argv[1], "--plant-stage-a") == 0);
  const uint32_t kNode = uint32_t(rdr_scene::kObjNoDepthWrite) | uint32_t(rdr_scene::kObjNodeLate);   // the node's z write 0 + its category after 33
  Check(W::OwnCallerClass(kLrGlass) == W::kStageLrOwnNode && W::OwnCallerClass(kLrFence) == W::kStageLrOwnNode, "the chamber's category-35 / 36 submits are the own-node class");
  Check(W::OwnCallerClass(W::kOwnChamberSeeNodeLo) == W::kStageLrOwnNode && W::OwnCallerClass(W::kOwnChamberSeeNodeHi - 4) == W::kStageLrOwnNode && W::OwnCallerClass(W::kOwnChamberSeeNodeHi) == W::kStageLrOther &&
            W::OwnCallerClass(W::kOwnChamberSeeNodeLo - 4) == W::kStageLrOther, "the range is sub_826F8F88's 2024 bytes, no more");
  Check(W::OwnCallerClass(kLrCat3) == W::kStageLrOther && W::OwnCallerClass(0x826FA0B8 + 0x100) == W::kStageLrCage && W::OwnCallerClass(0) == W::kStageLrNone && W::OwnCallerClass(0x82345678) == W::kStageLrOther,
        "the chamber's category-3 node, the cage node, no caller and any other caller keep their classes");
  Check(W::OwnOtherCaller(W::kStageLrOther) && W::OwnOtherCaller(W::kStageLrOwnNode) && !W::OwnOtherCaller(W::kStageLrCage) && !W::OwnOtherCaller(W::kStageLrNone),
        "map B and the census own-other family: class 3 and the own-node class, never the cage node or none");
  {
    const uint32_t f = sn::StageNodeObjectFlags(OwnRow(5, kLrGlass), 6, 5, plant);
    Check(f == (kNode | uint32_t(rdr_scene::kObjNodeAlpha)), "the pod glass (k 5, category 36): z write off + alpha blend (the node's code 0), a node after category 33");
    Check(!ss::DepthWriteOf(f, true, true) && ss::NodeBlendOf(f) == 1, "the front end: no depth write, blended SRC_ALPHA / INV_SRC_ALPHA");
  }
  {
    const uint32_t f = sn::StageNodeObjectFlags(OwnRow(1, kLrFence), 6, 1, plant);
    Check(f == (kNode | uint32_t(rdr_scene::kObjNodeAlpha)), "a chain fence (k 1, category 35): the same node state");
  }
  {
    const uint32_t f = sn::StageNodeObjectFlags(OwnRow(8, kLrGlass), 6, 8, plant);
    Check(f == (kNode | uint32_t(rdr_scene::kObjNodeAdditive)) && ss::NodeBlendOf(f) == 2, "the spotlight (k 8): the node's code 1, additive");
  }
  {   // The see-through order (FrontEnd::Record, Game order: the item's blend = NodeBlendOf, its late flag = LateNodeOf, SeeRank / SortSeeThrough): the game draws categories 35 / 36 after every
      // category-33 draw (the wrestlers' translucent pieces), so a fence / pane / frame with no depth write must come after a wrestler's hair, never before it (the hair would paint over the wire).
    struct It { std::string name; rdr_scene::SeeKey k; };
    auto node_item = [&](const char* name, uint32_t k, uint32_t lr) {
      const uint32_t f = sn::StageNodeObjectFlags(OwnRow(k, lr), 6, k, plant);
      It i{name, {}};
      i.k.blend = ss::NodeBlendOf(f) ? ss::NodeBlendOf(f) : uint8_t(1), i.k.late_node = ss::LateNodeOf(f);
      return i;
    };
    It hair{"hair", {}}, stage{"stage", {}}, glow{"glow", {}}, beam{"beam", {}};
    hair.k.blend = 1, hair.k.character = true;   // a wrestler's translucent piece (yCh_*): the character rank
    stage.k.blend = 1;                           // the stage's own blended piece (an arena glass / net): rank 0
    glow.k.blend = 2, glow.k.cutout = 2;         // an arena `a` slot piece (bg21 slot 163 beam_glow01: the loader's slot blend, cutout 2): the game's category 35
    beam.k.blend = 2;                            // an additive piece that is not the loader's slot blend: the same additive rank, before the slot pieces
    std::vector<It> items = {node_item("fence", 3, kLrFence), node_item("glass", 5, kLrGlass), node_item("spot", 8, kLrGlass), hair, stage, glow};   // record order: the chamber's rows first
    rdr_scene::SortSeeThrough(rdr_scene::SeeOrder::Game, items, [](const It& i) { return i.k; });
    std::string got;
    for (const It& i : items) got += (got.empty() ? "" : " ") + i.name;
    std::printf("      order: %s\n", got.c_str());
    Check(got == "stage hair glow fence glass spot", "Game order (S111 CHAMBERGLASS): the stage's piece, the wrestler's hair, the arena's `a` piece (category 35), then the chamber's fence and glass in their record (draw) order, the spotlight last (CDTA's additive)");
    std::vector<It> plain = {glow, beam, hair, stage};   // no late node in the frame: the order before CHAMBERGLASS (the additive rank only changed its number)
    rdr_scene::SortSeeThrough(rdr_scene::SeeOrder::Game, plain, [](const It& i) { return i.k; });
    std::string got_plain;
    for (const It& i : plain) got_plain += (got_plain.empty() ? "" : " ") + i.name;
    std::printf("      order with no late node: %s\n", got_plain.c_str());
    Check(got_plain == "stage hair beam glow", "no late node: alpha, character, then the additives (the loader's slot pieces after the others), as before");
  }
  Check(sn::StageNodeObjectFlags(OwnRow(4, kLrCat3), 6, 4, plant) == 0u, "a pod frame the category-3 node draws: no node flags (its material's state)");
  Check(sn::StageNodeObjectFlags(W::PackStageFlags(5, false, true, W::kStageLrCell), 3, 5, plant) == 0u, "a slot-32 cell call (class 2 = cell, not own): no node flags");
  Check(sn::StageNodeObjectFlags(W::PackEventFlags(5) | (W::kStageLrOwnNode << W::kStageLrShift), 6, 5, plant) == 0u, "a row that is not a stage call: no node flags");
  if (g_fail) std::printf("FAIL  %d check(s) failed%s\n", g_fail, plant ? " (planted twin: the material's state for every stage row)" : "");
  else std::printf("PASS  podglass_unit\n");
  return g_fail ? 1 : 0;
}
