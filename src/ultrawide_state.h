// S105 A2: the in-game verdict of the screen-shape rule (sequence word 9), published by ultrawide.h OnFrame (the game's VdSwap, both builds) and read by the stream-free
// host's poster (renderer_host\host_start.cpp). A separate header with no cvar so two translation units can include it. Pure: no game or SDK includes.
#pragma once

#include <atomic>

namespace wwe2k14::ultrawide {

// true = the in-game sequence: the scene and the host frame take the screen's shape (wide_frame.h); false = the game's own 16:9 (menus, Create modes).
inline std::atomic<bool> g_fill_now{false};
inline bool FillNow() { return g_fill_now.load(std::memory_order_relaxed); }

}  // namespace wwe2k14::ultrawide
