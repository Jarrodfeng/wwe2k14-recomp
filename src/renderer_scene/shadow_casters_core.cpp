// See shadow_casters_core.h. Session 101, lane 99.
#include "shadow_casters_core.h"

#include "../assets/wrestler_geom.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rdr_scene {

using rdr_adapter::KeepWhy;
namespace sv = assets::shadow_vertex;

bool ShadowFaults::Parse(const std::string& list, ShadowFaults& f, std::string& error) {
  size_t at = 0;
  while (at <= list.size()) {
    size_t comma = list.find(',', at);
    if (comma == std::string::npos) comma = list.size();
    const std::string w = list.substr(at, comma - at);
    at = comma + 1;
    if (w.empty()) continue;
    if (w == "palette_from_colour") f.palette_from_colour = true;
    else if (w == "fetch_order") f.fetch_order = true;
    else if (w == "weights_as_fill") f.weights_as_fill = true;
    else if (w == "xdword_colour") f.xdword_colour = true;
    else if (w == "bone_zero") f.bone_zero = true;
    else if (w == "proxy_part_shift") f.proxy_part_shift = true;
    else if (w == "no_verify") f.no_verify = true;
    else if (w == "arena_stale") f.arena_stale = true;
    else if (w == "flat_indices") f.flat_indices = true;
    else if (w == "editor_exact") f.editor_exact = true;
    else {
      error = "unknown shadow caster fault '" + w + "' (palette_from_colour, fetch_order, weights_as_fill, xdword_colour, bone_zero, proxy_part_shift, no_verify, arena_stale, flat_indices, editor_exact)";
      return false;
    }
  }
  return true;
}

namespace {
uint32_t FloatBits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
}  // namespace

std::vector<uint16_t> ShadowCore::CasterIndices(const assets::jboy_parts::Part& part, CasterClass klass, const ShadowFaults& f) {
  return assets::wrestler_geom::BuildIndices(part, f.flat_indices && klass == CasterClass::Wrestler);
}

// Every ring-class join and built buffer belongs to the arena that made it (review F1): an arena change drops them, so a (object, part) key can never reach the old arena's meshes.
void ShadowCore::DropRingState() {
  for (auto it = plane_joins_.begin(); it != plane_joins_.end();) {
    if (it->second.klass == CasterClass::Ring) it = plane_joins_.erase(it), ++stats_.ring_joins_cleared;
    else ++it;
  }
  for (auto it = prepared_.begin(); it != prepared_.end();) {
    if (it->second->ring_gen != 0) {
      if (release_) release_(*it->second);
      ++stats_.released;
      it = prepared_.erase(it);
    } else ++it;
  }
}

// Built buffers not used for kUnusedFrames bound frames are released (review F3), as the wrestlers' are.
void ShadowCore::Sweep() {
  for (auto it = prepared_.begin(); it != prepared_.end();) {
    if (frame_no_ - it->second->last_use > kUnusedFrames) {
      if (release_) release_(*it->second);
      ++stats_.released;
      it = prepared_.erase(it);
    } else ++it;
  }
  for (auto it = plane_failed_.begin(); it != plane_failed_.end();) it = it->second <= frame_no_ ? plane_failed_.erase(it) : std::next(it);
}

bool ShadowCore::BeginFrame(const rdr_adapter::LoadedFrame& frame) {
  ++frame_no_;
  bind_serial_ += serial_drift_ ? 2 : 1;   // lane 124: the running bind number (the wrestlers' early start no longer reads it; serial_drift_ = the rim map's serial_running plant)
  idle_ = 0;
  const ShadowJoinBoard& board = g_shadow_join_board;
  wrestlers_ready_ = board.wrestlers_begin ? board.wrestlers_begin(frame, bind_serial_) : false;
  std::shared_ptr<const assets::ring_gs::RingArena> ring = board.ring_arena ? board.ring_arena(frame) : nullptr;
  if (ring.get() != ring_.get() && !options_.faults.arena_stale) {   // another arena (or none): the ring-class pairings and buffers of the old one go
    ++ring_gen_, ++stats_.arena_changes;
    DropRingState();
  }
  ring_ = std::move(ring);
  body_cache_.clear();
  if (frame_no_ % 60 == 0) Sweep();
  colour_draw_.clear();
  if (options_.faults.palette_from_colour) {
    for (uint32_t d = 0; d < frame.draws.count; ++d) {
      const rdr_adapter::DrawOrigin* o = frame.OriginOf(d);
      if (o && o->object && (o->pass_site == rdr_adapter::kModelPartColourSite || o->pass_site == rdr_adapter::kModelPartMirrorSite))
        colour_draw_.emplace((uint64_t(o->object) << 32) | o->part, d);
    }
  }
  const bool ready = wrestlers_ready_ || ring_ != nullptr || proxy_ != nullptr || board.caw_join != nullptr;   // (lane 225: the editor has no scene record, the caw hook alone can make a frame ready)
  ++(ready ? stats_.frames_ready : stats_.frames_not_ready);
  return ready;
}

bool ShadowCore::PlaneJoin(const rdr_adapter::ReplaceIn& in, uint32_t object, uint32_t part, Join& out, KeepWhy& why) {
  const uint64_t key = (uint64_t(object) << 32) | part;
  const auto it = plane_joins_.find(key);
  if (it != plane_joins_.end()) {
    out = it->second;
    return true;
  }
  if (!ring_ && !proxy_) return why = KeepWhy::NotReady, false;
  const auto failed = plane_failed_.find(key);   // a pairing that failed is not recomputed (a content compare of the whole buffer) on every draw (review F8)
  if (failed != plane_failed_.end() && failed->second > frame_no_) return ++stats_.plane_pair_cached_failures, why = KeepWhy::Unjoined, false;
  const rdr_adapter::Draw& dr = *in.draw;
  const rdr_adapter::VertexLayout& lay = in.frame->layouts[dr.layout];
  if (!in.read_geometry || !in.read_geometry(dr.vertex_offset, dr.vertex_bytes, vb_) || vb_.size() < size_t(dr.vertex_count) * lay.host_stride) return why = KeepWhy::Mismatch, false;
  const auto positions_equal = [&](const assets::jboy_parts::Part& p) {   // bit equality (a float compare would call -0 and +0 equal and NaN unequal)
    if (p.vertices != dr.vertex_count || !p.ok || !p.tiles) return false;
    for (uint32_t v = 0; v < p.vertices; ++v)
      for (int c = 0; c < 3; ++c) {
        uint32_t bits;
        std::memcpy(&bits, vb_.data() + size_t(v) * lay.host_stride + 4 * size_t(c), 4);
        if (FloatBits(assets::BeF(p.s[0] + 28ull * v + 4 * size_t(c))) != bits) return false;
      }
    return true;
  };
  Join j;
  bool found = false;
  if (ring_) {   // the ring's rope proxy: entry 956, mesh = part
    if (const assets::ring_gs::RingMesh* m = ring_->Find(956, int(part)); m && positions_equal(m->data)) {
      j.klass = CasterClass::Ring, j.part = &m->data, j.ring = m, j.arena = ring_, j.gen = ring_gen_, j.mesh = int(part), j.id = 956 | (uint64_t(ring_gen_) << 16);
      found = true;
      ++stats_.plane_joined_ring;
    }
  }
  if (!found && proxy_) {   // the wrestlers' plane proxy: chCommon.pac entry 0, mesh = part (the plant: one off)
    const int mesh = int(part) + (options_.faults.proxy_part_shift ? 1 : 0);
    if (const assets::jboy_parts::Part* p = proxy_->Mesh(mesh); p && p->vertices == dr.vertex_count && (options_.faults.proxy_part_shift || positions_equal(*p))) {
      j.klass = CasterClass::Proxy, j.part = p, j.mesh = mesh, j.id = 0xC0C0;
      found = true;
      ++stats_.plane_joined_proxy;
    }
  }
  if (!found) {
    if (g_shadow_join_board.ring_arena && !ring_) return why = KeepWhy::NotReady, false;   // (the ring hook exists but its arena is not loaded yet: a fallback frame, not an unjoined object)
    plane_failed_[key] = frame_no_ + kRetryFrames;
    return why = KeepWhy::Unjoined, false;
  }
  plane_joins_[key] = j;
  out = j;
  return true;
}

// lane 225: the editor class's vertex check. Per vertex and element of the shadow layout: position (4 floats) within 5e-4 (absolute below 1, relative above: the caw class's tolerance, CawCore
// kCawPosTolerance); weights and ids bit-exact; the fill dword's bytes 0..2 exact and its byte 3 = the high byte of the STREAM's own position x (as caw_core Fill0Mode learns it), in which
// case our byte is set to the stream's (a vertex near x = 0 whose scaled x sits on the other side of an exponent boundary). `exact` (a plant): the old byte-for-byte rule.
static bool EditorVertexSame(const sv::Layout& lay, std::vector<uint8_t>& mine, const std::vector<uint8_t>& stream, bool exact, bool can_patch) {
  const uint32_t stride = lay.host_stride;
  if (!stride || mine.empty() || mine.size() % stride || stream.size() < mine.size()) return false;
  if (exact) return std::memcmp(stream.data(), mine.data(), mine.size()) == 0;
  int pos = -1;
  for (const auto& r : lay.r)
    if (r.role == sv::Role::Pos) pos = r.offset;
  if (pos < 0) return false;
  const size_t n = mine.size() / stride;
  for (size_t v = 0; v < n; ++v) {
    uint8_t* m = mine.data() + v * stride;
    const uint8_t* s = stream.data() + v * stride;
    for (const auto& r : lay.r) {
      if (r.role == sv::Role::Pos) {
        for (int k = 0; k < 4; ++k) {
          float a, b;
          std::memcpy(&a, m + r.offset + 4 * k, 4);
          std::memcpy(&b, s + r.offset + 4 * k, 4);
          if (!(std::fabs(a - b) <= 5e-4f * std::max(1.0f, std::fabs(b)))) return false;
        }
      } else if (r.role == sv::Role::Fill) {
        if (std::memcmp(m + r.offset, s + r.offset, 3) != 0) return false;
        if (m[r.offset + 3] != s[r.offset + 3]) {
          if (s[r.offset + 3] != s[pos + 3]) return false;   // the stream's fill byte is not the high byte of its own x: another rule, refused
          if (!can_patch) return false;   // review NOTE: the buffer is already uploaded; never rewrite it, refuse
          m[r.offset + 3] = s[r.offset + 3];
        }
      } else {
        const size_t bytes = r.role == sv::Role::Weights ? 12 : 4;
        if (std::memcmp(m + r.offset, s + r.offset, bytes) != 0) return false;
      }
    }
  }
  return true;
}

ShadowCore::Decision ShadowCore::Decide(const rdr_adapter::ReplaceIn& in) {
  Decision d;
  ++stats_.claimed;
  const auto keep = [&](KeepWhy why) {
    d.ok = false, d.why = why;
    ++stats_.kept[uint8_t(why)];
    return d;
  };
  if (!in.frame || !in.draw || !in.origin) return keep(KeepWhy::Mismatch);
  const rdr_adapter::DrawOrigin& o = *in.origin;
  const rdr_adapter::Draw& dr = *in.draw;
  const bool plane = o.pass_site == rdr_adapter::kPlaneShadowSite;
  if ((dr.flags & (rdr_adapter::kDrawNoGeometry | rdr_adapter::kDrawInline)) || dr.layout >= in.frame->layouts.count || !dr.index_bytes || !dr.vertex_bytes || dr.index_32bit || in.stream_index_32bit)
    return keep(KeepWhy::Variant);
  sv::Options opt;
  if (plane) {
    if (!wrestlers_ready_ && !ring_ && !proxy_) return keep(KeepWhy::NotOurs);   // review SF-1b: the caw join alone made this bind ready: a plane-mask draw goes on to plane_shadow, not a final NotReady
    Join j;
    KeepWhy why = KeepWhy::Unjoined;
    if (!PlaneJoin(in, o.object, o.part, j, why)) return keep(why), (why == KeepWhy::Unjoined ? ++stats_.kept_plane_unjoined : stats_.kept_plane_unjoined), d;
    // a ring object the ring hook joined to another entry than 956 is a ring piece: never a caster
    if (j.klass == CasterClass::Ring && g_shadow_join_board.ring_entry_of) {
      const int e = g_shadow_join_board.ring_entry_of(o.object);
      if (e >= 0 && e != 956) return keep(KeepWhy::NotOurs);
    }
    opt.scale = 1.0f;
    opt.fill = j.klass == CasterClass::Ring ? sv::FillMode::Weight255 : sv::FillMode::HiByteX;
    d = Finish(in, *j.part, j.klass, true, opt, j.id, j.klass == CasterClass::Ring ? j.gen : 0);
    if (!d.ok) {   // a failed compare drops the join (object addresses are reused) and is not retried at once
      plane_joins_.erase((uint64_t(o.object) << 32) | o.part);
      plane_failed_[(uint64_t(o.object) << 32) | o.part] = frame_no_ + kRetryFrames;
    }
    return d;
  }
  // a map-site draw: the wrestlers' join (asked once per object per frame, review F4)
  const ShadowJoinBoard& board = g_shadow_join_board;
  // lane 225: the editor's wrestler is the caw hook's, part by part. The wrestlers' hook answers NotWrestler for it (no roster model has its counts, no scene record), which used to leave all of its
  // shadow-pass and rim-map draws the stream's (K5w: "not ours 27600"). A part the caw hook holds byte-confirmed is a caster: same object, same part number; our bytes are re-checked below.
  const auto editor = [&]() -> Decision {
    CawShadowPart cp;
    if (!board.caw_join || !board.caw_join(o.object, o.part, cp) || !cp.Part()) return keep(KeepWhy::NotOurs), ++stats_.kept_map_not_ours, d;
    ++stats_.editor_joined;
    opt.scale = cp.scale;
    opt.fill = sv::FillMode::HiByteX;
    return Finish(in, *cp.Part(), CasterClass::Editor, false, opt, (uint64_t(cp.group) << 8) | 2, 0);
  };
  if (!board.wrestlers_join) return editor();   // no wrestlers hook: the editor's hook may still know the part
  if (!wrestlers_ready_) return board.caw_join ? editor() : keep(KeepWhy::NotReady);   // the wrestlers hook has no record yet (the first frame of a capture, and every editor frame: no record): a fallback frame
  auto bc = body_cache_.find(o.object);
  if (bc == body_cache_.end()) {
    BodyCache c;
    c.status = board.wrestlers_join(in.join_frame ? *in.join_frame : *in.frame, o.object, in.read_geometry, c.body);
    bc = body_cache_.emplace(o.object, std::move(c)).first;
  }
  const BodyStatus st = bc->second.status;
  const ShadowBody& body = bc->second.body;
  if (st == BodyStatus::NotWrestler) return editor();
  if (st == BodyStatus::Pending) return keep(KeepWhy::NotReady);
  if (st == BodyStatus::Unjoined || !body.model) return keep(KeepWhy::Unjoined);
  const assets::jboy_parts::Part* mesh = body.PartOf(o.part);   // the settled per-part source (lane 100: a part may come from another attire of the pac)
  if (!mesh) return keep(KeepWhy::Mismatch);
  opt.scale = body.scale;
  opt.fill = sv::FillMode::HiByteX;
  d = Finish(in, *mesh, CasterClass::Wrestler, false, opt, (uint64_t(uint32_t(body.EntryOf(o.part))) << 8) | 1, 0);
  if (d.ok && options_.faults.palette_from_colour) {   // planted: the colour pass's palette rows for the same (object, part) override the shadow draw's own
    const auto ci = colour_draw_.find((uint64_t(o.object) << 32) | o.part);
    const rdr_adapter::LoadedFrame& jf = in.join_frame ? *in.join_frame : *in.frame;   // lane 228 (N4): colour_draw_ indexes the REAL frame (a D3 marker's mini frame is in.frame)
    if (ci != colour_draw_.end() && mesh->palette_count && ci->second < jf.draws.count) {
      const rdr_adapter::Draw& cd = jf.draws[ci->second];
      if (cd.vs_bank < jf.constant_banks.count) {
        const float* bank = reinterpret_cast<const float*>(jf.constant_banks[cd.vs_bank].dwords);
        rows_.assign(bank + 4 * 76, bank + 4 * (76 + size_t(mesh->palette_count) * 3));
        d.first_register = 76, d.rows = mesh->palette_count * 3, d.row_data = rows_.data();
        ++stats_.rows_overridden;
      }
    }
  }
  return d;
}

ShadowCore::Decision ShadowCore::Finish(const rdr_adapter::ReplaceIn& in, const assets::jboy_parts::Part& part, CasterClass klass, bool plane, const sv::Options& opt, uint64_t model_id,
                                        uint32_t ring_gen) {
  Decision d;
  d.klass = klass;
  d.palette_count = part.palette_count;   // lane 124
  const auto keep = [&](KeepWhy why) {
    d.ok = false, d.why = why;
    ++stats_.kept[uint8_t(why)];
    return d;
  };
  const rdr_adapter::Draw& dr = *in.draw;
  const rdr_adapter::VertexLayout& lay = in.frame->layouts[dr.layout];
  if (!part.ok || !part.tiles || part.vertices != dr.vertex_count) return keep(KeepWhy::Mismatch);
  std::vector<sv::Element> els;
  uint64_t lsig = 1469598103934665603ull;
  for (uint32_t e = 0; e < lay.element_count; ++e) {
    const rdr_adapter::VertexElement& el = in.frame->elements[lay.element_first + e];
    els.push_back({el.vk_format, el.host_offset, el.source_format, el.flags, el.mini, el.guest_offset});
    for (uint64_t v : {uint64_t(el.vk_format), uint64_t(el.host_offset), uint64_t(el.source_format), uint64_t(el.flags), uint64_t(el.mini), uint64_t(uint32_t(el.guest_offset))}) lsig = Mix(lsig, v);
  }
  uint32_t sb;
  std::memcpy(&sb, &opt.scale, 4);
  const uint64_t key = Mix(Mix(Mix(Mix(Mix(model_id, part.mesh_index), sb), lsig), lay.host_stride), uint64_t(plane) + 2 * uint64_t(opt.fill == sv::FillMode::Weight255));
  auto it = prepared_.find(key);
  if (it == prepared_.end()) {
    auto pr = std::make_unique<ShadowPrepared>();
    pr->ring_gen = ring_gen;
    sv::Layout sl;
    if (!sv::Classify(part, els.data(), els.size(), lay.host_stride, lay.guest_stride, sl)) {
      pr->refused = true;
      ++stats_.layout_refused;
    } else if (!sv::Write(part, sl, opt, options_.faults.Writer(), pr->vertex)) {
      pr->refused = true;
      ++stats_.layout_refused;
    } else {
      pr->layout = sl;
      pr->index = CasterIndices(part, klass, options_.faults);   // lane 192: every class, index groups joined by 0xFFFF (one rule; a single-group part is unchanged)
      ++stats_.prepared_built;
    }
    it = prepared_.emplace(key, std::move(pr)).first;
  }
  ShadowPrepared& p = *it->second;
  p.last_use = frame_no_;
  if (p.refused) return keep(KeepWhy::Variant);
  if (p.failed) return keep(KeepWhy::Mismatch);
  if (dr.vertex_bytes != p.vertex.size() || dr.index_count != p.index.size()) return keep(KeepWhy::Mismatch);
  ++p.uses;
  const bool moved = klass == CasterClass::Editor && (p.v_off != dr.vertex_offset || p.i_off != dr.index_offset || p.v_bytes != dr.vertex_bytes || p.i_bytes != dr.index_bytes);   // review SF-b: new stream bytes = a full verify now
  if (!options_.faults.no_verify && (options_.verify_all || !p.verified || moved || (p.uses & 63) == 0)) {
    ++stats_.verify_runs;
    // lane 225: the editor's stream positions are the file's times the fitted scale plus the editor's own float noise (up to ~1e-5 absolute in the K5w capture: the last mantissa bits), so the editor's
    // class compares positions within the caw class's tolerance and everything else (weights, ids, fill) exactly; the wrestlers' / ring's / proxies' byte-for-byte rule is unchanged.
    bool same = in.read_geometry && in.read_geometry(dr.vertex_offset, dr.vertex_bytes, vb_) && vb_.size() >= p.vertex.size() &&
                (klass == CasterClass::Editor ? EditorVertexSame(p.layout, p.vertex, vb_, options_.faults.editor_exact, p.gpu[0] == 0) : std::memcmp(vb_.data(), p.vertex.data(), p.vertex.size()) == 0) &&
                dr.index_bytes == p.index.size() * 2 && in.read_geometry(dr.index_offset, dr.index_bytes, ib_) && ib_.size() >= dr.index_bytes && std::memcmp(ib_.data(), p.index.data(), dr.index_bytes) == 0;
    if (!same) {
      ++stats_.verify_bad, ++stats_.joins_dropped;
      p.verified = false;
      return keep(KeepWhy::Mismatch);
    }
    p.verified = true;
    p.v_off = dr.vertex_offset, p.i_off = dr.index_offset, p.v_bytes = dr.vertex_bytes, p.i_bytes = dr.index_bytes;
  }
  d.ok = true;
  d.prepared = &p;
  ++stats_.replaced;
  ++(plane ? stats_.replaced_plane : stats_.replaced_map);
  ++stats_.by_class[uint8_t(klass)];
  return d;
}

void ShadowCore::UploadFailed(ShadowPrepared* p, CasterClass c, bool plane) {
  if (p) p->failed = true;
  if (stats_.replaced) --stats_.replaced;
  if (plane && stats_.replaced_plane) --stats_.replaced_plane;
  if (!plane && stats_.replaced_map) --stats_.replaced_map;
  if (stats_.by_class[uint8_t(c)]) --stats_.by_class[uint8_t(c)];
  ++stats_.kept[uint8_t(KeepWhy::Mismatch)];
}

void ShadowCore::Clear() {
  for (auto& kv : prepared_) {
    if (release_) release_(*kv.second);
    ++stats_.released;
  }
  prepared_.clear();
  plane_joins_.clear();
  plane_failed_.clear();
  body_cache_.clear();
  ring_.reset();
  idle_ = 0;
}

std::string ShadowCore::Report() const {
  const ShadowStats& s = stats_;
  char b[1200];
  std::snprintf(b, sizeof b,
                "shadow casters: claimed %llu replaced %llu (map %llu, plane %llu; by class wrestler %llu / ring %llu / proxy %llu) | kept: map not ours %llu, plane unjoined %llu | plane joins ring %llu proxy %llu | "
                "verify %llu bad %llu, layout refused %llu, rows overridden %llu, joins dropped %llu, parts built %llu released %llu | arena changes %llu (ring joins cleared %llu) | frames ready %llu not ready %llu | editor parts (lane 225) offered %llu replaced %llu",
                (unsigned long long)s.claimed, (unsigned long long)s.replaced, (unsigned long long)s.replaced_map, (unsigned long long)s.replaced_plane, (unsigned long long)s.by_class[0],
                (unsigned long long)s.by_class[1], (unsigned long long)s.by_class[2], (unsigned long long)s.kept_map_not_ours, (unsigned long long)s.kept_plane_unjoined,
                (unsigned long long)s.plane_joined_ring, (unsigned long long)s.plane_joined_proxy, (unsigned long long)s.verify_runs, (unsigned long long)s.verify_bad,
                (unsigned long long)s.layout_refused, (unsigned long long)s.rows_overridden, (unsigned long long)s.joins_dropped, (unsigned long long)s.prepared_built, (unsigned long long)s.released,
                (unsigned long long)s.arena_changes, (unsigned long long)s.ring_joins_cleared, (unsigned long long)s.frames_ready, (unsigned long long)s.frames_not_ready, (unsigned long long)s.editor_joined, (unsigned long long)s.by_class[uint8_t(CasterClass::Editor)]);
  return b;
}

}  // namespace rdr_scene
