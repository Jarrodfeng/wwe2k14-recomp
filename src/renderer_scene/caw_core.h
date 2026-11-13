// The Create a Superstar editor's wrestler, drawn by us in place (Session 101, lane 153 = N3; docs\renderer\CAW_OURS.md). Header only, GPU-free: the decisions the hook
// (caw_ours.cpp) uploads and the offline checker (tools\wrestler\caw_ours_check.cpp) proves. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// WHY A CLASS OF ITS OWN (the cause, logged before this was written; CAW_OURS.md section 1): the editor's body is ONE stream object (12 parts) whose parts come from separate
// per-region files of pac\edit\edNN.pac (leaves such as 2001 = parts 0-2, 2002 = 3-4, 2003..2006 = 5-8, 2010 = 11). No chNNN.pac model has those counts, so the wrestlers' class
// (WrestlerCore: the object's drawn parts must fit ONE roster model) passes every one of its draws on as NotOurs, and the editor writes no scene record in the captures we hold.
// This class joins PART BY PART: a stream draw's (vertices, indices) pick the edit leaves' meshes with those counts, the stream's own vertex bytes confirm one (the file's strip
// rebuilt in the stream draw's host layout, the one position scale measured, every element compared: exactly the wrestlers' proof), and the draw is replaced in place by our
// geometry. Everything else stays the stream's, including the bone palette (the stream's own rows c76 ..: no scene record, no override; rows = 0).
//
// THE ONLY ANSWERS (review B1): a draw whose part key was never byte-confirmed gets NotOurs, whatever the reason (counts fit but the bytes do not, a leaf still loading, a layout this
// code cannot rebuild). The binder treats every other refusal as final, and the edit index holds 11,079 meshes, so a count-only fit says nothing: ring / props / arena draws of a MATCH
// must go on to their own hooks untouched. Unjoined / Mismatch / Variant are answered only for a part that WAS confirmed once and has since stopped fitting.
//
// STALE MODELS (the CAW bug class, review S1): the stream's geometry is content-addressed (a changed vertex array has a new offset, live a new GeoStore key; captures are append-only).
// A joined part is therefore re-checked in full only when its identity changes: vertex / index offset and byte count, or the layout. Any change takes the full byte check on that very
// draw; a part whose bytes no longer fit is un-joined (stale caught) and re-joined by bytes at once if some leaf fits, else left the stream's. PLANT `pinned`: the identity ignores the
// offsets (counts only), so a part that moved to a new offset with new bytes keeps drawing the old model: the past bug, a pin that does not see a new offset.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../assets/wrestler_geom.h"
#include "../assets/wrestler_roster.h"
#include "../renderer_adapter/capture_reader.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "wrestler_core.h"   // WrestlerPrepared, WrestlerCore::ReadGeometry

namespace rdr_scene {

// Review S5 / plant (f): the part key keeps the whole part index (a part above 255 must not collide with a low one).
constexpr uint64_t CawKey(uint32_t object, uint32_t part) { return uint64_t(object) << 32 | part; }
static_assert(CawKey(1, 300) != CawKey(1, 44) && CawKey(2, 0) != CawKey(1, 0), "the part key must not truncate the part index");

struct CawFaults {
  bool pinned = false;        // PLANT: the identity check ignores offsets (a pin that does not see a new offset): a changed part keeps the old model
  bool no_verify = false;     // PLANT: the byte check is off (a positive control for the independent proof)
  bool wrong_leaf = false;    // PLANT: the first count-fitting leaf is taken without the byte confirmation
  bool bg_wrong_source = false;   // lane 169 PLANT: a backdrop piece is joined to the OTHER bg66.pac entry, bytes unconfirmed (a wrong source entry)
  bool frozen_scroll = false;     // lane 169 PLANT (hook level, caw_ours.cpp): the backdrop's Time constant is frozen at the editor's first tick (0), the scroll stops
  bool wrong_picture = false;     // lane 169 PLANT (hook level): a backdrop piece gets the OTHER piece's picture in slot 0
  bool fill0_strict = false;  // lane 179 PLANT: fill0 is compared bit-exactly (a vertex near x = 0 moved by 1e-5 flips the byte: the editor's part 9 stays the stream's)
  bool scale_one = false;     // lane 179 PLANT (review): the RIGHT leaf is joined but built at scale 1 (counts fit, bytes wrong): with no_verify the independent proof must see wrong_vertex
  bool index_swap = false;    // lane 179 PLANT (review): the right leaf with two neighbouring different indices swapped (counts fit, strips wrong): with no_verify the proof must see wrong_index
  bool flat_indices = false;  // lane 179 PLANT: the draw's index count ignores the restart markers between index groups (the count-only fit): the editor's part 4 joins the WRONG leaf (ed22) or none
  bool count_only = false;    // PLANT (review B1): answer Unjoined for any count fit, even a part never confirmed: ring / props draws of a match are taken away
  static bool Parse(const std::string& list, CawFaults& f, std::string& error) {
    size_t at = 0;
    while (at <= list.size()) {
      size_t comma = list.find(',', at);
      if (comma == std::string::npos) comma = list.size();
      const std::string w = list.substr(at, comma - at);
      at = comma + 1;
      if (w.empty()) continue;
      if (w == "pinned") f.pinned = true;
      else if (w == "no_verify") f.no_verify = true;
      else if (w == "wrong_leaf") f.wrong_leaf = true;
      else if (w == "count_only") f.count_only = true;
      else if (w == "fill0_strict") f.fill0_strict = true;
      else if (w == "flat_indices") f.flat_indices = true;
      else if (w == "scale_one") f.scale_one = true;
      else if (w == "index_swap") f.index_swap = true;
      else if (w == "bg_wrong_source") f.bg_wrong_source = true;
      else if (w == "frozen_scroll") f.frozen_scroll = true;
      else if (w == "wrong_picture") f.wrong_picture = true;
      else return error = "unknown caw fault '" + w + "' (pinned, no_verify, wrong_leaf, count_only, bg_wrong_source, frozen_scroll, wrong_picture, fill0_strict, flat_indices, scale_one, index_swap)", false;
    }
    return true;
  }
};

struct CawStats {
  uint64_t claimed = 0, replaced = 0, not_caw = 0, loading = 0;
  uint64_t unjoined = 0, refused_layout = 0, refused_bytes = 0, refused_other = 0;
  uint64_t joins = 0, rejoins = 0, stale_caught = 0, verify_runs = 0, identity_hits = 0, built = 0, upload_failed = 0, released = 0;
  uint64_t frames_ready = 0, frames_not_ready = 0;
  uint64_t backdrop_replaced = 0;   // lane 169 (N3b): replaced draws that are backdrop pieces (bg66.pac), a subset of `replaced`
  uint64_t loading_first = 0;   // lane 243: of `unconfirmed`, the draws whose leaf was still loading (a part's first sight)
  uint64_t unconfirmed = 0;   // count fits answered NotOurs because the part was never byte-confirmed (a match capture: thousands, all passed on)
};

struct CawDecision {
  WrestlerPrepared* prepared = nullptr;
  uint32_t leaf_group = 0, mesh = 0;
  bool backdrop = false;                              // lane 169: a backdrop piece (bg66.pac), not a body part
  uint32_t src_pac = 0, src_pach = 0, src_leaf = 0;   // lane 169: the file the join was confirmed against (pac 1066 = pac\bg\bg66.pac)
};

class CawCore {
 public:
  using Model = assets::wrestler_roster::Model;
  CawCore(assets::wrestler_roster::Roster& roster, const CawFaults& faults, bool sync) : roster_(roster), faults_(faults), sync_(sync) {}
  ~CawCore() {
    {
      std::lock_guard<std::mutex> lock(lmu_);
      stop_ = true;
    }
    lcv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }
  CawCore(const CawCore&) = delete;
  CawCore& operator=(const CawCore&) = delete;
  void SetReleaseHook(std::function<void(WrestlerPrepared&)> f) { release_ = std::move(f); }
  const CawStats& stats() const { return stats_; }
  size_t resident() const { return parts_.size(); }
  std::vector<std::string> notes;   // why a candidate leaf did not join (capped; the checker prints them)
  // Review S5 / plant (f): the part key keeps the whole part index (a part above 255 must not collide with a low one).
  static uint64_t Key(uint32_t object, uint32_t part) { return CawKey(object, part); }

  // Once per bound frame. False: the edit-part index is not ready (every claimed draw stays the stream's).
  bool BeginFrame(const rdr_adapter::LoadedFrame& frame) {
    ++frame_no_;
    idle_ = 0;
    if (roster_.Poll() != 1 || !(frame.header.flags & rdr_adapter::kFrameSixteenPairsHighFirst)) return ++stats_.frames_not_ready, false;
    if (!indexed_) {
      for (size_t i = 0; i < roster_.edit_parts().size(); ++i) {
        const auto& ep = roster_.edit_parts()[i];
        by_counts_[uint64_t(ep.vertices) << 32 | ep.index_count].push_back(i);
      }
      indexed_ = true;
    }
    ++stats_.frames_ready;
    if (frame_no_ % 120 == 0) Sweep();
    return true;
  }

  bool Decide(const rdr_adapter::LoadedFrame& frame, uint32_t draw_index, const WrestlerCore::ReadGeometry& read, CawDecision& out, rdr_adapter::KeepWhy& why) {
    using rdr_adapter::KeepWhy;
    ++stats_.claimed;
    const rdr_adapter::DrawOrigin* o = frame.OriginOf(draw_index);
    if (!o) return why = KeepWhy::NotOurs, ++stats_.not_caw, false;
    const rdr_adapter::Draw& dr = frame.draws[draw_index];
    const uint64_t counts = uint64_t(dr.vertex_count) << 32 | dr.index_count;
    cands_.clear();   // lane 179: candidates by (vertices, index count - k) for k = 0..3: a mesh of k + 1 groups is drawn with k restart markers (JoinPart checks the part's group count)
    for (uint32_t k = 0; k < (faults_.flat_indices ? 1u : 4u) && dr.index_count > k; ++k) {
      auto f = by_counts_.find(uint64_t(dr.vertex_count) << 32 | (dr.index_count - k));
      if (f != by_counts_.end()) cands_.insert(cands_.end(), f->second.begin(), f->second.end());
    }
    if (cands_.empty()) return why = KeepWhy::NotOurs, ++stats_.not_caw, false;   // no edit leaf mesh has these counts: not the editor's body
    const uint64_t key = Key(o->object, o->part);
    auto pit = parts_.find(key);
    const bool known = pit != parts_.end() && pit->second.joined_once;   // confirmed byte for byte at least once
    // The only way to refuse a part never confirmed: NotOurs (review B1). A part that was ours and stopped fitting answers `w`.
    auto refuse = [&](KeepWhy w, uint64_t& counter) {
      if (known || faults_.count_only) return ++counter, why = w, false;   // answered: a part that WAS ours (or the count_only plant)
      return why = KeepWhy::NotOurs, ++stats_.not_caw, ++stats_.unconfirmed, false;
    };
    if (dr.index_32bit || !dr.index_bytes || !dr.vertex_bytes || (dr.flags & (rdr_adapter::kDrawInline | rdr_adapter::kDrawThin)) || dr.layout >= frame.layouts.count) return refuse(KeepWhy::Variant, stats_.refused_layout);
    const rdr_adapter::VertexLayout& lay = frame.layouts[dr.layout];
    if (uint64_t(dr.vertex_bytes) < uint64_t(dr.vertex_count) * lay.host_stride) return refuse(KeepWhy::Variant, stats_.refused_layout);   // the guard the byte compare relies on
    PartState& ps = parts_[key];
    ps.last_frame = frame_no_;
    const uint64_t lsig = LayoutSig(frame, lay);
    // 1. identity: the joined part's offsets, byte counts and layout are the ones its last full check passed against
    if (ps.model && ps.cur && Identity(dr, lsig, ps)) {
      ++stats_.identity_hits, ++stats_.replaced;
      out.prepared = ps.cur, out.leaf_group = ps.group, out.mesh = ps.mesh, out.backdrop = ps.backdrop, out.src_pac = ps.src_pac, out.src_pach = ps.src_pach, out.src_leaf = ps.src_leaf;
      if (ps.backdrop) ++stats_.backdrop_replaced;
      return true;
    }
    // 2. something moved (or first sight): the full byte check on this very draw
    std::vector<uint8_t> vb, ib;
    if (!read || !read(dr.vertex_offset, dr.vertex_bytes, vb) || vb.size() < dr.vertex_bytes) return refuse(KeepWhy::Mismatch, stats_.refused_other);
    if (ps.model) {
      if (!VerifyPart(frame, dr, lay, vb, ps)) {
        ++stats_.stale_caught;
        DropJoin(ps);
      }
    }
    if (!ps.model) {
      if (ps.tried_frame && frame_no_ - ps.tried_frame < kRetryFrames && ps.tried_vi == counts) return refuse(KeepWhy::Unjoined, stats_.unjoined);
      bool pending = false;
      auto index_ok = [&](const std::vector<uint16_t>& idx) {
        if (faults_.no_verify) return true;
        if (dr.index_bytes != idx.size() * 2 || !read(dr.index_offset, dr.index_bytes, ib) || ib.size() != idx.size() * 2) return false;
        return idx.empty() || !std::memcmp(ib.data(), idx.data(), ib.size());
      };
      if (!JoinPart(frame, dr, lay, vb, cands_, index_ok, ps, pending)) {
        if (pending) {
          if (!known) ++stats_.loading_first;   // lane 243 (lane 212's proposal): a never-confirmed part whose leaf is still loading answers NotOurs; counted here so "not caw" minus this reads "not the editor's body"
          return refuse(KeepWhy::Unjoined, stats_.loading);
        }
        ps.tried_frame = frame_no_, ps.tried_vi = counts;
        return refuse(KeepWhy::Unjoined, stats_.unjoined);
      }
      ++stats_.joins;
      ps.joined_once = true;
    }
    if (!VerifyIndex(read, dr, ps, ib)) {
      DropJoin(ps);
      ps.tried_frame = frame_no_, ps.tried_vi = counts;   // lane 179 (review): back off, the join is not redone at every frame of a part whose strips do not fit
      return refuse(KeepWhy::Mismatch, stats_.refused_bytes);
    }
    ps.vertex_offset = dr.vertex_offset, ps.vertex_bytes = dr.vertex_bytes, ps.index_offset = dr.index_offset, ps.index_bytes = dr.index_bytes, ps.layout_sig = lsig, ps.counts = counts;
    ++stats_.replaced;
    out.prepared = ps.cur, out.leaf_group = ps.group, out.mesh = ps.mesh, out.backdrop = ps.backdrop, out.src_pac = ps.src_pac, out.src_pach = ps.src_pach, out.src_leaf = ps.src_leaf;
    if (ps.backdrop) ++stats_.backdrop_replaced;
    return true;
  }

  // lane 225: the shadow passes and the rim map draw the editor's body parts too (same object, same part numbers): a part this class has byte-confirmed and still holds (a body part,
  // not a backdrop piece) is offered to them with its leaf mesh and fitted scale. Read only; render thread (the one Decide runs on).
  bool JoinedPart(uint32_t object, uint32_t part, std::shared_ptr<Model>& model, uint32_t& mesh, uint32_t& group, float& scale) const {
    const auto it = parts_.find(Key(object, part));
    if (it == parts_.end()) return false;
    const PartState& ps = it->second;
    if (!ps.model || !ps.cur || !ps.joined_once || ps.backdrop || !(ps.scale > 0)) return false;
    if (frame_no_ - ps.last_frame > 2) return false;   // review SF-b: freshness. Only a part the colour pass claimed in the last bind or two is offered: an old editor key (a match after the editor, a reused address) is not
    model = ps.model, mesh = ps.mesh, group = ps.group, scale = ps.scale;
    return true;
  }
  // review SF-b: binds where nothing was claimed (CawOurs::NoClaimFrame): after kIdleBinds every joined part goes, as ShadowCore does.
  void IdleFrame() {
    if (++idle_ > kIdleBinds && !parts_.empty()) Clear();
  }
  void UploadFailed() { ++stats_.upload_failed; if (stats_.replaced) --stats_.replaced; }
  void Clear() {
    for (auto& kv : parts_) DropJoin(kv.second);
    parts_.clear();
  }
  std::string Report() const {
    const CawStats& s = stats_;
    char b[800];
    std::snprintf(b, sizeof b, "caw: claimed %llu replaced %llu | not caw %llu loading %llu unjoined %llu refused layout %llu bytes %llu other %llu | joins %llu stale caught %llu verify %llu identity hits %llu built %llu released %llu upload failed %llu | parts resident %zu | unconfirmed (answered NotOurs) %llu (leaf still loading %llu) | frames ready %llu not ready %llu",
                  (unsigned long long)s.claimed, (unsigned long long)s.replaced, (unsigned long long)s.not_caw, (unsigned long long)s.loading, (unsigned long long)s.unjoined,
                  (unsigned long long)s.refused_layout, (unsigned long long)s.refused_bytes, (unsigned long long)s.refused_other, (unsigned long long)s.joins, (unsigned long long)s.stale_caught,
                  (unsigned long long)s.verify_runs, (unsigned long long)s.identity_hits, (unsigned long long)s.built, (unsigned long long)s.released, (unsigned long long)s.upload_failed, parts_.size(),
                  (unsigned long long)s.unconfirmed, (unsigned long long)s.loading_first, (unsigned long long)s.frames_ready, (unsigned long long)s.frames_not_ready);
    return b;
  }

 private:
  static constexpr uint64_t kRetryFrames = 60, kIdleSweep = 240, kIdleBinds = 120;
  static constexpr size_t kMaxLayouts = 6;   // prepared buffers kept per part (review S6)
  // The editor's positions are the file's times one body scale (96/95 in the capture) plus the editor's own morph blend at its resting sliders: up to ~2e-4 units from the file
  // (CAW_OURS.md section 3, measured by `caw_ours_check --fit`). The class's position tolerance is 5e-4 (absolute below 1, relative above): invisible (rule 6: not byte-identical, the bar is
  // owned_check's tolerance), and still far below any slider step. Every other element stays bit-exact / one field step, as the wrestlers'.
  static constexpr float kCawPosTolerance = 5e-4f;
  static constexpr double kScaleResidual = 2e-3;
  struct PartState {
    std::shared_ptr<Model> model;   // the leaf (the edit part's file) the bytes confirmed
    uint32_t mesh = 0, group = 0;
    bool backdrop = false;                       // lane 169 (N3b): the part is a backdrop piece of pac\bg\bg66.pac (CAS_wall / CAS_moya), not a body part
    uint32_t src_pac = 0, src_pach = 0, src_leaf = 0;   // lane 169: where the byte-confirmed join came from (the checker prints it)
    float scale = 0;
    bool joined_once = false;       // byte-confirmed at least once: only then may this class answer anything but NotOurs
    uint64_t counts = 0, tried_vi = 0, vertex_offset = 0, index_offset = 0, layout_sig = 0;
    uint32_t vertex_bytes = 0, index_bytes = 0;
    uint64_t last_frame = 0, tried_frame = 0;
    std::map<uint64_t, std::unique_ptr<WrestlerPrepared>> prepared;   // by roles signature (layout + scale + fill0): one per layout the part has been drawn in
    WrestlerPrepared* cur = nullptr;                                 // the one the last full check passed against
  };

  // lane 179: a mesh with SEVERAL index groups is drawn as ONE strip with a restart marker (0xFFFF) between the groups, so the draw's index count is the groups' sum + (groups - 1). The editor's
  // part 4 (ed02 leaf 2002 mesh 1: groups 685 + 678, draw count 1364) is the only multi-group part in the body; the count-only fit (sum 1364) found the WRONG leaf (ed22, 468 / 1364) whose
  // vertices are in another order and whose strips differ, which is why the byte check failed for it. The restart rule is measured: stream index = group 0, 0xFFFF, group 1, value for value.
  uint32_t DrawIndexCount(const assets::jboy_parts::Part& p) const { return assets::wrestler_geom::DrawIndexCount(p, faults_.flat_indices); }   // (PLANT flat_indices: the count-only rule)
  static std::vector<uint16_t> JoinedIndices(const assets::jboy_parts::Part& p) { return assets::wrestler_geom::BuildIndices(p); }

  static uint64_t LayoutSig(const rdr_adapter::LoadedFrame& frame, const rdr_adapter::VertexLayout& lay) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(lay.host_stride), mix(lay.guest_stride), mix(lay.element_count);
    for (uint32_t e = 0; e < lay.element_count && lay.element_first + e < frame.elements.count; ++e) {
      const rdr_adapter::VertexElement& el = frame.elements[lay.element_first + e];
      mix(el.vk_format), mix(el.host_offset), mix(el.source_format), mix(el.components), mix(el.flags), mix(el.guest_offset);
    }
    return h;
  }

  bool Identity(const rdr_adapter::Draw& dr, uint64_t lsig, const PartState& ps) const {
    if (faults_.pinned) return ps.counts == (uint64_t(dr.vertex_count) << 32 | dr.index_count);   // PLANT: the pin that does not see a new offset
    return ps.vertex_offset == dr.vertex_offset && ps.vertex_bytes == dr.vertex_bytes && ps.index_offset == dr.index_offset && ps.index_bytes == dr.index_bytes && ps.layout_sig == lsig &&
           ps.counts == (uint64_t(dr.vertex_count) << 32 | dr.index_count);
  }

  // lane 179: mode 2 is "the high byte of the position x float" and it is read off the STREAM's own vertex 0 (its x, not the file's x times a scale): a vertex near x = 0 that the editor moved by
  // 1e-5 has another exponent than the file's, so the file's x gave a false "unknown" on the editor's part 9. `scale` / `part` stay in the signature for the callers.
  static uint8_t Fill0Mode(const assets::wrestler_geom::Roles& roles, const std::vector<uint8_t>& vb, float /*scale*/, const assets::jboy_parts::Part& /*part*/) {
    int pos_offset = -1;
    for (const auto& r : roles.r) if (r.role == assets::wrestler_geom::Role::Pos) pos_offset = int(r.offset);
    for (const auto& r : roles.r) {
      if (r.role != assets::wrestler_geom::Role::Fill0) continue;
      if (vb.size() < size_t(r.offset) + 4) return 3;
      if (vb[r.offset] != 0 || vb[size_t(r.offset) + 1] != 0 || vb[size_t(r.offset) + 2] != 0) return 3;
      const uint8_t b3 = vb[size_t(r.offset) + 3];
      if (b3 == 255) return 1;
      if (pos_offset < 0 || vb.size() < size_t(pos_offset) + 4) return 3;
      uint32_t xb;
      std::memcpy(&xb, vb.data() + pos_offset, 4);
      return uint8_t(xb >> 24) == b3 ? 2 : 3;
    }
    return 0;
  }

  bool Classify(const rdr_adapter::LoadedFrame& frame, const rdr_adapter::VertexLayout& lay, const assets::jboy_parts::Part& part, assets::wrestler_geom::Roles& roles) const {
    std::array<assets::wrestler_geom::LayoutElement, 16> els;
    if (lay.element_count == 0 || lay.element_count > els.size()) return false;
    for (uint32_t e = 0; e < lay.element_count; ++e) {
      const rdr_adapter::VertexElement& el = frame.elements[lay.element_first + e];
      els[e] = {el.vk_format, el.host_offset, el.source_format, el.components, el.flags, el.guest_offset};
    }
    return assets::wrestler_geom::Classify(part, els.data(), lay.element_count, lay.host_stride, lay.guest_stride, roles, /*allow_short_maps=*/true, false);
  }

  static uint64_t Sig(const assets::wrestler_geom::Roles& roles, uint32_t scale_bits, uint8_t fill0) {
    uint64_t sig = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
    mix(roles.host_stride);
    for (const auto& r : roles.r) mix((uint64_t(uint8_t(r.role)) << 32) | (uint64_t(r.offset) << 16) | r.vk);
    mix(scale_bits), mix(fill0);
    return sig;
  }

  // ---- the leaf loader: ONE worker, requests batched and sorted by pac so a pac is inflated once per batch (review S2) ----
  struct LeafSlot { std::shared_ptr<Model> model; int state = 0; };   // 0 none, 1 queued, 2 loaded, 3 failed
  std::shared_ptr<Model> LeafOf(size_t edit_part, bool& pending) {
    pending = false;
    const uint32_t group = roster_.edit_parts()[edit_part].group;
    std::unique_lock<std::mutex> lock(lmu_);
    LeafSlot& m = leaves_[group];
    if (m.state == 2) return m.model;
    if (m.state == 3) return nullptr;
    if (m.state == 0) {
      m.state = 1;
      if (sync_) {
        lock.unlock();
        std::string err;
        std::shared_ptr<Model> loaded = roster_.LoadEditLeaf(edit_part, err);
        lock.lock();
        LeafSlot& s2 = leaves_[group];
        s2.model = loaded, s2.state = loaded ? 2 : 3;
        return s2.model;
      }
      lq_.push_back(edit_part);
      if (!worker_.joinable()) worker_ = std::thread([this] { Work(); });
      lcv_.notify_one();
    }
    pending = true;
    return nullptr;
  }
  void Work() {
    for (;;) {
      std::vector<size_t> batch;
      {
        std::unique_lock<std::mutex> lock(lmu_);
        lcv_.wait(lock, [this] { return stop_ || !lq_.empty(); });
        if (stop_) return;
        batch.swap(lq_);
      }
      std::sort(batch.begin(), batch.end(), [this](size_t a, size_t b) {
        const auto& x = roster_.edit_parts()[a];
        const auto& y = roster_.edit_parts()[b];
        return x.pac != y.pac ? x.pac < y.pac : x.pach < y.pach;
      });
      for (size_t ci : batch) {
        std::string err;
        std::shared_ptr<Model> loaded;
        try { loaded = roster_.LoadEditLeaf(ci, err); } catch (...) { loaded.reset(); }
        std::lock_guard<std::mutex> lock(lmu_);
        LeafSlot& s = leaves_[roster_.edit_parts()[ci].group];
        s.model = loaded, s.state = loaded ? 2 : 3;
      }
      roster_.DropEditCache();   // the batch is done: the open pac (tens of MB inflated) goes
    }
  }

  // Build our part from `part` for the draw's layout and compare it with the stream's bytes. Fills `p` on success.
  bool BuildAndCompare(const rdr_adapter::LoadedFrame& frame, const rdr_adapter::VertexLayout& lay, const std::vector<uint8_t>& vb, const assets::jboy_parts::Part& part, float scale,
                       bool fit_scale, WrestlerPrepared& p, float& scale_out, uint8_t& fill0_out, uint64_t& sig_out) const {
    assets::wrestler_geom::Roles roles;
    last_why_ = "";
    if (!Classify(frame, lay, part, roles)) return last_why_ = "classify: " + roles.why, false;
    double residual = 0;
    if (fit_scale) {
      scale = assets::wrestler_geom::FitScale(part, roles, vb.data(), &residual);
      if (!(scale > 0.1f && scale < 10.0f) || residual > kScaleResidual) { char b[100]; std::snprintf(b, sizeof b, "scale fit refused scale %.6f residual %.3g", double(scale), residual); return last_why_ = b, false; }
    }
    assets::wrestler_geom::BuildOptions bo;
    bo.scale = scale;
    bo.fill0 = Fill0Mode(roles, vb, scale, part);
    if (bo.fill0 > 2) return last_why_ = "fill0 unknown", false;
    p.vertex.clear();
    assets::wrestler_geom::BuildVertices(part, roles, bo, p.vertex);
    assets::wrestler_geom::CompareResult cr;
    assets::wrestler_geom::Compare(roles, p.vertex, vb.data(), part.vertices, cr, kCawPosTolerance, !faults_.fill0_strict);
    if (!cr.all_ok) { char b[160]; std::snprintf(b, sizeof b, "bytes differ role %s first vertex %u bad %llu", assets::wrestler_geom::RoleName(cr.first_bad_role), cr.first_bad_vertex, (unsigned long long)cr.role[uint8_t(cr.first_bad_role)].bad); return last_why_ = b, false; }
    p.index = JoinedIndices(part);
    p.vertex_count = part.vertices, p.stride = roles.host_stride;
    uint32_t sb;
    std::memcpy(&sb, &scale, 4);
    scale_out = scale, fill0_out = bo.fill0, sig_out = Sig(roles, sb, bo.fill0);
    return true;
  }

  bool JoinPart(const rdr_adapter::LoadedFrame& frame, const rdr_adapter::Draw& dr, const rdr_adapter::VertexLayout& lay, const std::vector<uint8_t>& vb, const std::vector<size_t>& cands,
                const std::function<bool(const std::vector<uint16_t>&)>& index_ok, PartState& ps, bool& pending) {
    // the same leaf can fit through several of its meshes' entries; one try per (leaf, mesh)
    std::set<uint64_t> tried;
    for (size_t ci0 : cands) {
      size_t ci = ci0;
      if (faults_.bg_wrong_source && roster_.edit_parts()[ci0].pac == assets::wrestler_roster::Roster::kCawBackdropPacId) {   // lane 169 PLANT: the OTHER backdrop piece's file (a wrong source entry)
        for (size_t k = 0; k < roster_.edit_parts().size(); ++k)
          if (roster_.edit_parts()[k].pac == assets::wrestler_roster::Roster::kCawBackdropPacId && roster_.edit_parts()[k].group != roster_.edit_parts()[ci0].group) { ci = k; break; }
      }
      const auto& ep = roster_.edit_parts()[ci];
      if (!tried.insert(uint64_t(ep.group) << 32 | ep.mesh).second) continue;
      bool pend = false;
      std::shared_ptr<Model> leaf = LeafOf(ci, pend);
      if (pend) { pending = true; return false; }
      if (!leaf || ep.mesh >= leaf->parts.parts.size() || !leaf->parts.parts[ep.mesh].ok) continue;
      const assets::jboy_parts::Part& part = leaf->parts.parts[ep.mesh];
      auto p = std::make_unique<WrestlerPrepared>();
      float scale = 0;
      uint8_t fill0 = 0;
      uint64_t sig = 0;
      // (lane 169: a mesh with other counts than the draw never reaches the compare, which reads part.vertices * stride bytes of `vb`: only the bg_wrong_source plant can offer one)
      const bool ok = part.vertices == dr.vertex_count && DrawIndexCount(part) == dr.index_count && BuildAndCompare(frame, lay, vb, part, 0, true, *p, scale, fill0, sig);
      if (!ok && notes.size() < 400) { char b[300]; std::snprintf(b, sizeof b, "JOIN counts %u/%u cand pac %u pach %u leaf %u mesh %u: ", dr.vertex_count, dr.index_count, ep.pac, ep.pach, ep.leaf, ep.mesh); notes.push_back(b + last_why_); }
      if (ok && index_ok && !index_ok(p->index)) {   // lane 179 (review): the stream's strips decide between candidates whose vertices are equal; a candidate with other strips is not the part
        if (notes.size() < 400) { char b[300]; std::snprintf(b, sizeof b, "JOIN counts %u/%u cand pac %u pach %u leaf %u mesh %u: vertices fit, the index buffer differs", dr.vertex_count, dr.index_count, ep.pac, ep.pach, ep.leaf, ep.mesh); notes.push_back(b); }
        continue;
      }
      if (ok && faults_.scale_one) {   // PLANT: the right leaf built at scale 1
        assets::wrestler_geom::Roles roles;
        if (Classify(frame, lay, part, roles)) {
          assets::wrestler_geom::BuildOptions bo;
          bo.scale = 1.0f;
          p->vertex.clear();
          assets::wrestler_geom::BuildVertices(part, roles, bo, p->vertex);
        }
      }
      if (ok && faults_.index_swap) {   // PLANT: two neighbouring different indices swapped
        for (size_t i = 0; i + 1 < p->index.size(); ++i)
          if (p->index[i] != p->index[i + 1] && p->index[i] != 0xFFFF && p->index[i + 1] != 0xFFFF) { std::swap(p->index[i], p->index[i + 1]); break; }
      }
      if (!ok && !faults_.wrong_leaf && !(faults_.bg_wrong_source && roster_.edit_parts()[ci0].pac == assets::wrestler_roster::Roster::kCawBackdropPacId)) continue;
      if (!ok) {   // PLANT wrong_leaf (and lane 169's bg_wrong_source): the first count-fitting leaf, bytes unconfirmed (built with scale 1)
        assets::wrestler_geom::Roles roles;
        if (!Classify(frame, lay, part, roles)) continue;
        assets::wrestler_geom::BuildOptions bo;
        bo.scale = 1.0f;
        p->vertex.clear();
        assets::wrestler_geom::BuildVertices(part, roles, bo, p->vertex);
        p->index = JoinedIndices(part);
        p->vertex_count = part.vertices, p->stride = roles.host_stride;
        scale = 1.0f, fill0 = 0, sig = Sig(roles, 0, 0);
      }
      DropJoin(ps);
      ps.model = leaf, ps.mesh = ep.mesh, ps.group = ep.group, ps.scale = scale;
      ps.backdrop = ep.pac == assets::wrestler_roster::Roster::kCawBackdropPacId, ps.src_pac = ep.pac, ps.src_pach = ep.pach, ps.src_leaf = ep.leaf;   // lane 169
      ps.cur = p.get();
      ps.prepared[sig] = std::move(p);
      ++stats_.built;
      return true;
    }
    return false;
  }

  // The joined part against this draw: layout classified, the one scale and fill0 re-derived from THIS draw, our bytes built for this layout if it is new, then every element compared.
  bool VerifyPart(const rdr_adapter::LoadedFrame& frame, const rdr_adapter::Draw& dr, const rdr_adapter::VertexLayout& lay, const std::vector<uint8_t>& vb, PartState& ps) {
    if (faults_.no_verify) return true;
    if (!ps.model || ps.mesh >= ps.model->parts.parts.size()) return false;
    ++stats_.verify_runs;
    const assets::jboy_parts::Part& part = ps.model->parts.parts[ps.mesh];
    if (part.vertices != dr.vertex_count || DrawIndexCount(part) != dr.index_count) return false;
    assets::wrestler_geom::Roles roles;
    if (!Classify(frame, lay, part, roles)) return false;
    uint32_t sb;
    std::memcpy(&sb, &ps.scale, 4);
    const uint8_t fill0 = Fill0Mode(roles, vb, ps.scale, part);
    if (fill0 > 2) return false;
    const uint64_t sig = Sig(roles, sb, fill0);
    auto it = ps.prepared.find(sig);
    if (it == ps.prepared.end()) {   // this layout is new for the part: build for it (still confirmed by the compare below); keep the others (review S6)
      if (ps.prepared.size() >= kMaxLayouts) {
        for (auto& kv : ps.prepared) if (release_) release_(*kv.second), ++stats_.released;
        ps.prepared.clear();
        ps.cur = nullptr;
      }
      auto p = std::make_unique<WrestlerPrepared>();
      assets::wrestler_geom::BuildOptions bo;
      bo.scale = ps.scale, bo.fill0 = fill0;
      assets::wrestler_geom::BuildVertices(part, roles, bo, p->vertex);
      p->index = JoinedIndices(part);
      p->vertex_count = part.vertices, p->stride = roles.host_stride;
      it = ps.prepared.emplace(sig, std::move(p)).first;
      ++stats_.built, ++stats_.rejoins;
    }
    assets::wrestler_geom::CompareResult cr;
    assets::wrestler_geom::Compare(roles, it->second->vertex, vb.data(), part.vertices, cr, kCawPosTolerance, !faults_.fill0_strict);
    if (!cr.all_ok) return false;
    ps.cur = it->second.get();
    return true;
  }

  bool VerifyIndex(const WrestlerCore::ReadGeometry& read, const rdr_adapter::Draw& dr, PartState& ps, std::vector<uint8_t>& ib) {
    if (!ps.cur) return false;
    if (faults_.no_verify) return true;
    if (dr.index_bytes != ps.cur->index.size() * 2 || !read(dr.index_offset, dr.index_bytes, ib) || ib.size() != ps.cur->index.size() * 2) return false;
    return ps.cur->index.empty() || !std::memcmp(ib.data(), ps.cur->index.data(), ib.size());
  }

  void DropJoin(PartState& ps) {   // (joined_once stays: the part WAS ours)
    for (auto& kv : ps.prepared)
      if (release_) release_(*kv.second), ++stats_.released;
    ps.prepared.clear();
    ps.cur = nullptr;
    ps.model.reset();
  }
  void Sweep() {
    for (auto it = parts_.begin(); it != parts_.end();) {
      if (frame_no_ - it->second.last_frame > kIdleSweep) {
        DropJoin(it->second);
        it = parts_.erase(it);
      } else ++it;
    }
  }

  assets::wrestler_roster::Roster& roster_;
  CawFaults faults_;
  bool sync_, indexed_ = false;
  uint64_t frame_no_ = 0, idle_ = 0;
  std::unordered_map<uint64_t, std::vector<size_t>> by_counts_;
  std::vector<size_t> cands_;   // lane 179: scratch (Decide runs on one thread)
  std::unordered_map<uint64_t, PartState> parts_;
  std::mutex lmu_;
  std::condition_variable lcv_;
  std::map<uint32_t, LeafSlot> leaves_;
  std::vector<size_t> lq_;
  std::thread worker_;
  bool stop_ = false;
  std::function<void(WrestlerPrepared&)> release_;
  CawStats stats_;
  mutable std::string last_why_;
};

}  // namespace rdr_scene
