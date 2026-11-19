// maps_draw (Session 103, lane E): draws the maps of lane E (src\renderer_scene\live_maps.h) from ONE scene record, headless, and writes them as PNGs:
//   maps_draw CAPTURE_FOLDER --frame N --game DIR --out OUTDIR [--fault LIST] [--other-frame M]
// OUTDIR gets ours_shadow.png (RGBA16F map as the display copy), ours_circles.png and counts.txt. It is the stand-in for lane R's rig until that lands: the record's wrestler
// assets are loaded with the viewer's loaders (as rsc_draw does), the record is converted to the viewer's space once (SCENE_RECORD "One space rule"), then the Maps stage hook
// records its passes into a frame that is rendered and read back. Exit 0 = the maps were made; 2 = bad record / args; 3 = a failure; 4 = validation errors.
// --fault: transpose_light, no_casters, circles_wrong_frame (needs --other-frame M: the circle rows of record M are used for this frame's).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/assets/view/loader_seam.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/live_maps.h"
#include "../../src/renderer_scene/props_map_rows.h"   // S103 PMS: --props-debug
#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/plane_eye_read.h"   // S103 E5
#include "e5_fake_memory.h"                           // S103 E5

using namespace rdr_scene;

namespace {

uint32_t Crc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ d[i]) & 255] ^ (crc >> 8);
  return ~crc;
}
void Put32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s)); }
void Chunk(std::vector<uint8_t>& out, const char* kind, const std::vector<uint8_t>& body) {
  Put32(out, uint32_t(body.size()));
  std::vector<uint8_t> kb(kind, kind + 4);
  kb.insert(kb.end(), body.begin(), body.end());
  out.insert(out.end(), kb.begin(), kb.end());
  Put32(out, Crc32(kb.data(), kb.size()));
}
bool WritePngRgb(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    for (uint32_t x = 0; x < w; ++x) {
      const uint8_t* p = &rgba[(size_t(y) * w + x) * 4];
      raw.push_back(p[0]), raw.push_back(p[1]), raw.push_back(p[2]);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
  for (size_t pos = 0; pos < raw.size() || pos == 0;) {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n & 255)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n & 255)), z.push_back(uint8_t((~n >> 8) & 255));
    z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
    pos += n;
    if (last) break;
  }
  Put32(z, (b << 16) | a);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  std::vector<uint8_t> ihdr;
  Put32(ihdr, w), Put32(ihdr, h);
  ihdr.push_back(8), ihdr.push_back(2), ihdr.push_back(0), ihdr.push_back(0), ihdr.push_back(0);
  Chunk(out, "IHDR", ihdr);
  Chunk(out, "IDAT", z);
  Chunk(out, "IEND", {});
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
  return bool(f);
}

bool ResolveRecord(std::string folder, long frame, std::string& path, std::string& err) {
  std::error_code ec;
  char name[32];
  std::snprintf(name, sizeof name, "frame_%06ld.rsc", frame);
  std::filesystem::path p = std::filesystem::path(folder) / "scene" / name;
  if (!std::filesystem::exists(p, ec)) p = std::filesystem::path(folder) / name;
  if (!std::filesystem::exists(p, ec)) { err = "no scene record for frame " + std::to_string(frame) + " under " + folder; return false; }
  path = p.string();
  return true;
}

// One space rule (rsc_draw.cpp ConvertLiveToViewer): the record's world / bone matrices take S X S, so the viewer's loaders' meshes (Y mirrored once) fit.
void ConvertLiveToViewer(SceneTables& t) {
  auto mirror = [](Mat4& m) {
    assets::Matrix a;
    std::memcpy(a.data(), m.m, sizeof(float) * 16);
    a = assets::MirrorY(a);
    std::memcpy(m.m, a.data(), sizeof(float) * 16);
  };
  for (Object& o : t.objects) mirror(o.world);
  for (Mat4& m : t.bones) mirror(m);
  t.header.source = uint8_t(Source::Viewer);
}

// S103 E2: the rig-told inputs (a test fixture, NOT a live path): text lines "KEY v v v ...". The values are what the game's own draws held; they stand in for the readers lane A has to build
// (plane_eye / plane_pos / plane_fade / plane_fit / plane_camera: kScPlaneFrames / kScPlaneDraws / EyePos; prelight_slot, sweat_item: the nodes' inputs).
bool ApplyTold(const std::string& path, E2Input& e2) {
  std::ifstream f(path);
  if (!f) return false;
  std::string line;
  PlaneFrame pf;
  bool any_plane = false;
  while (std::getline(f, line)) {
    std::istringstream ss(line);
    std::string key;
    ss >> key;
    std::vector<float> v;
    float x;
    while (ss >> x) v.push_back(x);
    if (key == "plane_eye" && v.size() >= 3) { e2.plane_eye_told = true; for (size_t i = 0; i < 4; ++i) e2.plane_eye[i] = i < v.size() ? v[i] : 1.f; }
    else if (key == "plane_pos" && v.size() >= 3) { if (!e2.plane_pos_from_record) { for (size_t i = 0; i < 4 && i < v.size(); ++i) e2.plane_pos[i] = v[i]; e2.have_plane_pos = true; } any_plane = true; }   // told only when the record has none
    else if (key == "plane_fade" && v.size() >= 10) { std::memcpy(pf.fade, v.data(), sizeof pf.fade); pf.have |= kPlaneHaveFade; any_plane = true; }
    else if (key == "plane_fit" && v.size() >= 16) { std::memcpy(pf.fit_matrix, v.data(), sizeof pf.fit_matrix); pf.have |= kPlaneHaveFit; }
    else if (key == "plane_camera" && v.size() >= 16) { std::memcpy(pf.camera, v.data(), sizeof pf.camera); pf.have |= kPlaneHaveCamera; }
    else if (key == "plane_normal" && v.size() >= 4) { std::memcpy(pf.plane_normal, v.data(), sizeof pf.plane_normal); pf.have |= kPlaneHaveNormal; }
    else if (key == "prelight_slot" && v.size() >= 1 + 16 + 16 + 4 + 16 + 4 + 12 + 12) {   // id, c8-c11, c0-c3, c198, c16-c19, c199, c204-c206, c207-c209
      PreLightSlotInput p;
      size_t k = 0;
      p.light_set = uint32_t(v[k++]);
      std::memcpy(p.dif_lgh_vec, &v[k], 64), k += 16;
      std::memcpy(p.dif_lgh_col, &v[k], 64), k += 16;
      std::memcpy(p.dif_lgh_max_d, &v[k], 16), k += 4;
      std::memcpy(p.dif_lgh_prm, &v[k], 64), k += 16;
      std::memcpy(p.dif_lgh_max_a, &v[k], 16), k += 4;
      std::memcpy(p.sss_lobe_a, &v[k], 48), k += 12;
      std::memcpy(p.sss_lobe_b, &v[k], 48), k += 12;
      e2.prelight_slots.push_back(p);
    }
  }
  if (any_plane && pf.have == kPlaneHaveFade + kPlaneHaveFit + kPlaneHaveCamera) pf.have |= kPlaneHaveNormal;
  if (any_plane) e2.plane_frame = pf, e2.have_plane = pf.have == kPlaneHaveAll, e2.plane_told = true;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string cap, game, out_dir, fault_list, told_path;
  long frame = -1, other_frame = -1, bones_frame = -1;
  bool props_debug = false;
  bool circles_as_latch = false, const_eye = false, eye_module = false;   // S103 E4; S103 E5: eye_module
  int eye_plant = 0;   // S103 E5
  std::map<int, std::array<int, 2>> wrestler_of_slot;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
    else if (a == "--frame" && i + 1 < argc) frame = std::atol(argv[++i]);
    else if (a == "--other-frame" && i + 1 < argc) other_frame = std::atol(argv[++i]);
    else if (a == "--fault" && i + 1 < argc) fault_list = argv[++i];
    else if (a == "--told" && i + 1 < argc) told_path = argv[++i];   // S103 E2: inputs the record does not carry yet, told to the rig (see ApplyTold)
    else if (a == "--bones-frame" && i + 1 < argc) bones_frame = std::atol(argv[++i]);
    else if (a == "--props-debug") props_debug = true;   // S103 PMS: print each light state's Trapezoidal x ViewProj against the circle rows (why a props ShadowPrm is or is not found)
    else if (a == "--const-eye") const_eye = true;   // S103 E4 FIXTURE: brock_losswin's arena values for the wrestler proxy's EyePos and PlanePos (review F2: per-arena data, not live)
    else if (a == "--eye-module") eye_module = true;   // S103 E5 FIXTURE: the plane module of brock_losswin's arena in a fake guest memory, read by the LATCH reader (plane_eye_read.h) into the record's PlaneEyes
    else if (a == "--eye-plant" && i + 1 < argc) eye_plant = std::atoi(argv[++i]);   // S103 E5: scene_inputs::g_eye_plant
    else if (a == "--circles-as-latch") circles_as_latch = true;   // S103 E4: the walk's circle rows carry A3's kCircleFromLatch (0x100): the rows a latch writes must be accepted the same
    else if (a == "--wrestler" && i + 1 < argc) {   // SLOT=CHID[:ATTIRE]: a live record names no model file for a wrestler (model_key only): the rig is told which character stands in slot N
      int slot = 0, ch = 0, attire = 0;
      if (std::sscanf(argv[++i], "%d=%d:%d", &slot, &ch, &attire) < 2) { std::fprintf(stderr, "bad --wrestler (SLOT=CHID[:ATTIRE])\n"); return 2; }
      wrestler_of_slot[slot] = {ch, attire};
    }
    else if (cap.empty()) cap = a;
  }
  if (cap.empty() || game.empty() || out_dir.empty() || frame < 0) {
    std::fprintf(stderr, "usage: maps_draw CAPTURE_FOLDER --frame N --game DIR --out OUTDIR [--fault LIST] [--other-frame M]\n");
    return 2;
  }
  MapsFaults faults;
  std::string err;
  // S103 E2: the E2 maps' plants are told apart by name; the rest goes to lane E's parser
  E2Faults e2faults;
  {
    std::string e_list, e2_list;
    size_t at = 0;
    while (at <= fault_list.size()) {
      size_t comma = fault_list.find(',', at);
      if (comma == std::string::npos) comma = fault_list.size();
      const std::string w = fault_list.substr(at, comma - at);
      at = comma + 1;
      if (w.empty()) continue;
      const bool is_e2 = w.rfind("rim_", 0) == 0 || w.rfind("plane_", 0) == 0 || w.rfind("prelight_", 0) == 0 || w.rfind("sweat_", 0) == 0;
      (is_e2 ? e2_list : e_list) += (is_e2 ? e2_list : e_list).empty() ? w : "," + w;
    }
    if (!E2Faults::Parse(e2_list, e2faults, err)) { std::fprintf(stderr, "maps_draw: %s\n", err.c_str()); return 2; }
    fault_list = e_list;
  }
  if (!MapsFaults::Parse(fault_list, faults, err)) { std::fprintf(stderr, "maps_draw: %s\n", err.c_str()); return 2; }
  std::string path;
  SceneTables t;
  if (!ResolveRecord(cap, frame, path, err) || !ReadRecord(path, t, err)) { std::fprintf(stderr, "maps_draw: FAIL %s\n", err.c_str()); return 2; }
  SceneTables other;
  bool have_other = false;
  if (faults.circles_wrong_frame) {
    if (other_frame < 0) { std::fprintf(stderr, "maps_draw: circles_wrong_frame needs --other-frame M\n"); return 2; }
    if (!ResolveRecord(cap, other_frame, path, err) || !ReadRecord(path, other, err)) { std::fprintf(stderr, "maps_draw: FAIL %s\n", err.c_str()); return 2; }
    have_other = true;
  }
  if (circles_as_latch)
    for (CircleShadow& cs : t.circle_shadows) cs.flags |= 0x100u;   // S103 E4: A3's kCircleFromLatch (s103-a3 scene_record.h)
  if (t.header.source == uint8_t(Source::Live)) ConvertLiveToViewer(t);
  ViewerInit(game);

  // the record's wrestler assets, by record asset index
  std::vector<assets::Model> models(t.assets.size());
  std::vector<char> loaded(t.assets.size(), 0);
  uint32_t wrestler_assets = 0, load_failures = 0;
  for (size_t i = 0; i < t.assets.size(); ++i) {
    const AssetRef& a = t.assets[i];
    if (a.kind != uint8_t(AssetKind::Model) || a.pac == 0) continue;
    std::string norm(t.Str(a.pac));
    std::replace(norm.begin(), norm.end(), '/', '\\');
    int id = 0;
    if (std::sscanf(norm.c_str(), "pac\\ch\\ch%d.pac", &id) != 1) continue;
    ++wrestler_assets;
    std::string lerr;
    if (!ViewerLoad("wrestler", id, int(a.variant), models[i], lerr)) { std::fprintf(stderr, "maps_draw: asset %zu (ch%d variant %u): %s\n", i, id, a.variant, lerr.c_str()); ++load_failures; continue; }
    loaded[i] = 1;
  }

  // told characters (--wrestler): an object "actor:slot:N" gets the character's model as a new asset index
  std::map<int, size_t> model_of_char;
  std::map<uint32_t, size_t> asset_of_object;   // object index -> asset index in `models`
  for (size_t oi = 0; oi < t.objects.size(); ++oi) {
    int slot = -1;
    if (std::sscanf(std::string(t.Str(t.objects[oi].id)).c_str(), "actor:slot:%d", &slot) != 1) continue;
    const auto w = wrestler_of_slot.find(slot);
    if (w == wrestler_of_slot.end()) continue;
    const int key = w->second[0] * 1000 + w->second[1];
    auto it = model_of_char.find(key);
    if (it == model_of_char.end()) {
      models.emplace_back();
      loaded.push_back(0);
      std::string lerr;
      if (!ViewerLoad("wrestler", w->second[0], w->second[1], models.back(), lerr)) { std::fprintf(stderr, "maps_draw: --wrestler %d: %s\n", slot, lerr.c_str()); ++load_failures; continue; }
      loaded.back() = 1;
      ++wrestler_assets;
      it = model_of_char.emplace(key, models.size() - 1).first;
    }
    asset_of_object[uint32_t(oi)] = it->second;
  }

  SceneFrame f;
  f.width = 512, f.height = 512;
  f.viewer_space = true;
  f.bones = t.bones;
  // --bones-frame B: the game's maps at stream frame F are drawn with the poses of record F-1 (SF1 run 7147): the objects' world / palette / mask come from record B (matched by id), the rest from F
  SceneTables tb;
  std::map<std::string, size_t> by_id;
  if (bones_frame >= 0) {
    if (!ResolveRecord(cap, bones_frame, path, err) || !ReadRecord(path, tb, err)) { std::fprintf(stderr, "maps_draw: FAIL bones frame: %s\n", err.c_str()); return 2; }
    if (tb.header.source == uint8_t(Source::Live)) ConvertLiveToViewer(tb);
    for (size_t k = 0; k < tb.objects.size(); ++k) by_id[std::string(tb.Str(tb.objects[k].id))] = k;
    f.bones = tb.bones;
  }
  for (size_t oi = 0; oi < t.objects.size(); ++oi) {
    Object o = t.objects[oi];
    if (bones_frame >= 0) {
      const auto m = by_id.find(std::string(t.Str(o.id)));
      if (m == by_id.end()) continue;   // not in the pose record: left out (its palette index would be wrong)
      const Object& p = tb.objects[m->second];
      o.world = p.world, o.bones_first = p.bones_first, o.bones_count = p.bones_count, o.flags = p.flags, o.mesh_mask_lo = p.mesh_mask_lo, o.mesh_mask_hi = p.mesh_mask_hi;
    }
    const auto told = asset_of_object.find(uint32_t(oi));
    if (told != asset_of_object.end()) o.asset = uint32_t(told->second);
    if (o.asset < loaded.size() && loaded[o.asset]) f.objects.push_back(o);   // (the record's asset index is the provider's key)
  }

  MapsInput input;
  BuildMapsInput(t, faults, have_other ? &other : nullptr, input);
  if (props_debug && !input.circles.empty()) {
    const float* c = input.circles[0].view_proj;
    std::fprintf(stderr, "[props-debug] circle rows (c4-c7):");
    for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %.6g", c[k]);
    std::fprintf(stderr, "\n[props-debug] header guest frame %llu, circle stamp %u, light states %zu, refs %zu\n", (unsigned long long)t.header.guest_frame, input.circle_stamp, t.light_states.size(), t.light_refs.size());
    for (const LightRef& r : t.light_refs) std::fprintf(stderr, "[props-debug] ref object %u ordinal %u state %u frame %u\n", r.object, r.ordinal, r.state, r.frame);
    for (size_t i = 0; i < t.light_states.size(); ++i) {
      const LightState& st = t.light_states[i];
      double tv[16];
      props_map::StateProduct(st, tv);
      const CasterRows cr = BuildCasterRows(st);
      double err = 0, sc = 1e-9;
      for (int k = 0; k < 16; ++k) sc = std::max(sc, std::fabs(double(c[k])));
      for (int k = 0; k < 16; ++k) err = std::max(err, std::fabs(tv[k] - double(c[k])));
      std::fprintf(stderr, "[props-debug] state %zu flags %u slot %u: rel err %.3e; ViewProj row3 %.5g %.5g %.5g %.5g; trapezoid row0 %.5g %.5g %.5g %.5g\n", i, st.flags, st.slot, err / sc, cr.view_proj[3][0], cr.view_proj[3][1], cr.view_proj[3][2], cr.view_proj[3][3], cr.trapezoid[0][0], cr.trapezoid[0][1], cr.trapezoid[0][2], cr.trapezoid[0][3]);
    }
  }
  std::string eye_line;   // S103 E5
  if (eye_module) {   // the reader is the production one; only the memory behind it is a fixture
    e5::BuildBrockModule(0x40001000u);
    scene_inputs::g_eye_plant = eye_plant;
    PlaneEyes pe;
    uint32_t eye_stamp = uint32_t(t.header.guest_frame) - 1u;   // the rule of the plane frame: header - 1, else the newest at or below the header
    {
      bool has_exact = false;
      for (const PlaneFrame& pf : t.plane_frames) has_exact = has_exact || pf.frame == eye_stamp;
      if (!has_exact)
        for (const PlaneFrame& pf : t.plane_frames)
          if (int32_t(pf.frame - uint32_t(t.header.guest_frame)) <= 0 && (eye_stamp == uint32_t(t.header.guest_frame) - 1u || int32_t(pf.frame - eye_stamp) > 0)) eye_stamp = pf.frame;
    }
    const scene_inputs::EyeWhy ew = scene_inputs::ReadPlaneEyes(&e5::ReadGuest, eye_stamp, pe);   // the eyes carry the plane frame's stamp (BuildE2Input requires it)
    char b[512];
    std::snprintf(b, sizeof b, "plane eyes from module: why %u have 0x%x wrestler %.3f %.3f %.3e ring4 %.3f %.3f %.3f ring5 %.3f %.3f %.3f ring6 %.3f %.3f %.3f ring7 %.3f %.3f %.3f planepos %.1f\n", unsigned(ew), pe.have, pe.eye[1][0], pe.eye[1][1], pe.eye[1][2],
                  pe.eye[4][0], pe.eye[4][1], pe.eye[4][2], pe.eye[5][0], pe.eye[5][1], pe.eye[5][2], pe.eye[6][0], pe.eye[6][1], pe.eye[6][2], pe.eye[7][0], pe.eye[7][1], pe.eye[7][2], pe.plane_pos[1]);
    eye_line = b;
    if (ew == scene_inputs::EyeWhy::Ok) t.plane_eyes.push_back(pe);
  }
  BuildE2Input(t, e2faults, input.e2);   // S103 E2
  if (const_eye) {   // S103 E4 fixture
    std::memcpy(input.e2.plane_eye, kPlaneEyeWrestler, 16), input.e2.plane_eye_const = true;
    std::memcpy(input.e2.plane_pos, kPlanePosDefault, 16), input.e2.have_plane_pos = true, input.e2.plane_pos_const = true;
  }
  if (!told_path.empty() && !ApplyTold(told_path, input.e2)) { std::fprintf(stderr, "maps_draw: cannot read --told %s\n", told_path.c_str()); return 2; }

  rdr::RendererOptions ro;
  if (const char* v = std::getenv("WWE_RDR_VALIDATION")) ro.validation = std::atoi(v);
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::fprintf(stderr, "maps_draw: FAIL renderer: %s\n", err.c_str()); return 3; }
  int rc = 0;
  {
    LiveMapsHook hook(*renderer, [&](uint32_t asset) -> const assets::Model* { return asset < loaded.size() && loaded[asset] ? &models[asset] : nullptr; });
    hook.SetInput(&input);
    hook.SetFaults(faults);
    hook.SetBlockingLoads(true);   // S103 B2F: one frame
    hook.SetGameRoot(game);   // S103 B2F: the rope proxy's mesh is read from the arena pac
    hook.SetPlaneReader(true);   // S103 E4: the offline driver is the plane map's reader
    hook.SetE2Faults(e2faults);   // S103 E2
    rdr::FrameBuilder fb;
    fb.Begin(0);
    StageCtx ctx;
    ctx.frame = &f, ctx.renderer = renderer.get(), ctx.width = 512, ctx.height = 512;
    f.maps = &hook.maps();   // S103 E: set BEFORE the Maps stage runs, as the front-end sees it (the hook refills the same LiveMaps)
    f.stage_hooks.push_back(&hook);
    for (StageHook* h : f.stage_hooks) h->Record(Stage::Maps, ctx, fb);   // the front-end's own call (FrontEnd::Record run_stage)
    renderer->RenderFrame(fb.Finish());
    renderer->WaitIdle();
    std::fprintf(stderr, "[maps_draw] maps handed to the colour pass by role: %zu (shadow %d, circles %d, props %d)\n", f.maps->maps.size(), int(f.maps->Find(MapRole::ShadowMap) != nullptr), int(f.maps->Find(MapRole::Circles) != nullptr), int(f.maps->Find(MapRole::PropsMap) != nullptr));
    std::string text;
    char line[256];
    std::snprintf(line, sizeof line, "record %s frame %ld (guest %llu): objects %zu, wrestler assets %u (load failures %u)\n", path.c_str(), frame, (unsigned long long)input.guest_frame, f.objects.size(), wrestler_assets, load_failures);
    text += line;
    std::snprintf(line, sizeof line, "light: %s; map %ux%u; circles %zu (rejected by stamp %u, incomplete %u), circle mesh %d\n", input.have_light ? input.light_why : input.light_why, input.map_w, input.map_h, input.circles.size(), input.circles_rejected_stamp,
                  input.circles_incomplete, int(input.have_circle_mesh));
    text += line;
    text += hook.CensusLine() + "\n";
    text += eye_line;   // S103 E5
    std::snprintf(line, sizeof line, "validation layer active: %d\n", int(renderer->validation_active()));   // S103 E2 review: "0 errors" with the layer off is vacuous
    text += line;
    std::snprintf(line, sizeof line, "validation errors: %llu\n", (unsigned long long)renderer->validation_errors());
    text += line;
    std::vector<std::string> pl_names;   // S103 E2: the pre-light strips, one PNG per slot
    for (size_t k = 0; k < hook.e2().prelight_strips.size() && k < hook.e2().prelight_slots; ++k) pl_names.push_back("ours_prelight" + std::to_string(k) + ".png");
    struct Out { const char* name; rdr::ImageId id; };
    const rdr_scene::MapImage* props_mi = hook.maps().Find(rdr_scene::MapRole::PropsMap);   // S103 PMS: the props map as the floor gets it (the circle image + the floor's matrix / ShadowPrm)
    {
      std::string pt = std::string("props_present ") + (props_mi ? "1" : "0") + "\n";
      if (props_mi) {
        pt += "props_matrix";
        for (int k = 0; k < 16; ++k) { std::snprintf(line, sizeof line, " %.9g", props_mi->matrix[k]); pt += line; }
        pt += "\nprops_prm";
        for (int k = 0; k < 12; ++k) { std::snprintf(line, sizeof line, " %.9g", props_mi->prm[k / 4][k % 4]); pt += line; }
        std::snprintf(line, sizeof line, "\nprops_params %.9g %.9g raw %d\n", props_mi->params[0], props_mi->params[1], int(props_mi->raw));
        pt += line;
      }
      std::error_code pec;
      std::filesystem::create_directories(out_dir, pec);
      std::ofstream(out_dir + "/props.txt") << pt;
      text += pt;
    }
    std::vector<Out> outs = {{"ours_shadow.png", hook.shadow_image()}, {"ours_circles.png", hook.circles_image()}, {"ours_props.png", props_mi ? props_mi->image : rdr::kNoImage},
                                                        {"ours_rim.png", hook.e2().rim_color}, {"ours_plane.png", hook.e2().plane_color}, {"ours_sweat.png", hook.e2().sweat_color}};   // S103 E2
    for (size_t k = 0; k < pl_names.size(); ++k) outs.push_back({pl_names[k].c_str(), hook.e2().prelight_strips[k]});
    {  // S103 L7: the PUBLISHED shadow / rim maps (after the filter chains: raw = false), beside the raw images above
      const rdr_scene::MapImage* ms = hook.maps().Find(rdr_scene::MapRole::ShadowMap);
      const rdr_scene::MapImage* mr = hook.maps().Find(rdr_scene::MapRole::RimMap);
      if (ms) outs.push_back({ms->raw ? "ours_shadow_pub_raw.png" : "ours_shadow_f.png", ms->image});
      if (mr) outs.push_back({mr->raw ? "ours_rim_pub_raw.png" : "ours_rim_f.png", mr->image});
    }
    for (const auto& o : outs) {
      if (o.id == rdr::kNoImage) { text += std::string(o.name) + ": no map\n"; continue; }
      std::vector<uint8_t> rgba;
      uint32_t w = 0, h = 0;
      rdr::ImageRef ref;
      ref.image = o.id;
      if (!renderer->ReadImage(ref, rgba, w, h)) { text += std::string(o.name) + ": read back failed\n"; rc = 3; continue; }
      if (!WritePngRgb(out_dir + "/" + o.name, rgba, w, h)) { rc = 3; text += std::string(o.name) + ": cannot write\n"; }
      // a count the checks use: pixels that are not the clear colour
      uint64_t changed = 0;
      for (size_t k = 0; k + 3 < rgba.size(); k += 4) changed += (rgba[k] != 255 || rgba[k + 1] != 255 || rgba[k + 2] != 255);
      std::snprintf(line, sizeof line, "%s: %ux%u, %llu pixels differ from the clear\n", o.name, w, h, (unsigned long long)changed);
      text += line;
    }
    if (renderer->validation_errors()) rc = rc ? rc : 4;
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    std::ofstream(out_dir + "/counts.txt") << text;
    std::fputs(text.c_str(), stdout);
  }
  return rc;
}
