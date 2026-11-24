// UiQuads (Session 101, lane 148; docs\renderer\UI_QUADS_OURS.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "ui_quads.h"
#include "rows_quad2d.h"   // S106 G4-2

#include <cmath>
#include <cstdio>
#include <cstring>

#include "../renderer/shaders/spv/fx_registry.h"
#include "../renderer/shaders/spv/rdr_uiglyph_frag.h"
#include "../renderer/shaders/spv/rdr_uiquad_flat_frag.h"
#include "../renderer/shaders/spv/rdr_uiquad_frag.h"
#include "../renderer/shaders/spv/rdr_uiquad_hsl_frag.h"
#include "../renderer/shaders/spv/rdr_uiquad_proj_vert.h"
#include "../renderer/shaders/spv/rdr_uiquad_vert.h"
#include "../renderer_adapter/capture_reader.h"
#include "quads2d_rig.h"   // S103 I1: --quads-2d-rows (the rect from a Quad2D row, offline)

namespace rdr_scene {

namespace {

using rdr_adapter::KeepWhy;

// The programs of the two stream draws this takes over (rdr::fx variant names) [C: fview --ops on the menu / editor / Bryan / WM30 / Brock / Check B captures].
constexpr const char* kSpriteVs = "yRect_texcoord_vertex";
constexpr const char* kSpriteFs = "yAudience_color_alpha_fragment";
constexpr const char* kRectVs = "yRect_base_vertex";
constexpr const char* kRectFs = "yDebugPrimitive_base_fragment";
constexpr const char* kSpriteHslFs = "ySprite_hsl_fragment";   // S103 I2: the hue-shifted sprite's pixel program (offline row rig only)
constexpr const char* kGlyphVs = "ySprite_channel_select_vertex";   // S103 I3: the text glyph's programs (offline row rig only) [C: fview --ops on brock_losswin frames 140-210 and the Brock demo frames 930-1062: every glyph draw at 0x82BC3784]
constexpr const char* kGlyphFs = "ySprite_channel_mask_fragment";

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

// S103 I2: the float4 at register `reg` of the stream's packed PIXEL block (the ps variant's ConstantRange table); false if the program does not read it or the hook was not given the block.
bool ReadPsRegister(const rdr_adapter::ReplaceIn& in, uint32_t reg, float out[4]) {
  if (!in.fs_block) return false;
  const rdr::fx::Variant& v = rdr::fx::kVariants[in.ps];
  for (uint32_t i = 0; i < v.constant_count; ++i) {
    const rdr::fx::ConstantRange& r = v.constants[i];
    if (reg < r.reg || reg >= uint32_t(r.reg) + r.count) continue;
    const uint32_t at = uint32_t(r.offset) + (reg - r.reg) * 16u;
    if (at + 16u > in.fs_block_bytes) return false;
    std::memcpy(out, in.fs_block + at, 16);
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
  if (rdr_scene::q2d::rig::g_on) {   // S103 I2: ONLY the offline row rig builds the hue program (fview sets g_on before UiOurs exists); with the switch off the program list is the one it always was (tools/scene/uiquads_programs_lint.py)
    program.name = "rdr_uiquad_hsl";
    program.fragment_spirv = kSpv_rdr_uiquad_hsl_frag;
    program.fragment_bytes = sizeof(kSpv_rdr_uiquad_hsl_frag);
    program_sprite_hsl_ = renderer.AddProgram(program);
    program.name = "rdr_uiglyph";   // S103 I3: the text glyph (the same vertex program)
    program.fragment_spirv = kSpv_rdr_uiglyph_frag;
    program.fragment_bytes = sizeof(kSpv_rdr_uiglyph_frag);
    program_glyph_ = renderer.AddProgram(program);
    program.name = "rdr_uiquad_flat_proj";   // S103 P3c: the matrix variants (rdr_uiquad_proj.vert): flat rect, picture sprite, text glyph; the row rig only
    program.vertex_spirv = kSpv_rdr_uiquad_proj_vert;
    program.vertex_bytes = sizeof(kSpv_rdr_uiquad_proj_vert);
    program.fragment_spirv = kSpv_rdr_uiquad_flat_frag;
    program.fragment_bytes = sizeof(kSpv_rdr_uiquad_flat_frag);
    program_rect_proj_ = renderer.AddProgram(program);
    program.name = "rdr_uiquad_proj";
    program.fragment_spirv = kSpv_rdr_uiquad_frag;
    program.fragment_bytes = sizeof(kSpv_rdr_uiquad_frag);
    program_sprite_proj_ = renderer.AddProgram(program);
    program.name = "rdr_uiglyph_proj";
    program.fragment_spirv = kSpv_rdr_uiglyph_frag;
    program.fragment_bytes = sizeof(kSpv_rdr_uiglyph_frag);
    program_glyph_proj_ = renderer.AddProgram(program);
    program.vertex_spirv = kSpv_rdr_uiquad_vert;   // (the programs below this block are the plain ones again)
    program.vertex_bytes = sizeof(kSpv_rdr_uiquad_vert);
  }
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
  bool sprite, hsl = false, glyph = false;
  if (rdr_scene::q2d::rig::g_on && in.origin && in.origin->call_site == kUiQuadSpriteSite) {   // S103 I2: the census of the sprite site's fragment programs by name (REMNANT_MAP U8: which sprites carry a hue shift)
    auto& e = rdr_scene::q2d::rig::g_programs[ps_name];
    if (!e.first++) e.second = in.frame->header.guest_frame;
  }
  if (std::strcmp(vs_name, kSpriteVs) == 0 && std::strcmp(ps_name, kSpriteFs) == 0) sprite = true;
  else if (rdr_scene::q2d::rig::g_on && std::strcmp(vs_name, kSpriteVs) == 0 && std::strcmp(ps_name, kSpriteHslFs) == 0) sprite = true, hsl = true;   // S103 I2: the row rig draws the hue-shifted sprite too
  else if (rdr_scene::q2d::rig::g_on && in.origin && in.origin->call_site == kUiQuadGlyphSite && std::strcmp(vs_name, kGlyphVs) == 0 && std::strcmp(ps_name, kGlyphFs) == 0) sprite = true, glyph = true;   // S103 I3: the text glyph; the row rig only
  else if (std::strcmp(vs_name, kRectVs) == 0 && std::strcmp(ps_name, kRectFs) == 0) sprite = false;
  else return keep(KeepWhy::Variant, n_.variant);   // (the hue-shifted sprite, gradients, glyph programs stay the stream's)
  const rdr_adapter::DrawFixed& fixed = in.frame->fixeds[in.draw->fixed];
  const rdr::Rect stream_scissor = in.frame->dynamics[in.draw->dynamic].host.scissor;   // S103 SC: the stream draw's window scissor (1x guest pixels): the row rig's row scissor
  const rdr_scene::q2d::rig::RigTarget stream_target = rdr_scene::q2d::rig::g_on ? rdr_scene::q2d::rig::TargetOf(in) : rdr_scene::q2d::rig::RigTarget{};   // S103 SC review F3: the surface the stream draw renders into (the row rig only)
  if (fixed.alpha_to_coverage) return keep(KeepWhy::Mismatch, n_.alpha_to_coverage);
  if (sprite && in.textures->slots[0].image == rdr::kNoImage) return keep(KeepWhy::Texture, n_.texture);   // (no picture, no sampler to use)

  // 2. The quad: 4 vertices (x, y, z[, u, v]) and the index list 0 1 2 0 2 3.
  const uint32_t stride = (sprite ? 5u : 3u) + (faults_.stride_plus_one ? 1u : 0u);   // the guest stride in dwords (plant stride_plus_one: one too many)
  if (glyph) ++rdr_scene::q2d::rig::g_stats.glyph_topology[in.stream_topology & 7];   // S103 I3: the census of the glyph draws' primitive topology
  // S103 P3b (row rig only): the two strip sites of the menu leaves: sub_82BC4450's single flat T_Rect (0x82BC484C) and sub_82BC8180's plain T_Sprite (technique 0x8206AA0C) (0x82BC8BEC) are 4-vertex STRIPS (op-19 primitive 6, Vulkan topology 4, not indexed), not the quad list the other sprite / rect draws are.
  const uint32_t call_site = in.origin ? in.origin->call_site : 0u;
  const bool strip_site = rdr_scene::q2d::rig::g_on && ((!sprite && call_site == rdr_scene::q2d::kSiteRectGrad) || (sprite && !glyph && call_site == rdr_scene::q2d::kSiteGlyphM));
  const bool unindexed = glyph || strip_site;
  if ((glyph ? in.index_count != 0 : strip_site ? (in.stream_topology != 4 || in.index_count != 0) : (in.stream_topology != 3 /* VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: a strip with these indices would draw another shape */ || in.index_count != 6)) || in.vertex_count != 4 || in.stream_index_32bit != 0 || !in.read_geometry ||
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
  if (!in.read_geometry(in.vertex_offset, 4 * stride * 4, vertex_bytes) || vertex_bytes.size() < 4 * stride * 4 || (!unindexed && (!in.read_geometry(in.index_offset, 12, index_bytes) || index_bytes.size() < 12)))
    return keep(KeepWhy::Mismatch, n_.geometry);
  static const uint16_t kIndicesPlant[6] = {0, 2, 1, 0, 3, 2};
  static const uint16_t kIndicesTrue[6] = {0, 1, 2, 0, 2, 3};
  const uint16_t* kIndices = faults_.indices_permuted ? kIndicesPlant : kIndicesTrue;   // (plant indices_permuted: the check expects another list)
  uint16_t indices[6];
  if (!unindexed) {   // (S103 I3: a glyph draw is not indexed; S103 P3b: nor are the two strip sites: its four vertices are drawn as the primitive the op-19 recorder was given: prim 6 = a strip, which the row's blocks take in perimeter order)
    std::memcpy(indices, index_bytes.data(), 12);
    if (std::memcmp(indices, kIndices, 12) != 0) return keep(KeepWhy::Mismatch, n_.quad);
  }
  float v[4][5] = {};
  const uint32_t used = stride < 5u ? stride : 5u;
  for (int i = 0; i < 4; ++i) std::memcpy(v[i], vertex_bytes.data() + size_t(i) * stride * 4, used * 4);
  for (int i = 0; i < 4; ++i)
    if (!Finite(v[i], int(used))) return keep(KeepWhy::Mismatch, n_.quad);

  // 3. The stream's constants: the pixel-unit placement (not the projection matrix) with its unit size, and the colour.
  float pos_scale[4], color[4], use_proj[4], selector[4] = {};
  if (glyph && !ReadRegister(in, 4, selector)) return keep(KeepWhy::Mismatch, n_.constants);   // S103 I3: the channel selector c4 of the channel_select program
  {   // S103 P3b: the census of the matrix-placed draws (c28 set); the check below keeps them
    float up[4] = {};
    if (rdr_scene::q2d::rig::g_on && ReadRegister(in, 28, up) && up[0] != 0.0f) ++n_.projection;
  }
  if (!ReadRegister(in, 0, pos_scale) || !ReadRegister(in, faults_.color_from_c2 ? 28u : 1u, color) || !ReadRegister(in, 28, use_proj)) return keep(KeepWhy::Mismatch, n_.constants);
  float proj_regs[16] = {};   // S103 P3c (row rig only): a matrix-placed draw (c28 != 0) is taken with the four matrix registers c29..c32 of the stream's own block; any other run keeps it as before
  bool matrix_placed = false;
  if ((faults_.proj_flag_forced ? 1.0f : use_proj[0]) != 0.0f) {
    if (!rdr_scene::q2d::rig::g_on || faults_.proj_flag_forced) return keep(KeepWhy::Mismatch, n_.constants);
    for (uint32_t k = 0; k < 4; ++k)
      if (!ReadRegister(in, 29 + k, proj_regs + 4 * k) || !Finite(proj_regs + 4 * k, 4)) return ++n_.proj_refused, keep(KeepWhy::Mismatch, n_.constants);
    matrix_placed = true;
    ++n_.proj_taken;
  }
  if (!Finite(pos_scale, 4) || !Finite(color, 4)) return keep(KeepWhy::Mismatch, n_.constants);

  float hsl_value[4] = {};
  if (hsl && !ReadPsRegister(in, 2, hsl_value)) return keep(KeepWhy::Mismatch, n_.constants);   // S103 I2: the hue-shift constant c2 of the stream's pixel block
  if (hsl && !Finite(hsl_value, 3)) return keep(KeepWhy::Mismatch, n_.constants);

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

  uint32_t fs_bytes = 8 * 4;
  bool proj_block = false;   // S103 P3c: the vertex block is the matrix variant's (vs_proj_, 52 floats)
  uint16_t own_program = sprite ? program_sprite_ : program_rect_;
  if (rdr_scene::q2d::rig::g_on && glyph) {   // S103 I3: the glyph from a Quad2D ROW (the truth of this stream draw: corners with uv, c0 / c1 / c4 / c28, the sheet's KEY, the primitive word), through the record's format and the planted twins, drawn by the pure BlocksOf
    namespace rig = rdr_scene::q2d::rig;
    ++rig::g_stats.glyph_taken;
    const uint8_t prim = in.stream_topology == 4 ? rdr_scene::q2d::kGlyphPrimStrip : uint8_t(100 + in.stream_topology);   // Vulkan strip = the game's primitive 6 (Xbox 360 D3DPT_TRIANGLESTRIP); anything else is carried as an unknown word (BlocksOf refuses it)
    Quad2D row = rig::Truth(kQkGlyph, in.origin->call_site, in.origin->object, in.frame->header.guest_frame, v, pos_scale, color, use_proj[0], fixed, in.origin->part, nullptr, selector, prim, matrix_placed ? proj_regs : nullptr, &stream_scissor, in.frame->dynamics[in.draw->dynamic].host.viewport, &stream_target);
    {   // S103 I3 review: the row that is drawn is the one the TAP path assembles from the stream's numbers (register shadow + the op-19 core), not the Truth row
      Quad2D tapped;
      if (!rig::BuildGlyphViaTap(row, tapped)) return keep(KeepWhy::Mismatch, n_.quad);
      row = tapped;
    }
    rdr_scene::q2d::Blocks blocks;
    bool degenerate = false;
    if (rig::Make(row, fixed, blocks, degenerate) != rdr_scene::q2d::Refuse::None) return keep(KeepWhy::Mismatch, n_.quad);
    rig::ApplyScissor(row, out.own);   // S103 SC
    static_assert(sizeof(vs_) == 36 * 4 && sizeof(fs_) == 12 * 4, "the glyph blocks are 36 + 12 floats");
    if (blocks.proj) std::memcpy(vs_proj_, blocks.vs, sizeof(vs_proj_)), proj_block = true;
    else std::memcpy(vs_, blocks.vs, sizeof(vs_));
    std::memcpy(fs_, blocks.fs, blocks.fs_bytes);
    fs_bytes = blocks.fs_bytes;
    own_program = blocks.proj ? program_glyph_proj_ : program_glyph_;
    rig::g_last_sprite_key = row.tex_key[0];
  } else if (rdr_scene::q2d::rig::g_on && sprite) {   // S103 I2: the sprite from a Quad2D ROW (the truth of this stream draw: corners with uv, c0 / c1, the picture KEY, the hue shift), through the record's format and the planted twins, drawn by the pure BlocksOf
    namespace rig = rdr_scene::q2d::rig;
    Quad2D row = rig::Truth(kQkSprite, in.origin ? in.origin->call_site : kUiQuadSpriteSite, in.origin ? in.origin->object : 0u, in.frame->header.guest_frame, v, pos_scale, color, use_proj[0], fixed, in.origin ? in.origin->part : 0u,
                            hsl ? hsl_value : nullptr, nullptr, strip_site ? rdr_scene::q2d::kGlyphPrimStrip : 0, matrix_placed ? proj_regs : nullptr, &stream_scissor, in.frame->dynamics[in.draw->dynamic].host.viewport, &stream_target);
    if (strip_site) {   // S103 P3c: the strip-site row is the one the TAP path assembles (scope constants: op 21 c0 / c1 / c28 / c29..c32, the op-19 core), not the Truth row
      Quad2D tapped;
      if (!rig::BuildStripViaTap(row, tapped)) return keep(KeepWhy::Mismatch, n_.quad);
      row = tapped;
    }
    rdr_scene::q2d::Blocks blocks;
    bool degenerate = false;
    if (rig::Make(row, fixed, blocks, degenerate) != rdr_scene::q2d::Refuse::None) return keep(KeepWhy::Mismatch, n_.quad);
    rig::ApplyScissor(row, out.own);   // S103 SC
    static_assert(sizeof(vs_) == 36 * 4 && sizeof(fs_) == 12 * 4, "the sprite blocks are 36 + (8 | 12) floats");
    if (blocks.proj) std::memcpy(vs_proj_, blocks.vs, sizeof(vs_proj_)), proj_block = true;
    else std::memcpy(vs_, blocks.vs, sizeof(vs_));
    std::memcpy(fs_, blocks.fs, blocks.fs_bytes);
    fs_bytes = blocks.fs_bytes;
    own_program = blocks.proj ? program_sprite_proj_ : blocks.program == rdr_scene::q2d::Program::SpriteHsl ? program_sprite_hsl_ : program_sprite_;
    rig::g_last_sprite_key = row.tex_key[0];
  }
  if (rdr_scene::q2d::rig::g_on && !sprite) {   // S103 I1 (REMNANT_MAP.md section 10): the rect is drawn from a Quad2D ROW (the truth row of this stream draw, through the record's format and the planted twins), by the drawer's pure
    namespace rig = rdr_scene::q2d::rig;      // function; the stream's constants above are no longer what the quad is made of. Total overwrite of both blocks. A refusal keeps the stream's draw (counted by reason).
    Quad2D row = rig::Truth(kQkRect, in.origin ? in.origin->call_site : kUiQuadRectSite, in.origin ? in.origin->object : 0u, in.frame->header.guest_frame, v, pos_scale, color, use_proj[0], fixed, 0, nullptr, nullptr, strip_site ? rdr_scene::q2d::kGlyphPrimStrip : 0, matrix_placed ? proj_regs : nullptr, &stream_scissor, in.frame->dynamics[in.draw->dynamic].host.viewport, &stream_target);
    if (strip_site) {   // S103 P3c: the tap path's row (see the sprite)
      Quad2D tapped;
      if (!rig::BuildStripViaTap(row, tapped)) return keep(KeepWhy::Mismatch, n_.quad);
      row = tapped;
    }
    rdr_scene::q2d::Blocks blocks;
    bool degenerate = false;
    if (rig::Make(row, fixed, blocks, degenerate) != rdr_scene::q2d::Refuse::None) return keep(KeepWhy::Mismatch, n_.quad);
    rig::ApplyScissor(row, out.own);   // S103 SC
    static_assert(sizeof(vs_) == 36 * 4, "the rect vertex block is 36 floats");
    if (blocks.proj) std::memcpy(vs_proj_, blocks.vs, sizeof(vs_proj_)), proj_block = true, own_program = program_rect_proj_;
    else std::memcpy(vs_, blocks.vs, sizeof(vs_));
    std::memcpy(fs_, blocks.fs, 8 * 4);   // (the rect fragment block is 8 floats: Color, AlphaMix)
  }
  out.own.set = true;
  out.own.program = own_program;
  out.own.vertex_layout = layout_;
  out.own.topology = 3;   // VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
  out.own.count = 6;
  if (proj_block) out.own.vs_constants = reinterpret_cast<const uint8_t*>(vs_proj_), out.own.vs_bytes = sizeof(vs_proj_);   // S103 P3c
  else out.own.vs_constants = reinterpret_cast<const uint8_t*>(vs_), out.own.vs_bytes = sizeof(vs_);
  out.own.fs_constants = reinterpret_cast<const uint8_t*>(fs_), out.own.fs_bytes = fs_bytes;
  ++(glyph ? n_.glyphs : sprite ? n_.sprites : n_.rects);   // (S103 I3: a glyph is counted apart: the I2 census compares `sprites` with its sprite rows)
  return true;
}

std::string UiQuads::Report() const {
  char line[560];
  std::snprintf(line, sizeof(line),
                "quads: decisions %llu, sprites %llu, rects %llu; kept: variant %llu, geometry %llu, quad %llu, constants %llu, alpha-to-coverage %llu, texture %llu, projection (c28 set, a subset of constants) %llu; last quad %.0f,%.0f - %.0f,%.0f, glyphs %llu",
                (unsigned long long)n_.decisions, (unsigned long long)n_.sprites, (unsigned long long)n_.rects, (unsigned long long)n_.variant, (unsigned long long)n_.geometry,
                (unsigned long long)n_.quad, (unsigned long long)n_.constants, (unsigned long long)n_.alpha_to_coverage, (unsigned long long)n_.texture, (unsigned long long)n_.projection, n_.last_pos[0], n_.last_pos[1],
                n_.last_pos[2], n_.last_pos[3], (unsigned long long)n_.glyphs);
  return line;
}

}  // namespace rdr_scene
