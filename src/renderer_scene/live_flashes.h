// Lane C (Session 103): the crowd camera flashes from the live record (docs\renderer\FLASH_OURS.md, docs\formats\crowd_flash.md). A flash row (kFxFlash: centre, size, colour,
// alpha) becomes one sprite object: the arena pac's entry 31000 model (the 20-vertex star), turned to face the camera, scaled, drawn blended SrcAlpha + One through the front-end's
// ordinary see-through path (far to near, depth test on). The game's flash also tests the scene depth at the flash's centre in the vertex stage and takes its colour from a 4-entry
// table: here the depth test is the ordinary one of the sprite and the colour is the file's (the per-flash colour table is not fed: [U], listed in FLASH_OURS.md "Lane C").
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include "rf_c.h"

namespace rdr_scene::rf::c {

// The world of a flash sprite at `pos` (viewer space) of size `scale`, facing `eye`: rows right, up, toward-eye, position (row vectors), `up_hint` = the world's up.
// S103 C3: the factor a flash row scales the sprite model by (kFxfGameScale rows: the row's scale as is; others: scale / model radius).
float FlashFactor(const StageFxRow& r, float model_radius);

assets::Matrix FlashWorld(const float pos[3], float scale, const float eye[3]);

}  // namespace rdr_scene::rf::c
