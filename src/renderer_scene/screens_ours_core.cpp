// See screens_ours_core.h. S101 lane 115 (a copy of props_ours_core.cpp's design on the screens' file side, geometry only).
#include "screens_ours_core.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>

namespace rdr_scene {
namespace {

using assets::ring_gs::RingMesh;
using rdr_adapter::KeepWhy;

uint64_t Mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

uint64_t LayoutSig(const rdr_adapter::LoadedFrame& f, const rdr_adapter::VertexLayout& lay) {
  uint64_t h = 0xcbf29ce484222325ull;
  h = Mix(h, lay.host_stride), h = Mix(h, lay.guest_stride), h = Mix(h, lay.element_count);
  for (uint32_t e = 0; e < lay.element_count; ++e) {
    const rdr_adapter::VertexElement& el = f.elements[lay.element_first + e];
    h = Mix(h, el.vk_format), h = Mix(h, el.host_offset), h = Mix(h, el.source_format), h = Mix(h, el.flags), h = Mix(h, uint32_t(el.guest_offset));
  }
  return h;
}

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

// The per-key exception (UNCLAIMED_MODEL_PARTS.md 1.2, measured on ops_f200.json): the file says yBG_Default, the stream draws the lit movie effect yMovieDefault.
bool IsMovieProgram(const char* ps_name) { return ps_name && std::strncmp(ps_name, "yMovieDefault", 13) == 0; }
bool IsFileScreenEffect(const std::string& effect) { return effect.compare(0, 10, "yBG_Defaul") == 0; }

}  // namespace

bool ScreensFaults::Parse(const std::string& list, ScreensFaults& f, std::string& error) {
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
    else if (w == "pair_swap") f.pair_swap = true;
    else if (w == "no_verify") f.no_verify = true;
    else if (w == "uv_from_pair") f.uv_from_pair = true;
    else if (w == "class_refuse") f.class_refuse = true;
    else if (w == "file_effect") f.file_effect = true;
    else if (w == "stale_switch") f.stale_switch = true;
    else if (w == "frozen_clock") f.frozen_clock = true;
    else {
      error = "unknown screens fault '" + w + "' (colour_order, uv_halves_swapped, uv_sign, pair_swap, no_verify, uv_from_pair, class_refuse, file_effect, stale_switch, frozen_clock)";
      return false;
    }
  }
  return true;
}

void ScreensCore::ReleaseAll() {
  if (release_)
    for (auto& kv : prepared_) release_(*kv.second);
  prepared_.clear();
}

void ScreensCore::SetScreens(std::shared_ptr<const assets::screen_gs::ScreenSet> set) {
  if (options_.faults.stale_switch && set_ && set) {   // the planted fault: another arena's set swapped in under the old state
    set_ = std::move(set);
    mesh_by_key_.clear();
    for (size_t i = 0; i < set_->meshes.size(); ++i) mesh_by_key_[uint32_t(set_->meshes[i].entry) << 16 | uint32_t(set_->meshes[i].part)] = int(i);
    return;
  }
  ReleaseAll();
  set_ = std::move(set);
  mesh_by_key_.clear();
  joins_.clear();
  foreign_.clear();
  byte_fails_.clear();
  uv_mode_ = 0;
  verified_key_.clear();
  if (!set_) return;
  for (size_t i = 0; i < set_->meshes.size(); ++i) mesh_by_key_[uint32_t(set_->meshes[i].entry) << 16 | uint32_t(set_->meshes[i].part)] = int(i);
}

bool ScreensCore::Claims(const rdr_adapter::DrawOrigin& o) const {
  if (o.klass != rdr_adapter::DrawClass::ModelPart || o.pass_site != kModelPartScreensSite) return false;
  return !IsForeign(o.object);
}

int ScreensCore::MeshIndex(int key, int part) const {
  const auto it = mesh_by_key_.find(uint32_t(key) << 16 | uint32_t(part));
  return it == mesh_by_key_.end() ? -1 : it->second;
}

// ---- the join, once per object ------------------------------------------------------------------------------------------------------------------------------

bool ScreensCore::JoinObject(const rdr_adapter::ReplaceIn& in, ObjJoin& out, const char*& reason) {
  const rdr_adapter::LoadedFrame& f = *in.frame;
  const uint32_t object = in.origin->object;
  std::map<uint32_t, uint32_t> part_draw;   // every part the object draws in this frame (the first draw of each)
  for (uint32_t d = 0; d < f.draws.count; ++d) {
    const rdr_adapter::DrawOrigin* o = f.OriginOf(d);
    if (!o || o->object != object || o->pass_site != kModelPartScreensSite || o->klass != rdr_adapter::DrawClass::ModelPart) continue;
    if ((f.draws[d].flags & rdr_adapter::kDrawNoGeometry) || f.draws[d].layout >= f.layouts.count) continue;
    part_draw.emplace(o->part, d);
  }
  if (part_draw.empty()) return reason = "no geometry", false;
  std::map<uint32_t, std::vector<uint8_t>> bytes;
  out.cands.clear();
  for (int key : set_->keys) {
    bool all = true;
    for (const auto& pd : part_draw) {
      const RingMesh* m = MeshOf(key, int(pd.first));
      const rdr_adapter::Draw& dr = f.draws[pd.second];
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
    if (all) out.cands.push_back(key);
  }
  if (out.cands.empty()) return reason = "not a screen of ours", out.foreign = true, false;
  out.key = out.cands[0];
  out.resolved = out.cands.size() == 1;
  if (out.resolved) ++stats_.joined_unique;
  return true;
}

bool ScreensCore::DetectUvSign(const rdr_adapter::ReplaceIn& in, const rdr_adapter::VertexLayout& lay, const RingMesh& m, bool resolved) {
  if (uv_mode_ != 0) return true;
  if (m.first_neg_u < 0 || !in.read_geometry || !in.frame) return false;
  uint32_t off = 0;
  bool have = false;
  for (uint32_t e = 0; e < lay.element_count; ++e) {
    const rdr_adapter::VertexElement& el = in.frame->elements[lay.element_first + e];
    if (el.source_format == 31 && el.vk_format == 83) off = el.host_offset, have = true;
  }
  std::vector<uint8_t> vb;
  if (!have) return true;   // lane 220 follow-up: no uv element: nothing to learn, nothing depends on the rule
  if (!in.read_geometry(in.vertex_offset, in.vertex_count * lay.host_stride, vb) || vb.size() < size_t(in.vertex_count) * lay.host_stride) return false;
  uint16_t h = 0, hs = 0;
  std::memcpy(&h, vb.data() + size_t(m.first_neg_u) * lay.host_stride + off, 2);
  const bool strict = m.first_strict_neg_u >= 0;
  if (strict) std::memcpy(&hs, vb.data() + size_t(m.first_strict_neg_u) * lay.host_stride + off, 2);
  const int mode = assets::ring_gs::LearnUvMode(h, strict, hs);
  if (mode == 2) return uv_mode_ = 2, true;   // a set sign bit proves signed, whichever candidate it was read from
  if (mode == 0 || !resolved) return false;   // a clear bit proves |u| only on the mesh the stream draws, and only at a strictly negative vertex (lane 220 follow-up)
  return uv_mode_ = 1, true;
}

// ---- our buffers --------------------------------------------------------------------------------------------------------------------------------------------

ScreensPrepared* ScreensCore::Prepare(const rdr_adapter::ReplaceIn& in, const rdr_adapter::VertexLayout& lay, int mi, int rule) {
  const RingMesh& m = set_->meshes[size_t(mi)];
  const uint64_t pkey = ((uint64_t(uint32_t(mi)) << 48) ^ LayoutSig(*in.frame, lay)) ^ (m.first_neg_u >= 0 ? uint64_t(rule) << 62 : 0);
  const auto pit = prepared_.find(pkey);
  if (pit != prepared_.end()) return pit->second->refused ? nullptr : pit->second.get();
  assets::wrestler_geom::Roles roles;
  auto p = std::make_unique<ScreensPrepared>();
  if (!ClassifyLayout(*in.frame, lay, m, roles) || !assets::ring_gs::WriteVertices(m, roles, options_.faults.Writer(), rule == 2, p->vertex)) p->refused = true;
  else p->index = assets::ring_gs::BuildIndices(m), ++stats_.prepared_built;
  ScreensPrepared* raw = p.get();
  prepared_.emplace(pkey, std::move(p));
  return raw->refused ? nullptr : raw;
}

// ---- the decision ---------------------------------------------------------------------------------------------------------------------------------------------

ScreensCore::Decision ScreensCore::Decide(const rdr_adapter::ReplaceIn& in, const char* ps_name) {
  Decision d;
  ++stats_.decisions;
  const auto keep = [&](KeepWhy why, const char* reason) -> Decision {
    d.ok = false, d.why = why, d.reason = reason;
    ++stats_.kept[uint8_t(why)];
    return d;
  };
  if (!set_) return keep(KeepWhy::NotReady, "no screens");
  const rdr_adapter::DrawOrigin* o = in.origin;
  if (!o || o->klass != rdr_adapter::DrawClass::ModelPart || o->pass_site != kModelPartScreensSite || !in.frame || !in.draw) return keep(KeepWhy::NotOurs, "site");
  const rdr_adapter::LoadedFrame& f = *in.frame;
  const rdr_adapter::Draw& dr = *in.draw;
  if (IsForeign(o->object)) return keep(KeepWhy::NotOurs, "foreign object");
  if (foreign_.erase(o->object)) byte_fails_.erase(o->object);   // an expired verdict: judged again below from scratch
  if ((dr.flags & (rdr_adapter::kDrawNoGeometry | rdr_adapter::kDrawInline)) || dr.layout >= f.layouts.count || dr.index_32bit || !dr.index_bytes) return keep(KeepWhy::Variant, "draw shape");
  if (options_.faults.class_refuse) ps_name = "yDefault_base_fragment";   // the planted fault
  if (!IsMovieProgram(ps_name)) {   // the per-key exception is "file yBG_Default, stream yMovieDefault": any other program at this site is not a screen of ours
    ++stats_.bad_class;
    return keep(KeepWhy::Variant, "stream fragment program is not yMovieDefault");
  }

  auto it = joins_.find(o->object);
  if (it == joins_.end() || it->second.key < 0) {
    if (it != joins_.end() && frame_no_ < it->second.blocked_until) return it->second.blocked_unjoined ? keep(KeepWhy::Unjoined, "blocked: the join could not be made") : keep(KeepWhy::Mismatch, "blocked after a failed check");
    ObjJoin j;
    const char* why = "";
    if (!JoinObject(in, j, why)) {
      if (j.foreign) {
        foreign_[o->object] = frame_no_ + 300;
        ++stats_.foreign;
        return keep(KeepWhy::NotOurs, why);
      }
      ++stats_.unjoined;
      ObjJoin b;
      b.blocked_until = frame_no_ + 60, b.blocked_unjoined = true;
      joins_[o->object] = b;
      return keep(KeepWhy::Unjoined, why);
    }
    it = joins_.insert_or_assign(o->object, j).first;
  }
  ObjJoin& join = it->second;
  const int part = int(o->part);
  const auto drop = [&](uint64_t& counter, const char* why, bool count_fail = true) {   // lane 115 F2: count_fail false for "geometry unreadable" (a missing read is not evidence against the join)
    ++counter;
    ++stats_.joins_dropped;
    if (first_fail_.empty()) {
      char b[200];
      std::snprintf(b, sizeof b, "%s (object 0x%08X part %d, stream %u vertices %u indices, key %d)", why, o->object, part, in.vertex_count, in.index_count, join.key);
      first_fail_ = b + (detail_.empty() ? std::string() : " " + detail_);
    }
    detail_.clear();
    ObjJoin dropped;
    dropped.blocked_until = frame_no_ + 120;
    for (auto vk = verified_key_.begin(); vk != verified_key_.end();) vk = (vk->first >> 32) == o->object ? verified_key_.erase(vk) : std::next(vk);
    // A lookalike (the props' WM30 lesson): positions equal a screen, bytes do not. Never retried in a loop: a second failure (or a fourth) makes the object NotOurs for 300 frames.
    uint32_t& fails = byte_fails_[o->object];
    if (count_fail) ++fails;
    if (count_fail && fails >= 2) {
      foreign_[o->object] = frame_no_ + 300;
      ++stats_.lookalike;
      dropped.blocked_until = 0;
    }
    joins_[o->object] = dropped;
    return keep(KeepWhy::Mismatch, why);
  };

  const rdr_adapter::VertexLayout& lay = f.layouts[dr.layout];
  // A pair (identical positions): told apart by CONTENT at the first draw: the candidate whose full vertex buffer and index buffer equal the stream's.
  if (!join.resolved) {
    std::vector<uint8_t> vb, ib;
    if (!in.read_geometry || !in.read_geometry(in.vertex_offset, in.vertex_count * lay.host_stride, vb) || !in.read_geometry(in.index_offset, in.index_count * 2, ib)) return drop(stats_.bad_bytes, "geometry unreadable", false);   // lane 115 F2
    std::vector<int> equal;
    std::map<int, int> rule_mask;   // candidate -> bit 1 = |u| bytes equal, bit 2 = signed bytes equal
    for (int cand : join.cands) {
      const int ci = MeshIndex(cand, part);
      if (ci < 0) continue;
      const RingMesh& cm = set_->meshes[size_t(ci)];
      // lane 115 F1: a set sign bit teaches "signed" from any candidate; a clear bit teaches nothing here (unresolved). The planted uv_from_pair is the old defect: it believed a clear bit.
      if (cm.first_neg_u >= 0) DetectUvSign(in, lay, cm, options_.faults.uv_from_pair);
      // Both rules are tried when the rule is still unknown: the (candidate, rule) whose bytes equal the stream's wins.
      const int rules[2] = {uv_mode_ ? uv_mode_ : 2, 1};
      for (int ri = 0; ri < (uv_mode_ ? 1 : 2); ++ri) {
        ScreensPrepared* cp = Prepare(in, lay, ci, rules[ri]);
        if (!cp || cp->vertex.empty() || vb.size() < cp->vertex.size() || ib.size() < cp->index.size() * 2) continue;
        if (std::memcmp(vb.data(), cp->vertex.data(), cp->vertex.size()) == 0 && cp->index.size() == in.index_count && std::memcmp(ib.data(), cp->index.data(), cp->index.size() * 2) == 0) {
          if (!rule_mask[cand]) equal.push_back(cand);
          rule_mask[cand] |= rules[ri] == 2 ? 2 : 1;
        }
      }
    }
    if (options_.faults.pair_swap) {   // the planted fault: the OTHER member of the pair (the first one that content does not name; 801 joined as 802 and the reverse)
      join.key = join.cands.back();
      for (int cand : join.cands)
        if (std::find(equal.begin(), equal.end(), cand) == equal.end()) {
          join.key = cand;
          break;
        }
      join.resolved = true;
      ++stats_.joined_tie_content;
    } else if (equal.empty()) {
      join.resolved = true;   // (the checks below name the failure)
      join.key = join.cands[0];
    } else {
      join.key = equal[0];   // the lowest key among the byte-equal members
      join.resolved = true;
      if (uv_mode_ == 0) {   // the bytes themselves taught the rule when only one of the two rules reproduces them
        const int mask = rule_mask[join.key];
        if (mask == 1 || mask == 2) uv_mode_ = mask, ++stats_.uv_learned_from_bytes;
      }
      ++(equal.size() > 1 ? stats_.joined_tie_interchangeable : stats_.joined_tie_content);
    }
  }

  const int mi = MeshIndex(join.key, part);
  if (mi < 0) return drop(stats_.bad_part, "part");
  const RingMesh& m = set_->meshes[size_t(mi)];
  if (in.vertex_count != m.vertices) return drop(stats_.bad_vertices, "vertices");
  if (in.index_count != m.StreamIndexCount()) return drop(stats_.bad_indices, "indices");
  if (!IsFileScreenEffect(options_.faults.file_effect ? std::string("yProp_Default") : m.effect)) {   // the exception is for the file's yBG_Default screens only
    ++stats_.bad_class;
    return keep(KeepWhy::Variant, "file effect is not yBG_Default");
  }
  if (m.first_neg_u >= 0 && !DetectUvSign(in, lay, m, true)) {
    ++stats_.layout_refused;
    return keep(KeepWhy::Variant, "u sign rule not known");
  }
  ScreensPrepared* pr = Prepare(in, lay, mi, uv_mode_);
  if (!pr) {
    ++stats_.layout_refused;
    return keep(KeepWhy::Variant, "layout / writer");
  }
  if (pr->failed) return keep(KeepWhy::Mismatch, "buffers could not be made");
  if (pr->index.size() != in.index_count) return drop(stats_.bad_indices, "index build");

  const uint64_t vk = (uint64_t(o->object) << 32) | uint32_t(part);
  const auto vit = verified_key_.find(vk);
  const bool fresh = vit == verified_key_.end() || vit->second.vertex != in.vertex_offset || vit->second.index != in.index_offset || vit->second.mesh != mi;
  if (!options_.faults.no_verify && in.read_geometry && (options_.verify_all || fresh)) {
    ++stats_.verify_runs;
    std::vector<uint8_t> vb, ib;
    if (!in.read_geometry(in.vertex_offset, in.vertex_count * lay.host_stride, vb) || vb.size() < pr->vertex.size() ||
        !in.read_geometry(in.index_offset, in.index_count * 2, ib) || ib.size() < pr->index.size() * 2)
      return drop(stats_.bad_bytes, "geometry unreadable", false);   // lane 115 F2
    if (std::memcmp(vb.data(), pr->vertex.data(), pr->vertex.size()) != 0) {
      size_t at = 0;   // where: the first differing byte, its vertex, the stream's element that holds it
      while (at < pr->vertex.size() && vb[at] == pr->vertex[at]) ++at;
      const uint32_t stride = lay.host_stride ? lay.host_stride : 1, voff = uint32_t(at % stride);
      const rdr_adapter::VertexElement* hit = nullptr;
      for (uint32_t e = 0; e < lay.element_count; ++e) {
        const rdr_adapter::VertexElement& el = f.elements[lay.element_first + e];
        if (el.host_offset <= voff && (!hit || el.host_offset >= hit->host_offset)) hit = &el;
      }
      char b[220];
      std::snprintf(b, sizeof b, "vertex bytes: first difference at vertex %zu byte %u (element vk_format %u source_format %u at offset %u): stream 0x%02X ours 0x%02X", at / stride, voff, hit ? hit->vk_format : 0u, hit ? hit->source_format : 0u,
                    hit ? hit->host_offset : 0u, vb[at], pr->vertex[at]);
      detail_ = b;
      return drop(stats_.bad_bytes, "vertex bytes");
    }
    if (std::memcmp(ib.data(), pr->index.data(), pr->index.size() * 2) != 0) return drop(stats_.bad_bytes, "index bytes");
    verified_key_[vk] = {in.vertex_offset, in.index_offset, mi};
  }
  d.mesh = mi;
  d.prepared = pr;
  d.ok = true;
  d.why = KeepWhy::Count;
  d.reason = "";
  ++stats_.replaced;
  return d;
}

std::string ScreensCore::Report() const {
  std::map<int, uint64_t> per;
  for (const auto& kv : joins_)
    if (kv.second.key >= 0 && !kv.second.foreign) ++per[kv.second.key];
  std::string objs;
  for (const auto& kv : per) objs += (objs.empty() ? "" : ", ") + std::to_string(kv.first) + (kv.second > 1 ? " x" + std::to_string(kv.second) : "");
  char b[800];
  std::snprintf(b, sizeof b,
                "screens: objects joined to keys [%s] (unique %llu, pair told apart by content %llu, pair interchangeable %llu), not a screen of ours %llu, unjoined %llu, class refused %llu; draw checks failed: part %llu, vertices %llu, indices %llu, bytes %llu "
                "(layout refused %llu); joins dropped %llu, verify runs %llu, lookalike objects given up %llu; u sign rule: %s",
                objs.c_str(), (unsigned long long)stats_.joined_unique, (unsigned long long)stats_.joined_tie_content, (unsigned long long)stats_.joined_tie_interchangeable, (unsigned long long)stats_.foreign, (unsigned long long)stats_.unjoined,
                (unsigned long long)stats_.bad_class, (unsigned long long)stats_.bad_part, (unsigned long long)stats_.bad_vertices, (unsigned long long)stats_.bad_indices, (unsigned long long)stats_.bad_bytes, (unsigned long long)stats_.layout_refused,
                (unsigned long long)stats_.joins_dropped, (unsigned long long)stats_.verify_runs, (unsigned long long)stats_.lookalike,
                uv_mode_ == 2 ? "signed (post SDK patch 109)" : uv_mode_ == 1 ? "|u| (before it)" : "not known yet");
  return b;
}

}  // namespace rdr_scene
