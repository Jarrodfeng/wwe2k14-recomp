// wwe2k14 - loader RAM file-cache copy skip: the pure decision + counters (S105 A6S; docs/research/s105/A6S.md, A6S_ADV.md).
// No SDK includes (unit test tools\tests\loader_cache_skip_test.cpp). The hook is src\loader_cache_skip.h.
//
// The game's sub_82C54DE0 (cache insert after a read-end callback) either MOVES a loader-allocated buffer into the cache (request +140 != 0
// and +116 == -2: no copy, no allocation) or COPIES the caller's own buffer (sub_82C54C90 -> sub_82C54BB0, which waits on the read-end
// thread's own done event when the 11 MB cache heap is full: the Move-set deadlock on slow machines). The copy is a 360 habit (it saved a
// DVD seek). We skip it; the move path is the game's own.
#pragma once

#include <atomic>
#include <cstdint>

namespace wwe2k14::loader_cache {

// The exact negation of the game's move test. true = skip (return "not cached" without calling the original).
inline bool CacheInsertSkips(uint32_t f140, uint32_t f116) { return !(f140 != 0 && f116 == 0xFFFFFFFEu); }

inline std::atomic<uint64_t> g_skipped{0};   // copy REQUESTS skipped (counted before the game's size gate: some of these the original would have refused anyway)
inline std::atomic<uint64_t> g_moved{0};     // moves the original really did (it returned r3 == 1)
inline std::atomic<uint64_t> g_fallback{0};  // a guard failed (r3 / cache word / unreadable request): the game's own code ran, copy included
inline constexpr uint64_t kLogEvery = 8;     // one census line per this many events of any kind, and on the first of each kind

// Each returns true when the caller should print the census line now (first of its kind, or every kLogEvery-th event in total).
inline bool Due(uint64_t kindCount) {
  const uint64_t total = g_skipped.load(std::memory_order_relaxed) + g_moved.load(std::memory_order_relaxed) +
                         g_fallback.load(std::memory_order_relaxed);
  return kindCount == 1 || (total % kLogEvery) == 0;
}
inline bool NoteSkip() { return Due(g_skipped.fetch_add(1, std::memory_order_relaxed) + 1); }
inline bool NoteMove() { return Due(g_moved.fetch_add(1, std::memory_order_relaxed) + 1); }
inline bool NoteFallback() { return Due(g_fallback.fetch_add(1, std::memory_order_relaxed) + 1); }

}  // namespace wwe2k14::loader_cache
