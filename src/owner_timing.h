// wwe2k14 - Session 100, strip-360 B4 / B5 (STRIP360_PLAN.md): in owner mode (the new renderer draws; the SDK's command processor draws and
// copies nothing, gpu_no_draw) the game's fences, interrupts and flips are answered here, on our own guest thread, instead of by packets the
// old command processor would execute. Facts from docs\renderer\ADAPTER_CALLS.md section 5 and the Session 87 code that did the same on the
// SDK's command thread (src\d3d_noring.h 680-810, thirdparty\rexglue-sdk-src\src\graphics\d3d_noring.cpp 720-830); this is our own cut:
// no ring, no packets, no SDK command thread in the path.
//
//   fence      the segment-end pair 0x82CB63D0: the library's bookkeeping as the original keeps it; the heap marker (block +4) then the fence
//              number (block +0) written by our thread, in order with the interrupts, after the reader has taken the work it covers.
//   interrupt  the emitter 0x82CB6510 (r5 flags: cpu mask in bits 24-29; r6 callback; r7 argument): scratch +16 callback, +20 argument
//              (unless flags & 0x80), +0 cpu mask, then the guest's interrupt routine for each cpu (GraphicsSystem::DispatchInterruptCallback,
//              source 1, on our guest thread as the command thread ran it), then (flags & 4) scratch +16 := 0x0BADF00D.
//   flip       the present's flip handshake 0x82CB3300: its two helper calls and per-frame bit as the original; the flip word (scratch +4)
//              := 1, the flip scheduler 0x82CB2A28 as an interrupt with the packed interval word, then THE pacing gate: fences after the flip
//              wait until the flip word reads 0 (the game's own flip scheduler or vblank handler clears it; the SDK's VSync thread runs the
//              vblank handler), so the next present's reason-3 wait (the frame limiter) passes at the due vblank, never at once (Session 51:
//              fences written at once = an unpaced game).
#pragma once

#include <algorithm>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/thread/mutex.h>
#include <rex/logging.h>
#include <rex/graphics/graphics_system.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>

#include <rex/det.h>

#include "d3d_noring.h"
#include "owner_boot_submit.h"
#include "owner_hires_wait.h"
#include "owner_timing_core.h"
#include "guest_gpu_only.h"
#include "renderer_adapter/reader.h"

REXCVAR_DEFINE_BOOL(wwe2k14_owner_timing, true, "wwe2k14",
                    "Session 100 (strip-360 B4/B5): in owner mode the game's fences, interrupts and flips are answered by our own guest thread "
                    "(no packets for the old command processor); false: the old path's packets as before");

REXCVAR_DEFINE_BOOL(wwe2k14_owner_no_ring, true, "wwe2k14",
                    "Session 100 (strip-360 B6): in owner mode (with --wwe2k14_owner_timing) the library's ring submit is skipped: the old command "
                    "processor gets no work and idles; false: segments still submitted (and parsed, drawing nothing)");

REXCVAR_DEFINE_BOOL(wwe2k14_owner_boot_submit_ours, false, "wwe2k14",
                    "Session 101 lane 231 (LEAVE_THE_COMMAND_STREAM 3a, owner mode with --wwe2k14_owner_no_ring): the boot's one ring submit is answered by us "
                    "(src/owner_boot_submit.h: the first segment is scanned; if it holds nothing the game needs from the processor, the ring read pointer word "
                    "is set and the submit is skipped like every other) instead of reaching the SDK's command processor; a packet the scan cannot vouch for "
                    "hands the submit to the original. false: the boot submit runs in the SDK as before");

REXCVAR_DEFINE_BOOL(wwe2k14_owner_guest_gpu_only, false, "wwe2k14",
                    "Session 101 lane 241 (LEAVE_THE_COMMAND_STREAM 3b, owner mode with --wwe2k14_owner_boot_submit_ours): the SDK builds no graphics provider, no "
                    "command processor and no presenter (SDK patch 113, RuntimeConfig::guest_gpu_only: the GPU MMIO range and the VSync thread only): no second "
                    "VkDevice, no 'GPU Commands' thread. Refused with a log line unless owner mode, owner timing, owner_no_ring and owner_boot_submit_ours are on. "
                    "A path that still needs the processor (the boot scan refusing, a tiling / recording / predication fallback, a raw ring write) is logged as "
                    "[owner][gpu-only] ERROR and cannot be served. false: the SDK keeps its command processor as before");

REXCVAR_DEFINE_BOOL(wwe2k14_owner_vblank_ours, false, "wwe2k14",
                    "Session 101 lane 260 (LEAVE_THE_COMMAND_STREAM 3e, owner mode with --wwe2k14_owner_guest_gpu_only): the guest's vblank comes from OUR clock: the 'Owner timing' "
                    "thread dispatches the game's interrupt dispatcher (source 0, cpu 2) at 60 Hz (vsync on; --gpu_vblank_rate_override_hz honoured) or 1 kHz / "
                    "--gpu_vblank_unthrottled_hz (vsync off) exactly as the SDK's VSync thread did, and the SDK starts no VSync thread (SDK patch 114, "
                    "RuntimeConfig::guest_vblank_ours). Refused with a log line when det_frames / det_serial are on, when guest_gpu_only is not on, or when the "
                    "SDK build lacks patch 114. false: the SDK's VSync thread as before");

REXCVAR_DEFINE_DOUBLE(wwe2k14_owner_scene_timing_fixed_ms, 0.0, "wwe2k14",
                      "Session 101 lane 260 (LEAVE_THE_COMMAND_STREAM 3e): > 0: the scene-timing pair 0x82C16978 / 0x82C16980 (the only GPU timing the game reads; it picks a quality "
                      "state, ADAPTER_CALLS.md section 5) reports this many milliseconds for every scene instead of the time our thread took between the two items. "
                      "0: as before (our thread's own elapsed time). Logged as [owner][scene-timing]");

REX_EXTERN(__imp__sub_82CB3300);
REX_EXTERN(__imp__sub_82CB5560);
REX_EXTERN(sub_82CB5758);  // the game's GPU interrupt dispatcher (VdSetGraphicsInterruptCallback(0x82CB5758, the device): run logs)

namespace wwe2k14::owner_timing {

namespace dl = wwe2k14::d3d_layer;
namespace dn = wwe2k14::d3d_noring;

inline bool Active() { return REXCVAR_GET(wwe2k14_rdr_owner) && REXCVAR_GET(wwe2k14_owner_timing); }

using Item = owner_core::Item;  // (src\owner_timing_core.h)

struct Counters {
  std::atomic<uint64_t> fences{0}, interrupts{0}, flips{0}, gated{0}, released{0}, fallbacks{0}, dispatch_waits{0};
  std::atomic<uint64_t> submits_skipped{0}, submits_run{0}, raw_ring_writes{0};  // B6
  std::atomic<uint64_t> boot_raw_writes{0}, boot_answered{0}, boot_original{0};  // 3a (lane 231)
  std::atomic<uint64_t> vblanks{0}, scene_begin{0}, scene_end{0}, scene_fixed{0};  // 3e (lane 260)
};

struct Thread {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<Item> q;
  std::once_flag once;
  rex::system::object_ref<rex::system::XHostThread> thread;
  uint8_t* base = nullptr;
  // 3e (lane 260): the vblank clock (our thread only) and the device the game registered with its interrupt callback
  owner_core::VblankClock clock;
  std::atomic<uint32_t> vdev{0};
  uint32_t rate_tick = 0;
  bool vsync = true;
  std::vector<uint32_t> late_us;  // how late each vblank was dispatched (thread-only; the 10 s line takes median / max and clears it)
  Counters c;
  // B6: set when packets the old command processor must see went into a segment or the ring (an original run for an unusual call, the
  // raw ring writer 0x82CB5560); the next ring submit then runs the original (its WPTR write wakes the processor), and clears it.
  std::atomic<bool> ring_needed{false};
  // 3a (lane 231): the boot's raw ring write was taken by --wwe2k14_owner_boot_submit_ours (it did not set ring_needed); the next submit is the boot
  // submit, which AnswerBoot decides.
  std::atomic<bool> boot_pending{false};
  std::vector<std::pair<uint32_t, uint32_t>> boot_scratch;  // (guest address, value) pairs; written before the item is pushed, read by the timing thread after it pops it
  uint32_t boot_prefix_pieces = 0;  // prefix pieces the boot answer has skipped before the segment (the Submit hook's thread only)
};
inline Thread g_t;

static_assert(wwe2k14::owner_boot::kDevFenceBlock == dn::kDevFenceBlock, "owner_boot_submit.h: the fence block offset");
// The guest float the scene-timing readers scale ticks by (listing 0x824D7988: lis 0x8393 / lfs -5772 -> 0x8392E974), see LEAVE_THE_COMMAND_STREAM.md 10.1.
inline constexpr uint32_t kSceneTimingScaleAddr = 0x8392E974u;

static_assert(wwe2k14::owner_boot::kDevRingWrite == 11088 && wwe2k14::owner_boot::kDevRingMask == 15304, "owner_boot_submit.h: ring words");

// The game's interrupt dispatcher run on our guest thread as the SDK's FunctionDispatcher::ExecuteInterrupt runs it for the command thread
// (thirdparty\rexglue-sdk-src\src\system\function_dispatcher.cpp 143-180): the global lock held, r3 the source, r4 the registered data
// (the device: the listing loads the scratch block from r4 + 11028), the PCR's TLS word zeroed during the call; the cpu set first (the
// dispatcher clears bit (1 << PCR+268) of scratch +0). GraphicsSystem::DispatchInterruptCallback itself is not exported to the exe.
inline void RunGuestInterrupt(uint8_t* base, uint32_t source, uint32_t cpu, uint32_t device) {
  auto* thread = rex::system::XThread::GetCurrentThread();
  if (!thread || !thread->thread_state()) return;
  thread->SetActiveCpu(uint8_t(cpu));
  auto lock = rex::thread::global_critical_region::AcquireDirect();
  PPCContext& ctx = *thread->thread_state()->context();
  ctx.r3.u64 = source;
  ctx.r4.u64 = device;
  const uint32_t pcr = uint32_t(ctx.r13.u64);
  const uint32_t old_tls = dl::Ld32(base, pcr);
  dl::St32(base, pcr, 0);
  sub_82CB5758(ctx, base);
  dl::St32(base, pcr, old_tls);
}

// ---- 3e (lane 260): the vblank from our clock ----
inline std::atomic<bool> g_vblank_ours{false};  // set once in OnPreSetup when the switch and its preconditions hold (and the SDK has patch 114)
inline bool VblankOurs() { return g_vblank_ours.load(std::memory_order_relaxed); }
inline std::atomic<bool> g_stop{false};  // set at shutdown (OnShutdown): no more vblanks are dispatched into a tearing-down runtime

// What the game registered with VdSetGraphicsInterruptCallback (the device), read from the SDK's GraphicsSystem (patch 114's accessors). 0 until the game registered.
template <class G>
inline uint32_t RegisteredDeviceOf(G* g) {
  if constexpr (requires { g->interrupt_callback_data(); }) return g->interrupt_callback_data();
  else return 0;
}
// The exe compiles against the installed SDK headers: without patch 114 the config field does not exist and the switch is refused (wwe2k14_app.h).
template <class C>
inline constexpr bool kSdkHasVblankOurs = requires(C& c) { c.guest_vblank_ours; };
template <class G>
inline constexpr bool kSdkHasDeviceAccessor = requires(G* g) { g->interrupt_callback_data(); };
// Both halves of patch 114 or neither: a config field without the accessor (or the reverse) would tick a vblank with no device.
static_assert(kSdkHasVblankOurs<rex::RuntimeConfig> == kSdkHasDeviceAccessor<rex::graphics::GraphicsSystem>, "SDK patch 114 is half applied: RuntimeConfig::guest_vblank_ours and GraphicsSystem::interrupt_callback_data() go together");

template <class C>
inline void SetVblankOurs(C& c) {
  if constexpr (kSdkHasVblankOurs<C>) c.guest_vblank_ours = true;
}

inline double CvarDouble(const char* name) {
  const std::string v = rex::cvar::GetFlagByName(name);
  return v.empty() ? 0.0 : std::strtod(v.c_str(), nullptr);
}

inline owner_wait::HiResWait g_hires;  // (src\owner_hires_wait.h, lane 306)
// Shutdown: the loop then waits for items with no deadline (it used to spin at 100 % until exit); the stop wakes it once.
inline void RequestStop() {
  g_stop.store(true, std::memory_order_relaxed);
  g_t.cv.notify_one();
  g_hires.Signal();
}

// The Env of owner_core::Core on the real guest (the timing thread's only).
struct GuestEnv {
  uint8_t* base = nullptr;
  std::chrono::steady_clock::time_point wait_t0;
  bool wait_logged = false;
  uint32_t wait_at = 0, wait_value = 0;

  uint32_t Ld32(uint32_t a) const { return dl::Ld32(base, a); }
  void St32(uint32_t a, uint32_t v) { dl::St32(base, a, v); }
  void Release() { std::atomic_thread_fence(std::memory_order_release); }
  void FullFence() { std::atomic_thread_fence(std::memory_order_seq_cst); }
  void RunGuestInterrupt(uint32_t source, uint32_t cpu, uint32_t device) { owner_timing::RunGuestInterrupt(base, source, cpu, device); }
  uint32_t Device() {
    uint32_t d = g_t.vdev.load(std::memory_order_relaxed);
    if (d) return d;
    auto* rt = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->emulator() : nullptr;
    auto* gfx = rt ? rt->graphics_system() : nullptr;
    d = gfx ? RegisteredDeviceOf(static_cast<rex::graphics::GraphicsSystem*>(gfx)) : 0;
    if (d) {
      g_t.vdev.store(d, std::memory_order_relaxed);
      REXLOG_INFO("[owner][vblank] the game registered its interrupt callback: device {:08X}; the vblank now comes from our clock", d);
    }
    return d;
  }
  void Count(owner_core::Count k) {
    using owner_core::Count;
    Counters& c = g_t.c;
    switch (k) {
      case Count::Fence: c.fences.fetch_add(1, std::memory_order_relaxed); break;
      case Count::Gated: c.gated.fetch_add(1, std::memory_order_relaxed); break;
      case Count::Released: c.released.fetch_add(1, std::memory_order_relaxed); break;
      case Count::Interrupt: c.interrupts.fetch_add(1, std::memory_order_relaxed); break;
      case Count::Flip: c.flips.fetch_add(1, std::memory_order_relaxed); break;
      case Count::DispatchWait: c.dispatch_waits.fetch_add(1, std::memory_order_relaxed); break;
      case Count::Vblank: c.vblanks.fetch_add(1, std::memory_order_relaxed); break;
    }
  }
  // The rate cvars are the SDK loop's own (vsync, gpu_vblank_rate_override_hz, gpu_vblank_unthrottled_hz), re-read every 32 calls.
  void RefreshRate() {
    if ((g_t.rate_tick++ & 31u) != 0) return;
    const std::string vs = rex::cvar::GetFlagByName("vsync");
    g_t.vsync = vs != "false" && vs != "0";
    g_t.clock.SetInterval(owner_core::VblankIntervalNs(g_t.vsync, 60.0, CvarDouble("gpu_vblank_rate_override_hz"), CvarDouble("gpu_vblank_unthrottled_hz")), NowNs());
  }
  static int64_t NowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
  uint32_t VblankOwed() {
    if (!VblankOurs() || g_stop.load(std::memory_order_relaxed)) return 0;
    RefreshRate();
    const int64_t now = NowNs();
    const int64_t deadline = g_t.clock.NextDeadline();
    const uint32_t n = g_t.clock.Due(now);
    if (!n || !Device()) return 0;  // (no callback registered yet: nothing to dispatch, as MarkVblank)
    if (g_t.late_us.size() < 8192) g_t.late_us.push_back(uint32_t(std::min<int64_t>(std::max<int64_t>(now - deadline, 0) / 1000, 4000000)));
    return n;
  }
  void WaitBegin(uint32_t at, uint32_t value) {
    wait_t0 = std::chrono::steady_clock::now();
    wait_logged = false;
    wait_at = at;
    wait_value = value;
  }
  bool Sleep() {
    if (VblankOurs() && g_hires.Available()) g_hires.Until(NowNs() + 200000, 0, false);
    else std::this_thread::sleep_for(std::chrono::microseconds(200));
    if (!wait_logged && std::chrono::steady_clock::now() - wait_t0 > std::chrono::seconds(5)) {
      wait_logged = true;
      REXLOG_WARN("[owner][timing] waiting over 5 s for guest word {:08X} == {:08X} (now {:08X})", wait_at, wait_value, Ld32(wait_at));
    }
    return true;
  }
  // Part 2 of 3e: the scene-timing end callback has run; report a fixed time instead of our thread's own elapsed time (guest 0x82C16980 stored `now - start` at arg + 8 as
  // two big-endian words; readers 0x824D7988 / 0x827F09E0 read that int64 and scale it by the float at 0x8392E974, section 10.1).
  void OnInterruptDone(const Item& it) {
    if (!(REXCVAR_GET(wwe2k14_owner_scene_timing_fixed_ms) > 0.0) && !VblankOurs()) return;  // both switches off: no extra work, no log line
    if (it.b == owner_core::kSceneTimingBegin) {
      g_t.c.scene_begin.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (it.b != owner_core::kSceneTimingEnd) return;
    const uint64_t n = g_t.c.scene_end.fetch_add(1, std::memory_order_relaxed) + 1;
    const double fixed_ms = REXCVAR_GET(wwe2k14_owner_scene_timing_fixed_ms);
    const uint32_t arg = it.c;
    const uint64_t real = (uint64_t(Ld32(arg + 8)) << 32) | Ld32(arg + 12);
    const float scale = std::bit_cast<float>(Ld32(kSceneTimingScaleAddr));
    if (fixed_ms > 0.0 && scale > 0.0f) {
      const uint64_t ticks = owner_core::SceneTicksForMs(fixed_ms, scale);
      St32(arg + 8, uint32_t(ticks >> 32));
      St32(arg + 12, uint32_t(ticks));
      Release();
      g_t.c.scene_fixed.fetch_add(1, std::memory_order_relaxed);
    } else if (fixed_ms > 0.0) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true)) REXLOG_WARN("[owner][scene-timing] the guest scale word {:08X} reads {} (not > 0): the real elapsed time is left as it is", kSceneTimingScaleAddr, scale);
    }
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    const auto now = std::chrono::steady_clock::now();
    if (n <= 10 || now - last > std::chrono::seconds(1)) {
      last = now;
      REXLOG_INFO("[owner][scene-timing] end #{} slot {:08X}: real elapsed {} ticks ({:.3f} ms by the guest scale {}), reported {} ({})", n, arg, real,
                  double(real) * double(scale) * 1000.0, scale, fixed_ms > 0.0 && scale > 0.0f ? owner_core::SceneTicksForMs(fixed_ms, scale) : real,
                  fixed_ms > 0.0 ? "fixed" : "as measured");
    }
  }
};
inline owner_core::Core<GuestEnv> g_core;

inline int Loop() {
  auto last_log = std::chrono::steady_clock::now();
  owner_wait::CpuMeter cpu_meter;  // lane 306: this thread's CPU in each 10 s window (the spin check)
  GuestEnv env;
  env.base = g_t.base;
  auto& core = g_core;
  for (;;) {
    Item it;
    bool have = false;
    {
      std::unique_lock lock(g_t.mu);
      auto has_item = [] { return !g_t.q.empty(); };
      if (g_stop.load(std::memory_order_relaxed) && VblankOurs()) {  // lane 306: shutting down, no vblank to wait for: items only
        if (g_t.q.empty()) {
          if (g_hires.Available()) {
            lock.unlock();
            g_hires.WaitEvent();
            lock.lock();
          } else {
            g_t.cv.wait(lock, has_item);
          }
        }
      } else if (VblankOurs() && g_hires.Available()) {  // 3e (Windows): the high-resolution timer + spin, the queue's event waking it at once
        if (g_t.q.empty()) {
          // Lane 306: the gate poll never spins (spin 0); only the real vblank deadline spins, for its last 0.5 ms (owner_wait::PlanWait, tested by tools\owner\spin_cpu_test.cpp)
          const owner_wait::WaitPlan plan = owner_wait::PlanWait(owner_wait::HiResWait::NowNs(), g_t.clock.NextDeadline(), core.gate_open, g_t.clock.interval_ns);
          lock.unlock();
          g_hires.Until(plan.target_ns, plan.spin_ns, true);
          lock.lock();
        }
      } else if (VblankOurs()) {  // 3e: wake for the next vblank deadline as well (and, behind a flip, every 1 ms since lane 306, 250 us before)
        auto until = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(g_t.clock.NextDeadline()));
        if (core.gate_open) until = std::min(until, std::chrono::steady_clock::now() + std::chrono::nanoseconds(owner_wait::kGatePollNs));  // (lane 306: 1 ms, was 250 us)
        g_t.cv.wait_until(lock, until, has_item);
      } else if (core.gate_open) {
        g_t.cv.wait_for(lock, std::chrono::microseconds(250), has_item);
      } else {
        g_t.cv.wait(lock, has_item);
      }
      if (!g_t.q.empty()) {
        it = g_t.q.front();
        g_t.q.pop_front();
        have = true;
      }
    }
    core.Turn(env);  // the vblanks that fell due (ours only), then the gate
    if (have) {
      if (it.boot_writes) {  // 3a: the boot segment's scratch write-backs, in stream order
        for (const auto& w : g_t.boot_scratch) env.St32(w.first, w.second);
        env.Release();
      } else {
        core.Process(env, it);
      }
    }
    if (std::chrono::steady_clock::now() - last_log > std::chrono::seconds(10)) {
      const auto now_log = std::chrono::steady_clock::now();
      const double window_ms = std::chrono::duration<double, std::milli>(now_log - last_log).count();
      last_log = now_log;
      const double cpu_ms = cpu_meter.Take(window_ms);
      const Counters& c = g_t.c;
      REXLOG_INFO("[owner][timing] so far: fences {} ({} held behind a flip, {} released), interrupts {}, flips {}, waits for a dispatch {}, "
                  "left to the original {}; this thread's CPU {:.0f} ms in the last {:.0f} ms = {:.1f} % of a core",
                  c.fences.load(), c.gated.load(), c.released.load(), c.interrupts.load(), c.flips.load(), c.dispatch_waits.load(),
                  c.fallbacks.load(), cpu_ms, window_ms, window_ms > 0.0 ? 100.0 * cpu_ms / window_ms : 0.0);
      if (VblankOurs()) {
        std::sort(g_t.late_us.begin(), g_t.late_us.end());
        const double med = g_t.late_us.empty() ? 0.0 : g_t.late_us[g_t.late_us.size() / 2] / 1000.0, mx = g_t.late_us.empty() ? 0.0 : g_t.late_us.back() / 1000.0;
        REXLOG_INFO("[owner][vblank] so far: vblanks {} from our clock (interval {} us, vsync {}, grid restarts {}, timer {}), late this period: median {:.2f} ms, max {:.2f} ms ({} samples); "
                    "scene timing: begin {}, end {}, fixed {}",
                    c.vblanks.load(), g_t.clock.interval_ns / 1000, g_t.vsync ? "on" : "off", g_t.clock.dropped, g_hires.Available() ? "high-resolution" : "condition_variable", med, mx,
                    g_t.late_us.size(), c.scene_begin.load(), c.scene_end.load(), c.scene_fixed.load());
        g_t.late_us.clear();
      }
      REXLOG_INFO("[owner][ring] so far: submits skipped {}, run {} (raw ring writes {}; boot submit: ours {}, to the SDK {}, boot raw writes taken {})",
                  c.submits_skipped.load(), c.submits_run.load(), c.raw_ring_writes.load(), c.boot_answered.load(), c.boot_original.load(),
                  c.boot_raw_writes.load());
      // B3 (owner_null_copy.h): the clears / resolves taken, and the ones left to the original by reason.
      const auto& nc = wwe2k14::owner_null_copy::g_c;
      std::string left;
      for (uint32_t i = 0; i < wwe2k14::owner_null_copy::kBailCount; ++i) {
        if (const uint64_t n = nc.bails[i].load()) left += fmt::format("{}{} {}", left.empty() ? "" : ", ", wwe2k14::owner_null_copy::kBailNames[i], n);
      }
      REXLOG_INFO("[owner][copy] so far: clears {} (+{} empty), resolves {}; left to the original: {}", nc.clears.load(), nc.clears_empty.load(),
                  nc.resolves.load(), left.empty() ? "none" : left);
    }
  }
}

inline void Start(uint8_t* base) {
  std::call_once(g_t.once, [base] {
    g_t.base = base;
    if (VblankOurs()) g_hires.Init();  // (the queue event must exist before any Push)
    g_t.thread = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(REX_KERNEL_STATE(), 128 * 1024, 0, [] { return Loop(); }));
    g_t.thread->set_name("Owner timing");
    g_t.thread->Create();
    REXLOG_INFO("[owner][timing] the game's fences, interrupts and flips are answered by our thread (no packets for the old command processor)");
  });
}

// 3e: with the vblank ours the thread must run before the first fence / flip item (the SDK thread ticked from boot): called from OnPostSetup.
inline void StartEarly(uint8_t* base) {
  if (VblankOurs()) Start(base);
}

inline void Push(const Item& it) {
  {
    std::lock_guard lock(g_t.mu);
    g_t.q.push_back(it);
  }
  g_t.cv.notify_one();
  if (VblankOurs()) g_hires.Signal();  // 3e: the high-resolution wait listens to this event, not the condition variable
}

// Everything a fence or an interrupt covers has been taken by the reader (texture reads on its workers included) before it is answered.
inline void JoinReader() {
  // Only that the reader's workers have finished reading guest memory (texture jobs read it; geometry jobs read copies made at the draw):
  // their own wait, not the reader's lock (a library call made under it would deadlock); the results are committed at the frame's join.
  rdr_reader::g_workers.WaitAll();
}

inline bool Unusual(const uint8_t* base, uint32_t dev) {
  return dn::t_original_depth || dn::Recording(base, dev) || dl::Ld32(base, dev + dn::kDevPredicate) != 0 || (base[dev + dn::kDevByte11068] & 1);
}

// Lane 241 (3b): under guest_gpu_only nothing parses a ring or a segment, so a path that hands work to "the original processor" can never complete (the fence /
// flip packets it wrote are never run: the next fence wait hangs). We cannot serve it; say so, loudly, the first few times, with the reason.
inline void NeedsProcessor(const char* why) {
  if (!GuestGpuOnly()) return;
  static std::atomic<int> n{0};
  if (n.fetch_add(1, std::memory_order_relaxed) < 5)
    REXLOG_ERROR("[owner][gpu-only] ERROR: {} needs the SDK's command processor, which --wwe2k14_owner_guest_gpu_only removed: the game will hang at its next fence wait", why);
}

// An original ran for an unusual call: its packets (a fence, an interrupt, a flip) are in the segment and must reach the old processor.
inline void FellBack() {
  g_t.c.fallbacks.fetch_add(1, std::memory_order_relaxed);
  NeedsProcessor("an unusual call (tiling wrapper / recording / predication / original depth)");
  g_t.ring_needed.store(true, std::memory_order_release);
}

// 0x82CB63D0 (r3 device, r4 the write pointer; returns the pointer after what it wrote).
inline bool Fence(PPCContext& ctx, uint8_t* base) {
  if (!Active()) return false;
  const uint32_t dev = ctx.r3.u32, ptr = ctx.r4.u32;
  if (Unusual(base, dev)) {
    FellBack();
    return false;
  }
  Start(base);
  const uint32_t heap_bits = dl::Ld32(base, dev + dn::kDevHeapBits);
  const uint32_t write_ptr = dl::Ld32(base, dev + dn::kDevWritePtr);
  const uint32_t fence = dl::Ld32(base, dev + dn::kDevFenceNext);
  const uint32_t block = dl::Ld32(base, dev + dn::kDevFenceBlock);
  dl::St32(base, dev + 13608, write_ptr);  // (the original's two bookkeeping stores)
  dl::St32(base, dev + 13612, heap_bits);
  if (base[dev + dn::kDevByte11072] & 0x80) dn::ApplyMiscFlush(base, dev);
  JoinReader();
  Item it;
  it.fence = true;
  it.a = block + 4;
  it.b = (heap_bits & 3u) | write_ptr;
  it.c = block;
  it.d = fence;
  Push(it);
  dl::St32(base, dev + dn::kDevFenceNext, fence + 2);  // (the original's last store)
  ctx.r3.u64 = ptr;  // nothing was written
  return true;
}

// 0x82CB6510 (r3 device, r4 the write pointer, r5 flags, r6 callback, r7 argument; returns the pointer after what it wrote).
inline bool Interrupt(PPCContext& ctx, uint8_t* base) {
  if (!Active()) return false;
  const uint32_t dev = ctx.r3.u32, ptr = ctx.r4.u32;
  if (Unusual(base, dev)) {
    FellBack();
    return false;
  }
  Start(base);
  uint32_t flags = ctx.r5.u32;
  if (!(flags & 6)) flags |= 6;
  if ((flags & 2) && (!(flags & 1) || (base[dev + dn::kDevByte11072] & 0x80))) dn::ApplyMiscFlush(base, dev);
  JoinReader();
  Item it;
  it.flip = ctx.r6.u32 == dn::kFlipScheduler;
  it.a = ctx.r5.u32;
  it.b = ctx.r6.u32;
  it.c = ctx.r7.u32;
  it.d = dl::Ld32(base, dev + dn::kDevScratch);
  it.e = 0;
  it.dev = dev;
  Push(it);
  ctx.r3.u64 = ptr;
  return true;
}

// 0x82CB3300 (r3 device, r4 flag): the present's flip handshake (listing read in Session 87; as d3d_noring's hook, our item instead of its).
inline bool Flip(PPCContext& ctx, uint8_t* base) {
  if (!Active()) return false;
  const uint32_t dev = ctx.r3.u32, flag = ctx.r4.u32;
  if (Unusual(base, dev)) {
    FellBack();
    return false;
  }
  Start(base);
  const uint32_t mode = dl::Ld32(base, dev + dn::kDevFrameRateMode);
  const uint32_t interval = (mode == 0 || mode == 1) ? 1u : mode == 2 ? 2u : mode == 4 ? 3u : 0u;
  const uint32_t packed = (((interval << 8) & 0xFFFFFF00u) | ((dl::Ld32(base, dev + dn::kDevLateThreshold) >> 23) & 0x7Fu)) | flag;
  const bool waits = flag == 0 && dl::Ld32(base, dev + dn::kDevNoWaitFlags) == 0;
  ctx.r3.u64 = dev;
  ctx.r4.u64 = 1;
  ctx.lr = 0x82CB331C;
  sub_82CC4338(ctx, base);
  ctx.r3.u64 = dev;
  ctx.r4.u64 = 0;
  ctx.lr = 0x82CB3328;
  sub_82CB7770(ctx, base);
  if (dl::Ld32(base, dev + dn::kDevWritePtr) > dl::Ld32(base, dev + dn::kDevSoftEnd)) {
    ctx.r3.u64 = dev;
    ctx.lr = 0x82CB3354;
    sub_82CB6EA8(ctx, base);
  }
  if (Unusual(base, dev)) {  // 0x82CB7770 turned the tiling wrapper on: the original whole (its helpers run once more, as d3d_noring)
    FellBack();
    ctx.r3.u64 = dev;
    ctx.r4.u64 = flag;
    ++dn::t_original_depth;
    __imp__sub_82CB3300(ctx, base);
    --dn::t_original_depth;
    return true;
  }
  dn::ApplyMiscFlush(base, dev);  // (the emitter's flags 0 -> 6: the flush branch is always taken for the handshake)
  JoinReader();
  Item it;
  it.flip = true;
  it.a = 0;
  it.b = dn::kFlipScheduler;
  it.c = packed;
  it.d = dl::Ld32(base, dev + dn::kDevScratch);
  it.e = waits ? 3u : 0u;
  it.dev = dev;
  Push(it);
  ctx.r3.u64 = dev;
  ctx.r4.u64 = 0;
  ctx.lr = 0x82CB34C8;
  sub_82CC4338(ctx, base);
  base[dev + dn::kDevByte11069] = uint8_t(base[dev + dn::kDevByte11069] | 0x10);
  ctx.r3.u64 = dl::Ld32(base, dev + dn::kDevWritePtr);
  return true;
}

// B6: the ring submit 0x82CB5EF0 (r3 device, r4 the pieces, r5 their count). With draws, clears and resolves reduced to bookkeeping (B3,
// OwnerNullDraw) and fences, interrupts and flips answered by our thread (B4 / B5), nothing in a segment is observed by the game through
// the old command processor any more (docs\systems\owner_mode_duties.md section 3: what is left is VdSwap's packets, the kick's coherency
// handshake and the GPR split, none of which the game reads back). The submit is skipped whole: no indirect-buffer packets into the primary
// ring, its software write index (device +11088) and CP_RB_WPTR (MMIO 0x714) untouched, so the ring-room wait 0x82CB54B0 (reason 1) is
// never reached and the processor's thread idles. Segment memory comes back through the heap marker our thread writes (B4). The original
// runs when packets the processor must see were written (ring_needed), in an unusual mode, or with a debug object / submit callback.
// Reads an indirect buffer by its GPU (physical) address through the 0xA0000000-view alias, as the boot's prefix piece is read (run 7157: that read returned the
// prefix the listing predicts, so the alias works there). Only the low physical range, only the sizes the scan allows.
struct GuestIbReader : wwe2k14::owner_boot::IbReader {
  uint8_t* base;
  explicit GuestIbReader(uint8_t* b) : base(b) {}
  bool Read(uint32_t gpu, uint32_t dwords, std::vector<uint32_t>& out) override {
    if (gpu & 3u || gpu >= 0x1FF00000u || dwords == 0 || dwords > wwe2k14::owner_boot::kMaxIbDwords) return false;
    out.resize(dwords);
    for (uint32_t i = 0; i < dwords; ++i) out[i] = dl::Ld32(base, 0xA0000000u + gpu + 4 * i);
    return true;
  }
};

// 3a (lane 231): the boot's ring submits (ctx.r3 device, r4 the pieces, r5 their count): true = answered by us (skip the original), false = the original
// must run. Run 7155 showed the FIRST submit after the raw ring write is not the open segment: every kick 0x82CB6EA8 first submits a small "prefix"
// buffer built by 0x82CB61C0 (a coherency flush: COHER_STATUS_HOST 0xA31 := ..., COHER_SIZE / BASE, a WAIT_REG_MEM on 0xA31, or WAIT_UNTIL + two
// registers; 6..22 dwords, its own allocation) with 0x82CB6920 -> 0x82CB5EF0, and only then the segment (0x82CB6B20 -> 0x82CB6920). So the boot
// answer takes up to kMaxPrefix prefix pieces (read through the 0xA0000000 view of their physical address, scanned, skipped) and ends at the piece whose
// physical address is the open segment's start (device +15332 in GPU form), which is scanned and answered as before. One piece per submit.
inline bool AnswerBoot(PPCContext& ctx, uint8_t* base, uint32_t dev) {
  namespace ob = wwe2k14::owner_boot;
  const uint32_t pieces = ctx.r4.u32, count = ctx.r5.u32;
  const char* why = nullptr;
  bool prefix = false;
  ob::Answer a;
  auto describe = [](const ob::Census& c) {
    std::string s;
    for (uint32_t k = 0; k < ob::kKindCount; ++k) {
      if (c.n[k]) s += fmt::format("{}{} {}", s.empty() ? "" : ", ", ob::KindName(k), c.n[k]);
    }
    return s;
  };
  const uint32_t fence_block = dl::Ld32(base, dev + ob::kDevFenceBlock), fence_gpu = dn::GpuForm(fence_block);
  const uint32_t scratch_block = dl::Ld32(base, dev + dn::kDevScratch), scratch_gpu = dn::GpuForm(scratch_block);
  if (count != 1) {
    why = "more than one piece";
  } else {
    const uint32_t dwords = dl::Ld32(base, pieces) & 0xFFFFFFu, piece_gpu = dl::Ld32(base, pieces + 4), start = dl::Ld32(base, dev + dn::kDevPieceStart);
    std::vector<uint32_t> seg;
    const ob::Piece kind = ob::ClassifyPiece(piece_gpu, dn::GpuForm(start), dwords, g_t.boot_prefix_pieces, &why);
    if (kind == ob::Piece::Segment) {  // the open segment
      seg.resize(dwords);
      for (uint32_t i = 0; i < dwords; ++i) seg[i] = dl::Ld32(base, start + 4 * i);
      GuestIbReader reader{base};
      a = ob::AnswerBootSubmit(seg.data(), dwords, fence_block, fence_gpu, scratch_gpu, dl::Ld32(base, dev + ob::kDevRingWrite), &reader);
      if (a.result != ob::Result::Answered) why = "a packet the scan cannot vouch for";
    } else if (kind == ob::Piece::Prefix) {  // the guest address of a physical one is its 0xA0000000-view alias
      prefix = true;
      seg.resize(dwords);
      for (uint32_t i = 0; i < dwords; ++i) seg[i] = dl::Ld32(base, 0xA0000000u + piece_gpu + 4 * i);
      a.census = ob::Scan(seg.data(), dwords, fence_gpu);
      if (a.census.NeedsOriginal()) why = "a packet the scan cannot vouch for (prefix piece)";
    }
  }
  if (why) {
    REXLOG_WARN("[owner][boot] the boot submit goes to the SDK's processor: {} (first offender: dword {} at indirect depth {}, class {}, opcode {:#x}); packets {}: {}", why,
                a.census.first_bad_at, a.census.first_bad_depth, a.census.NeedsOriginal() ? ob::KindName(a.census.first_bad_kind) : "-", a.census.first_bad_opcode, a.census.packets,
                describe(a.census));
    g_t.c.boot_original.fetch_add(1, std::memory_order_relaxed);
    g_t.boot_prefix_pieces = 0;
    return false;
  }
  // The pieces' register writes never reach the SDK's register file when the processor does not run them. A later forced processor run (FellBack: tiling
  // wrapper / recording / predication -> the original interrupt / flip / fence emitters) writes SCRATCH_REG back only if SCRATCH_ADDR (0x1DD) and
  // SCRATCH_UMSK (0x1DC) are set (7228's segment sets them), so the type-0 / type-1 writes below 0x2000 are stored raw (the processor thread has run
  // no packet). Without a register file the pieces go to the original.
  auto* rt = REX_KERNEL_STATE() ? REX_KERNEL_STATE()->emulator() : nullptr;
  auto* gfx = rt ? rt->graphics_system() : nullptr;
  rex::graphics::RegisterFile* regs = gfx ? static_cast<rex::graphics::GraphicsSystem*>(gfx)->register_file() : nullptr;
  if (!regs) {
    REXLOG_WARN("[owner][boot] the boot submit goes to the SDK's processor: no SDK register file to keep the pieces' registers in");
    g_t.c.boot_original.fetch_add(1, std::memory_order_relaxed);
    g_t.boot_prefix_pieces = 0;
    return false;
  }
  for (const ob::Write& w : a.census.regs) regs->values[w.addr] = w.value;
  if (prefix) {
    ++g_t.boot_prefix_pieces;
    g_t.boot_pending.store(true, std::memory_order_release);  // the segment is still to come
    REXLOG_INFO("[owner][boot] boot prefix piece {} skipped (the SDK's processor gets nothing): {} packets ({}), {} registers kept", g_t.boot_prefix_pieces,
                a.census.packets, describe(a.census), a.census.regs.size());
    return true;
  }
  g_t.boot_prefix_pieces = 0;
  for (uint32_t i = 0; i < a.write_count; ++i) dl::St32(base, a.writes[i].addr, a.writes[i].value);
  std::atomic_thread_fence(std::memory_order_release);
  // The segment's scratch write-backs (device init resets the scratch block: 0x82CCACE8's SCRATCH_REG0-7 packet) are written by the timing thread, at their place
  // in the stream: behind the interrupt items the boot already queued (0x82CCAAF0's), which in the original ran before that packet, never in front of them.
  Start(base);
  g_t.boot_scratch.clear();
  for (const ob::Write& w : a.census.scratch) g_t.boot_scratch.emplace_back(scratch_block + w.addr, w.value);
  if (!g_t.boot_scratch.empty()) {
    Item it;
    it.boot_writes = true;
    Push(it);
  }
  REXLOG_INFO("[owner][boot] the boot submit answered by us (the SDK's processor gets nothing): segment {} packets ({}; {} indirect buffers followed), {} guest word written: {:08X} := {}, {} scratch words queued, {} registers kept in the SDK register file",
              a.census.packets, describe(a.census), a.census.n[ob::kIndirectFollowed], a.write_count, a.writes[0].addr, a.writes[0].value,
              g_t.boot_scratch.size(), a.census.regs.size());
  g_t.c.boot_answered.fetch_add(1, std::memory_order_relaxed);
  return true;
}

inline bool Submit(PPCContext& ctx, uint8_t* base) {
  if (!Active() || !REXCVAR_GET(wwe2k14_owner_no_ring)) return false;
  const uint32_t dev = ctx.r3.u32;
  if (g_t.ring_needed.exchange(false, std::memory_order_acq_rel) || Unusual(base, dev) || dl::Ld32(base, dev + dn::kDevDebugObject) ||
      dl::Ld32(base, dev + dn::kDevSubmitCallback)) {
    g_t.boot_pending.store(false, std::memory_order_release);  // (3a: an original run now carries the boot packets too)
    NeedsProcessor("a ring submit (a raw ring write other than the boot's, an unusual call, a debug object or a submit callback)");
    g_t.c.submits_run.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (g_t.boot_pending.exchange(false, std::memory_order_acq_rel) && !AnswerBoot(ctx, base, dev)) {  // 3a: the boot submit
    NeedsProcessor("the boot submit (the scan refused it, or --wwe2k14_owner_boot_submit_ours is off)");
    g_t.c.submits_run.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  g_t.c.submits_skipped.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace wwe2k14::owner_timing

// B6: the raw primary-ring writer 0x82CB5560 (advances the software write index without CP_RB_WPTR; callers: the boot set-up 0x82CB7228,
// the tiling worker's bin select 0x82CC94C8, never-seen others): its dwords are seen by the processor at the next WPTR write, so the next
// submit runs the original.
REX_HOOK_RAW(sub_82CB5560) {
  if (wwe2k14::owner_timing::Active()) {
    auto& t = wwe2k14::owner_timing::g_t;
    t.c.raw_ring_writes.fetch_add(1, std::memory_order_relaxed);
    // 3a (lane 231): the boot's own raw write (the call inside the init-idle 0x82CB7228, return address 0x82CB753C) leaves ring_needed alone; the next
    // submit is the boot submit and AnswerBoot decides it. Any other caller (the tiling worker's bin select ...) still wakes the processor.
    if (REXCVAR_GET(wwe2k14_owner_boot_submit_ours) && REXCVAR_GET(wwe2k14_owner_no_ring) && ctx.lr == wwe2k14::owner_boot::kBootRingWriteReturn) {
      t.c.boot_raw_writes.fetch_add(1, std::memory_order_relaxed);
      t.boot_pending.store(true, std::memory_order_release);
    } else {
      t.ring_needed.store(true, std::memory_order_release);
    }
  }
  __imp__sub_82CB5560(ctx, base);
}
