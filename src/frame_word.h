#pragma once
// Session 73: the frame time the game is aiming for right now, for the code that judges frames against a budget
// (dynamic_res.h, android_perf_hint.h). The game's own frame-rate word (0x8392DC40: 60, or 30 in entrances with
// "60 matches, 30 entrances" and everywhere with the 30 fps choice), as written after step_rate.h's scaling, published
// once per presented frame by step_rate::OnFrame. Before the word is set up: --wwe2k14_step_hz=30 means 30, else 60.
#include <rex/cvar.h>

#include <atomic>
#include <cstdint>

namespace wwe2k14 {

inline std::atomic<uint32_t> g_frame_word{0};

inline double TargetFrameMs() {
  const uint32_t word = g_frame_word.load(std::memory_order_relaxed);
  if (word >= 25 && word <= 1000) return 1000.0 / double(word);
  return rex::cvar::GetFlagByName("wwe2k14_step_hz") == "30" ? 1000.0 / 30.0 : 1000.0 / 60.0;
}

}  // namespace wwe2k14
