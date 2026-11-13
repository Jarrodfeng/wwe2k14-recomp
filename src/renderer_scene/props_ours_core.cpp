// See props_ours_core.h. S101 lane 84 (a copy of ring_ours_core.cpp's design on the props' file side).
#include "props_ours_core.h"
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
using rdr_adapter::KeepWhy;

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

}  // namespace

bool PropsFaults::Parse(const std::string& list, PropsFaults& f, std::string& error) {
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
    else if (w == "counts_only_join") f.counts_only_join = true;
    else if (w == "wrong_table_variant") f.wrong_table_variant = true;
    else if (w == "steps_position_swap") f.steps_position_swap = true;
    else if (w == "lookalike") f.lookalike = true;
    else if (w == "scene_wrong") f.scene_wrong = true;   // lane 137
    else if (w == "scene_lookalike_p1") f.scene_lookalike_p1 = true;   // lane 137
    else if (w == "scene_lookalike_both") f.scene_lookalike_both = true;   // lane 137
    else if (w == "scene_desk_105") f.scene_desk_105 = true;   // lane 137
    else if (w == "special_ids") f.special_ids = true;   // lane 254
    else if (w == "editor_as_match") f.editor_as_match = true;   // lane 254
    else if (w == "scene_off") f.scene_off = true;   // lane 137
    else if (w == "neg_zero_u") f.neg_zero_u = true;   // lane 220
    else if (w == "zero_cleared") f.zero_cleared = true;   // lane 220 follow-up
    else if (w == "teach_abs") f.teach_abs = true;   // lane 220 follow-up
    else if (w == "lambert_off") {}                  // lane 137: a ring fault; fview hands one --replace-fault list to every class
    else if (w == "all_mips") f.all_mips = true;
    else if (w == "no_verify") f.no_verify = true;
    else if (w == "frozen_clock") f.frozen_clock = true;   // lane 115 F2
    else {
      error = "unknown props fault '" + w + "' (colour_order, uv_halves_swapped, uv_sign, bone_zero, weight_word, counts_only_join, wrong_table_variant, steps_position_swap, lookalike, scene_wrong, scene_lookalike_p1, scene_lookalike_both, scene_desk_105, scene_off, neg_zero_u, zero_cleared, teach_abs, all_mips, no_verify, frozen_clock, editor_as_match, special_ids)";
      return false;
    }
  }
  return true;
}

void PropsCore::ReleaseAll() {
  if (release_)
    for (auto& kv : prepared_) release_(*kv.second);
  prepared_.clear();
}

void PropsCore::SetProps(std::shared_ptr<const assets::prop_gs::PropSet> props) {
  ReleaseAll();
  props_ = std::move(props);
  mesh_by_key_.clear();
  joins_.clear();
  foreign_.clear();
  bad_sources_.clear();
  byte_fails_.clear();
  content_ok_.clear();
  file_hash_.clear();
  uv_mode_ = 0;
  verified_key_.clear();
  if (!props_) return;
  for (size_t i = 0; i < props_->meshes.size(); ++i) mesh_by_key_[uint32_t(props_->meshes[i].entry) << 16 | uint32_t(props_->meshes[i].part)] = int(i);
}

bool PropsCore::Claims(const rdr_adapter::DrawOrigin& o) const {
  if (o.klass != rdr_adapter::DrawClass::ModelPart || o.pass_site != kModelPartColourSite) return false;
  return !IsForeign(o.object);
}

int PropsCore::MeshIndex(int source, int part) const {
  const auto it = mesh_by_key_.find(uint32_t(source) << 16 | uint32_t(part));
  return it == mesh_by_key_.end() ? -1 : it->second;
}

// ---- the join, once per object ------------------------------------------------------------------------------------------------------------------------------

bool PropsCore::JoinObject(const rdr_adapter::ReplaceIn& in, ObjJoin& out, const char*& reason) {
  const rdr_adapter::LoadedFrame& f = *in.frame;
  const uint32_t object = in.origin->object;
  std::map<uint32_t, uint32_t> part_draw;   // every part the object draws in this frame (the first draw of each)
  for (uint32_t d = 0; d < f.draws.count; ++d) {
    const rdr_adapter::DrawOrigin* o = f.OriginOf(d);
    if (!o || o->object != object || o->pass_site != kModelPartColourSite || o->klass != rdr_adapter::DrawClass::ModelPart) continue;
    if ((f.draws[d].flags & rdr_adapter::kDrawNoGeometry) || f.draws[d].layout >= f.layouts.count) continue;
    part_draw.emplace(o->part, d);
  }
  if (part_draw.empty()) return reason = "no geometry", false;
  std::vector<int> cands;
  std::map<uint32_t, std::vector<uint8_t>> bytes;
  const auto bad_it = bad_sources_.find(object);
  for (const assets::prop_gs::Source& src : props_->sources) {
    if (options_.faults.scene_off && src.group == assets::prop_gs::Group::Scene) continue;   // lane 137 plant: the pre-promotion state (no scene sources)
    if (bad_it != bad_sources_.end() && std::find(bad_it->second.begin(), bad_it->second.end(), src.id) != bad_it->second.end()) continue;   // its bytes were already shown not to be this object's
    bool all = true;
    for (const auto& pd : part_draw) {
      const RingMesh* m = MeshOf(src.id, int(pd.first));
      const rdr_adapter::Draw& dr = f.draws[pd.second];
      if (!m || m->vertices != dr.vertex_count || m->StreamIndexCount() != dr.index_count) {
        all = false;
        break;
      }
      if (options_.faults.counts_only_join) continue;   // the planted counts-only join: no position check
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
    if (all) cands.push_back(src.id);
  }
  if (cands.empty()) return reason = "not a prop of ours", out.foreign = true, false;
  int pick = cands[0];
  const bool tie = cands.size() > 1;
  out.slot0_open = out.tie_texture = out.force_slot0 = false;
  if (options_.faults.counts_only_join) pick = cands.back();
  else if (tie) {
    if (options_.faults.wrong_table_variant && props_->SourceOf(cands[0])->group == assets::prop_gs::Group::Table) {
      pick = cands.back();
      out.force_slot0 = true;
    } else {
      // the slot-0 image content over every gathered part: the candidate whose textures the stream holds on the most parts
      int best_score = 0;
      for (int cand : cands) {
        int score = 0;
        for (const auto& pd : part_draw) {
          const rdr_adapter::Draw& dr = f.draws[pd.second];
          uint32_t image = 0;
          for (uint32_t t = 0; t < dr.texture_count && dr.texture_first + t < f.texture_slots.count; ++t)
            if (f.texture_slots[dr.texture_first + t].slot == 0) image = f.texture_slots[dr.texture_first + t].image;
          const RingMesh* m = MeshOf(cand, int(pd.first));
          uint32_t mips = 0;
          last_reason_.clear();
          if (image && m && m->diffuse >= 0 && ContentMatches(image, m->diffuse, false, mips)) ++score;
          else if (!last_reason_.empty() && differ_notes_.size() < 12 && cand == cands[0])
            differ_notes_.insert("tie candidate " + props_->SourceOf(cand)->label + " part " + std::to_string(pd.first) + " slot 0 texture '" + props_->textures[size_t(m->diffuse)].name + "': " + last_reason_);
        }
        if (score > best_score) best_score = score, pick = cand;
      }
      if (best_score > 0) out.tie_texture = true;
      else {
        out.slot0_open = true;
        // The steps' stair textures never hash equal (the stream holds smaller mips that differ from the file's: the canvas's "tail" case, lane 79: mip 0 equal, the smallest mips differ), so the tie stays open and
        // slots stay the stream's. The geometry of all 10 step leaves is byte-identical; the "32" family is the one whose mip 0 matched the stream's on Check B (UNCLAIMED_MODEL_PARTS.md 1.1 [C]).
        for (int cand : cands)
          if (props_->SourceOf(cand)->family32) {
            pick = cand;
            break;
          }
      }
    }
  }
  if (options_.faults.scene_desk_105 && tie)   // lane 137 plant: the desk joined to p9/105 with its file picture forced into slot 0
    for (int cand : cands)
      if (props_->SourceOf(cand)->label == "bgEtc p9/105/0") pick = cand, out.force_slot0 = true;
  if (options_.faults.scene_wrong &&props_->SourceOf(pick)->group == assets::prop_gs::Group::Scene) pick = 0;   // lane 137 plant: the first source (a step leaf), whatever the object is
  out.source = pick;
  out.tied = tie;
  ++(tie && !options_.faults.counts_only_join ? (out.tie_texture ? stats_.joined_tie_texture : stats_.joined_tie_open) : stats_.joined_content);
  return true;
}

int PropsCore::FirstNegU(const RingMesh& m) const {
  if (!options_.faults.neg_zero_u) return m.first_neg_u;
  for (uint32_t v = 0; v < m.vertices; ++v)   // the planted old rule: strictly below zero ("-0.0" does not count)
    if (assets::BeF(m.data.s[2] + 8ull * v) < 0.0f) return int(v);
  return -1;
}

int PropsCore::StrictNegU(const RingMesh& m) const {
  if (!options_.faults.neg_zero_u) return m.first_strict_neg_u;
  return FirstNegU(m);   // the planted old rule: strictly below zero is all it knew
}

bool PropsCore::DetectUvSign(const rdr_adapter::ReplaceIn& in, const rdr_adapter::VertexLayout& lay, const RingMesh& m) {
  if (uv_mode_ != 0) return true;
  const int first_neg = FirstNegU(m);
  if (first_neg < 0 || !in.read_geometry || !in.frame) return false;
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
  std::memcpy(&h, vb.data() + size_t(first_neg) * lay.host_stride + off, 2);
  if (options_.faults.zero_cleared) h &= 0x7FFFu;   // plant: the game cleared the bit
  const int strict_at = StrictNegU(m);
  if (strict_at >= 0) std::memcpy(&hs, vb.data() + size_t(strict_at) * lay.host_stride + off, 2);
  const int mode = options_.faults.teach_abs ? ((h & 0x8000u) ? 2 : 1) : assets::ring_gs::LearnUvMode(h, strict_at >= 0, hs);   // lane 220 follow-up: |u| only from a clear bit at a strictly negative vertex
  if (mode == 0) return false;
  uv_mode_ = mode;
  return true;
}

uint64_t PropsCore::FileHash(int texture, uint32_t mips) {
  const uint64_t key = (uint64_t(uint32_t(texture)) << 8) | std::min<uint32_t>(mips, 255);
  const auto it = file_hash_.find(key);
  if (it != file_hash_.end()) return it->second;
  const assets::Texture& t = props_->textures[size_t(texture)];
  size_t bytes = 0;
  for (uint32_t m = 0; m < mips; ++m) bytes += MipBytes(t.format, std::max(1u, t.width >> m), std::max(1u, t.height >> m));
  return file_hash_[key] = RingXxh3(t.data.data(), std::min(bytes, t.data.size()));
}

bool PropsCore::ContentMatches(uint32_t image_id, int texture, bool force, uint32_t& mips) {
  rdr_adapter::ImageContent c;
  if (!options_.image_content || !image_id || !options_.image_content(image_id, c) || c.mips == 0) return ++stats_.content_unknown, false;
  mips = c.mips;
  if (force) return true;
  const assets::Texture& t = props_->textures[size_t(texture)];
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

PropsCore::Decision PropsCore::Decide(const rdr_adapter::ReplaceIn& in, const uint8_t* ps_slots, uint32_t ps_slot_count) {
  Decision d;
  ++stats_.decisions;
  const auto keep = [&](KeepWhy why, const char* reason) -> Decision {
    d.ok = false, d.why = why, d.reason = reason;
    ++stats_.kept[uint8_t(why)];
    return d;
  };
  if (!props_) return keep(KeepWhy::NotReady, "no props");
  const rdr_adapter::DrawOrigin* o = in.origin;
  if (!o || o->klass != rdr_adapter::DrawClass::ModelPart || o->pass_site != kModelPartColourSite || !in.frame || !in.draw) return keep(KeepWhy::NotOurs, "site");
  const rdr_adapter::LoadedFrame& f = *in.frame;
  const rdr_adapter::Draw& dr = *in.draw;
  if (IsForeign(o->object)) return keep(KeepWhy::NotOurs, "foreign object");
  if (foreign_.erase(o->object)) bad_sources_.erase(o->object), byte_fails_.erase(o->object);   // an expired verdict: judged again below from scratch
  if ((dr.flags & (rdr_adapter::kDrawNoGeometry | rdr_adapter::kDrawInline)) || dr.layout >= f.layouts.count || dr.index_32bit || !dr.index_bytes) return keep(KeepWhy::Variant, "draw shape");

  auto it = joins_.find(o->object);
  if (it == joins_.end() || it->second.source < 0) {
    if (it != joins_.end() && frame_no_ < it->second.blocked_until) return it->second.blocked_unjoined ? keep(KeepWhy::Unjoined, "blocked: the join could not be made") : keep(KeepWhy::Mismatch, "blocked after a failed check");
    ObjJoin j;
    const char* why = "";
    if (!JoinObject(in, j, why)) {
      if (j.foreign) {
        foreign_[o->object] = frame_no_ + 300;
        ++stats_.foreign;
        if (bad_sources_.count(o->object)) ++stats_.lookalike;   // lane 220: every candidate's bytes were shown not to be this object's: a lookalike given up (not a mismatch)
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
      char b[200];   // (the byte-compare text rides in first_fail_ as detail_, not in b)
      std::snprintf(b, sizeof b, "%s (object 0x%08X part %d, stream %u vertices %u indices, source %d)", why, o->object, part, in.vertex_count, in.index_count, join.source);
      first_fail_ = b + (detail_.empty() ? std::string() : " " + detail_);
    }
    detail_.clear();
    ObjJoin dropped;
    dropped.blocked_until = frame_no_ + 120;
    for (auto vk = verified_key_.begin(); vk != verified_key_.end();) vk = (vk->first >> 32) == o->object ? verified_key_.erase(vk) : std::next(vk);
    // A lookalike (S101 lane 84, WM30 run 7084: 10,144 draws kept as mismatch): positions equal a prop, bytes do not. Never retried in a loop: the source is excluded and the object re-joins at once
    // among the others (a tie); when none is left, or a single-candidate join fails twice, the object is NotOurs for 300 frames.
    uint32_t& fails = byte_fails_[o->object];
    if (count_fail) ++fails;   // lane 115 F2
    bool retry_now = false;   // lane 220: a tied candidate's bytes failed: the join is being RESOLVED (the next candidate is tried in this same call), not a check on an established join failing
    if (join.tied && count_fail) {
      bad_sources_[o->object].push_back(join.source);
      dropped.blocked_until = 0;
      retry_now = true;
    }
    if (count_fail && ((!join.tied && fails >= 2) || fails >= 4)) {
      foreign_[o->object] = frame_no_ + 300;
      ++stats_.lookalike;
      dropped.blocked_until = 0;
      retry_now = false;
    }
    joins_[o->object] = dropped;
    if (retry_now && redecide_ < 8) {   // (the dropped candidate is in bad_sources_, so each round has one candidate fewer; all gone = "not a prop of ours", NotOurs)
      ++redecide_;
      --stats_.decisions;   // the nested call counts this draw again
      const Decision again = Decide(in, ps_slots, ps_slot_count);
      --redecide_;
      return again;
    }
    return keep(KeepWhy::Mismatch, why);
  };

  const int mi = MeshIndex(join.source, part);
  if (mi < 0) return drop(stats_.bad_part, "part");
  const RingMesh& m = props_->meshes[size_t(mi)];
  if (in.vertex_count != m.vertices) return drop(stats_.bad_vertices, "vertices");
  if (in.index_count != m.StreamIndexCount()) return drop(stats_.bad_indices, "indices");
  const rdr_adapter::VertexLayout& lay = f.layouts[dr.layout];
  if (FirstNegU(m) >= 0 && !DetectUvSign(in, lay, m)) {
    ++stats_.layout_refused;
    return keep(KeepWhy::Variant, "u sign rule not known");
  }
  const bool swap_xz = options_.faults.steps_position_swap && props_->SourceOf(join.source)->group == assets::prop_gs::Group::Steps;
  const uint64_t pkey = ((uint64_t(uint32_t(mi)) << 48) ^ LayoutSig(f, lay)) ^ (FirstNegU(m) >= 0 ? uint64_t(uv_mode_) << 62 : 0);
  PropsPrepared* pr;
  const auto pit = prepared_.find(pkey);
  if (pit != prepared_.end()) pr = pit->second.get();
  else {
    assets::wrestler_geom::Roles roles;
    if (!ClassifyLayout(f, lay, m, roles)) {
      auto refused = std::make_unique<PropsPrepared>();
      refused->refused = true;
      prepared_.emplace(pkey, std::move(refused));
      pr = nullptr;
    } else {
      auto p = std::make_unique<PropsPrepared>();
      if (!assets::ring_gs::WriteVertices(m, roles, options_.faults.Writer(), uv_mode_ == 2, p->vertex)) p->refused = true;
      else {
        if (swap_xz)
          for (const assets::wrestler_geom::Roles::R& r : roles.r)
            if (r.role == assets::wrestler_geom::Role::Pos)
              for (uint32_t v = 0; v < m.vertices; ++v) {
                uint8_t* at = p->vertex.data() + size_t(v) * roles.host_stride + r.offset;
                uint8_t tmp[4];
                std::memcpy(tmp, at, 4), std::memcpy(at, at + 8, 4), std::memcpy(at + 8, tmp, 4);
              }
        if (options_.faults.lookalike && props_->SourceOf(join.source)->group == assets::prop_gs::Group::Mic && !p->vertex.empty()) p->vertex[0] ^= 1;
        if ((options_.faults.scene_lookalike_p1 || options_.faults.scene_lookalike_both) && !p->vertex.empty()) {   // lane 137 plants: the cage twin PACH 1 (and PACH 2) written with a wrong first vertex byte
          const std::string& lb = props_->SourceOf(join.source)->label;
          if (lb.rfind("bgEtc p1/1/", 0) == 0 || (options_.faults.scene_lookalike_both && lb.rfind("bgEtc p2/1/", 0) == 0)) p->vertex[0] ^= 1;
        }
        p->index = assets::ring_gs::BuildIndices(m), ++stats_.prepared_built;
      }
      pr = prepared_.emplace(pkey, std::move(p)).first->second.get();
    }
  }
  if (!pr || pr->refused) {
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
      // where: the first differing byte, its vertex, the stream's element that holds it (so the next log names the role: position, ids, normal, colour or uv)
      size_t at = 0;
      while (at < pr->vertex.size() && vb[at] == pr->vertex[at]) ++at;
      const uint32_t stride = lay.host_stride ? lay.host_stride : 1, voff = uint32_t(at % stride);
      const rdr_adapter::VertexElement* hit = nullptr;
      for (uint32_t e = 0; e < lay.element_count; ++e) {
        const rdr_adapter::VertexElement& el = f.elements[lay.element_first + e];
        if (el.host_offset <= voff && (!hit || el.host_offset >= hit->host_offset)) hit = &el;
      }
      uint32_t differing = 0;   // lane 220: how many vertices differ (a mirrored / re-coloured copy differs in many, a CPU-written stream in some)
      for (uint32_t v = 0; v < in.vertex_count; ++v)
        if (std::memcmp(vb.data() + size_t(v) * stride, pr->vertex.data() + size_t(v) * stride, stride) != 0) ++differing;
      char b[260];
      std::snprintf(b, sizeof b, "vertex bytes: first difference at vertex %zu byte %u (element vk_format %u source_format %u at offset %u): stream 0x%02X ours 0x%02X; %u of %u vertices differ", at / stride, voff, hit ? hit->vk_format : 0u, hit ? hit->source_format : 0u,
                    hit ? hit->host_offset : 0u, vb[at], pr->vertex[at], differing, in.vertex_count);
      detail_ = b;
      return drop(stats_.bad_bytes, "vertex bytes");
    }
    if (std::memcmp(ib.data(), pr->index.data(), pr->index.size() * 2) != 0) return drop(stats_.bad_bytes, "index bytes");
    verified_key_[vk] = {in.vertex_offset, in.index_offset, mi};
  }
  d.mesh = mi;
  d.prepared = pr;

  const int roles_tex[3] = {m.diffuse, m.normal_map, m.specular};
  const uint8_t role_slot[3] = {0, 4, 5};
  for (int r = 0; r < 3; ++r) {
    bool wants = false;
    for (uint32_t s = 0; s < ps_slot_count; ++s) wants = wants || ps_slots[s] == role_slot[r];
    if (!wants) continue;
    if (roles_tex[r] < 0) {
      ++stats_.slot_kept_no_file;
      continue;
    }
    if (join.slot0_open) {   // an open tie names no source for the texture: all three roles stay the stream's
      ++stats_.slot_kept_tie;
      continue;
    }
    const rdr_adapter::TextureSlot* ts = nullptr;
    for (uint32_t t = 0; t < dr.texture_count && dr.texture_first + t < f.texture_slots.count; ++t)
      if (f.texture_slots[dr.texture_first + t].slot == role_slot[r]) ts = &f.texture_slots[dr.texture_first + t];
    if (!ts || ts->image == 0) {
      ++stats_.slot_kept_no_file;
      continue;
    }
    const bool plain = (!ts->swizzle[0] && !ts->swizzle[1] && !ts->swizzle[2] && !ts->swizzle[3]) || (ts->swizzle[0] == 3 && ts->swizzle[1] == 4 && ts->swizzle[2] == 5 && ts->swizzle[3] == 6);
    if (ts->view_format != 0 || !plain) {
      ++stats_.bad_view;
      continue;
    }
    if (ts->min_level != 0) {
      ++stats_.slot_kept_levels;
      continue;
    }
    uint32_t mips = 0;
    last_reason_.clear();
    if (!ContentMatches(ts->image, roles_tex[r], r == 0 && join.force_slot0, mips)) {
      if (!last_reason_.empty() && differ_notes_.size() < 12)
        differ_notes_.insert(props_->SourceOf(join.source)->label + " part " + std::to_string(part) + " slot " + std::to_string(role_slot[r]) + " texture '" + props_->textures[size_t(roles_tex[r])].name + "': " + last_reason_);
      continue;
    }
    if (mips != uint32_t(ts->max_level) + 1) {   // the record's mips and the fetch constant's level range must agree
      ++stats_.slot_kept_levels;
      continue;
    }
    if (options_.faults.all_mips) mips = std::max(1u, props_->textures[size_t(roles_tex[r])].mips);   // the planted fault: the file's whole chain
    (r == 0 ? d.diffuse : r == 1 ? d.normal_map : d.specular) = roles_tex[r];
    (r == 0 ? d.diffuse_mips : r == 1 ? d.normal_mips : d.specular_mips) = mips;
  }
  d.ok = true;
  d.why = KeepWhy::Count;
  d.reason = "";
  ++stats_.replaced;
  return d;
}

std::string PropsCore::Report() const {
  std::map<std::string, uint64_t> per;
  for (const auto& kv : joins_)
    if (kv.second.source >= 0) ++per[props_->SourceOf(kv.second.source)->label];
  std::string objs;
  for (const auto& kv : per) objs += (objs.empty() ? "" : ", ") + kv.first + (kv.second > 1 ? " x" + std::to_string(kv.second) : "");
  std::string notes = std::string("; u sign rule: ") + (uv_mode_ == 2 ? "signed (post SDK patch 109)" : uv_mode_ == 1 ? "|u| (before it)" : "not known yet");
  for (const std::string& n : differ_notes_) notes += "; content differs: " + n;
  char b[1000];
  std::snprintf(b, sizeof b,
                "props: objects joined [%s] (by content %llu, texture tie %llu, tie left open %llu), not a prop of ours %llu, unjoined %llu; draw checks failed: part %llu, vertices %llu, indices %llu, bytes %llu (layout refused %llu); "
                "content: checks %llu ok %llu differs %llu no feed %llu, view %llu; slots left to the stream: open tie %llu, no file texture %llu, base level %llu; joins dropped %llu, verify runs %llu, lookalike objects given up %llu",
                objs.c_str(), (unsigned long long)stats_.joined_content, (unsigned long long)stats_.joined_tie_texture, (unsigned long long)stats_.joined_tie_open, (unsigned long long)stats_.foreign, (unsigned long long)stats_.unjoined,
                (unsigned long long)stats_.bad_part, (unsigned long long)stats_.bad_vertices, (unsigned long long)stats_.bad_indices, (unsigned long long)stats_.bad_bytes, (unsigned long long)stats_.layout_refused,
                (unsigned long long)stats_.content_checks, (unsigned long long)stats_.content_ok, (unsigned long long)stats_.content_differs, (unsigned long long)stats_.content_unknown, (unsigned long long)stats_.bad_view,
                (unsigned long long)stats_.slot_kept_tie, (unsigned long long)stats_.slot_kept_no_file, (unsigned long long)stats_.slot_kept_levels, (unsigned long long)stats_.joins_dropped, (unsigned long long)stats_.verify_runs, (unsigned long long)stats_.lookalike);
  return b + notes;
}

}  // namespace rdr_scene
