// MovieOurs (Session 101, lane 131; docs\renderer\MOVIE_OURS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "movie_ours.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "../renderer/shaders/spv/rdr_movie_frag.h"
#include "../renderer/shaders/spv/rdr_movie_vert.h"
#include "../renderer/shaders/spv/fx_registry.h"
#include "../renderer_adapter/capture_reader.h"

namespace rdr_scene {

namespace {

using rdr_adapter::KeepWhy;

// The two programs of the stream draw this hook takes over (rdr::fx variant names) [C: fview --ops on the l125 Bryan capture].
constexpr const char* kStreamVertex = "yRect_texcoord_vertex";
constexpr const char* kStreamFragment = "ySprite_yuv_alpha_fragment";

// A conversion as three rows over (Y, Cb, Cr), each (a, b, c, offset) on planes read as 0..1.
struct Matrix {
  float row[3][4];
};

// The game's own coefficients (the ySprite yuv shader, docs\renderer\effects\ySprite.md; harness-proven against the original): BT.601, limited range.
//   r = 1.164 Y + 1.596 Cr - 0.87075     g = 1.164 Y - 0.391 Cb - 0.813 Cr + 0.52925     b = 1.164 Y + 2.018 Cb - 1.08175
// (the green offset is the game's three constants summed: -0.07275 + 0.1955 + 0.4065).
constexpr Matrix kBt601 = {{{1.164f, 0.0f, 1.596f, -0.87075f}, {1.164f, -0.391f, -0.813f, 0.52925f}, {1.164f, 2.018f, 0.0f, -1.08175f}}};

// BT.709 limited range (the plant): the same video scaling (255/219, 255/224) on the 709 weights.
Matrix Bt709() {
  const float y = 255.0f / 219.0f, c = 255.0f / 224.0f;
  const float rv = 1.5748f * c, gu = -0.1873f * c, gv = -0.4681f * c, bu = 1.8556f * c;
  Matrix m = {{{y, 0.0f, rv, 0.0f}, {y, gu, gv, 0.0f}, {y, bu, 0.0f, 0.0f}}};
  for (auto& r : m.row) r[3] = -(y * 16.0f / 255.0f) - (r[1] + r[2]) * (128.0f / 255.0f);
  return m;
}

// The float at register `reg` of the stream's packed vertex block (Binder::PackBlock's layout through the variant's ConstantRange table); false if the program does not read it.
bool ReadRegister(const rdr_adapter::ReplaceIn& in, uint32_t reg, float out[4]) {
  const rdr::fx::Variant& v = rdr::fx::kVariants[in.vs];
  for (uint32_t i = 0; i < v.constant_count; ++i) {
    const rdr::fx::ConstantRange& r = v.constants[i];
    if (reg < r.reg || reg >= uint32_t(r.reg) + r.count) continue;
    const uint32_t at = uint32_t(r.offset) + (reg - r.reg) * 16u;
    if (at + 16u > in.vs_block_bytes) return false;
    std::memcpy(out, in.vs_block + at, 16);
    return true;
  }
  return false;
}

}  // namespace

bool MovieFaults::Parse(const std::string& list, MovieFaults& out, std::string& error) {
  out = MovieFaults{};
  size_t at = 0;
  while (at < list.size()) {
    size_t end = list.find(',', at);
    if (end == std::string::npos) end = list.size();
    const std::string name = list.substr(at, end - at);
    at = end + 1;
    if (name.empty()) continue;
    if (name == "stale_planes") out.stale_planes = true;
    else if (name == "bt601_vs_709") out.bt601_vs_709 = true;
    else if (name == "no_alpha") out.no_alpha = true;
    else if (name == "swapped_uv") out.swapped_uv = true;
    else if (name == "half_pixel") out.half_pixel = true;
    else if (name == "uv_flip_v") out.uv_flip_v = true;
    else if (name == "green_bias") out.green_bias = true;
    else if (name == "no_alpha_test") out.no_alpha_test = true;
    else if (name == "alpha_test_inverted") out.alpha_test_inverted = true;
    else if (name == "own_count_zero") out.own_count_zero = true;
    else if (name == "own_program_zero") out.own_program_zero = true;
    else {
      error = "unknown movie fault '" + name + "' (stale_planes, bt601_vs_709, no_alpha, swapped_uv, half_pixel, uv_flip_v, green_bias, no_alpha_test, alpha_test_inverted, own_count_zero, own_program_zero)";
      return false;
    }
  }
  return true;
}

MovieOurs::MovieOurs(rdr::Renderer& renderer, const MovieFaults& faults) : faults_(faults) {
  rdr::ProgramDesc program;
  program.name = "rdr_movie";
  program.vertex_spirv = kSpv_rdr_movie_vert;
  program.vertex_bytes = sizeof(kSpv_rdr_movie_vert);
  program.fragment_spirv = kSpv_rdr_movie_frag;
  program.fragment_bytes = sizeof(kSpv_rdr_movie_frag);
  program_ = renderer.AddProgram(program);
  layout_ = renderer.AddVertexLayout(rdr::VertexLayoutDesc{});   // no vertex buffer: the corners come from gl_VertexIndex
}

bool MovieOurs::Claims(const rdr_adapter::DrawOrigin& origin) const {
  return origin.klass == rdr_adapter::DrawClass::Movie && origin.call_site == kMovieCallSite;
}

bool MovieOurs::Replace(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  ++n_.decisions;
  const auto keep = [&](KeepWhy reason, uint64_t& counter) {
    ++counter;
    why = reason;
    return false;
  };
  // 1. The stream draw is the one we know: the rectangle program with texture coordinates and the four-plane video pixel program.
  if (in.vs < 0 || in.ps < 0 || std::strcmp(rdr::fx::kVariants[in.vs].name, kStreamVertex) != 0 || std::strcmp(rdr::fx::kVariants[in.ps].name, kStreamFragment) != 0)
    return keep(KeepWhy::Variant, n_.variant);
  const rdr_adapter::DrawFixed& fixed = in.frame->fixeds[in.draw->fixed];
  if (fixed.alpha_to_coverage) return keep(KeepWhy::Mismatch, n_.alpha_test);   // (the alpha TEST is ours: rdr_movie.frag applies the stream's reference and function; coverage is not)
  // 2. The four planes are bound (a plane slot with no image has no sampler to use).
  for (int s = 0; s < 4; ++s)
    if (in.textures->slots[s].image == rdr::kNoImage) return keep(KeepWhy::Texture, n_.planes);
  // 3. The quad: 4 vertices x 20 bytes (x, y, z, u, v) and the index list 0 1 2 0 2 3, an axis-aligned rectangle with the uv corners to match.
  if (in.vertex_count != 4 || in.index_count != 6 || in.vertex_stride != 5 ||  /* the guest stride, in dwords: 20 bytes */ in.stream_index_32bit != 0 || !in.read_geometry) return keep(KeepWhy::Mismatch, n_.geometry);
  std::vector<uint8_t> vertex_bytes, index_bytes;
  if (!in.read_geometry(in.vertex_offset, 80, vertex_bytes) || vertex_bytes.size() < 80 || !in.read_geometry(in.index_offset, 12, index_bytes) || index_bytes.size() < 12)
    return keep(KeepWhy::Mismatch, n_.geometry);
  static const uint16_t kIndices[6] = {0, 1, 2, 0, 2, 3};
  uint16_t indices[6];
  std::memcpy(indices, index_bytes.data(), 12);
  if (std::memcmp(indices, kIndices, 12) != 0) return keep(KeepWhy::Mismatch, n_.quad);
  float v[4][5];
  std::memcpy(v, vertex_bytes.data(), 80);
  const bool rect = v[0][0] == v[3][0] && v[1][0] == v[2][0] && v[0][1] == v[1][1] && v[2][1] == v[3][1] &&   // positions: x by column, y by row
                    v[0][3] == v[3][3] && v[1][3] == v[2][3] && v[0][4] == v[1][4] && v[2][4] == v[3][4] &&   // uv: u by column, v by row
                    v[0][2] == v[1][2] && v[0][2] == v[2][2] && v[0][2] == v[3][2];                           // one depth
  if (!rect) return keep(KeepWhy::Mismatch, n_.quad);
  // 4. The stream's constants: the pixel-unit placement (not the projection matrix) with its unit size, and the colour.
  float pos_scale[4], color[4], use_proj[4];
  if (!ReadRegister(in, 0, pos_scale) || !ReadRegister(in, 1, color) || !ReadRegister(in, 28, use_proj) || use_proj[0] != 0.0f) return keep(KeepWhy::Mismatch, n_.constants);

  // Ours: the rectangle and the conversion.
  vs_[0] = pos_scale[0], vs_[1] = pos_scale[1], vs_[2] = v[0][2], vs_[3] = faults_.half_pixel ? 0.0f : 0.5f;   // w: the half unit the game takes off every position
  vs_[4] = v[0][0], vs_[5] = v[0][1], vs_[6] = v[1][0], vs_[7] = v[2][1];
  vs_[8] = v[0][3], vs_[9] = v[0][4], vs_[10] = v[1][3], vs_[11] = v[2][4];
  if (faults_.uv_flip_v) std::swap(vs_[9], vs_[11]);
  const Matrix m = faults_.bt601_vs_709 ? Bt709() : kBt601;
  std::memcpy(fs_, color, 16);
  std::memcpy(fs_ + 4, m.row[0], 16);
  std::memcpy(fs_ + 8, m.row[1], 16);
  std::memcpy(fs_ + 12, m.row[2], 16);
  if (faults_.green_bias) fs_[11] += 0.6f / 255.0f;   // RowG.w
  fs_[16] = faults_.no_alpha ? 0.0f : 1.0f, fs_[17] = faults_.no_alpha ? 1.0f : 0.0f, fs_[18] = faults_.no_alpha_test ? 0.0f : faults_.alpha_test_inverted ? -1.0f : 1.0f, fs_[19] = 0.0f;   // z: 1 = apply the stream's alpha test
  std::memcpy(n_.last_rect, vs_ + 4, 16);
  std::memcpy(n_.last_uv, vs_ + 8, 16);

  out = rdr_adapter::ReplaceOut{};
  out.own.set = true;
  out.own.program = faults_.own_program_zero ? 0 : program_;
  out.own.vertex_layout = layout_;
  out.own.topology = 3;   // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
  out.own.count = faults_.own_count_zero ? 0 : 6;
  out.own.vs_constants = reinterpret_cast<const uint8_t*>(vs_), out.own.vs_bytes = sizeof(vs_);
  out.own.fs_constants = reinterpret_cast<const uint8_t*>(fs_), out.own.fs_bytes = sizeof(fs_);

  // Plants on the planes: the very images the stream binds are used unless a plant says otherwise.
  const auto plane = [&](int s, const rdr::TextureSlot& source) {
    rdr_adapter::ReplaceOut::Slot& slot = out.slots[s];
    slot.set = true;
    slot.image = source.image;
    slot.view_format = source.view_format;
    std::memcpy(slot.swizzle, source.swizzle, sizeof(slot.swizzle));
  };
  const uint64_t guest_frame = in.frame->header.guest_frame;
  const bool new_frame = !have_current_ || guest_frame != current_frame_;
  if (faults_.stale_planes && (new_frame ? have_current_ : have_previous_)) {   // the last frame's images (a frame bound again does not count as a new one)
    const rdr::TextureSlot* last = new_frame ? current_ : previous_;
    for (int s = 0; s < 4; ++s) plane(s, last[s]);
  }
  if (faults_.swapped_uv) {
    plane(1, in.textures->slots[2]);
    plane(2, in.textures->slots[1]);
  }
  if (new_frame) {
    if (have_current_) { for (int s = 0; s < 4; ++s) previous_[s] = current_[s]; have_previous_ = true; }
    current_frame_ = guest_frame;
    have_current_ = true;
  }
  for (int s = 0; s < 4; ++s) current_[s] = in.textures->slots[s];
  ++n_.replaced;
  return true;
}

std::string MovieOurs::Report() const {
  char line[400];
  std::snprintf(line, sizeof(line),
                "decisions %llu, replaced %llu; kept: variant %llu, geometry %llu, quad %llu, constants %llu, alpha-to-coverage %llu, planes %llu; last quad %.0f,%.0f - %.0f,%.0f uv %.4f,%.4f - %.4f,%.4f",
                (unsigned long long)n_.decisions, (unsigned long long)n_.replaced, (unsigned long long)n_.variant, (unsigned long long)n_.geometry, (unsigned long long)n_.quad,
                (unsigned long long)n_.constants, (unsigned long long)n_.alpha_test, (unsigned long long)n_.planes, n_.last_rect[0], n_.last_rect[1], n_.last_rect[2], n_.last_rect[3],
                n_.last_uv[0], n_.last_uv[1], n_.last_uv[2], n_.last_uv[3]);
  return line;
}

}  // namespace rdr_scene
