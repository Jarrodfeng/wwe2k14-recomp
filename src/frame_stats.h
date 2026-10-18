// wwe2k14 - guest frame timing. The game calls VdSwap once per frame it presents (hooked in
// kernel_overrides.h), so the time between calls is the game's own frame time: what the player feels as
// slowdown, independent of how the host GPU presents. Logs a summary every few seconds plus every hitch,
// and writes every frame to logs/frames_NNN.csv (NNN = this run's wwe2k14_NNN.log).

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#ifdef _WIN32
#include <share.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

REXCVAR_DEFINE_INT32(wwe2k14_frame_summary_seconds, 5, "wwe2k14",
                     "Log a guest frame-time summary every N seconds (0 = off)");
REXCVAR_DEFINE_INT32(wwe2k14_hitch_ms, 100, "wwe2k14",
                     "Log every guest frame that takes at least this many milliseconds (0 = off)");
REXCVAR_DEFINE_BOOL(wwe2k14_frame_csv, true, "wwe2k14",
                    "Write every guest frame's time to logs/frames_NNN.csv");

namespace wwe2k14 {

// This run's log number: the newest logs/wwe2k14_NNN.log, which ReXGlue creates at startup. Our own files
// use it (frames_NNN.csv, captures/NNN_*.bmp) but never the "wwe2k14_" prefix, which ReXGlue's sequential
// log naming scans (core/logging.cpp NextSequentialLogPath). -1 if none.
// Session 8 task 0: taken ONCE and kept (CurrentLogNumber, primed in OnPostInitLogging right after ReXGlue created
// the log). With the run lock's shared mode up to 3 instances run at once, and a later scan would see another
// instance's newer log (runs 384/385 saved their screenshots as 386_*). run_and_triage serializes launches until
// the new log exists, so at priming time the newest log is this run's. One instance: same number as before.
inline int ScanNewestLogNumber() {
  int newest = -1;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path() / "logs", ec)) {
    const std::string stem = entry.path().stem().string();
    if (entry.path().extension() != ".log" || stem.rfind("wwe2k14_", 0) != 0) continue;
    const std::string digits = stem.substr(8);
    if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) continue;
    newest = std::max(newest, std::stoi(digits));
  }
  return newest;
}
inline int CurrentLogNumber() {
  static std::atomic<int> cached{-1};
  int v = cached.load(std::memory_order_acquire);
  if (v >= 0) return v;
  v = ScanNewestLogNumber();
  if (v < 0) return v;
  int expected = -1;
  cached.compare_exchange_strong(expected, v, std::memory_order_acq_rel);
  return cached.load(std::memory_order_acquire);
}

// Guest frames presented so far (VdSwap calls). Also the clock for input replay later.
inline std::atomic<uint64_t> g_guest_frame{0};

// Session 29: set by harness steps that pause the game on purpose (a memory scan): the next frame's length is not a
// hitch (src/bookmark.h's automatic bookmarks skip it).
inline std::atomic<bool> g_frame_time_ignore_next{false};

// Windows thread id of the first thread that called VdSwap (the game's main thread, as the profiler's "main thread");
// 0 before the first guest frame. thread_placement.h places it under the pseudo-name "main".
inline std::atomic<uint32_t> g_swap_thread_id{0};

class FrameStats {
 public:
  void OnSwap() {
    using clock = std::chrono::steady_clock;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = clock::now();
    const uint64_t frame = ++g_guest_frame;
#ifdef _WIN32
    const uint32_t thread_id = GetCurrentThreadId();
    if (frame == 1) {
      g_swap_thread_id.store(thread_id, std::memory_order_relaxed);
    } else if (thread_id != g_swap_thread_id.load(std::memory_order_relaxed) && !other_swapper_logged_) {
      other_swapper_logged_ = true;  // evidence for whether "main" is one stable thread; logged once per run
      REXLOG_INFO("[wwe2k14][frames] VdSwap called from thread {} at frame {} (first caller: thread {}); "
                  "logged once", thread_id, frame, g_swap_thread_id.load(std::memory_order_relaxed));
    }
#endif
    if (frame == 1) {
      start_ = last_ = window_start_ = now;
      window_first_frame_ = 1;
      OpenCsv();
      REXLOG_INFO("[wwe2k14][frames] first guest frame{}", csv_ ? " (per-frame CSV: " + csv_path_ + ")" : "");
      return;
    }
    const double ms = std::chrono::duration<double, std::milli>(now - last_).count();
    const double t = std::chrono::duration<double>(now - start_).count();
    last_ = now;

    if (csv_) std::fprintf(csv_, "%llu,%.3f,%.3f\n", static_cast<unsigned long long>(frame), t, ms);

    window_ms_.push_back(ms);
    const int32_t hitch_ms = REXCVAR_GET(wwe2k14_hitch_ms);
    if (hitch_ms > 0 && ms >= hitch_ms) {
      ++window_hitches_;
      if (++hitches_logged_ <= kMaxHitchLines) {
        REXLOG_INFO("[wwe2k14][frames] hitch: frame {} took {:.0f} ms (t={:.1f} s){}", frame, ms, t,
                    hitches_logged_ == kMaxHitchLines ? "; further hitches only counted in summaries" : "");
      }
    }

    const int32_t summary_s = REXCVAR_GET(wwe2k14_frame_summary_seconds);
    const double window_s = std::chrono::duration<double>(now - window_start_).count();
    if (summary_s > 0 && window_s >= summary_s) {
      LogSummary(frame, t, window_s);
      window_start_ = now;
      window_first_frame_ = frame + 1;
      window_ms_.clear();
      window_hitches_ = 0;
      if (csv_) std::fflush(csv_);
    }
  }

 private:
  static constexpr uint32_t kMaxHitchLines = 2000;

  void LogSummary(uint64_t frame, double t, double window_s) {
    if (window_ms_.empty()) return;
    std::vector<double> sorted = window_ms_;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double v : sorted) sum += v;
    const double avg = sum / sorted.size();
    const double p95 = sorted[std::min(sorted.size() - 1, size_t(sorted.size() * 0.95))];
    REXLOG_INFO("[wwe2k14][frames] t={:.0f} s, frames {}-{}: {:.1f} fps, avg {:.1f} ms, p95 {:.1f} ms, "
                "worst {:.0f} ms, hitches {}",
                t, window_first_frame_, frame, sorted.size() / window_s, avg, p95, sorted.back(),
                window_hitches_);
  }

  void OpenCsv() {
    if (!REXCVAR_GET(wwe2k14_frame_csv)) return;
    const int newest = CurrentLogNumber();
    if (newest < 0) return;
    char name[32];
    std::snprintf(name, sizeof(name), "frames_%03d.csv", newest);
    csv_path_ = (std::filesystem::current_path() / "logs" / name).string();
#ifdef _WIN32
    csv_ = _fsopen(csv_path_.c_str(), "w", _SH_DENYNO);  // shared: readable while the game runs
#else
    csv_ = std::fopen(csv_path_.c_str(), "w");
#endif
    if (csv_) std::fputs("frame,t_seconds,frame_ms\n", csv_);
  }

  std::mutex mutex_;
  std::chrono::steady_clock::time_point start_, last_, window_start_;
  uint64_t window_first_frame_ = 0;
  std::vector<double> window_ms_;
  uint32_t window_hitches_ = 0;
  uint32_t hitches_logged_ = 0;
  bool other_swapper_logged_ = false;
  std::FILE* csv_ = nullptr;
  std::string csv_path_;
};

inline FrameStats g_frame_stats;

}  // namespace wwe2k14
