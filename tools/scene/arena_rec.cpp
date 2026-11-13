// arena_rec: builds the arena contributor's scene record from the player's own files, with no game and no GPU (Session 101, lane 28).
//   arena_rec --game DIR --arena N [--time T] --camera-from REF.rsc --out OUT.rsc [--plant KIND]
// The snapshot is what a live game would publish, made from the files: the viewer's evaluated light sets at stage-key time T (default 132, the
// viewer's match default), the slot text of the arena pac, the screens. It goes through the REAL contributor (arena_contrib.cpp Emit), then the
// camera of REF.rsc (a viewer record of the same arena) is copied in (the live writer adds the camera in the game).
// --plant KIND (the selftest's planted faults, each a class of past bug):
//   swap_sets : the two most used light sets exchanged between the slots (every slot lit by the wrong set)
//   dirs      : every stored light direction negated (the S98 entrance light-direction bug class)
//   no_twins  : the 50001 r / y mirrored twins dropped (S97's half arena)
//   zero_fall : the cube falloff written as 0 instead of unknown (review F1's bug: the model's own falloff overwritten)
// --live-shaped : the snapshot shaped like what the game hook publishes: no falloff (unknown), five lights per set (review F1)
// --look match|unknown|entrance : the look the snapshot declares (default match; the game hook publishes unknown; review F2)
// --scene-word N : S101 lane 264: the look is decided the way the game hook does it: PublishAll with the look Unknown, then PublishLook(LookFromScenePhase(true, N))
//   (N = the scene phase word 0x836F3940: 1 = the match look, anything else the entrance look; --scene-word -1 = the word was unreadable -> Unknown)
// --from-provider : the slots / screens are the game-side provider's (the pac directory + text, what the live game publishes) instead of the viewer model's:
//   the record then has the same 14 when-flagged slots (+ twins) and 8 screens a real capture has (arena 4)
// --provider-check : also compares the game-side slot provider (arena_provider.h, from the pac directory) with the viewer model's slot list (review F5)
// Exit 0 written; 2 bad input; 3 any other failure. Prints one line "[arena_rec] key=value ..." with the counts the selftest reads.
#include "stb_image.h"  // declarations only: the loader seam defines the implementation

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/pac.h"
#include "../../src/assets/view/loader_seam.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/arena_apply.h"
#include "../../src/scene_live/arena_contrib.h"
#include "../../src/scene_live/arena_provider.h"

using namespace rdr_scene;
namespace ar = rdr_scene::live::arena;

int main(int argc, char** argv) {
  std::string game, out, cam, plant, look = "match";
  bool live_shaped = false, provider_check = false, have_scene_word = false, from_provider = false;
  long long scene_word = 0;
  int arena = 5;
  double time = 132;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--camera-from" && i + 1 < argc) cam = argv[++i];
    else if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "--live-shaped") live_shaped = true;
    else if (a == "--provider-check") provider_check = true;
    else if (a == "--from-provider") from_provider = true;
    else if (a == "--look" && i + 1 < argc) look = argv[++i];
    else if (a == "--scene-word" && i + 1 < argc) have_scene_word = true, scene_word = std::atoll(argv[++i]);
    else if (a == "--arena" && i + 1 < argc) arena = std::atoi(argv[++i]);
    else if (a == "--time" && i + 1 < argc) time = std::atof(argv[++i]);
  }
  if (game.empty() || out.empty() || cam.empty()) {
    std::fprintf(stderr, "usage: arena_rec --game DIR --arena N [--time T] --camera-from REF.rsc --out OUT.rsc [--plant swap_sets|dirs|no_twins]\n");
    return 1;
  }
  if (!plant.empty() && plant != "swap_sets" && plant != "dirs" && plant != "no_twins" && plant != "zero_fall") {
    std::fprintf(stderr, "arena_rec: FAIL unknown --plant %s\n", plant.c_str());
    return 1;
  }
  SceneTables ref;
  std::string err;
  if (cam != "none" && (!ReadRecord(cam, ref, err) || !ref.has_camera)) {  // S101 lane 264: --camera-from none = a record without a camera (a capture's scene record carries none)
    std::fprintf(stderr, "arena_rec: FAIL cannot read the camera record %s: %s\n", cam.c_str(), err.empty() ? "no camera" : err.c_str());
    return 2;
  }
  ViewerInit(game);
  if (!ViewerStageKey(true, time, err)) { std::fprintf(stderr, "arena_rec: FAIL stage key: %s\n", err.c_str()); return 3; }
  assets::Model model;
  if (!ViewerLoad("arena", arena, 0, model, err)) { std::fprintf(stderr, "arena_rec: FAIL load arena %d: %s\n", arena, err.c_str()); return 3; }

  // the slot text (pac entry 50001) from the arena pac itself
  std::vector<uint8_t> raw;
  char rel[64];
  std::snprintf(rel, sizeof rel, "pac\\bg\\bg%02d.pac", arena);
  if (!ViewerReadFile(rel, raw, err)) { std::fprintf(stderr, "arena_rec: FAIL read %s: %s\n", rel, err.c_str()); return 3; }
  assets::pac::Pac pc;
  if (!assets::pac::OpenBytes(std::move(raw), rel, pc, err)) { std::fprintf(stderr, "arena_rec: FAIL open %s: %s\n", rel, err.c_str()); return 3; }
  const assets::Bytes text = assets::pac::Get(pc.bytes, pc.pachs[0], 50001);
  if (text.empty()) { std::fprintf(stderr, "arena_rec: FAIL %s has no slot text (entry 50001)\n", rel); return 3; }

  ar::ArenaSnapshot snap = ar::SnapshotFromModel(model, arena, std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), look == "unknown" ? ar::Look::Unknown : look == "entrance" ? ar::Look::Entrance : ar::Look::Match);
  if (snap.slots.empty() || snap.sets.empty()) { std::fprintf(stderr, "arena_rec: FAIL the snapshot is empty (%zu slots, %zu sets)\n", snap.slots.size(), snap.sets.size()); return 3; }

  if (live_shaped) {
    for (ar::SetSnap& st : snap.sets) {
      st.has_fall = false;
      st.n_lights = 5;  // the hook reads a fifth light; the files' sets have none: a copy of light 3 stands in for the test
      std::memcpy(st.dir[4], st.dir[3], sizeof st.dir[4]);
      std::memcpy(st.col[4], st.col[3], sizeof st.col[4]);
    }
  }
  if (plant == "zero_fall") {
    for (ar::SetSnap& st : snap.sets) st.has_fall = true, std::fill(std::begin(st.fall), std::end(st.fall), 0.f);
  }
  if (plant == "swap_sets") {
    std::map<uint32_t, int> use;
    for (const ar::SlotSnap& s : snap.slots)
      if (s.light_set != ar::kNone && s.light_set != 502 && std::any_of(snap.sets.begin(), snap.sets.end(), [&](const ar::SetSnap& x) { return x.set_id == s.light_set; })) use[s.light_set]++;
    std::vector<std::pair<int, uint32_t>> order;
    for (auto& kv : use) order.push_back({-kv.second, kv.first});
    std::sort(order.begin(), order.end());
    if (order.size() < 2) { std::fprintf(stderr, "arena_rec: FAIL swap_sets needs two sets in use\n"); return 3; }
    const uint32_t A = order[0].second, B = order[1].second;
    for (ar::SlotSnap& s : snap.slots) {
      if (s.light_set == A) s.light_set = B;
      else if (s.light_set == B) s.light_set = A;
    }
    std::printf("[arena_rec] plant swap_sets %u<->%u\n", A, B);
  } else if (plant == "dirs") {
    for (ar::SetSnap& s : snap.sets)
      for (int k = 0; k < 5; ++k)
        for (int c = 0; c < 3; ++c) s.dir[k][c] = -s.dir[k][c];
  } else if (plant == "no_twins") {
    for (ar::SlotSnap& s : snap.slots) s.mirror = 0;
  }

  if (from_provider) {
    launcher::GameFiles gf0;
    gf0.SetRoot(std::filesystem::u8path(game));
    std::string e0;
    if (!ar::SlotsFromFiles(gf0, arena, snap.slots, snap.screens, e0)) { std::fprintf(stderr, "arena_rec: FAIL provider: %s\n", e0.c_str()); return 3; }
  }
  if (have_scene_word) snap.look = ar::Look::Unknown;  // what the slot provider's first publish carries before the per-frame read
  ar::PublishAll(snap);
  if (have_scene_word) ar::PublishLook(ar::LookFromScenePhase(scene_word >= 0, uint32_t(scene_word)));
  SceneTables t;
  t.header.source = uint8_t(Source::Live);
  t.header.guest_frame = 0;
  if (cam != "none") t.camera = ref.camera;
  // S101 lane27 (one space rule, SCENE_RECORD): a Live record carries the GAME's camera. The viewer camera copied from REF is converted to game space
  // (S on the world side: row 1 of view / viewproj negated, eye y negated) so a reader's single Live -> viewer conversion gives the viewer camera back.
  for (int k = 4; k < 8; ++k) t.camera.view.m[k] = -t.camera.view.m[k];
  t.camera.eye[1] = -t.camera.eye[1];
  t.has_camera = cam != "none";
  live::Counters c;
  live::FrameInfo fi;
  ar::Instance()->Emit(fi, t, c);
  if (!WriteRecord(out, t, err)) { std::fprintf(stderr, "arena_rec: FAIL write %s: %s\n", out.c_str(), err.c_str()); return 3; }
  // review F1: read the record back onto a fresh model; the model's own falloff must survive a record that does not know it
  int fall_kept = 1;
  {
    ar::RecordView view;
    assets::Model m2;
    std::string e3;
    if (!ar::ReadArenaRecord(t, view, e3) || !ViewerLoad("arena", arena, 0, m2, e3)) { std::fprintf(stderr, "arena_rec: FAIL read-back: %s\n", e3.c_str()); return 3; }
    const assets::Model before = m2;
    ar::ApplyToModel(view, m2);
    for (size_t i = 0; i < m2.light_sets.size(); ++i)
      for (int k = 0; k < 4; ++k)
        if (m2.light_sets[i].fall[k] != before.light_sets[i].fall[k]) fall_kept = 0;
  }
  size_t invisible = 0, vis_marked = 0, when_slots = 0, when_objects = 0, when_visible = 0;
  for (const Object& o : t.objects) invisible += (o.flags & kObjVisible) ? 0 : 1;
  for (const Object& o : t.objects)
    for (uint32_t k = 0; k < o.params_count; ++k)
      if (t.Str(t.params[o.params_first + k].name) == "arena.slot_when") ++when_objects, when_visible += (o.flags & kObjVisible) ? 1 : 0;
  for (const ParamValue& p : t.params) vis_marked += t.Str(p.name) == "arena.visibility_unknown" ? 1 : 0;
  for (const ar::SlotSnap& sl : snap.slots) when_slots += sl.when ? 1 : 0;
  int pmissing = -1, pextra = -1, pmismatch = -1;
  if (provider_check) {
    launcher::GameFiles gf;
    gf.SetRoot(std::filesystem::u8path(game));
    std::vector<ar::SlotSnap> ps;
    std::vector<ar::ScreenSnap> pscr;
    std::string e4;
    if (!ar::SlotsFromFiles(gf, arena, ps, pscr, e4)) { std::fprintf(stderr, "arena_rec: FAIL provider: %s\n", e4.c_str()); return 3; }
    pmissing = pextra = pmismatch = 0;
    for (const ar::SlotSnap& m : snap.slots) {
      const ar::SlotSnap* f = nullptr;
      for (const ar::SlotSnap& p : ps) if (p.slot == m.slot) f = &p;
      if (!f) { ++pmissing; continue; }
      if (f->light_set != m.light_set || f->mirror != m.mirror || f->blend != m.blend || f->when != m.when || f->motion != m.motion) ++pmismatch;
    }
    for (const ar::SlotSnap& p : ps) {
      bool in = false;
      for (const ar::SlotSnap& m : snap.slots) in = in || m.slot == p.slot;
      pextra += in ? 0 : 1;
    }
  }
  std::printf("[arena_rec] when_objects=%zu when_visible=%zu\n", when_objects, when_visible);
  std::printf("[arena_rec] fall_kept=%d invisible=%zu vis_marked=%zu when_slots=%zu provider_missing=%d provider_extra=%d provider_mismatch=%d\n", fall_kept, invisible, vis_marked,
              when_slots, pmissing, pextra, pmismatch);
  const ar::Totals tot = ar::GetTotals();
  size_t expect_twins = 0;
  for (const ar::SlotSnap& s : ar::ParseSlotText(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()))) {
    bool present = false;
    for (const ar::SlotSnap& m : snap.slots) present = present || m.slot == s.slot;
    if (present) expect_twins += (s.mirror & 1 ? 1 : 0) + (s.mirror & 2 ? 1 : 0);
  }
  std::printf("[arena_rec] arena=%d time=%g objects=%u slots=%zu twins=%llu expect_twins=%zu sets=%llu lights=%zu screens=%llu unknown=%u stale=%u\n", arena, time, c.objects,
              snap.slots.size(), (unsigned long long)tot.twins, expect_twins, (unsigned long long)tot.sets, t.lights.size(), (unsigned long long)tot.screens, c.unknown, c.stale);
  std::printf("wrote %s\n", out.c_str());
  return 0;
}
