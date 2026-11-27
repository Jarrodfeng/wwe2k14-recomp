// Lane C (Session 103): see live_flashes.h. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#include "live_flashes.h"
#include "flash_sprite.h"    // S110 FLASHOCC: FlashWorld / FlashSpriteFacing
#include "rows_stage_fx.h"   // S106 G4-2

#include <cmath>
#include <cstring>

namespace rdr_scene::rf::c {

// S103 C3: the uniform factor that multiplies the sprite model's own coordinates. A live row (kFxfGameScale) carries the game's multiplier (item +40, flash_fx_core.h) and is used as is;
// an older / synthetic row carries a world half-size, divided by the model's extent.
float FlashFactor(const StageFxRow& r, float model_radius) {
  if (r.flags & kFxfGameScale) return r.scale;
  return model_radius > 0.f ? r.scale / model_radius : r.scale;
}

bool ApplyFlashes(BuildCtx& ctx) {
  if (!(ctx.families & (1u << uint32_t(Family::Flashes)))) return true;
  uint32_t rows = 0;
  for (const StageFxRow& r : ctx.t.stage_fx) rows += r.kind == kFxFlash;
  if (!rows) {
    Report(ctx, Family::Flashes, 0, 0, 0, "");
    return true;
  }
  if (PlantsRef().ignore_flashes) {
    Report(ctx, Family::Flashes, rows, 0, rows, "flashes ignored (planted)");
    return true;
  }
  std::string why;
  bool miss = false;
  ArenaSet* as = EnsureArena(ctx, why, miss);
  if (miss) return false;
  if (!as || as->part[size_t(Part::Flash)].model < 0) {
    Report(ctx, Family::Flashes, rows, 0, rows, as ? "arena " + std::to_string(as->arena) + " has no flash model (key 31000)" : why);
    return true;
  }
  const FamilyModel& fm = as->part[size_t(Part::Flash)];
  uint32_t given = 0, own_colour = 0, file_colour = 0, light_set = 0;
  for (const StageFxRow& r : ctx.t.stage_fx) {
    if (r.kind != kFxFlash) continue;
    float p[3];
    PointToViewer(r.pos, p);
    // S110 FLASHOCC (FLASHDEPTH.md b8): the sprite faces the FRAME's eye (the viewer's: y negated at record_frame.cpp before any applier), never the record's game-space eye.
    FlashSprite s = FlashSpriteFacing(ctx.frame, p, FlashFactor(r, fm.radius), fm.model);
    if (r.flags & kFxfKeepCensus) {   // S111 FLASHCAM review (census only, src\scene_live\flash_keep.h Stamp): the game's keep rule and the slot camera's +176 scales of the row's own latch
      int32_t keep = -1;
      std::memcpy(&keep, &r.pad, 4), std::memcpy(&s.game_xs, &r.w, 4), std::memcpy(&s.game_ys, &r.h, 4);
      s.game_keeps = int8_t(keep);
    }
    // S110 CUTGREY: a flash is a sprite of its own, drawn AFTER the post chain (FrontEnd::DrawFlashesAfterPost; the game draws it after its combine, crisp), never a scene object: our far blur,
    // haze and sepia must not run over it. Its colour is the row's (the game's colour block of this flash, kFxfColorUnread = the block was not readable: the file's own colour, white,
    // counted), its alpha the row's (c185.x). No arena light set (the game's light is colour x (1 + abs(n.x)), applied by the draw).
    s.guest_frame = uint64_t(ctx.t.header.guest_frame), s.seq = r.seq, s.slot = r.slot, s.held = (r.flags & kFxfHeld) != 0, s.scale = r.scale;   // S110 FLASHOCC: census only
    const bool unread = (r.flags & kFxfColorUnread) != 0;
    for (int i = 0; i < 3; ++i) s.rgb[i] = unread ? 1.f : r.color[i];
    s.alpha = r.alpha;
    own_colour += unread ? 0u : 1u, file_colour += unread ? 1u : 0u, light_set += (r.flags & kFxfLightSet) ? 1u : 0u;
    ctx.frame.flashes.push_back(s);
    ++given;   // S110 RDRPREP F3: sprites GIVEN to the frame (FrontEnd::DrawFlashesAfterPost may skip some: behind the camera, no mesh, no sampled depth; FrontEnd::FlashLine counts what it drew)
  }
  Report(ctx, Family::Flashes, rows, given, rows - given,
         "given to the after-post draw (not counted as drawn here: see the flashes census line); colour from the row " + std::to_string(own_colour) + ", file colour (unread) " + std::to_string(file_colour) + ", light set 500 rows (colour is the manager's block, a stand-in) " + std::to_string(light_set));
  return true;
}

}  // namespace rdr_scene::rf::c
