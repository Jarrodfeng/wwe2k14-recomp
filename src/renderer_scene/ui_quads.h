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
  uint64_t replaced() const { return n_.sprites + n_.rects; }

 private:
  UiQuadFaults faults_;
  uint16_t program_sprite_ = 0, program_rect_ = 0, layout_ = 0;
  alignas(16) float vs_[36] = {};   // UnitScale, Pos[4], Uv[4]
  alignas(16) float fs_[8] = {};    // Color, AlphaMix
  struct Counts {
    uint64_t decisions = 0, sprites = 0, rects = 0, variant = 0, geometry = 0, quad = 0, constants = 0, alpha_to_coverage = 0, texture = 0;
    float last_pos[4] = {};   // x0 y0 x1 y1 of the last corners 0 and 2
  } n_;
};

}  // namespace rdr_scene
