// The shadow passes' casters, the part that needs no GPU (Session 101, lane 99 = SO; docs\renderer\SHADOW_PROMOTION_MAP.md sections 3-5, as built: docs\renderer\SHADOW_CASTERS_OURS.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. No game or SDK includes.
//
// ShadowCasters (shadow_casters_ours.h) is the DrawReplaceHook; this core answers, for one stream draw of the wrestler maps (pass site 0x82C04634) or the plane mask (0x82BF4D38): is it a caster whose
// file mesh we know, and if so the vertex + index bytes in the STREAM's shadow host layout (shadow_caster_vertex.h, lane SV), checked against the stream's own bytes. Every constant, palette row,
// target and the chain stay the stream's: the replaced draw differs from the stream's only in the buffers it reads.
//
// Pairing key (map section 4), by what the draw is:
//   a map-site draw      the wrestler hook's join of the object (ShadowJoinBoard::wrestlers_join: the file model and the body's one scale, confirmed through EVERY colour draw of the object); mesh = origin.part
//   a plane-site draw    a content join: the ring's entry 956 (the rope proxy; the ring hook's arena) or chCommon.pac entry 0 (the wrestlers' plane proxy), the mesh whose index is origin.part, counts equal and
//                        every position bit-equal to the stream's; cached per (object, part), dropped when a byte check fails
//   anything else        not ours (the evtobj props, the circles): counted, never written
// Every draw: vertex count and index count equal the mesh's; the shadow layout is one the writer knows; the first use of each built buffer is compared byte for byte with the stream's (every use
// when verify_all, every 64th live). A mismatch drops the join for that (object, part).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../assets/chcommon_proxy.h"
#include "../assets/shadow_caster_vertex.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "shadow_join_board.h"

namespace rdr_scene {

// Planted faults (map section 4). Each must fail its exact check; with no_verify the picture must move (our geometry is what is drawn).
struct ShadowFaults {
  bool palette_from_colour = false;   // R (S80 W3): the shadow draw's palette rows are overridden with the colour pass's (map A's collapsed bones are lost)
  bool fetch_order = false;           // R (S100 08:29): the dword-0 u8x4 written in the file's byte order
  bool weights_as_fill = false;       // R (ring.md B5 correction 1): the fill word where the layout has weights
  bool xdword_colour = false;         // R (ring.md B5 correction 3): the file's vertex colour in the plane proxies' dword-0 element
  bool bone_zero = false;             // every bone id 0
  bool proxy_part_shift = false;      // the chCommon join one mesh off
  bool no_verify = false;             // the byte compare against the stream is skipped (a writer fault then reaches the picture)
  bool arena_stale = false;           // R (review F1): a ring arena change does not clear the ring joins or key the built buffers: the old arena's meshes are reused (the unit test must fail)
  bool flat_indices = false;          // S101 lane 192 (a plant): the flat index rule (plain sum, no 0xFFFF between index groups) for the wrestler class; a multi-group part then differs from the draw and is refused (Mismatch)
  bool editor_exact = false;          // lane 225 (a plant): the editor class compares byte for byte like the wrestlers (the editor's float noise then refuses every part: the past state, all stream)
  bool Any() const { return editor_exact || palette_from_colour || fetch_order || weights_as_fill || xdword_colour || bone_zero || proxy_part_shift || no_verify || arena_stale || flat_indices; }
  assets::shadow_vertex::Faults Writer() const {
    assets::shadow_vertex::Faults w;
    w.fetch_order = fetch_order, w.weights_as_fill = weights_as_fill, w.xdword_colour = xdword_colour, w.bone_zero = bone_zero;
    return w;
  }
  static bool Parse(const std::string& list, ShadowFaults& f, std::string& error);   // "a,b,c"; false + error on an unknown name
};

struct ShadowOptions {
  ShadowFaults faults;
  bool verify_all = false;   // fview / the offline proof: compare the bytes on EVERY draw; live: first use, then every 64th
};

struct ShadowPrepared {
  std::vector<uint8_t> vertex;
  std::vector<uint16_t> index;
  uint64_t gpu[2] = {0, 0};
  uint64_t uses = 0;
  uint64_t last_use = 0;       // bound frame of the last draw that used it (the sweep, review F3)
  uint32_t ring_gen = 0;       // the ring arena generation it was built from (0: not a ring buffer); a buffer of an older generation is never reused (review F1)
  bool verified = false, refused = false, failed = false;
  uint64_t v_off = 0, i_off = 0;           // review SF-b: the stream offsets / byte counts the editor class last verified against (a changed one forces the verify)
  uint32_t v_bytes = 0, i_bytes = 0;
  assets::shadow_vertex::Layout layout;   // lane 225: the roles the vertex was written in (the editor class's tolerant compare reads them)
};

enum class CasterClass : uint8_t { Wrestler, Ring, Proxy, Editor, Count };   // Editor (lane 225): the Create a Superstar editor's body parts, joined by the caw hook
inline const char* CasterClassName(CasterClass c) {
  static const char* const n[] = {"wrestler", "ring", "proxy", "editor"};
  return uint8_t(c) < uint8_t(CasterClass::Count) ? n[uint8_t(c)] : "?";
}

struct ShadowStats {
  uint64_t frames_ready = 0, frames_not_ready = 0;
  uint64_t claimed = 0, replaced = 0;
  uint64_t replaced_map = 0, replaced_plane = 0;
  uint64_t by_class[uint8_t(CasterClass::Count)] = {};
  uint64_t kept[uint8_t(rdr_adapter::KeepWhy::Count)] = {};
  uint64_t kept_map_not_ours = 0, kept_plane_unjoined = 0;
  uint64_t editor_joined = 0;   // lane 225: map-site draws whose part the caw hook offered (replaced or refused after the byte check)
  uint64_t verify_runs = 0, verify_bad = 0, layout_refused = 0, rows_overridden = 0, joins_dropped = 0, prepared_built = 0;
  uint64_t plane_joined_ring = 0, plane_joined_proxy = 0;   // (object, part) joins made
  uint64_t released = 0, ring_joins_cleared = 0, plane_pair_cached_failures = 0, arena_changes = 0;   // review F1 / F3 / F8
};

class ShadowCore {
 public:
  explicit ShadowCore(const ShadowOptions& options) : options_(options) {}
  void SetProxy(std::shared_ptr<const assets::chcommon_proxy::ProxySet> p) { proxy_ = std::move(p); }
  bool has_proxy() const { return proxy_ != nullptr; }
  void SetReleaseHook(std::function<void(ShadowPrepared&)> f) { release_ = std::move(f); }
  // lane 192: the caster's index buffer = the draw's: the part's index groups joined by one 0xFFFF (a single-group part: its group, unchanged). The flat_indices plant applies to the wrestler class only.
  static std::vector<uint16_t> CasterIndices(const assets::jboy_parts::Part& part, CasterClass klass, const ShadowFaults& f);
  void SetSerialDrift(bool on) { serial_drift_ = on; }   // lane 124: plant serial_running (this core's bind number runs ahead of the other hook's)

  // Cheap, no allocation: a draw of the two shadow-pass sites with an object.
  static bool Claims(const rdr_adapter::DrawOrigin& o) {
    return (o.pass_site == rdr_adapter::kShadowMapSite || o.pass_site == rdr_adapter::kPlaneShadowSite) && o.object != 0;
  }

  // Once per bound frame (the hook's FrameReady): starts the wrestlers' / the ring's frame early (the shadow passes draw before the colour pass) and indexes the colour draws for the palette plant.
  bool BeginFrame(const rdr_adapter::LoadedFrame& frame);
  void NoClaimFrame() {
    if (++idle_ > kIdleFrames && (!prepared_.empty() || !plane_joins_.empty())) Clear();   // keyed on the claim, as the wrestlers' release is (review F3): between matches everything goes
  }

  struct Decision {
    bool ok = false;
    rdr_adapter::KeepWhy why = rdr_adapter::KeepWhy::NotOurs;
    ShadowPrepared* prepared = nullptr;
    CasterClass klass = CasterClass::Wrestler;
    uint32_t first_register = 76, rows = 0;
    const float* row_data = nullptr;   // rows * 4 floats, valid until the next Decide
    uint32_t palette_count = 0;        // lane 124: the part's palette bones (the rim map's no_pose plant writes that many identity bones)
  };
  Decision Decide(const rdr_adapter::ReplaceIn& in);
  // The hook could not make the buffers of an accepted draw.
  void UploadFailed(ShadowPrepared* p, CasterClass c, bool plane);

  const ShadowStats& stats() const { return stats_; }
  // For the unit test: the arena a plane pairing was made under (null: no pairing), the current ring generation.
  const assets::ring_gs::RingArena* JoinArena(uint32_t object, uint32_t part) const {
    const auto it = plane_joins_.find((uint64_t(object) << 32) | part);
    return it == plane_joins_.end() ? nullptr : it->second.arena.get();
  }
  uint32_t ring_generation() const { return ring_gen_; }
  size_t prepared_count() const { return prepared_.size(); }
  void Clear();
  std::string Report() const;

 private:
  struct Join {   // a plane draw's pairing, per (object, part)
    CasterClass klass = CasterClass::Proxy;
    const assets::jboy_parts::Part* part = nullptr;
    const assets::ring_gs::RingMesh* ring = nullptr;
    std::shared_ptr<const assets::ring_gs::RingArena> arena;   // keeps the ring meshes `part` / `ring` point into alive (review F1)
    uint32_t gen = 0;                                          // the arena generation it was made under
    int mesh = -1;
    uint64_t id = 0;
  };
  static constexpr uint64_t kIdleFrames = 120, kRetryFrames = 120, kUnusedFrames = 240;
  static uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 1099511628211ull; }
  void DropRingState();
  void Sweep();
  bool PlaneJoin(const rdr_adapter::ReplaceIn& in, uint32_t object, uint32_t part, Join& out, rdr_adapter::KeepWhy& why);
  Decision Finish(const rdr_adapter::ReplaceIn& in, const assets::jboy_parts::Part& part, CasterClass klass, bool plane, const assets::shadow_vertex::Options& opt, uint64_t model_id,
                  uint32_t ring_gen);

  ShadowOptions options_;
  ShadowStats stats_;
  std::shared_ptr<const assets::chcommon_proxy::ProxySet> proxy_;
  std::shared_ptr<const assets::ring_gs::RingArena> ring_;   // this frame's (null: the ring hook is off or not ready)
  bool wrestlers_ready_ = false;
  uint64_t frame_no_ = 0, bind_serial_ = 0, idle_ = 0;
  bool serial_drift_ = false;   // lane 124: plant (rim map serial_running)
  uint32_t ring_gen_ = 0;                                    // bumped whenever the ring hook's arena is another one than the last frame's (review F1)
  std::unordered_map<uint64_t, uint64_t> plane_failed_;      // (object << 32 | part) -> the bound frame before which the pairing is not tried again (review F8)
  struct BodyCache { BodyStatus status = BodyStatus::NotWrestler; ShadowBody body; };
  std::unordered_map<uint32_t, BodyCache> body_cache_;       // object -> this frame's JoinOf answer (review F4)
  std::unordered_map<uint64_t, Join> plane_joins_;           // (object << 32 | part)
  std::unordered_map<uint64_t, std::unique_ptr<ShadowPrepared>> prepared_;
  std::unordered_map<uint64_t, uint32_t> colour_draw_;       // (object << 32 | part) -> the colour-site draw (palette_from_colour only)
  std::vector<float> rows_;
  std::vector<uint8_t> vb_, ib_;
  std::function<void(ShadowPrepared&)> release_;
};

}  // namespace rdr_scene
