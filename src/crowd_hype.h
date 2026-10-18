// wwe2k14 - crowd hype pin (Session 14, stretch goal S4; prototype, default OFF).
//
// The crowd excitement level 0x835E1988 (0..5; -1 in the load image) is read by the crowd's random reaction triggers
// (sub_823B1B78, sub_823B2E08: db/rng.json, Session 13) and written by the game as clamp(base + delta, 0, 5) with
// base = 0x836F3A84 (sub_82258F58, sub_8240C020 / 0A8 / 120, sub_824DEC68; a director shot adds 2, Session 13). Every
// writer first checks the word 0x836F3A94 and does nothing while it is non-zero (listings; set / cleared only by the
// timed crowd event object sub_82761008 / sub_827611E0). The countdown 0x835E198C is set by sub_823B2CC0 /
// sub_823B2E08 / sub_823B3838 / sub_823B1B78.
//
// --wwe2k14_crowd_hype=N (0..5; -1 = off): once per frame at the start of the owner update (sub_82C302A0, main
// thread, called from scenario.h) the level AND the base are written as N, so the game's own "base + delta" writes
// land near N and the next frame puts N back. The lock word 0x836F3A94 is NOT touched (the crowd code reads it in four
// more places: sub_823B12E8, sub_823B1880, sub_823B2470, sub_823B2E08; what it gates there is unknown). Logged every
// 600 frames: how many frames the game had changed the level since our last write (= how much it pushes back).
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstdint>

#include "frame_stats.h"

REXCVAR_DEFINE_INT32(wwe2k14_crowd_hype, -1, "wwe2k14",
                     "Prototype (Session 14, S4): pin the crowd excitement level 0x835E1988 (and its base 0x836F3A84) "
                     "to N = 0..5 once per frame; -1 = off (the game's own level). See src/crowd_hype.h");

namespace wwe2k14::crowd {

constexpr uint32_t kLevel = 0x835E1988, kBase = 0x836F3A84, kLock = 0x836F3A94, kCountdown = 0x835E198C;

inline uint8_t* G(uint32_t a) { return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(a); }
inline int32_t Get(uint32_t a) { return int32_t(rex::memory::load_and_swap<uint32_t>(G(a))); }
inline void Set(uint32_t a, int32_t v) { rex::memory::store_and_swap<uint32_t>(G(a), uint32_t(v)); }

struct State {
  int32_t pinned = -1;
  int32_t base_before = 0;  // the base when the pin started, put back on unpin (Session 14, run 1061)
  uint64_t frames = 0, changed = 0, last_report = 0;
  int32_t last_seen = -99;
};
inline State& S() {
  static State s;
  return s;
}

// Main thread, once per frame (owner update).
inline void Tick() {
  const int32_t want = REXCVAR_GET(wwe2k14_crowd_hype);
  State& s = S();
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (want < 0 || want > 5) {
    if (s.pinned >= 0) {
      // The game writes the level as clamp(base + delta): leaving our base in place kept the crowd at the pinned
      // value after unpinning (run 1061). Put the base back and the level to it.
      Set(kBase, s.base_before);
      Set(kLevel, s.base_before < 0 ? 0 : (s.base_before > 5 ? 5 : s.base_before));
      REXLOG_INFO("[wwe2k14][crowd] pin off (was {}); base and level back to {} frame={}", s.pinned, s.base_before,
                  frame);
      s.pinned = -1;
    }
    return;
  }
  const int32_t level = Get(kLevel);
  if (s.pinned != want) {
    if (s.pinned < 0) s.base_before = Get(kBase);
    s.frames = s.changed = 0;  // counts per pin value
    REXLOG_INFO("[wwe2k14][crowd] pin {} (level was {}, base {}, lock {}, countdown {}) frame={}", want, level,
                Get(kBase), Get(kLock), Get(kCountdown), frame);
    s.pinned = want;
  } else if (level != want) {
    ++s.changed;  // the game wrote the level since our last write
  }
  ++s.frames;
  Set(kLevel, want);
  Set(kBase, want);
  if (frame >= s.last_report + 600) {
    s.last_report = frame;
    REXLOG_INFO("[wwe2k14][crowd] pinned {}: {} frames, the game changed the level in {} of them; lock {} countdown {} "
                "frame={}",
                want, s.frames, s.changed, Get(kLock), Get(kCountdown), frame);
  }
}

}  // namespace wwe2k14::crowd
