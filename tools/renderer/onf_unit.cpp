// S103 lane ONF: the decision of a build with no command stream (src\renderer_game\scene_frame.h DecideNow, WWE2K14_KEEP_STREAM = 0), GPU-free, no game.
// Built OFF and run by tools\renderer\onf_unit.py. `onf_unit.exe PLANT` runs one planted fault; the process must exit 1 (the check CAUGHT it); no argument = the real checks, exit 0.
// Rule under test (Jarrod, DECISIONS.md "in the player build every frame is ours"): READY and not held = Ours; a missing required input (run 7309's bit 10, the crowd snapshot) is COUNTED per bit, never a Walk.
//   1 old_rule_walks        the first-stream-free-run rule: a missing input answers GameWalks / InputsMissing (the frame repeats or stays black)
//   2 missing_not_counted   an OURS frame with a missing bit is not counted per bit (the census would hide the hole)
//   3 hold_ignored          a planned hold (light hold, lane LH) is answered Ours: the walked frame would be drawn from a stale record
//   4 not_ready_ours        an unready pass answers Ours (the assets are not resident: a hole)
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../../src/renderer_game/scene_frame.h"

static int g_fail = 0, g_plant = 0;
static void Check(bool ok, const char* name, const char* what) {
  std::printf("%s  %s: %s\n", ok ? "ok  " : "FAIL", name, what);
  if (!ok) ++g_fail;
}

static void ResetAll() {
  rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
  s.ready.store(false), s.required_inputs.store(0), s.hold_until.store(0), s.miss_hold_until.store(0), s.latched.store(false);
  for (auto& f : s.input_frame) f.store(0);
  s.ours.store(0), s.walks.store(0);
  for (auto& l : scene_frame::g_input_lag) l.store(0);
  for (int b = 0; b < 32; ++b) scene_frame::g_ours_missing_bit[b].store(0), scene_frame::g_ours_stale_bit[b].store(0);
}
static void Stamp(int bit, uint64_t frame) { rdr_own::S(rdr_own::Pass::Scene3D).input_frame[bit].store(frame, std::memory_order_release); }

// The decision under test, with the planted faults applied.
static rdr_own::OwnedPassStamp Decide(uint64_t frame) {
  rdr_own::OwnedPassStamp st = scene_frame::DecideNow(frame);
  rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
  if (g_plant == 1 && st.decision == uint8_t(rdr_own::Decision::Ours) && (scene_frame::InputsMask(frame) & s.required_inputs.load()) != s.required_inputs.load()) {
    st.decision = uint8_t(rdr_own::Decision::GameWalks), st.why_not = uint8_t(rdr_own::WhyNot::InputsMissing);
  }
  if (g_plant == 2) for (int b = 0; b < 32; ++b) scene_frame::g_ours_missing_bit[b].store(0);
  if (g_plant == 3 && frame < s.hold_until.load()) st.decision = uint8_t(rdr_own::Decision::Ours), st.why_not = 0;
  if (g_plant == 4 && !s.ready.load()) st.decision = uint8_t(rdr_own::Decision::Ours), st.why_not = 0;
  return st;
}
static bool IsOurs(const rdr_own::OwnedPassStamp& st) { return st.decision == uint8_t(rdr_own::Decision::Ours); }

int main(int argc, char** argv) {
  if (argc > 1) g_plant = std::atoi(argv[1]);
#if WWE2K14_STREAM()
  std::printf("FAIL  this unit must be built with -DWWE2K14_KEEP_STREAM=0\n");
  return 2;
#else
  rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
  constexpr int kPalette = 3, kLight = 4, kCrowd = 10;
  // 1: bit 10 withheld (the crowd presence hold), READY, no hold: OURS, bit 10 counted.
  ResetAll();
  for (int b : {kPalette, kLight, kCrowd}) scene_frame::RequireInputLag(b, 2);
  s.ready.store(true);
  Stamp(kPalette, 100), Stamp(kLight, 100);
  rdr_own::OwnedPassStamp st = Decide(100);
  Check(IsOurs(st), "missing_bit10_is_ours", "READY, bit 10 not stamped: the frame is Ours (the old rule: GameWalks / InputsMissing, a black screen)");
  Check(st.inputs_mask == ((1u << kPalette) | (1u << kLight)), "stamp_mask_honest", "the stamp inputs_mask still shows what was stamped (the census reads it)");
  Check(scene_frame::g_ours_missing_bit[kCrowd].load() == 1 && scene_frame::g_ours_missing_bit[kPalette].load() == 0, "counted_per_bit", "bit 10 counted once, the stamped bits not");
  const std::string line = scene_frame::OursMissingLine();
  Check(line.find("bit 10:1") != std::string::npos, "census_line", line.c_str());
  const std::string mh = scene_frame::missing_hist::MissingLine();
  Check(mh.find("ours 1") != std::string::npos && mh.find("{10}:1") != std::string::npos && mh.find("sum ok") != std::string::npos, "missing_hist_set", mh.c_str());
  // 2: every input stamped: Ours, nothing new counted.
  Stamp(kCrowd, 101), Stamp(kPalette, 101), Stamp(kLight, 101);
  st = Decide(101);
  Check(IsOurs(st) && scene_frame::g_ours_missing_bit[kCrowd].load() == 1, "complete_not_counted", "all inputs stamped: Ours, no new count");
  // 3: planned hold (the light hold path, lane LH) stays a Walk.
  s.hold_until.store(105);
  st = Decide(103);
  Check(!IsOurs(st) && st.why_not == uint8_t(rdr_own::WhyNot::Hold), "hold_still_walks", "inside hold_until the frame is a Walk (kept as planned)");
  st = Decide(105);
  Check(IsOurs(st), "hold_ends", "at hold_until the frame is Ours again");
  // 4: not READY: Walk (the assets are not resident).
  s.ready.store(false);
  st = Decide(106);
  Check(!IsOurs(st) && st.why_not == uint8_t(rdr_own::WhyNot::NotReady), "not_ready_walks", "an unready pass is a Walk, never Ours");
  // 5: a frame with several missing bits is Ours, every bit counted.
  ResetAll();
  s.ready.store(true);
  for (int b : {kPalette, kLight, kCrowd}) scene_frame::RequireInputLag(b, 2);
  st = Decide(200);
  Check(IsOurs(st) && scene_frame::g_ours_missing_bit[kPalette].load() == 1 && scene_frame::g_ours_missing_bit[kLight].load() == 1 && scene_frame::g_ours_missing_bit[kCrowd].load() == 1, "all_missing_counted", "nothing stamped: Ours, all three bits counted");
  // 6: the render side's stale note.
  scene_frame::NoteOursStale((1u << kCrowd) | (1u << 0));
  Check(scene_frame::OursMissingLine().find("record: bit 0:1 bit 10:1") != std::string::npos, "stale_counted", scene_frame::OursMissingLine().c_str());
  std::printf(g_fail ? "FAILED %d\n" : "all ok\n", g_fail);
  return g_fail ? 1 : 0;
#endif
}
