// UiQuads: the UI's 2D quads (T_Sprite 0x82BC5688, T_Rect 0x82BC3EE8) drawn with our own quad and our own 2D program in the stream draw's place
// (Session 101, lane 148; docs\renderer\UI_QUADS_OURS.md; the seam: src\renderer_adapter\draw_replace_hook.h `own`, first used by MovieOurs, lane 131).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// UiQuads is NOT a hook: UiOurs (ui_ours.*) owns the picture (the join), and asks UiQuads to turn the same stream draw into an `own` draw. What stays the stream's: the draw's place,
// target, blend, depth, viewport, alpha test and its slot 0 picture (or ours, where UiOurs joined it). What is ours: the quad (four corners read from the stream draw's vertices, checked)
// and the two programs (rdr_uiquad.vert + rdr_uiquad.frag / rdr_uiquad_flat.frag). Every draw is checked against the shape this reading knows; a failure keeps the stream's draw, counted by reason.
//
// No game or SDK includes. Render thread only. No game files are read.
#pragma once
#include <cstdint>
#include <string>

#include "../renderer/renderer.h"
#include "../renderer_adapter/draw_replace_hook.h"

namespace rdr_scene {

inline constexpr uint32_t kUiQuadSpriteSite = 0x82BC5688u;   // sub_82BC4890's draw call (T_Sprite) [C: UI_PROMOTION_MAP.md 1.1]
inline constexpr uint32_t kUiQuadGlyphSite = 0x82BC3784u;    // S103 I3: sub_82BC3518's draw call (a glyph quad of a text run); taken ONLY under the offline row rig (--quads-2d-rows)
inline constexpr uint32_t kUiQuadRectStripSite = 0x82BC484Cu;   // S103 P3b: sub_82BC4450's single flat T_Rect (a 4-vertex strip); taken ONLY under the offline row rig
inline constexpr uint32_t kUiQuadSpriteMSite = 0x82BC8BECu;      // S103 P3b: sub_82BC8180's plain T_Sprite (technique 0x8206AA0C) (a 4-vertex strip); taken ONLY under the offline row rig
inline constexpr uint32_t kUiQuadRectSite = 0x82BC3EE8u;     // sub_82BC3C18's draw call (T_Rect) [C: UI_PROMOTION_MAP.md 1.1]

// The switch: fview / rplay --ui-quads, live --wwe2k14_rdr_ui_quads_ours (ui_quads_factory-less: read by MakeUiOurs). Default off.
inline bool g_ui_quads = false;

struct UiQuadFaults {
  bool wrong_uv_rect = false;       // the sprite's texture rectangle taken as its upper-left quarter (every picture zoomed)
  bool no_blend = false;            // the sprite's alpha forced to 1 (a blended sprite drawn as if the blend were off)
  bool fade_alpha_ignored = false;  // the rectangle's alpha forced to 1 (a fade drawn solid)
  bool half_pixel = false;          // the half-unit shift of the game's placement dropped
  bool proj_flag_forced = false;    // review S3: the projection flag treated as 1 (every draw refused as "constants")
  bool indices_permuted = false;    // review S3: the index check expects 0 2 1 0 3 2 (every draw refused as "quad")
  bool stride_plus_one = false;     // review S3: the guest stride one dword too big (refused by the stride / host layout check; with unchecked_ the check is off and the quad is wrong)
  bool alpha_test_ignored = false;  // review S3: the stream's alpha test not applied
  bool color_from_c2 = false;       // review S3: the colour read from the wrong constant row (the projection flag's); the picture check sees it
  bool unchecked = false;           // unchecked_stride_plus_one: the geometry checks only skipped (test only)
  static bool Parse(const std::string& name, UiQuadFaults& out);   // false: unknown name
};

class UiQuads {
 public:
  UiQuads(rdr::Renderer& renderer, const UiQuadFaults& faults);

  // The stream draw `in` as an own draw (out.own filled; nothing else of `out` is touched) when it is a sprite or a rectangle of the known shape. False: `why` says why, the counters say which check.
  bool Build(const rdr_adapter::ReplaceIn& in, rdr_adapter::ReplaceOut& out, rdr_adapter::KeepWhy& why);
  std::string Report() const;
  uint64_t replaced() const { return n_.sprites + n_.rects + n_.glyphs; }

 private:
  UiQuadFaults faults_;
  uint16_t program_sprite_ = 0, program_rect_ = 0, program_sprite_hsl_ = 0, program_glyph_ = 0, layout_ = 0;
  uint16_t program_rect_proj_ = 0, program_sprite_proj_ = 0, program_glyph_proj_ = 0;   // S103 P3c: the matrix variants (rdr_uiquad_proj.vert); made ONLY under the offline row rig
  alignas(16) float vs_proj_[52] = {};   // S103 P3c: UnitScale, Pos[4], Uv[4], Mat[4] (the matrix-placed row's vertex block)   // (S103 I2: program_sprite_hsl_; I3: program_glyph_)
  alignas(16) float vs_[36] = {};   // UnitScale, Pos[4], Uv[4]
  alignas(16) float fs_[12] = {};   // Color, AlphaMix (+ HSL for the hue-shifted sprite; S103 I2) (+ Selector for the glyph; S103 I3)
  struct Counts {
    uint64_t proj_taken = 0, proj_refused = 0;   // S103 P3c: matrix-placed draws taken as rows / kept (a register unreadable)
    uint64_t decisions = 0, sprites = 0, rects = 0, glyphs = 0, variant = 0, geometry = 0, quad = 0, constants = 0, alpha_to_coverage = 0, texture = 0, projection = 0;   // (S103 P3b: `projection` = draws whose c28 fUseProjMatrix is set: counted apart from `constants`, still kept)
    float last_pos[4] = {};   // x0 y0 x1 y1 of the last corners 0 and 2
  } n_;
};

}  // namespace rdr_scene
