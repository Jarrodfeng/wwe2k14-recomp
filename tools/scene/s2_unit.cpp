// s2_unit: the offline proof of S103 lane S2 (docs/renderer/S103_FABLE_REVIEW_2.md Q1). 0 game runs, no SDK, no game memory.
//   s2_unit [--plant NAME]
// It runs the SHIPPED headers: presence_rules.h (bit 11's per-frame verdict), scene_frame.h (the sentinel walked frames on the real decision, rdr_own's hold), input_changes.h (the changes census
// on made-up records). A plant is a fault in that code (RDR_S2_PLANTS) that must make a NAMED check fail (s2_check.py runs them):
//   part_blend_ignored    an unproven blend code passes            -> "part.unproven_blend_no_stamp"
//   part_texture_ignored  an unnamed texture passes                -> "part.unnamed_texture_no_stamp"
//   sentinel_never        the sentinel hold is never raised        -> "sentinel.walks"
//   changes_frame_counted the frame fields are hashed              -> "changes.frame_field_ignored"
#ifndef RDR_S2_PLANTS
#error "build with -DRDR_S2_PLANTS (s2_check.py does)"
#endif
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/scene_live/presence_rules.h"
#include "../../src/scene_live/input_changes.h"

using namespace rdr_scene;
namespace pr = rdr_scene::live::presence;
namespace ic = rdr_scene::live::inputchanges;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& detail = "") {
  if (!ok) {
    ++g_fail;
    std::printf("FAIL %s %s\n", name, detail.c_str());
  } else {
    std::printf("ok   %s\n", name);
  }
}

// ---- bit 11: the per-frame particle verdict and the stamp rule (particle_source.h: StampParticles(kStampParticles, verdict) && WholePass)
static void ParticleTests() {
  ParticleStatus clean;
  ParticleStatus blend;
  blend.reasons = kPiBlend;
  ParticleStatus tex;
  tex.reasons = kPiTexture | kPiBlend;
  ParticleStatus other;
  other.other_class = 2;   // counted, the reason bit not set: still other-class
  ParticleStatus pivot;
  pivot.reasons = kPiPivot, pivot.pivot_not_centred = 3;   // info only since F3
  pr::g_particles.store(false);
  Check(pr::ParticleVerdict(clean, 0) == pr::PartWhy::WholeEmpty && !pr::StampParticles(false, pr::ParticleVerdict(clean, 0)), "part.switch_off_no_stamp");
  pr::g_particles.store(true);
  Check(pr::StampParticles(false, pr::ParticleVerdict(clean, 0)), "part.empty_frame_stamps");
  Check(pr::ParticleVerdict(clean, 40) == pr::PartWhy::WholeItems && pr::StampParticles(false, pr::ParticleVerdict(clean, 40)), "part.proven_items_stamp");
  Check(!pr::StampParticles(false, pr::ParticleVerdict(blend, 40)), "part.unproven_blend_no_stamp", pr::PartWhyName(pr::ParticleVerdict(blend, 40)));
  Check(!pr::StampParticles(false, pr::ParticleVerdict(tex, 40)) && pr::ParticleVerdict(tex, 40) == pr::PartWhy::Texture, "part.unnamed_texture_no_stamp", pr::PartWhyName(pr::ParticleVerdict(tex, 40)));
  Check(pr::ParticleVerdict(other, 0) == pr::PartWhy::OtherClass && !pr::StampParticles(false, pr::ParticleVerdict(other, 0)), "part.other_class_no_stamp");
  Check(pr::ParticleVerdict(pivot, 5) == pr::PartWhy::WholeItems, "part.pivot_is_info");
  ParticleStatus cam;
  cam.reasons = kPiNoCamera;
  Check(pr::ParticleVerdict(cam, 0) == pr::PartWhy::NoCamera, "part.no_camera_first");
  pr::g_particles.store(false);
  pr::Particles().Note(pr::PartWhy::Blend, false);
  pr::Particles().Note(pr::PartWhy::WholeEmpty, true);
  const std::string line = pr::Particles().Str();
  Check(line.find("stamped 1") != std::string::npos && line.find("blend 1") != std::string::npos && line.find("whole_empty 1") != std::string::npos, "part.census_line", line);
  // S103 PF: a planted "empty but drawn" frame: the reader's latest verdict whole_empty, the stream drew 120 particle-class draws -> the column counts it; a stream frame with no particle draw and a
  // frame where the verdict was not whole_empty do not add to the empty column. (plant 3 = the column ignores the stream draws: fails "part.empty_but_drawn_counted".)
  {
    pr::PartCensus c;
    c.NoteStreamFrame(50);   // no verdict yet: never counted as empty
    c.Note(pr::PartWhy::WholeItems, true);
    c.NoteStreamFrame(30);   // items read, drawn: not an empty-but-drawn frame
    c.Note(pr::PartWhy::WholeEmpty, true);
    c.NoteStreamFrame(0);    // empty and nothing drawn: honest
    c.NoteStreamFrame(120);  // empty but drawn: the lie
    const std::string s = c.Str();
    Check(c.empty_drawn_frames.load() == 1 && c.empty_drawn_draws.load() == 120 && c.stream_frames.load() == (pr::g_part_plant == 3 ? 0u : 3u) && s.find("whole_empty frames: frames 1, draws 120 (of 3 stream frames") != std::string::npos,
          "part.empty_but_drawn_counted", s);
  }
}

// ---- the sentinel walked frames on the real decision (scene_frame.h DecideNow / Decide, rdr_own's hold)
static int g_mode = 2;
static void SentinelTests() {
  scene_frame::g_mode_cvar.store(&g_mode);
  scene_frame::g_force_inputs.store(true);   // the forced mode-2 run: inputs not required
  scene_frame::SetReady(true);
  rdr_own::PassState& s = rdr_own::S(rdr_own::Pass::Scene3D);
  const uint64_t base = 1000;
  scene_frame::g_sentinel_every.store(0);
  bool all_ours = true;
  for (uint64_t f = base; f < base + 20; ++f) all_ours &= scene_frame::Decide(f).decision == uint8_t(rdr_own::Decision::Ours);
  Check(all_ours, "sentinel.off_never_walks");
  scene_frame::g_sentinel_every.store(5);
  const uint32_t strikes = s.strike_next.load();
  bool ok = true;
  std::string bad;
  for (uint64_t f = base + 20; f < base + 60; ++f) {
    const rdr_own::OwnedPassStamp st = scene_frame::Decide(f);
    const bool walks = st.decision == uint8_t(rdr_own::Decision::GameWalks);
    const bool want = f % 5 == 0;
    if (walks != want || (walks && st.why_not != uint8_t(rdr_own::WhyNot::Hold))) ok = false, bad += " f" + std::to_string((unsigned long long)f);
  }
  Check(ok, "sentinel.walks", bad);
  Check(s.strike_next.load() == strikes && !s.latched.load(), "sentinel.no_strike");
  Check(scene_frame::g_sentinel_planned.load() >= 8, "sentinel.planned_counted", std::to_string((unsigned long long)scene_frame::g_sentinel_planned.load()));
  Check(scene_frame::CensusLine().find("sentinel every 5 planned") != std::string::npos, "sentinel.census_line", scene_frame::CensusLine());
  // mirror mode: never planned, never held
  g_mode = 1;
  const uint64_t hold = s.hold_until.load();
  const rdr_own::OwnedPassStamp m1 = scene_frame::DecideNow(base + 100);
  Check(m1.why_not == uint8_t(rdr_own::WhyNot::SwitchOff) && s.hold_until.load() == hold, "sentinel.mode1_no_hold");
  g_mode = 2;
  // the sentinel sheets: N = 30, sheet_every 300 -> one pair per 10 sentinels; 0 = never
  scene_frame::g_sentinel_every.store(30);
  Check(scene_frame::SentinelSheet(0, 300) && scene_frame::SentinelSheet(300, 300) && !scene_frame::SentinelSheet(30, 300) && !scene_frame::SentinelSheet(301, 300) && !scene_frame::SentinelSheet(300, 0),
        "sentinel.sheets");
  Check(scene_frame::SentinelSheet(30, 20) && scene_frame::SentinelSheet(60, 20), "sentinel.sheets_every_sentinel_when_every_lt_n");
  scene_frame::g_sentinel_every.store(0);
  Check(!scene_frame::SentinelSheet(0, 300), "sentinel.sheets_off");
}

// ---- the changes census on made-up records: a record per frame, sentinel every 5. post_settings (bit 0) changes on every record (healthy); model_runtime (bit 1) carries only a new frame
// number (no change); the wrestler palette (bit 3, kScBones) changes ONLY on walked records (fed by the walk) and so jumps on each sentinel.
static SceneTables MakeRecord(uint64_t j, bool walked_value_moves, uint32_t& palette_value) {
  SceneTables t;
  const uint64_t hook = j - 1;
  OwnedStamp st;
  st.pass = uint8_t(rdr_own::Pass::Scene3D), st.guest_frame = hook;
  const bool sentinel = scene_frame::IsSentinel(hook);
  st.decision = uint8_t(sentinel ? rdr_own::Decision::GameWalks : rdr_own::Decision::Ours);
  st.why_not = uint8_t(sentinel ? rdr_own::WhyNot::Hold : rdr_own::WhyNot::None);
  t.owned_passes.push_back(st);
  PostParamsRow pp;
  pp.frame = j, pp.flags = kPpRead, pp.s[0] = uint32_t(j);
  t.post_params.push_back(pp);
  ModelRuntimeRow mr;
  mr.frame = j, mr.object = 0x80001000u, mr.alpha = 1.f;
  t.model_runtime.push_back(mr);
  if (sentinel && walked_value_moves) ++palette_value;
  Mat4 m{};
  m.m[0] = float(palette_value);
  t.bones.push_back(m);
  return t;
}
static void ChangesTests() {
  ic::The().Reset();
  ic::g_on.store(false);
  scene_frame::g_sentinel_every.store(5);
  uint32_t pal = 0;
  ic::The().Note(MakeRecord(1, true, pal));
  Check(ic::The().Frames(ic::Kind::Ours) + ic::The().Frames(ic::Kind::Walked) + ic::The().Frames(ic::Kind::Sentinel) == 0, "changes.off_costs_nothing");
  ic::g_on.store(true);
  for (uint64_t j = 2; j <= 41; ++j) ic::The().Note(MakeRecord(j, true, pal));
  Check(ic::The().Frames(ic::Kind::Sentinel) == 8 && ic::The().Frames(ic::Kind::Ours) == 32, "changes.classified",
        std::to_string((unsigned long long)ic::The().Frames(ic::Kind::Sentinel)) + " / " + std::to_string((unsigned long long)ic::The().Frames(ic::Kind::Ours)));
  Check(ic::The().Changes(scene_inputs::kInModelRuntime, ic::Kind::Ours) == 0 && ic::The().Changes(scene_inputs::kInModelRuntime, ic::Kind::Sentinel) == 0, "changes.frame_field_ignored",
        std::to_string((unsigned long long)ic::The().Changes(scene_inputs::kInModelRuntime, ic::Kind::Ours)));
  Check(ic::The().Changes(scene_inputs::kInPostSettings, ic::Kind::Ours) > 0 && !ic::The().FedByWalk(scene_inputs::kInPostSettings), "changes.healthy_not_named");
  Check(ic::The().FedByWalk(scene_inputs::kInWrestlerPalette) && ic::The().Changes(scene_inputs::kInWrestlerPalette, ic::Kind::Ours) == 0, "changes.fed_by_walk_named");
  Check(ic::The().Jumps(scene_inputs::kInWrestlerPalette) >= 7 && ic::The().Jumps(scene_inputs::kInPostSettings) == 0, "changes.sentinel_jump",
        std::to_string((unsigned long long)ic::The().Jumps(scene_inputs::kInWrestlerPalette)));
  const std::string line = ic::The().Line();
  Check(line.find("fed by the walk: wrestler_palette") != std::string::npos && line.find("sentinel jumps: wrestler_palette") != std::string::npos && line.find("(sentinel 8)") != std::string::npos,
        "changes.census_line", line);
  // a record without a Scene3D stamp (mode 0 / 1) is walked, never OURS
  SceneTables plain;
  ic::The().Reset();
  ic::The().Note(plain);
  Check(ic::The().Frames(ic::Kind::Walked) == 1, "changes.no_stamp_is_walked");
  ic::The().Reset();
  ic::g_on.store(false);
  scene_frame::g_sentinel_every.store(0);
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i + 1 < argc; ++i)
    if (!std::strcmp(argv[i], "--plant")) plant = argv[i + 1];
  if (plant == "part_blend_ignored") pr::g_part_plant = 1;
  else if (plant == "part_texture_ignored") pr::g_part_plant = 2;
  else if (plant == "part_empty_drawn_ignored") pr::g_part_plant = 3;
  else if (plant == "sentinel_never") scene_frame::g_sentinel_plant = 1;
  else if (plant == "changes_frame_counted") ic::g_changes_plant = 1;
  else if (!plant.empty()) { std::printf("unknown plant %s\n", plant.c_str()); return 2; }
  ParticleTests();
  SentinelTests();
  ChangesTests();
  std::printf(g_fail ? "FAILED %d check(s)\n" : "PASSED\n", g_fail);
  return g_fail ? 1 : 0;
}
