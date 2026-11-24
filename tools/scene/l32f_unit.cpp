// S103 L32F unit: the posed ring's slot state is per ModelCache (live and mirror each have their own pools). 0 game runs, no GPU, no SDK.
//   out\tools\l32f_unit.exe                          -> PASS / FAIL, exit 0 / 1
//   out\tools\l32f_unit.exe --plant shared_state     (the old shape: one ring scratch / created / hash state for every cache) must FAIL
// Links the REAL src\renderer_scene\rf_ring.cpp (ApplyRing) with stand-ins for EnsureArena / Report / LightsFor / WorldFromRows / PlantsRef and for ModelCache::PosedFind / PosedModel
// (the same pool + image-owner rules as record_frame.cpp, no front-end). One ArenaSet, two ModelCaches.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "../../src/renderer_scene/rf_c.h"
#include "../../src/renderer_scene/rows_ring.h"

namespace rdr_scene::rf {

// ---- ModelCache stand-ins (record_frame.cpp's PosedFind / PosedModel without the front-end) ----
ModelCache::ModelCache(FrontEnd& fe, AssetSource& source, bool load_on_miss) : fe_(fe), src_(source), load_on_miss_(load_on_miss) {}
static std::map<const ModelCache*, std::map<int, float>> g_uploaded_x;   // cache -> posed model id -> the x of its first vertex as last uploaded
static int g_next_id = 100;
void ModelCache::RetireOther(const std::string& prefix, const std::string& keep) {   // the pools part of record_frame.cpp's RetireOther
  for (auto it = pools_.begin(); it != pools_.end();) {
    if (it->first.rfind(prefix, 0) == 0 && it->first.find(keep) == std::string::npos) {
      for (size_t i = it->second.size(); i-- > 0;) if (it->second[i] >= 0) retired_.push_back(it->second[i]);
      it = pools_.erase(it);
    } else ++it;
  }
}
int ModelCache::PosedFind(const std::string& pool, size_t index) const {
  const auto it = pools_.find(pool);
  return it != pools_.end() && index < it->second.size() ? it->second[index] : -1;
}
int ModelCache::PosedModel(const std::string& pool, size_t index, const assets::Model& posed, bool textureless, uint8_t group) {
  (void)group;   // S103 GSTP signature
  std::vector<int>& v = pools_[pool];
  if (index < v.size() && v[index] >= 0) {
    g_uploaded_x[this][v[index]] = posed.meshes[0].vertices[0].pos[0];
    return v[index];
  }
  if (v.size() <= index) v.resize(index + 1, -1);
  const bool share = index > 0 && v[0] >= 0;
  if (textureless && !share) return -1;
  const int id = g_next_id++;
  v[index] = id;
  g_uploaded_x[this][id] = posed.meshes[0].vertices[0].pos[0];
  return id;
}

}  // namespace rdr_scene::rf

namespace rdr_scene {
bool BuildSkinChunks(const assets::Mesh&, std::vector<SkinChunk>&) { return false; }   // S103 RGP: rf_ring.cpp's GPU branch cuts chunks (char_effects.cpp); this unit's models never take it (piece.gpu stays unset: no skinned stand-in), so a stand-in that cuts nothing
}  // namespace rdr_scene

namespace rdr {
void Log(const char* fmt, ...) { (void)fmt; }   // S103 RLL: rf_ring.cpp's [ring_gpu] verify line goes through rdr::Log (src\renderer\problem.cpp, not linked into this unit); this unit never sets ring_gpu_verify
}  // namespace rdr

namespace rdr_scene::rf::c {

static Plants g_plants;
Plants& PlantsRef() { return g_plants; }
ArenaSet g_set;
uint32_t g_drawn = 0, g_skipped = 0;
std::string g_note;
ArenaSet* EnsureArena(BuildCtx&, std::string&, bool&) { return &g_set; }
ModelLights LightsFor(const SceneTables&, const FamilyModel&, uint32_t) { return ModelLights(); }
void Report(BuildCtx&, Family, uint32_t, uint32_t drawn, uint32_t skipped, const std::string& why) { g_drawn = drawn, g_skipped = skipped, g_note = why; }
assets::Matrix WorldFromRows(const float rows[12]) {   // row k = (m[k], m[4+k], m[8+k], m[12+k]): the translation is element 3 of each row
  assets::Matrix m = assets::Identity();
  m[12] = rows[3], m[13] = rows[7], m[14] = rows[11];
  return m;
}

}  // namespace rdr_scene::rf::c

using namespace rdr_scene;
using namespace rdr_scene::rf;

namespace {
struct NullSource : AssetSource {
  bool Load(const std::string&, const std::string&, uint32_t, assets::Model&, std::string& e) override { e = "none"; return false; }
};
int g_bad = 0;
void Check(bool ok, const char* name, const std::string& why = "") {
  if (!ok) ++g_bad;
  std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", name, why.empty() ? "" : "  ", why.c_str());
}
constexpr uint32_t kRopes = 12, kPosts = 4, kInst = kRopes + kPosts;

SceneTables MakeRecord(float tx) {   // 12 ropes (key 960) + 4 posts (key 951), one node each, translation x = tx + instance
  SceneTables t;
  t.ring_nodes.resize(kInst);
  for (uint32_t i = 0; i < kInst; ++i) {
    std::memset(t.ring_nodes[i].rows, 0, sizeof t.ring_nodes[i].rows);
    t.ring_nodes[i].rows[0] = t.ring_nodes[i].rows[5] = t.ring_nodes[i].rows[10] = 1.f;
    t.ring_nodes[i].rows[3] = tx + float(i);
    RingRows r;
    r.klass = i < kRopes ? kRcRopeHi : kRcPostHi;
    r.call = i < kRopes ? i : i - kRopes;
    r.first_node = i, r.n_nodes = 1;
    t.ring_rows.push_back(r);
  }
  return t;
}
void MakePiece(int key) {
  c::ArenaSet::RingPiece& p = c::g_set.ring_piece[key];
  assets::Mesh me;
  assets::Vertex v{};
  v.weights[0] = 1.f;
  me.vertices.assign(3, v);
  me.indices = {0, 1, 2};
  p.stored.meshes.push_back(me);
  assets::Texture tx;
  tx.name = "color";
  p.stored.textures.push_back(tx);
  p.fm.meshes = 1;
  c::g_set.ring_rest[key].model = 7;
}
struct Result { uint32_t drawn, skipped; std::string note; };
Result Frame(ModelCache& cache, const SceneTables& t) {
  SceneFrame frame;
  std::vector<ClassReport> reports;
  BuildCtx ctx{t, cache, frame, kAllFamilies, reports};
  c::ApplyRing(ctx);
  return {c::g_drawn, c::g_skipped, c::g_note};
}
}  // namespace

int main(int argc, char** argv) {
  const bool plant = argc > 2 && !std::strcmp(argv[1], "--plant") && !std::strcmp(argv[2], "shared_state");
  c::PlantsRef().ring_shared_state = plant;
  NullSource src;
  alignas(16) static unsigned char fe_storage[1];   // never dereferenced: the stand-ins above do not touch the front-end
  FrontEnd& fe = *reinterpret_cast<FrontEnd*>(fe_storage);
  ModelCache live(fe, src, false), mirror(fe, src, true);
  c::g_set.arena = 1, c::g_set.key = "c:arena:1:test";
  c::g_set.part[size_t(c::Part::Ring)].model = 5, c::g_set.part[size_t(c::Part::Ring)].meshes = 1;
  MakePiece(960), MakePiece(951);

  const SceneTables r1 = MakeRecord(0.f), r2 = MakeRecord(10.f);
  Result a = Frame(live, r1);
  Check(a.skipped == 0 && a.drawn == kInst, "live cache, first frame: every instance posed and drawn", "drawn " + std::to_string(a.drawn) + " skipped " + std::to_string(a.skipped) + " " + a.note);
  Result b = Frame(mirror, r1);
  Check(b.skipped == 0 && b.drawn == kInst, "mirror cache after the live cache built its pools: skips 0", "drawn " + std::to_string(b.drawn) + " skipped " + std::to_string(b.skipped) + " " + b.note);
  // a new pose: the live cache poses it, then the mirror cache must pose it too (a shared hash would call the mirror's slots "already posed")
  a = Frame(live, r2);
  b = Frame(mirror, r2);
  Check(b.skipped == 0, "mirror cache, new rows: not skipped", "skipped " + std::to_string(b.skipped));
  const int mirror_rope0 = mirror.PosedFind("c:arena:1:test:ringpose:960", 0);
  const float x = mirror_rope0 >= 0 ? rdr_scene::rf::g_uploaded_x[&mirror][mirror_rope0] : -1.f;
  Check(x == 10.f, "mirror cache's rope 0 holds the NEW pose (not an old rope pose)", "x " + std::to_string(x) + " want 10");
  // an unchanged frame: each cache keeps its own slots and draws all
  a = Frame(live, r2);
  b = Frame(mirror, r2);
  Check(a.skipped == 0 && b.skipped == 0 && a.drawn == kInst && b.drawn == kInst, "unchanged frame: both caches draw all from their own slots");
  // the set key changes (a new look / arena): each cache retires ITS OWN old pools, whichever one rebuilt the set; the new pools are made
  const size_t pend_live = live.retired_pending(), pend_mirror = mirror.retired_pending();
  c::g_set.key = "c:arena:1:test2";
  a = Frame(live, r2);
  Check(live.PosedFind("c:arena:1:test:ringpose:960", 0) < 0 && live.retired_pending() == pend_live + kInst, "set key change: the live cache retired its old pools", "retired " + std::to_string(live.retired_pending() - pend_live) + " want " + std::to_string(kInst));
  Check(mirror.PosedFind("c:arena:1:test:ringpose:960", 0) >= 0, "(the mirror has not seen the new key yet: its pools stay until its own frame)");
  b = Frame(mirror, r2);
  Check(mirror.PosedFind("c:arena:1:test:ringpose:960", 0) < 0 && mirror.retired_pending() == pend_mirror + kInst, "set key change: the mirror cache retired its old pools too", "retired " + std::to_string(mirror.retired_pending() - pend_mirror) + " want " + std::to_string(kInst));
  Check(a.skipped == 0 && b.skipped == 0 && a.drawn == kInst && b.drawn == kInst, "set key change: both caches draw all from new pools");
  // a NEW cache never inherits another one's state (the state lives on the cache, not under its address)
  {
    ModelCache fresh(fe, src, false);
    const Result f = Frame(fresh, r2);
    Check(f.skipped == 0 && f.drawn == kInst, "a new cache builds its own ring from empty state", "skipped " + std::to_string(f.skipped));
  }
  std::printf("%s l32f_unit: ApplyRing with one ArenaSet and two ModelCaches (live then mirror), a new pose reaches both, an unchanged frame draws from each cache's own slots\n", g_bad ? "FAIL" : "PASS");
  if (plant) { std::printf("%s: the planted twin (shared ring state) %s\n", g_bad ? "PASS" : "FAIL", g_bad ? "was caught" : "was NOT caught"); return g_bad ? 0 : 1; }
  return g_bad ? 1 : 0;
}
