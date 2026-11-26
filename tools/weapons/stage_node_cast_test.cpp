// stage_node_cast_test: the offline proof of S111 CHAMBERSHADOW (docs\research\s111\CHAMBERSHADOW.md): which stage structure draws the game casts into its shadow maps, per the node that submitted them
// (src\renderer_scene\stage_struct_nodes.h; the recorder's kCallStageCaster bit, weapon_rows_core.h; weapon_plan.h CastsMapB / CastsPropsMap). 0 game runs, no SDK, no game memory: it runs the SHIPPED code.
//   stage_node_cast_test [--plant node_blind]   the plant ignores the node (the rule before CHAMBERSHADOW: every own submit but the cage node's casts): the run must FAIL (exit 1)
//   node.of          caller -> node: inside sub_825DFA78 = a CWeapon piece; the cage / cell / rope / ladder lights / Inferno / chamber nodes by their function ranges (the cell's two submit helpers too); 0 and an unknown caller = Unknown
//   node.casts       only the CWeapon piece's node casts (category 26 / 41 is walked, it draws under the flag); every structure node does not, nor an unknown caller
//   cast.structure   a structure node's own row (chamber 3 / 35-36, ladder rope, Inferno, cell 27, unknown) is in neither map B nor the props map                         <- node_blind
//   cast.cweapon     a CWeapon piece's own row is in both maps; the cage node's own row, a slot-32 row and a shadow-pass row are in neither
#include <cstdio>
#include <string>

#include "../../src/renderer_scene/weapon_plan.h"

namespace W = rdr_scene::weapons;
namespace S = rdr_scene::weapons::stg_nodes;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  std::printf("%s  %-16s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

// Callers inside each node's slot-1 function (the CWeapon one is the return address of its slot-13 call at +0x4B0).
static constexpr uint32_t kLrCWeapon = 0x825DFF2C, kLrCage = 0x826FA500, kLrCell = 0x82660000, kLrRope = 0x825DB750, kLrInferno = 0x82669AC0, kLrChamber3 = 0x826F8A00, kLrChamberSee = 0x826F9100,
                          kLrUnknown = 0x82345678;
// S111 CHAMBERSHADOW review: the other own submitters of a holder model [C listings, bctrl + 4]: the ladder lights sub_825DB7B8 +0x11C (category 35), the cell's helpers sub_8265F158 +0x58 and
// sub_8265FAE0 +0x198 (called only by the cell's category-27 / 36 nodes, which return under the flag).
static constexpr uint32_t kLrLights = 0x825DB7B8 + 0x120, kLrCellPiece = 0x8265F158 + 0x5C, kLrCellWalls = 0x8265FAE0 + 0x19C;

// The row the recorder writes for an own (or slot-32) call of a stage piece: weapon_rows_core.h's PackStageFlags + stg_nodes::CasterBit.
static uint32_t RowFlags(bool own, uint32_t caller) {
  const uint32_t cls = own ? W::OwnCallerClass(caller) : W::kStageLrOther;
  return W::PackStageFlags(3, own, false, cls) | S::CasterBit(own, caller);
}
static bool InMapB(uint32_t flags) {
  W::Item it;
  it.stage = true, it.id = 6, it.entry = 3;
  W::WeaponCall w{};
  w.slot_flags = flags;
  return W::CastsMapB(it, w);
}
static bool InProps(uint32_t flags) {
  W::Item it;
  it.stage = true, it.id = 6, it.entry = 3;
  W::WeaponCall w{};
  w.slot_flags = flags;
  return W::CastsPropsMap(it, w);
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  if (!plant.empty() && plant != "node_blind") {
    std::printf("unknown plant %s\n", plant.c_str());
    return 2;
  }
  S::g_plant_node_blind = plant == "node_blind";
  {
    const bool ok = S::NodeOfCaller(kLrCWeapon) == S::Node::CWeaponPiece && S::NodeOfCaller(0x825DFA78) == S::Node::CWeaponPiece && S::NodeOfCaller(0x825DFF58) == S::Node::Unknown &&
                    S::NodeOfCaller(kLrCage) == S::Node::CageNode && S::NodeOfCaller(kLrCell) == S::Node::CellNode && S::NodeOfCaller(kLrRope) == S::Node::LadderRope &&
                    S::NodeOfCaller(kLrInferno) == S::Node::InfernoNode && S::NodeOfCaller(kLrChamber3) == S::Node::ChamberFloorNode && S::NodeOfCaller(kLrChamberSee) == S::Node::ChamberSeeNode &&
                    S::NodeOfCaller(0) == S::Node::Unknown && S::NodeOfCaller(kLrUnknown) == S::Node::Unknown && S::NodeOfCaller(kLrLights) == S::Node::LadderLights &&
                    S::NodeOfCaller(kLrCellPiece) == S::Node::CellNode && S::NodeOfCaller(kLrCellWalls) == S::Node::CellNode;
    Check(ok, "node.of", ok ? "" : "a caller was given the wrong node");
  }
  {
    bool ok = S::CastsIntoShadowMaps(S::Node::CWeaponPiece);
    for (S::Node n : {S::Node::Unknown, S::Node::CageNode, S::Node::CellNode, S::Node::LadderRope, S::Node::LadderLights, S::Node::InfernoNode, S::Node::ChamberFloorNode, S::Node::ChamberSeeNode}) ok = ok && !S::CastsIntoShadowMaps(n);
    ok = ok && S::CategoryOf(S::Node::CWeaponPiece) == 26 && S::CategoryOf(S::Node::ChamberFloorNode) == 3 && S::CategoryOf(S::Node::InfernoNode) == 23 && S::CategoryOf(S::Node::LadderLights) == 35 &&
         !S::InShadowPass(36) && !S::InShadowPass(35) && S::InShadowPass(41);
    Check(ok, "node.casts", ok ? "" : "only the CWeapon piece's node is in a walked category and draws under the flag");
  }
  {
    bool ok = true;
    for (uint32_t lr : {kLrChamber3, kLrChamberSee, kLrRope, kLrLights, kLrInferno, kLrCell, kLrCellPiece, kLrCellWalls, kLrUnknown}) ok = ok && !InMapB(RowFlags(true, lr)) && !InProps(RowFlags(true, lr)) && (RowFlags(true, lr) & W::kCallStageCaster) == 0;
    Check(ok, "cast.structure", ok ? "" : "a structure node's own row (chamber / rope / Inferno / cell / unknown) was cast");
  }
  {
    const uint32_t piece = RowFlags(true, kLrCWeapon);
    const bool ok = (piece & W::kCallStageCaster) != 0 && InMapB(piece) && InProps(piece) && !InMapB(RowFlags(true, kLrCage)) && !InProps(RowFlags(true, kLrCage)) && !InMapB(RowFlags(false, kLrCWeapon)) &&
                    RowFlags(false, kLrCWeapon) == W::PackStageFlags(3, false, false, W::kStageLrOther) && !InMapB(piece | W::kCallShadowPass) && W::StageEntry(piece) == 3 && W::IsStageOwn(piece) &&
                    W::StageLrClass(piece) == W::kStageLrOther;
    Check(ok, "cast.cweapon", ok ? "" : "the CWeapon piece's own row must cast into both maps; the cage node, slot-32 and shadow rows never");
  }
  std::printf("%s\n", g_fail ? "FAILED" : "ALL PASS");
  return g_fail ? 1 : 0;
}
