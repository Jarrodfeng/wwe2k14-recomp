// Our ring, the part that needs no GPU: the join of a stream draw to a ring file mesh and the per-draw checks (Session 101, lane 70 = RO; the design:
// docs\renderer\RING_PROMOTION_MAP.md sections 2-4, corrected by docs\systems\ring.md Part B; as built: docs\renderer\RING_OURS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// RingOurs (ring_ours.h) is the DrawReplaceHook the binder calls; it owns the renderer resources and calls RingCore::Decide for every claimed draw. Everything that can be wrong about WHICH
// file mesh a stream draw is, and whether it is safe to swap, lives here, so tools\ring\ring_ours_test.cpp runs the same code on the captures without a GPU. No game or SDK includes.
//
// The join (map section 2): a ring draw is made by the model-part recorder at call site 0x82C0A950. Its DrawOrigin names the model OBJECT and the PART (one object per piece, drawn once per
// instance: the instance is not part of the key). The object joins to a ring entry ONCE, by content: the candidates are the ring entries whose mesh `part` has the draw's vertex and index counts
// for EVERY part the object draws in this frame and whose positions equal the stream's bit for bit (intersection over the parts splits post 951 from 981); a last tie (skirts 973 / 974) is
// broken by the slot-0 image bytes when the owner can tell (texture_tie), else the geometry is still ours and slot 0 stays the stream's. Every draw then builds OUR vertex bytes in the stream
// draw's host layout and compares them with the stream's captured bytes (every draw in fview, the first use of each blob key live): a mismatch keeps the draw and drops the join.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../assets/ring_game_space.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "../renderer_adapter/image_content_table.h"
#include "record_io.h"

namespace rdr_scene {

// Planted faults (map section 4). Each must fail its exact check.
struct RingFaults {
  bool colour_order = false, uv_halves_swapped = false, uv_sign = false, bone_zero = false, weight_word = false;
  bool skirt_tie = false;       // every tied entry joined to the first (973 for both skirts) and its texture forced into slot 0
  bool part_only_join = false;  // the join from the lowest drawn part alone, the last entry of a tie (951 -> 981, 964 -> 982)
  bool lod_pin = false;         // every rope object taken as the high rope 960
  bool proxy_wrong = false;     // lane 137: the T_Lambert-site rope shadow proxy (entry 956) joined to the low rope 980
  bool lambert_guard_off = false, lambert_off = false;   // lane 137 review 2: the "Lambert site = the proxy only" guard removed (a re-tagged canvas draw is then replaced); the Lambert site not claimed at all (the real re-find of the pre-promotion state)
  bool no_proof_frames = false; // S101 lane 88: a verified (object, part) is never re-verified (the proof frames are off): a blob corrupted in place after its first pass must then go undetected, and the test that requires detection fails
  bool no_verify = false;       // F4: the byte compare of our vertex / index buffers against the stream's is skipped (a writer fault then reaches the picture: the proof that OUR geometry is drawn)
  bool frozen_clock = false;    // lane 115 F2: NoClaimFrame does not tick the core's clock (the 300-frame foreign verdict then never expires on frames where nothing is claimed)
  bool Any() const { return frozen_clock || no_verify || no_proof_frames ||  colour_order || uv_halves_swapped || uv_sign || bone_zero || weight_word || skirt_tie || part_only_join || lod_pin || proxy_wrong || lambert_guard_off || lambert_off; }
  assets::ring_gs::WriteFaults Writer() const {
    assets::ring_gs::WriteFaults w;
    w.colour_order = colour_order, w.uv_halves_swapped = uv_halves_swapped, w.uv_sign = uv_sign, w.bone_zero = bone_zero, w.weight_word = weight_word;
    return w;
  }
  // "a,b,c" (the fview --replace-fault list). False + error on an unknown name.
  static bool Parse(const std::string& list, RingFaults& f, std::string& error);
};

struct RingOursOptions {
  std::string game_root;
  RingFaults faults;
  bool verify_all = false;   // fview / the unit test: compare the vertex and index bytes on EVERY draw; live: the first use of each blob key
  // image record id -> width, height (null: the size check cannot be made, counted `size_unchecked`, and the stream's image stays in that slot).
  std::function<bool(uint32_t record_id, uint32_t& w, uint32_t& h)> image_info;
  // F1: a stream image record id -> its size / mips / content hash (image_content_table.h; lane 66's feed fills it live, fview / the unit test fill it from the capture). A texture slot (0 / 4 / 5) is ours ONLY
  // where this check passed: the stream image's size and mip count equal the file texture's (first `mips` mips) and its hash equals the hash of those file mips. Null / no entry: every slot stays the stream's,
  // the geometry is still ours (the picture is the same). The same feed breaks the skirt tie.
  std::function<bool(uint32_t record_id, rdr_adapter::ImageContent& out)> image_content;
};

using rdr_adapter::KeepWhy;

// lane 137 (WHATS_LEFT N9): WM30 (arena 29) draws the rope plane-shadow proxy (ring entry 956: 261 / 72 vertices, 12 instances) in the scene pass at this T_Lambert site, in the prop vertex layout,
// not at the plane-shadow site. The ring claims it here for entry 956 only.
inline constexpr uint32_t kRingLambertSite = 0x82BF80A4u;   // lane 137

// One built mesh in one stream layout. The hook puts its GPU buffers in `gpu`.
struct RingPrepared {
  std::vector<uint8_t> vertex;
  std::vector<uint16_t> index;
  uint64_t gpu[2] = {0, 0};
  bool refused = false;   // the layout / writer refused this (mesh, layout): remembered, no re-classify on every draw
  bool failed = false;    // the buffers could not be made: the draws stay the stream's (no retry on every draw)
};

class RingCore {
 public:
  explicit RingCore(const RingOursOptions& options) : options_(options) {}

  // Resets every join (object addresses are not stable across arenas). Null clears.
  void SetArena(std::shared_ptr<const assets::ring_gs::RingArena> arena, int arena_id);
  const assets::ring_gs::RingArena* arena() const { return arena_.get(); }
  int arena_id() const { return arena_id_; }
  void SetReleaseHook(std::function<void(RingPrepared&)> f) { release_ = std::move(f); }
  void SetImageInfo(std::function<bool(uint32_t, uint32_t&, uint32_t&)> f) { options_.image_info = std::move(f); }
  static int ArenaOfRecord(const SceneTables& record);   // the bgNN of the first arena asset, -1 none
  void ResetFrame() { ++frame_no_; }
  void NoClaimFrame() {   // lane 115 F2: a frame where nothing was claimed ticks the clock too (RingOurs::NoClaimFrame calls this)
    if (!options_.faults.frozen_clock) ++frame_no_;
  }

  // Cheap, no allocation: is this origin a ring candidate (the colour site, a model part, not an object already known not to be ours)?
  bool Claims(const rdr_adapter::DrawOrigin& o) const;

  struct Decision {
    bool ok = false;
    KeepWhy why = KeepWhy::NotReady;
    const char* reason = "";
    int mesh = -1;                    // index into arena()->meshes
    RingPrepared* prepared = nullptr;
    int diffuse = -1, normal_map = -1, specular = -1;   // texture indices for fragment slots 0, 4, 5 (-1: the stream's image stays)
    // The mips of OUR image in slot 0 / 4 / 5: the stream image RECORD's mip count (image_content), which must equal its fetch max level + 1 (else the slot stays the stream's). A longer chain would sample
    // different mips than the stream's on a minified rope / post (ring.md Part C).
    uint32_t diffuse_mips = 0, normal_mips = 0, specular_mips = 0;
  };
  // The hook could not make the buffers of an accepted draw: counted as kept, and remembered (no retry per draw).
  void UploadFailed(RingPrepared* p) {
    if (p) p->failed = true;
    if (stats_.replaced) --stats_.replaced;
    ++stats_.kept[uint8_t(KeepWhy::Mismatch)];
  }
  // `ps_slots`: the fragment texture slots the chosen pixel program samples.
  Decision Decide(const rdr_adapter::ReplaceIn& in, const uint8_t* ps_slots, uint32_t ps_slot_count);

  struct Stats {
    uint64_t decisions = 0, replaced = 0;
    uint64_t kept[uint8_t(KeepWhy::Count)] = {};
    uint64_t joined_content = 0, joined_tie_texture = 0, joined_tie_open = 0;   // objects (tie_open: tied, slot 0 left to the stream)
    uint64_t foreign = 0, unjoined = 0;                                          // objects: not a ring piece / ambiguous
    uint64_t bad_part = 0, bad_vertices = 0, bad_indices = 0, bad_bytes = 0, bad_view = 0, bad_size = 0, size_unchecked = 0;   // per-draw check failures
    uint64_t layout_refused = 0, joins_dropped = 0;
    uint64_t slot_kept_rope = 0, slot_kept_tie = 0, slot_kept_no_file = 0, slot_kept_tail = 0, slot_kept_levels = 0;
    uint64_t content_checks = 0, content_ok = 0, content_differs = 0, content_unknown = 0;   // F1: per slot, cached by (stream image id, generation, file texture, mips)   // slot 0 / 4 / 5 left to the stream, by reason (geometry still ours)
    uint64_t verify_runs = 0, prepared_built = 0, proof_runs = 0;   // proof_runs (S101 lane 88): the byte verifications that were a key's proof frame (of verify_runs)
  };
  const Stats& stats() const { return stats_; }
  std::string TakeFirstFail() {
    std::string s;
    s.swap(first_fail_);
    return s;
  }
  size_t prepared_count() const { return prepared_.size(); }
  void SetDropsWanted(bool on) { drops_wanted_ = on; }
  uint32_t DropSeq() const { return drop_seq_; }   // lane 129 (KC, as the arena's): objects whose join was dropped / refused / judged foreign leave the thin set with their keys
  void TakeDrops(std::vector<uint32_t>& out) { out.insert(out.end(), dropped_objects_.begin(), dropped_objects_.end()); dropped_objects_.clear(); }
  int uv_mode() const { return uv_mode_; }   // 0 unknown, 1 |u|, 2 signed
  void ReleaseAll();
  // The census text: objects joined (by content / tie), instances per piece (last full frame), LOD high / low, the check counters.
  std::string Report() const;
  // The entry an object is joined to (-1 none), for the unit test.
  int EntryOfObject(uint32_t object) const {
    const auto it = joins_.find(object);
    return it == joins_.end() || it->second.foreign ? -1 : it->second.entry;
  }

 private:
  struct ObjJoin {
    int entry = -1;
    bool foreign = false, slot0_open = false, tie_texture = false;
    uint64_t frame = 0;                       // the frame the instance counters belong to
    int32_t first_part = -1;
    uint32_t first_part_draws = 0, last_instances = 0;
    uint64_t blocked_until = 0;
    bool blocked_unjoined = false;            // blocked because the join could not be made (an unreadable blob): Unjoined, not Mismatch
  };
  struct VerKey {                             // F6: what a verified (object, part) was verified against
    uint64_t vertex = 0, index = 0;
    int mesh = -1;
  };
  bool ContentMatches(uint32_t image_id, int texture, bool force, uint32_t& mips);
  uint64_t FileHash(int texture, uint32_t mips);
  std::string last_reason_;                 // why the last failed ContentMatches failed (size / mips / hash), for the census notes
  std::set<std::string> differ_notes_;      // the first few (entry, part, slot) whose content check failed, with the reason
  const assets::ring_gs::RingMesh* MeshOf(int entry, int part) const;
  int MeshIndex(int entry, int part) const;
  bool JoinObject(const rdr_adapter::ReplaceIn& in, ObjJoin& out, const char*& reason);
  // The u-sign rule of the stream at hand, learned from its own bytes the first time a mesh with a negative file u is drawn: 1 = |u| (captures before SDK patch 109), 2 = signed (after). 0 = not known yet.
  int uv_mode_ = 0;
  bool DetectUvSign(const rdr_adapter::ReplaceIn& in, const rdr_adapter::VertexLayout& lay, const assets::ring_gs::RingMesh& m);
  RingPrepared* GetPrepared(uint64_t key, int mesh_index, const assets::wrestler_geom::Roles& roles);
  // lane 137 (review 2): the Lambert site keeps its own foreign verdicts; an object judged at one site says nothing about the other
  bool IsForeign(uint32_t object, uint32_t site = rdr_adapter::kModelPartColourSite) const {
    const auto& m = site == kRingLambertSite ? foreign_lambert_ : foreign_;
    const auto it = m.find(object);
    return it != m.end() && frame_no_ < it->second;
  }
  std::unordered_map<uint32_t, uint64_t> foreign_lambert_;   // lane 137: the same verdicts for the T_Lambert site

  RingOursOptions options_;
  std::shared_ptr<const assets::ring_gs::RingArena> arena_;
  int arena_id_ = -1;
  std::unordered_map<uint32_t, int> mesh_by_key_;   // entry << 16 | part -> mesh index
  std::unordered_map<uint32_t, ObjJoin> joins_;     // by DrawOrigin::object
  std::vector<uint32_t> dropped_objects_;             // lane 129 (KC): until TakeDrops
  uint32_t drop_seq_ = 0;
  bool drops_wanted_ = false;   // review F6: nothing is recorded for nobody (the thin binder asks)
  std::unordered_map<uint32_t, uint64_t> foreign_;   // object -> the frame its "not a ring piece" verdict expires (F5: addresses are reused)
  std::unordered_map<uint64_t, bool> content_ok_;      // (image id, generation, texture, mips) -> the hash check
  std::unordered_map<uint64_t, uint64_t> file_hash_;   // (texture, mips) -> hash of the file mips
  std::unordered_map<uint64_t, std::unique_ptr<RingPrepared>> prepared_;   // (mesh, layout signature)
  std::unordered_map<uint64_t, VerKey> verified_key_;                        // (object, part) -> the keys and mesh whose bytes passed (F6)
  std::function<void(RingPrepared&)> release_;
  std::string first_fail_;
  uint64_t frame_no_ = 0;
  Stats stats_;
};

}  // namespace rdr_scene
