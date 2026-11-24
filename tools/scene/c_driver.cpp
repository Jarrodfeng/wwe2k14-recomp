// body-scale: none (draws only the arena / ring / props / screens / flashes families (kMine), no wrestler)  [S103 RGF: body_scale_feed_lint.py]
// Lane C's own offline driver (Session 103): draws the arena / ring / props / screens / flashes classes (src\renderer_scene\rf_*.cpp, live_flashes.cpp) from a captured scene record,
// per family and whole, with the record's own light sets, and with synthetic stage rows (moving slots, screen pictures, flashes) where no capture carries them yet. Written because
// lane R's rig and record_frame.cpp were not merged when this lane was built: the three ModelCache members and the AssetSource below are the driver's own small versions
// (viewer loader seam), to be dropped for R's when it lands. 0 game runs: files only.
//   c_driver --game DIR --capture FOLDER --frame N --out OUTDIR [--size 960x540] [--tag NAME]
// Writes OUTDIR\<tag>_<panel>.png for each panel and prints one line per panel: "[c] panel NAME crc XXXXXXXX nonbg N report ...".
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
#include <set>
#include <string>
#include <vector>

#include "../../src/assets/load_plants.h"   // S103 RLB2: load_detail::RingLowKeys (the live arena load sets it; the driver mirrors it)
#include "../../src/assets/view/camera.h"
#include "../../src/assets/view/loader_seam.h"
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/rf_c.h"
#include "../../src/renderer_scene/arena_debounce.h"
#include "../../src/renderer_scene/rf_crowd.h"
#include "../../src/scene_live/arena_apply.h"
#include "../../src/renderer_scene/record_frame.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_ring.h"
#include "../../src/renderer_scene/rows_stage_fx.h"
#include "../../src/scene_live/flash_fx_core.h"   // S103 C3: the flash rows the LIVE writer makes (a fake guest below)

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

// rsc_draw's one space rule (SCENE_RECORD "One space rule for Live records"): a Live record is converted once to the viewer's space.
void ConvertLiveToViewer(SceneTables& t) {
  auto mirror = [](Mat4& m) {
    assets::Matrix a;
    std::memcpy(a.data(), m.m, sizeof(float) * 16);
    a = assets::MirrorY(a);
    std::memcpy(m.m, a.data(), sizeof(float) * 16);
  };
  for (int k = 4; k < 8; ++k) t.camera.view.m[k] = -t.camera.view.m[k];
  t.camera.eye[1] = -t.camera.eye[1];
  for (Object& o : t.objects) mirror(o.world);
  for (Mat4& m : t.bones) mirror(m);
  t.header.source = uint8_t(Source::Viewer);
}

// The driver's own asset source: the viewer's loader seam (arena pacs by id, event props by entry).
class SeamSource : public rf::AssetSource {
 public:
  bool Load(const std::string& pac, const std::string& entry, uint32_t variant, assets::Model& out, std::string& err) override {
    std::string norm = pac;
    std::replace(norm.begin(), norm.end(), '/', '\\');
    int id = 0;
    if (std::sscanf(norm.c_str(), "pac\\bg\\bg%d.pac", &id) == 1) {
      // S103 RLB2: the live arena load (live_assets.cpp) loads the low ring pieces 980 / 981 / 982: the driver does the same; WWE_C_RING_LOW=0 is the rig-only planted twin (the switch forced off: no 980)
      const char* low = std::getenv("WWE_C_RING_LOW");
      const bool saved = assets::load_detail::RingLowKeys();
      assets::load_detail::RingLowKeys() = !(low && std::string(low) == "0");
      const bool ok = ViewerLoad("arena", id, 0, out, err);
      assets::load_detail::RingLowKeys() = saved;
      return ok;
    }
    if (std::sscanf(norm.c_str(), "pac\\ch\\ch%d.pac", &id) == 1) return ViewerLoad("wrestler", id, int(variant), out, err);
    if (norm == "pac\\evt\\evtobj.pac" && std::sscanf(entry.c_str(), "%d", &id) == 1) return ViewerLoad("prop", id, 0, out, err);
    err = "no loader for pac \"" + pac + "\" entry \"" + entry + "\"";
    return false;
  }
};

// A screen feed that makes a picture per movie: a gradient in a colour of the movie's index, moving with the serial.
class TestFeed : public rf::c::ScreenFeed {
 public:
  bool Picture(uint32_t key, uint32_t seq, uint32_t& w, uint32_t& h, const uint8_t*& rgba, uint64_t& content_id) override {   // S103 C2: keyed by the screen key (800 + the old movie index: the colours are unchanged)
    const uint32_t movie = key - 800;
    w = 256, h = 128;
    buf.assign(size_t(w) * h * 4, 255);
    static const uint8_t kCol[8][3] = {{255, 60, 60}, {60, 255, 60}, {80, 80, 255}, {255, 255, 60}, {60, 255, 255}, {255, 120, 200}, {255, 160, 40}, {200, 200, 200}};
    const uint8_t* c = kCol[movie & 7];
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        const float g = float((x + seq * 8) % w) / float(w);
        uint8_t* p = &buf[(size_t(y) * w + x) * 4];
        const bool grid = (x % 32) < 3 || (y % 32) < 3;
        p[0] = grid ? 255 : uint8_t(c[0] * g), p[1] = grid ? 255 : uint8_t(c[1] * g), p[2] = grid ? 255 : uint8_t(c[2] * g);
      }
    rgba = buf.data();
    content_id = (uint64_t(key) << 32) | seq;   // S103 UPL: one buffer reused by every call (valid until the next call), the id says which picture it holds now
    ++calls;
    return true;
  }
  std::vector<uint8_t> buf;
  uint32_t calls = 0;
};

}  // namespace

// The driver's ModelCache (lane R's record_frame.cpp has the real one): loads on a miss through the source, keeps models by key.
namespace rdr_scene::rf {
ModelCache::ModelCache(FrontEnd& fe, AssetSource& source, bool load_on_miss) : fe_(fe), src_(source), load_on_miss_(load_on_miss) {}
int ModelCache::Get(const SceneTables& t, uint32_t asset_index, std::string* why) {
  if (asset_index >= t.assets.size()) { if (why) *why = "unknown asset"; return -1; }
  const AssetRef& a = t.assets[asset_index];
  const std::string key = std::string(t.Str(a.pac)) + "|" + std::string(t.Str(a.entry)) + "|" + std::to_string(a.variant);
  const int f = Find(key);
  if (f >= 0) return f;
  assets::Model m;
  std::string err;
  if (!src_.Load(std::string(t.Str(a.pac)), std::string(t.Str(a.entry)), a.variant, m, err)) { if (why) *why = err; return -1; }
  return Put(key, m);
}
int ModelCache::Put(const std::string& key, const assets::Model& m, uint8_t group) {
  (void)group;   // S103 GSTP signature (the driver draws no group stamps)
  std::vector<std::string> warnings;
  const int id = fe_.AddModel(m, warnings);
  for (const std::string& s : warnings) std::fprintf(stderr, "[c_driver] %s\n", s.c_str());
  models_[key] = id;
  return id;
}
int ModelCache::Find(const std::string& key) const {
  auto it = models_.find(key);
  return it == models_.end() ? -1 : it->second;
}
int ModelCache::PosedFind(const std::string& pool, size_t index) const {   // S103 C5: the driver's copy of record_frame.cpp's
  const auto it = pools_.find(pool);
  return it != pools_.end() && index < it->second.size() ? it->second[index] : -1;
}
uint64_t g_posed_model_calls = 0;   // S103 RGP: how many CPU-posed slots the run asked for (the GPU-skinned ring asks for none: ring_gpu_check.py)
int ModelCache::PosedModel(const std::string& pool, size_t index, const assets::Model& posed, bool textureless, uint8_t group) {
  ++g_posed_model_calls;
  (void)group;   // S103 GSTP signature   // S103 C4: the driver's copy of record_frame.cpp's (slot rewritten in place, later slots share the first's images)
  std::vector<int>& v = pools_[pool];
  if (index < v.size() && v[index] >= 0) {
    if (fe_.UpdateModelVertices(v[index], posed)) return v[index];
    v[index] = -1;
  }
  if (v.size() <= index) v.resize(index + 1, -1);
  const bool share = index > 0 && v[0] >= 0;
  if (textureless && !share) return -1;
  std::vector<std::string> warnings;
  const int id = share ? fe_.AddModelSharing(posed, v[0], warnings) : fe_.AddModel(posed, warnings);
  v[index] = id;
  return id;
}
void ModelCache::AdoptLoaded() {}
// S103 MBC (rf_props.cpp): the map-B props' CPU copy lives in record_frame.cpp, which the driver does not link (it has this ModelCache copy). The driver draws no map-B props: the copy is never kept, so these are no-ops / null.
void ModelCache::SetPropCpu(int, const assets::Model&) {}
void ModelCache::DropPropCpu(int) {}
const assets::Model* ModelCache::PropCpuModel(int, uint32_t&) const { return nullptr; }
}  // namespace rdr_scene::rf
#include "modelcache_stub_common.h"   // S103 TLF: StoredNamed / DropStored / RetireOther (rf_props.cpp, rf_c_core.cpp call them)
namespace rdr_scene::rf::crowd {   // S103 GRF: rf_arena_live.cpp calls these (rf_crowd.cpp is not linked; the driver draws no crowd)
bool PageStale(const FrontEnd&, uint64_t) { return false; }
void SetStagePage(FrontEnd&, uint64_t, uint32_t, uint32_t, const uint8_t*) {}
}  // namespace rdr_scene::rf::crowd
namespace rdr_scene::rf {
const char* FamilyName(Family f) {
  static const char* const k[] = {"Arena", "Ring", "Props", "Wrestlers", "Crowd", "Screens", "Flashes", "Particles", "Other"};
  return uint8_t(f) < uint8_t(Family::Count) ? k[uint8_t(f)] : "?";
}
}  // namespace rdr_scene::rf

namespace {

// S103 C3: the flash writer's rows from a fake guest (flash_fx_core.h): 24 items on a ring around the crowd in the game's space (-Y up), as the live writer would read them from the flash manager's item
// array. `swapped` = the planted fault: the guest keeps y and z swapped (+20 / +24), so every row's position is wrong. Scale 0.5 / 0.15 are the game's own two values (item +40).
struct FakeGuest {
  std::map<uint32_t, uint32_t> w;
  void Put(uint32_t a, uint32_t v) { w[a] = v; }
  void PutF(uint32_t a, float f) { uint32_t b; std::memcpy(&b, &f, 4); w[a] = b; }
};
FakeGuest g_fake;
bool FakeRead(uint32_t a, void* dst, size_t n) {
  auto* p = static_cast<uint8_t*>(dst);
  for (size_t i = 0; i < n; i += 4) {
    auto it = g_fake.w.find(a + uint32_t(i));
    if (it == g_fake.w.end()) return false;
    const uint32_t be = rdr_scene::live::flashfx::Swap32(it->second);
    std::memcpy(p + i, &be, 4);
  }
  return true;
}
void LiveFlashRows(bool swapped, std::vector<StageFxRow>& rows) {
  namespace ffx = rdr_scene::live::flashfx;
  constexpr uint32_t kM = 0x40100000, kP = 0x40180000, kPassObj = 0x40190000;
  g_fake.w.clear();
  g_fake.Put(ffx::kManagerGlobal, kM);
  g_fake.Put(ffx::kPassGlobal, kP);
  g_fake.Put(kP + 12, kPassObj);
  g_fake.Put(ffx::kLoopGate, 1);
  g_fake.Put(ffx::kLightSetFlag, 0);
  for (uint32_t k = 0; k < 4; ++k)
    for (uint32_t c = 0; c < 4; ++c) g_fake.PutF(kM + ffx::kColorBlocks + ffx::kBlockStride * k + ffx::kBlockColors + 4 * c, 1.f);
  for (uint32_t i = 0; i < ffx::kItemCount; ++i) {
    const uint32_t item = 0x40200000 + i * 0x100;
    g_fake.Put(kM + ffx::kItemArray + 4 * i, item);
    const float a = float(i) * 6.2831853f / float(ffx::kItemCount), rad = 1700.f + 60.f * float(i % 5), y = -(250.f + 40.f * float(i % 7)) + ffx::kYDrop;   // y - 40 = the ring of the old synthetic rows
    for (uint32_t o = 0; o < 64; o += 4) g_fake.Put(item + o, 0);
    g_fake.Put(item, ffx::kItemVtable);
    g_fake.PutF(item + 16, rad * std::cos(a));
    g_fake.PutF(item + (swapped ? 24 : 20), y);
    g_fake.PutF(item + (swapped ? 20 : 24), rad * std::sin(a));
    g_fake.Put(item + 32, 1);
    g_fake.Put(item + 36, 2);
    g_fake.PutF(item + 40, (i & 1) ? 0.15f : 0.5f);
    g_fake.Put(item + 44, 1);
    g_fake.Put(item + 52, 1);   // drawn
    g_fake.Put(item + 56, i & 3);
  }
  ffx::FlashCensus cn;
  ffx::FlashState st;
  ffx::BuildFlashRows(&FakeRead, 1, rows, cn, st);
  std::fprintf(stderr, "[c] live flash rows: %zu (census rows %llu empty %llu)\n", rows.size(), (unsigned long long)cn.rows, (unsigned long long)cn.empty_slots);
}

struct Panel {
  std::string name;
  uint32_t families;
  bool extras;       // the synthetic stage rows are in the record
  bool dim_lights;   // ONE set's lights are scaled (so the light set ids matter)
  const char* plant;
  uint32_t seq = 1;  // the screens' picture serial of this panel
  bool rest_rows = false;  // the moving slots' rows are the loader's own rest matrices (the picture must not change)
  const char* order = "";  // S103 FOB: the row order: "" = the game's (kCRows as listed); "skirts_late" / "screens_late" / "old" = one difference put back (the price panels)
};

}  // namespace

int main(int argc, char** argv) {
  std::string game, capture, out_dir = "out", tag = "c";
  long frame = -1;
  uint32_t w = 960, h = 540;
  float cam_dist = 2600, cam_h = 700, cam_yaw = 0, cam_fov = 55, cam_tx = 0, cam_ty = 150, cam_tz = 0;   // S103 FOB: --target for the price views
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--capture" && i + 1 < argc) capture = argv[++i];
    else if (a == "--frame" && i + 1 < argc) frame = std::atol(argv[++i]);
    else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
    else if (a == "--tag" && i + 1 < argc) tag = argv[++i];
    else if (a == "--target" && i + 1 < argc) std::sscanf(argv[++i], "%f,%f,%f", &cam_tx, &cam_ty, &cam_tz);
    else if (a == "--cam" && i + 1 < argc) std::sscanf(argv[++i], "%f,%f,%f,%f", &cam_dist, &cam_h, &cam_yaw, &cam_fov);
    else if (a == "--size" && i + 1 < argc) { if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2) return 1; }
  }
  if (game.empty() || capture.empty() || frame < 0) { std::fprintf(stderr, "usage: c_driver --game DIR --capture FOLDER --frame N --out DIR [--size WxH] [--cam dist,height,yaw,fov] [--tag T]\n"); return 1; }

  SceneTables base;
  std::string err;
  char name[32];
  std::snprintf(name, sizeof name, "frame_%06ld.rsc", frame);
  std::filesystem::path rp = std::filesystem::path(capture) / "scene" / name;
  if (!ReadRecord(rp.string(), base, err)) { std::fprintf(stderr, "c_driver: FAIL bad record: %s\n", err.c_str()); return 2; }
  if (base.header.source == uint8_t(Source::Live)) ConvertLiveToViewer(base);
  ViewerInit(game);

  // the camera: the record has none (no capture on disk carries one): a look-at at the ring from the arena's side, in viewer space (Y up)
  {
    const view::V3 eye = {cam_tx + cam_dist * std::sin(cam_yaw * view::kDeg), cam_h, cam_tz - cam_dist * std::cos(cam_yaw * view::kDeg)};
    const view::Mat4 vp = view::Mul(view::LookAt(eye, {cam_tx, cam_ty, cam_tz}, {0, 1, 0}), view::Perspective(cam_fov, float(w) / float(h), 20.f, 20000.f));
    base.has_camera = true;
    std::memcpy(base.camera.view.m, vp.m, sizeof vp.m);
    base.camera.proj = IdentityMat4();
    base.camera.flags = kCamViewIsViewProj;
    base.camera.eye[0] = eye.x, base.camera.eye[1] = eye.y, base.camera.eye[2] = eye.z;
    base.camera.fov_y_deg = cam_fov;
  }

  rdr::RendererOptions ro;
  if (const char* v = std::getenv("WWE_RDR_VALIDATION")) ro.validation = std::atoi(v);
  if (const char* g = std::getenv("WWE_RDR_RING_GPU_SKIN")) rdr_scene::g_ring_gpu_skin.store(std::string(g) != "0", std::memory_order_relaxed);   // S103 RGP (RIG-ONLY: the game reads the cvar wwe2k14_rdr_ring_gpu_skin): 0 = the CPU-posed ring, the "before" arm of ring_gpu_check.py
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::fprintf(stderr, "c_driver: FAIL renderer: %s\n", err.c_str()); return 3; }
  int rc = 0;
  {
    FrontEnd fe(*renderer, FrontEndOptions());
    SeamSource source;
    rf::ModelCache cache(fe, source, true);
    TestFeed feed;
    rf::c::SetScreenFeed(&feed);
    rf::c::SetOfflineLoad(true);  // offline: the arena is loaded through the source; live it comes from the loader thread's queue
    // the loader's rest matrices of the moving slots (game space rows), and the most used light set id of the slot objects (the one set the dim panels edit)
    std::map<int, std::array<float, 12>> rest;
    uint32_t dim_set = 0;
    std::map<std::string, std::pair<int, int>> fam_mesh;   // S103 FOB (review 5): family -> (meshes in the arena file, meshes whose material can show an order: blended / alpha-tested / soft diffuse alpha)
    {
      assets::Model am;
      live::arena::RecordView rv;
      std::string e2;
      if (live::arena::ReadArenaRecord(base, rv, e2) && ViewerLoad("arena", rv.arena_id, 0, am, e2)) {
        for (const assets::Mesh& me : am.meshes) {
          int k = 0;
          if (std::sscanf(me.part.c_str(), "slot %d", &k) != 1) continue;
          const char* fam = k == 953 ? "canvas" : (k == 973 || k == 974) ? "skirt" : (k == 951 || k == 981) ? "post" : k == 952 ? "turnbuckle" : (k == 964 || k == 982) ? "cover" : (k == 960 || k == 980) ? "rope" : (k >= 800 && k <= 809) ? "screens" : nullptr;
          if (!fam) continue;
          ++fam_mesh[fam].first;
          if (me.material >= 0 && size_t(me.material) < am.materials.size()) {
            const assets::Material& mt = am.materials[size_t(me.material)];
            if (mt.alpha_blend || mt.alpha_test || mt.blend_inline || mt.diffuse_alpha_clear > 0.f) ++fam_mesh[fam].second;
          }
        }
        {
          std::string fs;
          for (const auto& kv : fam_mesh) fs += " " + kv.first + "=" + std::to_string(kv.second.second) + "/" + std::to_string(kv.second.first);
          std::printf("[c] order-relevant meshes (soft-alpha or blended / in the file, not culled by view):%s\n", fs.c_str());
        }
        std::map<uint32_t, int> uses;
        for (const auto& kv : rv.slots)
          if (kv.second.set != live::arena::kNone) ++uses[kv.second.set];
        int best = -1;
        for (const auto& kv : uses)
          if (kv.second > best) best = kv.second, dim_set = kv.first;
        {
          std::map<uint32_t, int> ls;
          for (const Light& l : base.lights) ++ls[l.set];
          for (const auto& kv : ls) std::fprintf(stderr, "[c] record lights: set %u has %d lights\n", kv.first, kv.second);
        }
        if (const char* e = std::getenv("WWE_C_DIM_SET")) dim_set = uint32_t(std::atoi(e));
        for (const auto& kv : uses) std::fprintf(stderr, "[c] light set %u used by %d slots%s\n", kv.first, kv.second, kv.first == dim_set ? " (dimmed)" : "");
        for (uint16_t slot = 101; slot <= 116; ++slot) {
          assets::Matrix m = assets::Identity();
          for (const assets::Mesh& me : am.meshes)
            if (me.part == "slot " + std::to_string(slot) && me.placement >= 0 && !am.placements[size_t(me.placement)].transforms.empty()) { m = am.placements[size_t(me.placement)].transforms[0]; break; }
          m = assets::MirrorY(m);  // viewer -> game space
          std::array<float, 12> rows{};
          for (int k = 0; k < 3; ++k)
            for (int r = 0; r < 4; ++r) rows[size_t(4 * k + r)] = m[size_t(4 * r + k)];
          rest[slot] = rows;
        }
      }
    }

    using rf::Family;
    const auto bit = [](Family f) { return 1u << uint32_t(f); };
    const uint32_t kMine = bit(Family::Arena) | bit(Family::Ring) | bit(Family::Props) | bit(Family::Screens) | bit(Family::Flashes);
    const std::vector<Panel> panels = {
        {"whole_plain", kMine, false, false, ""},             // first: no synthetic rows, no moving slots yet: what the capture alone gives
        // S103 FOB (docs\research\s103\XFO.md section 4): the PRICE panels: whole_plain (the game's family order) with ONE difference put back. They sit right after whole_plain (before rest_rows: a panel with moving-slot rows rebuilds the arena set, and a panel without rows then loses those slots).
        {"fo_old_skirts", kMine, false, false, "", 1, false, "skirts_late"},   // D1 reverted: canvas + skirts right after the arena (before the screens and the ring's posts ...)
        {"fo_old_screens", kMine, false, false, "", 1, false, "screens_late"}, // D2 reverted: the screens after the props (last of the opaque rows)
        {"fo_old_rank", kMine, false, false, "ring_old_rank"},                 // D3 reverted: the ring pieces in the record's order (ropes, covers, turnbuckles, posts)
        {"fo_old_all", kMine, false, false, "ring_one_run", 1, false, "old"},  // the old order whole: arena, ring (one run, record order), props, screens, flashes: the planted twin of whole_plain
        {"rest_rows", kMine, false, false, "", 1, true},       // the moving slots at the loader's own rest matrices: must equal whole_plain
        {"rest_rows_last", kMine, false, false, "moving_last", 1, true},   // S103 RGF: planted twin: the rest rows, but the moving slots appended AFTER the static arena (the old order): must differ from whole_plain by far more than the bar
        {"whole", kMine, true, false, ""},
        {"arena", bit(Family::Arena), true, false, ""},
        {"ring", bit(Family::Ring), true, false, ""},
        {"ring_rest", bit(Family::Ring), true, false, "ring_at_rest"},      // S103 C4: the ring at rest although the record carries its rows (the A/B of the posed ring)
        {"ring_skirts", bit(Family::Ring), true, false, "ring_skirts"},   // S103 C4: the game's 3 skirt entries taken as ours (pad2 = their order): the skirts posed from their own 66-node rows
        {"ring_gamerope", bit(Family::Ring), true, false, "ring_game_rope"},   // S103 RLB2: the ropes from the GAME's own rows (ours dropped): its LOD classes (low = 980) reach the draw
        {"ring_ourslow", bit(Family::Ring), true, false, "ring_ours_low"},   // S103 RLB2 (review 3): OUR recomputed ropes flipped to the low class: the 980 path on our own rows
        {"ring_nopose", bit(Family::Ring), true, false, "ring_no_pose"},    // S103 C4: planted: the posed path with every node at the identity: must differ from "ring" when the rows move anything
        {"props", bit(Family::Props), true, false, ""},
        {"screens", bit(Family::Screens), true, false, ""},
        {"flashes", bit(Family::Flashes), true, false, ""},
        {"dim", kMine, true, true, ""},                       // ONE light set dimmed
        {"dim_plant_light_sets", kMine, true, true, "light_sets"},   // planted: ids ignored: must differ from "dim"
        {"dim_plant_shift", kMine, true, true, "shift_ids"},         // planted: the record's sets joined at id + 1: must differ from "dim"
        {"plant_slot_worlds", kMine, true, false, "slot_worlds"},    // planted: the moving pieces are not moved: must differ from "whole"
        {"plant_rotation", kMine, true, false, "rotation"},          // planted: rows' rotation dropped: must differ from "whole"
        {"plant_screens", kMine, true, false, "screens"},
        {"screens_plant", bit(Family::Screens), true, false, "screens", 2},
        {"screens_next", bit(Family::Screens), true, false, "", 2},
        {"plant_flashes", kMine, true, false, "flashes"},
        {"flash_none", kMine, false, false, ""},                      // S103 C3: no stage rows at all
        {"flash_live", kMine, false, false, "live_flashes"},          // S103 C3: the flash rows the live writer makes
        {"flash_live_plant", kMine, false, false, "live_flashes_swapped"},   // S103 C3: planted: the guest keeps y and z swapped: the flashes land elsewhere
    };
    std::vector<std::string> order_log;   // S103 FOB: the family-order tokens (LogOrder in the appliers)
    rf::c::OrderLogRef() = &order_log;
    const auto row_order = [](const std::string& o) -> std::vector<size_t> {   // indexes into kCRows (0 ring_early, 1 arena, 2 screens, 3 ring_late, 4 props, 5 flashes)
      if (o == "skirts_late") return {1, 0, 2, 3, 4, 5};
      if (o == "screens_late") return {0, 1, 3, 4, 2, 5};
      if (o == "old") return {1, 3, 4, 2, 5};
      return {0, 1, 2, 3, 4, 5};
    };
    for (const Panel& p : panels) {
      SceneTables t = base;
      if (p.dim_lights)
        for (Light& l : t.lights)
          if (l.set == dim_set)
            for (float& c : l.color) c *= 0.15f;
      if (p.plant && std::string(p.plant).rfind("live_flashes", 0) == 0) LiveFlashRows(std::string(p.plant) == "live_flashes_swapped", t.stage_fx);   // S103 C3
      if (p.rest_rows)
        for (const auto& kv : rest) {
          StageFxRow r;
          r.kind = kFxSlotWorld, r.slot = uint16_t(kv.first);
          std::memcpy(r.world, kv.second.data(), sizeof r.world);
          t.stage_fx.push_back(r);
        }
      if (p.extras) {
        // moving pieces: slots 101-108 lifted 250 units (the game's space is -Y up: translation y = -250), 109-116 turned 30 degrees about the vertical axis
        for (uint16_t slot = 101; slot <= 116; ++slot) {
          StageFxRow r;
          r.kind = kFxSlotWorld, r.slot = slot;
          const float c = 0.8660254f, sn = 0.5f;
          const float lift[12] = {1, 0, 0, 0, 0, 1, 0, -250.f, 0, 0, 1, 0};
          const float turn[12] = {c, 0, sn, 0, 0, 1, 0, 0, -sn, 0, c, 0};
          std::memcpy(r.world, slot <= 108 ? lift : turn, sizeof r.world);
          t.stage_fx.push_back(r);
        }
        // screens: one row per stage movie the record names (keys 800-807), a new picture serial each
        for (uint16_t slot = 800; slot <= 807; ++slot) {
          StageFxRow r;
          r.kind = kFxScreen, r.slot = slot, r.movie = uint32_t(slot - 800), r.seq = p.seq, r.w = 256, r.h = 128;
          t.stage_fx.push_back(r);
        }
        // flashes: a ring of 40 around the crowd, game space (-Y up)
        for (int k = 0; k < 40; ++k) {
          StageFxRow r;
          r.kind = kFxFlash;
          const float a = float(k) * 6.2831853f / 40.f, rad = 1700.f + 60.f * float(k % 5);
          r.pos[0] = rad * std::cos(a), r.pos[1] = -(250.f + 40.f * float(k % 7)), r.pos[2] = rad * std::sin(a);
          r.scale = 300.f, r.alpha = 1.f;
          t.stage_fx.push_back(r);
        }
      }
      rf::c::PlantsRef() = rf::c::Plants();
      const std::string pl = p.plant;
      if (pl == "light_sets") rf::c::PlantsRef().ignore_light_sets = true;
      if (pl == "shift_ids") rf::c::PlantsRef().shift_light_ids = true;
      if (pl == "rotation") rf::c::PlantsRef().translation_only = true;
      if (pl == "slot_worlds") rf::c::PlantsRef().ignore_slot_worlds = true;
      if (pl == "moving_last") rf::c::PlantsRef().moving_last = true;   // S103 RGF
      if (pl == "screens") rf::c::PlantsRef().never_update_screens = true;
      if (pl == "flashes") rf::c::PlantsRef().ignore_flashes = true;
      if (pl == "ring_at_rest") rf::c::PlantsRef().ring_at_rest = true;    // S103 C4
      if (pl == "ring_skirts") {   // S103 C4: a record from before the skirt reader: the game's skirt entries stand in for ours (the order of the three objects = the skirt index)
        uint16_t idx = 0;
        for (RingRows& r : t.ring_rows)
          if (r.klass == kRcSkirt && !(r.flags & kRrRecomputed)) r.flags |= kRrRecomputed, r.pad2 = idx++;
      }
      if (pl == "ring_game_rope")   // S103 RLB2: drop the recomputed rope entries so the game's own rope rows (their class carries its LOD choice) are the ones drawn
        t.ring_rows.erase(std::remove_if(t.ring_rows.begin(), t.ring_rows.end(), [](const RingRows& r) { return (r.klass == kRcRopeHi || r.klass == kRcRopeLo) && (r.flags & kRrRecomputed); }), t.ring_rows.end());
      if (pl == "ring_ours_low")   // S103 RLB2 (review 3): our recomputed rope rows take the low class (what RopeIsLow gives a far camera): they draw key 980
        for (RingRows& r : t.ring_rows)
          if (r.klass == kRcRopeHi && (r.flags & kRrRecomputed)) r.klass = kRcRopeLo;
      rf::c::PlantsRef().ring_gpu_verify = std::getenv("WWE_RDR_RING_GPU_VERIFY") != nullptr;   // S103 RLB2 review 6: was a getenv in rf_ring.cpp
      rf::c::PlantsRef().ring_key_census = std::getenv("WWE_RDR_RING_KEYS") != nullptr;  // S103 RLB2 (rig-only read, tools may read the env)
      if (pl == "ring_no_pose") rf::c::PlantsRef().ring_no_pose = true;
      if (pl == "ring_old_rank") rf::c::PlantsRef().ring_old_rank = true;   // S103 FOB
      if (pl == "ring_one_run") rf::c::PlantsRef().ring_one_run = true;     // S103 FOB

      // S103 CR: EnsureArena rebuilds its set for a NEW signature (moving slots, sized screens) only after kRebuildDebounceFrames consecutive guest frames of it (L31A). A single-frame panel never got there:
      // every panel is built over that many consecutive frames and the last one is drawn.
      SceneFrame f;
      std::vector<rf::ClassReport> reports;
      static uint64_t gframe = 1000;
      for (uint32_t pass = 0; pass <= rf::c::kRebuildDebounceFrames; ++pass) {
        t.header.guest_frame = ++gframe;
        f = SceneFrame();
        reports.clear();
        order_log.clear();
        f.camera = t.camera;
        f.width = w, f.height = h;
        f.time_s = t.header.time_s;
        f.viewer_space = true;
        rf::BuildCtx ctx{t, cache, f, p.families, reports};
        cache.NewFrame();
        for (const size_t ri : row_order(p.order)) {
          const rf::ClassEntry& e = rf::c::kCRows[ri];
          if (p.families & (1u << uint32_t(e.family)))
            if (!e.apply(ctx)) { std::fprintf(stderr, "c_driver: class %s returned false (a miss)\n", e.name); rc = 3; }
        }
      }
      int arena_rows = -1;
      std::string arena_order;   // S103 SOF: the arena objects in draw order: S = a static segment, M<slot> = a moving slot (c_sheet.py checks the order rule: ascending slot, the static meshes after the last moving slot drawn after it)
      if (p.families & bit(Family::Arena)) {
        SceneFrame f2;
        std::vector<rf::ClassReport> rep2;
        rf::BuildCtx ctx2{t, cache, f2, p.families, rep2};
        std::string why2;
        bool miss2 = false;
        if (rf::c::ArenaSet* as = rf::c::EnsureArena(ctx2, why2, miss2)) {
          std::map<uint32_t, std::string> tok;
          for (const auto& kv : as->moving) tok[uint32_t(kv.second.model)] = "M" + std::to_string(kv.first);
          if (as->part[size_t(rf::c::Part::Arena)].model >= 0) tok[uint32_t(as->part[size_t(rf::c::Part::Arena)].model)] = "S";
          for (const auto& c : as->cuts) if (c.seg.model >= 0) tok[uint32_t(c.seg.model)] = "S";
          std::set<int> row_slots;   // the moving slots the record carries a row for that have a model: each is drawn exactly once (c_sheet.py counts the M tokens against this)
          for (const StageFxRow& r : t.stage_fx)
            if (r.kind == kFxSlotWorld && !rf::c::PlantsRef().ignore_slot_worlds) {
              auto mv = as->moving.find(int(r.slot));
              if (mv != as->moving.end() && mv->second.model >= 0) row_slots.insert(int(r.slot));
            }
          arena_rows = int(row_slots.size());
          for (const Object& o : f.objects) {
            auto it = tok.find(o.asset);
            if (it != tok.end()) arena_order += (arena_order.empty() ? "" : " ") + it->second;
          }
        }
      }
      std::vector<uint8_t> rgba;
      DrawLists panel_lists;   // S103 RGP review F2 (RIG-ONLY): WWE_RDR_RING_LISTS=1 draws through the kept draw lists and prints their census, so ring_gpu_check.py can fail on "duplicate keys" (two instances sharing one draw key)
      const bool with_lists = std::getenv("WWE_RDR_RING_LISTS") != nullptr;
      if (with_lists) fe.SetDrawLists(&panel_lists);
      if (!fe.RenderToPixels(f, rgba, err)) { std::fprintf(stderr, "c_driver: FAIL draw: %s\n", err.c_str()); rc = 3; continue; }
      if (with_lists) { fe.SetDrawLists(nullptr); std::printf("panel %s %s\n", p.name.c_str(), panel_lists.Census().c_str()); }
      const std::string path = (std::filesystem::path(out_dir) / (tag + "_" + p.name + ".png")).string();
      if (!WritePngRgb(path, rgba, w, h)) { std::fprintf(stderr, "c_driver: FAIL cannot write %s\n", path.c_str()); rc = 3; continue; }
      // pixels that differ from the clear colour (a count of what was drawn)
      uint64_t drawn_px = 0;
      const uint8_t cl[3] = {uint8_t(f.clear[0] * 255.f + 0.5f), uint8_t(f.clear[1] * 255.f + 0.5f), uint8_t(f.clear[2] * 255.f + 0.5f)};
      uint64_t magenta = 0;
      for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        drawn_px += (std::abs(int(rgba[i]) - cl[0]) + std::abs(int(rgba[i + 1]) - cl[1]) + std::abs(int(rgba[i + 2]) - cl[2])) > 6;
        magenta += rgba[i] > 200 && rgba[i + 1] < 60 && rgba[i + 2] > 200;
      }
      std::printf("[c] panel %s crc %08x drawn_px %llu magenta_px %llu validation %llu texture_updates %llu\n", p.name.c_str(), Crc32(rgba.data(), rgba.size()),
                  (unsigned long long)drawn_px, (unsigned long long)magenta, (unsigned long long)fe.validation_errors(), (unsigned long long)fe.texture_updates());
      if (std::string(p.order) != "" || std::string(p.plant).rfind("ring_old", 0) == 0 || std::string(p.plant) == "ring_one_run") {   // S103 FOB (review 5): what the reverted difference can show at all
        std::vector<const char*> moved;
        if (std::string(p.order) == "skirts_late" || std::string(p.order) == "old") moved.insert(moved.end(), {"canvas", "skirt"});
        if (std::string(p.order) == "screens_late" || std::string(p.order) == "old") moved.push_back("screens");
        if (std::string(p.plant) == "ring_old_rank" || std::string(p.plant) == "ring_one_run") moved.insert(moved.end(), {"post", "turnbuckle", "cover", "rope"});
        int soft = 0;
        std::string fs;
        for (const char* m : moved) { soft += fam_mesh[m].second; fs += std::string(" ") + m + "=" + std::to_string(fam_mesh[m].second); }
        std::printf("[c]   order-relevant meshes of the moved families %s:%s%s\n", p.name.c_str(), fs.c_str(), soft == 0 ? " -> not observable by construction (no soft-alpha / blended mesh)" : "");
      }
      if (!order_log.empty()) {   // S103 FOB: the family order of the panel (consecutive repeats collapsed): c_sheet.py asserts the game's sequence on whole_plain
        std::string fo;
        for (const std::string& tk : order_log) fo += (fo.empty() ? "" : " ") + tk;
        std::printf("[c]   family order %s: %s\n", p.name.c_str(), fo.c_str());
      }
      if (!arena_order.empty()) std::printf("[c]   arena order %s: %s | rows %d\n", p.name.c_str(), arena_order.c_str(), arena_rows);
      for (const rf::ClassReport& r : reports)
        std::printf("[c]   %s: objects %u drawn %u skipped %u%s%s\n", rf::FamilyName(r.family), r.objects, r.drawn, r.skipped, r.why.empty() ? "" : " why ", r.why.c_str());
    }
    std::fputs(fe.CoverageText().c_str(), stdout);
    renderer->WaitIdle();
  }
  std::fprintf(stderr, "[c] posed_model_calls %llu\n", (unsigned long long)rdr_scene::rf::g_posed_model_calls);   // S103 RGP
  return rc;
}
