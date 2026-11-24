// crowd_live_rig (Session 103, lane D): the crowd drawn from a scene record ALONE by live_crowd (src\renderer_scene\live_crowd.*): no stream draw, no material, kept set or
// picture from a recorded draw. Offline, headless, no game. The camera is the capture's own (the world -> clip matrix of the frame's crowd draws, read once as a CAMERA, the
// way lane R's rig takes the camera of a capture), or an overview camera from the ring looking at the stands (--view overview).
//   crowd_live_rig --capture DIR --frame N --game DIR --out PICTURE.png [--size 1280x720] [--view capture|overview]
//                  [--plant palettes|cards_black|hidden|cards_back|atlas_offset|atlas_stale] [--hide-every K] [--make-floor K] [--only members|cards] [--card-atlas N]
// Prints "[crowd_live] ..." lines and one "RESULT" line. Exit 0 = drawn and the counts match the record; 2 = bad input; 3 = a failure; 4 = a count check failed.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include "../../src/assets/crowd_body.h"
#include "../../src/assets/view/loader_seam.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_adapter/capture_reader.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/live_crowd.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_crowd.h"

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

void Norm(float v[3]) {
  const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (l > 1e-9f) v[0] /= l, v[1] /= l, v[2] /= l;
}
// D3D style, left-handed, row vectors, depth 0..1, Y UP (the viewer's space). The game's matrix is then S * this (row 1 negated: the game's file space is -Y up).
void LookAtViewProj(const float eye[3], const float target[3], float fov_y_deg, float aspect, float zn, float zf, float out[16]) {
  float z[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
  Norm(z);
  const float up[3] = {0, 1, 0};
  float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
  Norm(x);
  const float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
  const float view[16] = {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
                          -(x[0] * eye[0] + x[1] * eye[1] + x[2] * eye[2]), -(y[0] * eye[0] + y[1] * eye[1] + y[2] * eye[2]), -(z[0] * eye[0] + z[1] * eye[1] + z[2] * eye[2]), 1};
  const float ys = 1.f / std::tan(fov_y_deg * 3.14159265f / 360.f), xs = ys / aspect;
  const float proj[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += view[i * 4 + k] * proj[k * 4 + j];
      out[i * 4 + j] = s;
    }
}

// The world -> clip matrix of the capture's own crowd draws (VS c56-c59: column k of the row-vector matrix), from the first crowd-class draw of stream frame N.
bool CaptureCamera(const std::string& capture, uint32_t frame, float vp[16], std::string& err) {
  rdr_adapter::CaptureReader cap;
  if (!cap.Open(capture)) return err = "cannot open the capture", false;
  rdr_adapter::LoadedFrame L;
  if (!cap.LoadFrame(frame, L)) return err = "cannot load stream frame " + std::to_string(frame), false;
  for (uint32_t d = 0; d < L.draws.count; ++d) {
    const rdr_adapter::DrawOrigin* o = L.OriginOf(d);
    if (!o || o->klass != rdr_adapter::DrawClass::Crowd) continue;
    const auto& dr = L.draws[d];
    if (dr.vs_bank >= L.constant_banks.count) continue;
    const uint32_t* c = L.constant_banks[dr.vs_bank].dwords;
    for (int k = 0; k < 4; ++k)
      for (int i = 0; i < 4; ++i) std::memcpy(&vp[i * 4 + k], &c[(56 + k) * 4 + i], 4);
    return true;
  }
  return err = "stream frame " + std::to_string(frame) + " has no crowd draw to read the camera from", false;
}

}  // namespace


// S103 CB1: the rig has no applier, so the crowd's light sets are the file's. For the key-coverage arm (+light) a rig lights function scales the used set by g_rig_gain (the light gain changing between two draws of one frame).
static float g_rig_gain = 1.f;
static bool RigLights(const SceneTables&, std::vector<assets::LightSet>& sets, int used) {
  if (used < 0 || size_t(used) >= sets.size()) return false;
  assets::LightSet& ls = sets[size_t(used)];
  for (auto& row : ls.col) for (float& v : row) v *= g_rig_gain;
  for (float& v : ls.sky) v *= g_rig_gain;
  for (float& v : ls.ground) v *= g_rig_gain;
  return true;
}
static bool RigLightsHas(const SceneTables&, int) { return true; }

// The record world as the three c76-c78 rows (row r = column r of the row-vector matrix).
static void WorldRowsOf(const Mat4& w, float* out) {
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 4; ++k) out[4 * r + k] = w.m[k * 4 + r];
}

int main(int argc, char** argv) {
  std::string capture, game, out, view = "capture", plant, only, frames_arg;
  uint32_t w = 1280, h = 720, hide_every = 0, make_floor = 0, strip_palettes = 0;
  int card_atlas = 0;   // S103 CB1: 1 one pass, 2 keep unchanged pictures (live_crowd.h LiveCrowdOptions::card_atlas)
  std::string dump_cards;   // S103 CB1: --dump-cards PREFIX: PREFIX_fN.bin per frame (card pictures, byte for byte)
  bool collapse_runs = false, use_async = false;
  long frame = -1;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--capture" && i + 1 < argc) capture = argv[++i];
    else if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--out" && i + 1 < argc) out = argv[++i];
    else if (a == "--frame" && i + 1 < argc) frame = std::atol(argv[++i]);
    else if (a == "--frames" && i + 1 < argc) frames_arg = argv[++i];   // several frames in ONE process (the cross-frame state: re-bake barriers, scratch reuse)
    else if (a == "--view" && i + 1 < argc) view = argv[++i];
    else if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "--only" && i + 1 < argc) only = argv[++i];
    else if (a == "--make-floor" && i + 1 < argc) make_floor = uint32_t(std::atoi(argv[++i]));
    else if (a == "--strip-palettes" && i + 1 < argc) strip_palettes = uint32_t(std::atoi(argv[++i]));
    else if (a == "--card-atlas" && i + 1 < argc) card_atlas = std::atoi(argv[++i]);
    else if (a == "--dump-cards" && i + 1 < argc) dump_cards = argv[++i];
    else if (a == "--collapse-runs") collapse_runs = true;   // the record's members become run markers (S102 T19): read straight from the runs
    else if (a == "--async") use_async = true;                // bodies load on a worker, 2 classes adopted a frame: the frame repeats until nothing is missed
    else if (a == "--hide-every" && i + 1 < argc) hide_every = uint32_t(std::atoi(argv[++i]));
    else if (a == "--size" && i + 1 < argc) { if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2) { std::fprintf(stderr, "bad --size\n"); return 1; } }
  }
  std::vector<long> frames;
  std::vector<std::string> mods;   // S103 CB1: per frame token: an optional 'd' (record and DISCARD: never rendered) and '+light' '+page' '+palette' '+dropN' edits of this draw's inputs, e.g. --frames 200,200+palette,d200,200
  if (!frames_arg.empty()) {
    size_t at = 0;
    while (at < frames_arg.size()) { size_t c = frames_arg.find(',', at); if (c == std::string::npos) c = frames_arg.size(); const std::string tk = frames_arg.substr(at, c - at); mods.push_back(tk); frames.push_back(std::atol(tk[0] == 'd' ? tk.c_str() + 1 : tk.c_str())); at = c + 1; }
  } else if (frame >= 0) frames.push_back(frame), mods.push_back(std::string());
  if (capture.empty() || game.empty() || out.empty() || frames.empty()) {
    std::fprintf(stderr, "usage: crowd_live_rig --capture DIR --frame N|--frames A,B,C --game DIR --out PICTURE.png [--size WxH] [--view capture|overview] [--plant NAME] [--hide-every K]\n"
                         "       [--make-floor K] [--strip-palettes K] [--collapse-runs] [--async] [--only members|cards]\n");
    return 1;
  }
  ViewerInit(game);
  assets::crowd_body::Init(game);
  rdr::RendererOptions ro;
  if (const char* v = std::getenv("WWE_RDR_VALIDATION")) ro.validation = std::atoi(v);
  std::string err;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::fprintf(stderr, "crowd_live: FAIL renderer: %s\n", err.c_str()); return 3; }
  int rc = 0;
  {
    FrontEnd fe(*renderer);
    live::LiveCrowdOptions lo;
    lo.fault_no_palettes = plant == "palettes";
    lo.fault_cards_black = plant == "cards_black";
    lo.fault_ignore_hidden = plant == "hidden";
    lo.fault_cards_back = plant == "cards_back";
    lo.fault_inst_first_world = plant == "inst_first";
    lo.card_atlas = card_atlas, lo.fault_atlas_offset = plant == "atlas_offset", lo.fault_atlas_stale = plant == "atlas_stale";   // S103 CB1
    lo.no_cards = only == "members";
    lo.no_members = only == "cards";
    for (const std::string& m : mods) if (m.find("+light") != std::string::npos) live::SetLightsFn(&RigLights, &RigLightsHas);   // S103 CB1
    live::AsyncBodies async(live::GameFilesBodies());
    if (use_async) lo.adopt_per_frame = 2;
    live::LiveCrowd crowd(*renderer, fe, use_async ? static_cast<live::BodySource&>(async) : live::GameFilesBodies(), lo);
    for (size_t fi = 0; fi < frames.size(); ++fi) {
      const long fr = frames[fi];
      const std::string& md = mods[fi];
      char name[32];
      std::snprintf(name, sizeof name, "frame_%06ld.rsc", fr);
      SceneTables t;
      const std::string rec = (std::filesystem::path(capture) / "scene" / name).string();
      if (!ReadRecord(rec, t, err)) { std::fprintf(stderr, "crowd_live: FAIL bad record %s: %s\n", rec.c_str(), err.c_str()); return 2; }
      float vp[16];
      if (view == "overview") {
        double cx = 0, cy = 0, cz = 0;
        uint32_t n = 0;
        for (const Object& o : t.objects)
          if (o.klass == rdr_adapter::DrawClass::Crowd) cx += o.world.m[12], cy += o.world.m[13], cz += o.world.m[14], ++n;
        if (!n) { std::fprintf(stderr, "crowd_live: FAIL the record has no crowd objects\n"); return 2; }
        cx /= n, cy /= n, cz /= n;
        const float eye_view[3] = {0.f, 450.f, 0.f}, target_view[3] = {float(cx), float(-cy), float(cz)};
        LookAtViewProj(eye_view, target_view, 28.f, float(w) / float(h), 10.f, 60000.f, vp);
        for (int k = 4; k < 8; ++k) vp[k] = -vp[k];
      } else if (!CaptureCamera(capture, uint32_t(fr), vp, err)) {
        std::fprintf(stderr, "crowd_live: FAIL camera: %s\n", err.c_str());
        return 2;
      }
      // Record edits that stand in for what the live writer does (each proves one path): --make-floor, --strip-palettes, --hide-every.
      uint32_t made = 0, stripped = 0;
      for (Object& o : t.objects) {
        if (o.klass != rdr_adapter::DrawClass::Crowd || t.Str(o.id).compare(0, 11, "crowd:card:") == 0) continue;
        if (made < make_floor) o.id = t.AddString("crowd:floor:00:member:" + std::to_string(made++));
        else if (stripped < strip_palettes) o.flags &= ~uint32_t(kObjSkinned), ++stripped;
      }
      g_rig_gain = 1.f;
      if (md.find("+light") != std::string::npos) {   // S103 CB1: only the light gain changes between two draws of one frame (the guest frame moves so the crowd's per-frame light cache is not served)
        g_rig_gain = 0.5f;
        t.header.guest_frame += 1000;
      }
      if (md.find("+palette") != std::string::npos)   // only one palette: the first card's second bone moves
        for (const Object& o : t.objects)
          if (o.klass == rdr_adapter::DrawClass::Crowd && t.Str(o.id).compare(0, 11, "crowd:card:") == 0 && o.bones_count > 1 && size_t(o.bones_first) + 1 < t.bones.size()) { t.bones[size_t(o.bones_first) + 1].m[12] += 7.f; break; }
      if (const size_t dp = md.find("+drop"); dp != std::string::npos) {   // the cards of one type leave this frame
        const std::string want = "card:" + std::to_string(std::atoi(md.c_str() + dp + 5));
        for (Object& o : t.objects)
          if (o.klass == rdr_adapter::DrawClass::Crowd && o.asset < t.assets.size() && t.Str(t.assets[o.asset].entry) == want) o.flags &= ~uint32_t(kObjVisible);
      }
      if (md.find("+page") != std::string::npos) {   // only the stage page changes (a new key, other pixels)
        std::vector<uint8_t> px(512 * 512 * 4);
        for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t((i * 37 + fi * 11) & 255);
        crowd.SetStagePage(0x777 + fi, 512, 512, px.data());
      }
      uint32_t expect_members = 0, expect_cards = 0, expect_floor = 0, hidden = 0, k_obj = 0;
      for (Object& o : t.objects) {
        if (o.klass != rdr_adapter::DrawClass::Crowd) continue;
        if (hide_every && (++k_obj % hide_every) == 0) o.flags &= ~uint32_t(kObjVisible);   // the game's cull, as the record's own flag
        if (!(o.flags & kObjVisible)) { ++hidden; continue; }
        const std::string_view id = t.Str(o.id);
        if (id.compare(0, 11, "crowd:card:") == 0) ++expect_cards;
        else ++expect_members, expect_floor += id.compare(0, 12, "crowd:floor:") == 0;
      }
      if (only == "members") expect_cards = 0;
      if (only == "cards") expect_members = expect_floor = 0;
      if (collapse_runs) {   // runs of consecutive visible crowd objects with one asset and one palette become CrowdRun markers; their objects leave the table
        std::vector<Object> keep;
        for (const Object& o : t.objects) {
          const bool crowd = o.klass == rdr_adapter::DrawClass::Crowd && (o.flags & kObjVisible);
          if (!crowd) { keep.push_back(o); continue; }
          const std::string_view id = t.Str(o.id);
          const uint8_t kind = id.compare(0, 11, "crowd:card:") == 0 ? kRunCard : id.compare(0, 12, "crowd:floor:") == 0 ? kRunFloor : kRunBlock;
          const bool skinned = (o.flags & kObjSkinned) != 0;
          if (t.crowd_runs.empty() || t.crowd_runs.back().asset != o.asset || t.crowd_runs.back().bones_first != o.bones_first || t.crowd_runs.back().kind != kind || t.crowd_runs.back().skinned != (skinned ? 1 : 0)) {
            CrowdRun r;
            r.at = uint32_t(keep.size()), r.first_member = uint32_t(t.crowd_members.size()), r.count = 0, r.asset = o.asset, r.bones_first = o.bones_first, r.bones_count = o.bones_count;
            r.kind = kind, r.skinned = skinned ? 1 : 0;
            t.crowd_runs.push_back(r);
          }
          CrowdMember m;
          WorldRowsOf(o.world, m.c);
          m.index = kCrowdNoIndex;
          t.crowd_members.push_back(m);
          ++t.crowd_runs.back().count;
        }
        t.objects = std::move(keep);
      }
      crowd.SetRecord(&t);
      SceneFrame f;
      std::memcpy(f.camera.view.m, vp, sizeof vp);
      f.camera.proj = IdentityMat4();
      f.camera.flags = kCamViewIsViewProj;
      f.width = w, f.height = h;
      f.viewer_space = false;
      f.clear[0] = 0.10f, f.clear[1] = 0.10f, f.clear[2] = 0.13f, f.clear[3] = 1.f;
      f.objects = t.objects;   // the live frame carries the record's objects too: the generic loop must not draw the crowd (or a hook-owned model) a second time
      f.stage_hooks.push_back(&crowd);
      if (!md.empty() && md[0] == 'd') {   // S103 CB1: recorded (the hooks ran) and thrown away before RenderFrame; --plant drop_unnoted = the host that forgets to tell the renderer
        if (!fe.RecordAndDiscard(f, plant != "drop_unnoted", err)) { std::fprintf(stderr, "crowd_live: FAIL discard record: %s\n", err.c_str()); return 3; }
        std::fprintf(stderr, "[crowd_live] frame %ld: recorded and discarded%s\n", fr, plant == "drop_unnoted" ? " (renderer not told)" : "");
        continue;
      }
      std::vector<uint8_t> rgba;
      uint32_t repeats = 0;
      for (;; ++repeats) {
        if (!fe.RenderToPixels(f, rgba, err)) { std::fprintf(stderr, "crowd_live: FAIL draw: %s\n", err.c_str()); return 3; }
        if (!use_async || crowd.frame_complete() || repeats >= 600) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      std::string path = out;
      if (frames.size() > 1) { const size_t dot = out.rfind('.'); path = out.substr(0, dot) + "_f" + std::to_string(fr) + out.substr(dot); }
      if (!WritePngRgb(path, rgba, w, h)) { std::fprintf(stderr, "crowd_live: FAIL cannot write %s\n", path.c_str()); return 3; }
      if (!dump_cards.empty()) {   // S103 CB1: every card picture the frame baked: per type 1 byte (baked) + 64 x 64 RGBA8 + the gutter row above / below (zeros when the pictures are per type)
        std::string dp = dump_cards + "_f" + std::to_string(fr) + ".bin";
        std::ofstream df(dp, std::ios::binary | std::ios::trunc);
        for (uint32_t ty = 0; ty < 20; ++ty) {
          std::vector<uint8_t> tile, gt, gb;
          const uint8_t present = crowd.ReadCardPicture(ty, tile, gt, gb) ? 1 : 0;
          tile.resize(64 * 64 * 4), gt.resize(64 * 4), gb.resize(64 * 4);
          if (!present) std::fill(tile.begin(), tile.end(), uint8_t(0)), std::fill(gt.begin(), gt.end(), uint8_t(0)), std::fill(gb.begin(), gb.end(), uint8_t(0));
          df.write(reinterpret_cast<const char*>(&present), 1);
          df.write(reinterpret_cast<const char*>(tile.data()), std::streamsize(tile.size()));
          df.write(reinterpret_cast<const char*>(gt.data()), std::streamsize(gt.size()));
          df.write(reinterpret_cast<const char*>(gb.data()), std::streamsize(gb.size()));
        }
      }
      const live::LiveCrowdCounts& c = crowd.counts();
      std::fprintf(stderr, "[crowd_live] frame %ld: %s\n%s", fr, c.Text().c_str(), fe.CoverageText().c_str());
      uint64_t lit = 0;
      constexpr int kGx = 16, kGy = 9;
      uint32_t cell_lit[kGx * kGy] = {};
      for (size_t i = 0; i < rgba.size(); i += 4) {
        const bool l = (std::abs(int(rgba[i]) - int(0.10f * 255 + 0.5f)) + std::abs(int(rgba[i + 1]) - int(0.10f * 255 + 0.5f)) + std::abs(int(rgba[i + 2]) - int(0.13f * 255 + 0.5f))) > 12;
        lit += l;
        if (l) { const size_t px = i / 4; ++cell_lit[(px / w * kGy / h) * kGx + (px % w) * kGx / w]; }
      }
      uint32_t cells = 0;   // grid cells of the picture holding a real share of the crowd: a crowd collapsed onto a few spots fills few
      for (uint32_t v : cell_lit) cells += v >= (w * h) / 5000;
      const uint64_t errs = fe.validation_errors();
      bool ok = true;
      const auto check = [&](bool pass, const char* what) { std::fprintf(stderr, "[crowd_live] check %s: %s\n", pass ? "PASS" : "FAIL", what); ok = ok && pass; };
      check(errs == 0, "validation errors 0");
      check(c.members_drawn == expect_members, "members drawn == visible member objects of the record");
      check(c.cards_drawn == expect_cards, "cards drawn == visible card objects of the record");
      check(c.floor_drawn == expect_floor, "floor people drawn == visible floor objects of the record");
      check(c.refused == 0, "no mesh refused");
      check(c.draws <= 400, "instanced draws a frame <= 400 (tens, not thousands)");
      check(c.hidden_listed == hidden || collapse_runs, "hidden-flag objects counted");
      check(c.misses == 0, "no class missing (the frame is whole)");
      check(c.hard_failures == 0, "no body failed to load");
      check(c.bad_bones == 0, "no vertex bone reference past a palette");
      if (plant != "palettes") check(c.stored_pose == (only == "cards" ? 0 : stripped), "members without a palette are drawn at the stored pose and counted");
      if (collapse_runs) check(c.run_members == expect_members + expect_cards, "run members read straight from the record's runs");
      std::printf("RESULT %s members %u/%u cards %u/%u floor %u/%u draws %u classes %u bakes %u hidden %u stored %u cells %u validation %llu lit_pixels %llu repeats %u bake_passes %u same_inputs %u\n", ok ? "PASS" : "FAIL",
                  c.members_drawn, expect_members, c.cards_drawn, expect_cards, c.floor_drawn, expect_floor, c.draws, c.classes_drawn, c.bakes, c.hidden_listed, c.stored_pose, cells,
                  (unsigned long long)errs, (unsigned long long)lit, repeats, c.bake_passes, c.bakes_same_inputs);
      std::printf("wrote %s (%ux%u)\n", path.c_str(), w, h);
      if (!ok) rc = 4;
    }
    renderer->WaitIdle();
  }
  return rc;
}
