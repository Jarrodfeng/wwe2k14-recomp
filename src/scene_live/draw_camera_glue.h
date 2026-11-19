// The draw thread's camera, the game plumbing (Session 103, lane WC, R5). Included by draw_origin.h. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// One call from the raw hook on sub_82C04DF0, beside the guest model reader's: reads the draw thread's camera exactly as sub_82BA7748 does (draw_camera_core.h). Read only; nothing is written to the guest.
#pragma once
#include <atomic>
#include <cstdint>

#include "../renderer_scene/live_scene.h"  // Enabled()
#include "draw_camera_core.h"
#include "wrestler_hook.h"  // ReadGuest

namespace wwe2k14::drawcam {

#if defined(_MSC_VER) || defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
inline void NoteDraw(uint32_t r13, uint64_t frame) {
  if (!rdr_scene::live::Enabled()) return;
  rdr_scene::live::drawcam::Cam().Note(&wwe2k14::wrestlers::ReadGuest, r13, frame);
}

}  // namespace wwe2k14::drawcam
