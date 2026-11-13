#pragma once
// wwe2k14 - Session 101 lane 260 (docs\renderer\LEAVE_THE_COMMAND_STREAM.md 3e, section 10): the state machine of the "Owner timing" thread, with no game or SDK
// header in it, so a host test (tools\owner\timing_core_test.cpp) runs the shipped code against a model of the guest. owner_timing.h supplies the real Env
// (guest memory, the game's interrupt dispatcher 0x82CB5758, the clock); behaviour with the new switches off is exactly what owner_timing.h did before this file.
//
//   items       a fence (heap marker then fence number), or an interrupt (the game's dispatcher run for each targeted cpu), or the flip handshake's interrupt
//   the gate    after a flip is due (flip word, scratch +4, set) the fences that follow are held until the word reads 0, so the next present's reason-3 wait
//               (the frame limiter) passes at the due vblank, never at once (Session 51)
//   the vblank  --wwe2k14_owner_vblank_ours: the same thread dispatches the guest's vblank interrupt (source 0, cpu 2) from its own clock, as
//               GraphicsSystem::MarkVblank did on the SDK's VSync thread. The game's vblank handler (0x82CB28F8) clears the flip word; a flip that waits for the
//               previous flip's word to clear (WaitWord) therefore TICKS the vblanks that fall due while it waits: nobody else would.
#include <cstdint>
#include <vector>

namespace wwe2k14::owner_core {

struct Item {
  bool fence = false;  // else an interrupt
  bool flip = false;   // the flip handshake's interrupt
  uint32_t a = 0, b = 0, c = 0, d = 0, e = 0;  // fence: marker at, marker, value at, value; interrupt: flags, callback, argument, scratch, options
  uint32_t dev = 0;
  bool boot_writes = false;  // 3a: apply the boot segment's scratch write-backs at this place in the stream
};

enum class Count { Fence, Gated, Released, Interrupt, Flip, DispatchWait, Vblank };

// The guest scene-timing pair (ADAPTER_CALLS.md section 5; listings: 0x82C16978 start, 0x82C16980 end).
inline constexpr uint32_t kSceneTimingBegin = 0x82C16978u, kSceneTimingEnd = 0x82C16980u;

// The ticks that make the scene-timing readers see `ms` milliseconds: they compute ticks * scale * 1000 (scale = the guest float, seconds per tick). 0 = leave the
// measured value (ms or scale not positive).
inline uint64_t SceneTicksForMs(double ms, float scale) {
  if (!(ms > 0.0) || !(scale > 0.0f)) return 0;
  if (ms > 1000.0) ms = 1000.0;  // (a huge value would overflow the int64 conversion: UB) one second is far past any budget
  const double ticks = ms / (double(scale) * 1000.0);
  return ticks >= 9.0e18 ? uint64_t(9.0e18) : uint64_t(ticks);
}

// ---- the vblank clock (pure) ----
// The grid the SDK's VSync loop kept: the next deadline is the last one + interval; behind by several intervals = several vblanks (catch-up). The SDK fires
// every owed vblank; after a stall of minutes (a suspend, a debugger) that is thousands in a row, so here at most kMaxCatchUp are fired and the grid restarts from now.
struct VblankClock {
  static constexpr uint32_t kMaxCatchUp = 8;
  int64_t interval_ns = 16666667;
  int64_t next_ns = 0;
  bool started = false;
  uint32_t dropped = 0;  // grid restarts (a stall longer than kMaxCatchUp intervals)

  // A changed interval (a rate cvar flipped, vsync on/off) re-grids from now: the old grid's next deadline would otherwise fire at the old spacing once.
  void SetInterval(int64_t ns, int64_t now_ns = 0) {
    ns = ns < 1 ? 1 : ns;
    if (started && ns != interval_ns) next_ns = now_ns + ns;
    interval_ns = ns;
  }
  // Vblanks owed at now_ns (monotonic ns); advances the grid.
  uint32_t Due(int64_t now_ns) {
    if (!started) {
      started = true;
      next_ns = now_ns + interval_ns;
      return 0;
    }
    uint32_t n = 0;
    while (now_ns >= next_ns) {
      ++n;
      next_ns += interval_ns;
      if (n >= kMaxCatchUp) {
        if (now_ns >= next_ns) {
          next_ns = now_ns + interval_ns;
          ++dropped;
        }
        break;
      }
    }
    return n;
  }
  int64_t NextDeadline() const { return started ? next_ns : 0; }
};

// The vblank interval the SDK loop chose (graphics_system.cpp 633-647): vsync on = the video mode's rate unless gpu_vblank_rate_override_hz > 0; vsync off = 1 kHz
// unless gpu_vblank_unthrottled_hz > 0. Returns ns.
inline int64_t VblankIntervalNs(bool vsync, double video_hz, double override_hz, double unthrottled_hz) {
  double hz = vsync ? (override_hz > 0.0 ? override_hz : video_hz) : (unthrottled_hz > 0.0 ? unthrottled_hz : 1000.0);
  if (hz < 1.0) hz = 1.0;
  return int64_t(1.0e9 / hz);
}

// ---- the state machine ----
// Env (the caller's): uint32_t Ld32(addr); void St32(addr, v); void Release() (a release fence); void FullFence() (seq_cst); void RunGuestInterrupt(source, cpu, device); uint32_t Device() (the vblank's
// registered data); void Count(Count); uint32_t VblankOwed() (vblanks due now, 0 when the vblank is not ours); void WaitBegin(at, value); bool Sleep() (one idle step;
// false = give up the wait: only a test ends a wait this way); void OnInterruptDone(const Item&).
template <class Env>
struct Core {
  bool gate_open = false;
  uint32_t flip_at = 0;
  std::vector<Item> gated;

  void WriteFence(Env& e, const Item& it) {
    e.St32(it.a, it.b);  // the heap marker first, then the fence number (the packet pair's order)
    e.Release();
    e.St32(it.c, it.d);
    e.Release();
    e.Count(Count::Fence);
  }

  // The gate: open while the last flip's word is set; fences behind it are written once the game cleared it.
  void TryRelease(Env& e) {
    if (!gate_open || e.Ld32(flip_at) != 0) return;
    for (const Item& it : gated) WriteFence(e, it);
    for (size_t i = 0; i < gated.size(); ++i) e.Count(Count::Released);
    gated.clear();
    gate_open = false;
  }

  // One vblank: the game's interrupt dispatcher with source 0 on cpu 2 (its vblank handler clears the flip word), then the gate re-reads the word.
  void Vblank(Env& e) {
    e.Count(Count::Vblank);
    e.RunGuestInterrupt(0, 2, e.Device());
    TryRelease(e);
  }

  // Waits until the guest word reads `value`; the guest code that changes it runs on other threads, or in the vblank this wait ticks itself. False = given up.
  bool WaitWord(Env& e, uint32_t at, uint32_t value) {
    e.WaitBegin(at, value);
    while (e.Ld32(at) != value) {
      if (const uint32_t n = e.VblankOwed()) {
        for (uint32_t i = 0; i < n; ++i) Vblank(e);
      } else if (!e.Sleep()) {
        return false;
      }
    }
    return true;
  }

  void RunInterrupt(Env& e, const Item& it) {
    if (it.flip && gate_open) {  // the previous flip still pending: as the command thread, this one waits for it
      WaitWord(e, flip_at, 0);
      TryRelease(e);
    }
    const uint32_t scratch = it.d;
    uint32_t cpu_mask = (it.a >> 24) & 0x3F;
    if (!cpu_mask) cpu_mask = 4;
    uint32_t flags = it.a;
    if (!(flags & 6)) flags |= 6;  // (the emitter's default: callback registers + interrupt + the wait for the CPU)
    if (it.flip && (it.e & 1)) e.St32(scratch + 4, 1);  // the flip word := 1
    if (flags & 2) {
      e.St32(scratch + 16, it.b);
      if (!(flags & 0x80)) e.St32(scratch + 20, it.c);
      e.St32(scratch + 0, cpu_mask);
      e.FullFence();  // (the original's seq_cst fence: the mask must be visible before any cpu's dispatcher clears its bit)
      for (uint32_t n = 0; n < 6; ++n) {
        if (cpu_mask & (1u << n)) e.RunGuestInterrupt(1, n, it.dev);
      }
    }
    if (flags & 4) {  // every targeted CPU has run it: the dispatcher cleared its bit of scratch +0
      if (e.Ld32(scratch + 0) != 0) {
        e.Count(Count::DispatchWait);
        WaitWord(e, scratch + 0, 0);
      }
      e.St32(scratch + 16, 0x0BADF00Du);
    }
    if (it.flip && (it.e & 2) && e.Ld32(scratch + 4) != 0) {  // the flip is due later: the gate opens
      gate_open = true;
      flip_at = scratch + 4;
    }
    e.OnInterruptDone(it);
    e.Count(it.flip ? Count::Flip : Count::Interrupt);
  }

  // One queue item, in stream order (the caller applies the boot's scratch write-backs for `boot_writes` itself).
  void Process(Env& e, const Item& it) {
    if (it.fence) {
      if (gate_open) {
        gated.push_back(it);
        e.Count(Count::Gated);
      } else {
        WriteFence(e, it);
      }
    } else {
      RunInterrupt(e, it);
    }
  }

  // One turn of the thread's loop after an item arrived or the wait timed out: the vblanks that fell due, then the gate.
  void Turn(Env& e) {
    if (const uint32_t n = e.VblankOwed()) {
      for (uint32_t i = 0; i < n; ++i) Vblank(e);
    }
    TryRelease(e);
  }
};

}  // namespace wwe2k14::owner_core
