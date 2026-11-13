// See arena_ours_core.h. Session 101, lane 40.
#include "arena_ours_core.h"
#include "proof_frames.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "../renderer/shaders/spv/fx_registry.h"
#include "fx_tables.h"

namespace rdr_scene {
namespace {

using assets::arena_gs::StageMesh;
using assets::arena_gs::Vertex;
using rdr_adapter::kModelPartColourSite;
using rdr_adapter::kModelPartMirrorSite;

constexpr uint16_t kRGB32F = 106, kRGBA32F = 109;  // VkFormat of the stream's position element
constexpr float kPosTolerance = 1e-3f;             // the join's content tolerance (map section 2)

// The effect table of a variant by name (the inputs the vertex program reads), cached.
const fxt::VariantTable* TableOf(int variant) {
  using rdr::fx::kVariants;
  static std::vector<const fxt::VariantTable*> by_index;
  if (by_index.empty()) {
    by_index.assign(std::size(kVariants), nullptr);
    for (size_t i = 0; i < std::size(kVariants); ++i) {
      if (!kVariants[i].name) continue;
      for (const fxt::VariantTable& t : fxt::kTables)
        if (std::strcmp(t.variant, kVariants[i].name) == 0) {
          by_index[i] = &t;
          break;
        }
    }
  }
  return variant >= 0 && size_t(variant) < by_index.size() ? by_index[size_t(variant)] : nullptr;
}
constexpr uint16_t kRG32F = 103, kRGBA8Unorm = 37, kRGBA8Uint = 41;  // (41: the stream's bone-id element, [C] vk 41 flags integer)

uint64_t Key(uint32_t a, uint32_t b) { return (uint64_t(a) << 32) | b; }

// "yBG_DefaultLM_base_vertex" -> "yBG_DefaultLM"; "yBG_Default" stays. The class is the first two '_' tokens.
std::string ClassOf(const char* name) {
  const char* p = std::strchr(name, '_');
  if (!p) return name;
  const char* q = std::strchr(p + 1, '_');
  return q ? std::string(name, size_t(q - name)) : std::string(name);
}
std::string StripLM(std::string s) {
  if (s.size() > 2 && s.compare(s.size() - 2, 2, "LM") == 0) s.resize(s.size() - 2);
  return s;
}

// The stream draw's vertex positions against a mesh's. `stride` / `offset`: element 0 of the draw's layout.
// lane 243: `first` / `count` = the window of the mesh the stream draw holds (the game drops a mesh's unused leading vertices: stream vertex i = file vertex first + i); the default is the whole mesh.
constexpr size_t kWholeMesh = ~size_t(0);
bool PositionsMatch(const std::vector<uint8_t>& bytes, uint32_t stride, uint32_t offset, const StageMesh& m, float* worst = nullptr, size_t first = 0, size_t count = kWholeMesh) {
  if (count == kWholeMesh) count = m.vertices.size() - first;
  if (stride == 0 || first + count > m.vertices.size() || bytes.size() < size_t(stride) * count) return false;
  float w = 0;
  for (size_t i = 0; i < count; ++i) {
    float p[3];
    std::memcpy(p, bytes.data() + i * stride + offset, 12);
    for (int c = 0; c < 3; ++c) {
      const float d = std::fabs(p[c] - m.vertices[first + i].pos[c]);
      if (!(d <= kPosTolerance)) {  // (also NaN)
        if (worst) *worst = std::isfinite(d) ? d : 1e30f;
        return false;
      }
      w = std::max(w, d);
    }
  }
  if (worst) *worst = w;
  return true;
}

// The stream draw's position element: stride and byte offset, or false when it is not float3 / float4.
bool PositionElement(const rdr_adapter::ReplaceIn& in, uint32_t& stride, uint32_t& offset) {
  if (!in.frame || !in.draw || in.draw->layout == 0xFFFFFFFFu || in.draw->layout >= in.frame->layouts.count) return false;
  const rdr_adapter::VertexLayout& lay = in.frame->layouts[in.draw->layout];
  if (lay.element_count == 0 || lay.element_first >= in.frame->elements.count) return false;
  const rdr_adapter::VertexElement& e = in.frame->elements[lay.element_first];
  if (e.vk_format != kRGB32F && e.vk_format != kRGBA32F) return false;
  stride = lay.host_stride;
  offset = e.host_offset;
  return stride != 0;
}

// float -> half bits, round to nearest even (the stream's converted uv is compared bit for bit).
constexpr bool kHalfRoundNearest = false;  // [C] the game's float -> half TRUNCATES (round to nearest left 5 beam-light objects ambiguous: a 3.7e-8 uv is 0000 in the stream, 0001 rounded)
uint16_t FloatToHalf(float f) {
  uint32_t x;
  std::memcpy(&x, &f, 4);
  const uint32_t sign = (x >> 16) & 0x8000u;
  const int32_t e = int32_t((x >> 23) & 0xFF) - 127 + 15;
  uint32_t m = x & 0x7FFFFFu;
  if (e >= 31) return uint16_t(sign | 0x7C00u);
  if (e <= 0) {
    if (e < -10) return uint16_t(sign);
    m |= 0x800000u;
    const int shift = 14 - e;
    uint32_t r = m >> shift;
    const uint32_t rem = m & ((1u << shift) - 1), halfway = 1u << (shift - 1);
    if (kHalfRoundNearest && (rem > halfway || (rem == halfway && (r & 1)))) ++r;
    return uint16_t(sign | r);
  }
  uint32_t r = (uint32_t(e) << 10) | (m >> 13);
  const uint32_t rem = m & 0x1FFFu;
  if (kHalfRoundNearest && (rem > 0x1000u || (rem == 0x1000u && (r & 1)))) ++r;
  return uint16_t(sign | r);
}

// Several meshes share these positions (the beam lights 272-277): the rest of the vertex tells them apart. The stream's colour element (R8G8B8A8, our byte order, measured)
// must equal ours byte for byte, and its uv (two halves) must equal ours to half precision with the u sign dropped (measured: the stream's converted u is |u|).
bool RestMatches(const rdr_adapter::ReplaceIn& in, const std::vector<uint8_t>& bytes, const StageMesh& m, size_t first = 0, size_t count = kWholeMesh) {
  if (count == kWholeMesh) count = m.vertices.size() - first;
  if (!in.frame || !in.draw || in.draw->layout >= in.frame->layouts.count) return true;
  const rdr_adapter::VertexLayout& lay = in.frame->layouts[in.draw->layout];
  if (lay.element_count != 4 || lay.element_first + 4 > in.frame->elements.count) return true;  // another layout: positions are all we can compare
  const rdr_adapter::VertexElement& ec = in.frame->elements[lay.element_first + 2];
  const rdr_adapter::VertexElement& eu = in.frame->elements[lay.element_first + 3];
  if (ec.vk_format != 37 || eu.vk_format != 83 || first + count > m.vertices.size() || bytes.size() < size_t(lay.host_stride) * count) return true;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* base = bytes.data() + i * lay.host_stride;
    if (std::memcmp(base + ec.host_offset, m.vertices[first + i].color, 4) != 0) return false;
    uint16_t h[2];
    std::memcpy(h, base + eu.host_offset, 4);
    if ((h[0] & 0x7FFFu) != FloatToHalf(std::fabs(m.vertices[first + i].uv[0])) || h[1] != FloatToHalf(m.vertices[first + i].uv[1])) {  // exact half bits of |u| and v (the stream's u has lost its sign: SDK codegen)
      return false;
    }
  }
  return true;
}

// lane 142: does the stream's uv (two halves, u without its sign) equal ours to the half bits? 1 yes, 0 no, -1 the layout is not the 4-element stage one (cannot tell).
// `shift` is the plant: our reference uv moved by that much.
int UvMatches(const rdr_adapter::ReplaceIn& in, const std::vector<uint8_t>& bytes, const StageMesh& m, float shift, size_t first = 0, size_t count = kWholeMesh) {
  if (count == kWholeMesh) count = m.vertices.size() - first;
  if (!in.frame || !in.draw || in.draw->layout >= in.frame->layouts.count) return -1;
  const rdr_adapter::VertexLayout& lay = in.frame->layouts[in.draw->layout];
  if (lay.element_count != 4 || lay.element_first + 4 > in.frame->elements.count) return -1;
  const rdr_adapter::VertexElement& eu = in.frame->elements[lay.element_first + 3];
  if (eu.vk_format != 83 || first + count > m.vertices.size() || bytes.size() < size_t(lay.host_stride) * count) return -1;
  for (size_t i = 0; i < count; ++i) {
    uint16_t h[2];
    std::memcpy(h, bytes.data() + i * lay.host_stride + eu.host_offset, 4);
    if ((h[0] & 0x7FFFu) != FloatToHalf(std::fabs(m.vertices[first + i].uv[0] + shift)) || h[1] != FloatToHalf(m.vertices[first + i].uv[1] + shift)) return 0;
  }
  return 1;
}

}  // namespace

void ArenaCore::SetArena(std::shared_ptr<const assets::arena_gs::StageArena> arena, int arena_id) {
  arena_ = std::move(arena);
  arena_id_ = arena_id;
  mesh_by_key_.clear();
  verified_key_.clear();
  pins_.clear();
  soft_blobs_.clear();
  slot_meshes_.clear();
  by_part_count_.clear();
  lead_.clear();
  span_.clear();
  stats_.trimmed_meshes = 0;
  joins_.clear();
  foreign_.clear();
  world_gap_slots_.clear();
  first_diffuse_ = -2;
  if (!arena_) return;
  lead_.assign(arena_->meshes.size(), 0);
  span_.assign(arena_->meshes.size(), 0);
  for (size_t i = 0; i < arena_->meshes.size(); ++i) {
    const StageMesh& m = arena_->meshes[i];
    mesh_by_key_[Key(uint32_t(m.slot), uint32_t(m.part))] = int(i);
    slot_meshes_[m.slot].push_back(int(i));
    by_part_count_[Key(uint32_t(m.part), uint32_t(m.vertices.size()))].push_back(int(i));
    // lane 243: the vertex range the file's indices use. The game loads a mesh without the vertices BEFORE the first one an index names (measured: arena 44 slot 111 part 2, 47 file vertices,
    // the indices use 2..46, the stream draw holds 45 = file vertices 2..46, every position equal). A mesh whose used range is shorter than its vertex array is also joined by that span.
    uint32_t lo = ~0u, hi = 0;
    for (uint32_t ix : m.indices) lo = std::min(lo, ix), hi = std::max(hi, ix);
    if (m.indices.empty() || hi >= m.vertices.size()) lo = 0, hi = uint32_t(m.vertices.size()) - 1;
    lead_[i] = lo, span_[i] = hi - lo + 1;
    if (!faults_.no_trim_window && span_[i] != m.vertices.size()) {
      by_part_count_[Key(uint32_t(m.part), span_[i])].push_back(int(i));
      ++stats_.trimmed_meshes;
    }
  }
}

int ArenaCore::ArenaOfRecord(const SceneTables& t) {
  for (const AssetRef& a : t.assets) {
    const std::string pac(t.Str(a.pac));
    const size_t p = pac.rfind("bg");
    int n = 0;
    if (p != std::string::npos && std::sscanf(pac.c_str() + p, "bg%d", &n) == 1) return n;
  }
  return -1;
}

void ArenaCore::SetRecord(std::shared_ptr<const SceneTables> record) {
  record_ = std::move(record);
  rec_slots_.clear();
  rec_twins_.clear();
  rec_world_.clear();
  if (!record_) return;
  for (const Object& o : record_->objects) {
    int k = 0;
    char tail[8] = {};
    const std::string id(record_->Str(o.id));
    const int n = std::sscanf(id.c_str(), "arena:slot:%d%7s", &k, tail);
    if (n < 1 || id.rfind("arena:slot:", 0) != 0) continue;
    if (n == 1) {
      rec_slots_.insert(k);
      std::array<float, 12> w{};
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) w[size_t(r * 4 + c)] = o.world.m[c * 4 + r];  // row-vector Mat4 -> the register layout (register r, component c)
      rec_world_[k] = w;
    } else if (tail[0] == ':' && tail[1] == 'r') {
      rec_twins_.insert(k);
    }
  }
}

bool ArenaCore::Claims(const rdr_adapter::DrawOrigin& o) const {
  if (o.klass != rdr_adapter::DrawClass::ModelPart) return false;
  if (o.pass_site != kModelPartColourSite && o.pass_site != kModelPartMirrorSite) return false;
  return foreign_.find(o.object) == foreign_.end();
}

bool ArenaCore::NeedsSkin(int vs) {
  static std::once_flag once;
  static std::vector<uint8_t> cache;   // per variant: 1 when the program reads weights / bone ids (filled once, like Feedable's answer, but static: the layout depends on the variant only)
  std::call_once(once, [] {
    cache.assign(std::size(rdr::fx::kVariants), 0);
    for (size_t i = 0; i < cache.size(); ++i) {
      bool skin = false;
      rdr::VertexLayoutDesc d;
      cache[i] = BuildLayout(int(i), d, true, &skin) && skin;
    }
  });
  return vs >= 0 && size_t(vs) < cache.size() && cache[size_t(vs)];
}

bool ArenaCore::BuildLayout(int vs, rdr::VertexLayoutDesc& desc, bool wide, bool* needs_skin) {
  desc = rdr::VertexLayoutDesc{};
  if (needs_skin) *needs_skin = false;
  const fxt::VariantTable* t = TableOf(vs);
  if (!t || t->input_count == 0) return false;
  desc.strides[0] = uint32_t(wide ? sizeof(assets::arena_gs::SkinVertex) : sizeof(Vertex));
  for (uint32_t k = 0; k < t->input_count; ++k) {
    if (desc.attribute_count >= rdr::kMaxVertexAttributes) return false;
    const std::string name = t->inputs[k].name;
    rdr::VertexAttribute a{};
    a.location = t->inputs[k].location;
    if (name == "inPosition" && t->inputs[k].components >= 3) a.format = kRGB32F, a.offset = uint32_t(offsetof(Vertex, pos));
    else if (name == "inNormal" && t->inputs[k].components == 3) a.format = kRGB32F, a.offset = uint32_t(offsetof(Vertex, normal));
    else if (name == "inColor" && t->inputs[k].components == 4) a.format = kRGBA8Unorm, a.offset = uint32_t(offsetof(Vertex, color));
    else if (name == "inUv" && t->inputs[k].components == 2) a.format = kRG32F, a.offset = uint32_t(offsetof(Vertex, uv));
    else if (name == "inWeights" && t->inputs[k].components == 4) {
      if (needs_skin) *needs_skin = true;
      if (!wide) return false;
      a.format = kRGBA8Unorm, a.offset = uint32_t(offsetof(assets::arena_gs::SkinVertex, weights));
    } else if (name == "inBoneIds" && t->inputs[k].components == 4) {
      if (needs_skin) *needs_skin = true;
      if (!wide) return false;
      a.format = kRGBA8Uint, a.offset = uint32_t(offsetof(assets::arena_gs::SkinVertex, ids));
    } else return false;
    desc.attributes[desc.attribute_count++] = a;
  }
  return true;
}

bool ArenaCore::Feedable(int vs) {
  const auto it = feedable_.find(vs);
  if (it != feedable_.end()) return it->second;
  rdr::VertexLayoutDesc d;
  return feedable_.emplace(vs, BuildLayout(vs, d, true)).first->second;  // (feedable by SOME mesh: a skin-input variant needs a skinned mesh, checked once the mesh is known)
}

bool ArenaCore::Interchangeable(const StageMesh& a, const StageMesh& b) const {
  if (a.vertices.size() != b.vertices.size() || a.indices != b.indices || a.effect != b.effect || a.material_hash != b.material_hash) return why_not_ = "indices or effect", false;
  if (a.skinned != b.skinned || a.skin_ids != b.skin_ids || a.skin_weights != b.skin_weights) return why_not_ = "skin words", false;   // S101 lane 49 (review D1): the bone-id words are part of the vertex
  if (std::memcmp(a.vertices.data(), b.vertices.data(), a.vertices.size() * sizeof(Vertex)) != 0) return why_not_ = "vertex bytes", false;
  const auto same_tex = [&](int x, int y) {
    if (x == y) return true;
    if (x < 0 || y < 0) return false;
    const assets::Texture &tx = arena_->textures[size_t(x)], &ty = arena_->textures[size_t(y)];
    return tx.width == ty.width && tx.height == ty.height && tx.format == ty.format && tx.data == ty.data;
  };
  if (!same_tex(a.diffuse, b.diffuse)) return why_not_ = "diffuse", false;
  if (!same_tex(a.normal_map, b.normal_map)) return why_not_ = "normal map", false;
  if (!same_tex(a.specular, b.specular)) return why_not_ = "specular", false;
  return true;
}

bool ArenaCore::SlotsInterchangeable(int slot_a, int slot_b) const {
  const auto a = slot_meshes_.find(slot_a), b = slot_meshes_.find(slot_b);
  if (a == slot_meshes_.end() || b == slot_meshes_.end() || a->second.size() != b->second.size()) return why_not_ = "mesh count of the slots", false;
  for (int ma : a->second) {
    const int mb = FindMesh(slot_b, arena_->meshes[size_t(ma)].part);
    if (mb < 0 || !Interchangeable(arena_->meshes[size_t(ma)], arena_->meshes[size_t(mb)])) return false;
  }
  return true;
}

int ArenaCore::FindMesh(int slot, int part) const {
  const auto it = mesh_by_key_.find(Key(uint32_t(slot), uint32_t(part)));
  return it == mesh_by_key_.end() ? -1 : it->second;
}

bool ArenaCore::ClassMatches(const std::string& effect, const char* variant_name) {
  if (!variant_name || !*variant_name) return false;
  const std::string a = StripLM(effect), b = StripLM(ClassOf(variant_name));
  if (a.empty() || b.empty()) return false;
  return a == b || a.compare(0, b.size(), b) == 0 || b.compare(0, a.size(), a) == 0;  // (the file's "yProp_UVScrolLM" vs the effect's spelling)
}

// lane 243: where in the file mesh does a stream draw with `stream_vertices` vertices start? The whole mesh (first 0), or, for a mesh whose indices use only vertices lead..lead + span - 1,
// that range (the game drops the unused leading vertices: measured on arena 44 slot 111 part 2). The positions are then compared with the window, so a wrong guess is a refusal.
bool ArenaCore::WindowOf(int mesh, uint32_t stream_vertices, uint32_t& first) const {
  const size_t i = size_t(mesh);
  if (stream_vertices == arena_->meshes[i].vertices.size()) return first = 0, true;
  if (faults_.no_trim_window || i >= span_.size() || stream_vertices != span_[i]) return false;
  return first = lead_[i] + (faults_.window_shift ? 1u : 0u), true;   // (plant window_shift: the window starts one vertex late: the position compare must refuse it)
}

ArenaCore::Decision ArenaCore::Decide(const rdr_adapter::ReplaceIn& in, const char* vs_name, const char* ps_name, const uint8_t* ps_slots, uint32_t ps_slot_count,
                                      const float* world) {
  (void)ps_name;
  Decision d;
  ++stats_.decisions;
  const auto keep = [&](KeepWhy why, const char* reason) -> Decision {
    d.ok = false, d.why = why, d.reason = reason;
    ++stats_.kept[uint8_t(why)];
    return d;
  };
  if (!arena_) return keep(KeepWhy::NotReady, "no arena");
  const rdr_adapter::DrawOrigin* o = in.origin;
  if (!o || o->klass != rdr_adapter::DrawClass::ModelPart) return keep(KeepWhy::NotOurs, "class");
  if (in.draw && (in.draw->flags & rdr_adapter::kDrawNoGeometry)) return keep(KeepWhy::NotOurs, "no geometry");  // (lane 39: their vertex bytes are garbage)
  const bool mirror = o->pass_site == kModelPartMirrorSite;
  if (!mirror && o->pass_site != kModelPartColourSite) return keep(KeepWhy::NotOurs, "site");
  if (mirror && faults_.no_mirror) return keep(KeepWhy::NotOurs, "fault no_mirror");
  if (foreign_.count(o->object)) return keep(KeepWhy::NotOurs, "foreign object");
  const int part = int(o->part);
  const bool group_rule = !faults_.no_group_rule;
  // A blob (object, part, key) whose positions were wrong (a prop with the same counts, OR one bad live read) is asked again after a while; ONLY that blob's draws are refused meanwhile (a
  // new key, or the object's other parts, are judged on their own: one bad blob must not hide the object, and it costs no re-read every frame).
  const uint64_t soft_id = ((uint64_t(o->object) << 32 | uint32_t(part)) * 0x9E3779B97F4A7C15ull) ^ in.vertex_offset;
  if (const auto sit = soft_blobs_.find(soft_id); sit != soft_blobs_.end()) {
    if (frame_no_ < sit->second) return keep(KeepWhy::NotOurs, "positions differ (retry later)");
    soft_blobs_.erase(sit);
  }

  // ---- the join, once per object ----
  auto it = joins_.find(o->object);
  if (it == joins_.end()) {
    Join j;
    const char* asset = in.frame ? in.frame->String(o->asset) : "";
    int declared_arena = -1, declared_slot = -1;
    if (asset && *asset && std::sscanf(asset, "%*[^:]:%d", &declared_slot) == 1) {
      const char* bg = std::strstr(asset, "bg");
      if (!bg || std::sscanf(bg, "bg%d", &declared_arena) != 1) declared_arena = -1;
    }
    if (declared_slot >= 0) {  // 1. declared
      if (declared_arena >= 0 && declared_arena != arena_id_) {
        j.foreign = true;  // another arena's pac: a prop from elsewhere
        ++stats_.foreign;
      } else if (declared_slot >= 400) {
        j.foreign = true;
        ++stats_.foreign;
      } else if (FindMesh(declared_slot, part) < 0) {
        j.slot = -2;
        ++stats_.unjoined;
      } else {
        j.slot = declared_slot, j.declared = true;
        ++stats_.joined_declared;
      }
    } else {  // 2. by content
      const auto cit = by_part_count_.find(Key(uint32_t(part), in.vertex_count));
      std::vector<int> cands;
      if (cit != by_part_count_.end())
        for (int mi : cit->second)
          if (arena_->meshes[size_t(mi)].StreamIndexCount(group_rule) == in.index_count) cands.push_back(mi);
      if (cands.empty()) {
        j.foreign = true;  // no stage mesh has these counts: ring piece, wrestler, prop of another pac
        ++stats_.foreign;
      } else if (faults_.join_shift) {  // the planted counts-only join, one candidate along
        j.slot = arena_->meshes[size_t(cands[1 % cands.size()])].slot;
        ++stats_.joined_content;
      } else {
        uint32_t stride = 0, offset = 0;
        std::vector<uint8_t> bytes;
        if (!PositionElement(in, stride, offset) || !in.read_geometry || !in.read_geometry(in.vertex_offset, in.vertex_count * stride, bytes)) {
          return keep(KeepWhy::Unjoined, "geometry unreadable");  // (not stored: the next draw tries again)
        }
        std::vector<int> hits;
        for (int mi : cands) {
          uint32_t first = 0;
          if (WindowOf(mi, in.vertex_count, first) && PositionsMatch(bytes, stride, offset, arena_->meshes[size_t(mi)], nullptr, first, in.vertex_count)) hits.push_back(mi);
        }
        if (hits.size() > 1) {  // positions alone do not name one: the colour and uv of the stream do
          std::vector<int> narrowed;
          for (int mi : hits) {
            uint32_t first = 0;
            WindowOf(mi, in.vertex_count, first);
            if (RestMatches(in, bytes, arena_->meshes[size_t(mi)], first, in.vertex_count)) narrowed.push_back(mi);
          }
          if (!narrowed.empty()) hits = narrowed;
        }
        if (hits.empty()) {
          soft_blobs_[soft_id] = frame_no_ + 300;  // counts fit, the positions do not: a prop whose mesh is not in this arena's stage, or a bad read: soft, this blob only
          ++stats_.foreign;
          return keep(KeepWhy::NotOurs, "positions differ");
        } else {
          const int slot = arena_->meshes[size_t(hits[0])].slot;
          bool same_slot = true, identical = true;
          for (int mi2 : hits) {
            same_slot = same_slot && arena_->meshes[size_t(mi2)].slot == slot;
            identical = identical && (arena_->meshes[size_t(mi2)].slot == slot || SlotsInterchangeable(slot, arena_->meshes[size_t(mi2)].slot));
          }
          if (!same_slot && !identical) {
            j.slot = -2;  // several different meshes hold these positions: never joined, counted
            ++stats_.ambiguous;
            ambiguous_note_ += std::string(ambiguous_note_.empty() ? "" : "; ") + "slots " + std::to_string(arena_->meshes[size_t(hits[0])].slot) + "/" + std::to_string(arena_->meshes[size_t(hits.back())].slot) + " differ in " + why_not_;
          } else {
            j.slot = slot;
            j.interchangeable = !same_slot;
            ++stats_.joined_content;
            stats_.interchangeable += !same_slot;
          }
        }
      }
    }
    if (j.foreign) foreign_.insert(o->object);
    it = joins_.emplace(o->object, j).first;
    d.by_content = !j.declared && j.slot >= 0;
    d.declared = j.declared;
    d.interchangeable = j.interchangeable;
  }
  if (it->second.foreign) return keep(KeepWhy::NotOurs, "no stage mesh");
  if (it->second.slot < 0) return keep(KeepWhy::Unjoined, "ambiguous or unnamed");
  d.slot = it->second.slot;
  d.mirrored = mirror;
  d.interchangeable = it->second.interchangeable;
  if (!Feedable(in.vs)) return keep(KeepWhy::Variant, "vertex inputs");

  // ---- the checks, every draw ----
  const uint64_t verify_id = (uint64_t(o->object) << 32) | (uint64_t(part & 0x3FFFFFFF) << 1) | (mirror ? 1u : 0u);  // per copy: the mirrored draw may carry another key
  const auto drop_join = [&](uint64_t& counter, const char* why) {
    ++counter;
    joins_.erase(o->object);
    dropped_objects_.push_back(o->object), ++drop_seq_;  // lane 122
    for (auto vit = verified_key_.begin(); vit != verified_key_.end();) vit = (vit->first >> 32) == o->object ? verified_key_.erase(vit) : std::next(vit);  // every part of the object: the join they were verified against is gone
    for (auto pit2 = pins_.begin(); pit2 != pins_.end();) pit2 = (pit2->first >> 32) == o->object ? pins_.erase(pit2) : std::next(pit2);  // S101 lane 88: their pins too
    ++stats_.joins_dropped;
    {
      char b[200];
      std::snprintf(b, sizeof b, "%s (object 0x%08X part %u site 0x%08X, stream %u vertices %u indices, slot %d)", why, o->object, o->part, o->pass_site, in.vertex_count, in.index_count, d.slot);
      std::string text = b;
      // lane 243: what the joined slot holds (part: vertices / draw indices), so one log line says which kind of mismatch it is (another mesh of the slot, or a lookalike object).
      const auto sm = slot_meshes_.find(d.slot);
      if (sm != slot_meshes_.end()) {
        text += "; slot parts";
        size_t shown = 0;
        for (int mj : sm->second) {
          if (++shown > 10) { text += " ..."; break; }
          const StageMesh& q = arena_->meshes[size_t(mj)];
          char c[48];
          std::snprintf(c, sizeof c, " %d:%zu/%u", q.part, q.vertices.size(), q.StreamIndexCount(group_rule));
          text += c;
        }
      }
      if (first_fail_.empty()) first_fail_ = text;
    }
    return keep(KeepWhy::Mismatch, why);
  };
  const int mi = FindMesh(d.slot, part);
  if (mi < 0) return drop_join(stats_.bad_part, "part");
  const StageMesh& m = arena_->meshes[size_t(mi)];
  uint32_t win = 0;   // lane 243: the first file vertex the stream draw holds (0 = the whole mesh)
  if (!WindowOf(mi, in.vertex_count, win)) return drop_join(stats_.bad_vertices, "vertices");
  if (win > 0 || in.vertex_count != m.vertices.size()) ++stats_.window_draws;
  if (in.index_count != m.StreamIndexCount(group_rule)) return drop_join(stats_.bad_indices, "indices");
  // S101 lane 88: pinned = this key passed its full check with this very variant, slot, counts and vertex key. Pinned and not on its proof frame: only the identity checks above run.
  const auto pin_it = pins_.find(verify_id);
  const bool pinned = kArenaPins && !verify_content_ && pin_it != pins_.end() && pin_it->second.offset == in.vertex_offset && pin_it->second.vs == in.vs && pin_it->second.slot == d.slot &&
                      pin_it->second.vcount == in.vertex_count && pin_it->second.icount == in.index_count;
  const bool proof = pinned && !faults_.no_proof_frames && ProofDue(verify_id, frame_no_, kArenaProofEvery);
  if (pinned) ++(proof ? stats_.proof_draws : stats_.pinned_draws);
  if ((!pinned || proof) && !ClassMatches(m.effect, vs_name)) return drop_join(stats_.bad_class, "class");
  // (a pinned key whose proof frame is not now skips the whole first / last re-read: the pin holds the vertex key it passed with)
  const auto vk_it = pinned ? verified_key_.end() : verified_key_.find(verify_id);
  if (!verify_content_ && in.read_geometry && (proof || (!pinned && (vk_it == verified_key_.end() || vk_it->second != in.vertex_offset)))) {
    // Address reuse after an arena change: the first and last vertex still have to be this mesh's. The draw's whole vertex blob is read by its key / offset as given (live: the store's
    // KEY, not an address, so no arithmetic on it; Opus review + run 7067: key + (n-1) * stride landed on another blob and failed every frame), once per (object, part) a frame.
    uint32_t stride = 0, offset = 0;
    std::vector<uint8_t> bytes;
    if (PositionElement(in, stride, offset) && in.vertex_count > 0 && in.read_geometry(in.vertex_offset, in.vertex_count * stride, bytes) && bytes.size() >= size_t(in.vertex_count) * stride) {
      float p0[3], p1[3];
      std::memcpy(p0, bytes.data() + offset, 12);
      std::memcpy(p1, bytes.data() + size_t(in.vertex_count - 1) * stride + offset, 12);
      const Vertex &f = m.vertices[win], &l = m.vertices[win + in.vertex_count - 1];
      bool ok = true;
      for (int c = 0; c < 3; ++c) ok = ok && std::fabs(p0[c] - f.pos[c]) <= kPosTolerance && std::fabs(p1[c] - l.pos[c]) <= kPosTolerance;
      if (!ok) {
        soft_blobs_[soft_id] = frame_no_ + 300;  // this blob is wrong: not read again every frame
        return drop_join(stats_.bad_content, "first/last vertex");
      }
      verified_key_[verify_id] = in.vertex_offset;  // only on a pass
    }
  }
  if (verify_content_) {
    uint32_t stride = 0, offset = 0;
    std::vector<uint8_t> bytes;
    if (!PositionElement(in, stride, offset) || !in.read_geometry || !in.read_geometry(in.vertex_offset, in.vertex_count * stride, bytes))
      return drop_join(stats_.bad_content, "content");
    if (!PositionsMatch(bytes, stride, offset, m, nullptr, win, in.vertex_count)) {
      // S101 thin vswap: the blob that disagreed with the join is marked soft, as the live first / last failure above does (:446). Without it this blob alone could re-join the object on the
      // next frame (to another slot whose mesh it holds) whenever it reaches the hook before a sibling: the offline hook's verdict then depended on which draws reach it (thin draws).
      if (!faults_.content_no_soft) soft_blobs_[soft_id] = frame_no_ + 300;
      return drop_join(stats_.bad_content, "content");
    }
  }
  d.mesh = mi;
  Pin* pn = nullptr;
  if (kArenaPins && !verify_content_) {
    if (pinned) {
      pn = &pin_it->second;
      if (proof) std::fill(pn->img_state, pn->img_state + 3, uint8_t(0));   // the picture sizes are asked again too
    } else {
      pn = &pins_[verify_id];
      *pn = Pin{in.vertex_offset, in.vs, d.slot, in.vertex_count, in.index_count, {0, 0, 0}, {0, 0, 0}};
    }
  }
  if (NeedsSkin(in.vs) && !m.skinned) return keep(KeepWhy::Variant, "no skin words in the mesh");  // S101 lane 49: a bone-id program needs the file's skin pairs (one per vertex)

  // ---- the textures this draw's pixel program reads and the file holds ----
  bool wants[3] = {false, false, false};  // 0, 4, 5
  bool wants_lightmap = false;
  for (uint32_t s = 0; s < ps_slot_count; ++s) {
    if (ps_slots[s] == 0) wants[0] = true;
    else if (ps_slots[s] == 4) wants[1] = true;
    else if (ps_slots[s] == 5) wants[2] = true;
    else if (ps_slots[s] == 13) wants_lightmap = true;
  }
  // lane 142: a picture the file lacks, an image the stream does not bind, a view that is not plain, a size that differs: the PICTURE of that slot is the game's (a dynamic or shared picture,
  // msg_score2, a screen); the draw's geometry is proven on its own, so the draw is still ours and the slot keeps the stream's image (the same path as "size unchecked"). `faults_.strict_texture`
  // is the old rule (the whole draw stays the stream's).
  const bool strict_tex = faults_.strict_texture;
  if (strict_tex) {
    if (wants[0] && m.diffuse < 0) return keep(KeepWhy::Texture, "no diffuse");
    if (wants[1] && m.normal_map < 0) return keep(KeepWhy::Texture, "no normal map");
    if (wants[2] && m.specular < 0) return keep(KeepWhy::Texture, "no specular map");
  }
  // The stream's own slot must be plain: an image bound (the sampler is its), no view reinterpretation (SNORM normal maps...), identity swizzle, and (when the owner can
  // tell) the same size as the file picture. Anything else is the game's texture, not the file's: the draw stays the stream's, counted.
  const int roles[3] = {m.diffuse, m.normal_map, m.specular};
  const uint8_t role_slot[3] = {0, 4, 5};
  bool unchecked[3] = {false, false, false};  // no image info: the stream's image stays in that slot (geometry and uv are still ours)
  bool new_path = false;                      // lane 142: a slot took one of the "the picture stays the stream's" paths (its uv is then proven below)
  for (int r = 0; r < 3; ++r) {
    if (!wants[r]) continue;
    if (roles[r] < 0) {   // lane 142: the file has no such picture: the stream's stays
      unchecked[r] = true;
      new_path = true;
      ++stats_.tex_stream_no_file;
      continue;
    }
    const rdr_adapter::TextureSlot* ts = nullptr;
    if (in.frame && in.draw)
      for (uint32_t t = 0; t < in.draw->texture_count && in.draw->texture_first + t < in.frame->texture_slots.count; ++t)
        if (in.frame->texture_slots[in.draw->texture_first + t].slot == role_slot[r]) ts = &in.frame->texture_slots[in.draw->texture_first + t];
    if (!ts || ts->image == 0) {
      if (strict_tex) return keep(KeepWhy::Texture, "stream binds no image");
      unchecked[r] = true;   // lane 142: nothing is bound there: the slot stays as the stream has it
      new_path = true;
      ++stats_.tex_stream_no_image;
      continue;
    }
    // VkComponentSwizzle: 0 identity, or the explicit R G B A (3 4 5 6) the reader writes for a plain RGBA fetch [C: every stage diffuse of Check B]
    const bool plain = (!ts->swizzle[0] && !ts->swizzle[1] && !ts->swizzle[2] && !ts->swizzle[3]) || (ts->swizzle[0] == 3 && ts->swizzle[1] == 4 && ts->swizzle[2] == 5 && ts->swizzle[3] == 6);
    if (ts->view_format != 0 || !plain) {
      if (strict_tex) {
        ++stats_.bad_view;
        return keep(KeepWhy::Texture, "view format / swizzle");
      }
      unchecked[r] = true;   // lane 142: the game reads this picture through its own view; the slot (image and view) stays the stream's
      new_path = true;
      ++stats_.tex_stream_view;
      continue;
    }
    const assets::Texture& t = arena_->textures[size_t(roles[r])];
    uint32_t sw = 0, sh = 0;
    if (pn && pn->img_state[r] && pn->img[r] == ts->image) {   // S101 lane 88: this image id passed (or was untellable) on the key's last full check
      unchecked[r] = pn->img_state[r] >= 2;
      if (pn->img_state[r] == 2) ++stats_.size_unchecked;
      else if (pn->img_state[r] == 3) ++stats_.tex_stream_size, new_path = true;   // lane 142
    } else if (image_info_ && image_info_(ts->image, sw, sh) && sw && sh) {
      // A different size is a different picture (lane 39: the file's 252x64 msg_score2 against the game's 256x64 image is NOT the file's picture padded: 0 of 16 block rows
      // match at either pitch, the padding is non-zero; cause unknown). Those draws stay the stream's, counted.
      if (t.width != sw || t.height != sh) {
        if (strict_tex) {
          ++stats_.bad_size;
          return keep(KeepWhy::Texture, "image size");
        }
        new_path = true;
        unchecked[r] = true;   // lane 142: another picture than the file's (msg_score2: 252x64 file, 256x64 game): the stream's image stays, the draw is ours
        ++stats_.tex_stream_size;
        if (pn) pn->img[r] = ts->image, pn->img_state[r] = 3;
        continue;
      }
      if (pn) pn->img[r] = ts->image, pn->img_state[r] = 1;
    } else {
      // The owner cannot tell the size (live today): the picture is NOT assumed to be the file's (msg_score2: 252x64 in the file, another 256x64 picture in the game). The slot is
      // left unset, so the stream's image stays; counted.
      ++stats_.size_unchecked;
      unchecked[r] = true;
      if (pn) pn->img[r] = ts->image, pn->img_state[r] = 2;
    }
  }
  // lane 142: the game's picture drawn with OUR uv. Where the file has a diffuse the picture bytes were proven equal elsewhere; here the picture is the stream's, so the uv (the loader's
  // set-1 / set-2 guess) is compared with the stream's on the full check (first sight of the key, a proof frame). A layout it cannot compare, or a difference: the draw stays the stream's.
  if (new_path && (!pinned || proof)) {
    uint32_t ustride = 0, uoffset = 0;
    std::vector<uint8_t> ubytes;
    int ok = -1;
    if (PositionElement(in, ustride, uoffset) && in.read_geometry && in.read_geometry(in.vertex_offset, in.vertex_count * ustride, ubytes) && ubytes.size() >= size_t(in.vertex_count) * ustride)
      ok = UvMatches(in, ubytes, m, faults_.uv_shift ? 0.0625f : 0.0f, win, in.vertex_count);
    if (ok != 1) {
      ++stats_.tex_uv_differs;
      pins_.erase(verify_id);   // (a pin made above must not let the next frame skip this check)
      return keep(KeepWhy::Texture, ok == 0 ? "uv differs" : "uv unchecked");
    }
  }
  if (wants[0] && !unchecked[0]) {
    d.diffuse = m.diffuse;
    if (faults_.first_texture) {
      if (first_diffuse_ == -2) first_diffuse_ = m.diffuse;
      d.diffuse = first_diffuse_;
    }
  }
  if (wants[1] && !unchecked[1]) d.normal_map = m.normal_map;
  if (wants[2] && !unchecked[2]) d.specular = m.specular;
  if (faults_.file_lightmap && wants_lightmap && arena_->stage_lightmap >= 0) d.file_lightmap = arena_->stage_lightmap;

  // ---- the record, compared and counted (not a fallback in P2a: the stream is the authority for the geometry) ----
  if (record_) {
    if (!rec_slots_.count(d.slot) && !d.interchangeable) ++stats_.gap_slot;
    if (mirror && !rec_twins_.count(d.slot)) ++stats_.gap_twin;
    if (!mirror && world && !d.interchangeable) {
      const auto w = rec_world_.find(d.slot);
      if (w != rec_world_.end()) {
        float worst = 0;
        for (int k = 0; k < 12; ++k) worst = std::max(worst, std::fabs(world[k] - w->second[size_t(k)]));
        if (worst > 1e-3f) {
          ++stats_.gap_world;
          world_gap_slots_.insert(d.slot);
        }
      }
    }
  }
  d.ok = true;
  d.why = KeepWhy::Count;
  d.reason = "";
  ++stats_.replaced;
  return d;
}

void ArenaCore::FillVertices(const StageMesh& mesh, std::vector<Vertex>& out) const {
  out = mesh.vertices;
  if (faults_.vertex_swizzle)
    for (Vertex& v : out) std::swap(v.color[0], v.color[2]);
  if (abs_u_)
    for (Vertex& v : out) v.uv[0] = std::fabs(v.uv[0]);
}

bool ArenaCore::FillSkinVertices(const StageMesh& mesh, std::vector<assets::arena_gs::SkinVertex>& out) const {
  if (!mesh.skinned || mesh.skin_ids.size() != 4 * mesh.vertices.size() || mesh.skin_weights.size() != 4 * mesh.vertices.size()) return false;
  std::vector<Vertex> base;
  FillVertices(mesh, base);
  out.resize(base.size());
  for (size_t i = 0; i < base.size(); ++i) {
    out[i].v = base[i];
    std::memcpy(out[i].weights, &mesh.skin_weights[4 * i], 4);
    std::memcpy(out[i].ids, &mesh.skin_ids[4 * i], 4);
    if (faults_.bone_off_by_one) out[i].ids[0] = uint8_t(out[i].ids[0] + 1);
    if (faults_.bone_zero) out[i].ids[0] = 0;
  }
  return true;
}

void ArenaCore::FillIndices(const StageMesh& mesh, std::vector<uint32_t>& out) const {
  out = mesh.indices;
  if (faults_.winding_flip)
    for (size_t i = 0; i + 2 < out.size(); i += 3) std::swap(out[i + 1], out[i + 2]);
}

std::string ArenaCore::Report() const {
  char buf[1100];   // (lane 142: longer line)
  std::snprintf(buf, sizeof buf,
                "objects joined %llu (by content %llu, declared %llu), unjoined %llu, ambiguous %llu, not stage %llu (counted again on every soft retry); draw checks failed: part %llu, vertices %llu, indices %llu, class %llu, "
                "content %llu, size %llu (size unchecked %llu); record gaps: slot missing %llu, twin missing %llu, world differs %llu (%zu slots); joins dropped %llu; picture stays the stream's (draw ours): no file %llu, no image %llu, view %llu, size %llu, put %llu, kept for uv %llu",
                (unsigned long long)(stats_.joined_content + stats_.joined_declared), (unsigned long long)stats_.joined_content, (unsigned long long)stats_.joined_declared,
                (unsigned long long)stats_.unjoined, (unsigned long long)stats_.ambiguous, (unsigned long long)stats_.foreign, (unsigned long long)stats_.bad_part,
                (unsigned long long)stats_.bad_vertices, (unsigned long long)stats_.bad_indices, (unsigned long long)stats_.bad_class, (unsigned long long)stats_.bad_content,
                (unsigned long long)stats_.bad_size, (unsigned long long)stats_.size_unchecked, (unsigned long long)stats_.gap_slot, (unsigned long long)stats_.gap_twin,
                (unsigned long long)stats_.gap_world, world_gap_slots_.size(), (unsigned long long)stats_.joins_dropped,
                (unsigned long long)stats_.tex_stream_no_file, (unsigned long long)stats_.tex_stream_no_image, (unsigned long long)stats_.tex_stream_view, (unsigned long long)stats_.tex_stream_size, (unsigned long long)stats_.tex_stream_put, (unsigned long long)stats_.tex_uv_differs);
  char extra[160];
  std::snprintf(extra, sizeof extra, "; trimmed meshes %llu, draws of a trimmed window %llu", (unsigned long long)stats_.trimmed_meshes, (unsigned long long)stats_.window_draws);   // lane 243
  return std::string(buf) + extra + (ambiguous_note_.empty() ? "" : "; ambiguous: " + ambiguous_note_);
}

}  // namespace rdr_scene
