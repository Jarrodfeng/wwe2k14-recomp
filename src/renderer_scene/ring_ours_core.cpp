// See ring_ours_core.h. Session 101, lane 70.
#include "ring_ours_core.h"
#include "proof_frames.h"
#include "ring_hash.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>

namespace rdr_scene {
namespace {

using assets::ring_gs::RingMesh;
using rdr_adapter::kModelPartColourSite;
bool RingSite(uint32_t s) { return s == kModelPartColourSite || s == kRingLambertSite; }   // lane 137: the colour site + the WM30 T_Lambert site (entry 956 only)

uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

size_t MipBytes(assets::TexFormat f, uint32_t w, uint32_t h) {
  switch (f) {
    case assets::TexFormat::kBC1: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 8;
    case assets::TexFormat::kBC2:
    case assets::TexFormat::kBC3: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
    case assets::TexFormat::kB4G4R4A4: return size_t(w) * h * 2;
    default: return size_t(w) * h * 4;
  }
}

// The layout signature: everything the writer's bytes depend on.
uint64_t LayoutSig(const rdr_adapter::LoadedFrame& f, const rdr_adapter::VertexLayout& lay) {
  uint64_t h = 0xcbf29ce484222325ull;
  h = Mix(h, lay.host_stride), h = Mix(h, lay.guest_stride), h = Mix(h, lay.element_count);
  for (uint32_t e = 0; e < lay.element_count; ++e) {
    const rdr_adapter::VertexElement& el = f.elements[lay.element_first + e];
    h = Mix(h, el.vk_format), h = Mix(h, el.host_offset), h = Mix(h, el.source_format), h = Mix(h, el.flags), h = Mix(h, uint32_t(el.guest_offset));
  }
  return h;
}

// The stream's position element: the float4 at guest dword 0 (host format 109). False when there is none.
bool PositionElement(const rdr_adapter::LoadedFrame& f, const rdr_adapter::VertexLayout& lay, uint32_t& offset) {
  for (uint32_t e = 0; e < lay.element_count; ++e) {
    const rdr_adapter::VertexElement& el = f.elements[lay.element_first + e];
    if (el.source_format == 38 && el.guest_offset == 0 && el.vk_format == 109) return offset = el.host_offset, true;
  }
  return false;
}

bool ClassifyLayout(const rdr_adapter::LoadedFrame& f, const rdr_adapter::VertexLayout& lay, const RingMesh& m, assets::wrestler_geom::Roles& roles) {
  std::array<assets::wrestler_geom::LayoutElement, 16> els;
  if (lay.element_count == 0 || lay.element_count > els.size()) return false;
  for (uint32_t e = 0; e < lay.element_count; ++e) {
    const rdr_adapter::VertexElement& el = f.elements[lay.element_first + e];
    els[e] = {el.vk_format, el.host_offset, el.source_format, el.components, el.flags, el.guest_offset};
  }
  return assets::wrestler_geom::Classify(m.data, els.data(), lay.element_count, lay.host_stride, lay.guest_stride, roles);
}

// The stream's positions (bit for bit) against a mesh's file positions.
bool PositionsEqual(const std::vector<uint8_t>& bytes, uint32_t stride, uint32_t offset, const RingMesh& m) {
  if (bytes.size() < size_t(m.vertices) * stride) return false;
  for (uint32_t v = 0; v < m.vertices; ++v) {
    const uint8_t* s = bytes.data() + size_t(v) * stride + offset;
    const uint8_t* f = m.data.s[0] + 28ull * v;
    float a[3];
    std::memcpy(a, s, 12);
    for (int c = 0; c < 3; ++c) {
      const float b = assets::BeF(f + 4 * c);
      if (std::memcmp(&a[c], &b, 4) != 0) return false;
    }
  }
  return true;
}

}  // namespace

bool RingFaults::Parse(const std::string& list, RingFaults& f, std::string& error) {
  size_t at = 0;
  while (at <= list.size()) {
    size_t comma = list.find(',', at);
    if (comma == std::string::npos) comma = list.size();
    const std::string w = list.substr(at, comma - at);
    at = comma + 1;
    if (w.empty()) continue;
    if (w == "colour_order") f.colour_order = true;
    else if (w == "uv_halves_swapped") f.uv_halves_swapped = true;
    else if (w == "uv_sign") f.uv_sign = true;
    else if (w == "bone_zero") f.bone_zero = true;
    else if (w == "weight_word") f.weight_word = true;
    else if (w == "skirt_tie") f.skirt_tie = true;
    else if (w == "part_only_join") f.part_only_join = true;
    else if (w == "lod_pin") f.lod_pin = true;
    else if (w == "proxy_wrong") f.proxy_wrong = true;   // lane 137
    else if (w == "lambert_guard_off") f.lambert_guard_off = true;   // lane 137
    else if (w == "lambert_off") f.lambert_off = true;   // lane 137
    else if (w == "scene_off") {}                        // lane 137: a props fault; fview hands one --replace-fault list to every class
    else if (w == "no_verify") f.no_verify = true;
    else if (w == "no_proof_frames") f.no_proof_frames = true;
    else if (w == "frozen_clock") f.frozen_clock = true;   // lane 115 F2
    else {
      error = "unknown ring fault '" + w + "' (colour_order, uv_halves_swapped, uv_sign, bone_zero, weight_word, skirt_tie, part_only_join, lod_pin, proxy_wrong, lambert_guard_off, lambert_off, no_verify, no_proof_frames, frozen_clock)";
      return false;
    }
  }
  return true;
}

void RingCore::ReleaseAll() {
  if (release_)
    for (auto& kv : prepared_) release_(*kv.second);
  prepared_.clear();
}

void RingCore::SetArena(std::shared_ptr<const assets::ring_gs::RingArena> arena, int arena_id) {
  ReleaseAll();
  arena_ = std::move(arena);
  arena_id_ = arena_id;
  mesh_by_key_.clear();
  joins_.clear();
  foreign_.clear();
  foreign_lambert_.clear();   // lane 137
  content_ok_.clear();
  file_hash_.clear();
  uv_mode_ = 0;
  verified_key_.clear();
  if (!arena_) return;
  for (size_t i = 0; i < arena_->meshes.size(); ++i) mesh_by_key_[uint32_t(arena_->meshes[i].entry) << 16 | uint32_t(arena_->meshes[i].part)] = int(i);
}

int RingCore::ArenaOfRecord(const SceneTables& t) {
  for (const AssetRef& a : t.assets) {
    const std::string pac(t.Str(a.pac));
    const size_t p = pac.rfind("bg");
    int n = 0;
    if (p != std::string::npos && std::sscanf(pac.c_str() + p, "bg%d", &n) == 1) return n;
  }
  return -1;
}

bool RingCore::Claims(const rdr_adapter::DrawOrigin& o) const {
  if (o.klass != rdr_adapter::DrawClass::ModelPart || !RingSite(o.pass_site)) return false;   // lane 137: + the Lambert site
  if (options_.faults.lambert_off && o.pass_site == kRingLambertSite) return false;   // lane 137 plant: the pre-promotion state
  return !IsForeign(o.object, o.pass_site);   // lane 137: per site
}

int RingCore::MeshIndex(int entry, int part) const {
  const auto it = mesh_by_key_.find(uint32_t(entry) << 16 | uint32_t(part));
  return it == mesh_by_key_.end() ? -1 : it->second;
}
const RingMesh* RingCore::MeshOf(int entry, int part) const {
  const int i = MeshIndex(entry, part);
  return i < 0 ? nullptr : &arena_->meshes[size_t(i)];
}

// ---- the join, once per object ------------------------------------------------------------------------------------------------------------------------------

bool RingCore::JoinObject(const rdr_adapter::ReplaceIn& in, ObjJoin& out, const char*& reason) {
  const rdr_adapter::LoadedFrame& f = *in.frame;
  const uint32_t object = in.origin->object;
  // every part the object draws in this frame (the first draw of each)
  std::map<uint32_t, uint32_t> part_draw;
  for (uint32_t d = 0; d < f.draws.count; ++d) {
    const rdr_adapter::DrawOrigin* o = f.OriginOf(d);
    if (!o || o->object != object || o->pass_site != in.origin->pass_site || o->klass != rdr_adapter::DrawClass::ModelPart) continue;   // lane 137: the incoming draw's own site
    if ((f.draws[d].flags & rdr_adapter::kDrawNoGeometry) || f.draws[d].layout >= f.layouts.count) continue;
    part_draw.emplace(o->part, d);
  }
  if (part_draw.empty()) return reason = "no geometry", false;
  if (options_.faults.part_only_join) part_draw.erase(std::next(part_draw.begin()), part_draw.end());
  std::vector<int> cands;
  std::map<uint32_t, std::vector<uint8_t>> bytes;
  for (int entry : assets::ring_gs::kEntries) {
    if (in.origin->pass_site == kRingLambertSite && entry != 956) continue;   // lane 137: the Lambert site draws the rope shadow proxy only
    bool all = true, any_mesh = false;
    for (const auto& pd : part_draw) {
      const RingMesh* m = MeshOf(entry, int(pd.first));
      const rdr_adapter::Draw& dr = f.draws[pd.second];
      any_mesh = any_mesh || m;
      if (!m || m->vertices != dr.vertex_count || m->StreamIndexCount() != dr.index_count) {
        all = false;
        break;
      }
      const rdr_adapter::VertexLayout& lay = f.layouts[dr.layout];
      uint32_t off = 0;
      if (!PositionElement(f, lay, off)) {
        all = false;
        break;
      }
      auto bit = bytes.find(pd.first);
      if (bit == bytes.end()) {
        std::vector<uint8_t> b;
        if (!in.read_geometry || !in.read_geometry(dr.vertex_offset, dr.vertex_count * lay.host_stride, b)) return reason = "geometry unreadable", false;
        bit = bytes.emplace(pd.first, std::move(b)).first;
      }
      if (!PositionsEqual(bit->second, lay.host_stride, off, *m)) {
        all = false;
        break;
      }
    }
    if (all) cands.push_back(entry);
  }
  if (cands.empty()) return reason = "not a ring piece", out.foreign = true, false;
  int pick = cands[0];
  bool tie = cands.size() > 1;
  out.slot0_open = false;
  out.tie_texture = false;
  if (tie) {
    if (options_.faults.part_only_join) pick = cands.back();
    else if (options_.faults.skirt_tie) pick = cands.front();
    else {
      // the last tie (skirts 973 / 974): the slot-0 image's content hash decides when the owner has the feed (F1)
      const auto first = part_draw.begin();
      const rdr_adapter::Draw& dr = f.draws[first->second];
      uint32_t image = 0;
      for (uint32_t t = 0; t < dr.texture_count && dr.texture_first + t < f.texture_slots.count; ++t)
        if (f.texture_slots[dr.texture_first + t].slot == 0) image = f.texture_slots[dr.texture_first + t].image;
      int resolved = -1;
      for (int entry : cands) {
        const RingMesh* m = MeshOf(entry, int(first->first));
        uint32_t mips = 0;
        last_reason_.clear();
        if (image && m && m->diffuse >= 0 && ContentMatches(image, m->diffuse, false, mips)) {
          if (resolved < 0) resolved = entry;
        } else if (!last_reason_.empty() && differ_notes_.size() < 12) {
          differ_notes_.insert("tie candidate entry " + std::to_string(entry) + " slot 0 texture '" + arena_->textures[size_t(m->diffuse)].name + "' is not the stream's image: " + last_reason_);
        }
      }
      if (resolved >= 0) pick = resolved, out.tie_texture = true;
      else out.slot0_open = true;
    }
  }
  if (options_.faults.lod_pin && pick == 980) pick = 960;
  if (options_.faults.proxy_wrong && pick == 956) pick = 980;   // lane 137 plant: the Lambert-site proxy joined to the low rope (counts differ: the draw checks must fail)
  out.entry = pick;
  if (tie) ++(out.tie_texture ? stats_.joined_tie_texture : stats_.joined_tie_open);
  else ++stats_.joined_content;
  return true;
}

// ---- the prepared meshes --------------------------------------------------------------------------------------------------------------------------------------

RingPrepared* RingCore::GetPrepared(uint64_t key, int mesh_index, const assets::wrestler_geom::Roles& roles) {
  const RingMesh& m = arena_->meshes[size_t(mesh_index)];
  auto p = std::make_unique<RingPrepared>();
  if (!assets::ring_gs::WriteVertices(m, roles, options_.faults.Writer(), uv_mode_ == 2, p->vertex)) p->refused = true;
  else p->index = assets::ring_gs::BuildIndices(m), ++stats_.prepared_built;
  return prepared_.emplace(key, std::move(p)).first->second.get();
}

// Learns the stream's u-sign rule from its own bytes: the half of u at the first vertex whose file u is negative carries the sign bit (signed, post patch 109) or not (|u|). Never guessed: unreadable = unknown.
bool RingCore::DetectUvSign(const rdr_adapter::ReplaceIn& in, const rdr_adapter::VertexLayout& lay, const RingMesh& m) {
  if (uv_mode_ != 0) return true;
  if (m.first_neg_u < 0 || !in.read_geometry || !in.frame) return false;
  uint32_t off = 0;
  bool have = false;
  for (uint32_t e = 0; e < lay.element_count; ++e) {
    const rdr_adapter::VertexElement& el = in.frame->elements[lay.element_first + e];
    if (el.source_format == 31 && el.vk_format == 83) off = el.host_offset, have = true;
  }
  std::vector<uint8_t> vb;
  if (!have) return true;   // lane 220 follow-up: a layout with no uv element has nothing to learn from and nothing that depends on the rule (no refusal, nothing learned)
  if (!in.read_geometry(in.vertex_offset, in.vertex_count * lay.host_stride, vb) || vb.size() < size_t(in.vertex_count) * lay.host_stride) return false;
  uint16_t h = 0, hs = 0;
  std::memcpy(&h, vb.data() + size_t(m.first_neg_u) * lay.host_stride + off, 2);
  const bool strict = m.first_strict_neg_u >= 0;
  if (strict) std::memcpy(&hs, vb.data() + size_t(m.first_strict_neg_u) * lay.host_stride + off, 2);
  const int mode = assets::ring_gs::LearnUvMode(h, strict, hs);   // lane 220 follow-up: |u| only from a clear bit at a strictly negative vertex
  if (mode == 0) return false;
  uv_mode_ = mode;
  return true;
}

// The file texture's first `mips` mips, hashed the way the feeds hash the stream image (all mips it holds, in order, tightly packed).
uint64_t RingCore::FileHash(int texture, uint32_t mips) {
  const uint64_t key = (uint64_t(uint32_t(texture)) << 8) | std::min<uint32_t>(mips, 255);
  const auto it = file_hash_.find(key);
  if (it != file_hash_.end()) return it->second;
  const assets::Texture& t = arena_->textures[size_t(texture)];
  size_t bytes = 0;
  for (uint32_t m = 0; m < mips; ++m) bytes += MipBytes(t.format, std::max(1u, t.width >> m), std::max(1u, t.height >> m));
  return file_hash_[key] = RingXxh3(t.data.data(), std::min(bytes, t.data.size()));
}

// F1: is the stream image behind `image_id` this file texture (size, mips, content hash)? Cached by (image id, generation, texture, mips): a runtime swap bumps the generation. `force`: the planted
// skirt_tie takes the texture without the hash. `mips` out: the stream image record's mip count.
bool RingCore::ContentMatches(uint32_t image_id, int texture, bool force, uint32_t& mips) {
  rdr_adapter::ImageContent c;
  if (!options_.image_content || !image_id || !options_.image_content(image_id, c) || c.mips == 0) return ++stats_.content_unknown, false;
  mips = c.mips;
  if (force) return true;
  const assets::Texture& t = arena_->textures[size_t(texture)];
  const uint64_t key = Mix(Mix(Mix(Mix(0xcbf29ce484222325ull, image_id), c.generation), uint64_t(uint32_t(texture))), c.mips);
  const auto it = content_ok_.find(key);
  if (it != content_ok_.end()) return it->second;
  ++stats_.content_checks;
  const bool ok = c.width == t.width && c.height == t.height && c.mips <= std::max(1u, t.mips) && c.hash == FileHash(texture, c.mips);
  if (!ok) {
    char b[160];
    std::snprintf(b, sizeof b, "%s%ux%u %u mips (file %ux%u %u mips, format %d)", (c.width != t.width || c.height != t.height) ? "size " : c.mips > std::max(1u, t.mips) ? "mips " : "hash ", c.width, c.height, c.mips, t.width, t.height, t.mips, int(t.format));
    last_reason_ = b;
  }
  ++(ok ? stats_.content_ok : stats_.content_differs);
  return content_ok_[key] = ok;
}

// ---- the decision ---------------------------------------------------------------------------------------------------------------------------------------------

RingCore::Decision RingCore::Decide(const rdr_adapter::ReplaceIn& in, const uint8_t* ps_slots, uint32_t ps_slot_count) {
  Decision d;
  ++stats_.decisions;
  const auto keep = [&](KeepWhy why, const char* reason) -> Decision {
    d.ok = false, d.why = why, d.reason = reason;
    ++stats_.kept[uint8_t(why)];
    return d;
  };
  if (!arena_) return keep(KeepWhy::NotReady, "no ring");
  const rdr_adapter::DrawOrigin* o = in.origin;
  if (!o || o->klass != rdr_adapter::DrawClass::ModelPart || !RingSite(o->pass_site) || !in.frame || !in.draw) return keep(KeepWhy::NotOurs, "site");   // lane 137
  const rdr_adapter::LoadedFrame& f = *in.frame;
  const rdr_adapter::Draw& dr = *in.draw;
  if (IsForeign(o->object, o->pass_site)) return keep(KeepWhy::NotOurs, "foreign object");
  (o->pass_site == kRingLambertSite ? foreign_lambert_ : foreign_).erase(o->object);   // lane 137: per site   // (an expired verdict: judged again below)
  if ((dr.flags & (rdr_adapter::kDrawNoGeometry | rdr_adapter::kDrawInline)) || dr.layout >= f.layouts.count || dr.index_32bit || !dr.index_bytes) return keep(KeepWhy::Variant, "draw shape");

  auto it = joins_.find(o->object);
  if (it == joins_.end() || it->second.entry < 0) {
    if (it != joins_.end() && frame_no_ < it->second.blocked_until) return it->second.blocked_unjoined ? keep(KeepWhy::Unjoined, "blocked: the join could not be made") : keep(KeepWhy::Mismatch, "blocked after a failed check");
    ObjJoin j;
    const char* why = "";
    if (!JoinObject(in, j, why)) {
      if (j.foreign) {
        (o->pass_site == kRingLambertSite ? foreign_lambert_ : foreign_)[o->object] = frame_no_ + 300;  // F5: addresses are reused; judged again after 300 frames
        if (drops_wanted_) dropped_objects_.push_back(o->object), ++drop_seq_;   // lane 129 (KC)
        ++stats_.foreign;
        return keep(KeepWhy::NotOurs, why);
      }
      ++stats_.unjoined;
      ObjJoin b;   // an unreadable blob is not re-scanned on every draw
      b.blocked_until = frame_no_ + 60, b.blocked_unjoined = true;
      joins_[o->object] = b;
      if (drops_wanted_) dropped_objects_.push_back(o->object), ++drop_seq_;   // lane 129 (KC)
      return keep(KeepWhy::Unjoined, why);
    }
    it = joins_.insert_or_assign(o->object, j).first;
  }
  ObjJoin& join = it->second;
  if (!options_.faults.lambert_guard_off && o->pass_site == kRingLambertSite && join.entry != 956) return keep(KeepWhy::NotOurs, "lambert: not the proxy");   // lane 137 (review 2): one join per object across both sites; the Lambert site is the proxy only
  const int part = int(o->part);
  const auto drop = [&](uint64_t& counter, const char* why) {
    ++counter;
    ++stats_.joins_dropped;
    if (first_fail_.empty()) {
      char b[200];
      std::snprintf(b, sizeof b, "%s (object 0x%08X part %d, stream %u vertices %u indices, entry %d)", why, o->object, part, in.vertex_count, in.index_count, join.entry);
      first_fail_ = b;
    }
    ObjJoin dropped;
    dropped.blocked_until = frame_no_ + 120;   // an object that fails its checks is not re-joined on every draw
    for (auto vk = verified_key_.begin(); vk != verified_key_.end();) vk = (vk->first >> 32) == o->object ? verified_key_.erase(vk) : std::next(vk);   // F6: every part of the object
    joins_[o->object] = dropped;
    if (drops_wanted_) dropped_objects_.push_back(o->object), ++drop_seq_;   // lane 129 (KC)
    return keep(KeepWhy::Mismatch, why);
  };
  // instances per object, for the census: the draws of the first part seen, per frame
  if (join.frame != frame_no_) {
    if (join.frame != 0) join.last_instances = join.first_part_draws;
    join.frame = frame_no_, join.first_part_draws = 0;
  }
  if (join.first_part < 0) join.first_part = part;
  if (part == join.first_part) ++join.first_part_draws;

  const int mi = MeshIndex(join.entry, part);
  if (mi < 0) return drop(stats_.bad_part, "part");
  const RingMesh& m = arena_->meshes[size_t(mi)];
  if (in.vertex_count != m.vertices) return drop(stats_.bad_vertices, "vertices");
  if (in.index_count != m.StreamIndexCount()) return drop(stats_.bad_indices, "indices");
  const rdr_adapter::VertexLayout& lay = f.layouts[dr.layout];
  if (m.first_neg_u >= 0 && !DetectUvSign(in, lay, m)) {   // a mesh whose bytes depend on the stream's u-sign rule, which is not known yet: never guessed
    ++stats_.layout_refused;
    return keep(KeepWhy::Variant, "u sign rule not known");
  }
  const uint64_t pkey = ((uint64_t(uint32_t(mi)) << 48) ^ LayoutSig(f, lay)) ^ (m.first_neg_u >= 0 ? uint64_t(uv_mode_) << 62 : 0);
  RingPrepared* pr;
  const auto pit = prepared_.find(pkey);   // CPU: the prepared buffer first, Classify only on a miss
  if (pit != prepared_.end()) pr = pit->second.get();
  else {
    assets::wrestler_geom::Roles roles;
    if (!ClassifyLayout(f, lay, m, roles)) {
      auto refused = std::make_unique<RingPrepared>();
      refused->refused = true;
      prepared_.emplace(pkey, std::move(refused));
      pr = nullptr;
    } else pr = GetPrepared(pkey, mi, roles);
  }
  if (!pr || pr->refused) {
    ++stats_.layout_refused;
    return keep(KeepWhy::Variant, "layout / writer");
  }
  if (pr->failed) return keep(KeepWhy::Mismatch, "buffers could not be made");
  if (pr->index.size() != in.index_count) return drop(stats_.bad_indices, "index build");

  // ---- the bytes: our vertex and index buffers against the stream's captured ones (F6: verified per (object, part) against the vertex key, the index key and the mesh) ----
  const uint64_t vk = (uint64_t(o->object) << 32) | uint32_t(part);
  const auto vit = verified_key_.find(vk);
  const bool fresh = vit == verified_key_.end() || vit->second.vertex != in.vertex_offset || vit->second.index != in.index_offset || vit->second.mesh != mi;
  // S101 lane 88 (D1): a key that passed is re-verified on its proof frame (one frame in kRingProofEvery, staggered by key); between proofs the identity checks above are all it pays.
  const bool proof = kRingPins && !fresh && !options_.verify_all && !options_.faults.no_proof_frames && ProofDue(vk, frame_no_, kRingProofEvery);
  if (!options_.faults.no_verify && in.read_geometry && (options_.verify_all || fresh || proof)) {
    ++stats_.verify_runs;
    stats_.proof_runs += proof;
    std::vector<uint8_t> vb, ib;
    if (!in.read_geometry(in.vertex_offset, in.vertex_count * lay.host_stride, vb) || vb.size() < pr->vertex.size() ||
        !in.read_geometry(in.index_offset, in.index_count * 2, ib) || ib.size() < pr->index.size() * 2)
      return drop(stats_.bad_bytes, "geometry unreadable");
    if (std::memcmp(vb.data(), pr->vertex.data(), pr->vertex.size()) != 0) return drop(stats_.bad_bytes, "vertex bytes");
    if (std::memcmp(ib.data(), pr->index.data(), pr->index.size() * 2) != 0) return drop(stats_.bad_bytes, "index bytes");
    verified_key_[vk] = {in.vertex_offset, in.index_offset, mi};
  }
  d.mesh = mi;
  d.prepared = pr;

  // ---- the textures this draw's pixel program samples and the file names ----
  const int roles_tex[3] = {m.diffuse, m.normal_map, m.specular};
  const uint8_t role_slot[3] = {0, 4, 5};
  for (int r = 0; r < 3; ++r) {
    bool wants = false;
    for (uint32_t s = 0; s < ps_slot_count; ++s) wants = wants || ps_slots[s] == role_slot[r];
    if (!wants) continue;
    if (roles_tex[r] < 0) {
      ++stats_.slot_kept_no_file;   // e.g. cover 964 mesh 1: a BumpMap program on a mesh that names no maps (ring.md B2.3): the game's image stays
      continue;
    }
    if (join.slot0_open) {   // F2: an open tie names no entry for the texture: all three roles stay the stream's
      ++stats_.slot_kept_tie;
      continue;
    }
    if (r == 0 && assets::ring_gs::IsRopeEntry(join.entry)) {   // the rope colour is chosen per instance at run time
      ++stats_.slot_kept_rope;
      continue;
    }
    if (r == 0 && join.entry == 953) {   // the canvas's `tail` texture: only its smallest mips differ from the file's, cause unknown (ring.md B2.3 [U]): the game's image stays
      ++stats_.slot_kept_tail;
      continue;
    }
    const rdr_adapter::TextureSlot* ts = nullptr;
    for (uint32_t t = 0; t < dr.texture_count && dr.texture_first + t < f.texture_slots.count; ++t)
      if (f.texture_slots[dr.texture_first + t].slot == role_slot[r]) ts = &f.texture_slots[dr.texture_first + t];
    if (!ts || ts->image == 0) {   // the stream binds no image: nothing to replace (the contract refuses a set slot there)
      ++stats_.slot_kept_no_file;
      continue;
    }
    const bool plain = (!ts->swizzle[0] && !ts->swizzle[1] && !ts->swizzle[2] && !ts->swizzle[3]) || (ts->swizzle[0] == 3 && ts->swizzle[1] == 4 && ts->swizzle[2] == 5 && ts->swizzle[3] == 6);
    if (ts->view_format != 0 || !plain) {
      ++stats_.bad_view;
      continue;
    }
    if (ts->min_level != 0) {   // a clamped base level: not the plain chain our image holds
      ++stats_.slot_kept_levels;
      continue;
    }
    uint32_t mips = 0;
    last_reason_.clear();
    if (!ContentMatches(ts->image, roles_tex[r], r == 0 && options_.faults.skirt_tie, mips)) {
      if (!last_reason_.empty() && differ_notes_.size() < 12) differ_notes_.insert("entry " + std::to_string(join.entry) + " part " + std::to_string(part) + " slot " + std::to_string(role_slot[r]) + " texture '" + arena_->textures[size_t(roles_tex[r])].name + "': " + last_reason_);
      continue;
    }   // F1: no content check passed: the stream's image stays
    if (mips != uint32_t(ts->max_level) + 1) {   // F3: the record's mips and the fetch constant's level range must agree
      ++stats_.slot_kept_levels;
      continue;
    }
    (r == 0 ? d.diffuse : r == 1 ? d.normal_map : d.specular) = roles_tex[r];
    (r == 0 ? d.diffuse_mips : r == 1 ? d.normal_mips : d.specular_mips) = mips;
  }
  d.ok = true;
  d.why = KeepWhy::Count;
  d.reason = "";
  ++stats_.replaced;
  return d;
}

std::string RingCore::Report() const {
  uint64_t objs = 0, high = 0, low = 0;
  std::map<int, uint64_t> per_entry;
  for (const auto& kv : joins_) {
    const ObjJoin& j = kv.second;
    if (j.entry < 0) continue;
    ++objs;
    per_entry[j.entry] += j.last_instances;
    if (j.entry == 960) high += j.last_instances;
    if (j.entry == 980) low += j.last_instances;
  }
  std::string inst;
  for (const auto& kv : per_entry) inst += (inst.empty() ? "" : " ") + std::to_string(kv.first) + "x" + std::to_string(kv.second);
  std::string notes = std::string("; u sign rule: ") + (uv_mode_ == 2 ? "signed (post SDK patch 109)" : uv_mode_ == 1 ? "|u| (before it)" : "not known yet");
  for (const std::string& n : differ_notes_) notes += "; content differs: " + n;
  char b[1200];
  std::snprintf(b, sizeof b,
                "ring %d: objects joined %llu (by content %llu, texture tie %llu, tie left open %llu), not a ring piece %llu, unjoined %llu; instances per entry [%s], rope LOD high %llu / low %llu; "
                "draw checks failed: part %llu, vertices %llu, indices %llu, bytes %llu (layout refused %llu); content: checks %llu ok %llu differs %llu no feed %llu, view %llu; slots left to the stream: rope colour %llu, "
                "open tie %llu, no file texture %llu, canvas tail %llu, base level %llu; joins dropped %llu, verify runs %llu",
                arena_id_, (unsigned long long)objs, (unsigned long long)stats_.joined_content, (unsigned long long)stats_.joined_tie_texture, (unsigned long long)stats_.joined_tie_open,
                (unsigned long long)stats_.foreign, (unsigned long long)stats_.unjoined, inst.c_str(), (unsigned long long)high, (unsigned long long)low, (unsigned long long)stats_.bad_part,
                (unsigned long long)stats_.bad_vertices, (unsigned long long)stats_.bad_indices, (unsigned long long)stats_.bad_bytes, (unsigned long long)stats_.layout_refused,
                (unsigned long long)stats_.content_checks, (unsigned long long)stats_.content_ok, (unsigned long long)stats_.content_differs, (unsigned long long)stats_.content_unknown, (unsigned long long)stats_.bad_view, (unsigned long long)stats_.slot_kept_rope,
                (unsigned long long)stats_.slot_kept_tie, (unsigned long long)stats_.slot_kept_no_file, (unsigned long long)stats_.slot_kept_tail, (unsigned long long)stats_.slot_kept_levels, (unsigned long long)stats_.joins_dropped, (unsigned long long)stats_.verify_runs);
  return b + notes;
}

}  // namespace rdr_scene
