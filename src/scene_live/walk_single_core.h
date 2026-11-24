// S110 WALKSINGLE (probe note docs\research\s110\WALKRACE.md): the game records its render categories on TWO workers at once in a match (the main thread walks key 0, CParallelDraw1 walks key 1, by the
// range table 0x83625190 that the shared setter sub_82BB4128 stores at [manager + 140]); the ONE shared 2D layout record [0x8392DD80] is saved / reset / set by ~100 draw functions with no lock, so the
// 30 Years captions jump to half their position when the two walks interleave. The game's own single-worker mode is a NULL range table: key 0 then records every category in ascending order (boot and
// story sequences run this way). THE FIX: in the stream-free build, when the setter is asked to store the match table for the manager G = [0x8392D9B8], store 0 instead.
// Pure: no game or renderer include, so tools\scene\walk_single_unit.cpp compiles it. The hooks are in walk_single.h.
#pragma once

#include <atomic>
#include <cstdint>

namespace wwe2k14::walk1 {

inline constexpr uint32_t kRangeTable = 0x83625190u;    // the match's range table (WALKRACE W3 / W4); passed by sub_827B2068 +80 and by sub_827B2050's tail call (0x827B2060), both on G
inline constexpr uint32_t kManagerWord = 0x8392D9B8u;   // [kManagerWord] = G, the draw manager whose +140 holds the table and +136 the live one (WALKRACE W3)

inline std::atomic<bool> g_on{false};   // wwe2k14_rdr_walk_single (stored once at start by start_settings.cpp; the stream-free build only)

// census (walk_single.h prints them every 600 frames)
inline std::atomic<uint64_t> g_refused{0};         // stores of the match table that were turned into 0
inline std::atomic<uint64_t> g_caption_builds{0};  // caption matrix rebuilds (anchor set)
inline std::atomic<uint64_t> g_unscaled{0};        // of those, built while the record's scale mode word was not 1
inline std::atomic<uint32_t> g_table_seen{0};      // the last [G + 136] read at a caption build (0x83625190 = the two-worker table is live, 0 = single)

// The value the setter stores: r4 unchanged except the match table asked for the manager itself, which becomes 0. r3 == g is the exact object test: the setter is shared with other classes' +140 setters.
inline uint32_t Decide(bool on, uint32_t r3, uint32_t r4, uint32_t g) {
  if (on && r3 == g && g != 0 && r4 == kRangeTable) return 0;
  return r4;
}

}  // namespace wwe2k14::walk1
