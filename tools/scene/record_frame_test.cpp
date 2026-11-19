// body-scale: none (ModelCache unit, no wrestler is drawn)  [S103 RGF: body_scale_feed_lint.py]
// Unit check of record_frame.h's ModelCache (S103 lane R). Needs a renderer (front-end AddModel); no game files.
//   a miss -> TakeMisses lists it once -> PutLoaded -> Get answers the model; Stored(): queued with stored = true, PutLoaded keeps the CPU copy and uploads nothing;
//   a failed load (MissFailed, or a load that fails with load_on_miss) is remembered: Get / Stored answer -1 "load failed: ...", not a miss, never queued again, one
//   source call only; ClearFailed makes it askable again; RetireOther + ReleaseRetired; the pool's in-place rewrite makes no model after the first.
// Planted faults (--plant N, the run must FAIL, exit 0 here means "the fault was caught"): 1 = the failure memo is cleared before the second ask (the source is asked
// twice: the failed-once check fails); 2 = PutLoaded is skipped (Get after it fails).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/record_frame.h"

using namespace rdr_scene;

struct CountingAssets : rf::AssetSource {
  int calls = 0;
  bool fail = true;
  bool Load(const std::string&, const std::string&, uint32_t, assets::Model&, std::string& e) override { ++calls; e = "no such file"; return !fail; }
};

static assets::Model OneTriangle() {
  assets::Model m;
  m.label = "tri";
  assets::Mesh me;
  me.part = "tri";
  me.vertices.resize(3);
  me.indices = {0, 1, 2};
  m.meshes.push_back(me);
  return m;
}

int main(int argc, char** argv) {
  const int plant = (argc > 2 && !std::strcmp(argv[1], "--plant")) ? std::atoi(argv[2]) : 0;
  std::string err;
  rdr::RendererOptions ro;
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::printf("FAIL renderer: %s\n", err.c_str()); return 1; }
  int bad = 0;
  auto check = [&](bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++bad; };
  {
    FrontEnd fe(*renderer);
    SceneTables t;
    for (const char* entry : {"arena", "20040"}) {
      AssetRef a;
      a.kind = uint8_t(AssetKind::Model);
      a.pac = t.AddString("pac\\bg\\bg05.pac");
      a.entry = t.AddString(entry);
      t.assets.push_back(a);
    }
    {  // ---- load_on_miss = false: the live cache ----
      CountingAssets src;
      rf::ModelCache cache(fe, src, false);
      std::string why;
      int m0 = cache.Get(t, 0, &why);
      int m1 = cache.Get(t, 0, &why);
      auto misses = cache.TakeMisses();
      check(m0 == -1 && m1 == -1 && why == "not resident", "a miss answers -1, why \"not resident\"");
      check(misses.size() == 1 && misses[0].pac == "pac\\bg\\bg05.pac" && misses[0].entry == "arena" && !misses[0].stored, "TakeMisses lists the missed asset once");
      check(cache.TakeMisses().empty(), "taken misses are gone");
      cache.Get(t, 0, &why);
      check(cache.TakeMisses().empty(), "a pending miss is not queued again until PutLoaded");
      check(src.calls == 0, "the live cache never calls the asset source");
      if (plant != 2) cache.PutLoaded(misses[0], OneTriangle());
      check(cache.Get(t, 0, &why) >= 0, "after PutLoaded, Get answers the model");
      // Stored: queued with stored = true; PutLoaded keeps the CPU copy and uploads nothing
      const uint64_t up0 = cache.uploads();
      check(cache.Stored(t, 1, &why) == nullptr && why == "not resident", "Stored of a missing asset answers nullptr, not resident");
      auto sm = cache.TakeMisses();
      check(sm.size() == 1 && sm[0].stored, "Stored's miss is queued with stored = true");
      const int r = cache.PutLoaded(sm[0], OneTriangle());
      check(r == -1 && cache.Stored(t, 1, &why) != nullptr && cache.uploads() == up0, "PutLoaded of a stored asset keeps the CPU copy and makes no front-end model");
      // MissFailed: remembered, never queued again, not a miss
      SceneTables t2 = t;
      t2.assets[0].entry = t2.AddString("other");
      cache.NewFrame();
      cache.Get(t2, 0, &why);
      auto fm = cache.TakeMisses();
      cache.MissFailed(fm[0], "disk error");
      if (plant == 1) cache.ClearFailed();
      cache.NewFrame();
      const int f1 = cache.Get(t2, 0, &why);
      check(f1 == -1 && why == "load failed: disk error" && cache.misses_this_frame() == 0 && cache.TakeMisses().empty(), "a failed asset answers \"load failed: ...\", is not a miss and is not queued again");
      cache.ClearFailed();
      cache.Get(t2, 0, &why);
      check(cache.TakeMisses().size() == 1, "ClearFailed makes it askable again");
    }
    {  // ---- load_on_miss = true: the offline cache remembers a failure ----
      CountingAssets src;
      rf::ModelCache cache(fe, src, true);
      std::string why;
      cache.Get(t, 0, &why);
      cache.Get(t, 0, &why);
      cache.Stored(t, 0, &why);
      check(src.calls == 1, "a failing load is asked of the source once, then remembered");
      check(why.rfind("load failed: no such file", 0) == 0, "the remembered failure says why");
    }
    {  // ---- retire + pool ----
      CountingAssets src;
      rf::ModelCache cache(fe, src, true);
      cache.Put("arena#pac\\bg\\bg04.pac", OneTriangle());
      cache.Put("arena#pac\\bg\\bg05.pac", OneTriangle());
      cache.RetireOther("arena#", "bg05.pac");
      check(cache.retired_pending() == 1 && cache.Find("arena#pac\\bg\\bg04.pac") < 0 && cache.Find("arena#pac\\bg\\bg05.pac") >= 0, "RetireOther retires the other arena's model only");
      renderer->WaitIdle();
      cache.ReleaseRetired();
      check(cache.retired_pending() == 0 && cache.retired_total() == 1, "ReleaseRetired releases it (the GPU is idle)");
      const assets::Model tri = OneTriangle();
      cache.PosedModel("crowdpose#x", 0, tri);
      cache.PosedModel("crowdpose#x", 1, tri);
      const uint64_t made = cache.uploads();
      for (int i = 0; i < 5; ++i) cache.PosedModel("crowdpose#x", 0, tri), cache.PosedModel("crowdpose#x", 1, tri);
      check(cache.uploads() == made, "a pool is rewritten in place: no model made after the first frame");
    }
  }
  renderer->WaitIdle();
  std::printf("%s\n", bad ? "FAIL record_frame_test" : "PASS record_frame_test");
  if (plant) return bad ? 0 : 1;  // a planted fault must be caught: exit 0 only when a check failed
  return bad ? 1 : 0;
}
