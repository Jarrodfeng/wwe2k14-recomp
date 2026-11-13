// S101 lane 271 (docs\renderer\SKIP_GAME_WORK_MAP.md 19.8): the arena the game's match card names, published for the live renderer's loads. No game or SDK includes.
//
// The in-game sequence starts ~1.5 s before the first frame that draws the arena (7174: match card 13:54:18.2, first record 13:54:19.75). session_log.h reads the card's arena id
// then and stores it here; live.cpp hands it to every class that loads per arena (ArenaOurs, RingOurs, CrowdOurs, ...::Announce) once per change, so their workers start while the
// game is still loading instead of after the first record. -1 = none (not in a match); the value is a hint: a class whose record names another arena discards the early load.
#pragma once

#include <atomic>

namespace wwe2k14 {

inline std::atomic<int> g_announced_arena{-1};

}  // namespace wwe2k14
