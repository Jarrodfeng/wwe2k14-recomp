// S102 R25-A (docs\renderer\SWAP_TAIL.md section 9): the swap thread's own waits that were ours. No game or SDK includes.
//
// One lane switch, wwe2k14_rdr_swap_tail2 (PerFrame, default 0), a bit mask; the reader copies it into g_mask once per guest frame at the present (reader.h, next to
// wwe2k14_rdr_upd_lean), so the fence / interrupt / flip hooks read one relaxed atomic:
//   1 kJoinLater   owner_timing.h JoinReader: the hook no longer waits for every reader worker job; it takes a ticket (join_ticket.h) and the "Owner timing" thread waits for
//                  the ticket right before it answers the item (the guest sees the fence only then). Off = today's WaitAll on the hook's thread, byte for byte.
// Bits for the guest critical section (sub_82507F68) and the SetEvents are NOT built: SWAP_TAIL.md section 9 says why (the lock is the game's own crowd-manager update;
// the SetEvents are mostly the game's KeSetEvent).
#pragma once
#include <atomic>
#include <cstdint>

namespace wwe2k14::swap_tail2 {

enum Bits : uint32_t { kJoinLater = 1, kAll = 1 };

inline std::atomic<uint32_t> g_mask{0};  // set once a guest frame by the reader (PerFrame switch wwe2k14_rdr_swap_tail2)
inline bool On(uint32_t bit) { return (g_mask.load(std::memory_order_relaxed) & bit) != 0; }

// Census (the "Owner timing" thread and the hooks bump them; read at print time only).
struct Census {
  std::atomic<uint64_t> hooks_waited_all{0};   // bit 1 off: the hook waited itself (WaitAll)
  std::atomic<uint64_t> tickets{0};            // bit 1 on: a ticket went on the item
  std::atomic<uint64_t> tickets_empty{0};      // bit 1 on: nothing was pending, no ticket (no wait anywhere)
  std::atomic<uint64_t> timing_waits{0};       // the timing thread had to wait for a ticket (jobs still running)
  std::atomic<uint64_t> timing_wait_ns{0};     // total, and
  std::atomic<uint64_t> timing_wait_max_ns{0}; // the longest
};
inline Census g_census;

inline void NoteMax(std::atomic<uint64_t>& m, uint64_t v) {
  uint64_t cur = m.load(std::memory_order_relaxed);
  while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

}  // namespace wwe2k14::swap_tail2
