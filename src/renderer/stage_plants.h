// S102 R21-B (docs\renderer\STAGE_SPLIT.md): the renderer's own test plants for the two-stage render thread. Set only by rplay (--stage-plant NAME);
// the game never sets them. Std only.
#pragma once
#include <atomic>

namespace rdr {

// split_no_release_defer: DestroyImage / DestroyBuffer are processed at the next Render (no kDestroyDelayFrames). With the split, an image released
// after frame N's bind is then destroyed by frame N's own Render before N is recorded (hazard 5's twin).
inline std::atomic<bool> g_plant_destroy_now{false};
// split_no_producer: SetProducerThread does nothing, so the bind stage waits for staging room only the record stage frees (hazard 6's twin: U1's stall).
inline std::atomic<bool> g_plant_no_producer{false};

}  // namespace rdr
