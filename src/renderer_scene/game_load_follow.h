// S110 ENTRBLACK round 5 (docs\research\s110\ENTRBLACK.md "Round 5"): our scene loads start when the GAME's loads start, never later; the game never waits for them.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. No game or SDK includes (the unit tools\renderer\host_core_unit.cpp drives it).
//
// The arena pac the game opens during its loading screen (pac\bg\bgNN.pac, kernel_overrides.h LogFileOpen, game thread) is handed to the host, which asks the
// loader for that arena through the existing ask path (LiveSceneDraw::QueueMisses -> rf::c::WantArenaFromGame -> g_wanted -> TakeArenaMisses). One slot: the newest open
// wins; a slot the host never takes is overwritten by the next open (no "asked" state is kept here: the loader's own g_asked / FailArena bookkeeping is the only one).
#pragma once
#include <atomic>
#include <cctype>
#include <cstdint>
#include <string_view>

namespace rdr_scene::loadfollow {

#ifdef WWE2K14_HOST_PLANTS
inline int g_plant = 0;   // test only: 1 = any .pac path names arena 0 (the parser checks nothing)
#define LOAD_FOLLOW_PLANT(n) (::rdr_scene::loadfollow::g_plant == (n))
#else
#define LOAD_FOLLOW_PLANT(n) false
#endif

inline std::atomic<int> g_game_arena{-1};   // the arena pac the game opened last, not yet taken by the host

// The arena of a game file path ("D:\PAC\BG\BG04.PAC": any case, either slash): 0..99, else -1. The last 12 characters must be SEP "bg" SEP "bg" D D ".pac".
inline int ArenaOfGamePath(std::string_view p) {
  if (p.size() < 12) return -1;
  const std::string_view s = p.substr(p.size() - 12);
  const auto sep = [](char c) { return c == '\\' || c == '/'; };
  const auto is = [](char c, char w) { return char(std::tolower(static_cast<unsigned char>(c))) == w; };
  const auto dig = [](char c) { return c >= '0' && c <= '9'; };
  if (LOAD_FOLLOW_PLANT(1)) return is(s[9], 'p') && is(s[10], 'a') && is(s[11], 'c') ? 0 : -1;
  if (!sep(s[0]) || !is(s[1], 'b') || !is(s[2], 'g') || !sep(s[3]) || !is(s[4], 'b') || !is(s[5], 'g') || !dig(s[6]) || !dig(s[7]) || s[8] != '.' || !is(s[9], 'p') ||
      !is(s[10], 'a') || !is(s[11], 'c'))
    return -1;
  return (s[6] - '0') * 10 + (s[7] - '0');
}

// Game thread (kernel_overrides.h LogFileOpen, a successful open).
inline void NoteGameOpen(std::string_view path) {
  const int a = ArenaOfGamePath(path);
  if (a >= 0) g_game_arena.store(a, std::memory_order_release);
}
// Host thread (LiveSceneDraw::QueueMisses): the arena the game opened since the last call, or -1.
inline int TakeGameArena() { return g_game_arena.exchange(-1, std::memory_order_acq_rel); }

}  // namespace rdr_scene::loadfollow
