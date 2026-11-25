// wwe2k14 - Session 63 (Android port): watch the game's render-command replay for a runaway list (diagnosis).
//
// Phone runs 013 / 015 / 016: a played exhibition freezes on the same crowd shot at the end of Cena's entrance; the
// render thread (CRenderCmdManager, sub_82BB5CC0) loops in render_cmd_replay_lists (sub_82BB5BE8), which walks a list
// from its start index while the position is not EQUAL to the end index, advancing by each record's length field + 1
// (generated code of sub_82BB5BE8: manager+44 = position, descriptors at [manager+108] stride 68 with start +16 /
// end +12, count at manager+132; records are 16-byte slots, opcode word +0, length word +8; 105 ends the replay, 104
// the list). One wrong length steps past the end and the walk never stops. This keeps the last 64 records dispatched
// (sub_82BB53D0) and, the first time one replay passes kRunaway dispatches, logs them with the lists' bounds: which
// record (opcode, length, position) carried the walk past its end. Android only (the PC never runs away); log once.
//
// Session 73, b78 (the freeze again in Jarrod's b77 run, report 118, ~12 s into Cena's entrance; the 3,000,000th
// dispatch was far too late to show the cause: the walk had gone 368 million slots, through zeroed memory): a GUARD.
// The listing of sub_82BB5BE8 (tools\analysis\list_function.py 0x82BB5BE8) checks the end index only before a list's
// first record (0x82BB5C24); after that only a 104 / 105 record stops the walk. Per record the loop:
//   record = [manager + 4 * ([manager+36] + 12)] + 16 * [manager+44]  (stored in the stack slot r4 points at)
//   opcode 105 -> return; [record] == 104 -> next list; else sub_82BB53D0(manager, &slot);
//   [manager+44] += [[slot] + 8] + 1                                   (0x82BB5C88-0x82BB5C9C)
// After each dispatch (the hook below) the guard works out that next position. When it would go past the end of the
// list being walked (end + 1 or more; a list whose start is above its end, a ring, is left alone), it logs the last 64
// records with the lists' bounds and the buffer index at the walk's start and now, then ends the list: a 16-byte
// record of ours with opcode 104 (guest heap) and [manager+44] set so the loop's own step lands on it (the record
// address wraps at 32 bits as the code's rlwinm / add do). The frame loses the rest of that list instead of the game
// freezing. Second guard: over kRunaway dispatches in one replay, the same with opcode 105 (the replay ends).
// --wwe2k14_replay_guard (default on Android, off on the PC); --wwe2k14_replay_guard_test_frame=N makes the guard
// trip on purpose at guest frame N (a test of the ending; the PC never runs away by itself).
//
// Session 77 (XDET_PLAN.md gap 23): passing a list's end is NOT a runaway. At the start of a tag or Elimination
// Chamber match the walk passes list 0's end (position 1613, length 2, end 1615) on the PC exactly as on the phone (PC
// 4941 with the guard on = phone 148, same frame, record and position; normal phone play 152 too): the buffer the walk
// indexes ([manager+48] = 0xAF0AA530 during the walk, moving between walks) is not the one the descriptors describe
// (0xAE850000 / 0xAEC80000 at VdSwap), and the walk ends on a 104 further on by itself. Ending the list there made the
// game crash right after (a read of guest 0 on the render thread) on both devices; with the guard off the PC (4934,
// 4940) and the phone (151) play the whole match, equal in every hashed layer. Now the guard ends a list only when
// the next position leaves the record buffer (capacity [manager+28] slots: 269,760 here; the Session 73 freeze walked
// 368 million) or wraps, plus the runaway count as before; a list's end crossed inside the buffer is logged (the
// first kMaxEndReports, report only, no memory change).

#pragma once

#include "renderer_adapter/census_flush.h"  // S102 CEN1
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>

#include "cost_counters.h"  // S101 lane 83
#include "nf/replay_native.h"  // S102 W3: the native replay loop and dispatcher (default off)
#include "frame_stats.h"  // g_guest_frame
#include "renderer_adapter/tail_release.h"  // S102 U1: a tail still pending finishes before the walk

#if defined(__ANDROID__)
REXCVAR_DECLARE(bool, wwe2k14_replay_guard);
#else
REXCVAR_DECLARE(bool, wwe2k14_replay_guard);
#endif
REXCVAR_DECLARE(int32_t, wwe2k14_replay_guard_test_frame);

#include "replay_guard.h"  // S102 W3 (review F5): the ring, the guard and the per-record hook body, moved out unchanged in behaviour

namespace wwe2k14::replay_watch {

inline uint32_t FakeRecords(uint8_t* base) {
  uint32_t a = g_fake_records.load(std::memory_order_acquire);
  if (a) return a;
  const uint32_t made = REX_KERNEL_MEMORY()->SystemHeapAlloc(48);
  if (!made) return 0;
  const uint32_t aligned = (made + 15) & ~15u;
  for (uint32_t i = 0; i < 32; i += 4) Put32(base, aligned + i, 0);
  Put32(base, aligned, 104);       // ends the list
  Put32(base, aligned + 16, 105);  // ends the replay
  uint32_t expected = 0;
  if (!g_fake_records.compare_exchange_strong(expected, aligned)) return expected;  // (another thread won; ours leaks)
  return aligned;
}

}  // namespace wwe2k14::replay_watch

// S102 W3 (docs/research/part2/W3_native_replay.md): native versions of the game's replay loop sub_82BB5BE8 and interpreter sub_82BB53D0
// (src/nf/replay_native.h; proved against the recompiled functions by tools/w3/diff). Both PerFrame, default off (the table is
// src/renderer_adapter/rdr_switches.h): the sub_82BB5BE8 hook reads them ONCE per replay through rdr_sw::BoolSeen with the guest frame and latches the
// dispatcher's value in a thread_local for that replay; the per-record hook never reads a cvar for them (tools/w3/hook_lint.py).
REXCVAR_DECLARE(bool, wwe2k14_nf_replay_loop);
REXCVAR_DECLARE(bool, wwe2k14_nf_render_dispatch);
namespace wwe2k14::replay_watch {
inline thread_local bool t_native_dispatch = false;  // this replay's dispatcher switch (latched by the sub_82BB5BE8 hook)
// Every 600 replays while a native switch is on: what the native code took (the census line the Surface reads to tell "no gain" from "did nothing").
inline void CensusTick(uint64_t guest_frame) {
  auto& c = wwe2k14::nf_replay::g_census;
  ++c.frames;
  static census_flush::Site cf_site;   // S102 CEN1: a flip phase end prints the nf_replay census now
  if (c.frames % 600 != 0 && !census_flush::Due(cf_site)) return;
  const uint64_t taken = c.native_calls + c.exits, declined = c.fallbacks + c.hooked_op81 + c.hooked_bookkeeping + c.hooked_dispatch_off;
  REXLOG_INFO("[rdr][nf_replay] frames {}, taken {}, declined {} (by reason: recompiled interpreter for ops 9/10/12/42/43/46/49/52-58/72/79/81/82/103 {}, "
              "op 81 through the hook {}, guard / cost counters / Android watch on {}, dispatcher switch off {}); guest frame {}, loop {}, dispatcher {}",
              c.frames, taken, declined, c.fallbacks, c.hooked_op81, c.hooked_bookkeeping, c.hooked_dispatch_off, guest_frame, int(c.loop_on), int(c.dispatch_on));
}
}  // namespace wwe2k14::replay_watch

// render_cmd_replay_lists: one call per frame; the count starts again and the lists' bounds are kept.
REX_HOOK_RAW(sub_82BB5BE8) {
  rdr_adapter::tail::The().FlushIfPending(rdr_adapter::tail::kAtWalk);  // S102 U1 (safety net; the release hook normally finished it): one relaxed load
  namespace rw = wwe2k14::replay_watch;
  wwe2k14::frame_split::WalkScope split_walk;  // S102 H3: the walk (the native loop or the recompiled one) timed for the swap-thread split; one relaxed load when off
  const uint64_t guest_frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  // S102 W3 (review F1): the switches are read here, once per replay, as the table's PerFrame entries (a mid-run flip is logged "[switch] frame N" and
  // "[switch] seen frame N"); the raw cvar is only the answer before the table's first refresh.
  const bool loop_on = rdr_sw::BoolSeen(rdr_sw::kNfReplayLoop, REXCVAR_GET(wwe2k14_nf_replay_loop), guest_frame);
  const bool dispatch_on = rdr_sw::BoolSeen(rdr_sw::kNfRenderDispatch, REXCVAR_GET(wwe2k14_nf_render_dispatch), guest_frame);
  rw::t_native_dispatch = dispatch_on;
  rw::BeginReplay(base, ctx.r3.u32, REXCVAR_GET(wwe2k14_replay_guard));
  if (loop_on) {
    // `fast` = no per-record bookkeeping is on (the replay guard, the cost counters, the Android watch) and the dispatcher switch is on, so a record
    // other than op 81 goes straight to the native dispatcher.
#if defined(__ANDROID__)
    constexpr bool kFast = false;
#else
    const bool kFast = dispatch_on && !REXCVAR_GET(wwe2k14_replay_guard) && !wwe2k14::cost::On();
#endif
    wwe2k14::nf_replay::g_census.loop_on = loop_on, wwe2k14::nf_replay::g_census.dispatch_on = dispatch_on;
    wwe2k14::nf_replay::ReplayLists(ctx, base, kFast, dispatch_on);
    rw::CensusTick(guest_frame);
    return;
  }
  __imp__sub_82BB5BE8(ctx, base);
  if (dispatch_on) {  // the dispatcher switch alone also reports (the recompiled loop calls the hook, which counts what Dispatch took)
    wwe2k14::nf_replay::g_census.loop_on = loop_on, wwe2k14::nf_replay::g_census.dispatch_on = dispatch_on;
    rw::CensusTick(guest_frame);
  }
}

// render_cmd_dispatch: r3 = the manager, r4 = the stack slot holding the record's address.
// Session 100 (WM30 crash, runs 6861-6892): a record with opcode 81 calls a model's cached record list (the address at rec+16, kept in the
// model: holder at object +864 / +868, filled by 0x82BB4AE0; recorder 0x82C24500). The update thread can release that model (0x82BD91B0)
// while the render thread replays last frame's lists, which still call the list: the call then lands in freed, reused memory (the
// crash: a "record" whose length is a code address). Before an op 81 runs, its target is checked: up to kCallCheckRecords records must have
// opcodes the replay knows (<= 105) and lengths that fit (< 4096 slots) until the list's own return (82). A target that fails is not
// called: the replay loop's own step moves past the 81 (the model misses that frame). --wwe2k14_replay_call_check (default on).
REXCVAR_DECLARE(bool, wwe2k14_replay_call_check);

REX_HOOK_RAW(sub_82BB53D0) {
  namespace rw = wwe2k14::replay_watch;
#if defined(__ANDROID__)
  constexpr bool kWatch = true;
#else
  const bool kWatch = REXCVAR_GET(wwe2k14_replay_guard);
#endif
  rw::HookedDispatch(ctx, base, REXCVAR_GET(wwe2k14_replay_call_check), kWatch, REXCVAR_GET(wwe2k14_replay_guard), [&] {
    // S101 lane 83: replay cost per record by opcode (outermost dispatch only; the record's opcode is read only when the counters are on).
    wwe2k14::cost::ReplaySpan cost_span(wwe2k14::cost::On() ? rw::Be32(base, rw::Be32(base, ctx.r4.u32)) : 0u);
    if (rw::t_native_dispatch) {
      wwe2k14::nf_replay::Dispatch(ctx, base);  // S102 W3: the native interpreter (latched for this replay); its Fall ops run the recompiled one
    } else {
      __imp__sub_82BB53D0(ctx, base);
    }
  });
}
