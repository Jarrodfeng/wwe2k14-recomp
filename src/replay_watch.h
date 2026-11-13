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

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>

#include "cost_counters.h"  // S101 lane 83
#include "frame_stats.h"  // g_guest_frame

#if defined(__ANDROID__)
REXCVAR_DEFINE_BOOL(wwe2k14_replay_guard, true, "wwe2k14",
                    "Session 73: end a render-command list the game's replay walks past its end (src/replay_watch.h)");
#else
REXCVAR_DEFINE_BOOL(wwe2k14_replay_guard, false, "wwe2k14",
                    "Session 73: end a render-command list the game's replay walks past its end (src/replay_watch.h)");
#endif
REXCVAR_DEFINE_INT32(wwe2k14_replay_guard_test_frame, 0, "wwe2k14",
                     "Session 73: test - the replay guard ends list 0 on purpose at this guest frame (0 = off)");

namespace wwe2k14::replay_watch {

inline constexpr uint64_t kRunaway = 3000000;  // a heavy entrance frame dispatches well under 100,000 records
inline constexpr uint32_t kMaxLists = 16;
inline constexpr uint32_t kMaxReports = 20;
inline constexpr uint32_t kMaxEndReports = 3;  // Session 77: report-only end crossings

struct Entry {
  uint32_t record, opcode, length, position;
};
inline thread_local uint64_t t_dispatches = 0;
inline thread_local Entry t_ring[64];
inline thread_local uint32_t t_list_count = 0, t_start[kMaxLists], t_end[kMaxLists], t_buffer_index = 0;
inline thread_local uint64_t t_test_frame_done = 0;
inline std::atomic<uint32_t> g_reports{0};
inline std::atomic<uint32_t> g_end_reports{0};
inline std::atomic<uint32_t> g_fake_records{0};  // guest address of two 16-byte records: opcode 104, then 105

inline uint32_t Be32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void Put32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

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

inline void LogRing(const uint8_t* base, uint32_t manager, const char* why) {
  const uint32_t lists = Be32(base, manager + 132), descs = Be32(base, manager + 108);
  REXLOG_WARN("[wwe2k14][replay_watch] {} (frame {}): manager 0x{:08X}, position {}, buffer index {} (at the walk's "
              "start {}), {} lists (descriptors at 0x{:08X})",
              why, g_guest_frame.load(std::memory_order_relaxed), manager, Be32(base, manager + 44),
              Be32(base, manager + 36), t_buffer_index, lists, descs);
  // Session 77 (gap 23): the buffer pointers the walk indexes ([manager + 4 * (index + 12)]) and the recorders'
  // descriptor array (+104), as they are now (a VdSwap hash showed other values than the walk's record addresses).
  REXLOG_WARN("[wwe2k14][replay_watch]   buffers +48..+60: 0x{:08X} 0x{:08X} 0x{:08X} 0x{:08X}; recorders' "
              "descriptors +104 0x{:08X}; capacity +28 {}",
              Be32(base, manager + 48), Be32(base, manager + 52), Be32(base, manager + 56), Be32(base, manager + 60),
              Be32(base, manager + 104), Be32(base, manager + 28));
  for (uint32_t i = 0; i < lists && i < kMaxLists; ++i) {
    const uint32_t d = descs + i * 68;
    REXLOG_WARN("[wwe2k14][replay_watch]   list {}: start {} end {} (at the walk's start: {} / {})", i,
                Be32(base, d + 16), Be32(base, d + 12), i < t_list_count ? t_start[i] : 0,
                i < t_list_count ? t_end[i] : 0);
  }
  const uint64_t n = t_dispatches;
  for (uint64_t k = (n > 64 ? n - 64 : 0); k < n; ++k) {
    const Entry& e = t_ring[k % 64];
    REXLOG_WARN("[wwe2k14][replay_watch]   #{}: record 0x{:08X} opcode {} length {} position {}", k, e.record,
                e.opcode, e.length, e.position);
  }
}

// After a dispatch: the loop's next step; ends the list (104) when it leaves the record buffer, or the replay (105)
// when it runs away. A list's end passed inside the buffer is only logged (Session 77, gap 23).
inline void Guard(uint8_t* base, uint32_t manager, uint32_t slot) {
  const uint32_t pos = Be32(base, manager + 44);
  const uint32_t record = Be32(base, slot);
  const uint32_t len = Be32(base, record + 8);
  const uint32_t next = pos + len + 1;
  const bool runaway = t_dispatches >= kRunaway;
  // The record buffer holds [manager+28] slots (269,760 in a tag match); 0 = unknown, then only a wrap counts.
  const uint32_t capacity = Be32(base, manager + 28);
  const bool outside = next < pos || (capacity != 0 && next >= capacity);  // (next < pos: wrapped past 2^32)
  // The list being walked, as the lists are now (a list still growing while it is walked is not cut short).
  const uint32_t descs = Be32(base, manager + 108);
  uint32_t count = Be32(base, manager + 132);
  count = count < kMaxLists ? count : kMaxLists;
  uint32_t list = kMaxLists, end = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t s = Be32(base, descs + i * 68 + 16), e = Be32(base, descs + i * 68 + 12);
    if (list == kMaxLists && s < e && pos >= s && pos <= e) list = i, end = e;
  }
  const int32_t test_frame = REXCVAR_GET(wwe2k14_replay_guard_test_frame);
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  const bool test = test_frame > 0 && frame >= uint64_t(test_frame) && t_test_frame_done == 0 && list == 0;
  if (!outside && !runaway && !test) {
    if (list < kMaxLists && next > end && g_end_reports.fetch_add(1, std::memory_order_relaxed) < kMaxEndReports) {
      LogRing(base, manager, "a render-command list's walk passed its end inside the buffer (report only)");
      REXLOG_WARN("[wwe2k14][replay_watch] last record 0x{:08X} length {}: next position {} (list {} ends at {}, "
                  "buffer {} slots); left alone",
                  record, len, next, list, end, capacity);
    }
    return;
  }
  if (test) t_test_frame_done = frame;
  const uint32_t fake = FakeRecords(base);
  const uint32_t index = Be32(base, manager + 36);
  const uint32_t buffer = Be32(base, manager + 4 * (index + 12));
  const uint32_t target = runaway ? fake + 16 : fake;
  const bool can_end = fake && ((target - buffer) & 15) == 0;
  if (g_reports.fetch_add(1, std::memory_order_relaxed) < kMaxReports) {
    LogRing(base, manager,
            test ? "TEST: the replay guard ends list 0 on purpose"
            : runaway ? "a render-command replay ran past 3000000 dispatches"
                      : "a render-command list's walk left the record buffer");
    REXLOG_WARN("[wwe2k14][replay_watch] last record 0x{:08X} length {}: next position {} (list {} ends at {}, "
                "buffer {} slots); {}",
                record, len, next, list < kMaxLists ? int(list) : -1, end, capacity,
                can_end ? (runaway ? "the replay is ended (105)" : "the list is ended (104)")
                        : "cannot end it (the record buffer is not 16-byte aligned)");
  }
  if (!can_end) return;
  // The loop adds len + 1 to [manager+44] and reads the record at buffer + 16 * position (32-bit wrap).
  const uint32_t target_pos = (target - buffer) >> 4;
  Put32(base, manager + 44, target_pos - len - 1);
}

}  // namespace wwe2k14::replay_watch

// render_cmd_replay_lists: one call per frame; the count starts again and the lists' bounds are kept.
REX_HOOK_RAW(sub_82BB5BE8) {
  namespace rw = wwe2k14::replay_watch;
  rw::t_dispatches = 0;
  if (REXCVAR_GET(wwe2k14_replay_guard)) {
    const uint32_t manager = ctx.r3.u32;
    const uint32_t count = rw::Be32(base, manager + 132), descs = rw::Be32(base, manager + 108);
    rw::t_list_count = count < rw::kMaxLists ? count : rw::kMaxLists;
    for (uint32_t i = 0; i < rw::t_list_count; ++i) {
      rw::t_start[i] = rw::Be32(base, descs + i * 68 + 16);
      rw::t_end[i] = rw::Be32(base, descs + i * 68 + 12);
    }
    rw::t_buffer_index = rw::Be32(base, manager + 36);
  }
  __imp__sub_82BB5BE8(ctx, base);
}

// render_cmd_dispatch: r3 = the manager, r4 = the stack slot holding the record's address.
// Session 100 (WM30 crash, runs 6861-6892): a record with opcode 81 calls a model's cached record list (the address at rec+16, kept in the
// model: holder at object +864 / +868, filled by 0x82BB4AE0; recorder 0x82C24500). The update thread can release that model (0x82BD91B0)
// while the render thread replays last frame's lists, which still call the list: the call then lands in freed, reused memory (the
// crash: a "record" whose length is a code address). Before an op 81 runs, its target is checked: up to kCallCheckRecords records must have
// opcodes the replay knows (<= 105) and lengths that fit (< 4096 slots) until the list's own return (82). A target that fails is not
// called: the replay loop's own step moves past the 81 (the model misses that frame). --wwe2k14_replay_call_check (default on).
REXCVAR_DEFINE_BOOL(wwe2k14_replay_call_check, true, "wwe2k14",
                    "Session 100: skip a render-command call (opcode 81) whose cached list is not a valid record list (freed memory) "
                    "instead of jumping into it (src/replay_watch.h)");
namespace wwe2k14::replay_watch {
inline constexpr uint32_t kCallCheckRecords = 64;
inline std::atomic<uint64_t> g_calls_skipped{0};
// True when the list at `list` looks like a record list up to its return (or for kCallCheckRecords records).
inline bool CallTargetValid(const uint8_t* base, uint32_t list) {
  if (!list || (list & 15)) return false;
  uint32_t at = list;
  for (uint32_t k = 0; k < kCallCheckRecords; ++k) {
    const uint32_t op = Be32(base, at), len = Be32(base, at + 8);
    if (op == 82) return true;
    if (op > 105 || len >= 4096) return false;
    at += 16 * (len + 1);
  }
  return true;
}
}  // namespace wwe2k14::replay_watch

REX_HOOK_RAW(sub_82BB53D0) {
  namespace rw = wwe2k14::replay_watch;
  const uint32_t manager = ctx.r3.u32, slot = ctx.r4.u32;
  if (REXCVAR_GET(wwe2k14_replay_call_check)) {
    const uint32_t record = rw::Be32(base, slot);
    if (rw::Be32(base, record) == 81) {
      const uint32_t target = rw::Be32(base, record + 16);
      if (!rw::CallTargetValid(base, target)) {
        const uint64_t n = rw::g_calls_skipped.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n & 1023) == 0) {
          REXLOG_WARN("[wwe2k14][replay_watch] call skipped (frame {}): record 0x{:08X} calls 0x{:08X}, whose first words {:08X} {:08X} {:08X} {:08X} "
                      "are not a record list (a released model's cached list); {} skipped so far",
                      wwe2k14::g_guest_frame.load(std::memory_order_relaxed), record, target, rw::Be32(base, target), rw::Be32(base, target + 4),
                      rw::Be32(base, target + 8), rw::Be32(base, target + 12), n + 1);
        }
        return;  // the loop's own step ([manager+44] += len + 1) moves past the 81
      }
    }
  }
#if defined(__ANDROID__)
  constexpr bool kWatch = true;
#else
  const bool kWatch = REXCVAR_GET(wwe2k14_replay_guard);
#endif
  if (kWatch) {
    const uint32_t record = rw::Be32(base, slot);
    rw::t_ring[rw::t_dispatches % 64] = {record, rw::Be32(base, record), rw::Be32(base, record + 8),
                                         rw::Be32(base, manager + 44)};
    ++rw::t_dispatches;
  }
  {
    // S101 lane 83: replay cost per record by opcode (outermost dispatch only; the record's opcode is read only when the counters are on).
    wwe2k14::cost::ReplaySpan cost_span(wwe2k14::cost::On() ? rw::Be32(base, rw::Be32(base, slot)) : 0u);
    __imp__sub_82BB53D0(ctx, base);
  }
  if (REXCVAR_GET(wwe2k14_replay_guard)) {
    rw::Guard(base, manager, slot);
  } else if (kWatch && rw::t_dispatches == rw::kRunaway && rw::g_reports.fetch_add(1) == 0) {
    rw::LogRing(base, manager, "a render-command replay ran past 3000000 dispatches");
  }
}
