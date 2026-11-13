// RimChainOurs (Session 101, lane 151; docs\renderer\RIM_MAP_OURS.md "P3"). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "rim_chain_ours.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "../renderer/shaders/spv/fx_registry.h"
#include "../renderer/shaders/spv/rdr_rimchain_blur_frag.h"
#include "../renderer/shaders/spv/rdr_rimchain_blur_vert.h"
#include "../renderer/shaders/spv/rdr_rimchain_flag_frag.h"
#include "../renderer/shaders/spv/rdr_rimchain_quad_vert.h"
#include "../renderer_adapter/capture_reader.h"
#include "replace_factories.h"

namespace rdr_scene {

namespace {

using rdr_adapter::KeepWhy;

// The stream's own programs for these draws (rdr::fx variant names) [C: fview --ops on Check B 7051 frame 200].
// (The reductions' vertex program is the pass-through quad: its hash is shared by several effects, so the binder names it by whichever effect comes first, "yAfterImage_quad_vertex" in this build: any variant "*_quad_vertex" with no constants is it.)
constexpr const char* kReduceFragment = "yBlur_flag_any_fragment";
constexpr const char* kBlurVertex = "yBlur_masked_vertex";
constexpr const char* kBlurFragment = "yBlur_t2_fragment";

// The blur's grid: 32 x 16 cells, 33 x 17 points of 24 bytes (x, y, z, w, u, v) and the triangle strip the game draws them with (per row: a0, then (a_i, b_i) for i = 0..32, then b32).
constexpr uint32_t kGridColumns = 32, kGridRows = 16, kGridPoints = (kGridColumns + 1) * (kGridRows + 1), kGridIndices = kGridRows * (1 + 2 * (kGridColumns + 1) + 1);
constexpr uint8_t kTopologyList = 3, kTopologyStrip = 4;   // VkPrimitiveTopology
constexpr uint32_t kReduceBlock = 32, kBlurBlock = 416, kBlurVertexBlock = 16;   // the stream variants' packed pixel / vertex constant sizes (fx_registry.h)

struct Grid {
  float vertices[kGridPoints][6];
  uint16_t indices[kGridIndices];
  Grid() {
    for (uint32_t r = 0; r <= kGridRows; ++r)
      for (uint32_t c = 0; c <= kGridColumns; ++c) {
        float* v = vertices[r * (kGridColumns + 1) + c];
        v[0] = -1.0f + float(c) / 16.0f, v[1] = -1.0f + float(r) / 8.0f, v[2] = 1.0f, v[3] = 1.0f, v[4] = float(c) / 32.0f, v[5] = 1.0f - float(r) / 16.0f;
      }
    uint32_t at = 0;
    for (uint32_t r = 0; r < kGridRows; ++r) {
      const uint16_t a0 = uint16_t(r * (kGridColumns + 1));
      indices[at++] = a0;
      for (uint32_t c = 0; c <= kGridColumns; ++c) {
        indices[at++] = uint16_t(r * (kGridColumns + 1) + c);
        indices[at++] = uint16_t((r + 1) * (kGridColumns + 1) + c);
      }
      indices[at++] = uint16_t((r + 1) * (kGridColumns + 1) + kGridColumns);
    }
  }
};
const Grid& TheGrid() {
  static const Grid grid;
  return grid;
}

}  // namespace

bool RimChainFaults::Parse(const std::string& list, RimChainFaults& out, std::string& error) {
  out = RimChainFaults{};
  size_t at = 0;
  while (at < list.size()) {
    size_t end = list.find(',', at);
    if (end == std::string::npos) end = list.size();
    const std::string name = list.substr(at, end - at);
    at = end + 1;
    if (name.empty()) continue;
    if (name == "reduction_wrong_size") out.reduction_wrong_size = true;
    else if (name == "blur_no_input") out.blur_no_input = true;
    else if (name == "blur_input_null") out.blur_input_null = true;
    else if (name == "rim_reader_stale") out.rim_reader_stale = true;
    else if (name == "unmasked") out.unmasked = true;
    else if (name == "no_verify") out.no_verify = true;
    else if (name == "grid_byte") out.grid_byte = true;
    else {
      error = "unknown rim chain fault '" + name + "' (reduction_wrong_size, blur_no_input, blur_input_null, rim_reader_stale, unmasked, no_verify, grid_byte)";
      return false;
    }
  }
  return true;
}

RimChainOurs::RimChainOurs(rdr::Renderer& renderer, const RimChainFaults& faults, bool live, std::function<bool(uint32_t, uint32_t&, uint32_t&)> image_info)
    : renderer_(renderer), faults_(faults), live_(live), image_info_(std::move(image_info)) {
  rdr::ProgramDesc reduce;
  reduce.name = "rdr_rimchain_reduce";
  reduce.vertex_spirv = kSpv_rdr_rimchain_quad_vert;
  reduce.vertex_bytes = sizeof(kSpv_rdr_rimchain_quad_vert);
  reduce.fragment_spirv = kSpv_rdr_rimchain_flag_frag;
  reduce.fragment_bytes = sizeof(kSpv_rdr_rimchain_flag_frag);
  reduce_program_ = renderer.AddProgram(reduce);
  rdr::ProgramDesc blur;
  blur.name = "rdr_rimchain_blur";
  blur.vertex_spirv = kSpv_rdr_rimchain_blur_vert;
  blur.vertex_bytes = sizeof(kSpv_rdr_rimchain_blur_vert);
  blur.fragment_spirv = kSpv_rdr_rimchain_blur_frag;
  blur.fragment_bytes = sizeof(kSpv_rdr_rimchain_blur_frag);
  blur_program_ = renderer.AddProgram(blur);
  layout_ = renderer.AddVertexLayout(rdr::VertexLayoutDesc{});   // no vertex buffer: the corners come from gl_VertexIndex
}

bool RimChainOurs::Claims(const rdr_adapter::DrawOrigin& o) const {
  // The two reductions are recorded from the generic post queue site (call_site, or `part` for a draw named inside a post scope); the blur is sub_82813EB0's draw. The variants decide the rest.
  return o.call_site == kRimBlurCallSite || o.call_site == kRimReduceCallSite || (o.op == 68 && o.part == kRimReduceCallSite);
}

bool RimChainOurs::FrameReady(const rdr_adapter::LoadedFrame&) {
  ++n_.frames;
  return true;   // needs no record and no file
}

uint32_t RimChainOurs::BlurDestCaptureId(const rdr_adapter::LoadedFrame& frame, uint32_t draw_index) const {
  size_t k = 0;
  for (; k < frame.ops.count; ++k)
    if (frame.ops[k].kind == rdr_adapter::OpKind::Draw && frame.ops[k].index == draw_index) break;
  if (k == frame.ops.count) return 0;
  uint32_t target = 0;
  for (size_t j = k + 1; j-- > 0;)
    if (frame.ops[j].kind == rdr_adapter::OpKind::SetTargets) {
      target = frame.targets[frame.ops[j].index].color[0].image;
      break;
    }
  if (!target) return 0;
  for (size_t j = k; j < frame.ops.count; ++j)
    if (frame.ops[j].kind == rdr_adapter::OpKind::Copy && frame.copies[frame.ops[j].index].source.image == target) return frame.copies[frame.ops[j].index].dest.image;
  return 0;
}

bool RimChainOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  ++n_.decisions;
  const auto keep = [&](KeepWhy reason, uint64_t& counter) {
    ++counter;
    why = reason;
    return false;
  };
  // 1. Which draw is it? The reductions and the blur by the stream's own program names and call site; any other draw at these sites (the glare's blur, other post quads) is not ours.
  const char* vs_name = in.vs >= 0 ? rdr::fx::kVariants[in.vs].name : "";
  const char* ps_name = in.ps >= 0 ? rdr::fx::kVariants[in.ps].name : "";
  const rdr_adapter::DrawOrigin& origin = *in.origin;
  const size_t vs_len = std::strlen(vs_name);
  const bool quad_vertex = vs_len > 12 && std::strcmp(vs_name + vs_len - 12, "_quad_vertex") == 0 && in.vs >= 0 && rdr::fx::kVariants[in.vs].constant_count == 0;
  const bool reduce = quad_vertex && std::strcmp(ps_name, kReduceFragment) == 0 && (origin.call_site == kRimReduceCallSite || origin.part == kRimReduceCallSite);
  const bool blur = std::strcmp(vs_name, kBlurVertex) == 0 && std::strcmp(ps_name, kBlurFragment) == 0 && origin.call_site == kRimBlurCallSite;
  const bool site_reduce = origin.call_site == kRimReduceCallSite || origin.part == kRimReduceCallSite, site_blur = origin.call_site == kRimBlurCallSite;
  if ((site_reduce && std::strcmp(ps_name, kReduceFragment) == 0 && !reduce) || (site_blur && std::strcmp(ps_name, kBlurFragment) == 0 && !blur))   // our pixel program at our site with another vertex program: counted, kept
    return keep(KeepWhy::Variant, n_.variant);
  if (!reduce && !blur) {
    ++n_.not_ours;
    why = KeepWhy::NotOurs;
    return false;
  }
  const rdr_adapter::DrawFixed& fixed = in.frame->fixeds[in.draw->fixed];
  // (The reductions ask for an alpha test, >= 0.502 on an alpha of 1: rdr_rimchain_flag.frag applies the stream's reference and function. The blur asks for none; coverage is nobody's.)
  if ((blur && fixed.alpha_test) || fixed.alpha_to_coverage) return keep(KeepWhy::Mismatch, n_.alpha);
  // 2. The constants: the pixel block as the stream packed it (the sizes the two variants have), and for the blur the grid's one vertex constant.
  const uint32_t want_fs = reduce ? kReduceBlock : kBlurBlock;
  if (!in.fs_block || in.fs_block_bytes != want_fs || rdr::fx::kVariants[in.ps].block_bytes != want_fs) return keep(KeepWhy::Mismatch, n_.constants);
  if (blur && (!in.vs_block || in.vs_block_bytes != kBlurVertexBlock)) return keep(KeepWhy::Mismatch, n_.constants);
  // 3. The textures: the input in slot 0 (and the coverage map in vertex slot 16 for the blur) must be bound; a plant can read the input as missing (SH2's blur_input_null class).
  const bool input_missing = in.textures->slots[0].image == rdr::kNoImage || (blur && faults_.blur_input_null);
  if (input_missing || (blur && in.textures->slots[16].image == rdr::kNoImage)) return keep(KeepWhy::Texture, n_.texture);
  // 4. The geometry, compared with the stream's own bytes.
  if (!in.read_geometry || in.stream_index_32bit != 0 || in.vertex_stride != 6 || in.stream_topology != (reduce ? kTopologyList : kTopologyStrip)) return keep(KeepWhy::Mismatch, n_.geometry);   // (the stream draws the reductions as a list and the grid as a strip)
  std::vector<uint8_t> vertex_bytes, index_bytes;
  if (reduce) {
    if (in.vertex_count != 4 || in.index_count != 6 || !in.read_geometry(in.vertex_offset, 96, vertex_bytes) || vertex_bytes.size() < 96 || !in.read_geometry(in.index_offset, 12, index_bytes) || index_bytes.size() < 12)
      return keep(KeepWhy::Mismatch, n_.geometry);
    static const uint16_t kQuadIndices[6] = {0, 1, 2, 0, 2, 3};
    uint16_t indices[6];
    std::memcpy(indices, index_bytes.data(), 12);
    float v[4][6];
    std::memcpy(v, vertex_bytes.data(), 96);
    // A rectangle: x by column, y by row, u by column, v by row, one z and w; the corners 0..1 in uv.
    const bool rect = std::memcmp(indices, kQuadIndices, 12) == 0 && v[0][0] == v[3][0] && v[1][0] == v[2][0] && v[0][1] == v[1][1] && v[2][1] == v[3][1] && v[0][4] == v[3][4] && v[1][4] == v[2][4] &&
                      v[0][5] == v[1][5] && v[2][5] == v[3][5] && v[0][4] == 0.0f && v[0][5] == 0.0f && v[2][4] == 1.0f && v[2][5] == 1.0f;
    bool one_depth = true;
    for (int i = 1; i < 4; ++i) one_depth = one_depth && v[i][2] == v[0][2] && v[i][3] == v[0][3];
    const float width = v[2][0] - v[0][0], height = v[2][1] - v[0][1];
    const bool first = width == 128.0f && height == 64.0f, second = width == 32.0f && height == 16.0f;   // the two reductions: 512x256 -> 128x64 -> 32x16
    if (!rect || !one_depth || (!first && !second)) return keep(KeepWhy::Mismatch, n_.geometry);
    // The input is four times the rectangle each way (when the owner can tell).
    uint32_t iw = 0, ih = 0;
    uint32_t capture_image = 0;
    for (uint32_t s = 0; s < in.draw->texture_count; ++s)
      if (in.frame->texture_slots[in.draw->texture_first + s].slot == 0) capture_image = in.frame->texture_slots[in.draw->texture_first + s].image;
    if (image_info_ && capture_image && image_info_(capture_image, iw, ih)) {
      if (float(iw) != width * 4.0f || float(ih) != height * 4.0f) return keep(KeepWhy::Mismatch, n_.size);
    } else {
      ++n_.size_unchecked;
    }
    quad_vs_[0] = v[0][0], quad_vs_[1] = v[0][1], quad_vs_[2] = v[2][0], quad_vs_[3] = v[2][1];
    quad_vs_[4] = v[0][4], quad_vs_[5] = v[0][5], quad_vs_[6] = v[2][4], quad_vs_[7] = v[2][5];
    quad_vs_[8] = v[0][2], quad_vs_[9] = v[0][3], quad_vs_[10] = 0.0f, quad_vs_[11] = 0.0f;
    if (faults_.reduction_wrong_size) quad_vs_[2] = quad_vs_[0] + width * 0.5f;   // plant: half the width is drawn
    std::memcpy(fs_, in.fs_block, kReduceBlock);
    out = rdr_adapter::ReplaceOut{};
    out.own.set = true;
    out.own.program = reduce_program_;
    out.own.vertex_layout = layout_;
    out.own.topology = 3;   // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    out.own.count = 6;
    out.own.vs_constants = reinterpret_cast<const uint8_t*>(quad_vs_), out.own.vs_bytes = sizeof(quad_vs_);
    out.own.fs_constants = fs_, out.own.fs_bytes = kReduceBlock;
    ++(first ? n_.reduce1 : n_.reduce2);
    return true;
  }
  // The blur: the grid, compared whole (the first draws and then every 64th live, every draw in fview).
  if (in.vertex_count != kGridPoints || in.index_count != kGridIndices) return keep(KeepWhy::Mismatch, n_.geometry);
  if (!faults_.no_verify) {   // every blur draw (one a frame, ~15 KB): call 0x82814080 is shared with other blurs, so a sampled compare could let a different grid through
    ++n_.verified;
    const Grid& grid = TheGrid();
    if (!in.read_geometry(in.vertex_offset, sizeof(grid.vertices), vertex_bytes) || vertex_bytes.size() < sizeof(grid.vertices) || !in.read_geometry(in.index_offset, sizeof(grid.indices), index_bytes) ||
        index_bytes.size() < sizeof(grid.indices) || std::memcmp(vertex_bytes.data(), grid.vertices, sizeof(grid.vertices)) != 0 || std::memcmp(index_bytes.data(), grid.indices, sizeof(grid.indices)) != 0 || faults_.grid_byte) {   // (grid_byte plant: one byte of the expected grid is wrong)
      ++n_.verify_bad;
      return keep(KeepWhy::Mismatch, n_.geometry);
    }
  }
  std::memcpy(grid_vs_, in.vs_block, kBlurVertexBlock);
  grid_vs_[4] = faults_.unmasked ? 1.0f : 0.0f, grid_vs_[5] = grid_vs_[6] = grid_vs_[7] = 0.0f;
  std::memcpy(fs_, in.fs_block, kBlurBlock);
  out = rdr_adapter::ReplaceOut{};
  out.own.set = true;
  out.own.program = blur_program_;
  out.own.vertex_layout = layout_;
  out.own.topology = 3;
  out.own.count = kGridColumns * kGridRows * 6;
  out.own.vs_constants = reinterpret_cast<const uint8_t*>(grid_vs_), out.own.vs_bytes = sizeof(grid_vs_);
  out.own.fs_constants = fs_, out.own.fs_bytes = kBlurBlock;
  if (faults_.blur_no_input) {   // plant: the blur's input bound to nothing
    out.slots[0].set = true;
    out.slots[0].image = rdr::kNoImage;
  }
  if (faults_.rim_reader_stale && in.map_image) {   // plant: the blur reads the blurred map of the previous frame (the copy after the blur rewrites it)
    const uint32_t dest = BlurDestCaptureId(*in.frame, in.draw_index);
    const rdr::ImageId stale = dest ? in.map_image(dest) : rdr::kNoImage;
    if (stale != rdr::kNoImage) {
      out.slots[0].set = true;
      out.slots[0].image = stale;
      out.slots[0].view_format = in.textures->slots[0].view_format;
      std::memcpy(out.slots[0].swizzle, in.textures->slots[0].swizzle, sizeof(out.slots[0].swizzle));
    }
  }
  ++n_.blur;
  return true;
}

std::string RimChainOurs::Report() const {
  char line[500];
  std::snprintf(line, sizeof(line),
                "rim chain: decisions %llu (not ours %llu), replaced reduce1 %llu reduce2 %llu blur %llu; kept: variant %llu, geometry %llu, constants %llu, texture %llu, alpha %llu, size %llu | verify %llu bad %llu, size unchecked %llu | frames %llu",
                (unsigned long long)n_.decisions, (unsigned long long)n_.not_ours, (unsigned long long)n_.reduce1, (unsigned long long)n_.reduce2, (unsigned long long)n_.blur, (unsigned long long)n_.variant,
                (unsigned long long)n_.geometry, (unsigned long long)n_.constants, (unsigned long long)n_.texture, (unsigned long long)n_.alpha, (unsigned long long)n_.size, (unsigned long long)n_.verified,
                (unsigned long long)n_.verify_bad, (unsigned long long)n_.size_unchecked, (unsigned long long)n_.frames);
  return line;
}

ReplaceHookHandle MakeRimChainHook(const ReplaceFactoryArgs& args) {
  ReplaceHookHandle h;
  if (!args.renderer) {
    h.error = "no renderer";
    return h;
  }
  RimChainFaults faults;
  std::string err;
  if (!RimChainFaults::Parse(args.fault, faults, err)) {
    h.error = err;
    return h;
  }
  auto hook = std::make_unique<RimChainOurs>(*args.renderer, faults, args.live, args.image_info);
  RimChainOurs* raw = hook.get();
  h.report = [raw]() { return raw->Report(); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
