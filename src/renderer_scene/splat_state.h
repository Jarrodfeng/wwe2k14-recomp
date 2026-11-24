// The floor blood splats' draw state and the front end's per-object depth-write rule (Session 108, lane BLD2; docs\research\s108\BLD_plan.md sections 1 and 10). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: rf_props.cpp sets the splat's object flags with SplatObjectFlags, front_end.cpp decides every mesh's depth write with DepthWriteOf; tools\weapons\gbr_test.cpp checks both (planted twin splat_depth_write).
// [C] the game's splat node sub_82698770 (0x82698898..0x826988E0, the recorder wrappers of docs\research\s103\T2_render_states.md): z enable 1, z WRITE 0 (sub_82BB7AD8 = slot 18), blend 1 (code 0), alpha test 1, GEQUAL, ref 1.
#pragma once
#include <cstdint>

#include "scene_record.h"

namespace rdr_scene::splat_state {

// A splat: blended by its record's alpha over an opaque material, and no depth write (the game's). plant_old = the twin (the old flags: force-blend alone).
inline uint32_t SplatObjectFlags(bool plant_old = false) {
  return kObjForceBlend | (plant_old ? 0u : uint32_t(kObjNoDepthWrite));
}

// The front end's depth write of one mesh draw: an opaque (unsorted) draw writes depth, a sorted one does not unless it is a prop cut-out drawn blended in Game order (S103 F); an object the game draws with z write off never does.
inline bool DepthWriteOf(uint32_t object_flags, bool sorted, bool game_order_cutout) {
  return !(object_flags & kObjNoDepthWrite) && (!sorted || game_order_cutout);
}

}  // namespace rdr_scene::splat_state
