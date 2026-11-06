#pragma once
// wwe2k14 - Session 93, idea 14 (Android only): the phone's thermal headroom (ADPF thermal API).
//
// AThermal_acquireManager / AThermal_getThermalHeadroom (android/thermal.h, API 30+; looked up in libandroid.so by dlsym, so
// the game still starts on older phones: no reading then). The headroom is a number the system forecasts: 0.0 = a cool
// device, 1.0 = the point where it throttles (severe), above 1.0 = throttling. NaN = no reading. The system wants at most one
// call a second; Tick() reads once per second.
//
// --wwe2k14_android_thermal=true (default false: nothing is read, nothing changes, nothing logged):
// - Tick() runs once per guest frame (src\android_pace.h OnFrame, from frame_stats.h) and reads the headroom every ~1 s;
//   a [pace][thermal] line every 5 s says what was read.
// - Headroom() is the latest reading for any other code (NaN until the first reading, or when the phone has none).
// - Dynamic resolution (dynamic_res.h) is told the phone is hot: headroom above kHot (0.85) for kHotSeconds (3) in a row
//   steps the resolution one step down, again every kStepSeconds (6) while it stays hot; it does not step back up until the
//   headroom falls under kCool (0.75). Only when --wwe2k14_dynamic_res is on (it does nothing otherwise).

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "dynamic_res.h"

REXCVAR_DEFINE_BOOL(wwe2k14_android_thermal, false, "wwe2k14",
                    "Session 93 (Android): read the phone's thermal headroom (ADPF thermal) once a second, log it, and step "
                    "dynamic resolution down while it stays above 0.85 (src/android_thermal.h). Default off");

#if defined(__ANDROID__)
#include <dlfcn.h>
#endif

namespace wwe2k14::android_thermal {

inline std::atomic<float> g_headroom{std::nanf("")};

// The latest headroom reading: 0 cool, 1 throttling point, NaN = none (switch off, API missing, or no reading yet).
inline float Headroom() { return g_headroom.load(std::memory_order_relaxed); }

class Reader {
 public:
  void Tick() {
    if (state_ == State::kOff) return;
    if (state_ == State::kUnset) {
      state_ = REXCVAR_GET(wwe2k14_android_thermal) && Load() ? State::kOn : State::kOff;
      if (state_ == State::kOff) return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now < next_) return;
    next_ = now + std::chrono::seconds(1);
    const float headroom = get_headroom_(manager_, 0);
    if (std::isnan(headroom)) {
      if (!nan_logged_) {
        nan_logged_ = true;
        REXLOG_INFO("[pace][thermal] no headroom reading yet (the system returned NaN)");
      }
      return;
    }
    g_headroom.store(headroom, std::memory_order_relaxed);
    ++reads_;
    max_ = std::max(max_, headroom);
    if (reads_ % 5 == 1) {
      REXLOG_INFO("[pace][thermal] headroom {:.2f} (max so far {:.2f}, {} reads, hot={})", headroom, max_, reads_,
                  hot_ ? "yes" : "no");
    }
    Judge(headroom);
  }

 private:
  enum class State { kUnset, kOn, kOff };
  static constexpr float kHot = 0.85f, kCool = 0.75f;
  static constexpr int kHotSeconds = 3, kStepSeconds = 6;

  using AcquireFn = void* (*)();
  using HeadroomFn = float (*)(void*, int);

  bool Load() {
#if defined(__ANDROID__)
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    auto acquire = lib ? reinterpret_cast<AcquireFn>(dlsym(lib, "AThermal_acquireManager")) : nullptr;
    get_headroom_ = lib ? reinterpret_cast<HeadroomFn>(dlsym(lib, "AThermal_getThermalHeadroom")) : nullptr;
    if (acquire && get_headroom_) manager_ = acquire();
    REXLOG_INFO("[pace][thermal] on: AThermal_acquireManager {}, AThermal_getThermalHeadroom {}, manager {}",
                acquire ? "found" : "missing", get_headroom_ ? "found" : "missing", manager_ ? "yes" : "no");
    return manager_ && get_headroom_;
#else
    REXLOG_INFO("[pace][thermal] --wwe2k14_android_thermal ignored: not Android");
    return false;
#endif
  }

  void Judge(float headroom) {
    if (headroom > kHot) {
      ++above_;
    } else {
      above_ = 0;
    }
    if (!hot_ && above_ >= kHotSeconds) {
      hot_ = true;
      since_step_ = kStepSeconds;  // the first step at once
      REXLOG_INFO("[pace][thermal] hot: headroom above {:.2f} for {} s", kHot, kHotSeconds);
    } else if (hot_ && headroom < kCool) {
      hot_ = false;
      REXLOG_INFO("[pace][thermal] cool again: headroom {:.2f}", headroom);
    }
    if (hot_ && headroom > kHot && ++since_step_ >= kStepSeconds) {
      since_step_ = 0;
      dynres::g_controller.OnThermal(true, headroom);
    } else if (!hot_) {
      dynres::g_controller.OnThermal(false, headroom);
    }
  }

  State state_ = State::kUnset;
  void* manager_ = nullptr;
  HeadroomFn get_headroom_ = nullptr;
  std::chrono::steady_clock::time_point next_{};
  bool nan_logged_ = false, hot_ = false;
  int above_ = 0, since_step_ = 0;
  uint32_t reads_ = 0;
  float max_ = 0.0f;
};

inline Reader g_reader;

}  // namespace wwe2k14::android_thermal
