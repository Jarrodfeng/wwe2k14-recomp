// wwe2k14 - load turbo (Session 12). DEFAULT ON (default ON since Session 12, Jarrod 2026-10-11 after the 95/95 roster sweep): _load_turbo,
// _boot and _menu; =false turns each window off.
//
// Why: most of what is left of a loading screen is counted in frames (run 853: the loader thread's per-frame waits,
// staged object setup 2079 / 2087 / 2114 / 2132, the logic thread sleeping 90% of each frame), so the loads end
// sooner when the guest frame clock runs faster. ReXGlue reads its `vsync` setting on every pass of the guest vblank
// thread (graphics_system.cpp): false = a 1 kHz vblank instead of 60 Hz, so the game's frame loop runs as fast as
// the PC allows. Experiment (input-script `cvar vsync false` around the loads, fast loader on, runs 854 / 855):
// launch loading screen 3.60 -> 1.04 s, match load 3.80 -> 1.10 s, boot 18.4 -> 4.8 s, main-menu load 9.4 -> 1.3 s.
//
// This file does it automatically, only inside known loading windows (each ends at the first sign of something to
// watch or play, and after wwe2k14_load_turbo_cap_ms of wall time at the latest):
//   --wwe2k14_load_turbo=true        in-game windows:
//       "ingame": the sequence id becomes 9 (in-game; states.toml [seq], chain 0x839400F8>0>48) -> the first of: a
//                 movie opens (any path with \movies\: titantrons, cutscene videos), slot 0's act becomes the
//                 entrance act 25000 (cutscene_motion.md, chain 0x836F38D0>28>0, high 16 bits), M3.PAC opens
//                 (-> "match"), the match clock runs ([0x836F3928] != 0).
//       "match":  the scene word goes 4 -> 1 in the in-game sequence (entrances over, the match loading screen comes
//                 up; [0x836F3940], run 859) or, as a fallback, M3.PAC opens -> slot 0's first real motion (act word
//                 not 0 / -1: just before the loading screen fades out), else the match clock, a movie, leaving the
//                 in-game sequence, or no M3.PAC within 240 frames of a scene-word start. Jarrod (run 858): ending
//                 at the match clock sped up the match's first second, starting at M3.PAC missed ~0.5 s of the
//                 loading screen.
//   --wwe2k14_load_turbo_boot=true   "boot": first guest frame -> SaveData.Dat opens (title screen data). Fast-forwards
//                 the publisher logos and the title movie too.
//   --wwe2k14_load_turbo_menu=true   "menu": SaveData.Dat opens again more than 30 frames after the title screen's
//                 own read (the title press loads the save) -> MNUIHD.PAC opens (the main menu).
// --wwe2k14_load_turbo_ui_real_time (default true; only matters inside a window): the 2D layout animation step
//   sub_82BBEC18 (applies the pose at time +144, then time += speed +148, loops; called per sprite per frame by the
//   CMenuSprite update sub_823E5A50 and the menus) runs at most once per 1/60 s of wall time per animation while a
//   window is on, so the spinning WWE logo turns at its normal speed (Jarrod's ask, 2026-10-11); skipped calls leave
//   the element at its last pose.
// vsync is only turned off when it is on, and put back at the end of every window. One line per window:
// "[wwe2k14][turbo] NAME on at frame F" / "... off at frame G (reason, frames, ms)".
// Session 27.5: with the turbo off (replay mode turns it off) but --wwe2k14_step_hz set and its hold on
// (--wwe2k14_step_hz_hold_loads, step_rate.h), the in-game windows are still tracked, "hold only": the window flag the
// step rate's hold reads is set, nothing else (vsync, vblank rate, skipped draws untouched) and no wall-clock cap
// (it would end the window at a different frame every run). Before, replay mode ran the pre-entrance load at the high
// step and every stage video screen stayed black (runs 1896-1899, 2032; 2034 with the turbo forced on: screens play).

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>

#include <rex/hook.h>
#include <rex/ppc/context.h>

#include "frame_stats.h"
#include "guest_read.h"

REXCVAR_DEFINE_BOOL(wwe2k14_load_turbo, true, "wwe2k14",
                    "Run the guest frame clock unthrottled (vsync off) during in-game loading screens: in-game "
                    "sequence start -> titantron / entrance / match, and M3.PAC -> match clock (Session 12)");
REXCVAR_DEFINE_BOOL(wwe2k14_load_turbo_boot, true, "wwe2k14",
                    "Load turbo from the first frame to the title screen data (fast-forwards the logos and the title "
                    "movie too)");
REXCVAR_DEFINE_BOOL(wwe2k14_load_turbo_menu, true, "wwe2k14",
                    "Load turbo from the title press (save data load) to the main menu");
REXCVAR_DEFINE_INT32(wwe2k14_load_turbo_cap_ms, 8000, "wwe2k14",
                     "Load turbo: longest a window may run (wall-clock ms)");
// Session 15 (sub-second loads): with vsync off ReXGlue's guest vblank runs at a fixed 1 kHz on Sleep(1) polling, and
// each loading-screen frame waits for ~2 vblanks (the CP's WAIT_REG_MEM on the game's vblank-driven pacing word
// 0x137FE006 ends at a vblank; run 1152: CP busy 0.5 ms, wrm 1.2 ms, frame 2.1 ms). With this > 0 a window turns vsync
// off as before and also sets the SDK's gpu_vblank_unthrottled_hz (patch 0008) to this rate, so the vblanks come on
// the precise timer at that rate. 0 = 1 kHz (Session 12). (First try, build s15_vbl: vsync on + the vsync rate
// override: no gain, the host present waits took over.) DEFAULT 16000 since Session 15b (Jarrod, 2026-10-13); 0 = 1 kHz.
REXCVAR_DEFINE_DOUBLE(wwe2k14_load_turbo_vblank_hz, 16000.0, "wwe2k14",
                      "Load turbo: > 0 = inside a window also run the vsync-off guest vblank at this rate (Hz, SDK "
                      "gpu_vblank_unthrottled_hz) instead of 1 kHz; 0 = 1 kHz (Session 12)");
// Session 15 (sub-second loads): with the vblank floor gone (above), the host GPU bounds the in-game windows (run
// 1178: GPU busy 1.88 ms per frame, 77% of wall; a loading-screen frame = ~406 small draws + 23 resolves), yet at
// ~400 guest frames per second only about one frame in seven can reach the screen. N > 1 = inside the in-game
// windows set the SDK's gpu_skip_frame_draws (patch 0009) to N: the GPU work of N-1 of every N frames is skipped
// (memexport draws excepted). The SDK logs any resolve destination written only in skipped frames (a possible lost
// one-off render) when the window ends. 0 = off.
REXCVAR_DEFINE_INT32(wwe2k14_load_turbo_skip_draws, 0, "wwe2k14",
                     "Load turbo: N > 1 = inside the in-game windows draw only 1 of every N frames (SDK "
                     "gpu_skip_frame_draws); 0 = off (Session 15 experiment)");
REXCVAR_DEFINE_BOOL(wwe2k14_load_turbo_ui_real_time, true, "wwe2k14",
                    "Load turbo: keep 2D layout animations (the loading screen's spinning logo, menu sprites) at their "
                    "normal speed while a turbo window runs");

// Session 27.5: the step rate's settings (defined in step_rate.h), for the hold-only windows.
REXCVAR_DECLARE(int32_t, wwe2k14_step_hz);
REXCVAR_DECLARE(bool, wwe2k14_step_hz_hold_loads);

namespace wwe2k14 {

inline std::atomic<bool>& g_load_turbo_active_flag() {
  static std::atomic<bool> active{false};
  return active;
}
// Session 20 (perf_report.h): called when an in-game window ("ingame" = pre-entrance load, "match") ends, with its
// length in ms; null = nobody listens.
inline void (*g_load_window_done)(const std::string& window, double ms) = nullptr;

// Session 15 (A1): true while an in-game window ("ingame" or "match") is open, whether or not vsync had to be turned
// off; the fast loader's scope "turbo" (load_probe.h) runs only then.
inline std::atomic<bool>& g_load_turbo_ingame_window() {
  static std::atomic<bool> open{false};
  return open;
}

// Session 36 (src\scene_freeze.h): true while the scene is frozen. The 2D layout animation step sub_82BBEC18 (below)
// then does nothing, so overlays such as an entrance's nameplate hold their pose and carry on from it on release.
inline std::atomic<bool>& g_ui_anim_hold() {
  static std::atomic<bool> hold{false};
  return hold;
}

class LoadTurbo {
 public:
  void OnFrame(uint64_t frame) {
    if (!AnyOn() && !HoldOnly()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame == 1 && REXCVAR_GET(wwe2k14_load_turbo_boot) && !boot_done_) Start("boot", frame);
    uint32_t v = 0;
    const bool seq_ok = Chain(0x839400F8u, {0, 48}, &v);
    const uint32_t seq = seq_ok ? v : 0;
    if (InGameWindows() && seq_ok && seq == 9 && last_seq_ != 9 && window_.empty()) {
      Start("ingame", frame);
    }
    if (seq_ok) last_seq_ = seq;
    if (seq_ok) g_in_game_sequence.store(seq == 9, std::memory_order_relaxed);  // Session 73 (keep-warm)
    // scene word (states.toml [scene_word]): 1 -> 4 while the entrances are set up, 4 -> 1 when they are over and the
    // match loading screen comes up (run 859: frame 3546, loading screen visible 3536-3650, M3.PAC 3598).
    uint32_t scene = 0;
    const bool scene_ok = Read(0x836F3940u, &scene);
    if (InGameWindows() && scene_ok && scene == 1 && last_scene_ == 4 && window_.empty() && seq_ok && seq == 9) {
      Start("match", frame);
      match_by_scene_ = true;
      m3_seen_ = false;
    }
    if (scene_ok) last_scene_ = scene;
    if (window_.empty()) return;
    if (window_ == "ingame") {
      if (Chain(0x836F38D0u, {28, 0}, &v) && (v >> 16) == 25000) return Stop(frame, "entrance act");
    }
    if (window_ == "ingame" || window_ == "match") {
      if (Read(0x836F3928u, &v) && v != 0) return Stop(frame, "match clock");
      if (seq_ok && seq != 9) return Stop(frame, "left the in-game sequence");
    }
    if (window_ == "match") {
      // Slot 0's first real motion (act word readable, not 0 / -1): run 859 frame 3639, just before the loading
      // screen fades out (3650) and the match fades in (3674); the match clock only runs from 3826.
      if (Chain(0x836F38D0u, {28, 0}, &v) && v != 0 && v != 0xFFFFFFFFu) return Stop(frame, "slot 0 motion");
      if (match_by_scene_ && !m3_seen_ && frame > start_frame_ + 240) return Stop(frame, "no match scene file");
    }
    if (hold_only_) return;  // no wall-clock cap for a hold-only window (replay mode: same frames every run)
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time_).count();
    if (ms >= double(REXCVAR_GET(wwe2k14_load_turbo_cap_ms))) return Stop(frame, "cap");
  }

  void OnFileOpened(const std::string& path) {
    if (!AnyOn() && !HoldOnly()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
    const bool movie = Contains(path, "\\movies\\");
    if (Contains(path, "SaveData.Dat")) {
      // The title screen reads it (twice in the same frame, run 857); the title press reads it again later.
      if (first_save_frame_ == 0) first_save_frame_ = frame;
      boot_done_ = true;
      if (window_ == "boot") return Stop(frame, "title data");
      if (REXCVAR_GET(wwe2k14_load_turbo_menu) && window_.empty() && !menu_done_ && frame > first_save_frame_ + 30) {
        Start("menu", frame);
      }
      return;
    }
    if (window_ == "menu" && Contains(path, "MNUIHD.PAC")) {
      menu_done_ = true;
      return Stop(frame, "main menu");
    }
    if (movie && (window_ == "ingame" || window_ == "match")) return Stop(frame, "movie " + path);
    if (Contains(path, "M3.PAC") && InGameWindows()) {
      m3_seen_ = true;
      if (window_ == "ingame") Stop(frame, "match scene file");
      if (window_.empty()) {
        Start("match", frame);  // fallback start (no scene-word transition seen, e.g. a match without entrances)
        match_by_scene_ = false;
      }
    }
  }

 private:
  static bool AnyOn() {
    return REXCVAR_GET(wwe2k14_load_turbo) || REXCVAR_GET(wwe2k14_load_turbo_boot) ||
           REXCVAR_GET(wwe2k14_load_turbo_menu);
  }
  // Session 27.5: in-game windows tracked only for the step rate's hold (see the header).
  static bool HoldOnly() {
    return !REXCVAR_GET(wwe2k14_load_turbo) && REXCVAR_GET(wwe2k14_step_hz) != 0 &&
           REXCVAR_GET(wwe2k14_step_hz_hold_loads);
  }
  static bool InGameWindows() { return REXCVAR_GET(wwe2k14_load_turbo) || HoldOnly(); }
  static bool Contains(const std::string& s, const char* needle) {
    const size_t n = std::char_traits<char>::length(needle);
    if (s.size() < n) return false;
    for (size_t i = 0; i + n <= s.size(); ++i) {
      size_t j = 0;
      while (j < n && std::tolower(static_cast<unsigned char>(s[i + j])) ==
                          std::tolower(static_cast<unsigned char>(needle[j]))) {
        ++j;
      }
      if (j == n) return true;
    }
    return false;
  }
  static bool Read(uint32_t guest, uint32_t* out) {
    if (guest == 0 || (guest & 3) != 0) return false;
    auto* host = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(guest);
    if (!host || !GuestWordReadable(guest, host)) return false;  // guest_read.h (Session 15)
    *out = (uint32_t(host[0]) << 24) | (uint32_t(host[1]) << 16) | (uint32_t(host[2]) << 8) | uint32_t(host[3]);
    return true;
  }
  // Same semantics as guest_peek.h: v = [addr]; for each offset v = [v + off].
  static bool Chain(uint32_t addr, std::initializer_list<uint32_t> offsets, uint32_t* out) {
    uint32_t v = 0;
    if (!Read(addr, &v)) return false;
    for (uint32_t off : offsets) {
      if (!Read(v + off, &v)) return false;
    }
    *out = v;
    return true;
  }
  void Start(const char* name, uint64_t frame) {
    window_ = name;
    start_frame_ = frame;
    start_time_ = std::chrono::steady_clock::now();
    hold_only_ = (window_ == "ingame" || window_ == "match") && HoldOnly();
    if (hold_only_) {
      restore_vsync_ = false;
      restore_rate_.clear();
      skip_draws_set_ = false;
      g_load_turbo_ingame_window().store(true, std::memory_order_relaxed);
      REXLOG_INFO("[wwe2k14][turbo] {} window at frame {} (hold only: the step rate keeps the game's own rate; load "
                  "turbo off)",
                  window_, frame);
      return;
    }
    restore_vsync_ = rex::cvar::GetFlagByName("vsync") == "true";
    const double hz = REXCVAR_GET(wwe2k14_load_turbo_vblank_hz);
    restore_rate_.clear();
    // In-game windows only: in "boot" the faster frames ran out the scripts' frame-counted waits before the title
    // screen loaded (runs 1171-1173), and the goal is the in-game loads.
    if (restore_vsync_ && hz > 0.0 && (window_ == "ingame" || window_ == "match")) {
      restore_rate_ = rex::cvar::GetFlagByName("gpu_vblank_unthrottled_hz");
      rex::cvar::SetFlagByName("gpu_vblank_unthrottled_hz", std::to_string(hz));
    }
    if (restore_vsync_) rex::cvar::SetFlagByName("vsync", "false");
    skip_draws_set_ = false;
    if (const int32_t n = REXCVAR_GET(wwe2k14_load_turbo_skip_draws);
        n > 1 && (window_ == "ingame" || window_ == "match")) {
      rex::cvar::SetFlagByName("gpu_skip_frame_draws", std::to_string(n));
      skip_draws_set_ = true;
    }
    g_load_turbo_active_flag().store(restore_vsync_, std::memory_order_relaxed);
    g_load_turbo_ingame_window().store(window_ == "ingame" || window_ == "match", std::memory_order_relaxed);
    REXLOG_INFO("[wwe2k14][turbo] {} on at frame {}{}", window_, frame,
                restore_vsync_ ? "" : " (vsync was already off: nothing to do)");
  }
  void Stop(uint64_t frame, const std::string& reason) {
    if (window_.empty()) return;
    if (restore_vsync_) rex::cvar::SetFlagByName("vsync", "true");
    if (!restore_rate_.empty()) rex::cvar::SetFlagByName("gpu_vblank_unthrottled_hz", restore_rate_);
    restore_rate_.clear();
    if (skip_draws_set_) rex::cvar::SetFlagByName("gpu_skip_frame_draws", "0");
    skip_draws_set_ = false;
    g_load_turbo_active_flag().store(false, std::memory_order_relaxed);
    g_load_turbo_ingame_window().store(false, std::memory_order_relaxed);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_time_).count();
    REXLOG_INFO("[wwe2k14][turbo] {} off at frame {} ({}; {} frames, {:.0f} ms)", window_, frame, reason,
                frame - start_frame_, ms);
    if (g_load_window_done && (window_ == "ingame" || window_ == "match")) g_load_window_done(window_, ms);
    // Session 73: the CSV's scene marker (frame_stats.h) - the moment the player sees the scene.
    NoteScene(window_ == "ingame" ? "entrance start" : window_ == "match" ? "match start" : window_ + " screen");
    window_.clear();
    restore_vsync_ = false;
    hold_only_ = false;
  }

  std::mutex mutex_;
  bool hold_only_ = false;  // Session 27.5: this window only holds the step rate (turbo off)
  std::string window_;
  uint64_t start_frame_ = 0;
  std::chrono::steady_clock::time_point start_time_;
  bool restore_vsync_ = false, boot_done_ = false, menu_done_ = false;
  bool skip_draws_set_ = false;  // this window set gpu_skip_frame_draws
  std::string restore_rate_;  // gpu_vblank_unthrottled_hz before a wwe2k14_load_turbo_vblank_hz window ("" = none)
  uint32_t last_seq_ = 0, last_scene_ = 0;
  bool match_by_scene_ = false, m3_seen_ = false;
  uint64_t first_save_frame_ = 0;
};

inline LoadTurbo g_load_turbo;

// 2D layout animation step at its normal speed during a turbo window (see the header).
struct UiAnimThrottle {
  std::mutex mutex;
  std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> last;
  uint64_t allowed = 0, skipped = 0;
  std::atomic<bool> dirty{false};  // something to report / clear after a window
};
inline UiAnimThrottle g_ui_anim_throttle;

}  // namespace wwe2k14

REX_EXTERN(__imp__sub_82BBEC18);
REX_HOOK_RAW(sub_82BBEC18) {
  using namespace wwe2k14;
  if (g_ui_anim_hold().load(std::memory_order_relaxed)) return;  // Session 36: frozen scene, hold the pose
  if (!g_load_turbo_active_flag().load(std::memory_order_relaxed) || !REXCVAR_GET(wwe2k14_load_turbo_ui_real_time)) {
    if (g_ui_anim_throttle.dirty.exchange(false, std::memory_order_relaxed)) {
      std::lock_guard<std::mutex> lock(g_ui_anim_throttle.mutex);
      if (g_ui_anim_throttle.allowed || g_ui_anim_throttle.skipped) {
        REXLOG_INFO("[wwe2k14][turbo] ui animation steps in the last window: {} run, {} held", g_ui_anim_throttle.allowed,
                    g_ui_anim_throttle.skipped);
      }
      g_ui_anim_throttle.last.clear();
      g_ui_anim_throttle.allowed = g_ui_anim_throttle.skipped = 0;
    }
    __imp__sub_82BBEC18(ctx, base);
    return;
  }
  const uint32_t anim = ctx.r3.u32;
  const auto now = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(g_ui_anim_throttle.mutex);
    auto it = g_ui_anim_throttle.last.find(anim);
    if (it != g_ui_anim_throttle.last.end()) {
      const auto due = it->second + std::chrono::microseconds(16667);
      if (now < due) {
        ++g_ui_anim_throttle.skipped;
        return;  // hold the last pose
      }
      // Keep the 60 Hz grid (no drift); restart it after a long gap.
      it->second = (now - due) < std::chrono::milliseconds(50) ? due : now;
    } else {
      g_ui_anim_throttle.last.emplace(anim, now);
      g_ui_anim_throttle.dirty.store(true, std::memory_order_relaxed);
    }
    ++g_ui_anim_throttle.allowed;
  }
  __imp__sub_82BBEC18(ctx, base);
}
