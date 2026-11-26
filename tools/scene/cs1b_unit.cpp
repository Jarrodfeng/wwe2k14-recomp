// body-scale: none (a caster-set unit, no wrestler is drawn)  [S103 RGF: body_scale_feed_lint.py]
// S103 lane CS1B (docs\agents\s103\CS1.md): a GUEST model (the Create-a-Superstar body, mods, created superstars: ModelCache::PutGuest) casts into the shadow maps like a file wrestler.
// Real FrontEnd + ModelCache + LiveMapsHook, one synthetic skinned yCh_skin mesh; 0 game files, 0 game runs (Renderer::Create only, as record_frame_test.cpp).
//   1. PutGuest -> CpuModel(id) is the guest's copy, IsGuestCaster(id); MeasureCasters over a frame holding that object gives n > 0 (the caster set is not empty).
//   2. RetireOther drops the copy with the model (CpuModel null, IsGuestCaster false).
//   3. No caster pass (KeepWrestlerCpu off) keeps no copy for a guest, the same rule as a file wrestler. (S111 PIECELINK F6: the guest-only switch wwe2k14_rdr_guest_casters is deleted.)
//   4. 500 guest-id changes (an editor slider drag makes a new id each): LiveMapsHook::casters_ stays bounded by LIVENESS (an entry whose model the provider dropped is swept), not by age.
//   5. F1: the kept CPU copy holds no texture data. (F5 census / F4 default are read-only changes, not unit-tested.)
// Planted faults (--plant NAME; exit 0 = the fault was caught, as record_frame_test): no_guest_cpu = no copy is kept (the cache's keep rule off; checks 1 / 4 fail),
// no_sweep = the sweep is not run (check 4 fails).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/live_maps.h"
#include "../../src/renderer_scene/prop_cache_sweep.h"
#include "../../src/renderer_scene/props_map_switch.h"
#include "../../src/renderer_scene/record_frame.h"

namespace rdr_scene {
struct LiveMapsTestAccess {
  static bool Measure(LiveMapsHook& h, const SceneFrame& f, uint32_t& n) { float c[3]; float r; return h.MeasureCasters(f, c, r, n); }
  static size_t Casters(const LiveMapsHook& h) { return h.casters_.size(); }
  static void Tick(LiveMapsHook& h, bool sweep) { ++h.frames_; if (sweep) h.SweepGraveyard(); }
};
}  // namespace rdr_scene

using namespace rdr_scene;

struct NoAssets : rf::AssetSource {
  bool Load(const std::string&, const std::string&, uint32_t, assets::Model&, std::string& e) override { e = "no game files in this unit"; return false; }
};

static assets::Model GuestBody() {
  assets::Model m;
  m.label = "guest body";
  assets::Material mat;
  mat.shader = "yCh_skin";
  m.materials.push_back(mat);
  assets::Texture tex;   // F1: decoded pictures the caster copy must not keep (64x64 RGBA8 = 16 KB)
  tex.name = "color", tex.format = assets::TexFormat::kR8G8B8A8, tex.width = tex.height = 64;
  tex.data.assign(size_t(64) * 64 * 4, 0x7F);
  m.textures.push_back(tex);
  m.materials[0].diffuse = 0;
  assets::Mesh me;
  me.part = "body";
  me.material = 0;
  me.skinned = true;
  me.vertices.resize(3);
  for (int i = 0; i < 3; ++i) {
    assets::Vertex& v = me.vertices[size_t(i)];
    std::memset(&v, 0, sizeof v);
    v.pos[0] = float(i), v.pos[1] = float(i * 2), v.pos[2] = 0.5f * float(i);
    v.normal[1] = 1.f;
    v.color = 0xFFFFFFFFu;
    v.weights[0] = 1.f;
  }
  me.indices = {0, 1, 2};
  m.meshes.push_back(me);
  return m;
}

static SceneFrame FrameOf(int model) {
  SceneFrame f;
  f.viewer_space = false;
  f.bones.resize(1);
  std::memset(&f.bones[0], 0, sizeof(Mat4));
  f.bones[0].m[0] = f.bones[0].m[5] = f.bones[0].m[10] = f.bones[0].m[15] = 1.f;
  Object o;
  o.asset = uint32_t(model);
  o.flags = kObjVisible | kObjSkinned;
  std::memset(&o.world, 0, sizeof o.world);
  o.world.m[0] = o.world.m[5] = o.world.m[10] = o.world.m[15] = 1.f;
  o.bones_first = 0, o.bones_count = 1;
  f.objects.push_back(o);
  return f;
}

int main(int argc, char** argv) {
  const std::string plant = (argc > 2 && !std::strcmp(argv[1], "--plant")) ? argv[2] : "";
  std::string err;
  rdr::RendererOptions ro;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::printf("FAIL renderer: %s\n", err.c_str()); return 1; }
  int bad = 0;
  auto check = [&](bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++bad; };
  FrontEnd fe(*renderer);
  NoAssets src;
  rf::ModelCache cache(fe, src, false);
  cache.KeepWrestlerCpu(plant != "no_guest_cpu");   // S111 PIECELINK F6: the one keep rule (no guest-only switch); the plant turns the keep off
  LiveMapsHook hook(*renderer, [&](uint32_t a) -> const assets::Model* { return cache.CpuModel(int(a)); });
  hook.SetGuestPredicate([&](uint32_t a) { return cache.IsGuestCaster(int(a)); });
  const assets::Model body = GuestBody();
  {
    const int id = cache.PutGuest("guest|k0k", body, rdr::kGrpWrestler);
    check(id >= 0, "PutGuest made a front-end model");
    check(cache.CpuModel(id) != nullptr && cache.IsGuestCaster(id), "PutGuest kept a CPU copy (CpuModel non-null, IsGuestCaster)");
    const assets::Model* cpu = cache.CpuModel(id);
    check(cpu && cpu->textures.empty() && !cpu->meshes.empty() && cpu->materials.size() == 1, "F1: the CPU copy holds meshes + materials and NO texture data");
    uint32_t n = 0;
    const bool ok = LiveMapsTestAccess::Measure(hook, FrameOf(id), n);
    check(ok && n > 0, "MeasureCasters over a frame with the guest body: n > 0 (the caster set is not empty)");
    std::printf("      samples %u\n", n);
    cache.RetireOther("guest|", "|k1k");
    cache.ReleaseRetired();
    check(cache.CpuModel(id) == nullptr && !cache.IsGuestCaster(id), "RetireOther drops the copy with the model");
  }
  {   // no caster pass (KeepWrestlerCpu off: no live maps hook): a guest keeps no copy, as a file wrestler keeps none
    cache.KeepWrestlerCpu(false);
    const int id = cache.PutGuest("guest|k1k", body, rdr::kGrpWrestler);
    uint32_t n = 0;
    LiveMapsTestAccess::Measure(hook, FrameOf(id), n);
    check(cache.CpuModel(id) == nullptr && !cache.IsGuestCaster(id) && n == 0, "no caster pass -> no CPU copy for a guest either, n == 0 (one keep rule)");
    cache.KeepWrestlerCpu(plant != "no_guest_cpu");
    cache.RetireOther("guest|", "|k2k");
    cache.ReleaseRetired();
  }
  {   // 500 guest-id changes, 3 frames each: the hook's caster cache stays bounded and drains
    size_t peak = 0;
    for (int i = 0; i < 500; ++i) {
      const std::string key = "guest|k" + std::to_string(i + 10) + "k";
      cache.RetireOther("guest|", "|k" + std::to_string(i + 10) + "k");
      const int id = cache.PutGuest(key, body, rdr::kGrpWrestler);
      for (int f = 0; f < 3; ++f) {
        uint32_t n = 0;
        if (f == 0 && plant != "no_guest_cpu") LiveMapsTestAccess::Measure(hook, FrameOf(id), n);
        LiveMapsTestAccess::Tick(hook, plant != "no_sweep");
        peak = std::max(peak, LiveMapsTestAccess::Casters(hook));
      }
      cache.ReleaseRetired();
    }
    // F2: bounded by LIVENESS (a caster entry lives only while the provider holds its model): at most the live guest(s) + the one retired this change, swept at the next tick
    const size_t bound = cache.cpu_count() + 1;
    check(peak <= bound, ("caster cache bounded by liveness over 500 guest-id changes (peak " + std::to_string(peak) + ", bound live cpu_ " + std::to_string(bound) + ")").c_str());
    LiveMapsTestAccess::Tick(hook, plant != "no_sweep");
    const size_t left = LiveMapsTestAccess::Casters(hook);
    check(left <= cache.cpu_count(), ("the cache holds only live models after the last change (left " + std::to_string(left) + ")").c_str());
    check(cache.cpu_count() <= 1, "the model cache holds at most the live guest's copy");
  }
  std::printf("%s (%d failed)\n", bad ? "FAIL" : "PASS", bad);
  if (!plant.empty()) return bad ? 0 : 1;   // a planted fault must be caught: exit 0 only when a check failed
  return bad ? 1 : 0;
}
