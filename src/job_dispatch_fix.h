#pragma once
// wwe2k14 - Session 71: the entrance freeze on PCs with few CPU threads (a v0.2.9 tester: Intel HD 520, i3-6100U, 2
// cores / 4 threads; a six-man tag (match type 11) froze for good about a second into the first entrance; "turning off
// entrances" avoided it).
//
// Reproduced here (runs s71_i3 003 and 006, the v0.2.9 release exe, the game held to 4 logical processors): 2 of 2
// froze at the same moment; with all 32 threads, pinned or not, the entrance played (runs 004, 005). The flight
// recorder's hang dump (flight_006.txt) shows the chain: the render thread (sub_82BB5CC0) waits in its frame barrier
// (sub_82C3BAE0) for the update thread; the update thread (task runner sub_82C302A0 -> sub_824F6350) waits forever in
// sub_8251D3F0 on a job's "done" event; the worker that ran the job had already set it.
//
// The game's job dispatcher sub_8251D1F0 (object = [0x83763038]):
//     if (obj && obj->go_event (+104)) { obj->pending (+100) = 1; SetEvent(go_event); ResetEvent(done_event (+108)); }
// resets "done" AFTER waking the worker. On the Xbox 360 the dispatcher keeps its hardware thread and the reset follows
// within a microsecond, long before the worker finishes. On a PC with few cores, SetEvent hands the core to the woken
// worker (that call took 0.097 ms in the dump), the worker runs the whole job and sets "done", and the dispatcher's late
// reset then erases it: sub_8251D3F0 waits on "done" with no timeout and the game stops (the dump's order: worker set
// 0xF8000640 at -10.735220 s, dispatcher reset it at -10.735208 s, then waited on it).
//
// This replacement does the same three things with the reset first (reset "done", mark pending, wake the worker), so
// "done" can only be set after it was cleared. --wwe2k14_job_reset_first=false runs the game's own order.
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(wwe2k14_job_reset_first, true, "wwe2k14",
                    "Session 71: the job dispatcher (sub_8251D1F0) clears the job's done event before waking its "
                    "worker, not after (the entrance freeze on 2-core / 4-thread CPUs; src/job_dispatch_fix.h)");

REX_EXTERN(__imp__sub_8251D1F0);
REX_EXTERN(sub_82262260);  // SetEvent(handle) wrapper
REX_EXTERN(sub_82C8F0F0);  // ResetEvent(handle) wrapper

namespace wwe2k14::job_dispatch_fix {
inline uint32_t Be32(uint8_t* base, uint32_t a) {
  const uint8_t* p = rex::memory::GuestPtr(base, a);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void StoreBe32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = rex::memory::GuestPtr(base, a);
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
}  // namespace wwe2k14::job_dispatch_fix

REX_HOOK_RAW(sub_8251D1F0) {
  using namespace wwe2k14::job_dispatch_fix;
  if (!REXCVAR_GET(wwe2k14_job_reset_first)) {
    __imp__sub_8251D1F0(ctx, base);
    return;
  }
  const uint32_t obj = Be32(base, 0x83763038u);
  if (!obj) return;
  const uint32_t go = Be32(base, obj + 104);
  if (!go) return;
  ctx.r3.u64 = Be32(base, obj + 108);  // the job's done event: cleared first
  sub_82C8F0F0(ctx, base);
  StoreBe32(base, obj + 100, 1);  // pending (sub_8251D3F0 waits only while it is set)
  ctx.r3.u64 = go;                // then wake the worker
  sub_82262260(ctx, base);
}
