// Quads2DBlocks (Session 103, lane I1; docs\renderer\REMNANT_MAP.md section 10). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The pure heart of the 2D drawer: a Quad2D row -> the two constant blocks the proven quad programs read (rdr_uiquad.vert + rdr_uiquad_flat.frag for the rect, rdr_movie.vert + rdr_movie.frag
// for the movie quad). Header-only and free of the renderer: UiQuads / MovieOurs call it under --quads-2d-rows (the offline rig), quads2d_draw.cpp's Append calls it for landing 2, and the
// unit test calls it directly. Same maths as the stream-constants paths of ui_quads.cpp / movie_ours.cpp (proven there byte-equal / within 1 level).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "../renderer/api.h"
#include "quads2d_corner.h"
#include "quads2d_plants.h"
#include "rows_quad2d.h"

namespace rdr_scene::q2d {

enum class Program : uint8_t { Rect, Movie, Sprite, SpriteHsl, Glyph, RectColor };   // S105 A8: RectColor = rdr_uiquad_vcol (a colour per corner)
// (S103 I3: Glyph = rdr_uiglyph.frag)   // S103 I3: Glyph = rdr_uiglyph.frag (the channel-selected text glyph)
constexpr uint8_t kGlyphPrimStrip = 6, kGlyphPrimQuadList = 13;   // S103 I3: the op-19 primitive words a glyph row may carry: 6 = the Xbox 360 D3DPT_TRIANGLESTRIP (four vertices in the Z order 0 1 2 3: the stream's draw is a Vulkan strip, [C] fview census: topology 4 on every glyph draw), 13 = a quad list (the sprite's, perimeter order); any other is refused [C: sub_82BC3518 passes 6, sub_82BC4890 13]
// The strip's four vertices in PERIMETER order (the order the vertex program's fixed triangles 0 1 2 / 0 2 3 expect): a parallelogram covers the same pixels either way.
constexpr int kStripToPerimeter[4] = {0, 1, 3, 2};   // S103 I2: Sprite = rdr_uiquad.frag (picture times colour), SpriteHsl = rdr_uiquad_hsl.frag (the same, then the game's hue shift)
// Why a row was not drawn (counted by the caller; a refused row is a hole, never a guess).
enum class Refuse : uint8_t { None, Layout, Kind, MissingVerts, MissingConstants, Projection, NotFinite, NotRect, Blend, Planes, Variant, Picture, Prim, Count };   // S103 I3: Prim = a glyph row whose primitive is not the strip / quad list the program draws   // S103 I2: Variant = a blur / gradation sprite (not drawn), Picture = the key resolves to no picture
inline const char* RefuseName(Refuse r) {
  static const char* const k[] = {"none", "layout", "kind", "no vertices", "no constants", "projection matrix", "non-finite", "not a rectangle", "blend unknown", "planes", "pixel variant", "picture", "primitive"};
  return uint8_t(r) < uint8_t(Refuse::Count) ? k[uint8_t(r)] : "?";
}

// The two constant blocks of a row, in the layout rdr_uiquad.vert / rdr_uiquad_flat.frag (rect) or rdr_movie.vert / rdr_movie.frag (movie) declare.
struct Blocks {
  Program program = Program::Rect;
  alignas(16) float vs[52] = {};   // rect: UnitScale, Pos[4], Uv[4] (36 floats); movie: the first 12 floats; S103 P3c: a MATRIX-PLACED row (proj) is 52 floats: + Mat[4] (rdr_uiquad_proj.vert)
  uint32_t vs_bytes = 0;           // rect 144, movie 48, matrix-placed 208
  bool proj = false;               // S103 P3c: the row is placed by its matrix (c28 != 0): drawn by the matrix variant of the vertex program (Pos = the draw's own vertex order, vs[2] = 1 for a strip, Mat = c29..c32 as uploaded)
  alignas(16) float fs[20] = {};   // rect / sprite: Color, AlphaMix (8 floats); hue-shifted sprite: + HSL (12 floats); glyph: + Selector (12 floats); movie: Color, RowR, RowG, RowB, AlphaMix (20 floats)
  uint32_t fs_bytes = 0;           // rect 32, movie 80
};






// The game's own conversion, BT.601 limited range (the ySprite yuv shader, docs\renderer\effects\ySprite.md; the same numbers as MovieOurs, proven against the stream there): three rows over (Y, Cb, Cr),
// each (a, b, c, offset). The green offset is the game's three constants summed.
inline constexpr float kBt601[3][4] = {{1.164f, 0.0f, 1.596f, -0.87075f}, {1.164f, -0.391f, -0.813f, 0.52925f}, {1.164f, 2.018f, 0.0f, -1.08175f}};

inline bool Finite(const float* f, int n) {
  for (int i = 0; i < n; ++i)
    if (!std::isfinite(f[i])) return false;
  return true;
}

// S103 P3c: the matrix variant's vertex block, from the pixel-placed one BlocksOf just built: Pos[] = the draw's own vertices in the DRAW's order (the strip's triangles are made by the shader: vs[2] = 1), Mat[k] = register 29 + k as
// uploaded (the shader: clip[k] = dot(Mat[k], (x y z 1)), the game's own program). A planted twin transposes the matrix (plant proj_transposed).
inline void PlaceByMatrix(const Quad2D& row, Blocks& out) {
  out.proj = true;
  out.vs[0] = out.vs[1] = out.vs[3] = 0.0f;
  out.vs[2] = (row.prim == kGlyphPrimStrip && !Q2D_PLANT(32)) ? 1.0f : 0.0f;
  for (int k = 0; k < 4; ++k)
    for (int c = 0; c < 4; ++c) out.vs[36 + 4 * k + c] = Q2D_PLANT(34) ? row.proj[4 * c + k] : row.proj[4 * k + c];
  out.vs_bytes = 52 * 4;
}

inline Refuse BlocksOf(const Quad2D& row, Blocks& out) {
  out = Blocks{};
  if (row.version != kQuad2DLayout) return Refuse::Layout;
  if (row.kind != kQkRect && row.kind != kQkYuvAlpha && row.kind != kQkYuv && row.kind != kQkSprite && row.kind != kQkGlyph && row.kind != kQkRectColor) return Refuse::Kind;
  if (row.nverts != 4 || !(row.have & kQhVerts)) return Refuse::MissingVerts;
  if ((row.have & (kQhColour | kQhProj)) != (kQhColour | kQhProj)) return Refuse::MissingConstants;
  // S103 P3c: c28 (fUseProjMatrix) != 0 places the quad by the matrix c29..c32: the matrix variant draws it, from the matrix the row carries; a matrix-placed row WITHOUT its matrix is refused (a hole), never guessed. The pixel unit constants (c0) are not read then.
  // The game's vertex program: clip[k] = dot(c(29 + k), (x y z 1)) [C: ySprite.vert / yRect.vert spritePosition / main, matProj register 29 count 4]; the pixel-unit path ((p - 0.5) * c0, y flipped) is NOT applied on top.
  const bool proj = row.use_proj != 0.0f && !Q2D_PLANT(35);   // (plant proj_c28_ignored: a matrix-placed row is drawn as a pixel-placed one)
  if (proj) {
    if (row.kind == kQkYuvAlpha || row.kind == kQkYuv) return Refuse::Projection;   // the movie quad's program has no matrix variant
    if (!(row.have & kQhMatrix)) return Refuse::Projection;
    if (!Finite(row.proj, 16)) return Refuse::NotFinite;
    if (row.kind == kQkSprite && (row.have & kQhHsl)) return Refuse::Variant;   // (no matrix variant of the hue-shifted program: no capture has one)
  } else if (!(row.have & kQhPosScale)) return Refuse::MissingConstants;
  if (row.kind == kQkSprite && (row.have & kQhVariant)) return Refuse::Variant;   // a blur / gradation sprite: another program, not this drawer's
  if (row.kind == kQkSprite && row.site == 0x82BC8BECu && (row.have & kQhHsl)) return Refuse::Variant;   // S103 P3b: the variant leaf's hue-shifted draw: no capture shows its program (a plain T_Sprite (technique 0x8206AA0C) only)
  if (row.kind == kQkGlyph && !(row.have & kQhChannel)) return Refuse::MissingConstants;   // S103 I3: no channel selector, no glyph
  // S103 P3c review: a matrix-placed rect / sprite row is a strip (the shader draws 0 1 2 / 2 1 3) or a quad list: any other primitive is refused, not drawn as a quad (the pixel-placed rows of the older sites keep their old rule)
  if ((row.kind == kQkGlyph || (proj && (row.kind == kQkRect || row.kind == kQkSprite))) && !Q2D_PLANT(17) && row.prim != kGlyphPrimStrip && row.prim != kGlyphPrimQuadList) return Refuse::Prim;
  if (row.kind == kQkRectColor && row.prim != kSiteRectColorPrimWord && !Q2D_PLANT(67)) return Refuse::Prim;   // S105 A8 review F1: the leaf passes the quad list (13) and the drawer lays out a quad list: any other primitive is refused (plant vcol_prim_any: the strip is accepted too)
  const int fl = row.kind == kQkRect ? 3 : 5;
  for (int v = 0; v < 4; ++v)
    if (!Finite(row.verts[v], fl)) return Refuse::NotFinite;
  if ((!proj && !Finite(row.pos_scale, 2)) || !Finite(row.color, 4) || !std::isfinite(row.alpha_ref)) return Refuse::NotFinite;   // (pos_scale z / w are never read: the leaf uploads two floats and the payload's rest is its stack)
  if ((row.have & kQhHsl) && !Finite(row.hsl, 3)) return Refuse::NotFinite;
  if (row.kind == kQkGlyph && !Finite(row.hsl, 4)) return Refuse::NotFinite;   // (the glyph's selector: all four floats)
  if (row.kind == kQkRectColor) {   // S105 A8: a colour per corner (the game's yRect vertex_color variant: out = vertexColour * Color); a quad list in perimeter order: the corners are taken as drawn
    out.program = Program::RectColor;
    float* vs = out.vs;
    vs[0] = row.pos_scale[0], vs[1] = row.pos_scale[1], vs[2] = 0.0f, vs[3] = Q2D_PLANT(6) ? 0.0f : 0.5f;
    for (int i = 0; i < 4; ++i) {
      const float* vert = row.verts[i];
      if (!CornerValid(vert[3], vert[4])) return Refuse::NotFinite;   // (the packed corner bytes: whole numbers in range)
      vs[4 + 4 * i + 0] = vert[0], vs[4 + 4 * i + 1] = vert[1], vs[4 + 4 * i + 2] = vert[2];
      UnpackCorner(vert[3], vert[4], vs + 20 + 4 * i);   // Col[i] = the corner's rgba, 0..1 (the same multiply the leaf made)
      if (Q2D_PLANT(66)) vs[20 + 4 * i + 3] = 1.0f;   // (plant vcol_alpha_dropped: every corner opaque)
    }
    out.vs_bytes = 36 * 4;
    std::memcpy(out.fs, row.color, 16);
    out.fs[4] = 1.0f, out.fs[5] = 0.0f, out.fs[6] = 1.0f, out.fs[7] = 0.0f;
    out.fs_bytes = 8 * 4;
    if (proj) PlaceByMatrix(row, out);
    return Refuse::None;
  }
  if (row.kind == kQkRect) {
    out.program = Program::Rect;
    float* vs = out.vs;
    vs[0] = row.pos_scale[0], vs[1] = row.pos_scale[1], vs[2] = 0.0f, vs[3] = Q2D_PLANT(6) ? 0.0f : 0.5f;   // w: the half unit the game takes off every position
    for (int i = 0; i < 4; ++i) {   // (the Uv block stays 0: the flat program reads none)
      const float* vert = row.verts[(row.prim == kGlyphPrimStrip && !Q2D_PLANT(32) && !proj) ? kStripToPerimeter[i] : i];   // S103 P3b (plant strip_order_ignored): the single T_Rect draw of sub_82BC4450 (0x82BC484C) is a 4-vertex strip (prim 6): perimeter order, like the glyph
      vs[4 + 4 * i + 0] = vert[0], vs[4 + 4 * i + 1] = vert[1], vs[4 + 4 * i + 2] = vert[2];
    }
    out.vs_bytes = 36 * 4;
    std::memcpy(out.fs, row.color, 16);
    out.fs[4] = 1.0f, out.fs[5] = 0.0f, out.fs[6] = 1.0f, out.fs[7] = 0.0f;   // AlphaMix: alpha as drawn; z: apply the alpha test (the shader skips it for "always")
    out.fs_bytes = 8 * 4;
    if (proj) PlaceByMatrix(row, out);
    return Refuse::None;
  }
  if (row.kind == kQkSprite || row.kind == kQkGlyph) {   // S103 I2: any four points (a rotated sprite is a quad); the uv per corner; picture times colour, then (hue-shifted program) the hue shift
    const bool glyph = row.kind == kQkGlyph;   // S103 I3: the glyph draws like a sprite (the quad, the uv, the picture, the colour) and its program also reads the channel selector
    const bool hsl = !glyph && (row.have & kQhHsl) != 0 && !Q2D_PLANT(11);   // (plant sprite_no_hsl: the hue constant dropped, the plain program draws)
    out.program = glyph ? Program::Glyph : hsl ? Program::SpriteHsl : Program::Sprite;
    float* vs = out.vs;
    vs[0] = row.pos_scale[0], vs[1] = row.pos_scale[1], vs[2] = 0.0f, vs[3] = Q2D_PLANT(6) ? 0.0f : 0.5f;
    for (int i = 0; i < 4; ++i) {
      const float* vert = row.verts[(row.prim == kGlyphPrimStrip && !Q2D_PLANT(32) && !proj) ? kStripToPerimeter[i] : i];   // (S103 I3: a strip's vertices are taken in perimeter order; S103 P3b: the plain T_Sprite (technique 0x8206AA0C) at 0x82BC8BEC is a strip too)
      vs[4 + 4 * i + 0] = vert[0], vs[4 + 4 * i + 1] = vert[1], vs[4 + 4 * i + 2] = vert[2];
      vs[20 + 4 * i + 0] = vert[3], vs[20 + 4 * i + 1] = vert[4];
    }
    out.vs_bytes = 36 * 4;
    std::memcpy(out.fs, row.color, 16);
    out.fs[4] = 1.0f, out.fs[5] = 0.0f, out.fs[6] = 1.0f, out.fs[7] = 0.0f;
    out.fs_bytes = 8 * 4;
    if (hsl) std::memcpy(out.fs + 8, row.hsl, 16), out.fs_bytes = 12 * 4;
    if (glyph) {
      if (!Q2D_PLANT(16)) std::memcpy(out.fs + 8, row.hsl, 16);   // (plant glyph_blocks_sel: the selector left 0)
      out.fs_bytes = 12 * 4;
    }
    if (proj) PlaceByMatrix(row, out);
    return Refuse::None;
  }
  // The movie quad: an axis-aligned rectangle with the uv corners to match (as MovieOurs checks), one depth.
  const float(*v)[5] = row.verts;
  const bool rect = v[0][0] == v[3][0] && v[1][0] == v[2][0] && v[0][1] == v[1][1] && v[2][1] == v[3][1] && v[0][3] == v[3][3] && v[1][3] == v[2][3] && v[0][4] == v[1][4] && v[2][4] == v[3][4] &&
                    v[0][2] == v[1][2] && v[0][2] == v[2][2] && v[0][2] == v[3][2];
  if (!rect) return Refuse::NotRect;
  out.program = Program::Movie;
  float* vs = out.vs;
  vs[0] = row.pos_scale[0], vs[1] = row.pos_scale[1], vs[2] = v[0][2], vs[3] = 0.5f;
  vs[4] = v[0][0], vs[5] = v[0][1], vs[6] = v[1][0], vs[7] = v[2][1];
  vs[8] = v[0][3], vs[9] = v[0][4], vs[10] = v[1][3], vs[11] = v[2][4];
  out.vs_bytes = 12 * 4;
  std::memcpy(out.fs, row.color, 16);
  std::memcpy(out.fs + 4, kBt601[0], 16);
  std::memcpy(out.fs + 8, kBt601[1], 16);
  std::memcpy(out.fs + 12, kBt601[2], 16);
  out.fs[16] = 1.0f, out.fs[17] = 0.0f, out.fs[18] = 1.0f, out.fs[19] = 0.0f;
  if (row.kind == kQkYuv) out.fs[16] = 0.0f, out.fs[17] = 1.0f;   // S103 SVY: no alpha plane: AlphaMix (0, 1, 1, 0): alpha = 1, then x Color (the game's FX_YUV: vec4(rgb * vColor.rgb, vColor.a))
  out.fs_bytes = 20 * 4;
  return Refuse::None;
}

inline bool BlendOf(uint8_t blend, rdr::BlendState& out) {
  constexpr uint8_t kOne = 1, kSrcAlpha = 6, kInvSrcAlpha = 7;   // VkBlendFactor
  out = rdr::BlendState{};
  out.write_mask = 0xF;
  switch (blend) {
    case kQbOff: return true;
    case kQbAlpha: if (Q2D_PLANT(7)) return BlendOf(kQbAdd, out); out.enable = 1, out.src_color = kSrcAlpha, out.dst_color = kInvSrcAlpha, out.src_alpha = kSrcAlpha, out.dst_alpha = kInvSrcAlpha; return true;   // (alpha factors: [U], REMNANT_MAP U6; the final picture's alpha is never read)
    case kQbAdd: out.enable = 1, out.src_color = kSrcAlpha, out.dst_color = kOne, out.src_alpha = kSrcAlpha, out.dst_alpha = kOne; return true;
    default: return false;
  }
}


}  // namespace rdr_scene::q2d
