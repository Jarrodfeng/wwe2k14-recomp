// The crowd's prop draws (signs, foam hands: T_Audience_LightMap at 0x82518D60, T_Audience at 0x825192D0), the part that needs no GPU: which file mesh a stream draw is, and the checks
// (S101 lane 133 = N4; docs\renderer\CROWD_PROMOTION.md "N4"). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. No game or SDK includes.
//
// CrowdPropsOurs (crowd_props_ours.h) is the DrawReplaceHook; everything that can be wrong about WHICH mesh a stream draw is lives here, so tools\crowd\crowd_props_join.py (an independent
// oracle) and the unit check read the same rule. The join: candidates = the arena's prop meshes (crowd_prop_game_space.h) with the draw's vertex and index counts; the winner is the first
// whose vertex bytes equal the stream's (position and uv word for word, normal within 1e-6: the game's normal differs from the file's by one ulp on some vertices [C]) and whose indices
// equal the stream's. No candidate with the counts: Unjoined. Counts fit but bytes differ: Mismatch (never drawn). Everything else of the draw (program, state, the light-map page, slot 0,
// the world matrix c76-c78) stays the stream's.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../assets/crowd_prop_game_space.h"
#include "../renderer_adapter/draw_replace_hook.h"

namespace rdr_scene {

// The recorder calls (DrawOrigin::call_site) of the crowd's two per-member loops sub_82518950 / sub_82518EB8: 0x82518D60 (T_Audience_LightMap) and 0x825192D0 (T_Audience) are the draws
// seen on every capture; 0x82518E68 / 0x825194B4 are the loops' other op-15 calls (never seen drawn [C: five captures], claimed so a map without the first two still joins).
inline constexpr uint32_t kCropSiteA0 = 0x82518D60u, kCropSiteA1 = 0x82518E68u, kCropSiteB0 = 0x825192D0u, kCropSiteB1 = 0x825194B4u;

// Planted faults (each must fail its exact check): the acceptance's twins.
struct CropFaults {
  bool sign_wrong_entry = false;   // the join takes the NEXT pac entry's mesh (the wrong sign)
  bool one_material = false;       // R, lane 35's bug: every prop of a frame takes the FIRST prop draw's slot-0 picture
  bool lightmap_stale = false;     // the light-map page replaced by a page the game never rewrote (a constant image: the hook has no GPU copy of last frame's page)
  bool no_verify = false;          // the byte compare of the chosen mesh against the stream is skipped (a wrong choice then reaches the picture)
  bool index32 = false;            // F2 plant: the hook sees the stream's index as 32-bit (it must keep every draw as Variant: our buffer is u16)
  bool normal_flip = false;        // F-extra plant: our normals are compared negated (every draw must mismatch)
  bool normal_tol0 = false;        // F-extra plant: tolerance 0 on the normal (the one-ulp vertices of the game must then mismatch)
  bool arena_switch = false;       // arena-switch arm: after 60 frames the hook is told the next arena (it must drop, reload and re-join)
  bool count_every_candidate = false;   // F1 plant: the old counting (a failed non-winning candidate counts as a mismatch even when another candidate joins)
  bool Any() const { return sign_wrong_entry || one_material || lightmap_stale || no_verify || index32 || normal_flip || normal_tol0 || count_every_candidate || arena_switch; }
  static bool Parse(const std::string& list, CropFaults& f, std::string& error) {
    size_t at = 0;
    while (at <= list.size() && !list.empty()) {
      size_t comma = list.find(',', at);
      if (comma == std::string::npos) comma = list.size();
      const std::string item = list.substr(at, comma - at);
      if (item == "sign_wrong_entry") f.sign_wrong_entry = true;
      else if (item == "one_material") f.one_material = true;
      else if (item == "lightmap_stale") f.lightmap_stale = true;
      else if (item == "no_verify") f.no_verify = true;
      else if (item == "index32") f.index32 = true;
      else if (item == "arena_switch") f.arena_switch = true;
      else if (item == "normal_flip") f.normal_flip = true;
      else if (item == "normal_tol0") f.normal_tol0 = true;
      else if (item == "count_every_candidate") f.count_every_candidate = true;
      else if (!item.empty()) return error = "unknown --replace-fault '" + item + "' for the crowd props (sign_wrong_entry, one_material, lightmap_stale, no_verify, index32, normal_flip, normal_tol0, count_every_candidate, arena_switch)", false;
      at = comma + 1;
    }
    return true;
  }
};

// First differing vertex, or -1. Position (words 0-2) and uv (word 6) must be bit-equal; the normal (words 3-5) within `normal_tol`.
inline int CropFirstVertexDiff(const uint8_t* ours, const uint8_t* stream, uint32_t vertex_count, float normal_tol = 1e-6f, bool flip_normal = false) {
  constexpr uint32_t kStride = assets::crowd_prop_gs::kVertexBytes;
  for (uint32_t i = 0; i < vertex_count; ++i) {
    const uint8_t* a = ours + size_t(i) * kStride;
    const uint8_t* b = stream + size_t(i) * kStride;
    if (std::memcmp(a, b, 12) != 0 || std::memcmp(a + 24, b + 24, 4) != 0) return int(i);
    float na[3], nb[3];
    std::memcpy(na, a + 12, 12);
    std::memcpy(nb, b + 12, 12);
    for (int c = 0; c < 3; ++c)
      if (!(std::fabs((flip_normal ? -na[c] : na[c]) - nb[c]) <= normal_tol)) return int(i);
  }
  return -1;
}

struct CropStats {
  uint64_t decisions = 0, replaced = 0;
  uint64_t kept[uint8_t(rdr_adapter::KeepWhy::Count)] = {};
  uint64_t bad_counts = 0, bad_vertices = 0, bad_indices = 0, bad_layout = 0, no_bytes = 0;
  std::map<uint32_t, uint64_t> joined_by_key;   // pac entry -> draws replaced with it
  std::map<uint32_t, uint64_t> replaced_by_site;
};

class CrowdPropsCore {
 public:
  explicit CrowdPropsCore(const CropFaults& faults) : faults_(faults) {}

  static bool Claims(const rdr_adapter::DrawOrigin& o) {
    return o.klass == rdr_adapter::DrawClass::Crowd && (o.call_site == kCropSiteA0 || o.call_site == kCropSiteA1 || o.call_site == kCropSiteB0 || o.call_site == kCropSiteB1);
  }

  void SetProps(std::shared_ptr<const assets::crowd_prop_gs::PropSet> props) { props_ = std::move(props); }
  const assets::crowd_prop_gs::PropSet* props() const { return props_.get(); }

  struct Decision {
    bool ok = false;
    int mesh = -1;   // index in PropSet::meshes
    rdr_adapter::KeepWhy why = rdr_adapter::KeepWhy::NotReady;
  };

  // `vertices` / `indices`: the stream draw's captured bytes (vertex_count * 28, index_count * 2). Null: not readable -> Unjoined.
  Decision Decide(uint32_t site, uint32_t vertex_count, uint32_t index_count, uint32_t host_stride, const uint8_t* vertices, size_t vertex_bytes, const uint8_t* indices, size_t index_bytes) {
    using assets::crowd_prop_gs::kVertexBytes;
    Decision d;
    ++stats_.decisions;
    const auto keep = [&](rdr_adapter::KeepWhy w) {
      d.why = w;
      ++stats_.kept[uint8_t(w)];
      return d;
    };
    if (!props_ || props_->meshes.empty()) return keep(rdr_adapter::KeepWhy::NotReady);
    if (host_stride != kVertexBytes) return ++stats_.bad_layout, first_fail_ = first_fail_.empty() ? "layout: host stride is not 28" : first_fail_, keep(rdr_adapter::KeepWhy::Variant);
    if (!vertices || !indices || vertex_bytes < size_t(vertex_count) * kVertexBytes || index_bytes < size_t(index_count) * 2) return ++stats_.no_bytes, keep(rdr_adapter::KeepWhy::Unjoined);
    int found = -1;
    bool counts_fit = false;
    int fail_kind = 0;   // 1 vertex bytes, 2 index bytes: the reason of the FIRST candidate with the counts that failed; counted only when no candidate joins (F1)
    size_t fail_mesh = 0;
    const float tol = faults_.normal_tol0 ? 0.0f : 1e-6f;
    for (size_t k = 0; k < props_->meshes.size(); ++k) {
      const assets::crowd_prop_gs::PropMesh& m = props_->meshes[k];
      if (m.vertex_count != vertex_count || m.indices.size() != index_count) continue;
      counts_fit = true;
      if (CropFirstVertexDiff(m.vertices.data(), vertices, vertex_count, tol, faults_.normal_flip) >= 0) {
        if (!fail_kind) fail_kind = 1, fail_mesh = k;
        if (faults_.count_every_candidate) ++stats_.bad_vertices, NoteFail("vertex bytes differ", site, k);
        continue;
      }
      if (std::memcmp(m.indices.data(), indices, size_t(index_count) * 2) != 0) {
        if (!fail_kind) fail_kind = 2, fail_mesh = k;
        if (faults_.count_every_candidate) ++stats_.bad_indices, NoteFail("index bytes differ", site, k);
        continue;
      }
      found = int(k);
      break;
    }
    if (found < 0) {
      if (!counts_fit) ++stats_.bad_counts;
      if (fail_kind && !faults_.count_every_candidate) {
        ++(fail_kind == 1 ? stats_.bad_vertices : stats_.bad_indices);
        NoteFail(fail_kind == 1 ? "vertex bytes differ" : "index bytes differ", site, fail_mesh);
      }
      return keep(counts_fit ? rdr_adapter::KeepWhy::Mismatch : rdr_adapter::KeepWhy::Unjoined);
    }
    if (faults_.sign_wrong_entry) {   // the next pac entry's mesh
      // the next entry (cyclic) whose vertex bytes differ: key 40100 is byte-identical to 40000, which would not be a wrong sign
      size_t next = size_t(found);
      for (size_t k = 1; k < props_->meshes.size(); ++k) {
        const size_t cand = (size_t(found) + k) % props_->meshes.size();
        if (props_->meshes[cand].vertices != props_->meshes[size_t(found)].vertices) {
          next = cand;
          break;
        }
      }
      found = int(next);
      if (!faults_.no_verify) {
        const assets::crowd_prop_gs::PropMesh& m = props_->meshes[size_t(found)];
        if (m.vertex_count != vertex_count || m.indices.size() != index_count || CropFirstVertexDiff(m.vertices.data(), vertices, vertex_count, tol, faults_.normal_flip) >= 0 ||
            std::memcmp(m.indices.data(), indices, size_t(index_count) * 2) != 0) {
          ++stats_.bad_vertices;
          NoteFail("planted wrong entry: bytes differ", site, size_t(found));
          return keep(rdr_adapter::KeepWhy::Mismatch);
        }
      }
    }
    d.ok = true;
    d.mesh = found;
    ++stats_.replaced;
    ++stats_.joined_by_key[props_->meshes[size_t(found)].key];
    ++stats_.replaced_by_site[site];
    return d;
  }

  const CropStats& stats() const { return stats_; }
  const CropFaults& faults() const { return faults_; }
  const std::string& first_fail() const { return first_fail_; }
  std::string TakeFirstFail() {
    std::string s;
    s.swap(first_fail_);
    return s;
  }

 private:
  void NoteFail(const char* what, uint32_t site, size_t mesh) {
    if (!first_fail_.empty()) return;
    char b[200];
    std::snprintf(b, sizeof b, "%s (site 0x%08X, candidate key %u)", what, site, props_ && mesh < props_->meshes.size() ? props_->meshes[mesh].key : 0u);
    first_fail_ = b;
  }

  CropFaults faults_;
  std::shared_ptr<const assets::crowd_prop_gs::PropSet> props_;
  CropStats stats_;
  std::string first_fail_;
};

}  // namespace rdr_scene
