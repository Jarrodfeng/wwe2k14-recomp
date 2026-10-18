// wwe2k14 - slow-frame report (Session 8 task 12). Off by default: --wwe2k14_slowframe_ms=0.
//
// --wwe2k14_slowframe_ms=B: every guest frame (VdSwap to VdSwap, as in frames_NNN.csv) that takes at least B ms
// gets a report, rate-limited (at most one per wwe2k14_slowframe_gap_ms, at most wwe2k14_slowframe_max per run).
// For each one this writes, under out/harness/slowframes/NNN/ (NNN = this run's log number; gitignored):
//   events.csv              one line per reported frame (frame, ms, anatomy capture requested or why not)
//   frame_<N>_stacks.txt    the sampling profiler's call stacks of the main thread and the GPU Commands thread
//                           for guest frames N-3..N+1 (stacks_NNN.txt format; frame N's own work is tagged N-1:
//                           a sample's tag is the VdSwap count when it was taken). They come from an in-memory
//                           ring the profiler keeps while this is on (no stacks file). Tags N-3..N-1 are copied
//                           aside at VdSwap N (so a long next frame cannot push them out of the ring); the file is
//                           written at frame N+2 with tags N..N+1 added. A run that ends before frame N+2 (a
//                           slow frame right before a quit: the usual close hard-exits) gets no stacks file.
// and starts a frame-anatomy capture (gpu_anatomy_capture = wwe2k14_slowframe_anatomy frames, sparse GPU marks):
// it starts at the command processor's next frame boundary. The CP runs behind VdSwap, so that can be frame N's own
// CP frame (or even an earlier one) or a later one; the report maps each capture frame to its guest frame and says
// when the capture included frame N (its CP / GPU numbers then include the capture's own overhead). The timeline
// also records every frame's counters (cpf_*: draws, texture loads, barrier batches, uploads, ...) and GPU time
// (gpu_ts), so frame N is covered by those either way.
// The report itself is built afterwards from these files plus the run's log, frames CSV and timeline:
//   py tools/slowframe/slowframe_report.py NNN
//
// Turning it on also sets, each only if left at default: wwe2k14_timeline, vulkan_gpu_timestamps,
// wwe2k14_profile_main, wwe2k14_profile_gpu_commands, gpu_anatomy_gpu_marks + gpu_anatomy_gpu_marks_mode=1.
// Off: no setting is touched, the profiler keeps no ring, and the per-frame cost is one null-pointer test in
// ProfilerOnSwap (sampling_profiler.h).
//
// Debug knob to make slow frames on purpose (to check the report names the cause): wwe2k14_slowframe_force_every=N
// sleeps wwe2k14_slowframe_force_ms on the VdSwap thread (the profiler's "main thread") inside the VdSwap hook every N frames, in
// SlowFrameForcedStall; the report reads such frames as "forced stall" (main thread in SleepEx under OnSwap).

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/perf/timeline.h>

#include "flight_recorder.h"  // FlightDump (--wwe2k14_slowframe_flight_dump, Session 14)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <string>

#include "frame_stats.h"
#include "sampling_profiler.h"

REXCVAR_DEFINE_BOOL(wwe2k14_slowframe_flight_dump, false, "wwe2k14",
                    "Session 14 (S1): with --wwe2k14_flight_recorder=true, also write a flight-recorder dump "
                    "(reason 'requested', every thread's recent kernel events) for each reported slow frame");
REXCVAR_DEFINE_DOUBLE(wwe2k14_slowframe_ms, 0.0, "wwe2k14",
                      "Slow-frame report: any guest frame at least this many ms long gets a stack sample, its "
                      "timeline data and an anatomy capture from the next CP frame in out/harness/slowframes/NNN "
                      "(0 = off; report: tools/slowframe/slowframe_report.py)");
REXCVAR_DEFINE_INT32(wwe2k14_slowframe_max, 20, "wwe2k14", "Slow-frame report: at most this many reports per run");
REXCVAR_DEFINE_INT32(wwe2k14_slowframe_gap_ms, 1000, "wwe2k14",
                     "Slow-frame report: at least this many ms between two reports");
REXCVAR_DEFINE_DOUBLE(wwe2k14_slowframe_ignore_ms, 0.0, "wwe2k14",
                      "Slow-frame report: frames at least this long are not reported (loading screens; 0 = none)");
REXCVAR_DEFINE_INT32(wwe2k14_slowframe_from_frame, 60, "wwe2k14",
                     "Slow-frame report: frames before this guest frame are not reported (boot)");
REXCVAR_DEFINE_INT32(wwe2k14_slowframe_anatomy, 2, "wwe2k14",
                     "Slow-frame report: frames of anatomy capture (sparse GPU marks) from the command processor's "
                     "next frame boundary after a slow frame (0 = none)");
REXCVAR_DEFINE_STRING(wwe2k14_slowframe_dir, "", "wwe2k14",
                      "Slow-frame report output folder (default <exe>/../../harness/slowframes = out/harness/...)");
REXCVAR_DEFINE_INT32(wwe2k14_slowframe_force_every, 0, "wwe2k14",
                     "Debug: sleep wwe2k14_slowframe_force_ms on the main thread every N guest frames (0 = off)");
REXCVAR_DEFINE_INT32(wwe2k14_slowframe_force_ms, 60, "wwe2k14", "Debug: length of a forced stall in ms");

namespace wwe2k14 {

#ifdef _WIN32

// The forced stall. In run 275 the stacks showed SleepEx called straight from
// SlowFrameReporter::OnSwap (probably a tail call); the report classifies either name as "forced stall".
__declspec(noinline) inline void SlowFrameForcedStall(int32_t ms) { Sleep(DWORD(std::max(0, ms))); }

class SlowFrameReporter {
 public:
  void OnPreSetup();
  void OnSwap();

 private:
  struct Pending {
    uint64_t frame;
    std::string path;
    std::string header;
  };

  void Report(uint64_t frame, double ms, double t);
  bool OpenOutput();

  using clock = std::chrono::steady_clock;
  bool on_ = false;
  bool have_last_ = false;
  clock::time_point start_, last_, last_report_;
  bool reported_any_ = false;
  int32_t reports_ = 0;
  uint64_t over_budget_ = 0, rate_limited_ = 0, ignored_ = 0, over_max_ = 0;
  bool max_logged_ = false;
  std::filesystem::path dir_;
  bool output_tried_ = false, output_ok_ = false;
  std::FILE* events_ = nullptr;
  std::deque<Pending> pending_;
};

inline SlowFrameReporter g_slow_frames;

inline void SlowFrameSwapHook() { g_slow_frames.OnSwap(); }

// From Wwe2k14App::OnPreSetup, after the GPU plugin is loaded (its settings exist from then on).
inline void SlowFrameOnPreSetup() { g_slow_frames.OnPreSetup(); }

inline void SlowFrameReporter::OnPreSetup() {
  if (REXCVAR_GET(wwe2k14_slowframe_ms) <= 0.0) return;
  on_ = true;
  // What the report needs, each only if left at default (so any of them can still be turned off).
  for (auto [name, value] : {std::pair{"wwe2k14_timeline", "true"},
                             std::pair{"vulkan_gpu_timestamps", "true"},
                             std::pair{"wwe2k14_profile_main", "true"},
                             std::pair{"wwe2k14_profile_gpu_commands", "true"},
                             std::pair{"gpu_anatomy_gpu_marks", "true"},
                             std::pair{"gpu_anatomy_gpu_marks_mode", "1"}}) {
    if (rex::cvar::GetFlagInfo(name) && rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName(name, value);
  }
  // 16k samples = ~15 s of two threads at the ~540 samples/s each seen with the 1 ms setting (8.6 MB).
  g_sampling_profiler.EnableRing(16384);
  g_profiler_swap_hook = &SlowFrameSwapHook;
  REXLOG_INFO("[wwe2k14][slowframe] on: budget {:.1f} ms, gap {} ms, max {} reports, ignore >= {:.0f} ms, from frame "
              "{}, anatomy {} frames{}",
              REXCVAR_GET(wwe2k14_slowframe_ms), REXCVAR_GET(wwe2k14_slowframe_gap_ms),
              REXCVAR_GET(wwe2k14_slowframe_max), REXCVAR_GET(wwe2k14_slowframe_ignore_ms),
              REXCVAR_GET(wwe2k14_slowframe_from_frame), REXCVAR_GET(wwe2k14_slowframe_anatomy),
              REXCVAR_GET(wwe2k14_slowframe_force_every) > 0
                  ? fmt::format("; FORCED stalls of {} ms every {} frames (debug)",
                                REXCVAR_GET(wwe2k14_slowframe_force_ms), REXCVAR_GET(wwe2k14_slowframe_force_every))
                  : std::string());
}

inline bool SlowFrameReporter::OpenOutput() {
  if (output_tried_) return output_ok_;
  output_tried_ = true;
  const std::string configured = REXCVAR_GET(wwe2k14_slowframe_dir);
  std::filesystem::path base = configured.empty()
                                   ? rex::filesystem::GetExecutableFolder() / ".." / ".." / "harness" / "slowframes"
                                   : std::filesystem::path(configured);
  char run[16];
  std::snprintf(run, sizeof(run), "%03d", CurrentLogNumber());
  dir_ = (base / run).lexically_normal();
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  const std::string events = (dir_ / "events.csv").string();
  events_ = std::fopen(events.c_str(), "w");
  output_ok_ = events_ != nullptr;
  if (!output_ok_) {
    REXLOG_WARN("[wwe2k14][slowframe] could not open {}; no reports", events);
    return false;
  }
  std::fputs("frame,t_seconds,frame_ms,report,stacks_file,anatomy_frames,anatomy_note\n", events_);
  std::fflush(events_);
  REXLOG_INFO("[wwe2k14][slowframe] reports -> {}", dir_.string());
  return true;
}

inline void SlowFrameReporter::OnSwap() {
  if (!on_) return;
  const auto now = clock::now();
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (!have_last_) {
    have_last_ = true;
    start_ = last_ = now;
    return;
  }
  const double ms = std::chrono::duration<double, std::milli>(now - last_).count();
  const double t = std::chrono::duration<double>(now - start_).count();
  last_ = now;

  // Stacks of earlier slow frames, once the frames after them are complete (tags up to N+1 at frame N+2): the
  // snapshot of tags N-3..N-1 taken at the report plus the ring's tags N..N+1.
  while (!pending_.empty() && frame >= pending_.front().frame + 2) {
    Pending& p = pending_.front();
    g_sampling_profiler.RequestRingDump(p.frame, p.frame + 1, std::move(p.path), std::move(p.header));
    pending_.pop_front();
  }

  const int32_t force_every = REXCVAR_GET(wwe2k14_slowframe_force_every);
  if (force_every > 0 && frame % uint64_t(force_every) == 0) SlowFrameForcedStall(REXCVAR_GET(wwe2k14_slowframe_force_ms));

  if (ms < REXCVAR_GET(wwe2k14_slowframe_ms)) return;
  ++over_budget_;
  if (frame < uint64_t(std::max(0, REXCVAR_GET(wwe2k14_slowframe_from_frame)))) return;
  const double ignore_ms = REXCVAR_GET(wwe2k14_slowframe_ignore_ms);
  if (ignore_ms > 0 && ms >= ignore_ms) {
    ++ignored_;
    return;
  }
  if (reports_ >= REXCVAR_GET(wwe2k14_slowframe_max)) {
    ++over_max_;
    if (!max_logged_) {
      max_logged_ = true;
      REXLOG_INFO("[wwe2k14][slowframe] frame {} ({:.1f} ms): {} reports reached, no more this run", frame, ms,
                  reports_);
    }
    return;
  }
  if (reported_any_ &&
      now - last_report_ < std::chrono::milliseconds(std::max(0, REXCVAR_GET(wwe2k14_slowframe_gap_ms)))) {
    ++rate_limited_;
    return;
  }
  Report(frame, ms, t);
  last_report_ = clock::now();  // the gap counts from the end of this report's work
  reported_any_ = true;
}

inline void SlowFrameReporter::Report(uint64_t frame, double ms, double t) {
  if (!OpenOutput()) return;
  ++reports_;
  rex::perf::timeline::Record("slowf", uint32_t(frame), uint32_t(ms * 1000.0), uint32_t(reports_));

  // Anatomy capture from the command processor's next frame boundary (which CP frame that is: see the top).
  int32_t anatomy = REXCVAR_GET(wwe2k14_slowframe_anatomy);
  std::string note;
  if (anatomy <= 0) {
    anatomy = 0;
    note = "off";
  } else if (!rex::cvar::GetFlagInfo("gpu_anatomy_capture")) {
    anatomy = 0;
    note = "SDK has no gpu_anatomy_capture";
  } else if (rex::cvar::GetFlagByName("gpu_anatomy_capture") != "0") {
    anatomy = 0;
    note = "capture already running";
  } else if (!rex::cvar::SetFlagByName("gpu_anatomy_capture", std::to_string(anatomy))) {
    anatomy = 0;
    note = "could not set gpu_anatomy_capture";
  } else {
    note = "requested";
  }

  char name[48];
  std::snprintf(name, sizeof(name), "frame_%06llu_stacks.txt", static_cast<unsigned long long>(frame));
  const bool stacks = g_sampling_profiler.ring_enabled() &&
                      (REXCVAR_GET(wwe2k14_profile_main) || REXCVAR_GET(wwe2k14_profile_gpu_commands));
  if (stacks) {
    // Tags N-3..N-1 are copied aside now (complete: g_guest_frame is already N), so a long next frame cannot push
    // them out of the ring; the file (those plus tags N..N+1) is written at frame N+2 (OnSwap). Writing resolves
    // names, which stops the sampler for a few ms, so it is not done now, in the middle of frame N+1.
    const std::string path = (dir_ / name).string();
    g_sampling_profiler.RequestRingSnapshot(frame >= 3 ? frame - 3 : 0, frame ? frame - 1 : 0, path);
    pending_.push_back({frame, path,
                        fmt::format("# slow frame {} ({:.3f} ms, t={:.3f} s, report {}); its own work is tagged {}\n",
                                    frame, ms, t, reports_, frame ? frame - 1 : 0)});
  }
  if (REXCVAR_GET(wwe2k14_slowframe_flight_dump) && REXCVAR_GET(wwe2k14_flight_recorder)) {
    FlightDump("requested", fmt::format("slow frame {} ({:.1f} ms), report {}", frame, ms, reports_), nullptr, 0);
  }
  std::fprintf(events_, "%llu,%.3f,%.3f,%d,%s,%d,%s\n", static_cast<unsigned long long>(frame), t, ms, reports_,
               stacks ? name : "", anatomy, note.c_str());
  std::fflush(events_);
  REXLOG_INFO("[wwe2k14][slowframe] frame {} took {:.1f} ms (t={:.1f} s): report {}; stacks {}; anatomy {} ({}); "
              "so far {} over budget, {} rate-limited, {} ignored",
              frame, ms, t, reports_, stacks ? name : "off (profiler not sampling)", anatomy, note, over_budget_,
              rate_limited_, ignored_);
}

#else
inline void SlowFrameOnPreSetup() {}
#endif

}  // namespace wwe2k14
