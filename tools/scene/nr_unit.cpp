// body-scale: none (counts the skel-piece holes, not the body-scale hole (a separate counter))  [S103 RGF: body_scale_feed_lint.py]
// S103 lane NR (landing 1): the not-ready rule's offline proof. No game files, no GPU work beyond Renderer::Create (as record_frame_test.cpp). 0 game runs.
//   nr_unit.exe --record <frame_000622.rsc of out\s103\run29_case.zip> [--plant old_rule]
// FIX A (rf_wrestlers.cpp SkelPieceIsHole): an invisible skinned `skel:` object is a hole only when the game drew it this frame (a light ref at the wrestler stamp) AND no ring row has its address.
//   Planted twins on the record: ring rows ignored -> the drawn skirts are holes again; a light ref planted for the undrawn 4th skirt -> one more; light refs dropped -> every piece (fail closed).
// SYMMETRY (live_scene_draw.cpp Warm == DrawLive): the shadow input's hole is asked by Warm through FrameHooks::ShadowInputHole (the rule BuildMapsInput owns): a record with wrestlers whose key light is
//   not in it is a hole there. (The arena / crowd hole rules run on the same code in both builds: tools\scene\nr_symmetry_lint.py.)
// Exit 0 = all checks passed. With --plant old_rule (the rule before NR) the run must FAIL (exit 1: the check expects that).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/frame_hooks.h"
#include "../../src/renderer_scene/record_frame.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_caster.h"
#include "../../src/renderer_scene/rows_inputs.h"
#include "../../src/renderer_scene/rows_lights.h"
#include "../../src/renderer_scene/rows_ring.h"
#include "../../src/renderer_scene/rf_wrestlers.h"
#include "../../src/renderer_scene/shadow_fit_record.h"

using namespace rdr_scene;

struct FailAssets : rf::AssetSource {
  bool Load(const std::string&, const std::string&, uint32_t, assets::Model&, std::string& e) override { e = "no game files in this unit"; return false; }
};

int main(int argc, char** argv) {
  std::string rec_path;
  bool plant_old = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--record") && i + 1 < argc) rec_path = argv[++i];
    else if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) plant_old = !std::strcmp(argv[++i], "old_rule");
  }
  if (rec_path.empty()) { std::printf("usage: nr_unit --record frame_000622.rsc [--plant old_rule]\n"); return 2; }
  if (plant_old) rdr_scene::rf::Plants().old_hole_rule = true;
  SceneTables base;
  std::string err;
  if (!ReadRecord(rec_path, base, err)) { std::printf("FAIL bad record: %s\n", err.c_str()); return 2; }
  rdr::RendererOptions ro;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::printf("FAIL renderer: %s\n", err.c_str()); return 2; }
  int bad = 0;
  auto check = [&](bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++bad; };
  FrontEnd fe(*renderer);
  FailAssets src;
  rf::ModelCache cache(fe, src, true);

  // skel holes of one record: the own-skeleton pieces counted minus the ones that are not holes (rf_wrestlers.h WrestlerStats), from a WARM build (ApplyWrestlers has no warm test: it runs there too)
  auto skel_holes = [&](SceneTables& t, uint32_t* pieces = nullptr) {
    SceneFrame f;
    std::vector<rf::ClassReport> reports;
    std::string e;
    cache.NewFrame();
    rf::BuildFrame(t, cache, 1u << uint32_t(rf::Family::Wrestlers), 1280, 720, f, reports, e, rf::kBuildWarm);
    const rf::WrestlerStats& s = rf::LastWrestlerStats();
    if (pieces) *pieces = s.own_skeleton_pieces;
    return int(s.own_skeleton_pieces) - int(s.skel_not_hole);
  };

  uint32_t pieces = 0;
  const int expect_base = 7;
  {
    SceneTables t = base;
    const int h = skel_holes(t, &pieces);
    std::printf("record: %u invisible skinned skel pieces, holes %d (light refs %zu, ring rows %zu)\n", pieces, h, t.light_refs.size(), t.ring_rows.size());
    check(pieces == 11, "the record holds the 11 own-skeleton objects (run 29 f622)");
    check(h == expect_base, "fixed rule: 7 holes (the 4 skirts are not: three drawn by the ring family, the 4th not drawn by the game)");
  }
  {
    SceneTables t = base;
    t.ring_rows.clear();
    check(skel_holes(t) == expect_base + 3, "twin: ring rows ignored -> the three drawn skirts are holes again (10)");
  }
  {
    SceneTables t = base;
    uint32_t stamp = 0;
    const bool have = WrestlerLightStamp(t, stamp);
    check(have, "the record has a wrestler light stamp");
    bool planted = false;
    for (size_t i = 0; i < t.objects.size() && i < t.engine_objects.size(); ++i)
      if (std::string(t.Str(t.objects[i].id)) == "skel:AAF7F4B0") {
        LightRef r;
        r.object = t.engine_objects[i], r.ordinal = 0, r.state = 0, r.frame = stamp;
        t.light_refs.push_back(r);
        planted = true;
      }
    check(planted, "skel:AAF7F4B0 (the undrawn 4th skirt) is in the record");
    check(skel_holes(t) == expect_base + 1, "twin: a light ref planted for the 4th skirt -> it is a hole (8)");
  }
  {
    SceneTables t = base;
    t.light_refs.clear();
    check(skel_holes(t) == 8, "twin: light refs dropped -> every piece the ring family does not draw is a hole (8: the three ring-drawn skirts have ring rows; the 4th skirt has none and no evidence, fail closed)");
  }

  {   // S103 NRBF (review F1): an OURS-shaped record carries only the latch's SYNTHETIC refs: "no ref for the piece" proves nothing, so every unlinked piece is a hole
    SceneTables t = base;
    for (LightState& s : t.light_states) s.flags |= kLsSynthetic;
    t.ring_rows.clear();
    check(skel_holes(t) == 11, "twin: OURS-shaped (synthetic refs only, no ring rows) -> every unlinked own-skeleton piece is a hole (11)");
    SceneTables u = base;
    for (LightState& s : u.light_states) s.flags |= kLsSynthetic;
    check(skel_holes(u) == 8, "twin: synthetic refs only, ring rows kept -> the three ring-drawn skirts are the only exemption (8)");
  }

  // ---- symmetry: the shadow input hole, asked the way Warm asks it ----
  {
    cache.KeepWrestlerCpu(true);
    FrameHooks hooks(*renderer, cache);
    std::string why;
    SceneTables t = base;
    const bool base_hole = hooks.ShadowInputHole(t, why);
    check(!base_hole, "the record's shadow input is whole: not a hole");
    SceneTables u = base;   // a wrestler record whose key light is gone: no light states / refs / objects / caster rows
    u.light_states.clear(), u.light_refs.clear(), u.light_objects.clear(), u.caster_calls.clear(), u.caster_states.clear();
    why.clear();
    check(hooks.ShadowInputHole(u, why) && !why.empty(), "twin: no key light in the record -> the shadow input is a hole in Warm too (why named)");
  }
  std::printf("%s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
  return bad ? 1 : 0;
}
