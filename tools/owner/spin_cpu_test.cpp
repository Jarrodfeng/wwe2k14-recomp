// Lane 306 (the "Owner timing" thread busy spin under vblank_ours): runs the shipped wait code (src/owner_hires_wait.h) and the shipped state machine
// (src/owner_timing_core.h) in a loop shaped like owner_timing.h Loop(), at 60 Hz for 10 s per arm with the flip gate open from each flip to the next vblank (the
// Surface match case), and measures the loop thread CPU (the same CpuMeter the 10 s [owner][timing] line uses) and the vblank dispatch lateness.
//   spin_cpu_test.exe [seconds]   arms: BEFORE (the pre-306 wait rule: gate target now + 250 us with a 1 ms spin) must burn a lot (the defect reproduces);
//                                 AFTER (PlanWait) must stay under 5 % of a core with median / p99 lateness not worse than BEFORE; GATE-CLOSED (no flips) is the baseline.
// Windows only (the spin lives in the Windows HiResWait; elsewhere the thread uses a condition variable). Exit 0 = all checks pass.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../../src/owner_hires_wait.h"
#include "../../src/owner_timing_core.h"

namespace oc = wwe2k14::owner_core;
namespace ow = wwe2k14::owner_wait;
using oc::Count;
using oc::Item;

static int g_fail = 0;
static void Check(bool ok, const char* name, const std::string& detail) {
  std::printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  g_fail += !ok;
}

#ifndef _WIN32
int main() {
  std::puts("spin_cpu_test: Windows only");
  return 0;
}
#else

static constexpr uint32_t kScratch = 0x137FE000;

struct Shared {
  std::mutex mu;
  std::deque<Item> q;
  ow::HiResWait hires;
  std::atomic<bool> stop{false};
};

struct Env {
  Shared& sh;
  uint32_t flip_word = 0, mask = 0;
  oc::VblankClock clock;
  std::vector<uint32_t> late_us;
  explicit Env(Shared& s) : sh(s) {}
  uint32_t Ld32(uint32_t a) const { return a == kScratch + 4 ? flip_word : a == kScratch ? mask : 0; }
  void St32(uint32_t a, uint32_t v) {
    if (a == kScratch + 4) flip_word = v;
    else if (a == kScratch) mask = v;
  }
  void Release() {}
  void FullFence() {}
  void RunGuestInterrupt(uint32_t source, uint32_t cpu, uint32_t) {
    if (source == 0) flip_word = 0;  // the game vblank handler clears the flip word
    else if (source == 1) mask &= ~(1u << cpu);  // the dispatcher clears its cpu bit
  }
  uint32_t Device() { return 1; }
  void Count(oc::Count) {}
  uint32_t VblankOwed() {
    const int64_t now = ow::HiResWait::NowNs(), deadline = clock.NextDeadline();
    const uint32_t n = clock.Due(now);
    if (n && deadline) late_us.push_back(uint32_t(std::max<int64_t>(now - deadline, 0) / 1000));
    return n;
  }
  void WaitBegin(uint32_t, uint32_t) {}
  bool Sleep() {
    sh.hires.Until(ow::HiResWait::NowNs() + 200000, 0, false);
    return !sh.stop.load();
  }
  void OnInterruptDone(const Item&) {}
};

struct Result {
  double cpu_pct = 0, med_ms = 0, p99_ms = 0, max_ms = 0;
  size_t vblanks = 0;
};

// before = the wait rule of owner_timing.h before lane 306 (verbatim): target = min(deadline, now + 250 us) while the gate is open, spin 1 ms for intervals >= 4 ms.
static Result RunArm(bool before, bool flips, double seconds, int64_t interval_ns = 16666667) {
  Shared sh;
  sh.hires.Init();
  Env env(sh);
  env.clock.SetInterval(interval_ns);
  oc::Core<Env> core;
  std::atomic<int64_t> vb_time{0};
  // The game: a flip 3 ms after each vblank (flip word := 1, the gate opens until the next vblank clears the word).
  std::thread producer;
  if (flips) {
    producer = std::thread([&] {
      ow::HiResWait phires;
      phires.Init();
      int64_t seen = 0;
      while (!sh.stop.load()) {
        const int64_t v = vb_time.load();
        if (v != seen) {
          seen = v;
          phires.Until(v + std::min<int64_t>(3000000, interval_ns / 4), 0, false);
          if (sh.stop.load()) break;
          Item it;
          it.flip = true;
          it.e = 3;
          it.d = kScratch;
          {
            std::lock_guard<std::mutex> l(sh.mu);
            sh.q.push_back(it);
          }
          sh.hires.Signal();
        } else {
          phires.Until(ow::HiResWait::NowNs() + 500000, 0, false);
        }
      }
    });
  }
  Result r;
  std::thread loop([&] {
    ow::CpuMeter meter;
    const int64_t t0 = ow::HiResWait::NowNs();
    const int64_t end = t0 + int64_t(seconds * 1e9);
    while (ow::HiResWait::NowNs() < end) {
      Item it;
      bool have = false;
      {
        std::unique_lock<std::mutex> lock(sh.mu);
        if (sh.q.empty()) {
          const int64_t now = ow::HiResWait::NowNs(), deadline = env.clock.NextDeadline();
          int64_t target, spin;
          if (before) {
            target = deadline;
            if (core.gate_open) target = std::min(target, now + 250000);
            spin = env.clock.interval_ns >= 4000000 ? 1000000 : 0;
          } else {
            const ow::WaitPlan p = ow::PlanWait(now, deadline, core.gate_open, env.clock.interval_ns);
            target = p.target_ns;
            spin = p.spin_ns;
          }
          lock.unlock();
          sh.hires.Until(target, spin, true);
          lock.lock();
        }
        if (!sh.q.empty()) {
          it = sh.q.front();
          sh.q.pop_front();
          have = true;
        }
      }
      const size_t before_n = env.late_us.size();
      core.Turn(env);
      if (env.late_us.size() != before_n) vb_time.store(ow::HiResWait::NowNs());
      if (have) core.Process(env, it);
    }
    const double wall_ms = double(ow::HiResWait::NowNs() - t0) / 1e6;
    r.cpu_pct = 100.0 * meter.Take(wall_ms) / wall_ms;
  });
  loop.join();
  sh.stop = true;
  sh.hires.Signal();
  if (producer.joinable()) producer.join();
  std::sort(env.late_us.begin(), env.late_us.end());
  if (!env.late_us.empty()) {
    r.med_ms = env.late_us[env.late_us.size() / 2] / 1000.0;
    r.p99_ms = env.late_us[size_t(double(env.late_us.size()) * 0.99)] / 1000.0;
    r.max_ms = env.late_us.back() / 1000.0;
  }
  r.vblanks = env.late_us.size();
  return r;
}

static std::string Fmt(const Result& r) {
  char b[200];
  std::snprintf(b, sizeof b, "cpu %.1f %% of a core; lateness median %.3f ms, p99 %.3f ms, max %.3f ms (%zu vblanks)", r.cpu_pct, r.med_ms, r.p99_ms, r.max_ms, r.vblanks);
  return b;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const double secs = argc > 1 ? std::atof(argv[1]) : 10.0;
  {  // PlanWait, pure
    const int64_t I = 16666667, now = 1000000000;
    auto a = ow::PlanWait(now, now + 10000000, true, I);
    Check(a.spin_ns == 0 && a.target_ns == now + ow::kGatePollNs, "plan.gate_poll_never_spins", "gate open, deadline 10 ms away: poll at +1 ms, spin 0");
    auto b = ow::PlanWait(now, now + 10000000, false, I);
    Check(b.spin_ns == ow::kVblankSpinNs && b.target_ns == now + 10000000, "plan.deadline_spins_last_half_ms", "gate closed: the deadline, spin 0.5 ms");
    auto c = ow::PlanWait(now, now + 600000, true, I);
    Check(c.target_ns == now + 600000 && c.spin_ns == ow::kVblankSpinNs, "plan.near_deadline_is_deadline", "gate open, deadline 0.6 ms away: the deadline (not a poll past it)");
    auto e = ow::PlanWait(now, now + 1800000, true, I);
    Check(e.target_ns == now + 1800000 && e.spin_ns == ow::kVblankSpinNs, "plan.poll_skipped_near_deadline", "gate open, deadline 1.8 ms away: no poll that could wake into the spin window");
    auto d = ow::PlanWait(now, now + 800000, true, 1000000);
    Check(d.spin_ns == 0, "plan.fast_interval_no_spin", "1 kHz vsync-off: no spin");
    Check(ow::kVblankSpinNs <= 500000 && ow::kGatePollNs > 100000, "plan.constants", "spin <= 0.5 ms; poll above the timer 100 us floor so Until arms the timer");
  }
  const Result closed = RunArm(false, false, secs);
  std::fprintf(stderr, "closed arm done\n");
  const Result bef = RunArm(true, true, secs);
  std::fprintf(stderr, "before arm done\n");
  const Result aft = RunArm(false, true, secs);
  const Result a144 = RunArm(false, true, secs, 6944444), a240 = RunArm(false, true, secs, 4166667);
  std::printf("AFTER 144 Hz         : %s\nAFTER 240 Hz         : %s\n", Fmt(a144).c_str(), Fmt(a240).c_str());
  Check(a144.cpu_pct < 5.0 && a240.cpu_pct < 5.0, "after.fast_rates_under_5_percent", "144 Hz " + std::to_string(a144.cpu_pct) + " %, 240 Hz " + std::to_string(a240.cpu_pct) + " %");
  std::printf("GATE-CLOSED baseline : %s\nBEFORE (pre-306 rule): %s\nAFTER  (PlanWait)    : %s\n", Fmt(closed).c_str(), Fmt(bef).c_str(), Fmt(aft).c_str());
  Check(bef.cpu_pct > 20.0, "before.spin_reproduces", "the old rule burns well over 20 % of a core with the gate open: " + Fmt(bef));
  Check(aft.cpu_pct < 5.0, "after.cpu_under_5_percent", Fmt(aft));
  Check(aft.vblanks >= size_t(secs * 55), "after.vblank_count", std::to_string(aft.vblanks) + " vblanks dispatched");
  Check(aft.med_ms <= bef.med_ms + 0.25, "after.median_lateness_not_worse", "median " + std::to_string(aft.med_ms) + " vs before " + std::to_string(bef.med_ms) + " ms (+0.25 ms allowed)");
  Check(aft.p99_ms <= std::max(bef.p99_ms * 1.5, bef.p99_ms + 0.5), "after.p99_lateness_not_worse", "p99 " + std::to_string(aft.p99_ms) + " vs before " + std::to_string(bef.p99_ms) + " ms");
  std::puts(g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
#endif
