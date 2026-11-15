// S102 M7: the guest frame at which each route mark (the input script's `mark N`) fired, for --wwe2k14_ab_flip's anchor=N. Two atomics' worth of state, no includes beyond <atomic>:
// input_script.h's kMark calls NoteMark; ab_flip.h reads MarkFrame once a frame. 0 = the mark has not fired.
#pragma once
#include <atomic>
#include <cstdint>

namespace ab_flip_anchor {
inline std::atomic<uint64_t> g_mark_frame[64];
inline void NoteMark(uint32_t id, uint64_t frame) {
  if (id >= 1 && id < 64) g_mark_frame[id].store(frame ? frame : 1, std::memory_order_relaxed);
}
inline uint64_t MarkFrame(uint32_t id) { return id >= 1 && id < 64 ? g_mark_frame[id].load(std::memory_order_relaxed) : 0; }
}  // namespace ab_flip_anchor
