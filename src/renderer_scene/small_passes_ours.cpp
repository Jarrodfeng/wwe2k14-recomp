// See small_passes_ours.h. Session 101, lane 135.
#include "small_passes_ours.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../renderer/problem.h"
#include "../renderer/shaders/spv/fx_registry.h"

namespace rdr_scene {
namespace {

using rdr_adapter::KeepWhy;
namespace sp = small_passes;

constexpr uint16_t kFloat2 = 103, kFloat3 = 106, kFloat4 = 109;   // VkFormat numbers of the stream's vertex elements
constexpr uint8_t kTopologyStrip = 4;                              // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
constexpr float kVertexTolerance = 2e-5f;                          // fitted max difference is 8e-7
constexpr uint16_t kScreenScaleRegister = 201;                     // yLiquidMap / yPreLight vertex ScreenScale (the effect descriptions)

// The stream draw's vertex elements as (vk format, host offset), in fetch order; false when the layout is unreadable.
bool Elements(const rdr_adapter::ReplaceIn& in, uint32_t& stride, std::vector<std::pair<uint16_t, uint32_t>>& out) {
  out.clear();
  if (!in.frame || !in.draw || in.draw->layout == 0xFFFFFFFFu || in.draw->layout >= in.frame->layouts.count) return false;
  const rdr_adapter::VertexLayout& lay = in.frame->layouts[in.draw->layout];
  stride = lay.host_stride;
  for (uint32_t k = 0; k < lay.element_count && lay.element_first + k < in.frame->elements.count; ++k) {
    const rdr_adapter::VertexElement& e = in.frame->elements[lay.element_first + k];
    out.emplace_back(uint16_t(e.vk_format), uint32_t(e.host_offset));
  }
  return true;
}

// The packed vertex block's float4 at a constant register, via the variant's range table (the same map ApplyRowOverride uses); false when the register is not in the block.
bool ReadRegister(const rdr_adapter::ReplaceIn& in, uint16_t reg, float out[4]) {
  if (in.vs < 0 || size_t(in.vs) >= std::size(rdr::fx::kVariants) || !in.vs_block) return false;
  const rdr::fx::Variant& v = rdr::fx::kVariants[in.vs];
  for (uint32_t r = 0; r < v.constant_count; ++r) {
    const rdr::fx::ConstantRange& c = v.constants[r];
    if (reg < c.reg || reg >= c.reg + c.count) continue;
    const uint32_t at = uint32_t(c.offset) + uint32_t(reg - c.reg) * 16u;
    if (at + 16u > in.vs_block_bytes) return false;
    std::memcpy(out, in.vs_block + at, 16);
    return true;
  }
  return false;
}

bool StartsWith(const char* s, const char* prefix) { return s && std::strncmp(s, prefix, std::strlen(prefix)) == 0; }

}  // namespace

bool SmallFaults::Parse(const std::string& list, SmallFaults& f, std::string& error) {
  size_t at = 0;
  while (at <= list.size()) {
    size_t comma = list.find(',', at);
    if (comma == std::string::npos) comma = list.size();
    const std::string w = list.substr(at, comma - at);
    at = comma + 1;
    if (w.empty()) continue;
    if (w == "sweat_no_prev") f.sweat_no_prev = true;
    else if (w == "slot_null") f.slot_null = true;
    else if (w == "prelight_wrong_target_size") f.prelight_wrong_target_size = true;
    else if (w == "no_verify") f.no_verify = true;
    else if (w == "disc_last_vertex") f.disc_last_vertex = true;
    else if (w == "cube_last_vertex") f.cube_last_vertex = true;
    else if (w == "cube_index_order") f.cube_index_order = true;
    else if (w == "disc_no_restart") f.disc_no_restart = true;
    else if (w == "sweat_one_frame") f.sweat_one_frame = true;
    else {
      error = "unknown small passes fault '" + w + "' (sweat_no_prev, sweat_one_frame, slot_null, prelight_wrong_target_size, disc_last_vertex, cube_last_vertex, cube_index_order, disc_no_restart, no_verify)";
      return false;
    }
  }
  return true;
}

SmallPassesOurs::SmallPassesOurs(rdr::Renderer& renderer, const SmallFaults& faults, bool live) : renderer_(renderer), faults_(faults), live_(live) { ready_ = Upload(); }

SmallPassesOurs::~SmallPassesOurs() {
  renderer_.WaitIdle();
  Drop();
}

void SmallPassesOurs::Drop() {
  for (rdr::BufferId* b : {&disc_vertex_, &disc_index_, &cube_vertex_, &cube_index_})
    if (*b != rdr::kNoBuffer) renderer_.DestroyBuffer(*b), *b = rdr::kNoBuffer;
  if (zero_image_ != rdr::kNoImage) renderer_.DestroyImage(zero_image_), zero_image_ = rdr::kNoImage;
  ready_ = false;
}

bool SmallPassesOurs::Upload() {
  sp::BuildDisc(disc_);
  sp::BuildDiscIndices(disc_idx_);
  sp::BuildCube(cube_, faults_.prelight_wrong_target_size ? 8.0f : 16.0f);
  sp::BuildCubeIndices(cube_idx_);
  // planted faults in OUR meshes: the per-draw compare is stream bytes against these arrays, so each must be refused
  if (faults_.disc_last_vertex) disc_.back().fade[3] += 1e-4f;
  if (faults_.cube_last_vertex) cube_.back().dir[0] += 1e-4f;
  if (faults_.cube_index_order)
    for (size_t i = 0; i + 1 < cube_idx_.size(); i += 2)
      if (cube_idx_[i] != sp::kRestart && cube_idx_[i + 1] != sp::kRestart) std::swap(cube_idx_[i], cube_idx_[i + 1]);
  if (faults_.disc_no_restart)
    for (size_t i = 1; i < disc_idx_.size(); ++i)
      if (disc_idx_[i] == sp::kRestart) disc_idx_[i] = disc_idx_[i - 1];
  disc_vertex_ = renderer_.CreateBuffer(disc_.size() * sizeof(sp::DiscVertex), disc_.data());
  disc_index_ = renderer_.CreateBuffer(disc_idx_.size() * 2, disc_idx_.data());
  cube_vertex_ = renderer_.CreateBuffer(cube_.size() * sizeof(sp::CubeVertex), cube_.data());
  cube_index_ = renderer_.CreateBuffer(cube_idx_.size() * 2, cube_idx_.data());
  if (faults_.sweat_no_prev || faults_.sweat_one_frame) {   // a cleared texel the sampler clamps: "the previous map is empty"
    rdr::ImageDesc d;
    d.width = 1, d.height = 1, d.mips = 1;
    d.format = rdr::Format::RGBA8;
    d.usage = rdr::kUsageSampled;
    zero_image_ = renderer_.CreateImage(d);
    const uint8_t zero[4] = {0, 0, 0, 0};
    if (zero_image_ != rdr::kNoImage) renderer_.UpdateImage(zero_image_, 0, 0, zero, sizeof zero);
  }
  const bool ok = disc_vertex_ != rdr::kNoBuffer && disc_index_ != rdr::kNoBuffer && cube_vertex_ != rdr::kNoBuffer && cube_index_ != rdr::kNoBuffer && ((!faults_.sweat_no_prev && !faults_.sweat_one_frame) || zero_image_ != rdr::kNoImage);
  if (!ok) rdr::Log("[rdr][small_passes] could not create the meshes: the stream's draws stay");
  return ok;
}

bool SmallPassesOurs::Claims(const rdr_adapter::DrawOrigin& o) const {
  return o.call_site == sp::kSweatCopyA || o.call_site == sp::kSweatCopyB || o.call_site == sp::kSweatApply || o.call_site == sp::kPreLight;
}

bool SmallPassesOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  ++stats_.decisions;
  const auto keep = [&](KeepWhy w) {
    why = w;
    ++stats_.kept[uint8_t(w)];
    return false;
  };
  const auto fail = [&](uint64_t& counter, KeepWhy w, const char* what) {
    ++counter;
    if (first_fail_.empty()) {
      char b[200];
      std::snprintf(b, sizeof b, "%s (call 0x%08X, stream %u vertices %u indices)", what, in.origin ? in.origin->call_site : 0u, in.vertex_count, in.index_count);
      first_fail_ = b;
    }
    return keep(w);
  };
  if (!ready_ || !in.origin || in.vs < 0 || size_t(in.vs) >= std::size(rdr::fx::kVariants)) return keep(KeepWhy::NotReady);
  const uint32_t site = in.origin->call_site;
  const bool pre = site == sp::kPreLight, sweat_apply = site == sp::kSweatApply;
  if (!pre && site != sp::kSweatCopyA && site != sp::kSweatCopyB && !sweat_apply) return keep(KeepWhy::NotOurs);
  // ---- the program is the effect we think it is
  if (!StartsWith(rdr::fx::kVariants[in.vs].name, pre ? "yPreLight_" : "yLiquidMap_")) return fail(stats_.bad_variant, KeepWhy::Variant, "variant");
  // ---- the stream's shape: counts, strip topology, 16-bit indices, the two-element 24-byte vertex
  const uint32_t want_v = pre ? sp::kCubeVertices : sp::kDiscVertices, want_i = pre ? sp::kCubeIndices : sp::kDiscIndices;
  if (in.vertex_count != want_v || in.index_count != want_i) return fail(stats_.bad_shape, KeepWhy::Mismatch, "counts");
  if (in.stream_topology != kTopologyStrip || in.stream_index_32bit) return fail(stats_.bad_shape, KeepWhy::Mismatch, "topology / index width");
  uint32_t stride = 0;
  std::vector<std::pair<uint16_t, uint32_t>> els;
  if (!Elements(in, stride, els) || stride != 24 || els.size() != 2) return fail(stats_.bad_layout, KeepWhy::Unjoined, "vertex layout");
  if (pre ? !(els[0] == std::make_pair(kFloat3, 0u) && els[1] == std::make_pair(kFloat3, 12u)) : !(els[0] == std::make_pair(kFloat2, 0u) && els[1] == std::make_pair(kFloat4, 8u)))
    return fail(stats_.bad_layout, KeepWhy::Mismatch, "vertex elements");
  // ---- the fetch slots the pass reads must hold images (the previous map in slot 0; the sweat's source in slot 1): a nulled slot is the SH2 blur_input_null class
  if (!pre && in.textures) {
    const int need = sweat_apply ? 2 : 1;
    rdr::TextureSet seen = *in.textures;   // the check reads a COPY of the stream's slots; the slot_null plant nulls slot 0 in the copy (the input), never inside the check
    if (faults_.slot_null) seen.slots[0].image = rdr::kNoImage;
    for (int s = 0; s < need; ++s) {
      const bool none = seen.slots[s].image == rdr::kNoImage;
      if (none) return fail(stats_.null_slot, KeepWhy::Texture, "fetch slot without an image");
    }
  }
  // ---- the target: ScreenScale must place the faces over the whole target (96 x 16 units onto 384 x 64); a mesh built for another size cannot
  if (pre && !faults_.no_verify) {
    float ss[4];
    const float tile = faults_.prelight_wrong_target_size ? 8.0f : 16.0f;
    if (!ReadRegister(in, kScreenScaleRegister, ss)) return fail(stats_.bad_clip, KeepWhy::Unjoined, "ScreenScale not in the block");
    const float x0 = ss[2], x1 = ss[0] * (6.0f * tile) + ss[2], y0 = ss[3], y1 = ss[1] * (-tile) + ss[3];
    if (std::fabs(x0 + 1.0f) > 2e-3f || std::fabs(x1 - 1.0f) > 2e-3f || std::fabs(y0 - 1.0f) > 2e-3f || std::fabs(y1 + 1.0f) > 2e-3f) return fail(stats_.bad_clip, KeepWhy::Mismatch, "the faces do not cover the target");
  }
  // ---- the stream's bytes against the rules (every draw offline; live: once per vertex offset)
  if (!faults_.no_verify) {
    auto& seen = pre ? verified_cube_ : verified_disc_;
    const std::pair<uint64_t, uint64_t> key{in.vertex_offset, in.index_offset};
    if (!live_ || seen != key) {
      if (!in.read_geometry) return fail(stats_.read_failed, KeepWhy::Unjoined, "live_geometry_missing (no read_geometry)");
      std::vector<uint8_t> bytes;
      if (!in.read_geometry(in.vertex_offset, in.vertex_count * stride, bytes) || bytes.size() < size_t(in.vertex_count) * stride) return fail(stats_.read_failed, KeepWhy::Unjoined, "live_geometry_missing (vertex read)");
      const float* got = reinterpret_cast<const float*>(bytes.data());
      const float* want = pre ? reinterpret_cast<const float*>(cube_.data()) : reinterpret_cast<const float*>(disc_.data());
      if (!(sp::MaxAbsDiff(got, want, size_t(in.vertex_count) * 6) <= kVertexTolerance)) {
        seen = {~0ull, ~0ull};
        return fail(stats_.bad_vertices, KeepWhy::Mismatch, "vertices");
      }
      std::vector<uint8_t> ib;
      if (!in.read_geometry(in.index_offset, in.index_count * 2, ib)) return fail(stats_.read_failed, KeepWhy::Unjoined, "live_geometry_missing (index read)");
      if (sp::CompareIndices(ib.data(), ib.size(), pre ? cube_idx_ : disc_idx_) < 0) {
        seen = {~0ull, ~0ull};
        return fail(stats_.bad_indices, KeepWhy::Mismatch, "indices");
      }
      seen = key;
      ++stats_.verified;
    }
  }
  out = rdr_adapter::ReplaceOut{};
  out.vertex = {pre ? cube_vertex_ : disc_vertex_, 0};
  out.index = {pre ? cube_index_ : disc_index_, 0};
  out.index_count = want_i;
  out.index_32bit = 0;
  out.vertex_layout = in.stream_layout;   // our vertex bytes have the stream's layout (checked above)
  out.topology = in.stream_topology;
  bool clear_prev = faults_.sweat_no_prev;
  if (faults_.sweat_one_frame && !pre) {   // R: ONE frame (the first sweat frame at or after the 150th claimed frame) reads a cleared previous map
    if (!one_frame_at_ && stats_.frames_ready >= 150) one_frame_at_ = stats_.frames_ready;
    clear_prev = one_frame_at_ && one_frame_at_ == stats_.frames_ready;
  }
  if (clear_prev && !pre && zero_image_ != rdr::kNoImage) {   // R: the previous map is empty
    out.slots[0].set = true;
    out.slots[0].image = zero_image_;
  }
  ++stats_.replaced;
  ++(pre ? stats_.prelight : stats_.sweat);
  return true;
}

std::string SmallPassesOurs::Report() const {
  char b[760];
  std::snprintf(b, sizeof b,
                "[rdr][small_passes] decisions %llu, replaced %llu (sweat %llu, prelight %llu) | verify %llu; read failed %llu; bad: vertices %llu indices %llu shape %llu layout %llu clip %llu variant %llu, fetch slot without image %llu | frames ready %llu idle %llu%s%s",
                (unsigned long long)stats_.decisions, (unsigned long long)stats_.replaced, (unsigned long long)stats_.sweat, (unsigned long long)stats_.prelight, (unsigned long long)stats_.verified, (unsigned long long)stats_.read_failed,
                (unsigned long long)stats_.bad_vertices, (unsigned long long)stats_.bad_indices, (unsigned long long)stats_.bad_shape, (unsigned long long)stats_.bad_layout, (unsigned long long)stats_.bad_clip,
                (unsigned long long)stats_.bad_variant, (unsigned long long)stats_.null_slot, (unsigned long long)stats_.frames_ready, (unsigned long long)stats_.frames_idle, first_fail_.empty() ? "" : " | first fail: ",
                first_fail_.c_str());
  return b;
}

}  // namespace rdr_scene
