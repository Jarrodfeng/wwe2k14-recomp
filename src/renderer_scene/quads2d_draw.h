// Quads2DDraw (Session 103, lane I1, landing 2; docs\renderer\REMNANT_MAP.md sections 5.3 and 10). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The drawer of the 2D layer: Quad2D rows (scene_record.h, section 58) -> draws over OUR frame, with the programs that are already proven against the stream (rdr_uiquad.vert +
// rdr_uiquad_flat.frag for the rect, rdr_movie.vert + rdr_movie.frag for the movie quad). A PURE function of the rows plus the pictures: BlocksOf (the two constant blocks of a row) has no
// state at all, Append (the draw ops of a run of rows) reads only its arguments and the picture resolver it is given. The offline rig calls BlocksOf in place of the stream's own constants
// (UiQuads / MovieOurs under --quads-2d-rows); landing 2 calls Append over the clean final picture. NOT wired into the live frame by this piece.
//
// Placement (both vertex programs): clip = ((x, y) - 0.5) * PosScale, y flipped (the game's fUseProjMatrix = 0 rule); the renderer's system block then maps the clip space to the target.
// Header-only apart from Register and the shader tables (quads2d_draw.cpp).
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "../renderer/frame_builder.h"
#include "../renderer/renderer.h"
#include "quads2d_blocks.h"
#include "scene_record.h"

namespace rdr_scene::q2d {

// The ids of the programs and the empty vertex layout (made once, with Register).
struct Programs {
  uint16_t rect = 0, movie = 0, layout = 0;
  uint16_t rect_color = 0, rect_color_proj = 0;   // S105 A8: the vertex-colour rectangle (rdr_uiquad_vcol.frag with rdr_uiquad_vcol.vert / rdr_uiquad_vcol_proj.vert): T_RectColor rows
  uint16_t rect_proj = 0, sprite_proj = 0, glyph_proj = 0;   // S103 P3c: the matrix variants (rdr_uiquad_proj.vert with the flat / picture / glyph fragment programs): the rows placed by their c29..c32 matrix
  uint16_t sprite = 0, sprite_hsl = 0, glyph = 0;   // S103 I3: glyph = rdr_uiglyph (the channel-selected text glyph; the vertex program is the sprite's)
  //   // S103 I2: rdr_uiquad (picture times colour), rdr_uiquad_hsl (+ the game's hue shift)
};
// S103 I3 review: `switch_on` = --wwe2k14_rdr_2d_record. Off registers NOTHING (a player with the switch off builds no extra shader module and no program id shifts); landing 2 calls this only when the switch is on.
Programs Register(rdr::Renderer& renderer, bool switch_on = false);

// The size of the target the rows are drawn into (pixels) and where the picture of a movie plane comes from.
struct Context {
  uint32_t width = 0, height = 0;
  // S105 A2 (ultrawide / tall screens): the rows' own pixel space is width x height (the game's 1280x720 box, scaled to the target), drawn with the viewport at (origin_x, origin_y) inside a target of
  // target_w x target_h pixels (0 = the rows' own size: today's behaviour, origin 0). A full-view flat row (a fade / flash / tint: wide_frame.h FullViewRow) gets the whole target. Scissors are offset by the origin.
  int32_t origin_x = 0, origin_y = 0;
  uint32_t target_w = 0, target_h = 0;
  // S103 SCS: the rows are drawn INSIDE the scene pass (the in-scene flashes / lens flares, RFF part B), onto a colour target that has our scene depth attached. Each row then takes the game's own depth state
  // (row.state: z enable 16 -> depth test, z write 32 -> depth write; the compare is our forward LEQUAL, the game's GEQUAL on its reversed depth), and a depth-tested row's clip z is flipped to our forward depth (a z-written row with the test off writes no depth on Vulkan: S105 A4B review F1; CAF's black rect works because it is drawn early, before the 3D)
  // (z' = w - z: the game's depth is the mirror 1 - z of ours, pass_state.h; the lens flare's z is the game's own projected depth, sub_827D5E98 +10C..+1A8 [L: the exact near / far equality is unconfirmed]).
  // Off (the default): the final-image F has no depth, every row's depth test stays off (as before).
  bool scene_depth = false;
  // The plane `slot` (0..3) of a movie row as a texture slot (image, view format, swizzle); the sampler is the drawer's. image == kNoImage: the row is refused as Refuse::Planes.
  std::function<rdr::TextureSlot(const Quad2D& row, int slot)> plane;
  // S103 I2: the picture of a sprite row (its key resolved by quads2d_pictures.h and uploaded by the caller). image == kNoImage: the row is refused as Refuse::Picture. The sampler is the drawer's (linear, clamped:
  // the UI quads are sampled 1:1 or scaled with the stream's default; S103 RFX: a sprite / hue / glyph row now carries the game's address code (Quad2D::address) and samples with it; the filter stays linear [U]).
  std::function<rdr::TextureSlot(const Quad2D& row)> picture;
  // S103 P7 H2: the scissor the game drew the row's draw with (true + `out` = clip to it; false / null = the whole target). S103 SC (layout 4): a row now carries its own scissor (kQhScissor / scissor_on / scissor[]),
  // which Append applies itself; this resolver (the offline host rig's capture-derived one) only applies to a row that has no scissor of its own, and a row's own scissor wins.
  std::function<bool(const Quad2D& row, rdr::Rect& out)> scissor;
};

struct Report {
  uint32_t scissor_empty = 0;   // S103 SC: rows skipped because their (guest-pixel) scissor has no area
  uint32_t drawn = 0, rect = 0, movie = 0, sprite = 0, sprite_hsl = 0, glyph = 0;
  uint32_t refused[uint8_t(Refuse::Count)] = {};
  int64_t first_refused = -1;           // S103 P7 H2: the index (into the rows given) of the first refused row, -1 = none; the caller names it in a census without asking the drawer a second time
  Refuse first_why = Refuse::None;      // ... and why
  uint32_t refused_total() const {
    uint32_t n = 0;
    for (uint32_t r : refused) n += r;
    return n - refused[uint8_t(Refuse::None)];
  }
};

// Appends one draw op per drawable row, in the order given (the rows are already in `seq` order), to the builder. The caller has set the targets (SetTargets) and owns the pass: this adds
// draws only. The viewport is the whole target; the scissor is the row's own (S103 SC: a menu list is clipped to its panel, the row's x y w h in the 1280x720 root scaled to the target; scissor off = the whole target); the depth test is off; the alpha test comes from the row.
// S103 SC: the recorded scissor / viewport / target size are GUEST pixels (the 1280x720 root of the rows for the frame target); the drawer scales the scissor to its target. The row's scissor as a rect of the target `width` x `height` (pure; the rows' pixels are the game's 1280x720 root): false = the row has none (the whole target); `out` is empty (width or height 0) when the box lies outside the target.
bool ScissorOf(const Quad2D& row, uint32_t width, uint32_t height, rdr::Rect& out);
// S103 RFX: the sampler address mode (VkSamplerAddressMode u / v) a sprite / hue / glyph row samples with, from the game's address code (see quads2d_draw.cpp); `cls` (optional) = the census class.
bool RowSetsAddress(const Quad2D& row);
void AddressModes(const Quad2D& row, uint8_t& u, uint8_t& v, uint8_t* cls = nullptr);
// The census line (rows by address mode since the last call; empty when none), reset on read. Printed after each [scene][2d] window line.
std::string AddressCensusLine();
Report Append(rdr::FrameBuilder& builder, const Programs& programs, const Context& ctx, const Quad2D* rows, size_t count);
// S103 L2a: the same per-row decision as Append (one shared function), without drawing: the live frame asks it first and draws the rows only when NONE is refused (a frame with one refused
// row keeps the stream's 2D whole: never half ours, half the stream's).
Report Check(const Context& ctx, const Quad2D* rows, size_t count);

}  // namespace rdr_scene::q2d
