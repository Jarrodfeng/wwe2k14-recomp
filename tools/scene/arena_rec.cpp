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
// --camera-screens entrance|match : WGS (with --camera-from none): the record carries that look's BEAUTY camera (arena_cams.h BeautyCam; [--canvas-top Y] [--stage-xz X,Z]) and prints
//   "[arena_rec] screen_keys K,... camera beauty look L stage <piece K | stage rect | no stage found> eye ... aim ... fov F dist D" (the keys 800-809 with a mesh, from the every-key model)
// --screen-pieces [--check-size WxH] : WGS: every screen key's pieces (arena_cams.h SplitPieces, the 32 largest), one line per camera, game space, for frame_rig --camera / --eye:
//   "[arena_rec] screen_key K pieces N kept M dropped_area_share S" and
//   "[arena_rec] piece K I area A tris T centroid x,y,z normal x,y,z weak W view front|back|x+|x-|z+|z- expect_px E vp 16 eye 3". tools\scene\arena_sheet.py reads them
// --cull-census (SCU, needs only --game / --arena): the stage slots 0..399 of the live key set (all keys, both looks' slots) as the LOADER read them: one line
//   "[arena_rec] cull_census arena N set S clear C r_set .. r_clear .. y_set .. y_clear .. ry_set .. m_set .. m_clear .. det_not_r 0 flag_disagrees 0 skipped_meshes K words 0x4:..,0x6:.."
//   (record word +0x8C bit 0x2; r / y / m(N) from the slot's TOKEN via the placement label; det_not_r = copies whose determinant sign disagrees with the `r` token) and one
//   "cull_census_m ..." line per bit-set mesh of a motion slot m(N); nothing written
//   It sets load_detail::KeepLookSlots() and stage_media::Cfg().all_keys itself because those are the LIVE load's settings (live_assets.cpp Load): the census counts what the live model holds.
// entry: ViewerLoad (the loader seam -> assets::LoadArena -> LoadJboy)
// Exit 0 written; 2 bad input; 3 any other failure. Prints one line "[arena_rec] key=value ..." with the counts the selftest reads.
#include "stb_image.h"  // declarations only: the loader seam defines the implementation

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "../../src/assets/pac.h"
#include "../../src/assets/stage_media.h"   // WGS: Cfg().all_keys (the screens camera loads every key 800-809)
#include "../../src/assets/load_plants.h"   // SCU: load_detail::KeepLookSlots (the cull census loads both looks' slots)
#include "../../src/assets/view/loader_seam.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/arena_apply.h"
#include "../../src/scene_live/arena_contrib.h"
#include "../../src/scene_live/arena_provider.h"

using namespace rdr_scene;
namespace ar = rdr_scene::live::arena;

#include "arena_cams.h"   // WGS round 2: the check pass pieces and the beauty camera

int main(int argc, char** argv) {
  std::string game, out, cam, plant, look = "match", camera_screens;
  bool screen_pieces = false, have_stage_xz = false, cull_census = false;
  double canvas_top = 120.0, stage_xz[2] = {0, 0};
  int check_w = 640, check_h = 360;   // WGS fix round: --check-size WxH, the check renders' size (expect_px is counted at it)
  std::string footprints;             // WGS fix round: --footprints DIR: each view's footprint as DIR\kK_pI_VIEW.fp (packed bits)
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
    else if (a == "--camera-screens" && i + 1 < argc) camera_screens = argv[++i];   // WGS: entrance | match (needs --camera-from none): the beauty camera of that look
    else if (a == "--check-size" && i + 1 < argc) { if (std::sscanf(argv[++i], "%dx%d", &check_w, &check_h) != 2 || check_w <= 0 || check_h <= 0) { std::fprintf(stderr, "arena_rec: FAIL bad --check-size\n"); return 1; } }
    else if (a == "--footprints" && i + 1 < argc) footprints = argv[++i];
    else if (a == "--cull-census") cull_census = true;   // SCU: one line "[arena_rec] cull_census arena N set S clear C r_set .. y_set .. words ..." (the stage record's +0x8C bit 0x2 as the loader set it), then exit
    else if (a == "--screen-pieces") screen_pieces = true;                        // WGS round 2: print every kept screen piece with its front-on cameras (the check pass)
    else if (a == "--canvas-top" && i + 1 < argc) canvas_top = std::atof(argv[++i]);  // WGS round 2: the ring canvas top (y up; arena_sheet.py reads entry 953)
    else if (a == "--stage-xz" && i + 1 < argc) { have_stage_xz = std::sscanf(argv[++i], "%lf,%lf", &stage_xz[0], &stage_xz[1]) == 2; }   // WGS round 2: the stage rect's centre (entry 50003 light 200)
  }
  if (!camera_screens.empty() && ((camera_screens != "entrance" && camera_screens != "match") || cam != "none")) {
    std::fprintf(stderr, "arena_rec: FAIL --camera-screens entrance|match goes with --camera-from none\n");
    return 1;
  }
  if (cull_census) {   // SCU first step: the stage record's cull bit as the LOADER sets it, every stage slot 0..399 (all keys, both looks' slots), no record written
    if (game.empty()) { std::fprintf(stderr, "arena_rec: FAIL --cull-census needs --game\n"); return 1; }
    ViewerInit(game);
    std::string e0;
    if (!ViewerStageKey(true, time, e0)) { std::fprintf(stderr, "arena_rec: FAIL stage key: %s\n", e0.c_str()); return 3; }
    const bool all0 = assets::stage_media::Cfg().all_keys, keep0 = assets::load_detail::KeepLookSlots();
    assets::stage_media::Cfg().all_keys = true, assets::load_detail::KeepLookSlots() = true;   // the live load's key set: 301-304 and the e / g slots stay
    assets::Model cm;
    const bool ok = ViewerLoad("arena", arena, 0, cm, e0);
    assets::stage_media::Cfg().all_keys = all0, assets::load_detail::KeepLookSlots() = keep0;
    if (!ok) { std::fprintf(stderr, "arena_rec: FAIL load arena %d: %s\n", arena, e0.c_str()); return 3; }
    uint64_t set = 0, clear = 0, r_set = 0, r_clear = 0, y_set = 0, y_clear = 0, ry_set = 0, m_set = 0, m_clear = 0, det_not_r = 0, other_set = 0, skipped = 0, own_total = 0, own_overlap = 0, own_still_record = 0;
    std::map<uint32_t, uint64_t> words;
    for (const assets::Mesh& me : cm.meshes) {
      if (me.src_kind != 1 || me.src_key < 0 || me.src_key >= 400 || me.material < 0) continue;
      const assets::Material& mt = cm.materials[size_t(me.material)];
      const bool b = (mt.cull_word & 0x2u) != 0;
      // a stage-slot mesh the movie-surface rule (stage_media.h Apply: its flat light set id 9000, cull_back forced) also took: the own rule wins (cull_from_record cleared there)
      // (offline there is no media folder, so Apply builds nothing: the candidates are counted by the sampler names its build() maps, stage_media.h, whether or not a picture exists)
      static const char* const kMovieNames[] = {"ppv_header&wall", "ss_header&wall", "mini_tron", "titantron_movie", "main&banner"};
      bool own_rule = me.light_set >= 0 && size_t(me.light_set) < cm.light_sets.size() && cm.light_sets[size_t(me.light_set)].id == 9000;
      for (const char* nm : kMovieNames) own_rule = own_rule || mt.diffuse_name == nm;
      if (own_rule) { ++own_total; if (b) ++own_overlap; if (mt.cull_from_record) ++own_still_record; }
      if (!own_rule && (b != mt.cull_from_record || b != mt.cull_back)) ++other_set;   // the loader's flag disagrees with its own word (must be 0)
      (b ? set : clear)++;
      ++words[mt.cull_word];
      // The slot's kind from its TOKEN, as LoadArena labels the placement (asset_load.h): " mirrored" = an `r` slot (a `y` token too when it has 3 copies), " turned" = `y` only,
      // " m(N)" = a motion slot (one placement, a rotation). (YCP 2026-11-20: a determinant / sign test cannot tell a `y` copy from a motion that turns a piece past 90 degrees.)
      bool r = false, y = false, mo = false, neg = false;
      if (me.placement >= 0 && size_t(me.placement) < cm.placements.size()) {
        const assets::Placement& pl = cm.placements[size_t(me.placement)];
        const auto ends = [&](const char* s) { const size_t n = std::strlen(s); return pl.label.size() >= n && pl.label.compare(pl.label.size() - n, n, s) == 0; };
        r = ends(" mirrored"), y = ends(" turned") || (r && pl.transforms.size() == 3), mo = pl.label.find(" m(") != std::string::npos;
        for (const assets::Matrix& t : pl.transforms)
          if (t[0] * (t[5] * t[10] - t[6] * t[9]) - t[1] * (t[4] * t[10] - t[6] * t[8]) + t[2] * (t[4] * t[9] - t[5] * t[8]) < 0.f) neg = true;
      }
      if (neg != r) ++det_not_r;   // a negative-determinant copy outside an `r` slot, or an `r` slot without one (must be 0: the front end's mirrored flag is the determinant)
      if (r) (b ? r_set : r_clear)++;
      if (y && !r) (b ? y_set : y_clear)++;   // a `y`-only slot (a slot with both tokens is counted once, as `r`, and in ry_set)
      if (y && r && b) ++ry_set;
      if (mo) (b ? m_set : m_clear)++;
      if (mo && b) {   // + the mesh's placed bounds in GAME space (the viewer's Y mirror taken off), for stage_cull_sheet.py's slot cameras
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (const assets::Matrix& t : cm.placements[size_t(me.placement)].transforms)
          for (const assets::Vertex& v : me.vertices)
            for (int c = 0; c < 3; ++c) {
              float w = v.pos[0] * t[size_t(c)] + v.pos[1] * t[size_t(4 + c)] + v.pos[2] * t[size_t(8 + c)] + t[size_t(12 + c)];
              if (c == 1) w = -w;
              lo[c] = std::min(lo[c], w), hi[c] = std::max(hi[c], w);
            }
        std::printf("[arena_rec] cull_census_m arena %d slot %d mesh %d word 0x%X effect %s placement %s bounds %.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n", arena, me.src_key, me.src_record, mt.cull_word, mt.shader.c_str(),
                    cm.placements[size_t(me.placement)].label.c_str(), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
      }
    }
    for (const std::string& w : cm.warnings) if (w.find("skipped") != std::string::npos) ++skipped;
    std::string ws;
    for (const auto& kv : words) { char b[48]; std::snprintf(b, sizeof b, "%s0x%X:%llu", ws.empty() ? "" : ",", kv.first, (unsigned long long)kv.second); ws += b; }
    std::printf("[arena_rec] cull_census arena %d set %llu clear %llu r_set %llu r_clear %llu y_set %llu y_clear %llu ry_set %llu m_set %llu m_clear %llu det_not_r %llu flag_disagrees %llu skipped_meshes %llu own_rule %llu own_rule_with_bit %llu own_rule_still_record %llu words %s\n", arena, (unsigned long long)set, (unsigned long long)clear,
                (unsigned long long)r_set, (unsigned long long)r_clear, (unsigned long long)y_set, (unsigned long long)y_clear, (unsigned long long)ry_set, (unsigned long long)m_set, (unsigned long long)m_clear,
                (unsigned long long)det_not_r, (unsigned long long)other_set, (unsigned long long)skipped, (unsigned long long)own_total, (unsigned long long)own_overlap, (unsigned long long)own_still_record, ws.c_str());
    return 0;
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
  bool screen_cam = false;
  if (!camera_screens.empty() || screen_pieces) {   // WGS: the screen pieces (the check pass) and the beauty camera (viewer space), the camera written as a view-projection like a viewer record's
    // the viewer model holds only one look's keys (stage_media StageKeys: the match set unless a wrestler is set); the camera needs every key, as the live model loads them (all_keys)
    assets::Model keys_model;
    std::string ek;
    const bool all0 = assets::stage_media::Cfg().all_keys;
    assets::stage_media::Cfg().all_keys = true;
    const bool keys_ok = ViewerLoad("arena", arena, 0, keys_model, ek);
    assets::stage_media::Cfg().all_keys = all0;
    if (!keys_ok) { std::fprintf(stderr, "arena_rec: FAIL load arena %d with every screen key: %s\n", arena, ek.c_str()); return 3; }
    const std::map<int, std::vector<wgs::Tri>> tris = wgs::ScreenTris(keys_model);
    std::map<int, std::vector<wgs::Piece>> kept;
    std::string ks;
    for (const auto& kv : tris) {
      if (kv.second.empty()) continue;
      ks += (ks.empty() ? "" : ",") + std::to_string(kv.first);
      std::vector<wgs::Piece> all;
      kept[kv.first] = wgs::SplitPieces(kv.first, kv.second, 0.0, 32, &all);   // WGS fix round: every piece is judged, the 32 largest at most
      if (!screen_pieces) continue;
      const std::vector<wgs::TriRef> order = wgs::KeyDrawOrder(all);   // WGS6: the key's triangles in draw order, every piece (the occlusion model of ExpectPx)
      double area_all = 0, area_kept = 0;
      for (const wgs::Piece& p : all) area_all += p.area;
      for (const wgs::Piece& p : kept[kv.first]) area_kept += p.area;
      std::printf("[arena_rec] screen_key %d pieces %zu kept %zu dropped_area_share %.4f\n", kv.first, all.size(), kept[kv.first].size(), area_all > 0 ? 1.0 - area_kept / area_all : 0.0);
      for (const wgs::Piece& p : kept[kv.first]) {
        // one line per camera: a piece with a normal gets `front` (the side its normal points to) and `back` (used only when the front draws nothing: PASS-BACK);
        // a weak-normal (closed / folded) piece gets four horizontal views x+ x- z+ z-, each judged. expect_px = its camera-facing triangles rasterised (ExpectPx) at check_w x check_h.
        std::vector<std::pair<std::string, wgs::V3>> views;
        if (p.weak_normal) views = {{"x+", {1, 0, 0}}, {"x-", {-1, 0, 0}}, {"z+", {0, 0, 1}}, {"z-", {0, 0, -1}}};
        else views = {{"front", p.normal}, {"back", {-p.normal[0], -p.normal[1], -p.normal[2]}}};
        uint32_t vcol = 0;   // the per-byte median of the piece's vertex colours (bits 24-31 = alpha, as rf_c_core.cpp reads it)
        for (int sh = 0; sh < 32; sh += 8) {
          std::vector<uint32_t> bytes;
          for (uint32_t c0 : p.cols) bytes.push_back((c0 >> sh) & 0xFFu);
          if (!bytes.empty()) { std::nth_element(bytes.begin(), bytes.begin() + bytes.size() / 2, bytes.end()); vcol |= bytes[bytes.size() / 2] << sh; }
        }
        // one view: the footprint and its planes are written (write = true) and the camera line printed; returns false on a write failure. expect / hidden are returned through the pointers.
        const auto emit = [&](const std::string& name, const wgs::V3& dir, bool write, uint64_t* expect_out, uint64_t* hidden_out) -> bool {
          const wgs::Cam pc = wgs::PieceCamDir(p, dir);
          std::vector<uint8_t> fp, vc, va;
          uint64_t hidden_px = 0;
          const bool planes = write && !footprints.empty();
          const uint64_t expect = wgs::ExpectPx(p, pc, check_w, check_h, planes ? &fp : nullptr, planes ? &vc : nullptr, planes ? &va : nullptr, &order, &hidden_px);
          *expect_out = expect, *hidden_out = hidden_px;
          if (!write) return true;
          if (planes) {   // the footprint, packed 8 pixels a byte (MSB first), row-major: the driver judges only the piece's own pixels
            std::string vn = name;
            for (char& ch : vn) ch = ch == '+' ? 'p' : ch == '-' ? 'm' : ch;
            const std::string path = footprints + "\\k" + std::to_string(p.key) + "_p" + std::to_string(p.index) + "_" + vn + ".fp";
            std::vector<uint8_t> packed((fp.size() + 7) / 8, 0);
            for (size_t q = 0; q < fp.size(); ++q)
              if (fp[q]) packed[q >> 3] = uint8_t(packed[q >> 3] | (0x80u >> (q & 7)));
            if (FILE* f = std::fopen(path.c_str(), "wb")) { std::fwrite(packed.data(), 1, packed.size(), f); std::fclose(f); }
            else { std::fprintf(stderr, "arena_rec: FAIL cannot write %s\n", path.c_str()); return false; }
            const std::string vpath = path.substr(0, path.size() - 3) + ".vc";   // DIM1 + WGS6: the composite vertex colour per pixel (3 bytes R G B, the footprint's rows)
            if (FILE* f = std::fopen(vpath.c_str(), "wb")) { std::fwrite(vc.data(), 1, vc.size(), f); std::fclose(f); }
            else { std::fprintf(stderr, "arena_rec: FAIL cannot write %s\n", vpath.c_str()); return false; }
            const std::string apath = path.substr(0, path.size() - 3) + ".va";   // review round 5 + WGS6: the composite alpha per pixel (1 byte, the footprint's rows)
            if (FILE* f = std::fopen(apath.c_str(), "wb")) { std::fwrite(va.data(), 1, va.size(), f); std::fclose(f); }
            else { std::fprintf(stderr, "arena_rec: FAIL cannot write %s\n", apath.c_str()); return false; }
          }
          float vp[16], ey[3];
          wgs::ToGame(pc, vp, ey);
          std::string line;
          char b[240];
          std::snprintf(b, sizeof b, "[arena_rec] piece %d %d area %.0f tris %zu centroid %.1f,%.1f,%.1f normal %.3f,%.3f,%.3f weak %d blend %d vcol 0x%08X view %s expect_px %llu vp ", p.key, p.index, p.area, p.tris,
                        p.centroid[0], p.centroid[1], p.centroid[2], p.normal[0], p.normal[1], p.normal[2], int(p.weak_normal), int(p.blended), vcol, name.c_str(), (unsigned long long)expect);
          line = b;
          for (int i = 0; i < 16; ++i) { std::snprintf(b, sizeof b, "%s%.9g", i ? "," : "", vp[i]); line += b; }
          std::snprintf(b, sizeof b, " eye %.9g,%.9g,%.9g hidden_px %llu", ey[0], ey[1], ey[2], (unsigned long long)hidden_px);   // WGS6: hidden_px = this piece's pixels an earlier nearer piece covers
          line += b;
          std::printf("%s\n", line.c_str());
          return true;
        };
        bool all_hidden = true, any_view = false;   // WGS6 round 2: a piece hidden from every one of its cameras gets one more camera ("alt"): the best of a ring of directions
        for (const auto& v : views) {
          uint64_t ex = 0, hd = 0;
          if (!emit(v.first, v.second, true, &ex, &hd)) return 3;
          if (ex + hd > 0) any_view = true, all_hidden = all_hidden && hd > ex;   // a view mostly hidden: more of its pixels covered than visible
        }
        if (any_view && all_hidden) {
          std::vector<wgs::V3> cands;
          const double pi = 3.14159265358979;
          if (p.weak_normal) {   // a closed / folded piece: eight yaws at five pitches (+ straight up / down), y is up
            for (int pitch : {-50, -25, 0, 25, 50})
              for (int yaw = 0; yaw < 8; ++yaw) {
                const double pr = pitch * pi / 180.0, yr = yaw * pi / 4.0;
                cands.push_back({std::cos(pr) * std::cos(yr), std::sin(pr), std::cos(pr) * std::sin(yr)});
              }
            cands.push_back({0, 1, 0}), cands.push_back({0, -1, 0});
          } else {   // a one-sided piece: a cone around its normal, three tilts, eight yaws
            const wgs::V3 up = std::fabs(p.normal[1]) > 0.9 ? wgs::V3{0, 0, 1} : wgs::V3{0, 1, 0};
            const wgs::V3 u = wgs::Norm(wgs::Cross(up, p.normal)), w2 = wgs::Cross(p.normal, u);
            for (int tilt : {25, 50, 70})
              for (int yaw = 0; yaw < 8; ++yaw) {
                const double tr = tilt * pi / 180.0, yr = yaw * pi / 4.0;
                wgs::V3 d;
                for (int c = 0; c < 3; ++c) d[size_t(c)] = std::cos(tr) * p.normal[size_t(c)] + std::sin(tr) * (std::cos(yr) * u[size_t(c)] + std::sin(yr) * w2[size_t(c)]);
                cands.push_back(wgs::Norm(d));
              }
          }
          uint64_t best_ex = 0, best_hd = 0;
          int best = -1;
          for (size_t ci = 0; ci < cands.size(); ++ci) {
            uint64_t ex = 0, hd = 0;
            emit("alt", cands[ci], false, &ex, &hd);
            if (ex > best_ex) best_ex = ex, best_hd = hd, best = int(ci);
          }
          if (best >= 0) {
            uint64_t ex = 0, hd = 0;
            if (!emit("alt", cands[size_t(best)], true, &ex, &hd)) return 3;
          }
          (void)best_hd;
        }
      }
    }
    if (!camera_screens.empty()) {   // the beauty camera of the look
      const int big = camera_screens == "entrance" ? 804 : 800;
      wgs::V3 st{0, canvas_top, 3000}, lo{-300, canvas_top, 2700}, hi{300, canvas_top + 600, 3300};
      std::string stage = "no stage found";
      if (kept.count(big) && !kept[big].empty()) {
        const wgs::Piece& p = kept[big][0];
        st = p.centroid, lo = p.lo, hi = p.hi, stage = "piece " + std::to_string(big);
      } else if (have_stage_xz) {
        st = {stage_xz[0], canvas_top, stage_xz[1]}, lo = {stage_xz[0] - 300, canvas_top, stage_xz[1] - 300}, hi = {stage_xz[0] + 300, canvas_top + 600, stage_xz[1] + 300}, stage = "stage rect";
      }
      const wgs::Cam bc = wgs::BeautyCam(keys_model, st, lo, hi, canvas_top);
      std::memcpy(t.camera.view.m, bc.vp, sizeof bc.vp);
      t.camera.proj = IdentityMat4();
      t.camera.flags = kCamViewIsViewProj;
      for (int j = 0; j < 3; ++j) t.camera.eye[j] = float(bc.eye[j]);
      t.camera.fov_y_deg = float(bc.fov);
      t.camera.near_z = float(bc.near_z), t.camera.far_z = float(bc.far_z);
      screen_cam = true;
      std::printf("[arena_rec] screen_keys %s camera beauty look %s stage %s eye %.1f,%.1f,%.1f aim %.1f,%.1f,%.1f fov %.1f dist %.1f\n", ks.empty() ? "none" : ks.c_str(), camera_screens.c_str(),
                  stage.c_str(), bc.eye[0], bc.eye[1], bc.eye[2], bc.target[0], bc.target[1], bc.target[2], bc.fov, bc.dist);
    } else {
      std::printf("[arena_rec] screen_keys %s\n", ks.empty() ? "none" : ks.c_str());
    }
  }
  // S101 lane27 (one space rule, SCENE_RECORD): a Live record carries the GAME's camera. The viewer camera copied from REF is converted to game space
  // (S on the world side: row 1 of view / viewproj negated, eye y negated) so a reader's single Live -> viewer conversion gives the viewer camera back.
  for (int k = 4; k < 8; ++k) t.camera.view.m[k] = -t.camera.view.m[k];
  t.camera.eye[1] = -t.camera.eye[1];
  t.has_camera = cam != "none" || screen_cam;
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
