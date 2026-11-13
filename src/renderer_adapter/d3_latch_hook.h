// lane 301 (SKIP_GAME_WORK_MAP.md 11.10): the D3 gate's latch line reaches the case ring. Run 7202's exit zip came 3 minutes after the latch and showed another scene; a case taken at the latch holds the moment
// (the ring's pre-trigger window). The app (wwe2k14_app.h) sets the pointer once bookmark.h's ring is installed; the live reader calls it on the render thread right after it logs a "[rdr][d3]... latched off" line.
// Unset (a player build, the ring off): nothing happens.
#pragma once

namespace wwe2k14 {
inline void (*g_d3_latch_hook)(const char* line) = nullptr;
}  // namespace wwe2k14
