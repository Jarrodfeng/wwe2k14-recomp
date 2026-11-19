// The rdr_host thread's per-stage CPU census (S103 CPU1; docs\research\s103\RHP.md): how long the record build takes, so the next Surface log shows it with no profiler.
// Header only, no includes of ours. Written by the rdr_host thread (LiveSceneDraw::DrawLive, SceneHost::Handle), read by the same thread at the 600-frame census
// (SceneHost: "[host][cpu] ..."). The counters are relaxed atomics so a tool thread may read them too. The cost is two steady_clock reads per stage.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include "live_screen_rule.h"   // S103 SCX: the live-screen invalidation counters ride the [host][cpu] line

namespace rdr_scene {

struct StageCensus {
  std::atomic<uint64_t> build_ns{0}, record_ns{0}, render_ns{0};   // rf::BuildFrame, FrontEnd::Record, Renderer::RenderFrame (all the calls of a presented frame)
  std::atomic<uint64_t> build_n{0}, record_n{0}, render_n{0};      // calls
  std::atomic<uint64_t> memo_calls{0}, memo_builds{0};              // MemoView calls / the times it had to read the record (ReadArenaRecord)
  std::atomic<uint64_t> lights_hashed{0};                           // times a table's light rows were hashed (once per table, not once per call)
};
inline StageCensus& StageCensusRef() {
  static StageCensus c;
  return c;
}

// One window of the census: the difference between two takes.
struct StageWindow {
  uint64_t build_ns = 0, record_ns = 0, render_ns = 0, build_n = 0, record_n = 0, render_n = 0, memo_calls = 0, memo_builds = 0, lights_hashed = 0;
};
inline StageWindow StageTake() {
  const StageCensus& c = StageCensusRef();
  StageWindow w;
  w.build_ns = c.build_ns.load(std::memory_order_relaxed), w.record_ns = c.record_ns.load(std::memory_order_relaxed), w.render_ns = c.render_ns.load(std::memory_order_relaxed);
  w.build_n = c.build_n.load(std::memory_order_relaxed), w.record_n = c.record_n.load(std::memory_order_relaxed), w.render_n = c.render_n.load(std::memory_order_relaxed);
  w.memo_calls = c.memo_calls.load(std::memory_order_relaxed), w.memo_builds = c.memo_builds.load(std::memory_order_relaxed), w.lights_hashed = c.lights_hashed.load(std::memory_order_relaxed);
  return w;
}
// "[host][cpu] last N presented frames, per presented frame: build X ms record Y ms render+present Z ms (the render / submit block of Handle, tween blend included); memo calls A record reads B light hashes C; yuv avx2|table".
inline std::string StageLine(const StageWindow& now, const StageWindow& before, uint64_t presented, const char* yuv_path = "?") {
  if (!presented) return "[host][cpu] no frame presented in the window";
  const double n = double(presented), ms = 1e-6;
  char b[400];
  std::snprintf(b, sizeof b, "[host][cpu] last %llu presented frames, per presented frame: build %.2f ms record %.2f ms render+present %.2f ms; memo calls %.1f record reads %.2f light hashes %.2f; yuv %s",
                (unsigned long long)presented, double(now.build_ns - before.build_ns) * ms / n, double(now.record_ns - before.record_ns) * ms / n, double(now.render_ns - before.render_ns) * ms / n,
                double(now.memo_calls - before.memo_calls) / n, double(now.memo_builds - before.memo_builds) / n, double(now.lights_hashed - before.lights_hashed) / n, yuv_path);
  return std::string(b) + LiveScreenCensusText();   // S103 SCX: cumulative, only when it happened
}

// Adds the time of its scope to one counter.
class StageTimer {
 public:
  // render-clock: perf - a scope timer added to one duration counter
  StageTimer(std::atomic<uint64_t>& ns, std::atomic<uint64_t>& n) : ns_(ns), n_(n), t0_(std::chrono::steady_clock::now()) {}
  ~StageTimer() {
    ns_.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0_).count()), std::memory_order_relaxed);
    n_.fetch_add(1, std::memory_order_relaxed);
  }
  StageTimer(const StageTimer&) = delete;
  StageTimer& operator=(const StageTimer&) = delete;

 private:
  std::atomic<uint64_t>& ns_;
  std::atomic<uint64_t>& n_;
  std::chrono::steady_clock::time_point t0_;
};

}  // namespace rdr_scene
