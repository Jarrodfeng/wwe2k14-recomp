// S105 A4B: what the scene host does with one 2D row of a record (moved out of scene_host.cpp so a unit can drive it; the rule is unchanged except the scene_on_f line). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pass (review (c)): no tap sets Quad2D::pass yet, so every row reads Unknown. Dropping them all would drop the whole layer, and drawing them all draws the rows that are not over the final image (the in-scene
// sprites before the post chain, the offscreen pages) onto F on a guess. A row of the final 2D layer is pixel-placed with PosScale (2 / w, 2 / h) of the game's screen (rdr_uiquad.vert: UnitScale), so an Unknown
// row is drawn only when it says so; the rig draws them all (HostOptions::draw_unknown_pass). A row the tap marks as scene colour / offscreen / other is dropped `pass` (P5 / U4).
#pragma once
#include <cmath>
#include <cstdint>

#include "../renderer_scene/frame_hooks.h"   // Scene2DSite
#include "../renderer_scene/quads2d_plants.h"
#include "../renderer_scene/scene_record.h"
#include "../renderer_scene/stage_lmpage.h"   // S103 SMX (IP): the own page switch

namespace wwe2k14::scene_host {

enum class Fate : uint8_t { Draw, DropPass, DropPassUnknown, InScene, PageRow };   // PageRow (S103 SMX): a stage light-map page row (IsStagePageRow): counted `page_rows`, never a dropped frame   // InScene (S103 SCS): a scene-colour row the scene hook draws inside the scene pass, never on F

inline bool Near(float v, float want) { return std::isfinite(v) && std::fabs(v - want) <= 0.015f * want; }

// scene_on_f (S105 A4B): a flat (TwoDOnly) frame has no 3D, so no scene pass drew the scene-colour rows: they are the whole content of the scene colour (Create a Title's belt), drawn on F in seq order over the black clear.
// (plant 71 scene_dropped_on_flat: the flag is ignored, the rows are dropped `pass` as before.)
inline Fate FateOf(const rdr_scene::Quad2D& r, bool draw_unknown, uint32_t w, uint32_t h, bool scene_hook, bool page_ctx, bool scene_on_f = false) {
  if (r.pass == rdr_scene::kQpFinal) return Fate::Draw;
  if (scene_on_f && !Q2D_PLANT(71) && r.pass == rdr_scene::kQpSceneColour && rdr_scene::Scene2DSite(r.site)) return Fate::Draw;
  if (r.pass == rdr_scene::kQpSceneColour && scene_hook && rdr_scene::Scene2DSite(r.site)) return Fate::InScene;   // S103 SCS: not drawn on F (the scene hook has them), counted in DrawRows from the hook's own report
  if (rdr_scene::IsStagePageRow(r, rdr_scene::stage_lmpage::On(), page_ctx)) return Fate::PageRow;
  if (r.pass != rdr_scene::kQpUnknown) return Fate::DropPass;
  if (draw_unknown) return Fate::Draw;
  const bool pixel_placed = (r.have & rdr_scene::kQhPosScale) && r.use_proj == 0.0f && w && h && Near(r.pos_scale[0], 2.0f / float(w)) && Near(r.pos_scale[1], 2.0f / float(h));
  return pixel_placed ? Fate::Draw : Fate::DropPassUnknown;
}

}  // namespace wwe2k14::scene_host
