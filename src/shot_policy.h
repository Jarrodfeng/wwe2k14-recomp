// S111 SHOTDIET review (main session decisions N1 / N2, 2026-11-26): when a screenshot is written at all, and how many writes may run at once. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Header-only, std only (the scene host includes it through cut_shot.h, the headless rig too; the unit drives it).
//
// N1, THE PHONE: on Android a screenshot is written only while diagnostics are on (--wwe2k14_diag: diag::On()), as before S111 (the old non-Windows SavePngWic refused without them: normal phone play
// writes no picture files nobody reads). The WebP default changed the FORMAT, not WHEN the phone writes. The PC always writes.
// N2, THE CAP: at most kMaxWrites screenshot writes in flight (a write = SaveScreenshot's capture + encode thread, or one screenshot_on_cut picture's encode thread; a lossless 1440p encode is ~0.5 s,
// ~30 MB held). Two kinds of shot (main session, 2026-11-26: every run is fast-forwarded, a dropped NAMED shot breaks regress / shot_refs / case pictures):
//   * a NAMED shot (`screenshot LABEL`, `shot`, a retry / fail picture, a bookmark) never drops: EnterWait HOLDS the caller (the script step) until a slot frees, at most kNamedWaitMs; past that it
//     writes anyway beyond the cap (Forced, said once per run by input_script.h). The route just takes longer.
//   * a PERIODIC shot (`screenshot_every`) and a `screenshot_on_cut` picture drop at the cap (TryEnter false), counted (input_script.h says the first drop at once and the run's total when the
//     script ends). The scene host does not queue the cut watch's read-back while the gate is full (scene_host.cpp QueueCutShot: such a tick is an unread tick, its cut carried by cut_shot::CutLatch).
#pragma once
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace wwe2k14::shot_policy {

// Test plants (the unit only): 1 = the phone writes always (the rule before this review); 2 = no cap on writes in flight; 3 = a named shot drops at a full gate like a periodic one.
inline int g_plant = 0;

// N1: may this build write a screenshot file now? `phone` = not Windows (the Android build), `diag_on` = diag::On().
inline bool MayWrite(bool phone, bool diag_on) { return !phone || diag_on || g_plant == 1; }

// N2: the writes in flight. TryEnter / EnterWait before a write starts; Leave when it ends (whatever its result).
inline constexpr int kMaxWrites = 2;
inline constexpr int kNamedWaitMs = 15000;
struct WriteGate {
  enum class Entered { Free, Forced, Dropped };
  std::atomic<int> in_flight{0};
  std::atomic<uint64_t> dropped{0}, forced{0};
  std::mutex mu;
  std::condition_variable cv;
  static int Max() { return g_plant == 2 ? INT_MAX : kMaxWrites; }
  bool TakeSlot() {
    int n = in_flight.load(std::memory_order_relaxed);
    while (n < Max())
      if (in_flight.compare_exchange_weak(n, n + 1, std::memory_order_acq_rel)) return true;
    return false;
  }
  // A periodic / cut picture: false = dropped (counted).
  bool TryEnter() {
    if (TakeSlot()) return true;
    dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // A named shot: holds until a slot frees, at most `bound`; then it enters past the cap (Forced, counted). Never Dropped (plant 3 only).
  Entered EnterWait(std::chrono::milliseconds bound) {
    if (TakeSlot()) return Entered::Free;
    if (g_plant == 3) return dropped.fetch_add(1, std::memory_order_relaxed), Entered::Dropped;
    std::unique_lock<std::mutex> lock(mu);
    if (cv.wait_for(lock, bound, [&] { return TakeSlot(); })) return Entered::Free;
    in_flight.fetch_add(1, std::memory_order_acq_rel);
    forced.fetch_add(1, std::memory_order_relaxed);
    return Entered::Forced;
  }
  void Leave() {
    in_flight.fetch_sub(1, std::memory_order_acq_rel);
    { std::lock_guard<std::mutex> lock(mu); }   // (a waiter between its test and its wait sees the slot or is woken)
    cv.notify_all();
  }
  bool Full() const { return in_flight.load(std::memory_order_relaxed) >= Max(); }
};
inline WriteGate g_write_gate;

}  // namespace wwe2k14::shot_policy
