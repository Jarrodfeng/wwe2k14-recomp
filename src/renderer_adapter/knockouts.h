// S102 lane KO: three test-only knock-out switches for the Surface (step S0-5). Each removes ONE layer's work in Bryan's entrance so one run per
// switch gives the ceiling that layer could reach today. They rank lanes; they never cancel a goal. No game or SDK includes (rplay uses it too).
//
//   wwe2k14_rdr_ko_draw_none    PerFrame  our renderer records no scene draws and no copies: RenderMode::NoPasses (renderer.h). Parse, contents
//                                         and bind still run, the present still runs (a black / stale picture). Ceiling of the GPU side.
//                                         Safe to flip inside one run (A/B windows): contents, image table and state all still flow.
//   wwe2k14_rdr_ko_render_drop  PerFrame  live.cpp's render thread drops each frame the moment it leaves the hand-off: no image table, no parse,
//                                         no contents, no bind, no record, no present (the last picture stays on screen). The reader still reads
//                                         and serialises. Ceiling of our render thread. NOT for A/B windows: image tables / texture contents of
//                                         dropped frames never reach the binder, so switching it OFF again gives a broken picture, and the
//                                         store keeps the dropped frames' texture bytes (memory grows ~0.6 MB a frame in Bryan: keep the run short).
//   wwe2k14_rdr_ko_reader_none  PerFrame  the reader on the game's swap thread reads nothing: every reader hook (draws, inline vertices, clears,
//                                         resolves, texture create / lock / unlock, float flush, gamma, present) is skipped through
//                                         rdr_reader::ReaderOn(); nothing is serialised or handed off. The game still records its own commands and
//                                         its swap / present / vblank / fences are answered as today. Ceiling of the reader's share of the swap
//                                         thread. Taken at the PRESENT hook (after the reader's own OnPresent), so the reader never stops in the
//                                         middle of a frame. NOT for A/B windows (the sink misses everything while it is on): use it ON from boot.
//
// Default off. Read through the S0-4 table (rdr_switches.h): PerFrame, logged "[switch] frame N NAME=VALUE" and "[switch] seen ..." by the read site.
// `[ko] <name> on` is logged once per switch by the read site (KoOnce). With a switch off nothing here runs beyond one relaxed load.
#pragma once
#include <atomic>
#include <cstdint>

namespace ko {

// The reader's gate. Set only at the present hook (swap thread), read by every reader hook (same thread, or the render thread never).
inline std::atomic<bool> g_reader_none{false};

// Logs "[ko] <name> on" the first time a switch is seen on. `slot` 0 draw_none, 1 render_drop, 2 reader_none. `log` is any printf-style sink the caller owns.
template <typename LogFn>
inline void KoOnce(int slot, const char* name, LogFn&& log) {
  static std::atomic<bool> said[3];
  if (slot >= 0 && slot < 3 && !said[slot].exchange(true, std::memory_order_relaxed)) log(name);
}

}  // namespace ko
