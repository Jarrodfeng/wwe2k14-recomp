// body-scale: none (counts the skel-piece holes, not the body-scale hole (a separate counter))  [S103 RGF: body_scale_feed_lint.py]
// S103 lane NR (landing 1): the not-ready rule's offline proof. No game files, no GPU work beyond Renderer::Create (as record_frame_test.cpp). 0 game runs.
//   nr_unit.exe --record <frame_000622.rsc of out\s103\run29_case.zip> [--plant old_rule]
// FIX A (rf_wrestlers.cpp SkelPieceIsHole): an invisible skinned `skel:` object is a hole only when the game drew it this frame (a light ref at the wrestler stamp) AND no ring row has its address.
//   Planted twins on the record: ring rows ignored -> the drawn skirts are holes again; a light ref planted for the undrawn 4th skirt -> one more; light refs dropped -> every piece (fail closed).
// SYMMETRY (live_scene_draw.cpp Warm == DrawLive): the arena / crowd hole rules run on the same code in both builds (tools\scene\nr_symmetry_lint.py). (S111 SHADOWKEY review: the shadow input's
//   hole, once asked here through FrameHooks::ShadowInputHole, is removed: the fitted shadow light needs no key light.)
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
#include "../../src/renderer_scene/rf_c.h"   // S110 ENTRBLACK round 6: TakeArenaMisses
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
    else if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) {
      ++i;
      plant_old = !std::strcmp(argv[i], "old_rule");
      if (!std::strcmp(argv[i], "warm_break")) rdr_scene::rf::Plants().warm_stops_at_miss = true;   // S110 ENTRBLACK round 6 twin
      if (!std::strcmp(argv[i], "undrawn_is_hole")) rdr_scene::rf::Plants().game_undrawn_is_hole = true;   // S111 PIECEHOLES twin: kObjGameUndrawn ignored (the rule before)
    }
  }
  if (rec_path.empty()) { std::printf("usage: nr_unit --record frame_000622.rsc [--plant old_rule|warm_break|undrawn_is_hole]\n"); return 2; }
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
  {   // S111 PIECEHOLES (rf_wrestlers.cpp unrouted branch -> guest_piece_fade.h DecideUnroutedPiece), through BuildFrame: kObjGameUndrawn = a slot body the contributor's quiet gate hid (no draw ask of the record's period)
    auto flag_undrawn = [](SceneTables& t) {
      bool found = false;
      for (Object& o : t.objects)
        if (std::string(t.Str(o.id)) == "skel:AAF7F4B0") o.flags |= kObjGameUndrawn, found = true;
      return found;
    };
    SceneTables t = base;   // OURS-shaped (synthetic refs only, no ring rows): 11 holes above
    for (LightState& s : t.light_states) s.flags |= kLsSynthetic;
    t.ring_rows.clear();
    check(flag_undrawn(t), "skel:AAF7F4B0 is in the record (flagged kObjGameUndrawn)");
    const int h_ours = skel_holes(t);
    check(h_ours == 10 && rf::LastWrestlerStats().game_undrawn == 1, "PIECEHOLES: no walk at the stamp -> the flagged body is counted not drawn by the game, never a hole (10 of 11; twin undrawn_is_hole: 11)");
    SceneTables u = base;   // walked, and the walk names the flagged object (a walked ref at the stamp): the game drew it although no ask was stamped
    uint32_t stamp = 0;
    WrestlerLightStamp(u, stamp);
    for (size_t i = 0; i < u.objects.size() && i < u.engine_objects.size(); ++i)
      if (std::string(u.Str(u.objects[i].id)) == "skel:AAF7F4B0") {
        LightRef r;
        r.object = u.engine_objects[i], r.ordinal = 0, r.state = 0, r.frame = stamp;
        u.light_refs.push_back(r);
      }
    flag_undrawn(u);
    check(skel_holes(u) == expect_base + 1, "PIECEHOLES review: a walked frame whose walk names the flagged body -> the walk decides, a hole (8; the flag beating the walk: 7)");
    SceneTables v = base;   // walked, no ref for it: the walk says not drawn
    flag_undrawn(v);
    check(skel_holes(v) == expect_base, "PIECEHOLES review: a walked frame with no ref for the flagged body -> not a hole (7)");
  }

  {   // S110 ENTRBLACK round 6 (record_frame.cpp BuildFrame): a WARM build asks every class's assets in the same build, past the arena miss (the classes after ring_early used to be asked only after the arena arrived)
    rf::ModelCache nl(fe, src, false);   // live-style: misses go to a loader (none here), nothing loads
    rf::c::TakeArenaMisses();
    SceneTables t = base;
    SceneFrame f;
    std::vector<rf::ClassReport> reports;
    std::string e;
    nl.NewFrame();
    rf::BuildFrame(t, nl, rf::kAllFamilies, 1280, 720, f, reports, e, rf::kBuildWarm);
    const std::vector<int> arenas = rf::c::TakeArenaMisses();
    const std::vector<rf::ModelCache::Missed> missed = nl.TakeMisses();
    check(!arenas.empty(), "warm build: the record's arena is not resident: asked (ring_early misses first)");
    check(!missed.empty(), "warm build: the classes after the arena miss ask their own assets in the same build (twin warm_break: none asked)");
  }
  std::printf("%s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
  return bad ? 1 : 0;
}
