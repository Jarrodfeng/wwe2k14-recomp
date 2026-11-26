// S111 STAGELESS2 unit (docs\research\s111\STAGELESS2.md): the rules of src\renderer_scene\stageless_scene.h, the SHIPPED functions live_scene_draw.cpp (NoteStage / BuildTerms), live_scene_draw.h
// (LastBuildHadStage) and scene_host.cpp (the never_fed mask) call. 0 game runs, no SDK. Built and run by tools\scene\stageless_check.py.
//   stageless_unit [--plant N]   1 = no stage always blocks READY (the old rule) -> stageless.belt_ours; 2 = the walk is not asked -> stageless.menu_not_ours; 3 = the stage-only inputs stay required -> stageless.required
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../src/renderer_scene/stageless_scene.h"
#include "../../src/renderer_scene/cam_gate.h"

namespace sl = rdr_scene::stageless;

static int g_fail = 0;
static void Check(bool ok, const char* name, const char* why) {
  std::printf("%s  %-28s %s\n", ok ? "PASS" : "FAIL", name, why);
  if (!ok) ++g_fail;
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--plant") == 0) sl::g_plant = std::atoi(argv[i + 1]);
  const uint32_t none = sl::NoStageHeldBits(true);    // the latch read [0x83763544] == 0
  const uint32_t held = sl::NoStageHeldBits(false);   // a stage held, or the word unreadable
  const uint32_t others = 1u | rdr_scene::kSceneFactMask | 24u | 32u;   // bit 0 kSceneStagePlaying (scene_record.h), 1 / 2 cam_gate.h, 3 / 4 the retired A3 pair, 5 kSceneSnapshot (rows_snapshot.h)
  Check(none == sl::kSceneNoStageHeld && held == 0u && (sl::kSceneNoStageHeld & others) == 0u, "stageless.bits", "bit 6 alone, clear of the SVO / cam_gate / retired A3 / snapshot bits");
  {   // a creations menu after Create a Title: no stage held and no model walk on any record: the no-stage term still blocks (the flat path)
    sl::StagelessGate g;
    bool any = false;
    for (int r = 0; r < 6; ++r) any = g.Step(none, false) || any;
    Check(!any && sl::NoStageBlocks(false, any), "stageless.menu_not_ours", "a no-stage record with no walk never waives READY's no-stage term");
  }
  {   // Create a Title (sequence 29): no stage held, the belt walks; one record missing its bit mid-scene is still the scene; three records with no walk end it
    sl::StagelessGate g;
    const bool a = g.Step(none, true), b = g.Step(none, true), c = g.Step(none, false), d = g.Step(none, true);
    const bool e1 = g.Step(none, false), e2 = g.Step(none, false), e3 = g.Step(none, false);
    Check(a && b && c && d && e1 && e2 && !e3 && !sl::NoStageBlocks(false, d), "stageless.belt_ours", "the belt's walk with no stage held waives the term (READY, ours: the backdrop rows are early scene rows); 3 walkless records close it");
  }
  {   // a match / a scene change: a stage held (or the holder unreadable): no waiver whatever walks; a stage never blocks
    sl::StagelessGate g;
    const bool w1 = g.Step(held, true), w2 = g.Step(held, true);
    sl::StagelessGate g2;
    const bool open = g2.Step(none, true), closed_by_stage = g2.Step(held, true);
    Check(!w1 && !w2 && open && !closed_by_stage && sl::NoStageBlocks(false, false) && !sl::NoStageBlocks(true, false) && !sl::NoStageBlocks(true, true), "stageless.match_unchanged",
          "with a stage held the old rule stands (S103 ONF M2: a record with no stage is a scene change)");
  }
  {   // the never_fed mask: with no stage held the five stage-only structural inputs are not required; the camera (12), the palette (3) and the post (0) stay
    const uint32_t req = (1u << 0) | (1u << 3) | (1u << 5) | (1u << 6) | (1u << 12) | (1u << 14) | (1u << 15) | (1u << 16);
    const uint32_t in_scene = sl::RequiredInScene(req, true), with_stage = sl::RequiredInScene(req, false);
    Check(in_scene == ((1u << 0) | (1u << 3) | (1u << 12)) && with_stage == req, "stageless.required", "bits 5 / 6 / 14 / 15 / 16 dropped only when no stage is held");
  }
  std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "ALL PASS", g_fail);
  return g_fail ? 1 : 0;
}
