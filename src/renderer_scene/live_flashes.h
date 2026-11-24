// Lane C (Session 103): the crowd camera flashes from the live record (docs\renderer\FLASH_OURS.md, docs\formats\crowd_flash.md). A flash row (kFxFlash: centre, size, colour,
// alpha) becomes one FlashSprite of SceneFrame::flashes: the arena pac's entry 31000 model (the 20-vertex star), turned to face the camera, scaled, with the row's own colour and alpha.
// S110 CUTGREY: it is NOT a scene object any more. FrontEnd::DrawFlashesAfterPost draws it onto the finished picture after the post chain, blended SrcAlpha + One, hidden whole when the scene depth at its
// centre is nearer, lit by the game's own rule (colour x (1 + abs(n.x))), as the game does (render category 52, docs\research\s110\CUTGREY.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include "rf_c.h"
#include "rows_stage_fx.h"   // S106 G4-2

namespace rdr_scene::rf::c {

// S103 C3: the factor a flash row scales the sprite model by (kFxfGameScale rows: the row's scale as is; others: scale / model radius).
// (The sprite's world, FlashWorld / FlashSpriteFacing, is in flash_sprite.h since S110 FLASHOCC.)
float FlashFactor(const StageFxRow& r, float model_radius);

}  // namespace rdr_scene::rf::c
