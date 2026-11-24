// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {


// S103 lane C (docs/renderer/FRAME_CONTRACT.md 8; lane C owns ids 38-41, but 40 and 41 are the crowd run markers above (S102 T19), so lane C uses 38 and 39 only).
// 38 kScModelLights: per-model light sets as the game's rig rewrote them this frame (the entrance rig rewrites a model's sets every frame) and the set each source mesh uses
//    (FrontEnd ModelLights). 39 kScStageFx: the stage's live parts in one table by kind: screen pictures (the tron / wall / mini screens), the crowd camera flashes, and the
//    evaluated world of a moving arena slot. Both optional; a record without them is byte-identical to today's; an unknown layout drops THAT section only (c_errors).
inline constexpr uint32_t kScModelLights = 38;
inline constexpr uint32_t kScStageFx = 39;
inline constexpr uint8_t kModelLightsLayout = 1, kStageFxLayout = 1;
enum ModelLightKind : uint8_t { kMlSet = 0, kMlMesh = 1 };
struct ModelLightRow {
  uint8_t kind = 0;      // ModelLightKind
  uint8_t layout = kModelLightsLayout;
  uint16_t mesh = 0;     // kMlMesh: the model's source mesh (the front-end's Mesh::src order)
  uint32_t asset = 0;    // index into kScAssets (Model): the model the row is for
  int32_t set = 0;       // kMlSet: the set's game id (LightSet::id); kMlMesh: index among this asset's kMlSet rows in table order, -1 = the file's choice
  uint32_t flags = 0;    // kMlSet: bit 0 = pcol valid (the cube is built from the previous frame's colours)
  float dir[12] = {}, col[12] = {}, sky[3] = {}, ground[3] = {}, fall[4] = {}, wt[4] = {}, hl_dir[3] = {}, hl_col[3] = {}, pcol[12] = {};
};
static_assert(sizeof(ModelLightRow) == 240, "ModelLightRow is a flat POD table");
enum StageFxKind : uint8_t { kFxScreen = 1, kFxFlash = 2, kFxSlotWorld = 3, kFxShow = 4, kFxVisMark = 5, kFxVisSkip = 6, kFxCar = 7, kFxSlotPalette = 8 };   // S110 BARRICADE: kFxSlotPalette = one NODE of a posed stage slot's palette ([model + 200], stage_posed.h): slot, movie = node index, flags = the slot's node count, pad = the model, world = the node's rows. S103 CAR: kFxCar = a created arena (200-249) stage's part indices (car_arena_rows.h). S103 WSL: kFxVisMark = one per frame while a stage exists (seq = stage generation, pad = stage, flags & 1 = the mask is valid, slot = count); kFxVisSkip = a slot the game's look gate skips (stage_slot_vis.h). S103 SCF: kFxShow = the game's show bit of a screen key 800-809 or an art slot 301-304 (stage_show.h; slot = the key / slot, flags & 0x200 = shown)
struct StageFxRow {
  uint8_t kind = 0;      // StageFxKind
  uint8_t layout = kStageFxLayout;
  uint16_t slot = 0;     // kFxScreen: the arena key 800-809; kFxSlotWorld: the stage slot
  uint32_t movie = 0xFFFFFFFFu;  // kFxScreen: index into kScMovies (where the picture comes from)
  uint32_t seq = 0;      // kFxScreen: the picture's serial (a new number = new pixels: the feed is asked for them)
  uint32_t w = 0, h = 0; // kFxScreen: the picture's size
  uint32_t flags = 0;
  float pos[3] = {};     // kFxFlash: the centre (the game's space)
  float scale = 0.f;     // kFxFlash: size
  float color[4] = {1, 1, 1, 1};  // kFxFlash: colour (rgb, w = the flash's life fraction 0..1)
  float alpha = 1.f;     // kFxFlash: c185.x
  float world[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // kFxSlotWorld: VS c76..c78 as the packer cuts them (ArenaWorld::world's rows)
  uint32_t pad = 0;      // S103 C2: kFxSlotWorld: the slot's model object (guest address; the cross-check's key against kScArenaWorlds), appliers ignore it
};
static_assert(sizeof(StageFxRow) == 112, "StageFxRow is a flat POD table");
// S103 C3: StageFxRow::flags of a kFxFlash row (the live writer, src\scene_live\flash_fx_core.h). Appliers test only kFxfGameScale; the rest is census / provenance.
// S103 E7: a kFxScreen row with this flag (w = h = 0) says the game plays no movie: the screen shows the arena file's own picture (the applier drops its live texture).
inline constexpr uint32_t kFxfScreenIdle = 0x100;
enum StageFxFlashFlags : uint32_t {
  kFxfGameScale = 1,    // scale is the game's multiplier of the sprite model's own coordinates (item +40), not a world half-size
  kFxfDrawDone = 2,     // the item's done flag was set by the draw's own last-frame mark (counter == life)
  kFxfLightSet = 4,     // the game takes the flash colour from light set 500 (the row colour is the manager's block, a stand-in)
  kFxfColorUnread = 8,  // the colour block was not readable: white
  kFxfHeld = 16,        // S110 FLASHOCC: shown from the flash hold (flash_hold_core.h), not the row the game gave this frame (census only)
};

}  // namespace rdr_scene
