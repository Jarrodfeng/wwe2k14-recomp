// S101 lane 270: fine phase clock for the crowd injection (docs\renderer\CROWD_COST.md). Off unless env RDR_CROWD_PHASES=1 (a test / fview measurement; the players never pay more than one
// predictable branch per lap). A lap is "the time since the previous lap goes to this phase" (rdtsc, calibrated against steady_clock over the run). End() closes one injection call into
// per-phase per-call samples (median / p95 in Report()). No game code, no game data.
#pragma once
#if defined(_M_X64) || defined(__x86_64__)
#include <intrin.h>
#define RDR_PHASE_TSC() __rdtsc()
#else
#include <chrono>
#define RDR_PHASE_TSC() uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())   // (arm: the clock itself, ticks = ns)
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace rdr_scene {

class PhaseClock {
 public:
  void Init(const char* const* names, size_t n) {
    names_ = names, n_ = n;
    const char* e = std::getenv("RDR_CROWD_PHASES");
    on_ = e && e[0] == '1';
    if (!on_) return;
    acc_.assign(n, 0), samples_.assign(n, {}), total_.assign(n, 0);
    wall0_ = std::chrono::steady_clock::now(), tsc0_ = RDR_PHASE_TSC();
  }
  bool on() const { return on_; }
  void Begin() {
    if (!on_) return;
    std::fill(acc_.begin(), acc_.end(), 0);
    last_ = RDR_PHASE_TSC();
  }
  void Lap(size_t id) {
    if (!on_) return;
    const uint64_t now = RDR_PHASE_TSC();
    acc_[id] += now - last_;
    last_ = now;
  }
  void End(size_t tail_id) {  // closes the call: the time since the last lap goes to tail_id
    if (!on_) return;
    Lap(tail_id);
    ++calls_;
    for (size_t i = 0; i < n_; ++i) {
      total_[i] += acc_[i];
      samples_[i].push_back(acc_[i]);
    }
  }
  void Count(uint64_t units) { units_ += units; }
  std::string Report(const char* title, const char* unit_name) const {
    if (!on_ || !calls_) return {};
    const double wall_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - wall0_).count();
    const double ns_per_tick = wall_ns / double(RDR_PHASE_TSC() - tsc0_);
    std::string out;
    char line[256];
    uint64_t sum = 0;
    for (size_t i = 0; i < n_; ++i) sum += total_[i];
    std::snprintf(line, sizeof line, "[rdr][crowd][phase] %s: %llu calls, %llu %s, total %.3f ms (%.3f ms per call, %.3f us per %s)", title, static_cast<unsigned long long>(calls_), static_cast<unsigned long long>(units_), unit_name,
                  sum * ns_per_tick / 1e6, sum * ns_per_tick / 1e6 / double(calls_), units_ ? sum * ns_per_tick / 1e3 / double(units_) : 0.0, unit_name);
    out += line;
    for (size_t i = 0; i < n_; ++i) {
      if (!total_[i]) continue;
      std::vector<uint64_t> s = samples_[i];
      std::sort(s.begin(), s.end());
      const double med = s[s.size() / 2] * ns_per_tick / 1e3, p95 = s[std::min(s.size() - 1, s.size() * 95 / 100)] * ns_per_tick / 1e3;
      std::snprintf(line, sizeof line, "\n[rdr][crowd][phase]   %-14s %8.3f ms  %5.1f %%  median %8.1f us  p95 %8.1f us  %6.3f us per %s", names_[i], total_[i] * ns_per_tick / 1e6, 100.0 * double(total_[i]) / double(sum ? sum : 1), med, p95,
                    units_ ? total_[i] * ns_per_tick / 1e3 / double(units_) : 0.0, unit_name);
      out += line;
    }
    std::vector<uint64_t> per_call(calls_, 0);
    for (size_t i = 0; i < n_; ++i)
      for (size_t c = 0; c < calls_; ++c) per_call[c] += samples_[i][c];
    std::sort(per_call.begin(), per_call.end());
    std::snprintf(line, sizeof line, "\n[rdr][crowd][phase]   %-14s median %.1f us  p95 %.1f us per call", "ALL", per_call[per_call.size() / 2] * ns_per_tick / 1e3, per_call[std::min(per_call.size() - 1, per_call.size() * 95 / 100)] * ns_per_tick / 1e3);
    out += line;
    return out;
  }

 private:
  bool on_ = false;
  const char* const* names_ = nullptr;
  size_t n_ = 0;
  std::vector<uint64_t> acc_, total_;
  std::vector<std::vector<uint64_t>> samples_;
  uint64_t last_ = 0, calls_ = 0, units_ = 0, tsc0_ = 0;
  std::chrono::steady_clock::time_point wall0_;
};

}  // namespace rdr_scene
