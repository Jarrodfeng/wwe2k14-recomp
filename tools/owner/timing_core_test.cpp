// Lane 260 (LEAVE_THE_COMMAND_STREAM 3e, section 10): the "Owner timing" thread's state machine (src/owner_timing_core.h, the code owner_timing.h runs) against a model of
// the guest on a virtual clock.
//   timing_core_test.exe               honest run: every check must PASS (exit 0)
//   timing_core_test.exe plant NAME    one planted fault, which must make a named check FAIL (exit 1)
// Plants: release_early (a fence written while the flip is pending), flip_never_cleared (the guest's vblank handler leaves the flip word), missed_vblank (the vblank
// source drops one owed vblank each turn), wait_no_tick (a flip waiting for the previous flip's word does not tick the vblank), fixed_ignored (the scene-timing value left as
// measured), regrid_every_vblank (the clock re-grids from now at each vblank: the grid drifts and loses vblanks).
// The guest model is written from the listings (docs/renderer/ADAPTER_CALLS.md section 5, LEAVE_THE_COMMAND_STREAM.md 10.1): the flip scheduler 0x82CB2A28 leaves the flip word
// set until the next vblank; the vblank handler 0x82CB28F8 clears it; 0x82C16978 stores the timebase at *arg, 0x82C16980 stores now - start at arg + 8; 0x824D7988 returns 1 when
// the stored time, scaled by the float at 0x8392E974, is at least one budget.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/owner_timing_core.h"

namespace oc = wwe2k14::owner_core;
using oc::Count;
using oc::Item;

static int g_fail = 0;
static std::string g_plant;
static bool Plant(const char* n) { return g_plant == n; }
static void Check(bool ok, const char* name, const std::string& detail) {
  std::printf("%s  %-36s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
  g_fail += !ok;
}

static constexpr uint32_t kScratch = 0x137FE000, kBlock = 0x137FF000, kDev = 0x40000000, kFlipSched = 0x82CB2A28, kBadF00d = 0x0BADF00D;
static constexpr int64_t kStep = 200000;  // 200 us, the real Sleep()
static constexpr float kScale = 1.0f / 49875000.0f;  // seconds per timebase tick (the 360's 49.875 MHz); the real value is read from the guest
static constexpr double kBudgetMs = 16.667;

struct Event {
  int64_t t;
  Item it;
};

struct Env {
  std::map<uint32_t, uint32_t> mem;
  std::vector<std::string> trace;  // ordered: "W addr val" (selected addresses), "I source cpu dev"
  int64_t now = 0;
  oc::VblankClock clock;
  bool ours = true;
  bool in_wait = false;
  long sleeps = 0;
  bool flip_pending = false;
  int vblanks = 0, dispatched_vblank = 0, scene_ends = 0;
  uint32_t last_vblank_dev = 0, last_vblank_cpu = 0, last_vblank_source = 9;
  double fixed_ms = 0.0;
  bool gave_up = false;
  std::map<Count, long> counts;

  uint32_t Ld32(uint32_t a) const {
    auto it = mem.find(a);
    return it == mem.end() ? 0 : it->second;
  }
  void St32(uint32_t a, uint32_t v) {
    mem[a] = v;
    if (a >= kBlock && a < kBlock + 8) trace.push_back("W " + std::to_string(a - kBlock) + " " + std::to_string(v));
  }
  void Release() {}
  void FullFence() {}
  uint64_t Tb() const { return uint64_t(double(now) * 1e-9 / double(kScale)); }
  void RunGuestInterrupt(uint32_t source, uint32_t cpu, uint32_t device) {
    trace.push_back("I " + std::to_string(source) + " " + std::to_string(cpu) + " " + std::to_string(device));
    if (source == 0) {  // the vblank handler 0x82CB28F8
      ++dispatched_vblank;
      last_vblank_source = source, last_vblank_cpu = cpu, last_vblank_dev = device;
      if (flip_pending && !Plant("flip_never_cleared")) {
        mem[kScratch + 4] = 0;
        flip_pending = false;
      }
      return;
    }
    // the dispatcher 0x82CB5758, source 1: clear this cpu's bit of the mask, run the callback
    mem[kScratch + 0] &= ~(1u << cpu);
    const uint32_t cb = mem[kScratch + 16], arg = mem[kScratch + 20];
    if (cb == kFlipSched) {
      flip_pending = true;  // (the word stays set until a vblank)
    } else if (cb == oc::kSceneTimingBegin) {
      const uint64_t tb = Tb();
      mem[arg] = uint32_t(tb >> 32), mem[arg + 4] = uint32_t(tb);
    } else if (cb == oc::kSceneTimingEnd) {
      ++scene_ends;
      const uint64_t start = (uint64_t(mem[arg]) << 32) | mem[arg + 4], el = Tb() - start;
      mem[arg + 8] = uint32_t(el >> 32), mem[arg + 12] = uint32_t(el);
    }
  }
  uint32_t Device() { return kDev; }
  void Count(oc::Count k) { ++counts[k]; if (k == Count::Vblank) ++vblanks; }
  uint32_t VblankOwed() {
    if (!ours) return 0;
    if (Plant("wait_no_tick") && in_wait) return 0;
    uint32_t n = clock.Due(now);
    if (Plant("regrid_every_vblank") && n) clock.next_ns = now + clock.interval_ns;
    if (Plant("missed_vblank") && n) --n;
    return n;
  }
  void WaitBegin(uint32_t, uint32_t) { in_wait = true; sleeps = 0; }
  bool Sleep() {
    now += kStep;
    if (++sleeps > 4000) {  // 0.8 s of virtual time without the word changing: a hang
      gave_up = true;
      in_wait = false;
      return false;
    }
    return true;
  }
  void OnInterruptDone(const Item& it) {
    if (it.b != oc::kSceneTimingEnd || Plant("fixed_ignored") || fixed_ms <= 0.0) return;
    const uint64_t ticks = oc::SceneTicksForMs(fixed_ms, kScale);
    mem[it.c + 8] = uint32_t(ticks >> 32), mem[it.c + 12] = uint32_t(ticks);
  }
};

static Item Fence(uint32_t n) {
  Item it;
  it.fence = true;
  it.a = kBlock + 4, it.b = 0x1000 + n, it.c = kBlock, it.d = n;
  return it;
}
static Item Flip(bool waits = true) {
  Item it;
  it.flip = true, it.b = kFlipSched, it.c = 0x100, it.d = kScratch, it.e = waits ? 3u : 0u, it.dev = kDev;
  return it;
}
static Item Scene(uint32_t cb, uint32_t arg) {
  Item it;
  it.a = 0, it.b = cb, it.c = arg, it.d = kScratch, it.dev = kDev;
  return it;
}

// The loop of owner_timing.h on a virtual clock: each 200 us step, the due items are processed in order after core.Turn (vblanks, the gate).
struct Sim {
  Env env;
  oc::Core<Env> core;
  std::vector<Event> ev;
  size_t next = 0;
  void At(int64_t t_ns, const Item& it) { ev.push_back({t_ns, it}); }
  void Process(const Item& it) {
    if (Plant("release_early") && it.fence && core.gate_open) {  // a fence written behind a pending flip
      core.WriteFence(env, it);
      return;
    }
    core.Process(env, it);
  }
  void Run(int64_t until_ns) {
    while (env.now <= until_ns) {
      core.Turn(env);
      while (next < ev.size() && ev[next].t <= env.now) Process(ev[next++].it);
      env.now += kStep;
    }
  }
};

static long Writes(const Env& e, const std::string& prefix) {
  long n = 0;
  for (const auto& s : e.trace) n += s.rfind(prefix, 0) == 0;
  return n;
}
static int IndexOf(const Env& e, const std::string& s, int from = 0) {
  for (size_t i = from; i < e.trace.size(); ++i)
    if (e.trace[i] == s) return int(i);
  return -1;
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "plant") == 0) g_plant = argv[2];

  // ---- the clock and the rate ----
  {
    oc::VblankClock c;
    c.SetInterval(oc::VblankIntervalNs(true, 60.0, 0.0, 0.0));
    uint32_t n = 0;
    for (int64_t t = 0; t <= 1000000000 + 1000000; t += 1000000) n += c.Due(t);
    Check(n == 60, "clock.60hz_one_second", "vblanks in 1.001 s at 1 ms steps: " + std::to_string(n) + " (want 60)");
    oc::VblankClock c2;
    c2.SetInterval(oc::VblankIntervalNs(true, 60.0, 0.0, 0.0));
    uint32_t m = 0;
    for (int64_t t = 0; t <= 1001000000; t += 5000000) m += c2.Due(t);
    Check(m == 60 || m == 59, "clock.5ms_polls_catch_up", "vblanks at 5 ms steps: " + std::to_string(m) + " (want 60; the grid never loses one to coarse polling)");
    oc::VblankClock c3;
    c3.SetInterval(16666666);
    c3.Due(0);
    const uint32_t a = c3.Due(100000000);  // 100 ms stall = 6 owed
    const uint32_t b = c3.Due(100000000 + 10000000000ll);  // 10 s stall: capped, grid restarts
    const uint32_t c4 = c3.Due(100000000 + 10000000000ll + 20000000);
    Check(a == 6 && b == oc::VblankClock::kMaxCatchUp && c3.dropped == 1 && c4 == 1, "clock.catch_up_and_cap",
          "100 ms stall " + std::to_string(a) + " (6); 10 s stall " + std::to_string(b) + " (cap " + std::to_string(oc::VblankClock::kMaxCatchUp) + "), restarts " +
              std::to_string(c3.dropped) + " (1), then " + std::to_string(c4) + " (1)");
    oc::VblankClock c5;
    c5.SetInterval(16666666, 0);
    c5.Due(0);
    c5.SetInterval(1000000, 5000000);  // vsync switched off at t = 5 ms: the next vblank is 1 ms later, not at the old 16.67 ms
    const uint32_t r1 = c5.Due(5500000), r2 = c5.Due(6100000);
    Check(r1 == 0 && r2 == 1, "clock.rate_change_regrids", "after a rate change at 5 ms: owed at 5.5 ms " + std::to_string(r1) + " (0), at 6.1 ms " + std::to_string(r2) + " (1)");
    Check(oc::VblankIntervalNs(true, 60.0, 0, 0) == 16666666 && oc::VblankIntervalNs(true, 60.0, 30.0, 240.0) == 33333333 && oc::VblankIntervalNs(false, 60.0, 30.0, 0.0) == 1000000 &&
              oc::VblankIntervalNs(false, 60.0, 30.0, 240.0) == 4166666,
          "interval.rules", "vsync on 60 Hz / override 30; vsync off 1 kHz / unthrottled 240 (graphics_system.cpp 633-647)");
  }

  // ---- the vblank ticks on a quiet thread, with the arguments MarkVblank gave ----
  {
    Sim s;
    s.Run(1001000000);
    Check(s.env.vblanks == 60 && s.env.dispatched_vblank == 60, "vblank.count_over_second", "dispatched " + std::to_string(s.env.dispatched_vblank) + " (want 60)");
    Check(s.env.last_vblank_source == 0 && s.env.last_vblank_cpu == 2 && s.env.last_vblank_dev == kDev, "vblank.dispatch_args", "source " + std::to_string(s.env.last_vblank_source) + " cpu " +
          std::to_string(s.env.last_vblank_cpu) + " device " + std::to_string(s.env.last_vblank_dev) + " (want 0 / 2 / the registered device)");
  }

  // ---- the gate: a fence behind a pending flip waits for the vblank that clears the flip word ----
  {
    Sim s;
    s.At(0, Fence(2));                // before any flip: written at once
    s.At(1000000, Flip());            // the flip is due at the next vblank: the gate opens
    s.At(2000000, Fence(4));          // behind it
    s.At(3000000, Fence(6));
    s.Run(10000000);                  // 10 ms: before the first vblank (16.67 ms)
    const bool early = s.env.Ld32(kBlock) == 2 && s.env.Ld32(kBlock + 4) == 0x1002;
    Check(early, "gate.fence_before_flip_at_once", "fence block after 10 ms: +0 = " + std::to_string(s.env.Ld32(kBlock)) + " (want 2: F2 written, F4 / F6 held)");
    Check(s.env.Ld32(kScratch + 4) == 1 && s.core.gate_open && s.core.gated.size() == 2, "gate.held_behind_flip", "flip word " + std::to_string(s.env.Ld32(kScratch + 4)) + ", held " +
          std::to_string(s.core.gated.size()) + " (want 1 / 2)");
    s.Run(20000000);                  // past the vblank
    const int vb = IndexOf(s.env, "I 0 2 " + std::to_string(kDev));
    const int f4 = IndexOf(s.env, "W 0 4");
    const int f6 = IndexOf(s.env, "W 0 6");
    Check(s.env.Ld32(kBlock) == 6 && s.env.Ld32(kScratch + 4) == 0 && !s.core.gate_open, "gate.released_by_vblank", "after the vblank: fence +0 = " + std::to_string(s.env.Ld32(kBlock)) +
          " (want 6), flip word " + std::to_string(s.env.Ld32(kScratch + 4)) + " (want 0)");
    Check(vb >= 0 && f4 > vb && f6 > f4, "gate.order_vblank_then_fences", "trace positions: vblank " + std::to_string(vb) + ", F4 " + std::to_string(f4) + ", F6 " + std::to_string(f6) +
          " (want vblank < F4 < F6)");
    Check(IndexOf(s.env, "W 4 4100") >= 0 && IndexOf(s.env, "W 4 4100") < IndexOf(s.env, "W 0 4"), "gate.marker_before_value", "heap marker before the fence number (the packet pair's order)");
  }

  // ---- two flips back to back: the second waits for the first's word, which only OUR vblank can clear ----
  {
    Sim s;
    s.At(1000000, Flip());
    s.At(2000000, Flip());
    s.At(3000000, Fence(8));
    s.Run(60000000);
    const long flips = s.env.counts[Count::Flip];
    Check(!s.env.gave_up && flips == 2, "waitword.ticks_vblank", "second flip dispatched after waiting on the first (flips " + std::to_string(flips) + ", gave up " + (s.env.gave_up ? "yes" : "no") + ")");
    Check(s.env.vblanks >= 2 && s.env.Ld32(kBlock) == 8, "waitword.fence_after_second_vblank", "vblanks " + std::to_string(s.env.vblanks) + " (>= 2), fence +0 = " + std::to_string(s.env.Ld32(kBlock)) + " (want 8)");
  }

  // ---- the vblank not ours (the SDK thread ticks it): nothing is dispatched here, the gate still follows the word ----
  {
    Sim s;
    s.env.ours = false;
    s.At(1000000, Flip());
    s.At(2000000, Fence(4));
    s.Run(100000000);
    Check(s.env.dispatched_vblank == 0 && s.core.gated.size() == 1, "notours.no_dispatch_held", "dispatched " + std::to_string(s.env.dispatched_vblank) + ", held " + std::to_string(s.core.gated.size()) + " (want 0 / 1)");
    s.env.mem[kScratch + 4] = 0;  // the SDK's thread ran the vblank handler
    s.Run(101000000);
    Check(s.env.Ld32(kBlock) == 4 && s.core.gated.empty(), "notours.released_when_word_clears", "fence +0 = " + std::to_string(s.env.Ld32(kBlock)) + " (want 4)");
  }

  // ---- the interrupt emitter's stores and order (as before this lane) ----
  {
    Sim s;
    s.At(0, Flip());
    s.Run(1000000);
    Check(s.env.Ld32(kScratch + 16) == kBadF00d && s.env.Ld32(kScratch + 20) == 0x100 && s.env.Ld32(kScratch + 0) == 0 && s.env.Ld32(kScratch + 4) == 1, "interrupt.scratch_words",
          "scratch +16 " + std::to_string(s.env.Ld32(kScratch + 16)) + " (BADF00D), +20 " + std::to_string(s.env.Ld32(kScratch + 20)) + " (0x100), +0 " + std::to_string(s.env.Ld32(kScratch)) + " (0), flip word " +
              std::to_string(s.env.Ld32(kScratch + 4)) + " (1)");
  }

  // ---- the scene-timing pair and the fixed value ----
  {
    auto run = [](double fixed_ms, double scene_ms, uint64_t& stored, bool& over_budget) {
      Sim s;
      s.env.fixed_ms = fixed_ms;
      const uint32_t arg = 0x00A00008;
      s.At(0, Scene(oc::kSceneTimingBegin, arg));
      s.At(int64_t(scene_ms * 1e6), Scene(oc::kSceneTimingEnd, arg));
      s.Run(int64_t(scene_ms * 1e6) + 2000000);
      stored = (uint64_t(s.env.Ld32(arg + 8)) << 32) | s.env.Ld32(arg + 12);
      over_budget = double(stored) * double(kScale) * 1000.0 / kBudgetMs >= 1.0;  // 0x824D7988
    };
    uint64_t real = 0, fixed = 0;
    bool over_real = false, over_fixed = false;
    run(0.0, 40.0, real, over_real);   // a scene our thread took 40 ms over (the reader's wait)
    run(1.0, 40.0, fixed, over_fixed);  // the same with the fixed value
    const uint64_t want = oc::SceneTicksForMs(1.0, kScale);
    Check(real > 1900000 && over_real, "scene.measured_is_our_elapsed", "as measured: " + std::to_string(real) + " ticks (~40 ms), 0x824D7988 would say over budget: " + (over_real ? "yes" : "no"));
    Check(fixed == want && want == 49875 && !over_fixed, "scene.fixed_value_reported", "fixed 1 ms: " + std::to_string(fixed) + " ticks (want " + std::to_string(want) + " = 49875), over budget: " + (over_fixed ? "yes" : "no") + " (want no)");
    Check(oc::SceneTicksForMs(1e300, kScale) == oc::SceneTicksForMs(1000.0, kScale) && oc::SceneTicksForMs(1e300, 1e-30f) <= 9000000000000000000ull, "scene.huge_value_clamped",
          "1e300 ms gives " + std::to_string(oc::SceneTicksForMs(1e300, kScale)) + " ticks (= the 1000 ms value), no overflow");
    Check(oc::SceneTicksForMs(1.0, 0.0f) == 0 && oc::SceneTicksForMs(0.0, kScale) == 0 && oc::SceneTicksForMs(-1.0, kScale) == 0, "scene.ticks_rules", "scale <= 0 or ms <= 0 gives 0 (left as measured)");
  }

  std::printf("%s\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
