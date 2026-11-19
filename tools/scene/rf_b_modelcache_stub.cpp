// body-scale: none (the lane-B ModelCache stand-in, draws nothing itself)  [S103 RGF: body_scale_feed_lint.py]
// Lane B (Session 103) TEST-ONLY stand-in for lane R's record_frame.cpp ModelCache, so rf_wrestlers.cpp can be built and checked before R merges.
// Dropped from build_rf_b.ps1 once src\renderer_scene\record_frame.cpp is on the session branch (the real one has the same public shape).
#include "../../src/renderer_scene/record_frame.h"
#include "modelcache_stub_common.h"   // S103 TLF: StoredNamed / DropStored / RetireOther (rf_wrestlers.cpp calls RetireOther)

namespace rdr_scene::rf {

ModelCache::ModelCache(FrontEnd& fe, AssetSource& source, bool load_on_miss) : fe_(fe), src_(source), load_on_miss_(load_on_miss) {}

int ModelCache::Get(const SceneTables& t, uint32_t asset_index, std::string* why) {
  if (asset_index >= t.assets.size()) { if (why) *why = "unknown asset index"; return -1; }
  const AssetRef& a = t.assets[asset_index];
  const std::string pac(t.Str(a.pac)), entry(t.Str(a.entry));
  const std::string key = pac + '\n' + entry + '\n' + std::to_string(a.variant);
  auto it = models_.find(key);
  if (it != models_.end()) return it->second;
  if (!load_on_miss_) { ++misses_; if (why) *why = "not resident"; return -1; }
  assets::Model m;
  std::string err;
  if (!src_.Load(pac, entry, a.variant, m, err)) { if (why) *why = "load failed: " + err; return -1; }
  std::vector<std::string> warnings;
  const int idx = fe_.AddModel(m, warnings);
  models_.emplace(key, idx);
  return idx;
}

int ModelCache::Put(const std::string& key, const assets::Model& m, uint8_t group) {
  (void)group;   // S103 GSTP signature (the rig draws no group stamps)
  std::vector<std::string> w;
  const int idx = fe_.AddModel(m, w);
  models_[key] = idx;
  return idx;
}

int ModelCache::PutGuest(const std::string& key, const assets::Model& m, uint8_t group) { return Put(key, m, group); }   // S103 CTX2: no shared guest pictures in the rig
void ModelCache::TrimGuestImages(const assets::Model&) {}

int ModelCache::Find(const std::string& key) const {
  auto it = models_.find(key);
  return it == models_.end() ? -1 : it->second;
}

void ModelCache::AdoptLoaded() {}

}  // namespace rdr_scene::rf
