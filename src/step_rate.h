// wwe2k14 - game step rate (Session 19 item 10): run the game's fixed per-update time step at N updates per game
// second instead of 60 (native 120 / 144 / 165 fps with a matching vblank, or true slow motion when presented at 60).
//
// The frame-rate word 0x8392DC40 and its step fields live in the timing block 0x8392DC38
// (docs/systems/frame_pacing.md; Session 8 run 276 snapshot of the block). Its three writers, listed in Session 19:
//   sub_82BB6BB0 set(rate): only 25 / 30 / 50 / 60 (any other rate keeps the old fields);
//   sub_82BB6D50 halve: 60 -> 30 and 50 -> 25, otherwise returns at once;
//   sub_82BB6DF0 restore: 30 -> 60 and 25 -> 50 unless the lock flag +12 is set, otherwise returns at once.
// Fields (stored by those functions, values from the listing / the S8 snapshot): +8 word (int), +0x1C seconds per
// update (float), +0x20 rate (float), +0x24 ms per update (float, 1000 / rate), +0x28 ms per update (int,
// 1000 / word), +0x3C 60 / rate (tick scale, 2.0 at 30), +0x40 rate / 60 (0.5 at 30).
//
// With --wwe2k14_step_hz=N (N >= 60; 0 = off, the game's own rate): the three writers are replaced by wrappers that
// put the game's own word back into +8, run the original (so its own 60 / 30 logic decides), remember the game word
// G it left there, and rewrite every field for the scaled rate S = G * N / 60. So matches step 1/N s and stock 30 fps
// entrances 2/N s (with --wwe2k14_entrance_60fps the halve is skipped and entrances step 1/N s too). The end-of-frame
// code (sub_82BA84E8) sets the present interval 2 only when the word is 30, so any S >= 60 presents every vblank;
// pair N with gpu_vblank_rate_override_hz=N for real time, or leave the vblank at 60 for N/60x slow motion.
//
// Live changes: the cvar can change mid-run (`cvar wwe2k14_step_hz 120` in a script); the next presented frame
// rewrites the fields from the remembered game word. 0 restores the game's own fields for G.
//
// Known limits (Session 19 study): the match clock 0x836F3928 is +1 per update (sub_827C1B28), so anything that
// counts updates as 1/60 s (that clock, frame-count timers) runs N/60x fast in game seconds. Hooks: REX_HOOK_RAW on
// the three writers; per-frame apply from VdSwapOverride (kernel_overrides.h).
#pragma once

#include <rex/cvar.h>
#include <rex/det.h>  // Session 27.5: det_frames_time_hz
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "decode_probe.h"  // GuestPtr
#include "frame_stats.h"
#include "load_turbo.h"  // g_load_turbo_ingame_window

REXCVAR_DEFINE_INT32(wwe2k14_step_hz, 0, "wwe2k14",
                     "Session 19 item 10 (experiment, default 0 = the game's own rate): game updates per game second. "
                     "N >= 60 (or exactly 30: the game's own 30 fps mode, for faster fast-forward) makes each update "
                     "step 1/N s (matches) or 2/N s (stock 30 fps entrances). Pair with "
                     "gpu_vblank_rate_override_hz=N for real time; at a 60 Hz vblank it is N/60x slow motion. "
                     "Live-switchable. See src/step_rate.h");
REXCVAR_DEFINE_BOOL(wwe2k14_step_hz_hold_loads, true, "wwe2k14",
                    "Session 19 item 10: with wwe2k14_step_hz, keep the game's own rate while an in-game loading window "
                    "is open (the load turbo's ingame / match windows; needs --wwe2k14_load_turbo, default on). The "
                    "match arena is black when the end of the in-game load runs at 120 (runs 1414-1422)");
REXCVAR_DEFINE_INT32(wwe2k14_step_hz_entrance, 0, "wwe2k14",
                     "Session 19 item 10: with wwe2k14_step_hz, use this rate instead while the entrances run (scene "
                     "word 0x836F3940 == 4); 0 = the same as wwe2k14_step_hz. E.g. 165 in matches, 120 in entrances "
                     "(heavy entrance shots could not hold 144 / 165, runs 1431 / 1432)");
REXCVAR_DEFINE_BOOL(wwe2k14_replay_time_follows_step, true, "wwe2k14",
                    "Session 27.5: in replay mode (det_frames) the guest's time per frame follows the update rate (SDK "
                    "det_frames_time_hz), so videos, sound and timed waits stay in step with the scene at any step "
                    "rate; false = 1/60 s per frame as in Session 15 (videos run N/60x ahead at step N)");
REXCVAR_DEFINE_BOOL(wwe2k14_step_vblank, false, "wwe2k14",
                    "Session 19 item 10: with wwe2k14_step_hz, set the SDK's gpu_vblank_rate_override_hz to the scaled "
                    "rate whenever the step is applied (and back to 0 = the video mode's 60 Hz while it is held or "
                    "off), so N fps runs in real time; false = leave the vblank alone (N/60x slow motion at 60 Hz)");

REXCVAR_DEFINE_DOUBLE(wwe2k14_ff_vblank_hz, 4000.0, "wwe2k14",
                      "Session 60: with vsync off (fast-forward), during the match (in game, not the entrances, not a "
                      "load window, not replay mode) run the guest vblank at this rate on the precise timer (SDK "
                      "gpu_vblank_unthrottled_hz) instead of 1 kHz Sleep(1) polling: the command thread waited ~25% "
                      "of its time for the next vblank (profile 4082); match 284 -> 365 fps (4076 / 4084). 0 = off");

REX_EXTERN(__imp__sub_82BB6BB0);
REX_EXTERN(__imp__sub_82BB6D50);
REX_EXTERN(__imp__sub_82BB6DF0);

namespace wwe2k14::step_rate {

constexpr uint32_t kBlock = 0x8392DC38;
constexpr uint32_t kWord = kBlock + 8;

inline std::atomic<uint32_t> g_game_word{0};   // the game's own word (60 / 30 / ...), 0 = not seen yet
inline std::atomic<int32_t> g_applied_hz{0};   // the step_hz value the fields were last written for
// True while the game is in its in-game sequence (load_turbo.h: [[0x839400F8]] + 48 == 9), refreshed every presented
// frame. The step applies only in game: boot and menus keep the game's own rate (at 144 the boot's frame-counted
// waits ran past the routes' limits, runs 1428 / 1429, and menu animations would run fast).
inline std::atomic<bool> g_in_game{false};
inline std::atomic<bool> g_entrances{false};  // scene word 0x836F3940 == 4 (load_turbo.h, states.toml scene_word)

inline uint32_t LoadU32(uint8_t* base, uint32_t addr) {
  const uint8_t* p = decode_probe::GuestPtr(base, addr);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void StoreU32(uint8_t* base, uint32_t addr, uint32_t v) {
  uint8_t* p = const_cast<uint8_t*>(decode_probe::GuestPtr(base, addr));
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
inline void StoreF32(uint8_t* base, uint32_t addr, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  StoreU32(base, addr, v);
}

inline int32_t ActiveHz() {
  int32_t n = REXCVAR_GET(wwe2k14_step_hz);
  int32_t n_ent = REXCVAR_GET(wwe2k14_step_hz_entrance);
  if (n_ent >= 60 && n >= 60 && g_entrances.load(std::memory_order_relaxed)) n = n_ent;
  // N >= 60, or exactly 30 (Session 19 follow-up: the game's own 30 fps mode as a coarse step for faster
  // fast-forward; the word 30 also turns on the game's 30-fps paths and a present interval of 2).
  if ((n < 60 && n != 30) || !g_in_game.load(std::memory_order_relaxed)) return 0;
  if (REXCVAR_GET(wwe2k14_step_hz_hold_loads) && g_load_turbo_ingame_window().load(std::memory_order_relaxed)) {
    return 0;
  }
  return n;
}

// Writes the fields for game word g scaled to step rate hz (hz 0 = unscaled, the game's own values for 25-60).
inline void WriteFields(uint8_t* base, uint32_t g, int32_t hz) {
  if (g == 0) return;
  uint32_t s = hz ? uint32_t((uint64_t(g) * uint32_t(hz) + 30) / 60) : g;
  if (s < 25) s = g;  // e.g. a stock 30 fps entrance at step 30 would be 15: not a rate the game knows
  const float rate = float(s);
  StoreU32(base, kWord, s);
  StoreF32(base, kBlock + 0x1C, 1.0f / rate);
  StoreF32(base, kBlock + 0x20, rate);
  StoreF32(base, kBlock + 0x24, 1000.0f / rate);
  StoreU32(base, kBlock + 0x28, 1000u / s);
  StoreF32(base, kBlock + 0x3C, 60.0f / rate);
  StoreF32(base, kBlock + 0x40, rate / 60.0f);
}

// With --wwe2k14_step_vblank: the guest vblank follows the update rate. The present interval is 1 at any scaled word
// >= 60, so one update per vblank: vblank rate = scaled rate (0 = the video mode's rate while held or off).
inline void ApplyVblank(uint32_t g, int32_t hz) {
  if (!REXCVAR_GET(wwe2k14_step_vblank)) return;
  const uint32_t s = hz ? uint32_t((uint64_t(g) * uint32_t(hz) + 30) / 60) : 0;
  rex::cvar::SetFlagByName("gpu_vblank_rate_override_hz", std::to_string(s));
}

// Wrapper body shared by the three writers: run the original on the game's own word, then rescale.
template <typename F>
inline void Wrap(F Original, PPCContext& ctx, uint8_t* base) {
  const int32_t hz = ActiveHz();
  const uint32_t g = g_game_word.load(std::memory_order_relaxed);
  if (!hz && !g_applied_hz.load(std::memory_order_relaxed)) {
    Original(ctx, base);
    return;
  }
  if (g) StoreU32(base, kWord, g);  // the original's own 60 / 30 tests see the game's word
  Original(ctx, base);
  const uint32_t g2 = LoadU32(base, kWord);
  if (g2 != g) {
    REXLOG_INFO("[wwe2k14][step] game word {} -> {} (step_hz {}) frame {}", g, g2, hz,
                g_guest_frame.load(std::memory_order_relaxed));
  }
  g_game_word.store(g2, std::memory_order_relaxed);
  if (hz) WriteFields(base, g2, hz);
  if (g2 != g || hz != g_applied_hz.load(std::memory_order_relaxed)) ApplyVblank(g2, hz);
  g_applied_hz.store(hz, std::memory_order_relaxed);
}

// Session 27.5: in replay mode (det_frames) one det vblank stands for 1/60 s of guest time (Session 15), but at a step
// rate above 60 each frame is one det vblank and only 1/N s of game time, so the guest's clocks (Bink's movie timer,
// audio, timed waits) ran N/60x ahead of the scene. The SDK's det_frames_time_hz sets the time per det vblank; keep
// it at the frame-rate word as it is now (twice the word for 30 / 25, which present every other vblank). At the
// game's own rates it stays 60, so replays at step 60 are unchanged. Called at the end of every OnFrame.
inline int32_t g_det_time_hz = 60;
inline void SyncDetTime(uint8_t* base) {
  if (!rex::det::Enabled() || !REXCVAR_GET(wwe2k14_replay_time_follows_step)) return;
  const uint32_t w = LoadU32(base, kWord);
  if (w < 25 || w > 1000) return;  // block not set up yet
  const int32_t hz = (w == 30 || w == 25) ? int32_t(2 * w) : int32_t(w);
  if (hz == g_det_time_hz) return;
  g_det_time_hz = hz;
  rex::cvar::SetFlagByName("det_frames_time_hz", std::to_string(hz));
  REXLOG_INFO("[wwe2k14][step] replay mode: guest time {} Hz per frame from frame {} (word {})", hz,
              g_guest_frame.load(std::memory_order_relaxed), w);
}

// Session 60 (wwe2k14_ff_vblank_hz): the faster fast-forward vblank in the match. Checked every 30 frames (the vsync
// and rate settings are read by name); only a rate this code set is ever replaced, so a command-line
// gpu_vblank_unthrottled_hz stays.
inline double g_ff_vblank_applied = 0.0;
inline bool g_ff_vblank_foreign = false;  // gpu_vblank_unthrottled_hz was set by someone else: leave it alone
inline void ApplyFastForwardVblank(uint64_t frame) {
  if (frame % 30 || g_ff_vblank_foreign) return;
  const double want = REXCVAR_GET(wwe2k14_ff_vblank_hz);
  const bool ff = want > 0.0 && g_in_game.load(std::memory_order_relaxed) &&
                  !g_entrances.load(std::memory_order_relaxed) &&
                  !g_load_turbo_active_flag().load(std::memory_order_relaxed) &&
                  !g_load_turbo_ingame_window().load(std::memory_order_relaxed) && !rex::det::Enabled() &&
                  rex::cvar::GetFlagByName("vsync") == "false";
  const double target = ff ? want : 0.0;
  if (target == g_ff_vblank_applied) return;
  if (g_ff_vblank_applied == 0.0) {
    const std::string now = rex::cvar::GetFlagByName("gpu_vblank_unthrottled_hz");
    if (!now.empty() && std::strtod(now.c_str(), nullptr) != 0.0) {
      g_ff_vblank_foreign = true;
      return;
    }
  }
  rex::cvar::SetFlagByName("gpu_vblank_unthrottled_hz", std::to_string(target));
  REXLOG_INFO("[wwe2k14][step] fast-forward vblank {} -> {} Hz at frame {}", g_ff_vblank_applied, target, frame);
  g_ff_vblank_applied = target;
}

// Once per presented frame (VdSwapOverride): applies a changed step_hz. Also learns the game word on the first
// frame (boot has already called the setter by then; the word is 60).
inline void OnFrameInner(uint8_t* base, uint64_t frame);
inline void OnFrame(uint8_t* base, uint64_t frame) {
  OnFrameInner(base, frame);
  SyncDetTime(base);
  ApplyFastForwardVblank(frame);
}
inline void OnFrameInner(uint8_t* base, uint64_t frame) {
  auto valid = [](uint32_t a) { return a >= 0x40000000u && a < 0xE0000000u; };
  const uint32_t p1 = LoadU32(base, 0x839400F8u);
  const uint32_t p2 = valid(p1) ? LoadU32(base, p1) : 0;
  g_in_game.store(valid(p2) && LoadU32(base, p2 + 48) == 9, std::memory_order_relaxed);
  g_entrances.store(LoadU32(base, 0x836F3940u) == 4, std::memory_order_relaxed);
  const int32_t hz = ActiveHz();
  const int32_t applied = g_applied_hz.load(std::memory_order_relaxed);
  if (hz == applied) return;
  uint32_t g = g_game_word.load(std::memory_order_relaxed);
  if (!g) {
    g = LoadU32(base, kWord);
    if (g != 25 && g != 30 && g != 50 && g != 60) return;  // block not set up yet
    g_game_word.store(g, std::memory_order_relaxed);
  }
  WriteFields(base, g, hz);
  g_applied_hz.store(hz, std::memory_order_relaxed);
  ApplyVblank(g, hz);
  REXLOG_INFO("[wwe2k14][step] step_hz {} -> {} at frame {}: game word {}, word now {}", applied, hz, frame, g,
              LoadU32(base, kWord));
}

}  // namespace wwe2k14::step_rate

REX_HOOK_RAW(sub_82BB6BB0) { wwe2k14::step_rate::Wrap(__imp__sub_82BB6BB0, ctx, base); }
REX_HOOK_RAW(sub_82BB6D50) { wwe2k14::step_rate::Wrap(__imp__sub_82BB6D50, ctx, base); }
REX_HOOK_RAW(sub_82BB6DF0) { wwe2k14::step_rate::Wrap(__imp__sub_82BB6DF0, ctx, base); }
