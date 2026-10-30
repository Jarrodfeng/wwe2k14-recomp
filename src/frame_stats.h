// wwe2k14 - guest frame timing. The game calls VdSwap once per frame it presents (hooked in
// kernel_overrides.h), so the time between calls is the game's own frame time: what the player feels as
// slowdown, independent of how the host GPU presents. Logs a summary every few seconds plus every hitch,
// and writes every frame to logs/frames_NNN.csv (NNN = this run's wwe2k14_NNN.log).
// Session 73 (Jarrod: "maximal information" from testers' logs): the CSV also has each frame's game-thread CPU time,
// the GPU command thread's CPU time (Android / Linux), the GPU's own busy time (of the latest frame it finished, from
// the submissions' timestamps: a frame or two behind), the scene's draw height and a scene marker ("entrance start");
// every frame over wwe2k14_slow_frame_ms gets a "slow frame" line saying what happened in it (pipelines / images /
// shaders made on the spot, files opened, memory probes, GPU waits); Android logs the thermal status, headroom and
// the CPU clusters' clocks with every summary.
// Session 73, b77 (Jarrod's b76 log: each entrance's first ~2 s ran the GPU at 35-40 ms a frame, 6-13 ms after, with
// the CPU idle and no heat): the GPU's clock where the phone lets an app read it (kgsl / devfreq sysfs: CSV gpu_mhz,
// the slow-frame and thermal lines), texture loads per frame, and "keep warm": with --wwe2k14_gpu_keep_warm, GPU busy
// work in the in-game sequence (SDK KeepWarmWork: buffer fills, at most once per 1/60 s of wall time), so the phone's
// GPU governor does not drop the clock through a load and start the next scene at the bottom. The phone launcher
// turns it on.
// b78 (Jarrod's b77 log, report 118: the fixed 96 MB cost the GPU ~2 ms a frame, too little; the clock files are
// "Permission denied" on his phone; the entrance's GPU time rose in flat steps 8 -> 17.6 -> 37.7 ms while the GPU sat
// idle half of each frame - the governor seemingly lowering the clock): keep-warm now steers the GPU's busy share (its
// submissions' timestamp time per wall time, every 100 ms) towards wwe2k14_gpu_keep_warm_target_ms per 1/60 s: more
// fill while under it, less while over, none in a frame over 20 ms (a GPU-bound frame gets no extra work), up to
// wwe2k14_gpu_keep_warm_mb. The GPU waits are split by why (SDK WaitSite: the frame limit, copies / resolves, occlusion
// queries, readbacks, draws): the slow-frame lines and the CSV say which one held the frame.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#if defined(_WIN32)  // Session 63 (Android port): Windows / x86 only
#include <share.h>
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <rex/perf/frame_events.h>  // Session 73: what a frame did on the GPU side

#if !defined(_WIN32)
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <ctime>
#include <map>
#endif

#include "blackbox.h"     // Session 73: the first frame in the start-up black box
#include "dynamic_res.h"  // Session 73: each frame's length to the dynamic resolution

#if defined(__ANDROID__)
#include "android_perf_hint.h"  // Session 64: each frame's length to the phone's CPU governor
#include "android_driver.h"     // Session 72: the custom driver's trial file goes after a few hundred frames
#include "android_frame_rate.h"  // Session 73: the screen's refresh rate set for a 60 fps source
#endif

REXCVAR_DEFINE_INT32(wwe2k14_frame_summary_seconds, 5, "wwe2k14",
                     "Log a guest frame-time summary every N seconds (0 = off)");
REXCVAR_DEFINE_INT32(wwe2k14_hitch_ms, 100, "wwe2k14",
                     "Log every guest frame that takes at least this many milliseconds (0 = off)");
REXCVAR_DEFINE_BOOL(wwe2k14_frame_csv, true, "wwe2k14",
                    "Write every guest frame's time to logs/frames_NNN.csv");
REXCVAR_DEFINE_INT32(wwe2k14_slow_frame_ms, 33, "wwe2k14",
                     "Session 73: every guest frame at least this long gets a 'slow frame' line with what happened in "
                     "it (0 = off)");
REXCVAR_DEFINE_BOOL(wwe2k14_frame_gpu_time, true, "wwe2k14",
                    "Session 73: measure each frame's GPU time (two timestamp queries per submission) for the "
                    "per-frame CSV and the slow-frame lines");
REXCVAR_DEFINE_BOOL(wwe2k14_gpu_keep_warm, false, "wwe2k14",
                    "Session 73: GPU busy work in light in-game frames (loads), so a phone's GPU clock stays up for "
                    "the next scene (src/frame_stats.h; the phone launcher turns it on)");
REXCVAR_DEFINE_INT32(wwe2k14_gpu_keep_warm_mb, 768, "wwe2k14",
                     "Session 73: the most keep-warm work per 1/60 s, MB of GPU buffer fill")
    .range(16, 1024);
REXCVAR_DEFINE_DOUBLE(wwe2k14_gpu_keep_warm_target_ms, 10.0, "wwe2k14",
                      "Session 73 (b78): keep-warm aims the GPU at this many ms of work per 1/60 s");

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

// Session 73: what the game side did in the current frame (the slow-frame lines): files opened (kernel_overrides.h
// file hook), memory pool probes (MmAllocatePhysicalMemoryEx calls), and the scene marker for the CSV
// (session_log.h's scene changes, load_turbo.h's window ends).
inline std::atomic<uint32_t> g_frame_file_opens{0};
inline std::atomic<uint32_t> g_frame_mem_probes{0};
// Session 73: true while the game is in its in-game sequence (9: the entrances and the match; load_turbo.h sets it),
// the only place the keep-warm work runs.
inline std::atomic<bool> g_in_game_sequence{false};
inline std::mutex g_scene_marker_mutex;
inline std::string g_scene_marker;
inline void NoteScene(const std::string& what) {
  std::lock_guard<std::mutex> lock(g_scene_marker_mutex);
  if (!g_scene_marker.empty()) g_scene_marker += " / ";
  g_scene_marker += what;
}

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
    if (frame == 1) blackbox::Step("first frame: the game is running");  // Session 73
    if (frame == 600) blackbox::Step("600 frames in");
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
      rex::perf::frame_events::Get().measure_gpu.store(REXCVAR_GET(wwe2k14_frame_gpu_time), std::memory_order_relaxed);
      last_cpu_ns_ = ThreadCpuNs();
      OpenCsv();
      REXLOG_INFO("[wwe2k14][frames] first guest frame{}", csv_ ? " (per-frame CSV: " + csv_path_ + ")" : "");
      return;
    }
    const double ms = std::chrono::duration<double, std::milli>(now - last_).count();
    const double t = std::chrono::duration<double>(now - start_).count();
    last_ = now;
    if (!g_frame_time_ignore_next.load(std::memory_order_relaxed)) dynres::g_controller.OnFrame(ms);  // Session 73
#if defined(__ANDROID__)
    android_driver::OnFrame();
    android_frame_rate::g_frame_rate.OnFrame(frame);
#endif

    // Session 73: what this frame did.
    const uint64_t cpu_ns = ThreadCpuNs();
    const double game_cpu_ms = cpu_ns && last_cpu_ns_ ? double(cpu_ns - last_cpu_ns_) / 1e6 : -1.0;
    last_cpu_ns_ = cpu_ns;
    const uint64_t gpu_thread_ns = GpuThreadCpuNs();
    const double gpu_thread_ms =
        gpu_thread_ns && last_gpu_thread_ns_ ? double(gpu_thread_ns - last_gpu_thread_ns_) / 1e6 : -1.0;
    last_gpu_thread_ns_ = gpu_thread_ns;
    auto& events = rex::perf::frame_events::Get();
    double gpu_ms = -1.0;
    if (const uint64_t done = events.gpu_frames_done.load(std::memory_order_relaxed); done != last_gpu_frames_done_) {
      last_gpu_frames_done_ = done;
      gpu_ms = events.gpu_frame_us.load(std::memory_order_relaxed) / 1000.0;
      last_gpu_ms_ = gpu_ms;
    }
#if defined(__ANDROID__)
    // The frame's length and the GPU's time (b77: the hint session reports GPU time too where Android has it, API 35).
    if (!g_frame_time_ignore_next.load(std::memory_order_relaxed)) perf_hint::g_perf_hint.OnFrame(ms, last_gpu_ms_);
#endif
    KeepWarm(now, ms);
    const int gpu_mhz = GpuClockMhz();
    const uint32_t textures = events.textures.load(std::memory_order_relaxed);
    const uint64_t texture_kb = events.texture_kb.load(std::memory_order_relaxed);
    const uint64_t warm_kb = events.warm_kb_done.load(std::memory_order_relaxed);
    const uint32_t pipelines = events.pipelines.load(std::memory_order_relaxed);
    const uint32_t images = events.images.load(std::memory_order_relaxed);
    const uint32_t shaders = events.shaders.load(std::memory_order_relaxed);
    const uint64_t gpu_wait_us = events.gpu_wait_us.load(std::memory_order_relaxed);
    // b78: this frame's waits by why; the biggest for the CSV.
    constexpr uint32_t kSites = rex::perf::frame_events::kWaitSiteCount;
    double wait_ms_by[kSites];
    uint32_t wait_main = 0;
    for (uint32_t i = 0; i < kSites; ++i) {
      const uint64_t v = events.gpu_wait_us_by[i].load(std::memory_order_relaxed);
      wait_ms_by[i] = double(v - last_wait_us_by_[i]) / 1000.0;
      last_wait_us_by_[i] = v;
      if (wait_ms_by[i] > wait_ms_by[wait_main]) wait_main = i;
    }
    const double wait_ms = double(gpu_wait_us - last_gpu_wait_us_) / 1000.0;
    const uint32_t files = g_frame_file_opens.load(std::memory_order_relaxed);
    const uint32_t probes = g_frame_mem_probes.load(std::memory_order_relaxed);
    std::string scene;
    {
      std::lock_guard<std::mutex> scene_lock(g_scene_marker_mutex);
      scene.swap(g_scene_marker);
    }
    if (!scene.empty()) last_scene_ = scene, last_scene_t_ = t;
    const int rows = dynres::g_controller.Rows();
    if (csv_) {
      for (char& c : scene) c = (c == ',' || c == '\n') ? ';' : c;
      char gpu_text[24] = "", game_text[24] = "", gthread_text[24] = "";
      if (gpu_ms >= 0) std::snprintf(gpu_text, sizeof(gpu_text), "%.3f", gpu_ms);
      if (game_cpu_ms >= 0) std::snprintf(game_text, sizeof(game_text), "%.3f", game_cpu_ms);
      if (gpu_thread_ms >= 0) std::snprintf(gthread_text, sizeof(gthread_text), "%.3f", gpu_thread_ms);
      char mhz_text[16] = "";
      if (gpu_mhz > 0) std::snprintf(mhz_text, sizeof(mhz_text), "%d", gpu_mhz);
      std::fprintf(csv_, "%llu,%.3f,%.3f,%s,%s,%s,%d,%s,%u,%.1f,%.0f,%.2f,%s,%s\n",
                   static_cast<unsigned long long>(frame), t, ms, game_text, gthread_text, gpu_text, rows, mhz_text,
                   textures - last_textures_, double(texture_kb - last_texture_kb_) / 1024.0,
                   double(warm_kb - last_warm_kb_) / 1024.0, wait_ms,
                   wait_ms > 0.05 ? rex::perf::frame_events::kWaitSiteNames[wait_main] : "", scene.c_str());
    }
    const int32_t slow_ms = REXCVAR_GET(wwe2k14_slow_frame_ms);
    if (slow_ms > 0 && ms >= slow_ms && slow_logged_ < kMaxSlowLines) {
      ++slow_logged_;
      std::string gpu_part = gpu_ms >= 0 ? fmt::format("GPU {:.1f} ms", gpu_ms) : std::string("GPU -");
      if (gpu_mhz > 0) gpu_part += fmt::format(" at {} MHz", gpu_mhz);
      REXLOG_INFO("[wwe2k14][frames] slow frame {}: {:.0f} ms (t={:.1f} s): game CPU {}, GPU thread {}, {}; made: "
                  "{} pipelines, {} images, {} shaders, {} texture loads ({:.1f} MB); {} files opened, {} memory "
                  "probes; GPU waits {:.1f} ms{}{}{}{}",
                  frame, ms, t, game_cpu_ms >= 0 ? fmt::format("{:.1f} ms", game_cpu_ms) : std::string("-"),
                  gpu_thread_ms >= 0 ? fmt::format("{:.1f} ms", gpu_thread_ms) : std::string("-"), gpu_part,
                  pipelines - last_pipelines_, images - last_images_, shaders - last_shaders_,
                  textures - last_textures_, double(texture_kb - last_texture_kb_) / 1024.0, files - last_files_,
                  probes - last_probes_, wait_ms, WaitSplit(wait_ms_by),
                  warm_kb != last_warm_kb_ ? fmt::format("; keep-warm {:.0f} MB", (warm_kb - last_warm_kb_) / 1024.0)
                                           : std::string(),
                  last_scene_.empty() ? "" : fmt::format("; {:.1f} s after {}", t - last_scene_t_, last_scene_),
                  slow_logged_ == kMaxSlowLines ? " (further slow frames only in the CSV)" : "");
    }
    last_pipelines_ = pipelines, last_images_ = images, last_shaders_ = shaders;
    last_files_ = files, last_probes_ = probes, last_gpu_wait_us_ = gpu_wait_us;
    last_textures_ = textures, last_texture_kb_ = texture_kb, last_warm_kb_ = warm_kb;

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
      LogThermal(t);  // Session 73 (Android)
      window_start_ = now;
      window_first_frame_ = frame + 1;
      window_ms_.clear();
      window_hitches_ = 0;
      if (csv_) std::fflush(csv_);
    }
  }

 private:
  static constexpr uint32_t kMaxHitchLines = 2000;
  static constexpr uint32_t kMaxSlowLines = 4000;

  // Session 73: this thread's CPU time (the game's swapping thread), ns; 0 = unknown.
  static uint64_t ThreadCpuNs() {
#if defined(_WIN32)
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0;
    const auto v = [](const FILETIME& f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (v(kernel) + v(user)) * 100;
#else
    timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) return 0;
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#endif
  }

  // Session 73: the GPU command thread's CPU time (Android / Linux: /proc/self/task/TID/schedstat, ns); 0 = unknown.
  // Found by name ("GPU Commands", cut to 15 characters) once, retried every 300 frames until found.
  uint64_t GpuThreadCpuNs() {
#if defined(_WIN32)
    return 0;
#else
    if (gpu_thread_fd_ < 0) {
      if (g_guest_frame.load(std::memory_order_relaxed) < gpu_thread_retry_frame_) return 0;
      gpu_thread_retry_frame_ = g_guest_frame.load(std::memory_order_relaxed) + 300;
      std::error_code ec;
      for (const auto& e : std::filesystem::directory_iterator("/proc/self/task", ec)) {
        char name[64] = {};
        if (FILE* f = std::fopen((e.path() / "comm").c_str(), "r")) {
          if (!std::fgets(name, sizeof(name), f)) name[0] = 0;
          std::fclose(f);
        }
        if (std::strncmp(name, "GPU Commands", 12) == 0) {
          gpu_thread_fd_ = ::open((e.path() / "schedstat").c_str(), O_RDONLY | O_CLOEXEC);
          break;
        }
      }
      if (gpu_thread_fd_ < 0) return 0;
    }
    char buf[96];
    const ssize_t n = ::pread(gpu_thread_fd_, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return 0;
    buf[n] = 0;
    return std::strtoull(buf, nullptr, 10);
#endif
  }

  // Session 73: Android's thermal status and headroom (AThermal, API 30 / 31, by dlsym) and the CPU clusters' clocks
  // now, with every summary: "[wwe2k14][thermal] t=125 s: status 2 (moderate), headroom 0.83; CPU 0-5 2.21 GHz, ...".
  void LogThermal(double t) {
#if defined(__ANDROID__)
    struct AThermalManager;
    using AcquireFn = AThermalManager* (*)();
    using StatusFn = int (*)(AThermalManager*);
    using HeadroomFn = float (*)(AThermalManager*, int);
    static void* lib = dlopen("libandroid.so", RTLD_NOW);
    static auto acquire = lib ? reinterpret_cast<AcquireFn>(dlsym(lib, "AThermal_acquireManager")) : nullptr;
    static auto status_fn = lib ? reinterpret_cast<StatusFn>(dlsym(lib, "AThermal_getCurrentThermalStatus")) : nullptr;
    static auto headroom_fn = lib ? reinterpret_cast<HeadroomFn>(dlsym(lib, "AThermal_getThermalHeadroom")) : nullptr;
    static AThermalManager* manager = acquire ? acquire() : nullptr;
    static constexpr const char* kStatus[] = {"none", "light", "moderate", "severe", "critical", "emergency",
                                              "shutdown"};
    std::string text = fmt::format("[wwe2k14][thermal] t={:.0f} s:", t);
    if (manager && status_fn) {
      const int status = status_fn(manager);
      text += fmt::format(" status {} ({})", status, status >= 0 && status <= 6 ? kStatus[status] : "?");
    } else {
      text += " status -";
    }
    if (manager && headroom_fn) {
      const float headroom = headroom_fn(manager, 0);  // (now; NaN when not known yet)
      if (headroom == headroom) text += fmt::format(", headroom {:.2f}", headroom);
    }
    // The clusters: CPUs grouped by their top clock, each group's current clock.
    static std::map<unsigned, std::vector<int>> clusters = [] {
      std::map<unsigned, std::vector<int>> by_max;
      const long cpus = sysconf(_SC_NPROCESSORS_CONF);
      for (int cpu = 0; cpu < cpus && cpu < 64; ++cpu) {
        unsigned khz = 0;
        if (FILE* f = std::fopen(fmt::format("/sys/devices/system/cpu/cpu{}/cpufreq/cpuinfo_max_freq", cpu).c_str(),
                                 "r")) {
          if (std::fscanf(f, "%u", &khz) != 1) khz = 0;
          std::fclose(f);
        }
        by_max[khz].push_back(cpu);
      }
      return by_max;
    }();
    std::string cpu_text;
    for (const auto& [max_khz, list] : clusters) {
      unsigned khz = 0;
      if (FILE* f = std::fopen(
              fmt::format("/sys/devices/system/cpu/cpu{}/cpufreq/scaling_cur_freq", list.front()).c_str(), "r")) {
        if (std::fscanf(f, "%u", &khz) != 1) khz = 0;
        std::fclose(f);
      }
      cpu_text += fmt::format("{}{}-{} {:.2f} GHz", cpu_text.empty() ? "; CPU " : ", ", list.front(), list.back(),
                              khz / 1e6);
    }
    std::string gpu_text;
    if (const int mhz = GpuClockMhz(); mhz > 0) gpu_text = fmt::format("; GPU {} MHz", mhz);
    if (const int busy = GpuBusyPercent(); busy >= 0) {
      gpu_text += fmt::format("{}busy {}%", gpu_text.empty() ? "; GPU " : ", ", busy);
    }
    {
      const auto now = std::chrono::steady_clock::now();
      const uint64_t busy = rex::perf::frame_events::Get().gpu_busy_us_total.load(std::memory_order_relaxed);
      if (thermal_busy_time_ != std::chrono::steady_clock::time_point{}) {
        const double wall_us = std::chrono::duration<double, std::micro>(now - thermal_busy_time_).count();
        if (wall_us > 0) {
          gpu_text += fmt::format("{}GPU busy {:.0f}% (timestamps)", gpu_text.empty() ? "; " : ", ",
                                  100.0 * double(busy - thermal_busy_us_) / wall_us);
        }
      }
      thermal_busy_us_ = busy;
      thermal_busy_time_ = now;
    }
    const uint64_t warm_kb = rex::perf::frame_events::Get().warm_kb_done.load(std::memory_order_relaxed);
    if (warm_kb != thermal_warm_kb_) {
      gpu_text += fmt::format("; keep-warm {:.0f} MB since the last line", (warm_kb - thermal_warm_kb_) / 1024.0);
      thermal_warm_kb_ = warm_kb;
    }
    REXLOG_INFO("{}{}{}", text, cpu_text, gpu_text);
#else
    (void)t;
#endif
  }

  // b78: " (frame 40.1, copy 3.2)": the frame's GPU waits by why, the ones over 0.05 ms.
  static std::string WaitSplit(const double* ms_by) {
    std::string out;
    for (uint32_t i = 0; i < rex::perf::frame_events::kWaitSiteCount; ++i) {
      if (ms_by[i] < 0.05) continue;
      out += fmt::format("{}{} {:.1f}", out.empty() ? " (" : ", ", rex::perf::frame_events::kWaitSiteNames[i], ms_by[i]);
    }
    return out.empty() ? out : out + ")";
  }

  // b78: keep-warm work (see the top of the file). Every 100 ms of wall time the GPU's busy share (its submissions'
  // timestamp time) is compared with the target; the fill grows 25% while under it and shrinks 20% while over.
  void KeepWarm(std::chrono::steady_clock::time_point now, double frame_ms) {
    if (!REXCVAR_GET(wwe2k14_gpu_keep_warm)) return;
    auto& events = rex::perf::frame_events::Get();
    const bool in_game = g_in_game_sequence.load(std::memory_order_relaxed);
    const bool light_frame = frame_ms <= 20.0;  // no extra work in a GPU-bound (or otherwise slow) frame
    if (warm_window_start_ == std::chrono::steady_clock::time_point{}) {
      warm_window_start_ = now;
      warm_busy_start_ = events.gpu_busy_us_total.load(std::memory_order_relaxed);
    }
    const double wall_us = std::chrono::duration<double, std::micro>(now - warm_window_start_).count();
    if (wall_us >= 100000.0) {
      const uint64_t busy = events.gpu_busy_us_total.load(std::memory_order_relaxed);
      const double share = double(busy - warm_busy_start_) / wall_us;
      warm_window_start_ = now;
      warm_busy_start_ = busy;
      const double want = REXCVAR_GET(wwe2k14_gpu_keep_warm_target_ms) / (1000.0 / 60.0);
      const double most = double(REXCVAR_GET(wwe2k14_gpu_keep_warm_mb));
      if (!in_game) {
        warm_mb_ = 0.0;
      } else if (share < want * 0.9 && light_frame) {
        warm_mb_ = std::min(most, warm_mb_ < 16.0 ? 16.0 : warm_mb_ * 1.25);
      } else if (share > want * 1.1) {
        warm_mb_ *= 0.8;
        if (warm_mb_ < 8.0) warm_mb_ = 0.0;
      }
      const bool on = warm_mb_ > 0.0;
      if (on != warm_on_ && ++warm_lines_ <= 200) {
        REXLOG_INFO("[wwe2k14][warm] keep-warm {} at frame {} (GPU busy {:.0f}% of the last 0.1 s, aiming at {:.0f}%)",
                    on ? "on" : "off", g_guest_frame.load(std::memory_order_relaxed), share * 100.0, want * 100.0);
      }
      warm_on_ = on;
    }
    // At most once per 1/60 s of wall time: fast-forwarded loads run hundreds of frames a second.
    if (!warm_on_ || !in_game || !light_frame || now - last_warm_ < std::chrono::microseconds(15000)) return;
    last_warm_ = now;
    events.warm_kb.store(uint32_t(warm_mb_ * 1024.0), std::memory_order_relaxed);
  }

  // b77: the GPU's clock (MHz) and busy share (%), where the phone lets an app read them (Qualcomm kgsl, devfreq,
  // some vendors' /sys/kernel/gpu); -1 = unknown. The files are found once (the first call logs which, or why none).
  int GpuClockMhz() {
#if defined(_WIN32)
    return -1;
#else
    FindGpuFiles();
    const long long v = ReadNumber(gpu_clock_fd_);
    if (v <= 0) return -1;
    return int(v >= 1000000 ? v / 1000000 : v >= 10000 ? v / 1000 : v);  // Hz, kHz or MHz
#endif
  }
  int GpuBusyPercent() {
#if defined(_WIN32)
    return -1;
#else
    FindGpuFiles();
    const long long v = ReadNumber(gpu_busy_fd_);
    return v >= 0 && v <= 100 ? int(v) : -1;
#endif
  }
#if !defined(_WIN32)
  static long long ReadNumber(int fd) {
    if (fd < 0) return -1;
    char buf[64];
    const ssize_t n = ::pread(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return -1;
    buf[n] = 0;
    char* end = nullptr;
    const long long v = std::strtoll(buf, &end, 10);
    return end == buf ? -1 : v;
  }
  void FindGpuFiles() {
    if (gpu_files_looked_) return;
    gpu_files_looked_ = true;
    std::vector<std::string> clocks = {"/sys/class/kgsl/kgsl-3d0/gpuclk", "/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq",
                                       "/sys/kernel/gpu/gpu_clock"};
    std::vector<std::string> busies = {"/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage", "/sys/kernel/gpu/gpu_busy"};
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/sys/class/devfreq", ec)) {
      const std::string name = e.path().filename().string();
      if (name.find("gpu") != std::string::npos || name.find("kgsl") != std::string::npos ||
          name.find("mali") != std::string::npos || name.find("g3d") != std::string::npos) {
        clocks.push_back((e.path() / "cur_freq").string());
      }
    }
    std::string tried;
    for (const auto& p : clocks) {
      const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd >= 0 && ReadNumber(fd) > 0) {
        gpu_clock_fd_ = fd;
        gpu_clock_path_ = p;
        break;
      }
      tried += fmt::format("{}{} ({})", tried.empty() ? "" : ", ", p, fd < 0 ? std::strerror(errno) : "no number");
      if (fd >= 0) ::close(fd);
    }
    for (const auto& p : busies) {
      const int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd >= 0 && ReadNumber(fd) >= 0) {
        gpu_busy_fd_ = fd;
        break;
      }
      if (fd >= 0) ::close(fd);
    }
    if (gpu_clock_fd_ >= 0) {
      REXLOG_INFO("[wwe2k14][frames] GPU clock from {} ({} MHz now){}", gpu_clock_path_, GpuClockMhz(),
                  gpu_busy_fd_ >= 0 ? ", busy share readable" : "");
    } else {
      REXLOG_INFO("[wwe2k14][frames] the GPU clock is not readable on this device: {}",
                  tried.empty() ? std::string("no candidate files") : tried);
    }
  }
#endif

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
    if (csv_) {
      std::fputs("frame,t_seconds,frame_ms,game_cpu_ms,gpu_thread_cpu_ms,gpu_ms,draw_rows,gpu_mhz,textures,texture_mb,"
                 "warm_mb,gpu_wait_ms,gpu_wait_main,scene\n",
                 csv_);
    }
  }

  std::mutex mutex_;
  std::chrono::steady_clock::time_point start_, last_, window_start_;
  uint64_t window_first_frame_ = 0;
  std::vector<double> window_ms_;
  uint32_t window_hitches_ = 0;
  uint32_t hitches_logged_ = 0;
  bool other_swapper_logged_ = false;
  // Session 73
  uint64_t last_cpu_ns_ = 0, last_gpu_thread_ns_ = 0, last_gpu_frames_done_ = 0, last_gpu_wait_us_ = 0;
  uint32_t last_pipelines_ = 0, last_images_ = 0, last_shaders_ = 0, last_files_ = 0, last_probes_ = 0;
  uint32_t slow_logged_ = 0;
  std::string last_scene_;
  double last_scene_t_ = 0.0;
  int gpu_thread_fd_ = -1;
  uint64_t gpu_thread_retry_frame_ = 0;
  // b77
  double last_gpu_ms_ = -1.0;
  uint32_t last_textures_ = 0;
  uint64_t last_texture_kb_ = 0, last_warm_kb_ = 0, thermal_warm_kb_ = 0;
  bool warm_on_ = false;
  double warm_mb_ = 0.0;
  std::chrono::steady_clock::time_point warm_window_start_{};
  uint64_t warm_busy_start_ = 0, thermal_busy_us_ = 0;
  std::chrono::steady_clock::time_point thermal_busy_time_{};
  uint64_t last_wait_us_by_[rex::perf::frame_events::kWaitSiteCount] = {};
  uint32_t warm_lines_ = 0;
  std::chrono::steady_clock::time_point last_warm_{};
  bool gpu_files_looked_ = false;
  int gpu_clock_fd_ = -1, gpu_busy_fd_ = -1;
  std::string gpu_clock_path_;
  std::FILE* csv_ = nullptr;
  std::string csv_path_;
};

inline FrameStats g_frame_stats;

}  // namespace wwe2k14
