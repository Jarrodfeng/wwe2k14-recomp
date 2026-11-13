// UiQuads (Session 101, lane 148; docs\renderer\UI_QUADS_OURS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "ui_quads.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../renderer/shaders/spv/fx_registry.h"
#include "../renderer/shaders/spv/rdr_uiquad_flat_frag.h"
#include "../renderer/shaders/spv/rdr_uiquad_frag.h"
#include "../renderer/shaders/spv/rdr_uiquad_vert.h"
#include "../renderer_adapter/capture_reader.h"

namespace rdr_scene {

namespace {

using rdr_adapter::KeepWhy;

// The programs of the two stream draws this takes over (rdr::fx variant names) [C: fview --ops on the menu / editor / Bryan / WM30 / Brock / Check B captures].
constexpr const char* kSpriteVs = "yRect_texcoord_vertex";
constexpr const char* kSpriteFs = "yAudience_color_alpha_fragment";
constexpr const char* kRectVs = "yRect_base_vertex";
constexpr const char* kRectFs = "yDebugPrimitive_base_fragment";

// The float4 at register `reg` of the stream's packed vertex block (Binder::PackBlock's layout through the variant's ConstantRange table); false if the program does not read it.
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

bool Finite(const float* f, int n) {
  for (int i = 0; i < n; ++i)
    if (!std::isfinite(f[i])) return false;
  return true;
}

}  // namespace

bool UiQuadFaults::Parse(const std::string& name, UiQuadFaults& out) {
  out = UiQuadFaults{};
  std::string n = name;
  if (n.rfind("unchecked_", 0) == 0) out.unchecked = true, n = n.substr(10);
  if (out.unchecked && n != "stride_plus_one") return false;   // only the stride plant has an unchecked twin
  if (n == "wrong_uv_rect") out.wrong_uv_rect = true;
  else if (n == "proj_flag_forced") out.proj_flag_forced = true;
  else if (n == "indices_permuted") out.indices_permuted = true;
  else if (n == "stride_plus_one") out.stride_plus_one = true;
  else if (n == "alpha_test_ignored") out.alpha_test_ignored = true;
  else if (n == "color_from_c2") out.color_from_c2 = true;
  else if (n == "no_blend") out.no_blend = true;
  else if (n == "fade_alpha_ignored") out.fade_alpha_ignored = true;
  else if (n == "half_pixel") out.half_pixel = true;
  else return false;
  return true;
}

UiQuads::UiQuads(rdr::Renderer& renderer, const UiQuadFaults& faults) : faults_(faults) {
  rdr::ProgramDesc program;
  program.name = "rdr_uiquad";
  program.vertex_spirv = kSpv_rdr_uiquad_vert;
  program.vertex_bytes = sizeof(kSpv_rdr_uiquad_vert);
  program.fragment_spirv = kSpv_rdr_uiquad_frag;
  program.fragment_bytes = sizeof(kSpv_rdr_uiquad_frag);
  program_sprite_ = renderer.AddProgram(program);
  program.name = "rdr_uiquad_flat";
  program.fragment_spirv = kSpv_rdr_uiquad_flat_frag;
  program.fragment_bytes = sizeof(kSpv_rdr_uiquad_flat_frag);
  program_rect_ = renderer.AddProgram(program);
  layout_ = renderer.AddVertexLayout(rdr::VertexLayoutDesc{});   // no vertex buffer: the corners come from the constants and gl_VertexIndex
}

bool UiQuads::Build(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, KeepWhy& why) {
  ++n_.decisions;
  const auto keep = [&](KeepWhy reason, uint64_t& counter) {
    ++counter;
    why = reason;
    return false;
  };
  // 1. Which stream draw: the sprite (position + texture coordinate, the picture times the colour) or the flat rectangle (position only, the colour).
  if (in.vs < 0 || in.ps < 0) return keep(KeepWhy::Variant, n_.variant);
  const char* vs_name = rdr::fx::kVariants[in.vs].name;
  const char* ps_name = rdr::fx::kVariants[in.ps].name;
  bool sprite;
  if (std::strcmp(vs_name, kSpriteVs) == 0 && std::strcmp(ps_name, kSpriteFs) == 0) sprite = true;
  else if (std::strcmp(vs_name, kRectVs) == 0 && std::strcmp(ps_name, kRectFs) == 0) sprite = false;
  else return keep(KeepWhy::Variant, n_.variant);   // (the hue-shifted sprite, gradients, glyph programs stay the stream's)
  const rdr_adapter::DrawFixed& fixed = in.frame->fixeds[in.draw->fixed];
  if (fixed.alpha_to_coverage) return keep(KeepWhy::Mismatch, n_.alpha_to_coverage);
  if (sprite && in.textures->slots[0].image == rdr::kNoImage) return keep(KeepWhy::Texture, n_.texture);   // (no picture, no sampler to use)

  // 2. The quad: 4 vertices (x, y, z[, u, v]) and the index list 0 1 2 0 2 3.
  const uint32_t stride = (sprite ? 5u : 3u) + (faults_.stride_plus_one ? 1u : 0u);   // the guest stride in dwords (plant stride_plus_one: one too many)
  if (in.stream_topology != 3 /* VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: a strip with these indices would draw another shape */ || in.vertex_count != 4 || in.index_count != 6 || in.stream_index_32bit != 0 || !in.read_geometry ||
      (!faults_.unchecked && in.vertex_stride != stride))
    return keep(KeepWhy::Mismatch, n_.geometry);
  if (!faults_.unchecked) {   // review S1: read_geometry returns HOST-converted bytes, so the host layout must be the one this reads: position (R32G32B32_SFLOAT) [+ texture coordinate (R32G32_SFLOAT) at 12], no mini fetch
    if (in.draw->layout == 0xFFFFFFFFu) return keep(KeepWhy::Mismatch, n_.geometry);
    const rdr_adapter::VertexLayout& L = in.frame->layouts[in.draw->layout];
    bool ok = L.host_stride == stride * 4 && L.element_count == (sprite ? 2u : 1u) && in.draw->vertex_bytes >= 4 * stride * 4;
    if (ok) {
      const rdr_adapter::VertexElement& e0 = in.frame->elements[L.element_first];
      ok = e0.vk_format == 106 && e0.host_offset == 0 && !e0.mini;
      if (ok && sprite) {
        const rdr_adapter::VertexElement& e1 = in.frame->elements[L.element_first + 1];
        ok = e1.vk_format == 103 && e1.host_offset == 12;   // (element 1 is the mini fetch of the same vertex fetch: measured mini = 1 on every sprite)
      }
    }
    if (!ok) return keep(KeepWhy::Mismatch, n_.geometry);
  }
  std::vector<uint8_t> vertex_bytes, index_bytes;
  if (!in.read_geometry(in.vertex_offset, 4 * stride * 4, vertex_bytes) || vertex_bytes.size() < 4 * stride * 4 || !in.read_geometry(in.index_offset, 12, index_bytes) || index_bytes.size() < 12)
    return keep(KeepWhy::Mismatch, n_.geometry);
  static const uint16_t kIndicesPlant[6] = {0, 2, 1, 0, 3, 2};
  static const uint16_t kIndicesTrue[6] = {0, 1, 2, 0, 2, 3};
  const uint16_t* kIndices = faults_.indices_permuted ? kIndicesPlant : kIndicesTrue;   // (plant indices_permuted: the check expects another list)
  uint16_t indices[6];
  std::memcpy(indices, index_bytes.data(), 12);
  if (std::memcmp(indices, kIndices, 12) != 0) return keep(KeepWhy::Mismatch, n_.quad);
  float v[4][5] = {};
  const uint32_t used = stride < 5u ? stride : 5u;
  for (int i = 0; i < 4; ++i) std::memcpy(v[i], vertex_bytes.data() + size_t(i) * stride * 4, used * 4);
  for (int i = 0; i < 4; ++i)
    if (!Finite(v[i], int(used))) return keep(KeepWhy::Mismatch, n_.quad);

  // 3. The stream's constants: the pixel-unit placement (not the projection matrix) with its unit size, and the colour.
  float pos_scale[4], color[4], use_proj[4];
  if (!ReadRegister(in, 0, pos_scale) || !ReadRegister(in, faults_.color_from_c2 ? 28u : 1u, color) || !ReadRegister(in, 28, use_proj) || (faults_.proj_flag_forced ? 1.0f : use_proj[0]) != 0.0f || !Finite(pos_scale, 4) || !Finite(color, 4))
    return keep(KeepWhy::Mismatch, n_.constants);

  // Ours: the four corners and the colour.
  std::memset(vs_, 0, sizeof(vs_));
  vs_[0] = pos_scale[0], vs_[1] = pos_scale[1], vs_[2] = 0.0f, vs_[3] = faults_.half_pixel ? 0.0f : 0.5f;   // w: the half unit the game takes off every position
  for (int i = 0; i < 4; ++i) {
    vs_[4 + 4 * i + 0] = v[i][0], vs_[4 + 4 * i + 1] = v[i][1], vs_[4 + 4 * i + 2] = v[i][2];
    if (sprite) vs_[20 + 4 * i + 0] = v[i][3], vs_[20 + 4 * i + 1] = v[i][4];
  }
  if (sprite && faults_.wrong_uv_rect)   // plant: the rectangle shrunk to its first quarter (corner 0 stays)
    for (int i = 1; i < 4; ++i) vs_[20 + 4 * i + 0] = vs_[20] + 0.25f * (vs_[20 + 4 * i + 0] - vs_[20]), vs_[20 + 4 * i + 1] = vs_[21] + 0.25f * (vs_[20 + 4 * i + 1] - vs_[21]);
  std::memcpy(fs_, color, 16);
  const bool alpha_off = sprite ? faults_.no_blend : faults_.fade_alpha_ignored;
  fs_[4] = alpha_off ? 0.0f : 1.0f, fs_[5] = alpha_off ? 1.0f : 0.0f, fs_[6] = faults_.alpha_test_ignored ? 0.0f : 1.0f, fs_[7] = 0.0f;   // z: 1 = apply the stream's alpha test
  n_.last_pos[0] = v[0][0], n_.last_pos[1] = v[0][1], n_.last_pos[2] = v[2][0], n_.last_pos[3] = v[2][1];

  out.own.set = true;
  out.own.program = sprite ? program_sprite_ : program_rect_;
  out.own.vertex_layout = layout_;
  out.own.topology = 3;   // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
  out.own.count = 6;
  out.own.vs_constants = reinterpret_cast<const uint8_t*>(vs_), out.own.vs_bytes = sizeof(vs_);
  out.own.fs_constants = reinterpret_cast<const uint8_t*>(fs_), out.own.fs_bytes = sizeof(fs_);
  ++(sprite ? n_.sprites : n_.rects);
  return true;
}

std::string UiQuads::Report() const {
  char line[400];
  std::snprintf(line, sizeof(line),
                "quads: decisions %llu, sprites %llu, rects %llu; kept: variant %llu, geometry %llu, quad %llu, constants %llu, alpha-to-coverage %llu, texture %llu; last quad %.0f,%.0f - %.0f,%.0f",
                (unsigned long long)n_.decisions, (unsigned long long)n_.sprites, (unsigned long long)n_.rects, (unsigned long long)n_.variant, (unsigned long long)n_.geometry,
                (unsigned long long)n_.quad, (unsigned long long)n_.constants, (unsigned long long)n_.alpha_to_coverage, (unsigned long long)n_.texture, n_.last_pos[0], n_.last_pos[1],
                n_.last_pos[2], n_.last_pos[3]);
  return line;
}

}  // namespace rdr_scene
