// wwe2k14 - Session 66 (native function factory, track F2): native version of sub_82CB1F50, the D3D library's GPU wait
// poll step (db gpu_wait_poll_step). Every CPU-side wait on the GPU is "begin, poll until this returns 0, end".
//
// What the guest function does (tools\analysis\list_function.py 0x82CB1F50, tools\q callees): r3 = a wait record whose
// first word is the device pointer. A short delay (the db16cyc / cctpl hints compile to nothing), then:
//   - if the device's hung flag (byte device+11069, mask 2) is set: return 0 (stop waiting);
//   - otherwise read the GPU-written fence word through the pointer at device+11024; when it differs from the record's
//     last-seen fence (record+8), store the current thread's clock (KTHREAD+0x58, via r13+256) at record+12 and the fence
//     (read again) at record+8;
//   - when the current thread id (sub_82261F28: KTHREAD+0x14C) equals device+11016 and device+11148 is non-zero, store the
//     clock at record+12 again;
//   - elapsed = clock - record+12; when elapsed is below the global threshold word at 0x8362F5B4 return 1 (keep waiting).
//     At or above it the original calls sub_82CBFED0 (the hang watchdog: DbgPrint, marks the device hung; not a leaf).
//     The db records that this cannot happen here (the clock field is never written, so elapsed stays 0) unless the
//     threshold word is 0.
//
// Exactness points mirrored: the same loads in the same order (the fence word is read twice, the second through a
// volatile read since the GPU writes it), the same two possible stores (record+12, record+8), the same return values.
// The watchdog case is decided from the values BEFORE any store and then goes to the recompiled original; counted and
// logged. In verify mode the native version does nothing for such a call (declares no range, r3 left alone: a register
// mismatch may then show) and only the original runs (it would otherwise run twice).
//
// Registers on return: r3 = 0 or 1 (kR3). Non-volatile registers untouched.
//
// Speed note: this is a spin step; the render thread time it takes is time spent waiting for the GPU, so a faster poll
// step frees no work (it spins faster). The gain is only less CPU per poll.
//
// --wwe2k14_nf_gpu_wait_poll_step = off | verify | on (default off), or the global --wwe2k14_nf_all.

#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "../native_fn.h"

REXCVAR_DEFINE_STRING(wwe2k14_nf_gpu_wait_poll_step, "off", "wwe2k14",
                      "Session 66: native version of sub_82CB1F50 (GPU wait poll step): off | verify | on");

REX_EXTERN(__imp__sub_82CB1F50);

namespace wwe2k14::nf_gpu_wait_poll_step {

inline std::atomic<uint64_t> g_deferred{0};
constexpr uint32_t kThresholdWord = 0x8362F5B4;

inline uint32_t Ld32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, native_fn::Host(base, a), 4);
  return __builtin_bswap32(v);
}
inline uint32_t Ld32Volatile(uint8_t* base, uint32_t a) {
  return __builtin_bswap32(*reinterpret_cast<const volatile uint32_t*>(native_fn::Host(base, a)));
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(native_fn::Host(base, a), &v, 4);
}

enum class Outcome { kStop, kKeepWaiting, kWatchdog };

// Decides the result from the current state without storing anything; *fence_changed / *touch_again say which stores
// the original would make before it returns.
inline Outcome Decide(const PPCContext& ctx, uint8_t* base, uint32_t* stamp_out, bool* fence_changed, bool* touch_again) {
  const uint32_t rec = ctx.r3.u32;
  const uint32_t dev = Ld32(base, rec);
  *fence_changed = *touch_again = false;
  if (Ld32(base, dev + 11069) & 2u) return Outcome::kStop;
  const uint32_t fence_ptr = Ld32(base, dev + 11024);
  const uint32_t kthread = Ld32(base, ctx.r13.u32 + 256);
  const uint32_t seen = Ld32(base, rec + 8);
  const uint32_t fence = Ld32Volatile(base, fence_ptr);
  const uint32_t stamp = Ld32(base, kthread + 88);
  *stamp_out = stamp;
  uint32_t last = Ld32(base, rec + 12);
  if (seen != fence) {
    *fence_changed = true;
    last = stamp;
  }
  const uint32_t tid = Ld32(base, Ld32(base, ctx.r13.u32 + 256) + 332);  // sub_82261F28
  if (Ld32(base, dev + 11016) == tid && Ld32(base, dev + 11148) != 0) {
    *touch_again = true;
    last = stamp;
  }
  const uint32_t elapsed = stamp - last;
  return elapsed < Ld32(base, kThresholdWord) ? Outcome::kKeepWaiting : Outcome::kWatchdog;
}

inline native_fn::Mode ModeNow() { return native_fn::ModeFromCvar(REXCVAR_GET(wwe2k14_nf_gpu_wait_poll_step)); }

[[gnu::noinline, gnu::cold]] inline void Defer(PPCContext& ctx, uint8_t* base) {
  const uint64_t k = g_deferred.fetch_add(1, std::memory_order_relaxed);
  if (k < 4 || (k & 0xFFFF) == 0) {
    REXLOG_INFO("[wwe2k14][nf] gpu_wait_poll_step: the hang watchdog branch was reached ({} so far), record {:08X}", k + 1,
                ctx.r3.u32);
  }
  if (ModeNow() == native_fn::Mode::kVerify) return;  // would run twice: not verifiable, the original runs on its own
  __imp__sub_82CB1F50(ctx, base);
}

inline void Run(PPCContext& ctx, uint8_t* base) {
  uint32_t stamp = 0;
  bool fence_changed, touch_again;
  const Outcome o = Decide(ctx, base, &stamp, &fence_changed, &touch_again);
  if (o == Outcome::kStop) {
    ctx.r3.u64 = 0;
    return;
  }
  if (__builtin_expect(o == Outcome::kWatchdog, 0)) return Defer(ctx, base);
  const uint32_t rec = ctx.r3.u32;
  if (fence_changed) {
    St32(base, rec + 12, stamp);
    St32(base, rec + 8, Ld32Volatile(base, Ld32(base, Ld32(base, rec) + 11024)));  // the fence, read again
  }
  if (touch_again) St32(base, rec + 12, stamp);
  ctx.r3.u64 = 1;
}

struct Spec {
  static constexpr const char* kName = "gpu_wait_poll_step";
  static constexpr uint32_t kGuest = 0x82CB1F50;
  static constexpr uint32_t kRegs = native_fn::kR3;
  static native_fn::Mode GetMode() { return ModeNow(); }
  static void Writes(const PPCContext& ctx, uint8_t* base, native_fn::Ranges& out) {
    uint32_t stamp;
    bool a, b;
    if (Decide(ctx, base, &stamp, &a, &b) == Outcome::kWatchdog) return;  // deferred: nothing is compared
    out.Add(ctx.r3.u32 + 8, 8);                                           // record+8 (fence) and record+12 (stamp)
  }
  static void Native(PPCContext& ctx, uint8_t* base) { Run(ctx, base); }
};

}  // namespace wwe2k14::nf_gpu_wait_poll_step

WWE2K14_NATIVE_FN(sub_82CB1F50, wwe2k14::nf_gpu_wait_poll_step::Spec)
