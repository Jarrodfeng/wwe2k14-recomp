#pragma once
// Session 73 (Jarrod's item 6, "dynamic resolution on phones"): when the game's frames run over their time for a few
// seconds (heat, a busy entrance), the NATIVE truth passes draw a step smaller (SDK gpu_native_truth_draw_height, which
// may change while running: the twins at the new size from the next frame end; the screen keeps its size), and a step
// bigger again after a while with time to spare. Steps: 100, 90, 80, 75, 67, 60% of the chosen resolution's height.
//
// Only a GPU-bound phone gains from a smaller picture. A step down that does not make the frames at least 5% faster
// means the CPU is what is slow: the step is undone and no further step down is tried for 30 s.
//
// Frames: the guest frames' times (FrameStats::OnSwap). Windows of 1 s; frames over 4x the target (loads, scene
// changes) are left out, and a window that is mostly such frames counts for nothing. Target: the game's frame time now
// (frame_word.h: 16.7 ms at 60 fps, 33.3 in 30 fps entrances and with the 30 fps choice), or
// --wwe2k14_dynamic_res_target_ms.
// Back up (fixed later in Session 73: the first version waited for frames well under the target, which never come with
// the game paced by its vblank, so it never stepped back up): a probe step up after 10 s on time; one that runs over
// comes back down, and the next probe waits twice as long (up to 2 min).
// Logs [wwe2k14][dynres] lines. --wwe2k14_dynamic_res=true turns it on (the phone launcher's switch, on by default).
#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include "frame_word.h"  // TargetFrameMs

REXCVAR_DEFINE_BOOL(wwe2k14_dynamic_res, false, "wwe2k14",
                    "Session 73: dynamic resolution - the NATIVE truth passes draw a step smaller while frames run over "
                    "their time, bigger again with time to spare (src\\dynamic_res.h). Needs NATIVE truth frames");
REXCVAR_DEFINE_INT32(wwe2k14_dynamic_res_min_percent, 60, "wwe2k14",
                     "Session 73: dynamic resolution's lowest step, in percent of the chosen resolution's height")
    .range(50, 100);
REXCVAR_DEFINE_DOUBLE(wwe2k14_dynamic_res_target_ms, 0.0, "wwe2k14",
                      "Session 73: dynamic resolution's frame time target in ms (0 = 16.7, or 33.3 with "
                      "--wwe2k14_step_hz=30); a test may set it low to force steps");

namespace wwe2k14::dynres {

class Controller {
 public:
  void OnFrame(double ms) {
    if (!REXCVAR_GET(wwe2k14_dynamic_res)) return;
    if (!started_ && !Start()) return;
    const double target = Target();
    window_ms_ += ms;
    if (ms > 4.0 * target) {
      ++window_skipped_;
    } else {
      window_used_ms_ += ms;
      ++window_frames_;
    }
    if (window_ms_ < 1000.0) return;
    const bool loading = window_skipped_ * 4 > window_frames_ + window_skipped_;
    const double avg = window_frames_ ? window_used_ms_ / window_frames_ : 0.0;
    window_ms_ = window_used_ms_ = 0.0;
    window_frames_ = window_skipped_ = 0;
    if (loading || avg <= 0.0) return;
    Window(avg, target);
  }

 private:
  static constexpr double kSteps[] = {1.0, 0.9, 0.8, 0.75, 2.0 / 3.0, 0.6};
  static constexpr int kStepCount = int(sizeof(kSteps) / sizeof(kSteps[0]));

  bool Start() {
    started_ = true;
    const std::string native = rex::cvar::GetFlagByName("gpu_native");
    const std::string truth = rex::cvar::GetFlagByName("gpu_native_truth");
    if (native != "2" || truth != "true") {
      REXLOG_INFO("[wwe2k14][dynres] off: needs NATIVE truth frames (gpu_native {}, truth {})", native, truth);
      active_ = false;
      return false;
    }
    int scale = 1;
    try { scale = std::max(1, std::stoi(rex::cvar::GetFlagByName("resolution_scale"))); } catch (...) {}
    int chosen = 0;
    try { chosen = std::stoi(rex::cvar::GetFlagByName("gpu_native_truth_draw_height")); } catch (...) {}
    full_ = 720 * scale;
    base_ = chosen > 0 && chosen < full_ ? chosen : full_;
    chosen_flag_ = chosen;
    const int min_percent = REXCVAR_GET(wwe2k14_dynamic_res_min_percent);
    lowest_ = 0;
    while (lowest_ + 1 < kStepCount && kSteps[lowest_ + 1] * 100.0 >= min_percent - 0.01) ++lowest_;
    active_ = true;
    REXLOG_INFO("[wwe2k14][dynres] on: {} rows (of {}), steps down to {}%, target {:.1f} ms", base_, full_,
                int(std::lround(kSteps[lowest_] * 100.0)), Target());
    return true;
  }

 public:
  // Session 73: the truth passes' draw height now (frame_stats.h's CSV): the step's rows while dynamic resolution
  // runs, else the chosen draw height (0 = the whole size).
  int Rows() const {
    if (active_) return Height(step_);
    if (rows_flag_ < -1) {
      try { rows_flag_ = std::stoi(rex::cvar::GetFlagByName("gpu_native_truth_draw_height")); } catch (...) {
        rows_flag_ = 0;
      }
    }
    return rows_flag_;
  }

 private:
  double Target() const {
    const double forced = REXCVAR_GET(wwe2k14_dynamic_res_target_ms);
    if (forced > 0.0) return forced;
    // The game's frame-rate word now (frame_word.h): 30 fps entrances with "60 matches, 30 entrances" are on time at
    // 33 ms, not over a 16.7 ms budget.
    return TargetFrameMs();
  }

  void Window(double avg, double target) {
    ++windows_;
    if (hold_ > 0) --hold_;
    if (cooldown_ > 0) {
      --cooldown_;
      return;
    }
    // A step down checked against the frames before it.
    if (check_after_down_) {
      check_after_down_ = false;
      if (avg > before_down_ms_ * 0.95) {
        REXLOG_INFO("[wwe2k14][dynres] {:.1f} ms after the step down vs {:.1f} before: not the GPU - back to {} rows, "
                    "no step down for 30 s",
                    avg, before_down_ms_, Height(step_ - 1));
        Apply(step_ - 1, avg);
        hold_ = 30;
        cooldown_ = 2;
        over_ = under_ = 0;
        return;
      }
    }
    over_ = avg > target * 1.12 ? over_ + 1 : 0;
    // On time. (Frames cannot come faster than the target: the game waits for its vblank, so time to spare never shows
    // in frame times. A step up is a probe instead: after probe_wait_ windows on time, one step up; a probe that runs
    // over within kProbeWindows windows comes back down and the next waits twice as long, up to kProbeWaitMax.)
    under_ = avg <= target * 1.03 ? under_ + 1 : 0;
    if (probe_left_ > 0) --probe_left_;
    if (over_ >= 2 && step_ < lowest_ && hold_ == 0) {
      if (probe_left_ > 0) {
        probe_wait_ = std::min(probe_wait_ * 2, kProbeWaitMax);
        REXLOG_INFO("[wwe2k14][dynres] the step up ran over: back down, next try after {} s", probe_wait_);
        probe_left_ = 0;
      }
      before_down_ms_ = avg;
      check_after_down_ = true;
      Apply(step_ + 1, avg);
      cooldown_ = 1;
      over_ = under_ = 0;
    } else if (under_ >= probe_wait_ && step_ > 0) {
      Apply(step_ - 1, avg);
      probe_left_ = kProbeWindows;
      cooldown_ = 1;
      over_ = under_ = 0;
    } else if (probe_left_ == 1 && over_ == 0) {
      probe_wait_ = kProbeWaitStart;  // the step up held
    }
  }

  static constexpr int kProbeWaitStart = 10, kProbeWaitMax = 120, kProbeWindows = 4;

  int Height(int step) const {
    const int h = int(std::lround(base_ * kSteps[step] / 2.0)) * 2;
    return std::clamp(h, 2, full_);
  }

  void Apply(int step, double avg) {
    step = std::clamp(step, 0, lowest_);
    if (step == step_) return;
    const int old_rows = Height(step_);
    step_ = step;
    const int rows = Height(step_);
    // The full height as 0 (the resolution scale's own size: no draw twins), as the launcher gives it.
    const int flag = step_ == 0 ? chosen_flag_ : (rows >= full_ ? 0 : rows);
    rex::cvar::SetFlagByName("gpu_native_truth_draw_height", std::to_string(flag));
    ++changes_;
    REXLOG_INFO("[wwe2k14][dynres] {} -> {} rows ({}%; frames {:.1f} ms, target {:.1f}); change {}", old_rows, rows,
                int(std::lround(kSteps[step_] * 100.0)), avg, Target(), changes_);
  }

  bool started_ = false, active_ = false;
  mutable int rows_flag_ = -2;  // Session 73: Rows() without dynamic resolution (read once)
  int full_ = 720, base_ = 720, chosen_flag_ = 0, lowest_ = 0, step_ = 0;
  double window_ms_ = 0.0, window_used_ms_ = 0.0;
  int window_frames_ = 0, window_skipped_ = 0;
  int over_ = 0, under_ = 0, cooldown_ = 0, hold_ = 0;
  int probe_wait_ = kProbeWaitStart, probe_left_ = 0;
  bool check_after_down_ = false;
  double before_down_ms_ = 0.0;
  uint64_t windows_ = 0, changes_ = 0;
};

inline Controller g_controller;

}  // namespace wwe2k14::dynres
