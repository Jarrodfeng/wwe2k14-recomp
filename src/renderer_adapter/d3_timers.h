// S102 K3 rework (step 1, "measure first"): the render thread's own time in the D3 chains' bookkeeping, per chain and phase. There were no D3 timings; ab11's Surface regression (armed-off cost ~13 ms a drawn
// frame) had to be found by arithmetic. No game, SDK or renderer includes. The writers are the render thread (binder.h Begin / End calls, live.cpp's record add + latch takes); the readers are the census line and
// the K3 controller (it reads the total as its armed-off penalty). Cost: two steady_clock reads per scope, a handful of scopes per frame, and only while a D3 chain exists (AnyD3Chain()).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rdr_adapter::d3t {

enum Slot : uint8_t { kArenaBegin, kArenaEnd, kCasterBegin, kCasterEnd, kRingBegin, kRingEnd, kRecords, kLatch, kSubScan, kSubObserve, kSubPublish, kSlots };   // (the Sub slots are INSIDE the Ends: not added to the total)
inline const char* SlotName(int s) {
  static const char* const k[kSlots] = {"arena begin", "arena end", "caster begin", "caster end", "ring begin", "ring end", "records add + evict", "latch takes", "of the Ends: run scan", "of the Ends: observe (learn)", "of the Ends: publish (mark set)"};
  return s >= 0 && s < int(kSlots) ? k[s] : "?";
}
inline std::atomic<uint64_t> g_ns[kSlots];
inline std::atomic<uint64_t> g_calls[kSlots];
inline std::atomic<uint64_t> g_total_ns{0};
inline std::atomic<uint64_t> g_resume_refused{0}, g_resume_unproved_published{0};   // review F1: marks withheld because the object was not re-proved since the chains resumed / marks published although it was not (must be 0)
inline std::atomic<uint64_t> g_marker_off_chain{0};   // review F2: markers the binder met on a frame bound without the chains (must be 0)
inline std::atomic<uint64_t> g_hook_ns{0}, g_hook_samples{0}, g_hook_calls{0};   // review F5: the swap thread's D3 call decisions, 1 call in 16 timed (the census the Surface reads)
inline std::atomic<uint64_t> g_frames_chain{0}, g_frames_off{0};   // handled frames bound with / without the chains (live.cpp)

struct Sub {   // a phase INSIDE an End: counted in its own slot only
  Slot slot;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  explicit Sub(Slot s) : slot(s) {}
  ~Sub() {
    g_ns[slot].fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
    g_calls[slot].fetch_add(1, std::memory_order_relaxed);
  }
  Sub(const Sub&) = delete;
  Sub& operator=(const Sub&) = delete;
};
struct Scope {
  Slot slot;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  explicit Scope(Slot s) : slot(s) {}
  ~Scope() {
    const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    g_ns[slot].fetch_add(ns, std::memory_order_relaxed);
    g_calls[slot].fetch_add(1, std::memory_order_relaxed);
    g_total_ns.fetch_add(ns, std::memory_order_relaxed);
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

// The hook's sampled timer (game side, any thread): 1 call in 16 pays two clock reads; the line scales the samples to all calls.
struct HookSample {
  bool on;
  std::chrono::steady_clock::time_point t0;
  HookSample() : on((g_hook_calls.fetch_add(1, std::memory_order_relaxed) & 15u) == 0) { if (on) t0 = std::chrono::steady_clock::now(); }
  ~HookSample() {
    if (!on) return;
    g_hook_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
    g_hook_samples.fetch_add(1, std::memory_order_relaxed);
  }
  HookSample(const HookSample&) = delete;
  HookSample& operator=(const HookSample&) = delete;
};

// The census line: microseconds a handled frame paid in each slot since the previous line (render thread only), and how many frames ran with / without the chains.
inline std::string Line() {
  static uint64_t last_ns[kSlots] = {}, last_chain = 0, last_off = 0;
  const uint64_t chain = g_frames_chain.load(std::memory_order_relaxed), off = g_frames_off.load(std::memory_order_relaxed);
  const uint64_t frames = (chain - last_chain) + (off - last_off);
  const double f = frames ? double(frames) : 1.0;
  std::string s = "[rdr][d3_time] per handled frame (" + std::to_string(frames) + " frames, " + std::to_string(chain - last_chain) + " with the chains, " + std::to_string(off - last_off) + " without), us:";
  double sum = 0;
  for (int i = 0; i < int(kSlots); ++i) {
    const uint64_t n = g_ns[i].load(std::memory_order_relaxed);
    char b[96];
    std::snprintf(b, sizeof b, " %s %.1f;", SlotName(i), double(n - last_ns[i]) / f / 1e3);
    s += b;
    if (i < int(kSubScan)) sum += double(n - last_ns[i]) / f / 1e3;
    last_ns[i] = n;
  }
  char t[420];
  const uint64_t hs = g_hook_samples.load(std::memory_order_relaxed), hc = g_hook_calls.load(std::memory_order_relaxed);
  std::snprintf(t, sizeof t, " total %.1f us a frame; markers on a chain-off frame %llu (must be 0); marks withheld after a gap %llu, published unproved after a gap %llu (must be 0); swap thread D3 call decisions (entry to after the decision): %llu calls, mean %.2f us (sampled 1 in 16)", sum,
                (unsigned long long)g_marker_off_chain.load(std::memory_order_relaxed), (unsigned long long)g_resume_refused.load(std::memory_order_relaxed),
                (unsigned long long)g_resume_unproved_published.load(std::memory_order_relaxed), (unsigned long long)hc, hs ? double(g_hook_ns.load(std::memory_order_relaxed)) / double(hs) / 1e3 : 0.0);
  s += t;
  last_chain = chain, last_off = off;
  return s;
}

}  // namespace rdr_adapter::d3t
