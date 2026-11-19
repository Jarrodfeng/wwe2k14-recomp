// rf_b_draw: lane B's (Session 103) own driver for the wrestlers applier, used until lane R's rig (tools\scene\frame_rig) is on the session branch.
//   rf_b_draw CAPTURE_FOLDER --frame N --game DIR --out PICTURE.png [--size 1280x720] [--cam FILE] [--link ID=CHAR:ATTIRE] [--light-id N] [--mask LOHEX,HIHEX]
//             [--plant palette|light|mask] [--arena] [--green]
// The live records of the older captures carry no camera and (for the main wrestler) no model link, so this TEST driver patches the record in memory and says so on stderr:
//   --cam FILE   16 floats of the game's ViewProj constant (vertex c56..c59, the four registers as uploaded, column-vector form) + the eye, read from the capture's own
//                character draw by B_camgrab.py (test only; lane A's inputs carry the camera in the real chain)
//   --link ID=CHAR:ATTIRE  names the model of the object ID ("actor:slot:0=343:0": pac\ch\ch343.pac, variant 0): the declared link the live contributor misses for that skeleton
//   --light-id N / --mask LO,HI  set every wrestler object's light set id / mesh mask (the planted-fault checks need a record that says something)
// Draws ONLY what the wrestlers applier (src\renderer_scene\rf_wrestlers.cpp) builds, plus (--arena) the arena of the record's slot objects as the background.
#include <algorithm>
#include <array>
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
#include "../../src/renderer_scene/live_assets.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/body_scale.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/record_frame.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rf_wrestlers.h"
#include "../../src/scene_live/arena_apply.h"

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
    for (uint32_t x = 0; x < w; ++x) { const uint8_t* p = &rgba[(size_t(y) * w + x) * 4]; raw.push_back(p[0]), raw.push_back(p[1]), raw.push_back(p[2]); }
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


bool ReadCam(const std::string& path, SceneTables& t) {
  std::ifstream f(path);
  if (!f) return false;
  float reg[16], eye[3];
  for (float& x : reg) f >> x;
  for (float& x : eye) f >> x;
  if (!f) return false;
  // column-vector M (row i = register i) -> the row-vector form: m[j * 4 + i] = reg[i * 4 + j]
  Mat4 vp;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) vp.m[j * 4 + i] = reg[i * 4 + j];
  t.camera.view = vp;
  t.camera.proj = IdentityMat4();
  t.camera.flags = kCamViewIsViewProj;
  t.camera.eye[0] = eye[0], t.camera.eye[1] = eye[1], t.camera.eye[2] = eye[2];
  t.has_camera = true;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string rec, game, out, cam, link, plant, body_scale;
  uint32_t w = 1280, h = 720;
  long frame = -1;
  long light_id = -1;
  bool arena = false, green = false, has_mask = false;
  uint32_t mask_lo = 0xFFFFFFFFu, mask_hi = 0xFFFFFFFFu;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--frame" && i + 1 < argc) frame = std::atol(argv[++i]);
    else if (a == "--cam" && i + 1 < argc) cam = argv[++i];
    else if (a == "--link" && i + 1 < argc) link = argv[++i];
    else if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "--body-scale" && i + 1 < argc) body_scale = argv[++i];   // S103 RGF: ID=S[,ID=S] the TOLD body scale of a record object (W2: an unknown scale is a hole, never drawn unscaled)
    else if (a == "--light-id" && i + 1 < argc) light_id = std::atol(argv[++i]);
    else if (a == "--mask" && i + 1 < argc) { has_mask = true; std::sscanf(argv[++i], "%x,%x", &mask_lo, &mask_hi); }
    else if (a == "--size" && i + 1 < argc) { if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2) { std::fprintf(stderr, "bad --size\n"); return 1; } }
    else if (a == "--arena") arena = true;
    else if (a == "--green") green = true;
    else if (rec.empty()) rec = a;
  }
  if (rec.empty() || game.empty() || out.empty() || frame < 0) { std::fprintf(stderr, "usage: rf_b_draw CAPTURE --frame N --game DIR --out PNG [...]\n"); return 1; }
  char name[32];
  std::snprintf(name, sizeof name, "frame_%06ld.rsc", frame);
  const std::string path = (std::filesystem::path(rec) / "scene" / name).string();
  SceneTables t;
  std::string err;
  if (!ReadRecord(path, t, err)) { std::fprintf(stderr, "rf_b_draw: FAIL bad record: %s\n", err.c_str()); return 2; }
  if (!t.has_camera) {
    if (cam.empty() || !ReadCam(cam, t)) { std::fprintf(stderr, "rf_b_draw: FAIL the record has no camera and --cam FILE was not given / unreadable\n"); return 2; }
    std::fprintf(stderr, "[rf_b_draw] TEST PATCH: camera from %s (the record has none)\n", cam.c_str());
  }
  // TEST PATCHES of the record's wrestler objects (listed)
  if (!link.empty()) {
    const size_t eq = link.find('=');
    int ch = 0, at = 0;
    if (eq == std::string::npos || std::sscanf(link.c_str() + eq + 1, "%d:%d", &ch, &at) != 2) { std::fprintf(stderr, "bad --link\n"); return 1; }
    AssetRef a;
    a.kind = uint8_t(AssetKind::Model);
    char pac[32];
    std::snprintf(pac, sizeof pac, "pac\\ch\\ch%03d.pac", ch);
    a.pac = t.AddString(pac);
    a.entry = t.AddString("model_key:" + std::to_string(ch * 100 + at * 10 + 2));  // the ring model of the attire (kind 2); the loader falls back to it anyway
    a.variant = uint32_t(at);
    const uint32_t ai = uint32_t(t.assets.size());
    t.assets.push_back(a);
    int n = 0;
    for (Object& o : t.objects)
      if (std::string(t.Str(o.id)) == link.substr(0, eq)) o.asset = ai, ++n;
    std::fprintf(stderr, "[rf_b_draw] TEST PATCH: %s -> %s variant %d (%d object(s))\n", link.substr(0, eq).c_str(), pac, at, n);
  }
  // S103 RGF: the offline rig has no game stream, so the body scale a live WrestlerCore would measure is TOLD, the way frame_rig.cpp's ParseBodyScales takes it (record object id -> scale, BodyScaleTable::SetByName).
  // Without it W2's rule (rf_wrestlers.cpp: an unknown scale is a hole) draws no wrestler at all. The value is labelled TOLD [U] in the report (rf_b_check.py takes it from frame_rig.py's DEFAULT_SCALES).
  rdr_scene::BodyScaleTable::Get().Clear();
  {
    size_t at = 0;
    while (at < body_scale.size()) {
      size_t end = body_scale.find(',', at);
      if (end == std::string::npos) end = body_scale.size();
      const std::string item = body_scale.substr(at, end - at);
      const size_t eq = item.find('=');
      if (eq == std::string::npos) { std::fprintf(stderr, "bad --body-scale\n"); return 1; }
      const float sv = std::strtof(item.c_str() + eq + 1, nullptr);
      rdr_scene::BodyScaleTable::Get().SetByName(item.substr(0, eq), sv);
      std::fprintf(stderr, "[rf_b_draw] TEST PATCH: body scale %s = %.7f (TOLD [U])\n", item.substr(0, eq).c_str(), double(sv));
      at = end + 1;
    }
  }
  for (Object& o : t.objects) {
    if (!rf::IsWrestlerObject(t, o)) continue;
    if (light_id >= 0) o.light_set = uint32_t(light_id);
    if (has_mask) o.mesh_mask_lo = mask_lo, o.mesh_mask_hi = mask_hi;
  }
  if (light_id >= 0) std::fprintf(stderr, "[rf_b_draw] TEST PATCH: wrestler light set id %ld\n", light_id);
  if (has_mask) std::fprintf(stderr, "[rf_b_draw] TEST PATCH: wrestler mesh mask %08X,%08X\n", mask_lo, mask_hi);
  {
    std::map<uint32_t, int> ids;
    for (const Light& l : t.lights) ++ids[l.set];
    std::string s;
    for (const auto& kv : ids) s += " " + std::to_string(kv.first);
    std::fprintf(stderr, "[rf_b_draw] record light sets:%s\n", s.c_str());
    if (std::getenv("RF_B_LIST_SETS"))
      for (const Light& l : t.lights)
        if (l.kind == uint8_t(LightKind::Directional) && l.slot < 5) std::fprintf(stderr, "  set %u slot %u dir %.3f %.3f %.3f col %.4f %.4f %.4f\n", l.set, l.slot, l.dir_or_pos[0], l.dir_or_pos[1], l.dir_or_pos[2], l.color[0], l.color[1], l.color[2]);
  }
  if (plant == "palette") rf::Plants().no_palette = true;
  else if (plant == "light") rf::Plants().ignore_light = true;
  else if (plant == "mask") rf::Plants().ignore_mask = true;
  else if (!plant.empty()) { std::fprintf(stderr, "bad --plant\n"); return 1; }
  if (!plant.empty()) std::fprintf(stderr, "[rf_b_draw] PLANTED FAULT: %s\n", plant.c_str());

  ViewerInit(game);  // (the arena below still loads through the viewer seam)
  rdr::RendererOptions ro;
  if (const char* v = std::getenv("WWE_RDR_VALIDATION")) ro.validation = std::atoi(v);
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::fprintf(stderr, "rf_b_draw: FAIL renderer: %s\n", err.c_str()); return 3; }
  int rc = 0;
  {
    FrontEndOptions fo;
    FrontEnd fe(*renderer, fo);
    GameFilesSource source;  // the code the game exe uses (live_assets.cpp): model_key entries resolve by the full key
    if (!source.SetRoot(game)) { std::fprintf(stderr, "rf_b_draw: FAIL no game files at %s\n", game.c_str()); return 3; }
    rf::ModelCache models(fe, source, true);
    SceneFrame f;
    f.camera = t.camera;
    f.width = w, f.height = h;
    f.time_s = t.header.time_s;
    if (t.header.source == uint8_t(Source::Live)) {  // the one space rule (rsc_draw ConvertLiveToViewer): the camera takes S on its world side, the eye's y negates
      for (int k = 4; k < 8; ++k) f.camera.view.m[k] = -f.camera.view.m[k];
      f.camera.eye[1] = -f.camera.eye[1];
      f.viewer_space = true;
    }
    if (green) f.clear[0] = 0, f.clear[1] = 1, f.clear[2] = 0, f.clear[3] = 1;
    // the arena of the record's slot objects as the background (rsc_draw's way)
    if (arena) {
      live::arena::RecordView view;
      std::string aerr;
      if (!live::arena::ReadArenaRecord(t, view, aerr)) { std::fprintf(stderr, "rf_b_draw: FAIL arena part: %s\n", aerr.c_str()); return 2; }
      assets::Model am;
      if (!ViewerLoad("arena", view.arena_id, 0, am, err)) { std::fprintf(stderr, "rf_b_draw: FAIL arena: %s\n", err.c_str()); return 3; }
      live::arena::ApplyToModel(view, am);
      std::vector<std::string> warnings;
      Object c;
      c.asset = uint32_t(fe.AddModel(am, warnings));
      c.world = IdentityMat4();
      f.objects.push_back(c);
    }
    std::vector<rf::ClassReport> reports;
    rf::BuildCtx ctx{t, models, f, rf::kAllFamilies, reports};
    if (!rf::ApplyWrestlers(ctx)) { std::fprintf(stderr, "rf_b_draw: FAIL the applier refused the frame\n"); return 3; }
    std::fprintf(stderr, "%s\n", rf::WrestlerStatsText().c_str());
    std::vector<uint8_t> rgba;
    if (!fe.RenderToPixels(f, rgba, err)) { std::fprintf(stderr, "rf_b_draw: FAIL draw: %s\n", err.c_str()); rc = 3; }
    else if (!WritePngRgb(out, rgba, w, h)) { std::fprintf(stderr, "rf_b_draw: FAIL cannot write %s\n", out.c_str()); rc = 3; }
    else {
      std::fprintf(stderr, "[rf_b_draw] %zu objects drawn, validation errors: %llu\n%s", f.objects.size(), (unsigned long long)fe.validation_errors(), fe.CoverageText().c_str());
      std::printf("wrote %s (%ux%u)\n", out.c_str(), w, h);
    }
    renderer->WaitIdle();
  }
  return rc;
}
