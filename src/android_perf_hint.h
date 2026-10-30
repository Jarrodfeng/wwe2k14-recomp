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
//
// Session 73 (Jarrod's idea 7 of the third ten, big cores and the performance hints):
// - The target follows the game's frame rate now (frame_word.h: 33.3 ms with the 30 fps choice and in 30 fps
//   entrances), as dynamic_res.h's does. With the fixed 16.7 ms a 30 fps frame reported twice the target, and the
//   governor held the clocks at the top: heat, then throttling. Re-set the frame the rate changes
//   (APerformanceHint_updateTargetWorkDuration). An explicit --wwe2k14_android_perf_hint_target_us still wins.
// - The phone's cores are logged once (each cluster's top clock, from cpufreq), and each thread-set change logs the
//   core every busy thread ran on last ("/proc/self/task/TID/stat" field 39), so a log shows whether the hint session's
//   threads sit on the big cores.
// - --wwe2k14_android_big_cores=true (off by default; the launcher's "Big cores" switch): the busy threads are kept
//   off the slowest cluster (sched_setaffinity to every faster core; never one core per thread: pinning was harmful on
//   the PC, Session 5's thread placement). For A/B on phones; not measured on a phone yet.
// - b77: where Android has it (API 35, APerformanceHint_reportActualWorkDuration2 + AWorkDuration), each frame's GPU
//   time (frame_stats.h, the submissions' timestamps) goes with the frame's length, so the system can raise the GPU
//   clock too (Jarrod's b76 log: each entrance's first ~2 s at 35-40 ms of GPU a frame, 6-13 after, CPU idle).

#if defined(__ANDROID__)

#include <dirent.h>
#include <dlfcn.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cmath>

#include "frame_word.h"  // Session 73: TargetFrameMs

REXCVAR_DEFINE_BOOL(wwe2k14_android_perf_hint, true, "wwe2k14",
                    "Session 64 (Android): an APerformanceHint session for the game's busy threads (src/android_perf_hint.h)");
REXCVAR_DEFINE_INT32(wwe2k14_android_perf_hint_target_us, 0, "wwe2k14",
                     "Session 64 (Android): the performance hint's target frame time in microseconds (16667 = 60 fps); "
                     "0 = the game's frame rate (33333 with --wwe2k14_step_hz=30, Session 73)");
REXCVAR_DEFINE_BOOL(wwe2k14_android_big_cores, false, "wwe2k14",
                    "Session 73 (Android): keep the busy game threads off the phone's slowest CPU cluster "
                    "(src/android_perf_hint.h)");

namespace wwe2k14::perf_hint {

struct APerformanceHintManager;
struct APerformanceHintSession;

class PerfHint {
 public:
  // Once per guest frame (frame_stats.h OnSwap), with the frame's length.
  void OnFrame(double frame_ms, double gpu_ms = -1.0) {
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
    // The frame rate changes mid-run (30 fps entrances): the session's target follows it at once.
    const int64_t target_ns = TargetNs();
    if (session_ && update_target_ && target_ns != session_target_ns_ && update_target_(session_, target_ns) == 0) {
      REXLOG_INFO("[wwe2k14][perf_hint] target now {:.2f} ms", target_ns / 1e6);
      session_target_ns_ = target_ns;
    }
    if (session_ && frame_ms > 0.0 && frame_ms < 1000.0) {
      if (!ReportWithGpu(frame_ms, gpu_ms)) report_(session_, int64_t(frame_ms * 1e6));
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
  using UpdateTargetFn = int (*)(APerformanceHintSession*, int64_t);
  using CloseFn = void (*)(APerformanceHintSession*);
  // API 35
  struct AWorkDuration;
  using WdCreateFn = AWorkDuration* (*)();
  using WdSetFn = void (*)(AWorkDuration*, int64_t);
  using Report2Fn = int (*)(APerformanceHintSession*, AWorkDuration*);

  // b77: the frame with its GPU time (API 35); false = not reported this way (the plain report then).
  bool ReportWithGpu(double frame_ms, double gpu_ms) {
    if (!report2_ || gpu_ms < 0.0 || gpu2_failed_) return false;
    if (!work_) work_ = wd_create_();
    if (!work_) {
      gpu2_failed_ = true;
      return false;
    }
    const int64_t total_ns = int64_t(frame_ms * 1e6);
    const int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch()).count();
    wd_start_(work_, std::max<int64_t>(1, now_ns - total_ns));
    wd_total_(work_, total_ns);
    wd_cpu_(work_, total_ns);  // as the plain report: the frame's length
    wd_gpu_(work_, std::max<int64_t>(1, int64_t(std::min(gpu_ms, 1000.0) * 1e6)));
    const int result = report2_(session_, work_);
    if (!gpu2_logged_) {
      gpu2_logged_ = true;
      REXLOG_INFO("[wwe2k14][perf_hint] reporting the GPU time too (API 35): {}",
                  result == 0 ? std::string("ok") : fmt::format("failed ({}): plain reports", result));
    }
    if (result != 0) gpu2_failed_ = true;
    return result == 0;
  }

  // The frame time the game aims for now (frame_word.h, as dynamic_res.h's Target()).
  static int64_t TargetNs() {
    const int32_t forced_us = REXCVAR_GET(wwe2k14_android_perf_hint_target_us);
    if (forced_us > 0) return int64_t(std::max(1000, forced_us)) * 1000;
    return int64_t(std::llround(TargetFrameMs() * 1e6));
  }

  // The phone's CPU clusters by top clock (cpufreq cpuinfo_max_freq, kHz), logged once; big_mask_ = every core
  // faster than the slowest cluster (0 when all cores are alike).
  void ReadClusters() {
    std::map<uint32_t, std::vector<int>> by_khz;
    const long cpus = sysconf(_SC_NPROCESSORS_CONF);
    for (int cpu = 0; cpu < cpus && cpu < 64; ++cpu) {
      char path[96];
      std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
      FILE* f = std::fopen(path, "r");
      unsigned khz = 0;
      if (f) {
        if (std::fscanf(f, "%u", &khz) != 1) khz = 0;
        std::fclose(f);
      }
      by_khz[khz].push_back(cpu);
    }
    std::string text;
    for (const auto& [khz, list] : by_khz) {
      text += fmt::format("{}cores {}-{} {:.2f} GHz", text.empty() ? "" : ", ", list.front(), list.back(), khz / 1e6);
    }
    big_mask_ = 0;
    if (by_khz.size() > 1 && by_khz.begin()->first > 0) {
      for (auto it = std::next(by_khz.begin()); it != by_khz.end(); ++it) {
        for (int cpu : it->second) big_mask_ |= uint64_t(1) << cpu;
      }
    }
    REXLOG_INFO("[wwe2k14][perf_hint] CPU: {} ({} cores faster than the slowest cluster)", text,
                __builtin_popcountll(big_mask_));
  }

  // The busy threads kept on the faster cores (--wwe2k14_android_big_cores), or let go again when it is off.
  void PlaceThreads(const std::vector<int32_t>& tids) {
    const bool on = REXCVAR_GET(wwe2k14_android_big_cores) && big_mask_ != 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    const long cpus = sysconf(_SC_NPROCESSORS_CONF);
    for (int cpu = 0; cpu < cpus && cpu < 64; ++cpu) {
      if (!on || (big_mask_ >> cpu) & 1) CPU_SET(cpu, &set);
    }
    int placed = 0;
    // Off: every thread placed before goes back to all cores (busy now or not).
    const std::vector<int32_t> targets = on ? tids : std::vector<int32_t>(placed_.begin(), placed_.end());
    for (int32_t tid : targets) {
      const bool was = placed_.count(tid) != 0;
      if (on == was) continue;
      if (sched_setaffinity(tid, sizeof(set), &set) == 0) {
        ++placed;
        if (on) {
          placed_.insert(tid);
        } else {
          placed_.erase(tid);
        }
      }
    }
    if (placed) REXLOG_INFO("[wwe2k14][perf_hint] big cores {}: {} threads", on ? "on" : "off", placed);
  }

  bool Load() {
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return false;
    auto get_manager = reinterpret_cast<GetManagerFn>(dlsym(lib, "APerformanceHint_getManager"));
    create_ = reinterpret_cast<CreateSessionFn>(dlsym(lib, "APerformanceHint_createSession"));
    report_ = reinterpret_cast<ReportFn>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    set_threads_ = reinterpret_cast<SetThreadsFn>(dlsym(lib, "APerformanceHint_setThreads"));  // API 34
    update_target_ = reinterpret_cast<UpdateTargetFn>(dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
    close_ = reinterpret_cast<CloseFn>(dlsym(lib, "APerformanceHint_closeSession"));
    report2_ = reinterpret_cast<Report2Fn>(dlsym(lib, "APerformanceHint_reportActualWorkDuration2"));  // API 35
    wd_create_ = reinterpret_cast<WdCreateFn>(dlsym(lib, "AWorkDuration_create"));
    wd_start_ = reinterpret_cast<WdSetFn>(dlsym(lib, "AWorkDuration_setWorkPeriodStartTimestampNanos"));
    wd_total_ = reinterpret_cast<WdSetFn>(dlsym(lib, "AWorkDuration_setActualTotalDurationNanos"));
    wd_cpu_ = reinterpret_cast<WdSetFn>(dlsym(lib, "AWorkDuration_setActualCpuDurationNanos"));
    wd_gpu_ = reinterpret_cast<WdSetFn>(dlsym(lib, "AWorkDuration_setActualGpuDurationNanos"));
    if (!wd_create_ || !wd_start_ || !wd_total_ || !wd_cpu_ || !wd_gpu_) report2_ = nullptr;
    REXLOG_INFO("[wwe2k14][perf_hint] GPU time reports: {}", report2_ ? "available (API 35)" : "not on this Android");
    ReadClusters();
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
  static std::map<int32_t, uint64_t> ReadThreadTimes(std::map<int32_t, std::string>* names,
                                                     std::map<int32_t, int>* last_cpu = nullptr) {
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
      if (last_cpu) {
        // Field 39, "processor": the core the thread last ran on (36 fields after state, field 3).
        const char* p = close_paren + 2;
        for (int field = 3; field < 39 && p; ++field) {
          p = std::strchr(p, ' ');
          if (p) ++p;
        }
        if (p) (*last_cpu)[tid] = std::atoi(p);
      }
    }
    closedir(dir);
    return out;
  }

  void UpdateThreads() {
    const auto now = std::chrono::steady_clock::now();
    std::map<int32_t, std::string> names;
    std::map<int32_t, int> last_cpu;
    std::map<int32_t, uint64_t> times = ReadThreadTimes(&names, &last_cpu);
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
    // A flip of the big-core switch (the launcher, or a `cvar` line) applies at the next look.
    if (tids == tids_ && REXCVAR_GET(wwe2k14_android_big_cores) == big_cores_was_) return;
    big_cores_was_ = REXCVAR_GET(wwe2k14_android_big_cores);
    PlaceThreads(tids);
    if (tids == tids_) return;
    std::string list;
    for (const auto& b : busy) {
      const auto cpu = last_cpu.find(b.second);
      list += fmt::format("{}{} ({:.0f}%, core {})", list.empty() ? "" : ", ", names[b.second], b.first,
                          cpu == last_cpu.end() ? -1 : cpu->second);
    }
    const int64_t target_ns = TargetNs();
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
      session_target_ns_ = target_ns;
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
  UpdateTargetFn update_target_ = nullptr;
  CloseFn close_ = nullptr;
  Report2Fn report2_ = nullptr;
  WdCreateFn wd_create_ = nullptr;
  WdSetFn wd_start_ = nullptr, wd_total_ = nullptr, wd_cpu_ = nullptr, wd_gpu_ = nullptr;
  AWorkDuration* work_ = nullptr;
  bool gpu2_failed_ = false, gpu2_logged_ = false;
  int64_t session_target_ns_ = 0;
  uint64_t big_mask_ = 0;
  bool big_cores_was_ = false;
  std::set<int32_t> placed_;
  std::chrono::steady_clock::time_point next_scan_{};
  std::chrono::steady_clock::time_point last_scan_time_{};
  std::map<int32_t, uint64_t> last_times_;
  std::vector<int32_t> tids_;
};

inline PerfHint g_perf_hint;

}  // namespace wwe2k14::perf_hint

#endif  // __ANDROID__
