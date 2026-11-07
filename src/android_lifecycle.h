#pragma once
// wwe2k14 - Session 95, piece LIFE (out\s95\android_wide.md section 7): the game's side of what Android does to the app.
//
// In front: GameActivity.onPause / onResume call nativeAppInFront. Resume marks the next frame's length as not a hitch
// (g_frame_time_ignore_next, frame_stats.h), so a 26 s gap reaches neither the dynamic resolution nor the perf hint; frames that
// end while out of front are marked the same way. SDL's own pause comes later (onStop), so this flag turns off slightly early and
// on slightly late. HANG's guard reads InFront().
//
// Background count: every return logs "[life] background: N ms, N guest frames ran", which answers whether the guest stops by
// itself. Phone run r021 shows that only for the FIFO path; mailbox phones are unconfirmed.
//
// --wwe2k14_android_park (default off): the guest waits at the frame boundary while out of front. Skipped for precompile and
// diagnostic runs: both must keep running with the screen off.
//
// Exits: WriteExitRequest writes logs/exit_request.txt, GameActivity polls for it every 400 ms and brings the launcher back.
//
// Test switches, for the PC too: --wwe2k14_test_background_at=<guest frame> --wwe2k14_test_background_ms=<ms> send SDL's real
// DID_ENTER_BACKGROUND and DID_ENTER_FOREGROUND through the SDK's event watcher with the UI thread blocked between them, in the
// order a phone does it: SDL's Android pump blocks as soon as it has handled the pause, so the surface events reach the UI loop
// together, after the return. The guest and the paint path keep the old surface in that time, which on the PC is still alive.
#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <thread>

#include "blackbox.h"

#if defined(__ANDROID__)
#include <jni.h>
#endif

REXCVAR_DEFINE_BOOL(wwe2k14_android_park, false, "wwe2k14",
                    "Session 95: the guest waits at the frame boundary while the app is out of front (Android). Off until the "
                    "phone list passes; skipped for precompile and diagnostic runs");
REXCVAR_DEFINE_INT32(wwe2k14_test_background_at, 0, "wwe2k14",
                     "Session 95 (test only): at this guest frame send SDL's real background event through the event watcher, "
                     "block the UI thread for --wwe2k14_test_background_ms, then send the foreground event. 0 = off");
REXCVAR_DEFINE_INT32(wwe2k14_test_background_ms, 90000, "wwe2k14",
                     "Session 95 (test only): how long --wwe2k14_test_background_at keeps the UI thread blocked");

namespace wwe2k14::android_lifecycle {

using Clock = std::chrono::steady_clock;

inline std::atomic<bool> g_in_front{true};
inline std::atomic<bool> g_ignore_next{false};
inline std::atomic<uint64_t> g_frames_seen{0};
inline std::mutex g_mutex;  // the two fields below, and the park wait
inline std::condition_variable g_front_changed;
inline Clock::time_point g_left_at;
inline uint64_t g_left_frames = 0;
inline bool g_park_allowed = false;  // Install: not a precompile or diagnostic run
inline std::atomic<bool> g_test_sent{false};
inline std::function<void(std::function<void()>)> g_post_to_ui;

inline bool InFront() { return g_in_front.load(std::memory_order_acquire); }

// Any thread (Android's main thread through JNI, the UI thread in a test).
inline void SetInFront(bool front) {
  const auto now = Clock::now();
  const uint64_t frames = g_frames_seen.load(std::memory_order_relaxed);
  int64_t ms = 0;
  uint64_t ran = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (InFront() == front) return;  // the first onResume: already in front
    if (!front) {
      g_left_at = now;
      g_left_frames = frames;
    } else {
      ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_left_at).count();
      ran = frames - g_left_frames;
      g_ignore_next.store(true, std::memory_order_relaxed);
    }
    g_in_front.store(front, std::memory_order_release);
  }
  g_front_changed.notify_all();
  if (!front) {
    REXLOG_INFO("[life] out of front at guest frame {}", frames);
    return;
  }
  REXLOG_INFO("[life] background: {} ms, {} guest frames ran", ms, ran);
  blackbox::Note("back in front: %lld ms in the background, %llu guest frames ran", static_cast<long long>(ms),
                 static_cast<unsigned long long>(ran));
}

// UI thread. See the file comment for the order.
inline void TestBackground() {
  const int ms = REXCVAR_GET(wwe2k14_test_background_ms);
  REXLOG_INFO("[life] test: background for {} ms, UI thread blocked", ms);
  SetInFront(false);
  const bool sent = rex::cvar::SetFlagByName("wwe2k14_test_app_event", "background");
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  rex::cvar::SetFlagByName("wwe2k14_test_app_event", "foreground");
  SetInFront(true);
  REXLOG_INFO("[life] test: UI thread free again{}", sent ? "" : " (no wwe2k14_test_app_event: this SDK sent no events)");
}

inline void Park(uint64_t frame) {
  const auto t0 = Clock::now();
  REXLOG_INFO("[life] park: the guest waits at frame {}", frame);
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_front_changed.wait(lock, [] { return InFront(); });
  }
  REXLOG_INFO("[life] park: the guest goes on after {} ms",
              std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
}

// The guest's swap, before FrameStats takes its lock (a parked guest holds nothing). True: this frame's length says nothing about
// the game (frame_stats.h sets g_frame_time_ignore_next).
inline bool AtFrameBoundary(uint64_t frame) {
  g_frames_seen.store(frame, std::memory_order_relaxed);
  const int test_at = REXCVAR_GET(wwe2k14_test_background_at);
  if (test_at > 0 && frame >= uint64_t(test_at) && g_post_to_ui && !g_test_sent.exchange(true)) g_post_to_ui(TestBackground);
  if (InFront()) return g_ignore_next.exchange(false, std::memory_order_relaxed);
  if (g_park_allowed && REXCVAR_GET(wwe2k14_android_park)) {
    Park(frame);
    g_ignore_next.store(false, std::memory_order_relaxed);  // this frame carries the gap
  }
  return true;
}

// Once, before the first guest frame. `post_to_ui` runs a function on the UI thread.
inline void Install(std::function<void(std::function<void()>)> post_to_ui) {
  g_post_to_ui = std::move(post_to_ui);
  g_park_allowed = rex::cvar::GetFlagByName("wwe2k14_precompile").empty() && rex::cvar::GetFlagByName("wwe2k14_diag").empty();
  if (REXCVAR_GET(wwe2k14_android_park)) {
    REXLOG_INFO("[life] park {}", g_park_allowed ? "on" : "skipped: a precompile or diagnostic run");
  }
}

// Asks GameActivity to bring the launcher back and end this process. Renamed into place, so the poll never sees half a file. The
// working folder is the app's files folder, as for logs/startup.txt.
inline void WriteExitRequest(const char* why) {
  if (FILE* f = std::fopen("logs/exit_request.tmp", "wb")) {
    std::fprintf(f, "%s\n", why);
    std::fclose(f);
    std::rename("logs/exit_request.tmp", "logs/exit_request.txt");
  }
}

}  // namespace wwe2k14::android_lifecycle

#if defined(__ANDROID__)
extern "C" JNIEXPORT void JNICALL Java_io_github_jarrodfeng_wwe2k14_GameActivity_nativeAppInFront(JNIEnv*, jclass,
                                                                                                 jboolean front) {
  wwe2k14::android_lifecycle::SetInFront(front != JNI_FALSE);
}
#endif
