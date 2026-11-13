// The wrestlers' decisions, without a GPU (Session 101, lane 45 = W2; docs\renderer\WRESTLER_PROMOTION_MAP.md sections 3-5 and 10-11).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only. No game or SDK includes: the stream's frame (capture_reader.h), the scene record (record_io.h),
// the roster of the player's own models (assets\wrestler_roster.h) and the geometry builder (assets\wrestler_geom.h).
//
// WrestlerCore answers, for one stream draw of a wrestler's model part: is it ours, which model / part / vertex bytes / index bytes replace it, and which palette rows
// (c76 ..) come from the scene record - or, if any check fails, WHY it stays the stream's. The hook (wrestler_ours.cpp) only uploads what Decide returns; the offline
// checker (tools\wrestler\wrestler_ours_check.cpp) drives the same Core over a capture with no renderer, so what is proven is the shipped decision code.
//
// The checks (all per draw, all exact; docs section 3 "Verified every frame"):
//   join     the stream object's drawn parts (part, vertices, indices) fit a roster model, confirmed by building the biggest draw's vertices in the stream's host layout
//            and comparing them with the stream's captured bytes (a wrong model never survives that), the one position scale of the body measured from that draw;
//   link     the object is a record skeleton: the record's declared engine address when it carries one, else the skeleton with the same node count whose palette rows
//            are bit-equal to the object's draws' (>= 30 % of them);
//   counts   our vertex count and index count are the draw's;
//   palette  the draw's rows c76 .. c76 + 3N - 1 (N = the part's bone list, never where matching stops) equal the record's rows of those nodes bit for bit.
//            Brock's nodes >= 59 do not today (the update-hook palette): his draws are REFUSED (Palette), never drawn wrong;
//   bytes    the vertex bytes we built equal the stream draw's captured bytes (positions within a few float ulps, normals one field step) and our index buffer is the
//            stream's: every draw in fview, the first use of each built part plus every 64th live.
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "skeleton_want.h"   // lane 207
#include "../assets/wrestler_geom.h"
#include "../assets/wrestler_roster.h"
#include "../renderer_adapter/capture_reader.h"
#include "../renderer_adapter/draw_replace_hook.h"
#include "proof_frames.h"
#include "record_io.h"
#include "shadow_join_board.h"   // S101 lane 99: BodyStatus, ShadowBody

namespace rdr_scene {

// Planted faults (the acceptance test's twins). Each one is the same code path with one thing wrong; "checks_off" / "accept_record_palette" switch a check off so the
// picture shows what the check was protecting.
struct WrestlerFaults {
  bool bind_pose = false;              // the palette rows are the identity (the bind pose), not the record's
  bool record_off_by_one = false;      // the record of the frame before is used (lesson 1: the pairing is +1, not assumed)
  bool part_off_by_one = false;        // the mesh of part + 1 replaces part
  bool wrong_attire = false;           // another roster model of the same node count (another attire) replaces the matched one
  bool scale_one = false;              // positions are not scaled (lesson 5)
  bool fill0_unknown = false;          // lane 262, planted: the fill0 element reads as neither known mode (GetPrepared refuses: counted "bytes fill0" and noted)
  bool lent_constants = false;         // the first draw's palette rows of the object are lent to every part (lesson 2)
  bool uv_swap = false;                // the uv 16-bit halves exchanged (Session 100's bug)
  bool accept_record_palette = false;  // the palette check is off: the record's rows are used as they are (today's update-hook palette for Brock)
  bool checks_off = false;             // counts / palette / bytes are not checked (a plant's picture)
  bool flat_indices = false;           // lane 179 PLANT: a multi-group part's index count is the plain sum and its groups are joined without 0xFFFF (the old rule): such a part is refused
  bool piece_wrong_owner = false;      // S101 lane 48: a piece is linked to another body's skeleton
  bool piece_node_off = false;         // S101 lane 48: a piece's palette slot reads the body node after the one its name names
  bool piece_body_scale = false;       // S101 lane 48: a piece's positions use its owner body's scale, not its own
  bool own_model_only = false;         // S101 lane 100: every part is read from the object's one joined model (the bug: WM30 Hogan's part 1 is another attire's)
  bool no_short_maps = false;          // S101 lane 134: the 11-dword fallback maps are off (the referee's patch and alpha piece stay unjoined again)
  bool short_map_shift = false;        // S101 lane 134: the fallback maps put the uv at the wrong dword (the layout must stay refused, checks on or off)
  bool piece_wrong_entry = false;      // S101 lane 134: a piece (small node table) is joined to the NEXT entry of its pac (same attire) instead of its own
  bool piece_bind_pose = false;        // S101 lane 134: a piece's palette rows are the identity (bind pose); the bodies keep theirs
  bool slow_load = false;              // lane 142 (TEST ONLY, fview): every roster model answers "still loading" for its first 3 asks, as a worker thread would (the offline census then shows `loading`)
  bool no_pac_keep = false;            // lane 142: every model load unpacks its pac again (the old cost)
  bool thread_per_read = false;        // lane 142: one std::async thread per model read (the old way: up to 13-40 threads contending on the load mutex)
  bool serial_load = false;            // lane 142: the old wait: the candidates' reads start one per frame, in series
  bool no_proof_frames = false;        // S101 lane 88: a pinned part is never fully re-checked (proof frames off): a vertex blob corrupted after the pin must then go undetected, and the test that requires detection fails
  static bool Parse(const std::string& list, WrestlerFaults& f, std::string& error) {
    size_t at = 0;
    while (at <= list.size()) {
      size_t comma = list.find(',', at);
      if (comma == std::string::npos) comma = list.size();
      const std::string w = list.substr(at, comma - at);
      at = comma + 1;
      if (w.empty()) continue;
      if (w == "bind_pose") f.bind_pose = true;
      else if (w == "record_off_by_one") f.record_off_by_one = true;
      else if (w == "part_off_by_one") f.part_off_by_one = true;
      else if (w == "wrong_attire") f.wrong_attire = true;
      else if (w == "scale_one") f.scale_one = true;
      else if (w == "fill0_unknown") f.fill0_unknown = true;
      else if (w == "lent_constants") f.lent_constants = true;
      else if (w == "uv_swap") f.uv_swap = true;
      else if (w == "accept_record_palette") f.accept_record_palette = true;
      else if (w == "checks_off") f.checks_off = true;
      else if (w == "flat_indices") f.flat_indices = true;
      else if (w == "piece_wrong_owner") f.piece_wrong_owner = true;
      else if (w == "piece_node_off") f.piece_node_off = true;
      else if (w == "piece_body_scale") f.piece_body_scale = true;
      else if (w == "own_model_only") f.own_model_only = true;
      else if (w == "no_proof_frames") f.no_proof_frames = true;
      else if (w == "piece_wrong_entry") f.piece_wrong_entry = true;   // lane 134
      else if (w == "no_short_maps") f.no_short_maps = true;   // lane 134
      else if (w == "short_map_shift") f.short_map_shift = true;   // lane 134
      else if (w == "piece_bind_pose") f.piece_bind_pose = true;   // lane 134
      else if (w == "serial_load") f.serial_load = true;   // lane 142
      else if (w == "thread_per_read") f.thread_per_read = true;   // lane 142
      else if (w == "slow_load") f.slow_load = true;   // lane 142
      else if (w == "no_pac_keep") f.no_pac_keep = true;   // lane 142
      else {
        error = "unknown wrestler fault '" + w + "' (bind_pose, record_off_by_one, part_off_by_one, wrong_attire, scale_one, fill0_unknown, lent_constants, uv_swap, accept_record_palette, checks_off, flat_indices, piece_wrong_owner, piece_node_off, piece_body_scale, piece_wrong_entry, piece_bind_pose, no_short_maps, short_map_shift, own_model_only, no_proof_frames, serial_load, slow_load, no_pac_keep, thread_per_read)";
        return false;
      }
    }
    return true;
  }
  bool any() const { return bind_pose || record_off_by_one || part_off_by_one || wrong_attire || scale_one || fill0_unknown || lent_constants || uv_swap || accept_record_palette || checks_off || flat_indices || piece_wrong_owner || piece_node_off || piece_body_scale || own_model_only || no_proof_frames || piece_wrong_entry || piece_bind_pose || no_short_maps || short_map_shift || serial_load || slow_load || no_pac_keep || thread_per_read; }
};

struct WrestlerStats {
  uint64_t frames_ready = 0, frames_not_ready = 0;
  uint64_t not_ready_no_record = 0, not_ready_roster = 0;   // lane 253: WHY a frame was not ready (the board's "Wrestlers ours 79-91 %" is the share of frames): no scene record yet / the roster is still being read
  uint64_t objects_seen = 0;            // distinct stream objects that fitted a roster model by counts
  uint64_t objects_joined = 0;          // ... and were confirmed by bytes
  uint64_t objects_unjoined = 0;        // ... fitted but no candidate matched the bytes
  uint64_t objects_ambiguous = 0;       // more than one candidate fitted the counts (resolved by the bytes)
  uint64_t rekeyed = 0;                 // lane 220: a pinned (object, part) whose built-part key changed (which field: rekey_notes_): the thin template of that key is stale until its proof frame
  uint64_t scale_measured = 0;          // bodies whose one position scale was measured from a stream draw
  uint64_t links_declared = 0, links_by_rows = 0, links_none = 0;   // per object per frame
  uint64_t claimed = 0, replaced = 0;
  uint64_t not_wrestler = 0;            // claimed draws of objects that fit no roster model (the arena's / props: passed on)
  uint64_t no_link = 0;                 // a wrestler model but no record skeleton (pieces with their own nodes)
  uint64_t loading = 0;                 // model still being read
  uint64_t refused_counts = 0, refused_layout = 0, refused_palette = 0, refused_bytes = 0, refused_other = 0;
  uint64_t verify_runs = 0, verify_bad = 0;
  uint64_t layout_old_capture = 0;
  uint64_t palette_rows_checked = 0, palette_rows_exact = 0;
  uint64_t prepared_built = 0;
  uint64_t replaced_by_slot[16] = {}, palette_refused_by_slot[16] = {};   // by record slot ("actor:slot:N"); the last entry = not a slot
  uint64_t part_bytes_vertex = 0, part_bytes_index = 0;
  uint64_t cmp_values = 0, cmp_exact = 0, cmp_near = 0, cmp_bad = 0;   // every vertex element compared with the stream's bytes (verify): bit-equal / within the role's tolerance / beyond it
  uint64_t upload_failed = 0, released = 0;
  uint64_t bytes_read = 0, bytes_stride = 0, bytes_fill0 = 0, bytes_verify = 0;   // lane 262: WHY a draw was refused on bytes (refused_bytes = these + upload_failed): fewer bytes than one vertex, the first vertex unreadable, the fill0 element in neither known mode, the byte verify failed
  uint64_t pinned_draws = 0, proof_draws = 0, fresh_draws = 0;   // S101 lane 88: draws that took the cheap identity checks only / the full check on their proof frame / the full check because the part was not pinned (first sight or its identity changed)
  // S101 lane 48: pieces (hair, pads, alpha pieces: own node table, rows from the owner body's skin array by node NAME)
  uint64_t piece_objects_linked = 0;   // object-frames: one per piece object per bound frame it was linked to an owner
  uint64_t models_started = 0;          // S101 lane 100: roster models read in full (each one is a worker's job live; a draw waits for it as Unjoined / loading)
  uint64_t part_other_attire = 0;       // S101 lane 100: (object, part) pairs whose bytes came from another entry of the character's pac
  uint64_t piece_no_owner = 0, piece_replaced = 0, piece_palette_refused = 0;   // DRAWS
};

// One part's vertex + index data, built once per (model, part, layout, scale, fill0, uv). The hook puts its GPU buffers in `gpu`.
struct WrestlerPrepared {
  std::vector<uint8_t> vertex;
  std::vector<uint16_t> index;
  uint32_t vertex_count = 0, stride = 0;
  uint64_t gpu[2] = {0, 0};
  uint64_t last_use = 0, uses = 0;
  bool verified = false;
  bool tangent_not_in_file = false;   // lane 206: the checker skips the tangent bytes of such a part
};

struct WrestlerDecision {
  WrestlerPrepared* prepared = nullptr;
  uint32_t first_register = 76, rows = 0;
  const float* row_data = nullptr;       // rows * 4 floats, valid until the next Decide
  int slot = -1;                         // the record slot ("actor:slot:N"), -1 none
  bool is_piece = false;                 // S101 lane 48: a piece of a body (rows by node name), for the checker's piece-only counts
};

class WrestlerCore {
 public:
  using ReadGeometry = std::function<bool(uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out)>;
  using Model = assets::wrestler_roster::Model;

  // `sync`: models are read on the calling thread (fview); else on worker threads and a draw whose model is still loading stays the stream's.
  WrestlerCore(assets::wrestler_roster::Roster& roster, const WrestlerFaults& faults, bool sync, bool verify_all)
      : roster_(roster), faults_(faults), sync_(sync), verify_all_(verify_all) {}

  void SetRecord(std::shared_ptr<const SceneTables> record) {
    if (record != record_next_) record_prev_ = record_next_;
    record_next_ = std::move(record);
  }
  void SetReleaseHook(std::function<void(WrestlerPrepared&)> f) { release_ = std::move(f); }
  // S101 lane 207: plain function pointers (the core stays SDK-free and does not include scene_live): a small (own-skeleton) model's object address and node count are WANTED once per object,
  // so the live hook keeps that skeleton; unwanted when the object goes. Wired by the app.
  void SetSkeletonWantHooks(void (*want)(uint32_t, uint32_t), void (*unwant)(uint32_t)) { skel_want_ = want; skel_unwant_ = unwant; }
  const WrestlerStats& stats() const { return stats_; }
  const WrestlerFaults& faults() const { return faults_; }
  struct JoinedBody { int pac, attire; uint32_t entry, nodes; float scale; };
  // lane 207 (checker only): the model an object was joined to (null: not joined), to build the planted / injected own-skeleton bones.
  std::shared_ptr<const Model> ModelOfObject(uint32_t object) const {
    const auto it = objects_.find(object);
    return it == objects_.end() ? nullptr : it->second.model;
  }
  const std::vector<JoinedBody>& joined() const { return joined_; }   // every distinct (model, scale) the bytes confirmed
  size_t prepared_count() const { return prepared_.size(); }
  // S101 lane 100: WHY a draw was refused as Mismatch (bytes / counts), for the offline checker (--refusals). Capped; the hook ignores it.
  struct RefusalNote { uint32_t object, entry, part, vertices, stream_vertices; int slot; const char* what; const char* role; uint32_t vertex; double max_err; uint64_t bad; uint32_t ours_word, theirs_word; };
  const std::vector<RefusalNote>& refusal_notes() const { return notes_; }
  const std::vector<std::string>& candidate_notes() const { return cand_notes_; }

  // Once per bound frame. False: no usable record or the roster is not ready (every claimed draw stays the stream's).
  bool BeginFrame(const rdr_adapter::LoadedFrame& frame) {
    ++frame_no_;
    roster_.TrimIdle(1.5);   // lane 142: before the readiness check: a pac a load kept does not stay resident through menus and load screens
    rec_ = faults_.record_off_by_one ? record_prev_ : record_next_;
    if (!rec_ || roster_.Poll() != 1 || !(frame.header.flags & rdr_adapter::kFrameSixteenPairsHighFirst)) {
      if (frame.header.flags & rdr_adapter::kFrameSixteenPairsHighFirst) {} else ++stats_.layout_old_capture;
      if (!rec_) ++stats_.not_ready_no_record;
      else if (roster_.Poll() != 1) ++stats_.not_ready_roster;
      ++stats_.frames_not_ready;
      return false;
    }
    ++stats_.frames_ready;
    // This frame's skeletons of the record.
    skeletons_.clear();
    for (uint32_t k = 0; k < rec_->objects.size(); ++k) {
      const rdr_scene::Object& ob = rec_->objects[k];
      const std::string_view id = rec_->Str(ob.id);
      if (!(id.rfind("actor:", 0) == 0 || id.rfind("skel:", 0) == 0) || ob.bones_count == 0 || ob.bones_first + ob.bones_count > rec_->bones.size()) continue;
      Skeleton s;
      s.object = k, s.first = ob.bones_first, s.count = ob.bones_count;
      s.slot = -1;
      s.id = std::string(id);
      if (id.rfind("actor:slot:", 0) == 0) s.slot = std::atoi(std::string(id.substr(11)).c_str());
      skeletons_.push_back(s);
    }
    min_skel_nodes_ = 0;
    // S101 lane 206: the smallest BODY skeleton (60+ nodes, the contributor's default size filter). A small piece skeleton the hook now records (hair, 48 nodes) must not lower the bar a model has to be
    // under to be a piece borrowing a body's rows (else every 11-node alpha piece fails the test at PieceCandidate).
    for (const Skeleton& sk : skeletons_)
      if (sk.count >= kBodySkeletonNodes) min_skel_nodes_ = min_skel_nodes_ ? std::min(min_skel_nodes_, sk.count) : sk.count;
    for (auto bi = bodies_.begin(); bi != bodies_.end();) {   // S101 lane 48: a body whose skeleton is not in this frame's record is dropped (its model with it)
      bool present = false;
      for (const Skeleton& sk : skeletons_) present = present || sk.id == bi->first;
      if (present) ++bi;
      else bi = bodies_.erase(bi);
    }
    if (skeletons_.empty()) {   // keyed on the record, as the crowd's release is: no wrestler skeleton for a while = a load screen / between matches
      if (++idle_ > kIdleFrames) ReleaseAll();
    } else idle_ = 0;
    // This frame's stream objects.
    for (auto& kv : objects_) kv.second.draws.clear();
    for (uint32_t d = 0; d < frame.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
      if (!o || (o->pass_site != rdr_adapter::kModelPartColourSite && o->pass_site != rdr_adapter::kModelPartMirrorSite) || o->object == 0) continue;
      ObjState& st = objects_[o->object];
      st.addr = o->object;
      if (drops_wanted_) {   // lane 129 (KC, thin draws): a lite draw carries no vertex count (the reader did not scan its indices) and no geometry. The object's signature (Resolve) needs every part's counts every
          // frame, so a lite draw takes the vertex count of the last full draw of this (object, part) (the key's template was learned from that draw and the proof frames re-prove it) and its index count from the facts table.
        const rdr_adapter::Draw& dr = frame.draws[d];
        if (dr.flags & rdr_adapter::kDrawThin) {
          const auto kc = st.thin_counts.find(o->part);
          if (kc != st.thin_counts.end()) {
            rdr_adapter::Draw& wd = const_cast<rdr_adapter::Draw&>(dr);   // (the frame's draw table is the reader's own, rewritten for this bind; the same cast as fview's lite-reader emulation)
            wd.vertex_count = kc->second[0];
            wd.index_count = frame.thin_facts.count == frame.draws.count ? frame.thin_facts[d].index_count : kc->second[1];
          }
        } else {
          st.thin_counts[o->part] = {dr.vertex_count, dr.index_count};
        }
      }
      st.draws.push_back(d);
    }
    for (auto it = objects_.begin(); it != objects_.end();) {
      ObjState& st = it->second;
      if (st.draws.empty()) {
        if (frame_no_ - st.last_frame > 240) { UnwantObject(st); it = objects_.erase(it); }
        else ++it;
        continue;
      }
      st.last_frame = frame_no_;   // (resolved and linked lazily at its first claimed draw: that call carries the stream's geometry reader)
      ++it;
    }
    Sweep();
    return true;
  }

  // One claimed draw. True: `out` is filled. False: `why` (NotOurs: another class's object, passed on).
  bool Decide(const rdr_adapter::LoadedFrame& frame, uint32_t draw_index, const ReadGeometry& read, WrestlerDecision& out, rdr_adapter::KeepWhy& why) {
    using rdr_adapter::KeepWhy;
    ++stats_.claimed;
    const rdr_adapter::DrawOrigin* o = frame.OriginOf(draw_index);
    if (!o) return why = KeepWhy::NotOurs, ++stats_.not_wrestler, false;
    auto it = objects_.find(o->object);
    if (it == objects_.end()) return why = KeepWhy::NotOurs, ++stats_.not_wrestler, false;
    ObjState& st = it->second;
    if (st.ready_frame != frame_no_) {
      Resolve(frame, st, read);
      LinkObject(frame, st);
      st.ready_frame = frame_no_;
      if (PieceCandidate(st)) {
        // S101 lane 48: a piece needs its owner body linked (registered) first: the other objects that fit a roster model are resolved and linked now, then every piece finds its owner
        for (auto& kv : objects_) {
          ObjState& o2 = kv.second;
          if (o2.ready_frame == frame_no_ || o2.draws.empty() || o2.cands.empty()) continue;
          Resolve(frame, o2, read);
          LinkObject(frame, o2);
          o2.ready_frame = frame_no_;
        }
        for (auto& kv : objects_) {
          ObjState& o2 = kv.second;
          if (!o2.draws.empty() && PieceCandidate(o2)) LinkPiece(frame, o2);
        }
      }
      if (drops_wanted_) {   // lane 129 review F6: an object that lost its skeleton or was linked to another (by id: an index is rebuilt every frame) leaves the thin set
        for (auto& kv : objects_) {
          ObjState& o2 = kv.second;
          if (o2.ready_frame != frame_no_ || o2.draws.empty()) continue;
          const std::string id = o2.model && o2.skeleton >= 0 && size_t(o2.skeleton) < skeletons_.size() ? skeletons_[size_t(o2.skeleton)].id : std::string();
          if (!o2.skel_id.empty() && o2.skel_id != id) DropObject(kv.first);
          o2.skel_id = id;
        }
      }
    }
    if (st.cands.empty()) return why = KeepWhy::NotOurs, ++stats_.not_wrestler, false;    // fits no roster model: the ring, props, belts (the arena hook's)
    if (st.pending) return why = KeepWhy::Unjoined, ++stats_.loading, false;
    if (!st.model) return why = KeepWhy::Unjoined, ++stats_.refused_other, false;           // fitted by counts, no candidate matched the bytes
    if (st.skeleton < 0) {
      if (st.link_rows_failed) return why = KeepWhy::Palette, ++stats_.refused_palette, false;   // a skeleton of this size exists but its rows are not the draw's
      if (PieceCandidate(st)) ++stats_.piece_no_owner;
      return why = KeepWhy::Unjoined, ++stats_.no_link, false;           // a model with its own nodes and no owner body found (S101 lane 48: pieces are linked by node name)
    }
    const Skeleton& sk = skeletons_[size_t(st.skeleton)];
    const rdr_adapter::Draw& dr = frame.draws[draw_index];
    if (dr.index_32bit || !dr.index_bytes || !dr.vertex_bytes || (dr.flags & rdr_adapter::kDrawInline)) return why = KeepWhy::Variant, ++stats_.refused_layout, false;   // the file's strip is u16, indexed, not inline
    // ---- the part ----
    int64_t pi = int64_t(o->part) + (faults_.part_off_by_one ? 1 : 0);
    const assets::jboy_parts::Parts& jp = st.model->parts;
    if (pi < 0 || pi >= int64_t(jp.parts.size()) || !jp.parts[size_t(pi)].ok) return why = KeepWhy::Mismatch, ++stats_.refused_counts, false;
    const assets::jboy_parts::Part* part_own = &jp.parts[size_t(pi)];
    if (!faults_.checks_off && (part_own->vertices != dr.vertex_count || assets::wrestler_geom::DrawIndexCount(*part_own, faults_.flat_indices) != dr.index_count)) return why = KeepWhy::Mismatch, ++stats_.refused_counts, false;
    // ---- the layout ----
    if (dr.layout >= frame.layouts.count) return why = KeepWhy::Variant, ++stats_.refused_layout, false;
    const rdr_adapter::VertexLayout& lay = frame.layouts[dr.layout];
    // S101 lane 100: a body's parts need not all come from the one attire the biggest draw joined (WM30 Hogan: part 1 is another attire's piece of the same character).
    // The part's SOURCE is settled once per (object, part) by the stream's own bytes: the object's model, else another entry of the same character pac.
    const assets::jboy_parts::Part* part_alt = nullptr;
    int part_entry = st.entry;
    if (!faults_.checks_off && !faults_.own_model_only) {
      bool pending = false;
      const PartSource* src = ResolvePartSource(frame, st, uint32_t(pi), dr, lay, read, pending);
      if (pending) return why = KeepWhy::Unjoined, ++stats_.loading, false;
      if (src && src->model) part_alt = &src->model->parts.parts[size_t(pi)], part_entry = src->entry;
    }
    const assets::jboy_parts::Part& part = part_alt ? *part_alt : *part_own;
    assets::wrestler_geom::Roles roles;
    if (!Classify(frame, lay, part, roles)) return why = KeepWhy::Variant, ++stats_.refused_layout, false;
    // ---- the palette ----
    const uint32_t N = part.palette_count;
    if (N == 0 || dr.vs_bank >= frame.constant_banks.count) return why = KeepWhy::Palette, ++stats_.refused_palette, false;
    rows_.assign(size_t(N) * 12, 0.0f);
    const rdr_adapter::ConstantBank& bank = frame.constant_banks[dr.vs_bank];
    bool exact = true;
    for (uint32_t k = 0; k < N; ++k) {
      uint32_t node1 = part.palette[k];
      if (st.is_piece) {   // a piece's palette slot names a node of ITS table; the owner body's node of the same name carries the rows
        if (node1 < 1 || node1 > st.piece_map.size() || st.piece_map[node1 - 1] < 0) { exact = false; continue; }
        node1 = uint32_t(st.piece_map[node1 - 1]) + 1 + (faults_.piece_node_off ? 1 : 0);
      }
      if (node1 < 1 || node1 > sk.count) { exact = false; continue; }
      const rdr_scene::Mat4& M = rec_->bones[sk.first + node1 - 1];
      for (int j = 0; j < 3; ++j) {
        float* row = &rows_[(size_t(k) * 3 + size_t(j)) * 4];
        for (int c = 0; c < 4; ++c) row[c] = (faults_.bind_pose || (faults_.piece_bind_pose && st.is_piece)) ? (c == j ? 1.0f : 0.0f) : M.m[c * 4 + j];   // lane 134: piece_bind_pose
      }
    }
    if (faults_.lent_constants) {   // planted: the first draw's rows of the object are lent to every part
      if (st.first_draw_rows_valid) {
        for (uint32_t r = 0; r < std::min<uint32_t>(N * 3, uint32_t(st.first_rows.size() / 4)); ++r) std::memcpy(&rows_[size_t(r) * 4], &st.first_rows[size_t(r) * 4], 16);
      } else {
        st.first_rows.assign(rows_.begin(), rows_.end());
        st.first_draw_rows_valid = true;
      }
    }
    // The check is on what we would SEND: its rows must be the stream draw's own, bit for bit (so every way of getting the rows wrong - the record one frame off, the
    // update-hook palette, the bind pose, another part's block - is caught here, at the same place).
    for (uint32_t r = 0; r < N * 3; ++r) {
      const float* g = Reg(bank, 76 + int(r));
      bool same = true;
      for (int c = 0; c < 4; ++c) same = same && Bits(g[c]) == Bits(rows_[size_t(r) * 4 + size_t(c)]);
      ++stats_.palette_rows_checked;
      stats_.palette_rows_exact += same;
      exact = exact && same;
    }
    const int slot_i = sk.slot >= 0 && sk.slot < 15 ? sk.slot : 15;
    if (!exact && !faults_.checks_off && !faults_.accept_record_palette) {
      ++stats_.refused_palette, ++stats_.palette_refused_by_slot[slot_i];
      if (st.is_piece) ++stats_.piece_palette_refused;
      return why = KeepWhy::Palette, false;
    }
    // ---- the built part ----
    int pin_state = 2;   // S101 lane 88: 0 pinned (identity checks only), 1 its proof frame, 2 not pinned: the full check
    WrestlerPrepared* pr = GetPrepared(frame, st, o->object, uint32_t(pi), part, roles, lay, dr, read, part_entry, pin_state);
    if (!pr) {
      ++stats_.refused_bytes;
      NoteBytes(st, uint32_t(pi), dr, lay, read);   // lane 262
      return why = KeepWhy::Mismatch, false;
    }
    // ---- the bytes ----
    ++pr->uses;
    pr->last_use = frame_no_;
    const bool pins_on = kWrestlerPins && !verify_all_ && !faults_.checks_off;
    if (!faults_.checks_off && (verify_all_ || !pr->verified || (pins_on ? pin_state != 0 : (pr->uses & 63) == 0))) {
      ++stats_.verify_runs;
      verify_what_ = "ok", verify_role_ = "", verify_vertex_ = 0, verify_err_ = 0;
      if (!VerifyBytes(frame, dr, roles, *pr, part, read)) {
        if (diag_cands_ && cand_noted_.insert(uint64_t(st.addr) << 8 | uint64_t(pi)).second) NoteCandidates(frame, st, uint32_t(pi), dr, lay, read);
        if (notes_.size() < 8192) notes_.push_back({st.addr, uint32_t(st.entry), uint32_t(pi), part.vertices, dr.vertex_count, sk.slot, verify_what_, verify_role_, verify_vertex_, verify_err_, verify_bad_, verify_ours_, verify_theirs_});
        pr->verified = false;   // a part that failed once is verified on every later use (live too) until it passes again
        ++stats_.verify_bad, ++stats_.refused_bytes, ++stats_.bytes_verify;
        return why = KeepWhy::Mismatch, false;
      }
      pr->verified = true;
    }
    out.prepared = pr;
    out.first_register = 76, out.rows = N * 3, out.row_data = rows_.data(), out.slot = sk.slot, out.is_piece = st.is_piece;
    ++stats_.replaced, ++stats_.replaced_by_slot[slot_i];
    if (st.is_piece) ++stats_.piece_replaced;
    return true;
  }

  // S101 lane 99 (ShadowJoinBoard): read-only join of a stream object for the shadow casters, asked BEFORE this frame's colour draws are decided (the shadow passes draw first).
  // BeginFrame must have run for this frame. It resolves the object by content exactly as Decide would (Resolve), then requires that EVERY colour draw of the object rebuilds bit for bit
  // from the joined model with the one scale (a candidate that rebuilds one part is not yet the body). It changes no decision of Decide.
  BodyStatus JoinOf(const rdr_adapter::LoadedFrame& frame, uint32_t object, const ReadGeometry& read, ShadowBody& out) {
    const auto it = objects_.find(object);
    if (it == objects_.end() || it->second.draws.empty()) return BodyStatus::NotWrestler;
    ObjState& st = it->second;
    Resolve(frame, st, read);
    if (st.cands.empty()) return BodyStatus::NotWrestler;
    if (st.pending) return BodyStatus::Pending;
    if (!st.model) return BodyStatus::Unjoined;
    if (st.confirm_sig != st.sig_hash && frame_no_ >= st.confirm_next) {
      // S101 lane 99 (review F5): the settled per-part source of lane 100 (a part may come from another attire of the character's pac) is used; a model still being read or an unreadable draw is NOT cached
      // as a verdict (retry next frame / after kRetryFrames); only a definite yes or no is kept until the object's drawn parts change.
      const int r = ConfirmAll(frame, st, read);
      if (r >= 0) st.confirmed = r == 1, st.confirm_sig = st.sig_hash;
      else st.confirm_next = frame_no_ + (r == -1 ? 1 : kRetryFrames);
      if (r == -1) return BodyStatus::Pending;
    }
    if (st.confirm_sig != st.sig_hash || !st.confirmed) return BodyStatus::Unjoined;
    out.model = st.model, out.scale = st.scale, out.entry = st.entry, out.part_models = st.shadow_parts;
    return BodyStatus::Joined;
  }

  std::string Report() const {
    const WrestlerStats& s = stats_;
    char b[2000];
    std::snprintf(b, sizeof b,
                  "wrestlers: objects seen %llu joined %llu (ambiguous %llu, unjoined %llu), scale measured %llu, links declared %llu by rows %llu none %llu, claimed %llu replaced %llu | not wrestler %llu, no link %llu, "
                  "loading %llu, refused: counts %llu layout %llu palette %llu bytes %llu other %llu | palette rows %llu exact %llu | verify %llu bad %llu, vertex elements compared %llu: exact %llu near %llu bad %llu | parts built %llu (%.1f MB), released %llu, upload failed %llu",
                  (unsigned long long)s.objects_seen, (unsigned long long)s.objects_joined, (unsigned long long)s.objects_ambiguous, (unsigned long long)s.objects_unjoined,
                  (unsigned long long)s.scale_measured, (unsigned long long)s.links_declared, (unsigned long long)s.links_by_rows, (unsigned long long)s.links_none, (unsigned long long)s.claimed,
                  (unsigned long long)s.replaced, (unsigned long long)s.not_wrestler, (unsigned long long)s.no_link, (unsigned long long)s.loading, (unsigned long long)s.refused_counts,
                  (unsigned long long)s.refused_layout, (unsigned long long)s.refused_palette, (unsigned long long)s.refused_bytes, (unsigned long long)s.refused_other,
                  (unsigned long long)s.palette_rows_checked, (unsigned long long)s.palette_rows_exact, (unsigned long long)s.verify_runs, (unsigned long long)s.verify_bad,
                  (unsigned long long)s.cmp_values, (unsigned long long)s.cmp_exact, (unsigned long long)s.cmp_near, (unsigned long long)s.cmp_bad,
                  (unsigned long long)s.prepared_built, double(s.part_bytes_vertex + s.part_bytes_index) / 1048576.0, (unsigned long long)s.released, (unsigned long long)s.upload_failed);
    std::string text = b;
    text += " | bytes refused: stride " + std::to_string(s.bytes_stride) + " read " + std::to_string(s.bytes_read) + " fill0 " + std::to_string(s.bytes_fill0) + " verify " + std::to_string(s.bytes_verify) + " upload " + std::to_string(s.upload_failed);   // lane 262
    for (const std::string& n : bytes_notes_) text += " [" + n + "]";
    text += " | pins re-keyed " + std::to_string(s.rekeyed);
    for (const std::string& n : rekey_notes_) text += " [" + n + "]";
    text += " | models read " + std::to_string(s.models_started) + ", parts from another attire of the pac " + std::to_string(s.part_other_attire);
    text += " | pieces (draws unless noted): replaced " + std::to_string(s.piece_replaced) + " palette-refused " + std::to_string(s.piece_palette_refused) + " no owner " + std::to_string(s.piece_no_owner) + ", object-frames linked " + std::to_string(s.piece_objects_linked);
    text += " | by slot (replaced / palette refused):";
    for (int i = 0; i < 16; ++i)
      if (s.replaced_by_slot[i] || s.palette_refused_by_slot[i])
        text += " " + (i < 15 ? std::to_string(i) : std::string("other")) + "=" + std::to_string(s.replaced_by_slot[i]) + "/" + std::to_string(s.palette_refused_by_slot[i]);
    return text;
  }

  // A bound frame with nothing this hook claimed (the binder's NoClaimFrame): after kIdleFrames of them everything is released.
  // lane 227 (Wrestlers D3 H2): the part's palette length from the joined model's file (the part's settled source when it comes from another attire of the pac, lane 100), 0 when the object is not joined / the part is not in it.
  // Read only (the D3 chain's don't-care slots: the part's vertices address only the first N slots of c76..c138).
  uint32_t PartPaletteCount(uint32_t object, uint32_t part) const {
    const auto it = objects_.find(object);
    if (it == objects_.end() || !it->second.model) return 0;
    const ObjState& st = it->second;
    const auto ps = st.part_src.find(part);
    const std::shared_ptr<Model>& m = (ps != st.part_src.end() && ps->second.resolved && ps->second.for_entry == st.entry && ps->second.src.model) ? ps->second.src.model : st.model;
    return part < m->parts.parts.size() && m->parts.parts[part].ok ? uint32_t(m->parts.parts[part].palette_count) : 0;
  }
  void SetDropsWanted(bool on) { drops_wanted_ = on; }   // lane 129 review F6: drops and the lite-draw counts are kept only while the thin binder asks
  uint32_t GlobalReleaseSeq() const { return global_release_seq_; }   // counts ReleaseAll / Clear (every part gone): the class's thin epoch
  void TestReleaseOne() {   // TEST ONLY (fview --thin-release-part): the stalest built part is released as a sweep would
    auto best = prepared_.end();
    for (auto it = prepared_.begin(); it != prepared_.end(); ++it)
      if (it->second->gpu[0] && (best == prepared_.end() || it->second->last_use < best->second->last_use)) best = it;
    if (best == prepared_.end()) return;
    if (release_) release_(*best->second);
    ++stats_.released;
    prepared_.erase(best);
    for (auto pi = pins_.begin(); pi != pins_.end();) pi = prepared_.count(pi->second.key) ? std::next(pi) : pins_.erase(pi);
  }
  void DropObject(uint32_t object) { if (drops_wanted_) dropped_objects_.push_back(object), ++drop_seq_; }
  uint32_t DropSeq() const { return drop_seq_; }   // lane 129 (KC)
  void TakeDrops(std::vector<uint32_t>& out) { out.insert(out.end(), dropped_objects_.begin(), dropped_objects_.end()); dropped_objects_.clear(); }
  void NoClaimFrame() {
    roster_.TrimIdle(1.5);   // lane 142
    if (++idle_ > kIdleFrames) ReleaseAll();
  }
  // The hook could not upload what Decide just returned (counted as replaced there): the draw stays the stream's, the counts are corrected.
  void UploadFailed(const WrestlerDecision& dec) {
    ++stats_.upload_failed;
    if (stats_.replaced) --stats_.replaced;
    const int slot_i = dec.slot >= 0 && dec.slot < 15 ? dec.slot : 15;
    if (stats_.replaced_by_slot[slot_i]) --stats_.replaced_by_slot[slot_i];
    ++stats_.refused_bytes;
  }

  // Drops every built part (the hook releases its buffers through the release hook first).
  void Clear() {
    ++global_release_seq_;   // lane 129 (KC)
    for (auto& kv : prepared_) if (release_) release_(*kv.second);
    prepared_.clear();
    pins_.clear();
    rekey_notes_.clear();
    for (auto& kv : objects_) UnwantObject(kv.second);
    objects_.clear();
    bodies_.clear();
  }

 private:
  static constexpr size_t kJoinDraws = 6;          // draws of an object tried (largest first) when joining it to a model
  static constexpr uint64_t kRetryFrames = 120;    // a failed join is retried after this many bound frames
  static constexpr uint64_t kIdleFrames = 120;     // bound frames with no wrestler skeleton in the record before everything is released (between matches)
  struct Skeleton { uint32_t object = 0, first = 0, count = 0; int slot = -1; std::string id; };
  struct PartSource { int entry = -1; std::shared_ptr<Model> model; };   // model null: the object's own model
  struct BodyInfo { std::shared_ptr<Model> model; float scale = 0; };
  struct ObjState {
    uint32_t addr = 0;
    uint64_t last_frame = 0, sig_hash = 0, ready_frame = 0, tried_frame = 0;
    std::vector<uint32_t> draws;
    std::vector<int> cands;               // roster entries whose counts fit the drawn parts
    int entry = -1;                       // the one the bytes confirmed
    std::shared_ptr<Model> model;
    bool pending = false;                 // a candidate model is being read
    bool tried = false;
    float scale = 0;
    int skeleton = -1;                    // index into skeletons_ this frame
    bool link_rows_failed = false;        // no skeleton of the node count had rows that fit (the record is not this frame's, or not this body's)
    std::vector<float> first_rows;        // the lent_constants plant
    bool first_draw_rows_valid = false;
    bool is_piece = false;                // S101 lane 48: linked as a piece of a body
    std::vector<int> piece_map;           // piece node (0-based) -> owner body node (0-based), -1 none
    float plant_scale = 0;                // the piece_body_scale plant: another body's scale (st.scale stays the piece's own)
    struct PartSourceSlot { bool resolved = false; int for_entry = -1; PartSource src; };
    std::map<uint32_t, PartSourceSlot> part_src;   // S101 lane 100: part index -> where its bytes come from
    struct NameCache { std::shared_ptr<Model> piece, body; std::vector<int> map; };
    std::map<std::string, NameCache> name_cache;   // owner skeleton id -> the name map for (this model, that body)
    std::map<uint32_t, std::array<uint32_t, 2>> thin_counts;   // lane 129 (KC, review F6): part -> the vertex and index counts of its last full draw (only while thin is wanted; dies with the object)
    std::string skel_id;                  // lane 129 (review F6): the id of the skeleton this object was linked to last (an index into skeletons_ is rebuilt every frame; the id is the identity)
    uint64_t confirm_sig = 0;             // S101 lane 99: the sig_hash the all-draws confirmation below was made for
    uint64_t confirm_next = 0;            // S101 lane 99 (F5): no confirmation attempt before this bound frame (a read failure is retried after kRetryFrames, a loading model next frame)
    std::shared_ptr<std::vector<std::shared_ptr<Model>>> shadow_parts;   // S101 lane 99 (F5): per part, the settled other-attire model (null: the object's own)
    bool wanted_skel = false;             // lane 207: this object's (address, node count) is in the live hook's wanted table
    bool confirmed = false;               // S101 lane 99: every colour draw of the object rebuilds from the joined model with its one scale (ShadowJoinBoard)
  };
  struct ModelSlot { std::shared_ptr<Model> model; std::shared_future<std::shared_ptr<Model>> future; bool started = false, failed = false; int asks = 0; };

  static const float* Reg(const rdr_adapter::ConstantBank& b, int n) { return reinterpret_cast<const float*>(b.dwords) + 4 * n; }
  static uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

  bool Classify(const rdr_adapter::LoadedFrame& frame, const rdr_adapter::VertexLayout& lay, const assets::jboy_parts::Part& part, assets::wrestler_geom::Roles& roles) const {
    std::array<assets::wrestler_geom::LayoutElement, 16> els;
    if (lay.element_count == 0 || lay.element_count > els.size()) return false;
    for (uint32_t e = 0; e < lay.element_count; ++e) {
      const rdr_adapter::VertexElement& el = frame.elements[lay.element_first + e];
      els[e] = {el.vk_format, el.host_offset, el.source_format, el.components, el.flags, el.guest_offset};
    }
    return assets::wrestler_geom::Classify(part, els.data(), lay.element_count, lay.host_stride, lay.guest_stride, roles, !faults_.no_short_maps, faults_.short_map_shift, /*allow_long_map=*/!faults_.no_short_maps);   // lane 134: the short maps are this core's only
  }

  // The model of a roster entry: ready, pending or failed.
  Model* ModelOf(int entry, bool& pending) {
    ModelSlot& m = models_[entry];
    pending = false;
    if (m.model) return m.model.get();
    if (m.failed) return nullptr;
    if (faults_.slow_load && !m.started && ++m.asks <= 3) { pending = true; return nullptr; }   // lane 142: a worker's latency, simulated
    if (!m.started) {
      m.started = true;
      ++stats_.models_started;
      assets::wrestler_roster::Roster* roster = &roster_;
      const int e = entry;
      const bool keep_pac = !faults_.no_pac_keep;
      auto work = [roster, e, keep_pac]() { std::string err; return roster->LoadModel(e, err, keep_pac); };   // lane 142: keep_pac
      if (sync_) {
        try { m.model = work(); } catch (...) { m.model.reset(); }
        m.failed = !m.model;
        return m.model.get();
      }
      m.future = faults_.thread_per_read ? std::async(std::launch::async, work).share() : roster->LoadModelAsync(e, !faults_.no_pac_keep);   // lane 142: one FIFO loader thread (plant thread_per_read: one thread per read)
    }
    if (m.future.valid() && m.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      try {
        m.model = m.future.get();
      } catch (...) {   // the worker threw (out of memory, a bad file): the model is failed, the object stays the stream's
        m.model.reset();
      }
      m.future = {};
      m.failed = !m.model;
      return m.model.get();
    }
    pending = true;
    return nullptr;
  }

  // S101 lane 100. Where the bytes of part `pi` come from: the object's own model when its built part equals the stream draw's bytes (every element), else the first other
  // entry of the SAME character pac whose part `pi` has the draw's counts and does. None fits: the own model (the draw is then refused by the byte check, as before).
  // Decided on the first draw of the part and kept; a model still being read: `pending`.
  const PartSource* ResolvePartSource(const rdr_adapter::LoadedFrame& frame, ObjState& st, uint32_t pi, const rdr_adapter::Draw& dr, const rdr_adapter::VertexLayout& lay,
                                      const ReadGeometry& read, bool& pending) {
    pending = false;
    ObjState::PartSourceSlot& slot = st.part_src[pi];
    if (slot.resolved && slot.for_entry == st.entry) return &slot.src;
    slot = {};
    slot.for_entry = st.entry;
    std::vector<uint8_t> vb;
    if (!st.model || !read || pi >= st.model->parts.parts.size() || !read(dr.vertex_offset, dr.vertex_bytes, vb) || vb.size() < dr.vertex_bytes) return nullptr;   // (tried again at the next draw)
    auto fits = [&](const assets::jboy_parts::Part& part) {
      if (part.vertices != dr.vertex_count || assets::wrestler_geom::DrawIndexCount(part, faults_.flat_indices) != dr.index_count || part.vertices * size_t(lay.host_stride) != dr.vertex_bytes) return false;
      assets::wrestler_geom::Roles roles;
      if (!Classify(frame, lay, part, roles)) return false;
      assets::wrestler_geom::BuildOptions bo;
      bo.scale = st.scale;
      bo.fill0 = Fill0Mode(roles, vb, st.scale, part);
      if (bo.fill0 > 2) return false;
      std::vector<uint8_t> ours;
      assets::wrestler_geom::BuildVertices(part, roles, bo, ours);
      assets::wrestler_geom::CompareResult cr;
      assets::wrestler_geom::Compare(roles, ours, vb.data(), part.vertices, cr);
      return cr.all_ok;
    };
    if (fits(st.model->parts.parts[pi])) { slot.resolved = true; return &slot.src; }
    const auto& entries = roster_.entries();
    const assets::wrestler_roster::Entry& own = entries[size_t(st.entry)];
    for (size_t ei = 0; ei < entries.size(); ++ei) {
      const assets::wrestler_roster::Entry& e = entries[ei];
      if (int(ei) == st.entry || e.pac != own.pac || pi >= e.parts.size()) continue;   // (the node count may differ: the palette rows are proven against the stream's by Decide)
      if (e.parts[pi].vertices != dr.vertex_count || e.parts[pi].index_count != dr.index_count) continue;
      bool pend = false;
      Model* m = ModelOf(int(ei), pend);
      if (pend) { slot = {}; pending = true; return nullptr; }
      if (!m || pi >= m->parts.parts.size() || !fits(m->parts.parts[pi])) continue;
      slot.src.entry = int(ei), slot.src.model = models_[int(ei)].model;
      ++stats_.part_other_attire;
      break;
    }
    slot.resolved = true;
    return &slot.src;
  }

  // Candidates by counts, then the one the bytes confirm, then the body's scale.
  void Resolve(const rdr_adapter::LoadedFrame& frame, ObjState& st, const ReadGeometry& read) {
    // signature of the drawn parts
    if (st.draws.empty() || st.draws[0] >= frame.draws.count) return;   // S101 lane 99 (F2)
    std::map<uint32_t, std::array<uint32_t, 2>> sig;
    uint32_t big = st.draws[0];
    for (uint32_t d : st.draws) {
      const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
      if (!o || d >= frame.draws.count) continue;   // S101 lane 99 (F2)
      const rdr_adapter::Draw& dr = frame.draws[d];
      sig[o->part] = {dr.vertex_count, dr.index_count};
      if (dr.vertex_count > frame.draws[big].vertex_count) big = d;
    }
    uint64_t h = 1469598103934665603ull;
    for (auto& [p, s] : sig) for (uint32_t v : {p, s[0], s[1]}) h = (h ^ v) * 1099511628211ull;
    if (h != st.sig_hash) {
      st.sig_hash = h;
      if (st.model) DropObject(st.addr);   // lane 129 review F5: a join that is reset here (the shadow board's JoinOf resolves first, so this is where it is seen) leaves the thin set
      st.cands.clear();
      st.model.reset();
      st.entry = -1;
      st.tried = false;
      st.scale = 0;
      st.part_src.clear();
      const auto& entries = roster_.entries();
      for (size_t ci = 0; ci < entries.size(); ++ci) {
        bool fits = true;
        for (auto& [p, s] : sig) {
          if (p >= entries[ci].parts.size() || entries[ci].parts[p].vertices != s[0] || entries[ci].parts[p].index_count != s[1]) { fits = false; break; }
        }
        if (fits) st.cands.push_back(int(ci));
      }
      if (!st.cands.empty()) ++stats_.objects_seen;
      if (st.cands.size() > 1) ++stats_.objects_ambiguous;
    }
    st.pending = false;
    if (st.tried && !st.model && frame_no_ - st.tried_frame > kRetryFrames) st.tried = false;   // a failed join is tried again (the first draws may have been unreadable)
    if (st.cands.empty() || st.model || st.tried) return;
    // confirm by bytes: the biggest draw, built from each candidate in turn
    std::vector<Model*> models;
    // lane 142: every candidate's read is STARTED now (they used to start one per frame, in series: 13 attires of one hat = 13 waits); the join still needs all of them (first fit in candidate order).
    for (int ci : st.cands) {
      bool pending = false;
      Model* m = ModelOf(ci, pending);
      if (pending) { st.pending = true; if (faults_.serial_load) break; continue; }   // (plant serial_load: the old one-read-at-a-time wait)
      models.push_back(m);
    }
    (void)big;
    if (st.pending) return;   // (every candidate's read has been started above; the join runs when all are read)
    // Not the first fit only: the object's draws in descending vertex count (a part whose layout this code cannot fill must not leave the whole body unjoined), up to
    // kJoinDraws of them; the first (draw, candidate) whose built bytes equal the stream's wins. A failed join is tried again after kRetryFrames.
    std::vector<uint32_t> order;
    for (uint32_t d : st.draws)
      if (d < frame.draws.count && !(frame.draws[d].flags & rdr_adapter::kDrawThin)) order.push_back(d);   // lane 129 (KC): a lite draw has no bytes to join with
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return frame.draws[a].vertex_count > frame.draws[b].vertex_count; });
    if (order.size() > kJoinDraws) order.resize(kJoinDraws);
    if (order.empty()) return;   // lane 129 review F5: a frame whose draws are all lite has no bytes to join with: no verdict (tried stays false), the next frame with a full draw tries
    st.tried = true;
    st.tried_frame = frame_no_;
    for (uint32_t d : order) {
      const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
      if (!o || d >= frame.draws.count) continue;   // S101 lane 99 (F2)
      const rdr_adapter::Draw& dr = frame.draws[d];
      std::vector<uint8_t> vb;
      if (dr.layout >= frame.layouts.count || !read || !read(dr.vertex_offset, dr.vertex_bytes, vb) || vb.size() < dr.vertex_bytes) continue;
      const rdr_adapter::VertexLayout& lay = frame.layouts[dr.layout];
      if (JoinWith(frame, st, models, o->part, lay, vb)) return;
    }
    ++stats_.objects_unjoined;
  }

  // S101 lane 134: why a candidate did not join (the checker's --refusals prints them as CAND lines).
  void JoinNote(const ObjState& st, uint32_t part_index, size_t cand, const char* what, double scale, double residual, const assets::wrestler_geom::CompareResult* cr = nullptr) {
    if (!diag_cands_ || cand_notes_.size() > 4096) return;
    char b[400];
    const assets::wrestler_roster::Entry& e = roster_.entries()[size_t(st.cands[cand])];
    std::snprintf(b, sizeof b, "JOIN obj %08X part %u cand ch%03d attire %d entry %u nodes %u: %s scale %.6f residual %.3g%s", st.addr, part_index, e.pac, e.attire, e.entry, e.node_count, what, scale, residual, cr ? " (compare)" : "");
    if (cr) { char c[300]; std::snprintf(c, sizeof c, " bad pos %llu weights %llu slots %llu normal %llu tangent %llu colour %llu uv %llu", (unsigned long long)cr->role[0].bad, (unsigned long long)cr->role[1].bad, (unsigned long long)cr->role[2].bad, (unsigned long long)cr->role[3].bad, (unsigned long long)cr->role[4].bad, (unsigned long long)cr->role[5].bad, (unsigned long long)cr->role[6].bad); std::strncat(b, c, sizeof b - std::strlen(b) - 1); }
    cand_notes_.push_back(b);
  }

  bool JoinWith(const rdr_adapter::LoadedFrame& frame, ObjState& st, const std::vector<Model*>& models, uint32_t part_index, const rdr_adapter::VertexLayout& lay, const std::vector<uint8_t>& vb) {
    for (size_t i = 0; i < st.cands.size(); ++i) {
      if (!models[i] || part_index >= models[i]->parts.parts.size()) continue;
      const assets::jboy_parts::Part& part = models[i]->parts.parts[part_index];
      assets::wrestler_geom::Roles roles;
      if (!Classify(frame, lay, part, roles)) {   // lane 134
        if (diag_cands_) {   // (the refusal text is only built for the checker's --refusals)
          std::string why = "classify refused: " + roles.why + " | file strides " + std::to_string(part.stride[0]) + "/" + std::to_string(part.stride[1]) + "/" + std::to_string(part.stride[2]) + "/" + std::to_string(part.stride[3]) + " | stream elements";
          for (uint32_t e = 0; e < lay.element_count; ++e) { const rdr_adapter::VertexElement& el = frame.elements[lay.element_first + e]; why += " f" + std::to_string(el.source_format) + "@" + std::to_string(el.guest_offset); }
          why += " guest stride " + std::to_string(lay.guest_stride);
          JoinNote(st, part_index, i, why.c_str(), 0, 0);
        }
        continue;
      }   // lane 134
      double residual = 0;
      const float scale = assets::wrestler_geom::FitScale(part, roles, vb.data(), &residual);
      if (!(scale > 0.1f && scale < 10.0f) || residual > 1e-5) { JoinNote(st, part_index, i, "scale fit refused", double(scale), residual); continue; }   // lane 134
      assets::wrestler_geom::BuildOptions bo;
      bo.scale = scale;
      bo.fill0 = Fill0Mode(roles, vb, scale, part);
      if (bo.fill0 > 2) { JoinNote(st, part_index, i, "fill0 unknown", double(scale), residual); continue; }   // lane 134
      std::vector<uint8_t> ours;
      assets::wrestler_geom::BuildVertices(part, roles, bo, ours);
      assets::wrestler_geom::CompareResult cr;
      assets::wrestler_geom::Compare(roles, ours, vb.data(), part.vertices, cr);
      if (!cr.all_ok) { JoinNote(st, part_index, i, "bytes differ", double(scale), residual, &cr); continue; }   // lane 134
      st.entry = st.cands[i];
      st.model = models_[st.entry].model;
      st.scale = scale;
      ++stats_.objects_joined, ++stats_.scale_measured;
      {
        const assets::wrestler_roster::Entry& e = roster_.entries()[size_t(st.entry)];
        JoinedBody jb{e.pac, e.attire, e.entry, e.node_count, scale};
        bool seen = false;
        for (const JoinedBody& j : joined_) seen = seen || (j.pac == jb.pac && j.attire == jb.attire && j.entry == jb.entry && j.scale == jb.scale);
        if (!seen) joined_.push_back(jb);
      }
      if (faults_.piece_wrong_entry && roster_.entries()[size_t(st.entry)].node_count < 40) {   // lane 134, planted: a piece joined to the NEXT entry of its pac / attire (the previous one when it is the last)
        const auto& entries = roster_.entries();
        const size_t me = size_t(st.entry);
        for (int d : {1, -1}) {
          const int ci = int(me) + d;
          if (ci < 0 || size_t(ci) >= entries.size() || entries[size_t(ci)].pac != entries[me].pac || entries[size_t(ci)].attire != entries[me].attire) continue;
          bool pending = false;
          Model* m = ModelOf(ci, pending);
          if (m) { st.model = models_[ci].model; st.entry = ci; break; }
        }
      }
      if (faults_.wrong_attire) {   // planted: another roster model with the same node count (another attire), counts unchecked
        const auto& entries = roster_.entries();
        for (size_t ci = 0; ci < entries.size(); ++ci) {
          if (int(ci) == st.entry || entries[ci].node_count != entries[size_t(st.entry)].node_count || entries[ci].parts.size() < entries[size_t(st.entry)].parts.size()) continue;
          bool pending = false;
          Model* m = ModelOf(int(ci), pending);
          if (m) { st.model = models_[int(ci)].model; st.entry = int(ci); break; }
        }
      }
      return true;
    }
    return false;
  }

  // S101 lane 99: does every colour draw of the object rebuild bit for bit from the joined model (or, per part, the settled other-attire source, lane 100) with its one scale? (draws without
  // geometry or a layout are skipped, as the unit test's join does.) 1 yes, 0 no, -1 a model is still being read, -2 a draw could not be read (not a verdict).
  int ConfirmAll(const rdr_adapter::LoadedFrame& frame, ObjState& st, const ReadGeometry& read) {
    if (!st.model || !read) return -2;
    auto parts = std::make_shared<std::vector<std::shared_ptr<Model>>>(st.model->parts.parts.size());
    uint32_t checked = 0;
    for (uint32_t d : st.draws) {
      if (d >= frame.draws.count) continue;
      const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
      const rdr_adapter::Draw& dr = frame.draws[d];
      if (o && (dr.flags & rdr_adapter::kDrawThin) && o->part < parts->size()) {   // lane 129 review F5: a lite draw has no bytes to re-check; its part keeps the source lane 100 settled (the last full draw's)
        const auto ps = st.part_src.find(o->part);
        if (ps != st.part_src.end() && ps->second.resolved && ps->second.for_entry == st.entry) (*parts)[o->part] = ps->second.src.model;
        continue;
      }
      if (!o || dr.layout >= frame.layouts.count || (dr.flags & rdr_adapter::kDrawNoGeometry)) continue;
      if (o->part >= st.model->parts.parts.size()) return 0;
      const rdr_adapter::VertexLayout& lay = frame.layouts[dr.layout];
      bool pending = false;
      const PartSource* src = ResolvePartSource(frame, st, o->part, dr, lay, read, pending);
      if (pending) return -1;
      const std::shared_ptr<Model> alt = src ? src->model : nullptr;
      if (alt && o->part >= alt->parts.parts.size()) return 0;
      const assets::jboy_parts::Part& part = alt ? alt->parts.parts[o->part] : st.model->parts.parts[o->part];
      if (!part.ok || part.vertices != dr.vertex_count) return 0;
      std::vector<uint8_t> vb;
      if (!read(dr.vertex_offset, dr.vertex_bytes, vb) || vb.size() < dr.vertex_bytes) return -2;
      assets::wrestler_geom::Roles roles;
      if (!Classify(frame, lay, part, roles)) return 0;
      assets::wrestler_geom::BuildOptions bo;
      bo.scale = st.scale;
      bo.fill0 = Fill0Mode(roles, vb, st.scale, part);
      if (bo.fill0 > 2) return 0;
      std::vector<uint8_t> ours;
      assets::wrestler_geom::BuildVertices(part, roles, bo, ours);
      assets::wrestler_geom::CompareResult cr;
      assets::wrestler_geom::Compare(roles, ours, vb.data(), part.vertices, cr);
      if (!cr.all_ok) return 0;
      (*parts)[o->part] = alt;
      ++checked;
    }
    if (!checked) return 0;
    st.shadow_parts = parts;
    return 1;
  }

  uint8_t Fill0Mode(const assets::wrestler_geom::Roles& roles, const std::vector<uint8_t>& vb, float scale, const assets::jboy_parts::Part& part) const {
    // Only the two modes measured on 7051: {0,0,0,255} (1) and {0,0,0, high byte of the position x float} (2). Anything else is 3 = unknown: the caller refuses.
    for (const auto& r : roles.r) {
      if (r.role != assets::wrestler_geom::Role::Fill0) continue;
      if (vb.size() < size_t(r.offset) + 4) return 3;
      if (vb[r.offset] != 0 || vb[size_t(r.offset) + 1] != 0 || vb[size_t(r.offset) + 2] != 0) return 3;
      const uint8_t b3 = vb[size_t(r.offset) + 3];
      if (b3 == 255) return 1;
      float x = assets::BeF(part.s[0]) * scale;
      uint32_t xb;
      std::memcpy(&xb, &x, 4);
      return uint8_t(xb >> 24) == b3 ? 2 : 3;
    }
    return 0;   // no fill0 element in this layout
  }

  // S101 lane 206 (review B1), the two rules of LinkObject for a model under the body size, public so the checker's --link-rule-test can pin them:
  // a declared link to a small skeleton holds only when its rows are the draws' rows; a same-count miss of a small model does not close the piece route (LinkPiece, by node name).
 public:
  static bool DeclaredLinkOk(bool small_model, double rows_score, bool checks_off) { return !small_model || checks_off || rows_score >= 0.3; }
  static bool MissClosesPieceRoute(bool same_count, bool small_model) { return same_count && !small_model; }
 private:
  void LinkObject(const rdr_adapter::LoadedFrame& frame, ObjState& st) {
    st.skeleton = -1;
    st.link_rows_failed = false;
    st.is_piece = false;
    if (!st.model) return;
    const Model& model = *st.model;
    // S101 lane 206 (review B1): a model under the body size is maybe a piece borrowing its body's rows by name (LinkPiece). A small recorded skeleton (a hair's own, now kept by the hook) is
    // accepted for it only when its rows are the draws' rows (score >= 0.3); any other same-count small skeleton is NOT a failed link: the piece path stays open (skeleton -1, link_rows_failed false).
    const bool small_model = model.parts.node_count < kBodySkeletonNodes;
    if (small_model && !st.wanted_skel && model.parts.node_count) {   // lane 207
      auto fn = skel_want_ ? skel_want_ : g_skeleton_want_hooks.want;
      if (fn) st.wanted_skel = true, fn(st.addr, model.parts.node_count);
    }
    auto score_of = [&](const Skeleton& sk) {
      uint64_t rows = 0, exact = 0;
      for (uint32_t d : st.draws) {
        const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
        const rdr_adapter::Draw& dr = frame.draws[d];
        if (o->part >= model.parts.parts.size() || dr.vs_bank >= frame.constant_banks.count) continue;
        const assets::jboy_parts::Part& part = model.parts.parts[o->part];
        const rdr_adapter::ConstantBank& bank = frame.constant_banks[dr.vs_bank];
        for (uint32_t k = 0; k < part.palette_count; ++k) {
          if (part.palette[k] < 1 || part.palette[k] > sk.count) continue;
          const rdr_scene::Mat4& M = rec_->bones[sk.first + part.palette[k] - 1];
          for (int j = 0; j < 3; ++j) {
            const float* g = Reg(bank, 76 + 3 * int(k) + j);
            bool same = true;
            for (int c = 0; c < 4; ++c) same = same && Bits(g[c]) == Bits(M.m[c * 4 + j]);
            ++rows, exact += same;
          }
        }
      }
      return rows ? double(exact) / double(rows) : 0.0;
    };
    // declared: the record names the engine address
    if (!rec_->engine_objects.empty()) {
      for (size_t si = 0; si < skeletons_.size(); ++si) {
        if (skeletons_[si].object < rec_->engine_objects.size() && rec_->engine_objects[skeletons_[si].object] == st.addr && skeletons_[si].count == model.parts.node_count) {
          if (small_model && !DeclaredLinkOk(small_model, score_of(skeletons_[si]), faults_.checks_off)) break;   // lane 206: its own rows are not the draws': try the piece path
          st.skeleton = int(si);
          ++stats_.links_declared;
          RegisterBody(skeletons_[si], st);
          return;
        }
      }
    }
    double best = -1;
    bool same_count = false;
    for (size_t si = 0; si < skeletons_.size(); ++si) {
      const Skeleton& sk = skeletons_[si];
      if (sk.count != model.parts.node_count) continue;
      same_count = true;
      const double score = score_of(sk);
      if (score > best) best = score, st.skeleton = int(si);
    }
    // (checks_off, a plant's picture: the best skeleton of the same node count is taken whatever its rows say)
    if (best >= 0.3 || (faults_.checks_off && best >= 0)) { ++stats_.links_by_rows; RegisterBody(skeletons_[size_t(st.skeleton)], st); }
    else st.skeleton = -1, st.link_rows_failed = MissClosesPieceRoute(same_count, small_model), ++stats_.links_none;   // lane 206 B1: a small model's same-count miss never closes the piece path
  }

  // S101 lane 48. A body (a joined model whose node count is its skeleton's) is remembered by the skeleton's record id, with its node names and its measured scale.
  void RegisterBody(const Skeleton& sk, const ObjState& st) {
    if (!st.model || sk.id.empty() || st.model->parts.node_count != sk.count) return;
    bodies_[sk.id] = BodyInfo{st.model, st.scale};
  }

  // A model with fewer nodes than the smallest skeleton of the record's frame, joined, with no skeleton of its own: a piece (hair, pads, alpha pieces).
  bool PieceCandidate(const ObjState& st) const {
    return st.model && st.skeleton < 0 && !st.link_rows_failed && min_skel_nodes_ && st.model->parts.node_count < min_skel_nodes_;
  }

  const std::vector<int>& CachedNameMap(ObjState& st, const std::string& body_id, const std::shared_ptr<Model>& body) {
    ObjState::NameCache& c = st.name_cache[body_id];
    if (c.piece != st.model || c.body != body) c = {st.model, body, NameMap(*st.model, *body)};
    return c.map;
  }

  std::vector<int> NameMap(const Model& piece, const Model& body) const {
    std::vector<int> map(piece.parts.node_names.size(), -1);
    const std::vector<std::string>& bn = body.parts.node_names;
    for (size_t n = 0; n < map.size(); ++n)
      for (size_t b = 0; b < bn.size(); ++b)
        if (!piece.parts.node_names[n].empty() && bn[b] == piece.parts.node_names[n]) { map[n] = int(b); break; }
    return map;
  }

  // A piece (hair, pads, the alpha pieces) has its own small node table and no record skeleton. Each of its palette slots names a node of that table; the OWNER body's node of
  // the same NAME carries the rows (measured on run 7051: "Bip Head" -> Brock node 24 / ch187 node 18, "l_mabuta" -> 43, "l_meziri" -> 35, "r_mabuta" -> 44, "r_meziri" -> 29, every row
  // bit-equal). The owner is the skeleton whose body, by those names, gives rows bit-equal for >= 30 % of the draws' slots; Decide re-proves every draw.
  void LinkPiece(const rdr_adapter::LoadedFrame& frame, ObjState& st) {
    if (!PieceCandidate(st)) return;
    const Model& pm = *st.model;
    double best = -1;
    int best_si = -1;
    std::vector<int> best_map;
    std::vector<std::pair<int, double>> scored;
    for (size_t si = 0; si < skeletons_.size(); ++si) {
      const Skeleton& sk = skeletons_[si];
      auto bi = bodies_.find(sk.id);
      if (bi == bodies_.end() || !bi->second.model || bi->second.model->parts.node_count != sk.count) continue;
      const std::vector<int>& map = CachedNameMap(st, sk.id, bi->second.model);
      uint64_t rows = 0, exact = 0;
      for (uint32_t d : st.draws) {
        const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
        const rdr_adapter::Draw& dr = frame.draws[d];
        if (o->part >= pm.parts.parts.size() || dr.vs_bank >= frame.constant_banks.count) continue;
        const assets::jboy_parts::Part& part = pm.parts.parts[o->part];
        const rdr_adapter::ConstantBank& bank = frame.constant_banks[dr.vs_bank];
        for (uint32_t k = 0; k < part.palette_count; ++k) {
          const uint32_t p = part.palette[k];
          const int bnode = p >= 1 && p <= map.size() ? map[p - 1] : -1;
          for (int j = 0; j < 3; ++j) {
            ++rows;
            if (bnode < 0 || uint32_t(bnode) >= sk.count) continue;
            const rdr_scene::Mat4& M = rec_->bones[sk.first + uint32_t(bnode)];
            const float* g = Reg(bank, 76 + 3 * int(k) + j);
            bool same = true;
            for (int c = 0; c < 4; ++c) same = same && Bits(g[c]) == Bits(M.m[c * 4 + j]);
            exact += same;
          }
        }
      }
      const double score = rows ? double(exact) / double(rows) : 0;
      scored.push_back({int(si), score});
      if (score > best) best = score, best_si = int(si), best_map = map;
    }
    if (faults_.piece_wrong_owner && scored.size() > 1) {   // planted: the body with the LOWEST score (another body)
      auto lo = std::min_element(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
      best_si = lo->first;
      best = lo->second;
      best_map = CachedNameMap(st, skeletons_[size_t(best_si)].id, bodies_[skeletons_[size_t(best_si)].id].model);
    }
    if (best_si >= 0 && (best >= 0.3 || faults_.checks_off || faults_.piece_wrong_owner)) {
      st.skeleton = best_si;
      st.is_piece = true;
      st.piece_map = best_map;
      if (faults_.piece_body_scale) {   // planted: the piece takes a body's scale that is not its own (in 7051 a piece's own scale equals its owner's, so another body's)
        st.plant_scale = 0;
        for (const auto& kv : bodies_) if (std::fabs(kv.second.scale - st.scale) > 1e-3f) { st.plant_scale = kv.second.scale; break; }
      }
      ++stats_.piece_objects_linked;
    } else {
      st.link_rows_failed = best_si >= 0;   // bodies exist but their rows are not this piece's: Palette, never drawn wrong
    }
  }

  // S101 lane 262 (run 7177, 30 Years era presentation: 2,552 draws refused on bytes with verify bad 0, i.e. GetPrepared returned null): says WHICH object and part, and what the stream's
  // first vertex holds (host layout), once per (object, part), so the next log names the cause instead of a count.
  void NoteBytes(const ObjState& st, uint32_t part_index, const rdr_adapter::Draw& dr, const rdr_adapter::VertexLayout& lay, const ReadGeometry& read) {
    if (bytes_notes_.size() >= 6 || !bytes_noted_.insert(uint64_t(st.addr) << 8 | part_index).second) return;
    const auto& entries = roster_.entries();
    char hb[24];
    std::snprintf(hb, sizeof hb, "%08X", st.addr);
    std::string t = std::string("object ") + hb + " ";
    if (st.entry >= 0 && size_t(st.entry) < entries.size()) {
      const auto& e = entries[size_t(st.entry)];
      t += "ch" + std::to_string(e.pac) + " a" + std::to_string(e.attire) + " entry " + std::to_string(e.entry) + " nodes " + std::to_string(e.node_count) + (st.is_piece ? " piece " : " ");
    }
    t += "part " + std::to_string(part_index) + " draw vertices " + std::to_string(dr.vertex_count) + " bytes " + std::to_string(dr.vertex_bytes) + " flags " + std::to_string(unsigned(dr.flags)) + " host stride " + std::to_string(lay.host_stride) + " scale " + std::to_string(st.scale);
    std::vector<uint8_t> vb;
    if (read && lay.host_stride && dr.vertex_bytes >= lay.host_stride && read(dr.vertex_offset, lay.host_stride, vb) && vb.size() >= lay.host_stride) {
      t += " first vertex";
      static const char* hx = "0123456789abcdef";
      for (size_t i = 0; i < vb.size() && i < 48; ++i) t += ' ', t += hx[vb[i] >> 4], t += hx[vb[i] & 15];
    } else {
      t += " first vertex unreadable";
    }
    bytes_notes_.push_back(t);
  }

  WrestlerPrepared* GetPrepared(const rdr_adapter::LoadedFrame& frame, ObjState& st, uint32_t object, uint32_t part_index, const assets::jboy_parts::Part& part, const assets::wrestler_geom::Roles& roles,
                                const rdr_adapter::VertexLayout& lay, const rdr_adapter::Draw& dr, const ReadGeometry& read, int entry, int& pin_state) {
    (void)lay;
    uint64_t sig = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { sig = (sig ^ v) * 1099511628211ull; };
    mix(roles.host_stride);
    mix(roles.tangent_not_in_file ? 1 : 0);
    for (const auto& r : roles.r) mix((uint64_t(uint8_t(r.role)) << 32) | (uint64_t(r.offset) << 16) | r.vk);
    const float own_scale = (faults_.piece_body_scale && st.is_piece && st.plant_scale > 0) ? st.plant_scale : st.scale;   // (the plant: another body's scale)
    const float scale = faults_.scale_one ? 1.0f : own_scale;
    uint32_t sb;
    std::memcpy(&sb, &scale, 4);
    // The stream's FIRST vertex tells what the vertex shader's fetch selects did: fill0 (the dummy 8888 element) is read off it, and a hash of its bytes outside the position
    // goes into the key, so two shaders whose fetches fill an element differently (the uv order, a patched constant) never share one built buffer.
    // S101 lane 88 (SKIP_GAME_WORK_MAP.md D1): a part that passed its full check (the first vertex read, hashed and classified, the bytes verified) is pinned by (object, part): the same
    // model entry, vertex / index offsets and byte counts, layout signature and scale take the built part straight, with no read of the stream. Its proof frame (kWrestlerProofEvery, staggered by
    // key) runs the full path and the byte verify; a changed identity runs it at once. The palette rows are NOT pinned: they change every frame and are compared on every draw.
    const uint64_t pin_id = (uint64_t(object) << 32) | part_index;
    const bool pins_on = kWrestlerPins && !verify_all_ && !faults_.checks_off;
    if (pins_on) {
      const auto pi = pins_.find(pin_id);
      if (pi != pins_.end()) {
        const Pin& pn = pi->second;
        if (pn.entry == uint32_t(entry) &&   // (the settled part source: a source change unpins the part)
            pn.sig == sig && pn.scale_bits == sb && pn.vertex_offset == dr.vertex_offset && pn.index_offset == dr.index_offset && pn.vertex_bytes == dr.vertex_bytes &&
            pn.index_bytes == dr.index_bytes && pn.uv_swap == faults_.uv_swap) {
          const bool proof = !faults_.no_proof_frames && ProofDue(pin_id, frame_no_, kWrestlerProofEvery);
          const auto it = prepared_.find(pn.key);
          if (it != prepared_.end()) {
            if (!proof) return pin_state = 0, ++stats_.pinned_draws, it->second.get();
            pin_state = 1, ++stats_.proof_draws;
          }
        }
      }
      if (pin_state == 2) ++stats_.fresh_draws;
    }
    uint8_t fill0 = 0;
    bool has_fill0 = false;
    for (const auto& r : roles.r) has_fill0 = has_fill0 || r.role == assets::wrestler_geom::Role::Fill0;
    std::vector<uint8_t> vb0;
    if (!read || dr.vertex_bytes < roles.host_stride) return ++stats_.bytes_stride, nullptr;   // lane 262: the draw holds less than one vertex
    if (!read(dr.vertex_offset, roles.host_stride, vb0) || vb0.size() < roles.host_stride) return ++stats_.bytes_read, nullptr;   // lane 262: the stream's first vertex is unreadable
    uint64_t first_hash = 1469598103934665603ull;
    for (uint32_t i = 0; i < roles.host_stride; ++i) {
      bool in_pos = false;
      for (const auto& r : roles.r) in_pos = in_pos || (r.role == assets::wrestler_geom::Role::Pos && i >= r.offset && i < uint32_t(r.offset) + 16u);
      if (!in_pos) first_hash = (first_hash ^ vb0[i]) * 1099511628211ull;
    }
    if (has_fill0) {
      fill0 = Fill0Mode(roles, vb0, own_scale, part);
      if (faults_.fill0_unknown) fill0 = 3;   // lane 262 plant
      if (fill0 > 2) return ++stats_.bytes_fill0, nullptr;   // neither known mode: refused (counted; lane 262: its bytes are noted by NoteBytes), never guessed
    }
    const Key key{uint64_t(uint32_t(entry)) << 32 | part_index, sig, sb, uint64_t(fill0) | (faults_.uv_swap ? 2u : 0u), first_hash};
    if (pins_on) {
      const auto old_pin = pins_.find(pin_id);
      if (old_pin != pins_.end() && old_pin->second.key != key) {   // lane 220 (run 7134, the Undertaker's entrance: 2 thin geometry proof mismatches): WHICH field of the key moved, so the next log names the cause
        ++stats_.rekeyed;
        if (rekey_notes_.size() < 6) {
          const Key& k0 = old_pin->second.key;
          char nb[200];
          std::snprintf(nb, sizeof nb, "object %08X part %u:%s%s%s%s%s", object, part_index, k0[0] != key[0] ? " entry" : "", k0[1] != key[1] ? " layout" : "", k0[2] != key[2] ? " scale" : "", k0[3] != key[3] ? " fill0" : "", k0[4] != key[4] ? " first-vertex-bytes" : "");
          rekey_notes_.push_back(nb);
        }
      }
      pins_[pin_id] = Pin{uint32_t(entry), sig, sb, dr.vertex_offset, dr.index_offset, dr.vertex_bytes, dr.index_bytes, faults_.uv_swap, key};
    }
    auto it = prepared_.find(key);
    if (it != prepared_.end()) return it->second.get();
    (void)frame;
    auto pr = std::make_unique<WrestlerPrepared>();
    assets::wrestler_geom::BuildOptions bo;
    bo.scale = scale, bo.uv_swap = faults_.uv_swap, bo.fill0 = fill0;
    assets::wrestler_geom::BuildVertices(part, roles, bo, pr->vertex);
    pr->index = assets::wrestler_geom::BuildIndices(part, faults_.flat_indices);   // lane 179: groups joined by 0xFFFF
    pr->vertex_count = part.vertices, pr->stride = roles.host_stride, pr->tangent_not_in_file = roles.tangent_not_in_file;
    ++stats_.prepared_built;
    stats_.part_bytes_vertex += pr->vertex.size(), stats_.part_bytes_index += pr->index.size() * 2;
    return (prepared_[key] = std::move(pr)).get();
  }

  bool VerifyBytes(const rdr_adapter::LoadedFrame& frame, const rdr_adapter::Draw& dr, const assets::wrestler_geom::Roles& roles, const WrestlerPrepared& pr,
                   const assets::jboy_parts::Part& part, const ReadGeometry& read) {
    (void)frame;
    (void)part;
    if (!read) return false;
    std::vector<uint8_t> vb, ib;
    if (dr.vertex_bytes != pr.vertex.size() || !read(dr.vertex_offset, dr.vertex_bytes, vb) || vb.size() < dr.vertex_bytes) return verify_what_ = "vertex_size", false;
    assets::wrestler_geom::CompareResult cr;
    assets::wrestler_geom::Compare(roles, pr.vertex, vb.data(), pr.vertex_count, cr);
    if (!cr.all_ok) verify_what_ = "vertex_bytes", verify_role_ = assets::wrestler_geom::RoleName(cr.first_bad_role), verify_vertex_ = cr.first_bad_vertex, verify_err_ = cr.role[uint8_t(cr.first_bad_role)].max_err, verify_bad_ = cr.role[uint8_t(cr.first_bad_role)].bad;
    if (!cr.all_ok)
      for (const assets::wrestler_geom::Roles::R& r : roles.r)
        if (r.role == cr.first_bad_role) {
          std::memcpy(&verify_ours_, pr.vertex.data() + size_t(cr.first_bad_vertex) * roles.host_stride + r.offset, 4);
          std::memcpy(&verify_theirs_, vb.data() + size_t(cr.first_bad_vertex) * roles.host_stride + r.offset, 4);
        }
    for (const assets::wrestler_geom::RoleCompare& rc : cr.role) stats_.cmp_values += rc.values, stats_.cmp_exact += rc.exact, stats_.cmp_near += rc.near, stats_.cmp_bad += rc.bad;
    if (!cr.all_ok) return false;
    if (dr.index_bytes != pr.index.size() * 2 || dr.index_32bit || !read(dr.index_offset, dr.index_bytes, ib) || ib.size() < dr.index_bytes) return verify_what_ = "index_size", false;
    if (!pr.index.empty() && std::memcmp(ib.data(), pr.index.data(), ib.size())) return verify_what_ = "index_bytes", false;
    return true;
  }

  void Sweep() {
    if (frame_no_ % 60) return;
    for (auto it = prepared_.begin(); it != prepared_.end();) {
      if (frame_no_ - it->second->last_use > 240) {
        if (release_) release_(*it->second);
        ++stats_.released;
        it = prepared_.erase(it);
      } else ++it;
    }
    for (auto pi = pins_.begin(); pi != pins_.end();) pi = prepared_.count(pi->second.key) ? std::next(pi) : pins_.erase(pi);
    DropUnusedModels();
  }

  // The models no object refers to any more (a match ended, another began): their blobs are released. A model still being read is kept until it finishes.
  void DropUnusedModels() {
    std::set<int> used;
    for (const auto& kv : objects_) {
      if (kv.second.entry >= 0) used.insert(kv.second.entry);
      for (int c : kv.second.cands) used.insert(c);
      for (const auto& ps : kv.second.part_src) if (ps.second.src.entry >= 0) used.insert(ps.second.src.entry);
    }
    for (auto it = models_.begin(); it != models_.end();) {
      const bool reading = it->second.future.valid();
      if (!used.count(it->first) && !reading) it = models_.erase(it);
      else ++it;
    }
  }

  // Everything goes: built parts (the hook frees their buffers), objects, models. Between matches / on a load screen (no wrestler skeleton in the record for a while).
  void ReleaseAll() {
    roster_.TrimIdle(0);   // lane 142: everything goes, the kept pac too
    ++global_release_seq_;   // lane 129 (KC)
    for (auto& kv : prepared_) {
      if (release_) release_(*kv.second);
      ++stats_.released;
    }
    prepared_.clear();
    pins_.clear();
    rekey_notes_.clear();
    for (auto& kv : objects_) UnwantObject(kv.second);
    objects_.clear();
    bodies_.clear();
    DropUnusedModels();
  }

  std::vector<uint32_t> dropped_objects_;   // lane 129 (KC): until TakeDrops
  uint32_t drop_seq_ = 0, global_release_seq_ = 0;
  bool drops_wanted_ = false;
  using Key = std::array<uint64_t, 5>;
  struct Pin {
    uint32_t entry = 0;
    uint64_t sig = 0;
    uint32_t scale_bits = 0;
    uint64_t vertex_offset = 0, index_offset = 0;
    uint32_t vertex_bytes = 0, index_bytes = 0;
    bool uv_swap = false;
    Key key{};
  };
  std::unordered_map<uint64_t, Pin> pins_;   // (object, part) -> what its full check passed against

  assets::wrestler_roster::Roster& roster_;
  WrestlerFaults faults_;
  bool sync_, verify_all_;
  std::shared_ptr<const SceneTables> record_next_, record_prev_, rec_;
  std::vector<Skeleton> skeletons_;
  uint32_t min_skel_nodes_ = 0;
  static constexpr uint32_t kBodySkeletonNodes = 60;   // = WrestlerContributor::kDefaultMinNodes (a core does not include the live layer)
  std::map<std::string, BodyInfo> bodies_;   // S101 lane 48: record id -> joined body
  std::unordered_map<uint32_t, ObjState> objects_;
  std::map<int, ModelSlot> models_;
  std::map<Key, std::unique_ptr<WrestlerPrepared>> prepared_;
  std::vector<std::string> rekey_notes_;   // lane 220: the first re-keyed pins, by field
  std::vector<std::string> bytes_notes_;   // lane 262: the first (object, part)s refused on bytes, with the stream's first vertex
  std::set<uint64_t> bytes_noted_;
  std::vector<float> rows_;
  std::function<void(WrestlerPrepared&)> release_;
  void (*skel_want_)(uint32_t, uint32_t) = nullptr;   // (a test's own; the app's are in g_skeleton_want_hooks)
  void (*skel_unwant_)(uint32_t) = nullptr;
  void UnwantObject(ObjState& st) {
    auto fn = skel_unwant_ ? skel_unwant_ : g_skeleton_want_hooks.unwant;
    if (st.wanted_skel && fn) fn(st.addr);
    st.wanted_skel = false;
  }
  WrestlerStats stats_;
  std::vector<JoinedBody> joined_;
  uint64_t frame_no_ = 0, idle_ = 0;
  std::vector<RefusalNote> notes_;
  std::vector<std::string> cand_notes_;
  std::set<uint64_t> cand_noted_;
 public:
  bool diag_cands_ = false;   // S101 lane 100: the checker's --refusals asks for the candidate comparison
 private:
  // For a part whose bytes failed: every candidate model of the object built for this draw and compared (which models carry this part's bytes?).
  void NoteCandidates(const rdr_adapter::LoadedFrame& frame, ObjState& st, uint32_t pi, const rdr_adapter::Draw& dr, const rdr_adapter::VertexLayout& lay, const ReadGeometry& read) {
    std::vector<uint8_t> vb;
    if (!read(dr.vertex_offset, dr.vertex_bytes, vb)) return;
    for (int ci : st.cands) {
      bool pending = false;
      Model* m = ModelOf(ci, pending);
      char b[320];
      if (!m || pi >= m->parts.parts.size()) { std::snprintf(b, sizeof b, "obj %08X part %u cand entry#%d: no model", st.addr, pi, ci); cand_notes_.push_back(b); continue; }
      const assets::jboy_parts::Part& part = m->parts.parts[pi];
      assets::wrestler_geom::Roles roles;
      if (!Classify(frame, lay, part, roles)) { std::snprintf(b, sizeof b, "obj %08X part %u cand entry#%d: classify refused", st.addr, pi, ci); cand_notes_.push_back(b); continue; }
      assets::wrestler_geom::BuildOptions bo;
      bo.scale = st.scale;
      bo.fill0 = Fill0Mode(roles, vb, st.scale, part);
      std::vector<uint8_t> ours;
      assets::wrestler_geom::BuildVertices(part, roles, bo, ours);
      assets::wrestler_geom::CompareResult cr;
      assets::wrestler_geom::Compare(roles, ours, vb.data(), part.vertices, cr);
      const assets::wrestler_roster::Entry& e = roster_.entries()[size_t(ci)];
      std::snprintf(b, sizeof b, "obj %08X part %u cand entry#%d ch%03d attire %d entry %u nodes %u%s: bad pos %llu weights %llu slots %llu normal %llu tangent %llu colour %llu uv %llu fill0 %llu",
                    st.addr, pi, ci, e.pac, e.attire, e.entry, e.node_count, ci == st.entry ? " (JOINED)" : "", (unsigned long long)cr.role[0].bad, (unsigned long long)cr.role[1].bad,
                    (unsigned long long)cr.role[2].bad, (unsigned long long)cr.role[3].bad, (unsigned long long)cr.role[4].bad, (unsigned long long)cr.role[5].bad, (unsigned long long)cr.role[6].bad, (unsigned long long)cr.role[7].bad);
      cand_notes_.push_back(b);
      if (ci == st.entry) {   // ANY roster part (every entry, every part index) with this part's counts whose bytes fit the stream's draw
        const auto& all = roster_.entries();
        for (size_t ei = 0; ei < all.size(); ++ei)
          for (size_t pj = 0; pj < all[ei].parts.size(); ++pj) {
            if (all[ei].parts[pj].vertices != dr.vertex_count || all[ei].parts[pj].index_count != dr.index_count) continue;
            bool pend = false;
            Model* m2 = ModelOf(int(ei), pend);
            if (!m2 || pj >= m2->parts.parts.size()) continue;
            const assets::jboy_parts::Part& p2 = m2->parts.parts[pj];
            assets::wrestler_geom::Roles r2;
            if (!Classify(frame, lay, p2, r2)) continue;
            assets::wrestler_geom::BuildOptions b2;
            b2.scale = st.scale, b2.fill0 = Fill0Mode(r2, vb, st.scale, p2);
            std::vector<uint8_t> o2;
            assets::wrestler_geom::BuildVertices(p2, r2, b2, o2);
            assets::wrestler_geom::CompareResult c2;
            assets::wrestler_geom::Compare(r2, o2, vb.data(), p2.vertices, c2);
            char w[200];
            std::snprintf(w, sizeof w, "   same-counts part: entry#%zu ch%03d attire %d entry %u part %zu: %s", ei, all[ei].pac, all[ei].attire, all[ei].entry, pj, c2.all_ok ? "ALL BYTES FIT" : "no");
            if (c2.all_ok) cand_notes_.push_back(w);
          }
      }
      if (ci == st.entry) {   // the joined model: the first differing vertices (uv, tangent), ours against the stream's
        int shown = 0;
        for (const assets::wrestler_geom::Roles::R& r : roles.r) {
          if (r.role != assets::wrestler_geom::Role::Uv && r.role != assets::wrestler_geom::Role::Tangent) continue;
          shown = 0;
          for (uint32_t v = 0; v < part.vertices && shown < 100000; ++v) {
            uint8_t* a = ours.data() + size_t(v) * roles.host_stride + r.offset;
            const uint8_t* t = vb.data() + size_t(v) * roles.host_stride + r.offset;
            const size_t n = r.role == assets::wrestler_geom::Role::Uv ? 4 : 8;
            if (!std::memcmp(a, t, n)) continue;
            ++shown;
            char w[200];
            if (r.role == assets::wrestler_geom::Role::Uv) {
              uint16_t ha[2], ht[2];
              std::memcpy(ha, a, 4), std::memcpy(ht, t, 4);
              std::snprintf(w, sizeof w, "   v%u uv ours (%04X,%04X) stream (%04X,%04X)", v, ha[0], ha[1], ht[0], ht[1]);
            } else {
              int16_t qa[4], qt[4];
              std::memcpy(qa, a, 8), std::memcpy(qt, t, 8);
              std::snprintf(w, sizeof w, "   v%u tangent ours (%d,%d,%d) stream (%d,%d,%d)", v, qa[0], qa[1], qa[2], qt[0], qt[1], qt[2]);
            }
            cand_notes_.push_back(w);
          }
        }
      }
    }
  }
  const char* verify_what_ = "ok";
  const char* verify_role_ = "";
  uint32_t verify_vertex_ = 0;
  double verify_err_ = 0;
  uint64_t verify_bad_ = 0;
  uint32_t verify_ours_ = 0, verify_theirs_ = 0;
};

}  // namespace rdr_scene
