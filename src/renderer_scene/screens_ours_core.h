// The stage screens, the part that needs no GPU: the join of a stream draw at pass site 0x82C0F4BC to a mesh of the arena's pac keys 800-809, and the per-draw checks
// (S101 lane 115 = SO; docs\renderer\UNCLAIMED_MODEL_PARTS.md section 3.3; as built: docs\renderer\SCREENS_OURS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// A copy of PropsCore's design (S101 lane 84) on the screens' file side, GEOMETRY ONLY: no texture is read or replaced (every slot, including the run-time video planes in 7 / 8 / 9, is the
// stream's). ScreensOurs (screens_ours.h) is the DrawReplaceHook; everything that can be wrong about WHICH file mesh a stream draw is lives here, so tools\screens\screens_ours_test.cpp
// runs the same code on the captures without a GPU. No game or SDK includes.
//
// The join (once per object): the candidates are the keys whose mesh `part` has the draw's vertex and index counts for EVERY part the object draws in this frame and whose element-0
// positions equal the stream's bit for bit. Pairs with identical positions (801 / 802, 803 / 804, 806 / 807) are told apart by CONTENT: the full vertex buffer our writer produces for each
// candidate is compared with the stream's bytes and the candidate whose bytes (and index buffer) equal is taken; when several are byte-equal they are interchangeable geometry and the
// lowest key is taken (counted `tie_interchangeable`). Every draw then compares our vertex bytes and index buffer with the stream's (every draw in fview, the first use of each blob key
// live). The stream's program must be a yMovieDefault one while the file names yBG_Default: the per-key exception for keys 800-809 (any other pairing is refused as a Variant).
// The u-sign rule is learned from the stream (the ring's, DetectUvSign), never assumed.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../assets/screen_game_space.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "record_io.h"
#include "ring_ours_core.h"   // RingPrepared (the same prepared buffer)

namespace rdr_scene {

inline constexpr uint32_t kModelPartScreensSite = 0x82C0F4BCu;   // sub_82C0CA08: the stage screens' pass (UNCLAIMED_MODEL_PARTS.md 1.2)

// Planted faults (each must fail its exact check).
struct ScreensFaults {
  bool colour_order = false, uv_halves_swapped = false, uv_sign = false;
  bool pair_swap = false;   // a tied pair joined to its LAST member without the content check (801 joined as 802)
  bool no_verify = false;   // the byte compare of our buffers against the stream's is skipped (a writer / join fault then reaches the picture)
  bool uv_from_pair = false;   // lane 115 F1: the u-sign rule learned from the first candidate of an unresolved pair even when its sign bit is clear (the old defect: 806's file u is negative where 807's is +1e-10)
  bool class_refuse = false;   // the stream's fragment name forced to yDefault_base_fragment: every draw must be kept, class refused > 0
  bool file_effect = false;    // the file's effect read as a non-yBG one: every draw must be kept, class refused > 0
  bool stale_switch = false;   // SetScreens keeps the joins, the foreign verdicts, the u rule and the buffers of the arena before (the arena_switch plant)
  bool frozen_clock = false;   // NoClaimFrame does not tick the core's clock (lane 115 F2: the 300-frame foreign verdict never expires on frames where nothing is claimed)
  bool Any() const { return colour_order || uv_halves_swapped || uv_sign || pair_swap || no_verify || uv_from_pair || class_refuse || file_effect || stale_switch || frozen_clock; }
  assets::ring_gs::WriteFaults Writer() const {
    assets::ring_gs::WriteFaults w;
    w.colour_order = colour_order, w.uv_halves_swapped = uv_halves_swapped, w.uv_sign = uv_sign;
    return w;
  }
  static bool Parse(const std::string& list, ScreensFaults& f, std::string& error);
};

struct ScreensOursOptions {
  std::string game_root;
  ScreensFaults faults;
  bool verify_all = false;   // fview / the unit test: compare the vertex and index bytes on EVERY draw; live: the first use of each blob key
};

using ScreensPrepared = RingPrepared;

class ScreensCore {
 public:
  explicit ScreensCore(const ScreensOursOptions& options) : options_(options) {}

  // Resets every join (object addresses are not stable across arenas). Null clears.
  void SetScreens(std::shared_ptr<const assets::screen_gs::ScreenSet> set);
  const assets::screen_gs::ScreenSet* screens() const { return set_.get(); }
  void SetReleaseHook(std::function<void(ScreensPrepared&)> f) { release_ = std::move(f); }
  static int ArenaOfRecord(const SceneTables& record) { return RingCore::ArenaOfRecord(record); }
  void ResetFrame() { ++frame_no_; }   // a frame where a claimed draw was asked (FrameReady)
  void NoClaimFrame() {                // lane 115 F2: a frame where nothing was claimed ticks the clock too (the hook calls this from its NoClaimFrame)
    if (!options_.faults.frozen_clock) ++frame_no_;
  }

  bool Claims(const rdr_adapter::DrawOrigin& o) const;

  struct Decision {
    bool ok = false;
    rdr_adapter::KeepWhy why = rdr_adapter::KeepWhy::NotReady;
    const char* reason = "";
    int mesh = -1;   // index into screens()->meshes
    ScreensPrepared* prepared = nullptr;
  };
  void UploadFailed(ScreensPrepared* p) {
    if (p) p->failed = true;
    if (stats_.replaced) --stats_.replaced;
    ++stats_.kept[uint8_t(rdr_adapter::KeepWhy::Mismatch)];
  }
  // `ps_name`: the stream's fragment program name (rdr::fx::kVariants[in.ps].name; null when the draw has none). The vertex program is the generic yDefault_base_vertex, so the class is read from the fragment.
  Decision Decide(const rdr_adapter::ReplaceIn& in, const char* ps_name);

  struct Stats {
    uint64_t decisions = 0, replaced = 0;
    uint64_t kept[uint8_t(rdr_adapter::KeepWhy::Count)] = {};
    uint64_t joined_unique = 0, joined_tie_content = 0, joined_tie_interchangeable = 0;   // objects
    uint64_t foreign = 0, unjoined = 0, bad_class = 0, uv_learned_from_bytes = 0;
    uint64_t bad_part = 0, bad_vertices = 0, bad_indices = 0, bad_bytes = 0;
    uint64_t layout_refused = 0, joins_dropped = 0;
    uint64_t verify_runs = 0, prepared_built = 0, lookalike = 0;
  };
  const Stats& stats() const { return stats_; }
  std::string TakeFirstFail() {
    std::string s;
    s.swap(first_fail_);
    return s;
  }
  size_t prepared_count() const { return prepared_.size(); }
  int uv_mode() const { return uv_mode_; }
  void ReleaseAll();
  std::string Report() const;
  // The key an object is joined to (-1 none), for the unit test.
  int KeyOfObject(uint32_t object) const {
    const auto it = joins_.find(object);
    return it == joins_.end() || it->second.foreign ? -1 : it->second.key;
  }

 private:
  struct ObjJoin {
    int key = -1;
    std::vector<int> cands;   // every key whose positions equal the stream's (more than one: a pair, resolved by content at the first draw)
    bool foreign = false, resolved = false;
    uint64_t blocked_until = 0;
    bool blocked_unjoined = false;
  };
  struct VerKey {
    uint64_t vertex = 0, index = 0;
    int mesh = -1;
  };
  int MeshIndex(int key, int part) const;
  const assets::ring_gs::RingMesh* MeshOf(int key, int part) const {
    const int i = MeshIndex(key, part);
    return i < 0 ? nullptr : &set_->meshes[size_t(i)];
  }
  bool JoinObject(const rdr_adapter::ReplaceIn& in, ObjJoin& out, const char*& reason);
  // Our buffers for mesh `mi` in the draw's layout (cached); null: the layout / writer refused it.
  ScreensPrepared* Prepare(const rdr_adapter::ReplaceIn& in, const rdr_adapter::VertexLayout& lay, int mi, int rule);   // rule: 1 = |u|, 2 = signed (0 = |u|, for a mesh with no negative u)
  int uv_mode_ = 0;   // 0 unknown, 1 |u|, 2 signed: learned from the stream's own u half (the ring's rule)
  // The rule from the stream's own u half at the mesh's first negative-u vertex. A SET sign bit proves "signed" from any mesh, candidate of a pair or not; a CLEAR bit proves "|u|" only when
  // `resolved` (the stream draws THIS mesh): an unresolved pair candidate's vertex may hold the other member's u (806's file u is negative where 807's is +1e-10): lane 115 F1.
  bool DetectUvSign(const rdr_adapter::ReplaceIn& in, const rdr_adapter::VertexLayout& lay, const assets::ring_gs::RingMesh& m, bool resolved);
  bool IsForeign(uint32_t object) const {
    const auto it = foreign_.find(object);
    return it != foreign_.end() && frame_no_ < it->second;
  }

  ScreensOursOptions options_;
  std::shared_ptr<const assets::screen_gs::ScreenSet> set_;
  std::unordered_map<uint32_t, int> mesh_by_key_;   // key << 16 | part -> mesh index
  std::unordered_map<uint32_t, ObjJoin> joins_;
  std::unordered_map<uint32_t, uint32_t> byte_fails_;   // object -> byte-compare failures since its last good join: a second one makes it foreign
  std::unordered_map<uint32_t, uint64_t> foreign_;      // object -> the frame its "not a screen of ours" verdict expires (addresses are reused)
  std::unordered_map<uint64_t, std::unique_ptr<ScreensPrepared>> prepared_;
  std::unordered_map<uint64_t, VerKey> verified_key_;
  std::function<void(ScreensPrepared&)> release_;
  std::string first_fail_, detail_;   // detail_: where the last failed byte compare differed
  uint64_t frame_no_ = 0;
  Stats stats_;
};

}  // namespace rdr_scene
