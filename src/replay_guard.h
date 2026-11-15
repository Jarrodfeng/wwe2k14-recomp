// wwe2k14 - Session 63 / 73 / 77 / 100 / 102: the render-command replay's bookkeeping (the runaway ring, the guard that ends a list that walks off the
// record buffer, the opcode-81 call check around each record), moved out of replay_watch.h UNCHANGED in behaviour (S102 lane W3 review F5) so the offline
// differential test (tools\w3\diff) compiles the code the game runs, with a stub FakeRecords and a test frame. The game's replay_watch.h still owns the
// cvars, the hooks and FakeRecords (guest heap); it includes this header. The history of the guard is in replay_watch.h's header comment.
//
// Before including: PPCContext (rex/ppc/context.h) and REXLOG_WARN. A test defines W3_GUARD_TEST_FRAME() / W3_GUARD_FRAME() first and supplies FakeRecords.
#pragma once

#include <atomic>
#include <cstdint>

#include "replay_call_check.h"

#ifndef W3_GUARD_TEST_FRAME
#define W3_GUARD_TEST_FRAME() REXCVAR_GET(wwe2k14_replay_guard_test_frame)
#define W3_GUARD_FRAME() g_guest_frame.load(std::memory_order_relaxed)
#endif

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

inline uint32_t FakeRecords(uint8_t* base);  // replay_watch.h (guest heap) in the game; the test's own

inline void LogRing(const uint8_t* base, uint32_t manager, const char* why) {
  const uint32_t lists = Be32(base, manager + 132), descs = Be32(base, manager + 108);
  REXLOG_WARN("[wwe2k14][replay_watch] {} (frame {}): manager 0x{:08X}, position {}, buffer index {} (at the walk's "
              "start {}), {} lists (descriptors at 0x{:08X})",
              why, W3_GUARD_FRAME(), manager, Be32(base, manager + 44),
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
  const int32_t test_frame = W3_GUARD_TEST_FRAME();
  const uint64_t frame = W3_GUARD_FRAME();
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


// S102 W3: the start of one replay (the sub_82BB5BE8 hook, before the loop, native or recompiled): the runaway count starts again and, with the guard on, the
// lists' bounds are kept.
inline void BeginReplay(const uint8_t* base, uint32_t manager, bool guard) {
  t_dispatches = 0;
  if (guard) {
    const uint32_t count = Be32(base, manager + 132), descs = Be32(base, manager + 108);
    t_list_count = count < kMaxLists ? count : kMaxLists;
    for (uint32_t i = 0; i < t_list_count; ++i) {
      t_start[i] = Be32(base, descs + i * 68 + 16);
      t_end[i] = Be32(base, descs + i * 68 + 12);
    }
    t_buffer_index = Be32(base, manager + 36);
  }
}

using wwe2k14::replay_check::kCallCheckRecords;
using wwe2k14::replay_check::CallTargetValid;
inline std::atomic<uint64_t> g_calls_skipped{0};

// The per-record bookkeeping around one dispatch (the sub_82BB53D0 hook's body, the flags read by the caller): the opcode-81 call check (Session 100),
// the ring and the guard (Sessions 73 / 77). `run` is the dispatch itself (native or recompiled, with the cost span the game adds).
template <class Run>
inline void HookedDispatch(PPCContext& ctx, uint8_t* base, bool call_check, bool watch, bool guard, Run&& run) {
  const uint32_t manager = ctx.r3.u32, slot = ctx.r4.u32;
  if (call_check) {
    const uint32_t record = Be32(base, slot);
    if (Be32(base, record) == 81) {
      const uint32_t target = Be32(base, record + 16);
      if (!CallTargetValid(base, target)) {
        const uint64_t n = g_calls_skipped.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n & 1023) == 0) {
          REXLOG_WARN("[wwe2k14][replay_watch] call skipped (frame {}): record 0x{:08X} calls 0x{:08X}, whose first words {:08X} {:08X} {:08X} {:08X} "
                      "are not a record list (a released model's cached list); {} skipped so far",
                      W3_GUARD_FRAME(), record, target, Be32(base, target), Be32(base, target + 4), Be32(base, target + 8), Be32(base, target + 12), n + 1);
        }
        return;  // the loop's own step ([manager+44] += len + 1) moves past the 81
      }
    }
  }
  if (watch) {
    const uint32_t record = Be32(base, slot);
    t_ring[t_dispatches % 64] = {record, Be32(base, record), Be32(base, record + 8), Be32(base, manager + 44)};
    ++t_dispatches;
  }
  run();
  if (guard) {
    Guard(base, manager, slot);
  } else if (watch && t_dispatches == kRunaway && g_reports.fetch_add(1) == 0) {
    LogRing(base, manager, "a render-command replay ran past 3000000 dispatches");
  }
}

}  // namespace wwe2k14::replay_watch
