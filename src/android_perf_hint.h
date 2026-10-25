#pragma once
// wwe2k14 - Session 64 (Android only): ask the phone's CPU governor for the clocks a 60 fps frame needs.
//
// Phone runs 025-030 (SESSION64_SUMMARY.md): with the audio threads no longer spinning, the governor ran the busy game
// threads' cores at 1.0-2.4 GHz (of 3.5 / 4.3) while frames ran late: it sees moderate load per core, not the frame
// deadline. Android's performance hint API (APerformanceHint, API 33+) is how a game tells it: a session holds the
// threads that make a frame and a target frame time; every frame reports how long it took, and the governor raises
// those threads' clocks when frames run over the target (and may lower them when they finish early).
//
// The threads: every few seconds the busiest threads of the process by CPU time since the last look (/proc/self/task:
// the GPU command thread, the game's own threads, the submit thread ...), at least kMinBusyPercent of a core. The
// session is made with them and updated when the set changes (APerformanceHint_setThreads, API 34; on API 33 the session
// is made again). The frame time reported: VdSwap to VdSwap (frame_stats.h). The functions come from libandroid.so by
// dlsym, so the game still starts on phones older than API 33 (no session then).
//
// --wwe2k14_android_perf_hint=false turns it off; --wwe2k14_android_perf_hint_target_us sets the target (16667 = 60 fps).

#if defined(__ANDROID__)

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(wwe2k14_android_perf_hint, true, "wwe2k14",
                    "Session 64 (Android): an APerformanceHint session for the game's busy threads (src/android_perf_hint.h)");
REXCVAR_DEFINE_INT32(wwe2k14_android_perf_hint_target_us, 16667, "wwe2k14",
                     "Session 64 (Android): the performance hint's target frame time in microseconds (16667 = 60 fps)");

namespace wwe2k14::perf_hint {

struct APerformanceHintManager;
struct APerformanceHintSession;

class PerfHint {
 public:
  // Once per guest frame (frame_stats.h OnSwap), with the frame's length.
  void OnFrame(double frame_ms) {
    if (state_ == State::kOff) return;
    if (state_ == State::kUnset) {
      if (!REXCVAR_GET(wwe2k14_android_perf_hint) || !Load()) {
        state_ = State::kOff;
        return;
      }
      state_ = State::kOn;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_scan_) {
      next_scan_ = now + std::chrono::seconds(kScanSeconds);
      UpdateThreads();
    }
    if (session_ && frame_ms > 0.0 && frame_ms < 1000.0) {
      report_(session_, int64_t(frame_ms * 1e6));
    }
  }

 private:
  enum class State { kUnset, kOn, kOff };
  static constexpr int kScanSeconds = 5;
  static constexpr double kMinBusyPercent = 15.0;
  static constexpr size_t kMaxThreads = 16;

  using GetManagerFn = APerformanceHintManager* (*)();
  using CreateSessionFn = APerformanceHintSession* (*)(APerformanceHintManager*, const int32_t*, size_t, int64_t);
  using ReportFn = int (*)(APerformanceHintSession*, int64_t);
  using SetThreadsFn = int (*)(APerformanceHintSession*, const int32_t*, size_t);
  using CloseFn = void (*)(APerformanceHintSession*);

  bool Load() {
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return false;
    auto get_manager = reinterpret_cast<GetManagerFn>(dlsym(lib, "APerformanceHint_getManager"));
    create_ = reinterpret_cast<CreateSessionFn>(dlsym(lib, "APerformanceHint_createSession"));
    report_ = reinterpret_cast<ReportFn>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    set_threads_ = reinterpret_cast<SetThreadsFn>(dlsym(lib, "APerformanceHint_setThreads"));  // API 34
    close_ = reinterpret_cast<CloseFn>(dlsym(lib, "APerformanceHint_closeSession"));
    if (!get_manager || !create_ || !report_ || !close_) {
      REXLOG_INFO("[wwe2k14][perf_hint] APerformanceHint not available (Android before 13): off");
      return false;
    }
    manager_ = get_manager();
    if (!manager_) {
      REXLOG_INFO("[wwe2k14][perf_hint] no performance hint manager on this phone: off");
      return false;
    }
    return true;
  }

  // CPU time (utime + stime, clock ticks) per thread of this process.
  static std::map<int32_t, uint64_t> ReadThreadTimes(std::map<int32_t, std::string>* names) {
    std::map<int32_t, uint64_t> out;
    DIR* dir = opendir("/proc/self/task");
    if (!dir) return out;
    while (dirent* e = readdir(dir)) {
      const int32_t tid = std::atoi(e->d_name);
      if (tid <= 0) continue;
      char path[64];
      std::snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
      FILE* f = std::fopen(path, "r");
      if (!f) continue;
      char buf[512];
      const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
      std::fclose(f);
      buf[n] = 0;
      // "tid (comm) state ..." - comm may hold spaces and parentheses: the fields start after the last ')'.
      const char* close_paren = std::strrchr(buf, ')');
      const char* open_paren = std::strchr(buf, '(');
      if (!close_paren || !open_paren) continue;
      unsigned long long utime = 0, stime = 0;
      // after ") ": state(3) ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime(14) stime(15)
      if (std::sscanf(close_paren + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2) {
        continue;
      }
      out[tid] = utime + stime;
      if (names) (*names)[tid] = std::string(open_paren + 1, close_paren);
    }
    closedir(dir);
    return out;
  }

  void UpdateThreads() {
    const auto now = std::chrono::steady_clock::now();
    std::map<int32_t, std::string> names;
    std::map<int32_t, uint64_t> times = ReadThreadTimes(&names);
    const double seconds = std::chrono::duration<double>(now - last_scan_time_).count();
    const double ticks_per_second = double(sysconf(_SC_CLK_TCK));
    std::vector<std::pair<double, int32_t>> busy;
    if (!last_times_.empty() && seconds > 0.5) {
      for (const auto& [tid, t] : times) {
        auto it = last_times_.find(tid);
        if (it == last_times_.end() || t < it->second) continue;
        const double percent = 100.0 * double(t - it->second) / ticks_per_second / seconds;
        if (percent >= kMinBusyPercent) busy.push_back({percent, tid});
      }
    }
    last_times_ = std::move(times);
    last_scan_time_ = now;
    if (busy.empty()) return;  // (the first look, or nothing busy: keep the session as it is)
    std::sort(busy.rbegin(), busy.rend());
    if (busy.size() > kMaxThreads) busy.resize(kMaxThreads);
    std::vector<int32_t> tids;
    for (const auto& b : busy) tids.push_back(b.second);
    std::sort(tids.begin(), tids.end());
    if (tids == tids_) return;
    std::string list;
    for (const auto& b : busy) list += fmt::format("{}{} ({:.0f}%)", list.empty() ? "" : ", ", names[b.second], b.first);
    const int64_t target_ns = int64_t(std::max(1000, REXCVAR_GET(wwe2k14_android_perf_hint_target_us))) * 1000;
    if (session_ && set_threads_ && set_threads_(session_, tids.data(), tids.size()) == 0) {
      REXLOG_INFO("[wwe2k14][perf_hint] threads now: {}", list);
    } else {
      if (session_) close_(session_);
      session_ = create_(manager_, tids.data(), tids.size(), target_ns);
      if (!session_) {
        REXLOG_WARN("[wwe2k14][perf_hint] createSession failed ({} threads): off", tids.size());
        state_ = State::kOff;
        return;
      }
      REXLOG_INFO("[wwe2k14][perf_hint] session, target {:.2f} ms, threads: {}", target_ns / 1e6, list);
    }
    tids_ = std::move(tids);
  }

  State state_ = State::kUnset;
  APerformanceHintManager* manager_ = nullptr;
  APerformanceHintSession* session_ = nullptr;
  CreateSessionFn create_ = nullptr;
  ReportFn report_ = nullptr;
  SetThreadsFn set_threads_ = nullptr;
  CloseFn close_ = nullptr;
  std::chrono::steady_clock::time_point next_scan_{};
  std::chrono::steady_clock::time_point last_scan_time_{};
  std::map<int32_t, uint64_t> last_times_;
  std::vector<int32_t> tids_;
};

inline PerfHint g_perf_hint;

}  // namespace wwe2k14::perf_hint

#endif  // __ANDROID__
