// S102 lane P4P5: the node switch's decision layer for the shadow module and the rim map (src\renderer_game\own_shadow_rim.h on node_switch.h), GPU-free, no game.
// Built and run by tools\renderer\own_nodes_unit.py. `own_nodes_unit.exe PLANT` runs one planted fault; the process must exit 1 (the check CAUGHT it); no argument = the real checks, exit 0.
//   1 stamp_wrong_frame   a stamp for frame j-1 is read as Ours for frame j: DrawOurs must say no
//   2 inputs_ignored      the decision ignores the inputs (a missing input answers Ours): the game would not walk and nothing would draw the pass
//   3 miss_no_hold        a render-side miss does not put the pass on hold: the next frame decides Ours again
//   4 double_uncounted    a stamp Ours with the stream also holding the pass is not counted
//   5 ready_ignored       the decision ignores readiness
//   6 memo_off            the decision is made again on a second walk of the node (two stamps for one frame)
//   7 hole_ignores_frame  the hole rule accepts a stamp of another frame
//   8 present_swallowed   a pass runs on past the Present op
//   9 id_29               the contract's id is read as 29 (the scene-record enum says 28)
//  11 zero_mask_ours      a pass whose required-input mask is 0 is decided Ours
//  10 refusal_ignored     the D3 caster skip and the node switch both on are not refused
//   6 memo_off            the decision is made again on a second walk of the node (two stamps for one frame)
//   7 hole_ignores_frame  the hole rule accepts a stamp of another frame
//   8 present_swallowed   a pass runs on past the Present op
//   9 id_29               the contract's id is read as 29 (the scene-record enum says 28)
//  11 zero_mask_ours      a pass whose required-input mask is 0 is decided Ours
//  10 refusal_ignored     the D3 caster skip and the node switch both on are not refused
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../src/renderer_adapter/own_group.h"
#include "../../src/renderer_game/own_shadow_rim.h"
#include "../../src/renderer_scene/scene_record.h"   // kScOwnedPasses must be the id the enum gives it

using namespace rdr_own;
using namespace rdr_own::shadow_rim;

static int g_fail = 0;
static int g_plant = 0;
static void Check(bool ok, const char* name, const char* what) {
  std::printf("%s  %s: %s\n", ok ? "ok  " : "FAIL", name, what);
  if (!ok) ++g_fail;
}

// What the hook does for one frame with the planted faults applied.
static OwnedPassStamp Node(Pass p, uint64_t frame, bool on) {
  if (g_plant == 2 || g_plant == 5) {   // the faults live in a copy of the decision
    OwnedPassStamp st;
    st.pass = uint8_t(p);
    st.guest_frame = frame;
    const PassState& s = S(p);
    const bool ready_ok = g_plant == 5 || s.ready.load();
    const bool inputs_ok = g_plant == 2 || InputsMask(p, frame) == s.required_inputs.load();
    st.decision = uint8_t(on && ready_ok && inputs_ok && frame >= s.hold_until.load() ? Decision::Ours : Decision::GameWalks);
    g_board.Put(st);
    return st;
  }
  if (g_plant == 6) {   // no memo: decide again and write a second stamp
    const OwnedPassStamp st = Decide(p, frame, on);
    g_board.Put(st);
    return st;
  }
  return DecideNode(p, frame, on);
}

static void MarkAll(Pass p, int count, uint64_t frame) {
  for (int b = 0; b < count; ++b) InputCaptured(p, b, frame);
}

int main(int argc, char** argv) {
  g_plant = argc > 1 ? std::atoi(argv[1]) : 0;
  RegisterInputs();
  Check(S(Pass::ShadowModule).required_inputs.load() == 0xF && S(Pass::Rim).required_inputs.load() == 0x3, "inputs", "shadow module requires 4 inputs, rim map 2");

  // 1. nothing marked captured (this lane's state): the game walks, whatever the switch and readiness say
  SetReady(Pass::ShadowModule, true);
  SetReady(Pass::Rim, true);
  OwnedPassStamp a = Node(Pass::ShadowModule, 100, true);
  Check(a.decision == uint8_t(Decision::GameWalks) && a.why_not == uint8_t(WhyNot::InputsMissing), "no_inputs", "switch on, ready, no input captured: the game walks (InputsMissing)");

  // 2. the switch off, or not ready
  MarkAll(Pass::ShadowModule, 4, 98);
  MarkAll(Pass::ShadowModule, 4, 99);
  MarkAll(Pass::ShadowModule, 4, 101);
  Check(Node(Pass::ShadowModule, 98, false).why_not == uint8_t(WhyNot::SwitchOff), "switch_off", "all inputs captured, switch off: SwitchOff");
  SetReady(Pass::ShadowModule, false);
  Check(Node(Pass::ShadowModule, 99, true).decision == uint8_t(Decision::GameWalks), "not_ready", "all inputs captured, render side not ready: the game walks");
  SetReady(Pass::ShadowModule, true);

  // 3. Ours: all inputs of THIS frame, ready, switch on
  OwnedPassStamp o = Node(Pass::ShadowModule, 101, true);
  Check(o.decision == uint8_t(Decision::Ours) && o.inputs_mask == 0xF, "ours", "all inputs stamped with this frame, ready, switch on: Ours");
  // an input stamped for another frame
  OwnedPassStamp stale = Node(Pass::ShadowModule, 102, true);
  Check(stale.decision == uint8_t(Decision::GameWalks), "stale_inputs", "the inputs are one frame old: the game walks");

  // 4. the stamp is read for the frame it names only
  const std::vector<OwnedPassStamp> stamps = g_board.Take(101);
  bool has_ours = false;
  for (const auto& s : stamps) has_ours |= s.pass == uint8_t(Pass::ShadowModule) && s.decision == uint8_t(Decision::Ours);
  Check(has_ours, "board", "the board holds the frame's Ours stamp");
  const auto draw_ours = [&](uint64_t frame) {   // plant 1: the reader does not compare the stamp's frame
    if (g_plant != 1) return DrawOurs(stamps.data(), stamps.size(), Pass::ShadowModule, frame);
    for (const auto& s : stamps) if (s.pass == uint8_t(Pass::ShadowModule) && s.decision == uint8_t(Decision::Ours)) return true;
    return false;
  };
  Check(!draw_ours(102), "stamp_frame", "a stamp for frame 101 is not read as Ours for another frame");
  Check(DrawOurs(stamps.data(), stamps.size(), Pass::ShadowModule, 101), "stamp_own_frame", "the stamp is Ours for its own frame");

  // 5. a render-side miss: a hold, counted as a hole; three holds in the window latch the pass
  const uint64_t holes0 = S(Pass::ShadowModule).holes.load();
  if (g_plant != 3) ReportMiss(Pass::ShadowModule, 101);
  MarkAll(Pass::ShadowModule, 4, 103);
  Check(Node(Pass::ShadowModule, 103, true).decision == uint8_t(Decision::GameWalks), "hold", "after a miss the pass is on hold: the game walks the next frames");
  Check(S(Pass::ShadowModule).holes.load() == holes0 + 1, "hole", "the miss is counted as a hole");
  MarkAll(Pass::ShadowModule, 4, 101 + 61);
  Check(Node(Pass::ShadowModule, 101 + 61, true).decision == uint8_t(Decision::Ours), "hold_over", "after hold_frames the pass tries again");
  bool latched = false;
  latched |= ReportMiss(Pass::ShadowModule, 200);
  latched |= ReportMiss(Pass::ShadowModule, 300);
  Check(latched && S(Pass::ShadowModule).latched.load(), "latch", "three holds inside the window latch the pass off (one ERROR line in the live hook's log)");
  MarkAll(Pass::ShadowModule, 4, 5000);
  Check(Node(Pass::ShadowModule, 5000, true).decision == uint8_t(Decision::GameWalks), "latched", "a latched pass stays with the game for the session");

  // 6. a double: a stamp Ours with the stream also holding the pass
  const uint64_t d0 = S(Pass::Rim).doubles.load();
  if (g_plant != 4) ReportDouble(Pass::Rim);
  Check(S(Pass::Rim).doubles.load() == d0 + 1, "double", "a stamp Ours with the pass also in the stream is counted a contract fault");

  // 7. the rim map is its own pass: marking the shadow module's inputs does not make the rim Ours
  Check(Node(Pass::Rim, 5000, true).decision == uint8_t(Decision::GameWalks), "pass_apart", "the rim map's decision does not follow the shadow module's inputs");
  MarkAll(Pass::Rim, 2, 5001);
  Check(Node(Pass::Rim, 5001, true).decision == uint8_t(Decision::Ours), "rim_ours", "the rim map is Ours when its own inputs are captured and it is ready");

  // 8. one decision per (pass, frame): a miss reported between two walks does not change the stamp, and the board holds one stamp for the frame (review D)
  MarkAll(Pass::Rim, 2, 6000);
  const OwnedPassStamp first = Node(Pass::Rim, 6000, true);
  ReportMiss(Pass::Rim, 6000);
  const OwnedPassStamp second = Node(Pass::Rim, 6000, true);
  int rim_stamps = 0;
  for (const auto& st : g_board.Take(6000)) rim_stamps += st.pass == uint8_t(Pass::Rim);
  Check(first.decision == second.decision && rim_stamps == 1, "once", "two walks of the node in one frame with a miss between them give ONE stamp, and the same decision");

  // 9. a hole: the node is enabled, the stamp is absent or GameWalks (or written for frame + 1), the stream has none of the pass's passes
  OwnedPassStamp off_by_one;
  off_by_one.pass = uint8_t(Pass::ShadowModule), off_by_one.decision = uint8_t(Decision::Ours), off_by_one.guest_frame = 701;   // written at frame + 1
  const auto hole = [&](bool enabled, const OwnedPassStamp* st, uint64_t n, uint64_t frame, uint32_t stream_passes) {
    if (g_plant == 7) return enabled && stream_passes == 0 && n == 0;   // plant: any stamp counts, whatever its frame
    return IsHole(enabled, st, n, Pass::ShadowModule, frame, stream_passes);
  };
  Check(hole(true, &off_by_one, 1, 700, 0), "hole_off_by_one", "stamps written at frame + 1 give a hole: the game did not walk and ours did not draw");
  Check(!hole(true, &off_by_one, 1, 701, 0), "no_hole_own_frame", "an Ours stamp for its own frame is not a hole");
  Check(!hole(true, &off_by_one, 1, 700, 7), "no_hole_stream", "the stream holding the pass's passes is not a hole");
  Check(!hole(false, &off_by_one, 1, 700, 0), "no_hole_disabled", "a node that was not enabled is not a hole");
  OwnedPassStamp walks = off_by_one;
  walks.guest_frame = 700, walks.decision = uint8_t(Decision::GameWalks);
  Check(hole(true, &walks, 1, 700, 0), "hole_walks_but_empty", "a GameWalks stamp with an empty stream is a hole");

  // 10. the group never swallows the present (review 6)
  {
    using namespace rdr_adapter::own_group;
    FactsBuilder fb;
    DrawFacts rim;
    rim.has_origin = true, rim.pass_site = kRimPassSite;
    fb.SetTargets(0), fb.Draw(rim), fb.Copy();
    if (g_plant != 8) fb.Present(3);
    fb.Clear();   // an op after the present (a clear): no pass's
    const std::vector<PassFacts>& passes = fb.Finish(5);
    Plan plan;
    Layout(passes, 5, plan);
    Check(plan.g[kRim].passes == 1 && plan.g[kRim].last_op == 3 && plan.g[kRim].ops == 3, "present", "a synthetic frame that ends after the rim pass: the group stops at the Present (ops 0..3), the Present and what follows are not in it");
    // 12. the refusal
    const char* refusal = g_plant == 10 ? nullptr : Refusal(true, true, false);
    Check(refusal != nullptr && Refusal(false, true, true) == nullptr && Refusal(true, false, false) == nullptr, "refusal", "the D3 caster skip with an owned node on is refused; either alone is not");
  }
  // 11. the two ids agree: the contract's constant and the scene-record enum (a planted 29 must fail)
  const uint32_t contract_id = g_plant == 9 ? 29u : kScOwnedPasses;
  Check(contract_id == uint32_t(rdr_scene::kScOwnedPasses) && uint32_t(rdr_scene::kScOwnedPasses) == 28u && uint32_t(rdr_scene::kScCount) >= 29u, "id", "kScOwnedPasses is 28 in node_switch.h and in SceneSectionId, and kScCount follows it");

  // 13. a pass with required_inputs == 0 (registration skipped or reset) is never Ours (P8 review)
  {
    const uint32_t saved = S(Pass::Particles).required_inputs.load();
    S(Pass::Particles).required_inputs.store(0);
    SetReady(Pass::Particles, true);
    OwnedPassStamp z = Decide(Pass::Particles, 7000, true);
    if (g_plant == 11) z.decision = uint8_t(Decision::Ours);   // plant: the zero mask matches "all inputs ready"
    Check(z.decision == uint8_t(Decision::GameWalks) && (g_plant == 11 || z.why_not == uint8_t(WhyNot::NoInputsRegistered)), "zero_mask", "cleared / unregistered inputs (mask 0): the game walks, why_not NoInputsRegistered");
    S(Pass::Particles).required_inputs.store(saved);
  }

  std::printf("%s\n", g_fail ? "FAILED" : "all ok");
  return g_fail ? 1 : 0;
}
