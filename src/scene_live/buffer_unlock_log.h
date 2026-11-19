// A ring of the data addresses of the game's vertex / index buffer unlocks (Session 103, lane CW-2b; docs\research\s103\CW2_review.md bug 1). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure (standard library only). The game writes a buffer's bytes between its lock and its unlock (VB: sub_82CACE50, IB: sub_82CACF10; src\vb_unlock_report.h hooks both and calls Note). A reader that
// keeps the bytes it read asks Dirty(its buffers' data addresses, the sequence it read at): true when one of them was unlocked since, or when more unlocks happened than the ring holds (then every
// reader re-reads: never a stale model). Note and Dirty may run on different game threads; the race window (an unlock claimed but not yet published) is closed by the reader's forced re-read period
// (guest_model_contrib.h kForceRereadCalls) and counted there as a "missed unlock" when such a re-read finds different bytes.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr_scene::live::unlocklog {

inline constexpr uint32_t kRing = 1024;   // power of two

struct Ring {
  std::atomic<uint64_t> seq{0};   // unlocks claimed so far
  std::atomic<uint32_t> addr[kRing] = {};
  std::atomic<uint64_t> at[kRing] = {};
};
inline Ring& TheRing() { static Ring r; return r; }

// An unlock of the buffer whose data starts at `data` (CPU virtual address, fetch-type bits masked off by the caller or not: both are masked here).
inline void Note(uint32_t data, Ring& r = TheRing()) {
  const uint64_t s = r.seq.fetch_add(1, std::memory_order_acq_rel) + 1;
  const uint32_t slot = uint32_t(s) & (kRing - 1);
  r.addr[slot].store(data & ~3u, std::memory_order_release);
  r.at[slot].store(s, std::memory_order_release);
}
// The sequence a reader takes BEFORE it reads bytes (anything unlocked after it makes the read dirty).
inline uint64_t Now(const Ring& r = TheRing()) { return r.seq.load(std::memory_order_acquire); }

// True when any of addrs[0..n) was unlocked after `since`, or the ring was overrun since.
inline bool Dirty(const uint32_t* addrs, size_t n, uint64_t since, const Ring& r = TheRing()) {
  const uint64_t now = r.seq.load(std::memory_order_acquire);
  if (now == since) return false;
  if (now - since >= kRing) return true;   // overrun: unknown
  for (uint64_t s = since + 1; s <= now; ++s) {
    const uint32_t slot = uint32_t(s) & (kRing - 1);
    if (r.at[slot].load(std::memory_order_acquire) < s) return true;   // claimed, not published yet: treat as dirty
    const uint32_t a = r.addr[slot].load(std::memory_order_acquire) & ~3u;
    for (size_t i = 0; i < n; ++i)
      if ((addrs[i] & ~3u) == a) return true;
  }
  return false;
}

}  // namespace rdr_scene::live::unlocklog
