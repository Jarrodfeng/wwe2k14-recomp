// S102 lane L1b (docs/renderer/PRELIGHT_DIRECT.md section 9): the game's light cubes written by ONE compute dispatch (a DispatchOp, api.h) instead of five strip passes and thirty copies.
// The renderer's side: a compute pipeline (src/renderer/shaders/rdr_light_cubes.comp), one descriptor set per frame slot (the constants, five storage-image views), the cubes' layout changes in
// ONE barrier, one vkCmdDispatch of 8 x 8 x (6 per cube) groups. Offline instruments only: the globals below (fview / rplay flags); a player's frame never reads them but for one relaxed load.
#pragma once
#include <atomic>
#include <cstdint>

#include "api.h"

namespace rdr {

inline std::atomic<bool> g_force_no_compute{false};   // fview / rplay --no-compute: the renderer says it cannot dispatch (the fallback arm and its planted twin)
// Offline plants (fview / rplay --light-cube-plant N): 1 = the barrier between the dispatch and the readers is left out (the cube's state is kept as if it had run: validation must catch it),
// 2 = the face-swapped shader (view face 5 - f: the cube hashes must differ).
inline std::atomic<int> g_light_cube_plant{0};
inline constexpr uint32_t kLightCubeSize = 64;      // the cube's face edge (the planner proves it)
inline constexpr uint32_t kLightCubeGroups = 8;     // the shader's local size (8 x 8)

}  // namespace rdr
