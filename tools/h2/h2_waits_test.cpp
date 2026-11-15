// S102 lane H2, offline proof of two of its three pieces (0 game runs, 0 Surface runs, no window):
//   piece 1: the 'Owner timing' thread's load-turbo wait (src/owner_hires_wait.h PlanWaitBatched + src/owner_timing_core.h VblankClock::max_catch_up): the SHIPPED plan and clock run
//            in a loop shaped like owner_timing.h Loop() at 16 kHz (62.5 us interval, the load-turbo rate). BEFORE (PlanWait, catch-up 8) must burn a lot (the defect reproduces:
//            a wait under the timer's 100 us floor is a pure spin); AFTER (batched) must burn a fraction of it, deliver every vblank (count within 1 % of the grid, at most 2 grid
//            restarts on a loaded machine) and keep each vblank's lateness inside the tolerance (median <= 0.7 ms, p90 <= 1.5 ms: one 0.5 ms window plus the timer's overshoot).
//   piece 3: the render thread's fence wait (src/renderer/fence_wait.h): a fake fence that signals 1-8 ms after it is armed. SPIN (what a driver that spins inside
//            vkWaitForFences does, modelled as a poll loop with a pause) must burn a core; FRIENDLY (the shipped Wait with the shipped SleepNs) must burn a fraction of it, never
//            return before the signal, see the signal at most one slice late (median <= 0.6 ms, p90 <= 1.5 ms) and give up (false) on a fence that never signals.
//   h2_waits_test.exe [seconds_per_arm]          exit 0 = all checks pass
//   h2_waits_test.exe plant NAME [seconds]       a planted twin that must FAIL the check named for it (exit 1):
//        window_5ms (a checker test: a batched vblank window of 5 ms must trip the lateness check), no_catchup_raise (batched with the clock's catch-up left at 8: count check + restarts),
//        slice_5ms (fence sleep slice 5 ms: lateness check), early_return (the real Wait with cap_ns = 0 gives up before the signal: early check),
//        catchup_always (CatchUpFor returns 256 at every rate: plan.catchup_60hz_unchanged), gate_ignored (batching while a flip is pending: plan.no_batch_while_flip_pending)
// Windows only (the spin lives in the Windows wait; elsewhere the thread uses a condition variable).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../../src/owner_hires_wait.h"
#include "../../src/owner_timing_core.h"
#include "../../src/renderer/fence_wait.h"

namespace ow = wwe2k14::owner_wait;
namespace oc = wwe2k14::owner_core;

static int g_fail = 0;
static std::string g_plant;
static void Check(bool ok, const char* name, const std::string& detail) {
  std::printf("%s  %-36s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  g_fail += !ok;
}

#ifndef _WIN32
int main() {
  std::puts("h2_waits_test: Windows only");
  return 0;
}
#else

static double Pct(std::vector<double>& v, double q) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(double(v.size()) * q))];
}

// ---- piece 1 ----
struct VResult {
  double x = 0;  // the timer-jitter allowance measured just before the run that gave this result
  double cpu_pct = 0, med_ms = 0, p90_ms = 0, max_ms = 0;
  uint64_t vblanks = 0, expected = 0, restarts = 0, wakes = 0;
};

static void Work(int64_t ns) {  // the dispatch of one vblank (the guest interrupt run): a fixed ~2 us of real work
  const int64_t t = ow::HiResWait::NowNs();
  while (ow::HiResWait::NowNs() - t < ns) YieldProcessor();
}

static VResult RunVblank(bool batched, double seconds, int64_t kInterval) {  // 62500 = 16 kHz (the cvar default), 250000 = 4 kHz (s95 lean: every default player config)
  ow::HiResWait hires;
  hires.Init();
  oc::VblankClock clock;
  clock.SetInterval(kInterval);
  const bool plant_nocatch = g_plant == "no_catchup_raise";
  clock.max_catch_up = plant_nocatch ? oc::VblankClock::kMaxCatchUp : ow::CatchUpFor(batched, kInterval);  // what the Loop sets from the switch
  VResult r;
  std::vector<double> late_ms;
  ow::CpuMeter meter;
  const int64_t t0 = ow::HiResWait::NowNs(), end = t0 + int64_t(seconds * 1e9);
  clock.Due(t0);  // starts the grid
  const int64_t first_deadline = clock.NextDeadline();
  int64_t last_wake = 0;
  while (ow::HiResWait::NowNs() < end) {
    const int64_t now = ow::HiResWait::NowNs();
    ow::WaitPlan plan = ow::PlanWaitBatched(now, clock.NextDeadline(), false, kInterval, last_wake, batched);
    if (batched && g_plant == "window_5ms")  // (a checker test: a window far over the tolerance must trip the lateness check)
      plan.target_ns = std::max(plan.target_ns, last_wake + 5000000);  // the planted drift: a 5 ms window
    hires.Until(plan.target_ns, plan.spin_ns, true);
    last_wake = ow::HiResWait::NowNs();
    ++r.wakes;
    const int64_t at = last_wake;
    const int64_t deadline = clock.NextDeadline();
    const uint32_t n = clock.Due(at);
    for (uint32_t k = 0; k < n; ++k) {
      late_ms.push_back(double(std::max<int64_t>(at - (deadline + int64_t(k) * kInterval), 0)) / 1e6);
      Work(2000);
    }
    r.vblanks += n;
  }
  const double wall_ms = double(ow::HiResWait::NowNs() - t0) / 1e6;
  r.cpu_pct = 100.0 * meter.Take(wall_ms) / wall_ms;
  r.med_ms = Pct(late_ms, 0.5);
  r.p90_ms = Pct(late_ms, 0.9);
  r.max_ms = late_ms.empty() ? 0.0 : *std::max_element(late_ms.begin(), late_ms.end());
  r.expected = uint64_t(double(clock.NextDeadline() - first_deadline) / double(kInterval));  // the grid's own count to the last deadline passed
  r.restarts = clock.dropped;
  return r;
}

static std::string FmtV(const VResult& r) {
  char b[260];
  std::snprintf(b, sizeof b, "cpu %.1f %% of a core; %llu vblanks (%llu expected, %llu grid restarts, %llu wakes); lateness median %.3f ms, p90 %.3f ms, max %.3f ms", r.cpu_pct,
                (unsigned long long)r.vblanks, (unsigned long long)r.expected, (unsigned long long)r.restarts, (unsigned long long)r.wakes, r.med_ms, r.p90_ms, r.max_ms);
  return b;
}

// ---- piece 3 ----
struct FResult {
  double x = 0;
  double cpu_pct = 0, med_ms = 0, p90_ms = 0;
  int early = 0, n = 0;
};

static FResult RunFence(bool friendly, double seconds) {
  FResult r;
  std::vector<double> late_ms;
  uint32_t lcg = 12345;
  rdr::fence_wait::Params params;
  if (g_plant == "slice_5ms") params.slice_min_ns = params.slice_max_ns = 5000000;
  ow::CpuMeter meter;
  const int64_t t0 = ow::HiResWait::NowNs(), end = t0 + int64_t(seconds * 1e9);
  while (ow::HiResWait::NowNs() < end) {
    lcg = lcg * 1664525u + 1013904223u;
    const int64_t dur = 1000000 + int64_t((lcg >> 8) % 7000) * 1000;  // 1..8 ms
    const int64_t due = ow::HiResWait::NowNs() + dur;
    auto poll = [&] { return ow::HiResWait::NowNs() >= due; };  // the fence is signalled once its time has passed (the GPU is the clock)
    bool ok;
    if (!friendly) {
      while (!poll()) YieldProcessor();  // the model of a driver spin
      ok = true;
    } else if (g_plant == "early_return") {
      rdr::fence_wait::Params none;
      none.cap_ns = 0;  // planted: the real Wait gives up at once (cap 0) while the fence is unsignalled; the caller's early count must see it
      ok = rdr::fence_wait::Wait(poll, rdr::fence_wait::NowNs, rdr::fence_wait::SleepNs, none);
    } else {
      ok = rdr::fence_wait::Wait(poll, rdr::fence_wait::NowNs, rdr::fence_wait::SleepNs, params);
    }
    const int64_t got = ow::HiResWait::NowNs();
    if (!ok || got < due) ++r.early;
    late_ms.push_back(double(std::max<int64_t>(got - due, 0)) / 1e6);
    ++r.n;
  }
  const double wall_ms = double(ow::HiResWait::NowNs() - t0) / 1e6;
  r.cpu_pct = 100.0 * meter.Take(wall_ms) / wall_ms;
  r.med_ms = Pct(late_ms, 0.5);
  r.p90_ms = Pct(late_ms, 0.9);
  return r;
}

static std::string FmtF(const FResult& r) {
  char b[200];
  std::snprintf(b, sizeof b, "cpu %.1f %% of a core; %d waits, %d early; seen late: median %.3f ms, p90 %.3f ms", r.cpu_pct, r.n, r.early, r.med_ms, r.p90_ms);
  return b;
}

// The machine's own timer jitter: how late a bare 0.5 ms high-resolution wait wakes (p90). The lateness bounds below are for a quiet machine; on a loaded one they widen by the
// excess over 0.6 ms, capped at 1 ms: noisier than that is a loud failure ("machine too loaded"), never a quiet pass.
static double g_excess_ms = 0;
static bool g_print_calib = true;
static void Calibrate() {
  ow::HiResWait hw;
  hw.Init();
  std::vector<double> over;
  for (int i = 0; i < 400; ++i) {
    const int64_t t = ow::HiResWait::NowNs() + 500000;
    hw.Until(t, 0, false);
    over.push_back(double(ow::HiResWait::NowNs() - t) / 1e6);
  }
  const double p90 = Pct(over, 0.9);
  g_excess_ms = std::max(0.0, p90 - 0.6);
  if (g_print_calib) std::printf("timer jitter calibration   : bare 0.5 ms wait wakes late: median %.3f ms, p90 %.3f ms -> bounds widened by %.3f ms%s\n", Pct(over, 0.5), p90, std::min(g_excess_ms, 1.0),
              g_excess_ms > 1.0 ? " (MACHINE TOO LOADED: widening capped at 1 ms)" : "");
  g_excess_ms = std::min(g_excess_ms, 1.0);
}

// A loaded PC (other lanes compile and run) makes any one timing window noisy: up to 5 attempts, each with its own jitter calibration; the first inside its bounds is the result, else the
// attempt with the smallest p90 (and the check then fails).
template <class R, class F, class Ok>
static R Best(F run, Ok ok) {
  R best{};
  bool have = false;
  for (int i = 0; i < 5; ++i) {
    Calibrate();
    R r = run();
    r.x = g_excess_ms;
    if (ok(r)) return r;
    if (!have || r.p90_ms < best.p90_ms) best = r, have = true;
  }
  return best;
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  double secs = 3.0;
  int ai = 1;
  if (argc > 2 && std::strcmp(argv[1], "plant") == 0) {
    g_plant = argv[2];
    ai = 3;
  }
  if (argc > ai) secs = std::atof(argv[ai]);
  const bool plant = !g_plant.empty();
  Calibrate();
  g_print_calib = false;

  {  // the plan, pure
    const int64_t now = 1000000000;
    auto off = ow::PlanWaitBatched(now, now + 62500, false, 62500, now, false);
    auto base = ow::PlanWait(now, now + 62500, false, 62500);
    Check(off.target_ns == base.target_ns && off.spin_ns == base.spin_ns, "plan.switch_off_is_unchanged", "enabled=false returns PlanWait's plan");
    auto fast = ow::PlanWaitBatched(now, now + 62500, false, 62500, now, true);
    Check(fast.target_ns == now + ow::kBatchWakeNs && fast.spin_ns == 0, "plan.fast_interval_waits_a_window", "62.5 us interval: next wake one window after the last, spin 0");
    auto slow = ow::PlanWaitBatched(now, now + 16666667, false, 16666667, now, true);
    auto slow0 = ow::PlanWait(now, now + 16666667, false, 16666667);
    Check(slow.target_ns == slow0.target_ns && slow.spin_ns == slow0.spin_ns, "plan.60hz_unchanged", "a 16.7 ms interval is never batched (match / menu timing as before)");
    Check(ow::kBatchWakeNs / 62500 * 4 <= int64_t(ow::kBatchCatchUp), "plan.catchup_covers_window", "catch-up limit >= 4x a window's vblanks (the late-timer margin)");
    auto catchup = [&](bool b, int64_t i) { return g_plant == "catchup_always" ? ow::kBatchCatchUp : ow::CatchUpFor(b, i); };
    Check(catchup(true, 16666667) == 8 && catchup(true, 1000000) == 8 && catchup(false, 62500) == 8 && catchup(false, 250000) == 8 && catchup(true, 250000) == 256 && catchup(true, 62500) == 256,
          "plan.catchup_60hz_unchanged", "catch-up is 8 at 16.7 ms and 1 ms with the switch on, 8 with it off; 256 only batching at 250 us / 62.5 us");
    auto enabled = [&](bool sw, bool win, bool gate) { return g_plant == "gate_ignored" ? (sw && win) : ow::BatchEnabled(sw, win, gate); };
    Check(!enabled(true, true, true) && !enabled(true, false, false) && !enabled(false, true, false) && enabled(true, true, false), "plan.no_batch_while_flip_pending",
          "batching needs the switch, an open in-game window and no pending flip (a pending flip waits a wake per load frame otherwise)");
    {
      const int64_t t = 1000000000;
      const auto open = ow::PlanWaitBatched(t, t + 250000, true, 250000, t, enabled(true, true, true));
      const auto ref = ow::PlanWait(t, t + 250000, true, 250000);
      Check(open.target_ns == ref.target_ns && open.spin_ns == ref.spin_ns, "plan.gate_open_unbatched", "gate open at 250 us: the plan is PlanWait's");
    }
    Check(oc::VblankClock{}.max_catch_up == oc::VblankClock::kMaxCatchUp, "clock.default_catchup_unchanged", "VblankClock default catch-up is still 8");
  }

  const VResult vb = RunVblank(false, secs, 62500);
  std::printf("VBLANK BEFORE (PlanWait)   : %s\n", FmtV(vb).c_str());
  const VResult va = Best<VResult>([&] { return RunVblank(true, secs, 62500); }, [](const VResult& r) { return double(r.vblanks) >= 0.99 * double(r.expected) && r.restarts <= 2 && r.med_ms <= 0.7 + r.x && r.p90_ms <= 1.5 + r.x; });
  std::printf("VBLANK AFTER  (batched)    : %s\n", FmtV(va).c_str());
  if (!plant) Check(vb.cpu_pct > 40.0, "vblank.before_spin_reproduces", "the unbatched 16 kHz wait burns over 40 % of a core: " + FmtV(vb));
  Check(va.cpu_pct < 0.5 * vb.cpu_pct && va.cpu_pct < 25.0, "vblank.after_cpu_under_half", "after " + std::to_string(va.cpu_pct) + " % vs before " + std::to_string(vb.cpu_pct) + " % (and under 25 %)");
  Check(double(va.vblanks) >= 0.99 * double(va.expected) && va.restarts <= 2, "vblank.count_kept", std::to_string(va.vblanks) + " of " + std::to_string(va.expected) + ", " + std::to_string(va.restarts) + " grid restarts");
  Check(va.med_ms <= 0.7 + va.x && va.p90_ms <= 1.5 + va.x, "vblank.lateness_in_tolerance", "median " + std::to_string(va.med_ms) + " ms (<= 0.7 + jitter), p90 " + std::to_string(va.p90_ms) + " ms (<= 1.5)");
  {  // 250 us = 4 kHz: what every default player config runs (s95 lean), the Surface included
    const VResult vb4 = RunVblank(false, secs, 250000);
    const VResult va4 = Best<VResult>([&] { return RunVblank(true, secs, 250000); }, [](const VResult& r) { return double(r.vblanks) >= 0.99 * double(r.expected) && r.restarts <= 2 && r.med_ms <= 0.9 + r.x && r.p90_ms <= 2.0 + r.x; });
    std::printf("VBLANK 4 kHz BEFORE        : %s\nVBLANK 4 kHz AFTER         : %s\n", FmtV(vb4).c_str(), FmtV(va4).c_str());
    Check(va4.cpu_pct < 15.0, "vblank4k.cpu_low", "after " + std::to_string(va4.cpu_pct) + " % of a core (before, for the record: " + std::to_string(vb4.cpu_pct) + " %: at 4 kHz the unbatched wait already uses the timer)");
    Check(double(va4.vblanks) >= 0.99 * double(va4.expected) && va4.restarts <= 2, "vblank4k.count_kept", std::to_string(va4.vblanks) + " of " + std::to_string(va4.expected) + ", " + std::to_string(va4.restarts) + " grid restarts");
    Check(va4.med_ms <= 0.9 + va4.x && va4.p90_ms <= 2.0 + va4.x, "vblank4k.lateness_in_tolerance", "median " + std::to_string(va4.med_ms) + " ms (<= 0.9), p90 " + std::to_string(va4.p90_ms) + " ms (<= 2.0)");
  }

  const FResult fs = RunFence(false, secs);
  std::printf("FENCE SPIN (driver model)  : %s\n", FmtF(fs).c_str());
  const FResult ff = Best<FResult>([&] { return RunFence(true, secs); }, [](const FResult& r) { return r.early == 0 && r.med_ms <= 0.6 + r.x && r.p90_ms <= 1.5 + r.x; });
  std::printf("FENCE FRIENDLY             : %s\n", FmtF(ff).c_str());
  if (!plant) Check(fs.cpu_pct > 60.0, "fence.spin_reproduces", "the spin model burns over 60 % of a core: " + FmtF(fs));
  Check(ff.cpu_pct < 0.5 * fs.cpu_pct && ff.cpu_pct < 30.0, "fence.friendly_cpu_under_half", "friendly " + std::to_string(ff.cpu_pct) + " % vs spin " + std::to_string(fs.cpu_pct) + " %");
  Check(ff.early == 0, "fence.never_early", std::to_string(ff.early) + " of " + std::to_string(ff.n) + " waits returned before the signal");
  Check(ff.med_ms <= 0.6 + ff.x && ff.p90_ms <= 1.5 + ff.x, "fence.lateness_in_tolerance", "median " + std::to_string(ff.med_ms) + " ms (<= 0.6), p90 " + std::to_string(ff.p90_ms) + " ms (<= 1.5)");
  {  // a fence that never signals: the cap
    rdr::fence_wait::Params p;
    p.cap_ns = 20000000;
    const int64_t a = ow::HiResWait::NowNs();
    const bool got = rdr::fence_wait::Wait([] { return false; }, rdr::fence_wait::NowNs, rdr::fence_wait::SleepNs, p);
    const double ms = double(ow::HiResWait::NowNs() - a) / 1e6;
    Check(!got && ms >= 20.0 && ms < 40.0, "fence.cap_gives_up", "never-signalled fence: false after " + std::to_string(ms) + " ms (cap 20)");
  }
  std::puts(g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
#endif
