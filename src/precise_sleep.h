// wwe2k14 - precise guest sleeps (Session 8 task B, docs/research/entrance_drops_4k.md). Default OFF.
//
// Why: ReXGlue runs a guest relative KeDelayExecutionThread (non-alertable) as host ::Sleep(whole ms)
// (sdk src/system/xthread.cpp XThread::Delay -> src/core/threading_win.cpp Sleep). With the process timer at 1 ms
// (SDL's timeBeginPeriod) a host Sleep(1) lasts about 1.5-2 ms. The game's update thread (CGameMainThread) runs a
// loop of 14 x Sleep(1) inside one frame in sub_824F6350 (2 + 12 iterations, each `li r3,1; bl 0x82262008`;
// sub_82262008 = sleep(ms, alertable=0) -> sub_82261F40 -> KeDelayExecutionThread with -ms*10000), seen in the
// slow frames of run 327 (Undertaker entrance +18.7 s and +112.6 s): 48-50% of the update thread's samples there.
//
//   --wwe2k14_precise_sleep=true   a non-alertable relative guest delay of at least 1 ms waits on a per-thread
//        high-resolution waitable timer (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION) for exactly the requested time
//        (100 ns units, not truncated to whole ms) instead of host Sleep(ms); requests up to
//        --wwe2k14_precise_sleep_spin_us (default 2000) spin on the host performance counter instead (the timer alone
//        still overshot a 1 ms request to 1.5-2 ms in 29 of 42 cases, runs 333/334). Everything else (alertable delays,
//        zero / sub-ms / absolute / over-60 s intervals, a guest clock scale other than 1.0, non-guest threads, no timer
//        available) goes to ReXGlue's own KeDelayExecutionThread unchanged. Can be flipped mid-run (input-script
//        `cvar wwe2k14_precise_sleep true`): each call reads the setting.
//   --wwe2k14_sleep_stats=true    every ~5 s log "[wwe2k14][sleep]" per path, for relative delays of 1 ms - 60 s:
//          precise       = ours (the setting on and the delay eligible),
//          sdk-eligible  = ReXGlue's path for a delay precise sleep WOULD take (non-alertable, guest thread, clock
//                          scale 1.0): the like-for-like comparison with "precise" (e.g. the other arm of an A/B),
//          sdk-other     = ReXGlue's path for the rest (alertable delays and the other exclusions above);
//        delays measured, mean requested and mean measured ms, and for requests of exactly 1 ms a histogram of the
//        measured time. Measured = QueryPerformanceCounter around the call. (Builds before the fixer pass, s8_tB_2
//        and earlier, logged one "sdk" path that also counted alertable "forever" delays: runs 333/334 have windows
//        with a mean requested of ~1e12 ms. Since s8_tB_3 delays over 60 s are excluded.)
//
// Hook point / coupling: src/flight_recorder.h's REX_HOOK of __imp__KeDelayExecutionThread
// (FlightKeDelayExecutionThread) calls Delay() below in place of ReXGlue's KeDelayExecutionThread_entry, flight
// recorder on or off. That hook is the ONLY way in: if it is removed or stops calling Delay(), these settings do
// nothing. With both settings off Delay() is one call through to ReXGlue after two setting reads.
//
// Scope (reviewer, Session 8): the setting retimes EVERY eligible guest sleep, not only the 14 x Sleep(1) loop:
// run 358's flight rings show ~1,030 each of non-alertable 16 / 10 / 3 ms sleeps on other threads (audio, timer).
// Those take the timer path (> 2 ms), i.e. they end closer to the requested time than host Sleep(ms) does.

#pragma once

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/det.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

#include "frame_stats.h"

REXCVAR_DEFINE_BOOL(wwe2k14_precise_sleep, false, "wwe2k14",
                    "Guest non-alertable relative sleeps of >= 1 ms wait on a high-resolution timer for the exact "
                    "requested time instead of host Sleep(whole ms) (Session 8 task B)");
REXCVAR_DEFINE_INT32(wwe2k14_precise_sleep_spin_us, 2000, "wwe2k14",
                     "With wwe2k14_precise_sleep: requests up to this many microseconds spin on the host counter "
                     "instead of the timer (0 = timer only)");
REXCVAR_DEFINE_BOOL(wwe2k14_sleep_stats, false, "wwe2k14",
                    "Log guest KeDelayExecutionThread requested vs measured time every ~5 s ([wwe2k14][sleep])");
// Session 95 (Wwise and small threads piece): idle polling loops that wake every few ms to find nothing to do.
REXCVAR_DEFINE_STRING(wwe2k14_idle_poll_stretch, "", "wwe2k14",
                      "Session 95: comma-separated ENTRY=MAXMS, ENTRY = hex guest start address of a thread that "
                      "polls with a short non-alertable relative sleep (e.g. 827DFA18=16, the movie thread; a thread START address, as the [idle] log prints it). Once "
                      "the thread has slept wwe2k14_idle_poll_after times in a row with next to no work between "
                      "the sleeps, each sleep lasts MAXMS instead; the first sleep after any real work is the "
                      "game's own again. Off when empty (the default) and in replay (det) mode. Read at the "
                      "first sleep of the run. Log: [wwe2k14][idle]");
REXCVAR_DEFINE_INT32(wwe2k14_idle_poll_after, 100, "wwe2k14",
                     "With wwe2k14_idle_poll_stretch: consecutive near-work-free sleeps before a thread's sleeps "
                     "are stretched (100 = ~0.3 s of a 3 ms poll)");

namespace rex::kernel::xboxkrnl {
// ReXGlue's implementation (also declared in flight_recorder.h; same signature, from xboxkrnl_threading.cpp).
uint32_t KeDelayExecutionThread_entry(uint32_t processor_mode, uint32_t alertable, mapped_u64 interval_ptr);
}  // namespace rex::kernel::xboxkrnl

namespace wwe2k14::sleep {

inline int64_t QpcNow() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

inline int64_t QpcFreq() {
  static const int64_t f = [] {
    LARGE_INTEGER v;
    QueryPerformanceFrequency(&v);
    return int64_t(v.QuadPart);
  }();
  return f;
}

// Histogram edges (ms) for 1-ms requests: [<1.05, <1.25, <1.5, <2, <3, >=3]
constexpr int kBins = 6;
inline const char* kBinNames[kBins] = {"<1.05", "1.05-1.25", "1.25-1.5", "1.5-2", "2-3", ">=3"};

struct PathStats {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> req_us{0};
  std::atomic<uint64_t> act_us{0};
  std::atomic<uint64_t> bins[kBins] = {};
};
constexpr int kPaths = 3;
inline const char* kPathNames[kPaths] = {"sdk-other", "precise", "sdk-eligible"};
inline PathStats g_stats[kPaths];  // 0 = ReXGlue's path (not eligible), 1 = precise, 2 = ReXGlue's path (eligible)
inline std::atomic<int64_t> g_last_log{0};
inline std::atomic<bool> g_timer_warned{false};

inline void Record(int path, int64_t req_100ns, int64_t qpc_ticks) {
  PathStats& s = g_stats[path];
  const uint64_t act_us = uint64_t(qpc_ticks * 1000000 / QpcFreq());
  s.calls.fetch_add(1, std::memory_order_relaxed);
  s.req_us.fetch_add(uint64_t(req_100ns / 10), std::memory_order_relaxed);
  s.act_us.fetch_add(act_us, std::memory_order_relaxed);
  if (req_100ns == 10000) {
    const int b = act_us < 1050 ? 0 : act_us < 1250 ? 1 : act_us < 1500 ? 2 : act_us < 2000 ? 3 : act_us < 3000 ? 4 : 5;
    s.bins[b].fetch_add(1, std::memory_order_relaxed);
  }
  const int64_t now = QpcNow();
  int64_t last = g_last_log.load(std::memory_order_relaxed);
  if (last == 0) {
    g_last_log.compare_exchange_strong(last, now);
    return;
  }
  if (now - last < 5 * QpcFreq() || !g_last_log.compare_exchange_strong(last, now)) return;
  for (int p = 0; p < kPaths; ++p) {
    PathStats& q = g_stats[p];
    const uint64_t c = q.calls.exchange(0, std::memory_order_relaxed);
    const uint64_t rq = q.req_us.exchange(0, std::memory_order_relaxed);
    const uint64_t ac = q.act_us.exchange(0, std::memory_order_relaxed);
    uint64_t b[kBins];
    for (int k = 0; k < kBins; ++k) b[k] = q.bins[k].exchange(0, std::memory_order_relaxed);
    if (!c) continue;
    REXLOG_INFO("[wwe2k14][sleep] frame {} {}: {} delays, mean requested {:.3f} ms, mean measured {:.3f} ms; "
                "1-ms requests (ms): {} {} | {} {} | {} {} | {} {} | {} {} | {} {}",
                wwe2k14::g_guest_frame.load(std::memory_order_relaxed), kPathNames[p], c,
                double(rq) / 1000.0 / double(c), double(ac) / 1000.0 / double(c), kBinNames[0], b[0], kBinNames[1],
                b[1], kBinNames[2], b[2], kBinNames[3], b[3], kBinNames[4], b[4], kBinNames[5], b[5]);
  }
}

// One high-resolution waitable timer per host thread, created on first use and closed when the thread exits
// (thread_local destructor; a thread killed with TerminateThread skips it, as any thread_local would).
struct ThreadTimer {
  HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  ThreadTimer() = default;
  ThreadTimer(const ThreadTimer&) = delete;
  ThreadTimer& operator=(const ThreadTimer&) = delete;
  ~ThreadTimer() {
    if (h) CloseHandle(h);
  }
};

// Exact relative wait: requests up to wwe2k14_precise_sleep_spin_us spin on the host performance counter (run 333/334:
// the high-resolution timer alone still took 1.5-2 ms for 29 of 42 1-ms requests); longer ones wait on this thread's
// high-resolution timer. False if no timer (caller falls back).
inline bool PreciseWait(int64_t hundred_ns) {
  if (hundred_ns <= int64_t(REXCVAR_GET(wwe2k14_precise_sleep_spin_us)) * 10) {
    const int64_t end = QpcNow() + hundred_ns * QpcFreq() / 10000000;
    while (QpcNow() < end) YieldProcessor();
    return true;
  }
  thread_local ThreadTimer tt;
  const HANDLE timer = tt.h;
  if (!timer) {
    if (!g_timer_warned.exchange(true))
      REXLOG_WARN("[wwe2k14][sleep] no high-resolution waitable timer (error {}); precise sleep falls back",
                  GetLastError());
    return false;
  }
  LARGE_INTEGER due;
  due.QuadPart = -hundred_ns;  // negative = relative
  if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) return false;
  WaitForSingleObject(timer, INFINITE);
  return true;
}

// ---- Session 95: idle poll stretch (wwe2k14_idle_poll_stretch) ----
struct IdleRule {
  uint32_t entry;
  int max_ms;
};
inline const std::vector<IdleRule>& IdleRules() {
  static const std::vector<IdleRule> rules = [] {
    std::vector<IdleRule> r;
    const std::string spec = REXCVAR_GET(wwe2k14_idle_poll_stretch);
    size_t pos = 0;
    while (pos < spec.size()) {
      size_t end = spec.find(',', pos);
      if (end == std::string::npos) end = spec.size();
      const std::string item = spec.substr(pos, end - pos);
      pos = end + 1;
      const size_t eq = item.find('=');
      if (eq == std::string::npos) continue;
      const uint32_t entry = uint32_t(std::strtoul(item.substr(0, eq).c_str(), nullptr, 16));
      const int ms = std::atoi(item.c_str() + eq + 1);
      if (entry && ms >= 2 && ms <= 100) r.push_back({entry, ms});
    }
    return r;
  }();
  return rules;
}
inline std::atomic<int64_t> g_idle_stretched{0};  // sleeps that ran stretched, all threads
inline std::atomic<int> g_idle_logs{0};       // first-sleep notices only
inline std::atomic<int> g_idle_rule_logs{0};  // engage / release lines of threads that have a rule
constexpr double kIdleWorkGapUs = 100.0;      // more than this between two sleeps counts as real work

struct IdleThread {
  bool resolved = false;
  int max_ms = 0;        // 0 = this thread has no rule
  uint32_t entry = 0;
  int64_t last_return = 0;  // QPC when the previous sleep of this thread returned
  int idle = 0;             // consecutive sleeps with next to no work in front of them
  bool stretching = false;
  int64_t stretched_run = 0;  // stretched sleeps in the current idle stretch
};

inline IdleThread& IdleTls() {
  thread_local IdleThread t;
  return t;
}
// After the game's own sleep returned: the time the thread's next run of work is measured from.
inline void IdleStamp() {
  IdleThread& th = IdleTls();
  if (th.max_ms) th.last_return = QpcNow();
}

// The sleep for a thread with a rule. Returns true (result in *result) when it did the sleep itself.
inline bool IdleStretch(int64_t t, uint32_t alertable, uint32_t* result) {
  if (alertable || t > -10000 || t < -600000000LL || rex::det::Enabled() || !rex::system::XThread::IsInThread()) return false;
  IdleThread& th = IdleTls();
  if (!th.resolved) {
    th.resolved = true;
    if (auto* xt = rex::system::XThread::GetCurrentThread()) {
      const uint32_t entry = xt->creation_params()->start_address;
      for (const IdleRule& r : IdleRules())
        if (r.entry == entry) {
          th.max_ms = r.max_ms;
          th.entry = entry;
        }
      if (g_idle_logs.fetch_add(1) < 12)
        REXLOG_INFO("[wwe2k14][idle] first sleep of guest thread {:08X} (start sub_{:08X}, {} ms): {}", xt->handle(),
                    entry, (-t) / 10000, th.max_ms ? "has a stretch rule" : "no rule");
    } else if (g_idle_logs.fetch_add(1) < 12) {
      REXLOG_INFO("[wwe2k14][idle] a sleep on a thread with no XThread object");
    }
  }
  if (!th.max_ms) return false;
  const int64_t now = QpcNow();
  const double gap_us = th.last_return ? double(now - th.last_return) * 1e6 / double(QpcFreq()) : 1e9;
  if (gap_us < kIdleWorkGapUs) {
    ++th.idle;
  } else {
    if (th.stretching) {
      if (g_idle_rule_logs.fetch_add(1) < 400)
        REXLOG_INFO("[wwe2k14][idle] sub_{:08X}: work after {} stretched sleeps of {} ms ({} gap us); back to the game's own sleeps",
                    th.entry, th.stretched_run, th.max_ms, int64_t(gap_us));
      th.stretching = false;
      th.stretched_run = 0;
    }
    th.idle = 0;
  }
  const int64_t req_ms = (-t) / 10000;
  if (th.idle < std::max(1, REXCVAR_GET(wwe2k14_idle_poll_after)) || req_ms >= th.max_ms ||
      rex::chrono::Clock::guest_time_scalar() != 1.0) {
    th.last_return = 0;  // let the caller run the game's own sleep, then stamp below
    return false;
  }
  if (!th.stretching) {
    th.stretching = true;
    if (g_idle_rule_logs.fetch_add(1) < 400)
      REXLOG_INFO("[wwe2k14][idle] sub_{:08X}: {} sleeps of {} ms with no work between them; sleeping {} ms from now on until it does work",
                  th.entry, th.idle, req_ms, th.max_ms);
  }
  rex::system::XThread::CheckTitleTermination();
  rex::thread::Sleep(std::chrono::milliseconds(th.max_ms));
  rex::system::XThread::CheckTitleTermination();
  ++th.stretched_run;
  const int64_t n = g_idle_stretched.fetch_add(1) + 1;
  if ((n & 1023) == 0)
    REXLOG_INFO("[wwe2k14][idle] {} stretched sleeps so far (each replaces a wake {} ms sooner)", n, th.max_ms);
  th.last_return = QpcNow();
  *result = 0;
  return true;
}

// Called by FlightKeDelayExecutionThread (flight_recorder.h) in place of ReXGlue's KeDelayExecutionThread_entry.
inline uint32_t Delay(uint32_t processor_mode, uint32_t alertable, mapped_u64 interval_ptr) {
  namespace xk = rex::kernel::xboxkrnl;
  const bool precise = REXCVAR_GET(wwe2k14_precise_sleep);
  const bool stats = REXCVAR_GET(wwe2k14_sleep_stats);
  if (!IdleRules().empty() && (precise || stats)) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true))
      REXLOG_WARN("[wwe2k14][idle] wwe2k14_idle_poll_stretch is OFF while wwe2k14_precise_sleep or wwe2k14_sleep_stats is on");
  }
  if (!precise && !stats) {
    if (!IdleRules().empty() && interval_ptr) {
      uint32_t r = 0;
      if (IdleStretch(int64_t(uint64_t(*interval_ptr)), alertable, &r)) return r;
      const uint32_t res = xk::KeDelayExecutionThread_entry(processor_mode, alertable, interval_ptr);
      IdleStamp();
      return res;
    }
    return xk::KeDelayExecutionThread_entry(processor_mode, alertable, interval_ptr);
  }
  const int64_t t = interval_ptr ? int64_t(uint64_t(*interval_ptr)) : 0;
  // Relative, 1 ms to 60 s: the cases ReXGlue runs as host Sleep(ms). The guest's "forever" (sub_82261F40 with -1
  // builds 0x8000000000000000) and anything else over 60 s stay on ReXGlue's path and out of the stats.
  const bool measured = t <= -10000 && t >= -600000000LL;
  const bool eligible_kind = measured && !alertable && rex::chrono::Clock::guest_time_scalar() == 1.0 &&
                             rex::system::XThread::IsInThread();
  const bool eligible = precise && eligible_kind;
  const int64_t t0 = QpcNow();
  uint32_t result;
  bool used_precise = false;
  if (eligible) {
    // Same safe points as XThread::Delay (exits the thread if the title is terminating; does not return then).
    rex::system::XThread::CheckTitleTermination();
    used_precise = PreciseWait(-t);
    if (used_precise) {
      rex::system::XThread::CheckTitleTermination();
      result = 0;  // X_STATUS_SUCCESS, as XThread::Delay returns for a non-alertable delay
    } else {
      result = xk::KeDelayExecutionThread_entry(processor_mode, alertable, interval_ptr);
    }
  } else {
    result = xk::KeDelayExecutionThread_entry(processor_mode, alertable, interval_ptr);
  }
  if (stats && measured) Record(used_precise ? 1 : eligible_kind ? 2 : 0, -t, QpcNow() - t0);
  return result;
}

}  // namespace wwe2k14::sleep
