#pragma once
// wwe2k14 - Session 101 lane 302: the worst file write of the current window, for the [perf][sys] line (src/perf_sys.h).
// Dependency free (atomics only) so renderer code can call it. NoteFileWrite(ms) from any thread; the sampling thread reads and resets.
#include <atomic>
#include <cstdint>

namespace wwe2k14::perf_sys {

inline std::atomic<uint64_t>& FileWriteWorstUs() {
  static std::atomic<uint64_t> v{0};
  return v;
}

inline void NoteFileWrite(double ms) {
  const uint64_t us = ms <= 0 ? 0 : uint64_t(ms * 1000.0);
  uint64_t cur = FileWriteWorstUs().load(std::memory_order_relaxed);
  while (us > cur && !FileWriteWorstUs().compare_exchange_weak(cur, us, std::memory_order_relaxed)) {}
}

}  // namespace wwe2k14::perf_sys
