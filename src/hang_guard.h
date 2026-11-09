#pragma once
// wwe2k14 - Session 95, piece HANG (out\s95\android_wide.md section 7): every freeze, black screen and memory climb gets a name.
//
// One thread (lifted from the diagnostic run's watchdog, src/diag.h), default on for Android, off for precompile runs and when
// the diagnostic run's own watchdog runs. It counts its own 1 s ticks while the app is in front, never clock time, and starts
// over on return and after any sleep that measured over 5 s (Android's app freezer). The longest foreground gap seen is 1.9 s.
//
// Lines, in logs/startup.txt and the game log ([hang]). None may hold the words LastRun looks for ("first frame", "memory low",
// "quit to the launcher", "step: "):
//   HANG? no guest frame / no present for 20 s      something stopped, not named yet
//   HANG: <side>, ...                               60 s (120 s before the first frame), and logs/hang.txt
//   HANG over                                       it went on after all: the two lines above no longer stand
//   BLACK: no present ...                           guest frames rise, presents do not, 10 s (r021's return; S92's black)
//   BLACK: dark picture ...                         the guest output was dark in all 3 captures, 30, 60, 120 s in
//   BLACK over                                      presents came back
// Side: "fence wait", "paint thread", "command thread" or "guest frame" (Side()). The GPU side's signs of life come from the SDK
// (rex/perf/hang_beats.h). The launcher reads the lines (phone_check.h LastRun).
//
// The captures are the only work here that can block (they queue GPU work), so each runs on a thread of its own: a hung GPU must
// not stop this thread from naming it. They run only while presents arrive, at most 3 per run. They cannot see a fault after the
// guest output image: "no present" covers the present itself; a broken final pass is unseen.
//
// Memory, every 10 s from this thread, in or out of front: logs/mem_NNN.csv and a black box line: RssAnon, RssFile, RssShmem,
// VmHWM, MemAvailable, the Vulkan budget. The mapping count once, on the first trim. On the PC the columns come from
// GetProcessMemoryInfo (rss_anon = private bytes, rss_file = working set, vm_hwm = peak working set, rss_shmem = 0).
//
// --wwe2k14_hang_exit (default off): after HANG: write logs/exit_request.txt (GameActivity brings the launcher back), wait 4 s,
// _Exit(3). Off until the phone list passes.
//
// Test only: --wwe2k14_test_hang=guest|paint|fence stops that side at guest frame --wwe2k14_test_hang_at, for good.
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/hang_beats.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "android_lifecycle.h"
#include "blackbox.h"

#if defined(_WIN32)
#include "platform/compat.h"  // <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__ANDROID__)
REXCVAR_DEFINE_BOOL(wwe2k14_hang_guard, true, "wwe2k14",
                    "Session 95: names freezes, black pictures and memory in logs/startup.txt, hang.txt and mem_NNN.csv "
                    "(src\\hang_guard.h). Off for precompile and diagnostic runs");
#else
REXCVAR_DEFINE_BOOL(wwe2k14_hang_guard, false, "wwe2k14",
                    "Session 95: names freezes, black pictures and memory in logs/startup.txt, hang.txt and mem_NNN.csv "
                    "(src\\hang_guard.h). Off on the PC by default");
#endif
REXCVAR_DEFINE_BOOL(wwe2k14_hang_exit, false, "wwe2k14",
                    "Session 95: after a HANG: line ask for the launcher and end the process (exit code 3). Off until the "
                    "phone list passes");
REXCVAR_DEFINE_STRING(wwe2k14_test_hang, "", "wwe2k14",
                      "Session 95 (test only): guest, paint or fence: that side stops for good at --wwe2k14_test_hang_at");
REXCVAR_DEFINE_INT32(wwe2k14_test_hang_at, 600, "wwe2k14", "Session 95 (test only): the guest frame --wwe2k14_test_hang acts at");

namespace wwe2k14::hang_guard {

using Clock = std::chrono::steady_clock;
using Capture = std::function<bool(rex::ui::RawImage&)>;

// Limits, in the guard's own ticks.
constexpr uint32_t kSuspectTicks = 20, kHangTicks = 60, kHangBootTicks = 120;
constexpr uint32_t kBlackTicks = 10;
constexpr uint32_t kFreshTicks = 3;   // a beat this recent still counts as alive (a 1 s fence step can miss one tick)
constexpr uint32_t kStoppedTicks = 5;  // a thread this quiet counts as stopped
constexpr uint32_t kMemoryTicks = 10;
constexpr uint32_t kCaptureAt[3] = {30, 60, 120};
constexpr uint64_t kStderrLimit = 8ull << 20;
// A frame ahead of the last present is the pipeline, not a fault: a stopped guest leaves at most one.
constexpr uint64_t kBlackFrames = 2;

enum TestSide : int { kTestNone = 0, kTestGuest, kTestPaint, kTestFence };
inline std::atomic<int> g_test{kTestNone};
inline std::atomic<bool> g_test_done{false};
inline std::atomic<bool> g_trim_seen{false};

// Android's memory warnings (android_memory.h).
inline void NoteTrim() { g_trim_seen.store(true, std::memory_order_relaxed); }

// The guest's swap, before FrameStats takes its lock. One relaxed load unless a test is on.
inline void AtFrameBoundary(uint64_t frame) {
  const int test = g_test.load(std::memory_order_relaxed);
  if (test == kTestNone || frame < uint64_t(std::max(1, REXCVAR_GET(wwe2k14_test_hang_at)))) return;
  if (g_test_done.exchange(true)) return;
  REXLOG_WARN("[hang] test: stopping the {} side at guest frame {}", REXCVAR_GET(wwe2k14_test_hang), frame);
  auto& side = rex::perf::hang_beats::Get().test_side;
  if (test == kTestPaint) side.store(rex::perf::hang_beats::kTestPaint, std::memory_order_relaxed);
  if (test == kTestFence) side.store(rex::perf::hang_beats::kTestFence, std::memory_order_relaxed);
  while (test == kTestGuest) std::this_thread::sleep_for(std::chrono::seconds(1));
}

// A line for both records.
inline void Say(const char* format, ...) {
  char line[400];
  va_list args;
  va_start(args, format);
  std::vsnprintf(line, sizeof(line), format, args);
  va_end(args);
  blackbox::Note("%s", line);
  REXLOG_WARN("[hang] {}", line);
}

struct Sample {
  uint64_t guest = 0, command = 0, paints = 0, presents = 0, fence = 0;
};

inline Sample Read() {
  const auto& b = rex::perf::hang_beats::Get();
  constexpr auto r = std::memory_order_relaxed;
  Sample s;
  s.guest = android_lifecycle::g_frames_seen.load(r);
  s.command = b.command.load(r);
  s.paints = b.paints.load(r);
  s.presents = b.presents.load(r);
  s.fence = b.fence_steps.load(r);
  return s;
}

// ---- memory

struct Memory {
  long long anon_kb = -1, file_kb = -1, shmem_kb = -1, hwm_kb = -1, available_kb = -1;
};

#if defined(_WIN32)
inline Memory ReadMemory() {
  Memory m;
  PROCESS_MEMORY_COUNTERS_EX pmc = {};
  pmc.cb = sizeof(pmc);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
    m.anon_kb = (long long)(pmc.PrivateUsage >> 10);
    m.file_kb = (long long)(pmc.WorkingSetSize >> 10);
    m.shmem_kb = 0;
    m.hwm_kb = (long long)(pmc.PeakWorkingSetSize >> 10);
  }
  MEMORYSTATUSEX status = {};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status)) m.available_kb = (long long)(status.ullAvailPhys >> 10);
  return m;
}
inline int CountMappings() { return -1; }
inline void StderrLimit() {}
#else
inline Memory ReadMemory() {
  Memory m;
  char line[160];
  if (FILE* f = std::fopen("/proc/self/status", "r")) {
    while (std::fgets(line, sizeof(line), f)) {
      long long v = 0;
      if (std::sscanf(line, "RssAnon: %lld", &v) == 1) m.anon_kb = v;
      else if (std::sscanf(line, "RssFile: %lld", &v) == 1) m.file_kb = v;
      else if (std::sscanf(line, "RssShmem: %lld", &v) == 1) m.shmem_kb = v;
      else if (std::sscanf(line, "VmHWM: %lld", &v) == 1) m.hwm_kb = v;
    }
    std::fclose(f);
  }
  if (FILE* f = std::fopen("/proc/meminfo", "r")) {
    while (std::fgets(line, sizeof(line), f)) {
      long long v = 0;
      if (std::sscanf(line, "MemAvailable: %lld", &v) == 1) {
        m.available_kb = v;
        break;
      }
    }
    std::fclose(f);
  }
  return m;
}
inline int CountMappings() {
  FILE* f = std::fopen("/proc/self/maps", "r");
  if (!f) return -1;
  int lines = 0;
  char buffer[4096];
  size_t n;
  while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) lines += int(std::count(buffer, buffer + n, '\n'));
  std::fclose(f);
  return lines;
}
// A driver that talks to stderr must not fill the phone (blackbox.h points stderr at logs/stderr.txt). Once only.
inline void StderrLimit() {
  struct stat st = {};
  if (fstat(STDERR_FILENO, &st) != 0 || !S_ISREG(st.st_mode) || uint64_t(st.st_size) <= kStderrLimit) return;
  // (F6) truncate, not redirect: the file is O_APPEND, so a later FATAL line still lands in it.
  if (::ftruncate(STDERR_FILENO, 0) != 0) return;
  Say("stderr.txt passed 8 MB: cut to empty");
}
#endif

// ---- the picture

// Bright = any channel over 24 of 255, on a grid of about 2300 points. Dark = under 0.5% of them bright: a fade, a logo or a
// scoreboard on black all count as a picture.
inline bool IsDark(const rex::ui::RawImage& image, uint32_t& bright, uint32_t& points) {
  bright = points = 0;
  for (uint32_t gy = 0; gy < 36; ++gy) {
    const uint32_t y = image.height * gy / 36 + image.height / 72;
    for (uint32_t gx = 0; gx < 64; ++gx) {
      const uint32_t x = image.width * gx / 64 + image.width / 128;
      const uint8_t* p = image.data.data() + size_t(y) * image.stride + size_t(x) * 4;
      bright += (p[0] > 24 || p[1] > 24 || p[2] > 24) ? 1 : 0;
      ++points;
    }
  }
  return uint64_t(bright) * 200 < points;
}

class Guard {
 public:
  Guard(int log_number, Capture capture) : capture_(std::move(capture)) {
    char name[32];
    if (log_number >= 0) {
      std::snprintf(name, sizeof(name), "logs/mem_%03d.csv", log_number);
      csv_ = std::fopen(name, "w");
    }
    if (csv_) {
      std::fputs("t_seconds,guest_frame,rss_anon_kb,rss_file_kb,rss_shmem_kb,vm_hwm_kb,mem_available_kb,vk_used_mb,vk_budget_mb\n", csv_);
      std::fflush(csv_);
    }
  }

  void Run() {
    Reset(Read());
    bool was_out = false;
    for (;;) {
      const auto t0 = Clock::now();
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const auto slept = Clock::now() - t0;
      if (++memory_ticks_ >= kMemoryTicks) {
        memory_ticks_ = 0;
        WriteMemory();
      }
      if (g_trim_seen.exchange(false, std::memory_order_relaxed) && !mappings_logged_) {
        mappings_logged_ = true;
        Say("mem: %d mappings at the first memory warning", CountMappings());
      }
      if (!android_lifecycle::InFront()) {
        was_out = true;
        continue;
      }
      if (was_out || slept > std::chrono::seconds(5)) {
        was_out = false;
        Restart();
        continue;
      }
      Tick();
    }
  }

 private:
  void Reset(const Sample& s) {
    last_ = s;
    guest_ticks_ = command_ticks_ = paint_ticks_ = present_ticks_ = fence_ticks_ = 0;
    guest_at_present_ = s.guest;
  }

  // Back in front, or awake after the freezer: nothing measured before counts.
  void Restart() {
    // (F3) a freeze or black picture already named stays named: only the counters restart.
    // suspected_, named_, black_ kept
    Reset(Read());
  }

  static void Count(uint64_t now, uint64_t& last, uint32_t& ticks) {
    ticks = now == last ? ticks + 1 : 0;
    last = now;
  }

  // Which side stopped. A fence wait still stepping is the plainest evidence, then a thread that went into the paint and never
  // came out, then presents gone while the guest runs on. A quiet command thread comes last: it sits still behind any of those.
  const char* Side(const Sample& s) const {
    const uint32_t in_paint = rex::perf::hang_beats::Get().in_paint.load(std::memory_order_relaxed);
    if (s.fence > 0 && fence_ticks_ < kFreshTicks && guest_ticks_ >= kStoppedTicks) return "fence wait";
    if (in_paint && paint_ticks_ >= kStoppedTicks) return "paint thread";
    if (s.guest > 0 && guest_ticks_ < kStoppedTicks) return "paint thread";
    if (command_ticks_ >= kStoppedTicks) return "command thread";
    return "guest frame";
  }

  void Tick() {
    const Sample s = Read();
    if (s.presents != last_.presents) guest_at_present_ = s.guest;
    Count(s.guest, last_.guest, guest_ticks_);
    Count(s.command, last_.command, command_ticks_);
    Count(s.paints, last_.paints, paint_ticks_);
    Count(s.presents, last_.presents, present_ticks_);
    Count(s.fence, last_.fence, fence_ticks_);
    const bool playing = s.guest > 0;
    if (!playing) present_ticks_ = 0;  // a long start says nothing about presents

    if (playing && !black_ && present_ticks_ >= kBlackTicks && s.guest - guest_at_present_ >= kBlackFrames) {
      black_ = true;
      Say("BLACK: no present for %u s, %llu guest frames ran (presents %llu, acquire timeouts %llu)", present_ticks_,
          (unsigned long long)(s.guest - guest_at_present_), (unsigned long long)s.presents,
          (unsigned long long)rex::perf::hang_beats::Get().acquire_timeouts.load(std::memory_order_relaxed));
    } else if (black_ && present_ticks_ == 0) {
      black_ = false;
      Say("BLACK over: presents came back");
    }

    const uint32_t stall = playing ? std::max(guest_ticks_, present_ticks_) : guest_ticks_;
    const char* what = guest_ticks_ >= present_ticks_ || !playing ? "guest frame" : "present";
    if (stall >= kSuspectTicks && !suspected_) {
      suspected_ = true;
      Say("HANG? no %s for %u s, guest frame %llu", what, stall, (unsigned long long)s.guest);
    }
    if (stall >= (playing ? kHangTicks : kHangBootTicks) && !named_) {
      named_ = true;
      Name(s, stall, what);
    }
    if (suspected_ && stall == 0) {
      suspected_ = named_ = false;
      Say("HANG over: guest frames and presents go on, guest frame %llu", (unsigned long long)s.guest);
    }
    if (playing) Pictures();
  }

  void Name(const Sample& s, uint32_t stall, const char* what) {
    const char* side = Side(s);
    char why[200];
    std::snprintf(why, sizeof(why), "%s, no %s for %u s, guest frame %llu%s", side, what, stall, (unsigned long long)s.guest,
                  s.guest ? "" : " (still starting)");
    Say("HANG: %s", why);
    if (FILE* f = std::fopen("logs/hang.txt", "wb")) {
      const auto& b = rex::perf::hang_beats::Get();
      std::fprintf(f, "HANG: %s\nticks since: guest frame %u, command thread %u, paint %u, present %u, fence step %u\n"
                      "counts: guest frames %llu, paints %llu, presents %llu, fence steps %llu, acquire timeouts %llu, in paint %u\n",
                   why, guest_ticks_, command_ticks_, paint_ticks_, present_ticks_, fence_ticks_, (unsigned long long)s.guest,
                   (unsigned long long)s.paints, (unsigned long long)s.presents, (unsigned long long)s.fence,
                   (unsigned long long)b.acquire_timeouts.load(std::memory_order_relaxed),
                   b.in_paint.load(std::memory_order_relaxed));
      std::fclose(f);
    }
    if (!REXCVAR_GET(wwe2k14_hang_exit)) return;
    // (F7) the request first: a log call or flush can block on a lock the frozen thread holds.
    android_lifecycle::WriteExitRequest("the game froze");
    std::thread([] {
      Say("hang exit: back to the launcher");
      rex::FlushLogging();
    }).detach();
    std::this_thread::sleep_for(std::chrono::seconds(4));
    std::_Exit(3);
  }

  void Pictures() {
    ++picture_ticks_;
    if (capture_index_ >= 3 || picture_ticks_ < kCaptureAt[capture_index_] || !capture_ || present_ticks_ != 0) return;
    if (capture_busy_.exchange(true)) return;
    const uint32_t at = kCaptureAt[capture_index_++];
    const bool last = capture_index_ == 3;
    std::thread([this, at, last] {
      rex::ui::RawImage image;
      const auto t0 = Clock::now();
      const bool ok = capture_(image) && image.width && image.height;
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
      uint32_t bright = 0, points = 0;
      const bool dark = ok && IsDark(image, bright, points);
      dark_captures_ += dark ? 1 : 0;
      REXLOG_INFO("[hang] picture {} s in: {}, {} of {} points bright, {} ms", at, ok ? (dark ? "dark" : "fine") : "no capture",
                  bright, points, ms);
      if (last && dark_captures_.load() == 3) Say("BLACK: dark picture, all 3 captures, 30 to 120 s in");
      capture_busy_.store(false);
    }).detach();
  }

  void WriteMemory() {
    const Memory m = ReadMemory();
    const auto& b = rex::perf::hang_beats::Get();
    constexpr auto r = std::memory_order_relaxed;
    const unsigned long long used = b.vk_used_mb.load(r), budget = b.vk_budget_mb.load(r);
    const unsigned long long frame = android_lifecycle::g_frames_seen.load(r);
    const double t = std::chrono::duration<double>(Clock::now() - blackbox::g_t0).count();
    if (csv_) {
      std::fprintf(csv_, "%.1f,%llu,%lld,%lld,%lld,%lld,%lld,%llu,%llu\n", t, frame, m.anon_kb, m.file_kb, m.shmem_kb, m.hwm_kb,
                   m.available_kb, used, budget);
      std::fflush(csv_);
    }
    blackbox::Note("mem: anon %lld file %lld shmem %lld hwm %lld available %lld KB, vulkan %llu of %llu MB, guest frame %llu",
                   m.anon_kb, m.file_kb, m.shmem_kb, m.hwm_kb, m.available_kb, used, budget, frame);
    StderrLimit();
  }

  Capture capture_;
  FILE* csv_ = nullptr;
  Sample last_;
  uint32_t guest_ticks_ = 0, command_ticks_ = 0, paint_ticks_ = 0, present_ticks_ = 0, fence_ticks_ = 0;
  uint64_t guest_at_present_ = 0;
  uint32_t memory_ticks_ = 0, picture_ticks_ = 0, capture_index_ = 0;
  bool suspected_ = false, named_ = false, black_ = false, mappings_logged_ = false;
  std::atomic<bool> capture_busy_{false};
  std::atomic<uint32_t> dark_captures_{0};
};

// Once, before the first guest frame. The guard lives as long as the process, which ends with _Exit.
inline void Start(int log_number, Capture capture) {
  if (!REXCVAR_GET(wwe2k14_hang_guard)) return;
  // (Session 98 phone fixes: + the tron export - it holds the guest in one VdSwap for minutes on purpose, src/tron_export.h)
  if (!rex::cvar::GetFlagByName("wwe2k14_precompile").empty() || !rex::cvar::GetFlagByName("wwe2k14_diag").empty() ||
      !rex::cvar::GetFlagByName("wwe2k14_tron_export").empty()) {
    REXLOG_INFO("[hang] guard off: a precompile, diagnostic or tron export run");
    return;
  }
  const std::string test = REXCVAR_GET(wwe2k14_test_hang);
  g_test.store(test == "guest" ? kTestGuest : test == "paint" ? kTestPaint : test == "fence" ? kTestFence : kTestNone,
               std::memory_order_relaxed);
  rex::perf::hang_beats::Get().want_budget.store(true, std::memory_order_relaxed);
  std::remove("logs/hang.txt");
  REXLOG_INFO("[hang] guard on: HANG? {} s, HANG: {} s ({} s before the first frame), exit {}", kSuspectTicks, kHangTicks,
              kHangBootTicks, REXCVAR_GET(wwe2k14_hang_exit) ? "on" : "off");
  auto* guard = new Guard(log_number, std::move(capture));
  std::thread([guard] { guard->Run(); }).detach();
}

}  // namespace wwe2k14::hang_guard
