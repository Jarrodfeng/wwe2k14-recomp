// wwe2k14 - Session 100: work a route's quit must wait for, shared without pulling the input script into the renderer adapter (and
// the other way round). The input script's quit waits while any counter here is above 0, as it does for its own screenshots.
#pragma once

#include <atomic>

namespace wwe2k14 {

// Named live pictures (--wwe2k14_rdr_live_picture) handed to the new renderer and not yet written: the renderer can be several frames
// behind a fast-forwarded game, so a route that quits right after its screenshot would end first (S100 run 6865).
inline std::atomic<int> g_live_pictures_in_flight{0};

}  // namespace wwe2k14
