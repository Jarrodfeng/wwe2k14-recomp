// S111 MATLIVE (docs\research\s111\MATLIVE.md): the game's model-create scale. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: asset_load.h LoadBloodEntry calls ApplyCreateScale on the splat model before FinishModel; tools\assets\splatscale_unit.cpp checks it (planted twin: no_create_scale).
// The crowd's paths read kCrowdCreateScale (below) for their own x 10.
// [C] The model-create record's init sub_82BD2068 stores 10.0 (image word 0x82192140) at record +0. The model create sub_82BDB460 copies the 76-byte record to model +528 and,
// when record +0 is not 1.0, scales every vertex position of the model data by it (sub_82BD6030: x, y, z each times the value; the normal is only renormalised) and every
// node's bind translation (sub_82BD8D60, the +16 triple times the value), both once per model data. The stage object setup sub_824ECAD8 creates the blood splat model
// (bg key 32000) with that record unchanged, so the game draws the file's 2 x 2 quad at y -0.1 as a 20 x 20 quad at y -1 (the v0.3.3 stream's splat draws: vertices +-10, -1).
// Ours loaded the file's floats as they are: every splat was a tenth of the game's size on screen (a 5 x 1 px speck where the game draws a 50 x 9 px blob).
#pragma once
#include "asset_model.h"

namespace assets::create_scale {

// The value sub_82BD2068 puts at record +0 (the model-create default): the scale a model gets when its creator leaves the record alone.
inline constexpr float kDefaultCreateScale = 10.f;

// The scale per family, from each family's creator (S111 MATLIVE review, CREATESCALE). Only families whose creator keeps the default are x 10:
//   the blood splat (bg key 32000, sub_824ECAD8): the default (LoadBloodEntry, through ApplyCreateScale);
//   the crowd (bodies, skeleton, audience props and signs: sub_8250A200, sub_825074C8, sub_82507408, sub_8250ACA8, sub_8250B1E8): the default,
//     applied by the crowd's own paths as a factor (kCrowdCreateScale: asset_load.h's crowd and stands placement, crowd_ours.cpp and live_crowd.cpp
//     kBodyScale, crowd_prop_game_space.cpp ConvertJboy), not through ApplyCreateScale;
//   key 5000 and the crowd flash (key 31000): 1.0 (sub_824ECAD8 stores 1.0 over record +0 after the init): ours loads them raw.
inline constexpr float kCrowdCreateScale = kDefaultCreateScale;

// TEST ONLY (tools\assets\splatscale_unit.cpp --plant no_create_scale): the rule before MATLIVE (the file's floats unscaled). The game never does this.
inline bool g_plant_no_create_scale = false;

// The game's create step on a loaded model (FILE space, before FinishModel): positions and bone bind translations times `s`; s == 1 changes nothing (the game skips it too).
inline void ApplyCreateScale(Model& m, float s) {
  if (g_plant_no_create_scale || s == 1.f) return;
  for (Mesh& me : m.meshes)
    for (Vertex& v : me.vertices)
      for (float& c : v.pos) c *= s;
  for (Bone& b : m.bones)
    for (float& c : b.local_translation) c *= s;
}

}  // namespace assets::create_scale
