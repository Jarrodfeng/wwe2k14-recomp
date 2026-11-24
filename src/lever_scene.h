#pragma once
// S110 FLASHHOLD (docs\research\s110\FLASH60.md): "the game would be at 30 fps here". True from our skip of the scene's switch to 30
// (the halve call at 0x82331778, pacing_hooks.h) until the next restore call at any of its 15 sites (pacing::Restore; not while the lock flag is set).
// A per-update effect the game does not compensate for the frame-rate word (the crowd flash's life) holds its steps by HoldFactor (the game glue,
// stage_fx_glue.h LeverHoldFactor, gives it the live values). Pure: no game or SDK includes (the offline unit tools\scene\flash_fx_unit.cpp table-tests it).
#include <atomic>
#include <cstdint>

namespace wwe2k14 {

inline std::atomic<bool> g_lever_scene{false};

// S110 FLASHHOLD review F2: k = round(presented updates per second / the game's own rate for this scene), at least 1.
//  * The game's own rate: 30 when the game would run this scene at 30 = our skip flag (the word kept at 60), or the game's own word is 30 / 25 while our step
//    rate scales it (applied_hz != 0: step_rate.h g_game_word is the game's word under the scaling); else 60 (a word of 30 without our scaling is the game's own 30: k = 1).
//  * Presented: frame_word = the word after our scaling (frame_word.h g_frame_word: 60, 120, ...; the real rate when step 30 keeps the word at 60).
// Our skip at 60 -> 2, at step 120 -> 4; a stock 30 scene scaled to 60 -> 2; a real 60 match at step 120 -> 2; a plain 60 match -> 1.
inline uint32_t HoldFactor(bool lever, uint32_t game_word, uint32_t frame_word, int32_t applied_hz) {
  const bool scaled_low = applied_hz != 0 && (game_word == 30 || game_word == 25);
  const uint32_t own = lever ? 30u : scaled_low ? game_word : 60u;
  const uint32_t k = (frame_word + own / 2) / own;
  return k > 1 ? k : 1u;
}

}  // namespace wwe2k14
