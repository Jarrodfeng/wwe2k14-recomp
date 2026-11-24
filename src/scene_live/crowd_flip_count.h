// wwe2k14 - Session 110, lane CROWDFLIPS. A CENSUS ONLY: counts what the crowd's DRAWN classes do from latch to latch; changes nothing.
// std-only (no game, no SDK): the latch reader (crowd_latch_reader.h ReadSnapshot) fills one class byte per (type, member) per latch, the tracker keeps the
// last two latches' arrays and counts. A player sees flicker as a member that changes class and changes BACK (A-B-A): `flip_back`.
//
// Class byte: 0 = culled (or no such member), 1 / 2 / 3 = body LOD group 0 / 1 / 2, 4 = flat card.
// The rule (CountMember), prev = the class at the previous latch, prev2 = two latches ago, cur = now:
//  - cur culled: counts `culled` when prev was visible; nothing else.
//  - cur visible, prev culled: `appeared` (never a flip, never a flip_back, whatever prev2 was).
//  - cur visible, prev visible, cur != prev: one `flip`; a `card` flip when exactly one of the two is the card (body <-> card), else a `lod` flip;
//    it is also a `flip_back` when prev2 == cur (A-B-A). A-B-B counts one flip at the B step and nothing at the second B.
// The first latch has no previous: it only counts visible members. A latch whose lists were not fully read is not committed (the caller).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace wwe2k14::crowd_flips {

inline constexpr uint32_t kTypes = 20, kMaxPerType = 768, kSlots = kTypes * kMaxPerType;
inline constexpr uint8_t kCulledClass = 0, kCardClass = 4;

struct Counts {
  uint64_t latches = 0, visible = 0, flips = 0, card = 0, lod = 0, flip_back = 0, frames_with_flip_back = 0, longest_run = 0, appeared = 0, culled = 0;
};

struct Delta {
  uint32_t visible = 0, flips = 0, card = 0, lod = 0, flip_back = 0, appeared = 0, culled = 0;
};

// The pure per-member rule; `plant` = 1: any change counts as flip_back (not A-B-A only); 2: a culled step counts as a flip (planted twins of the unit).
inline void CountMember(uint8_t prev2, uint8_t prev, uint8_t cur, Delta& d, int plant = 0) {
  if (cur == kCulledClass) {
    if (prev != kCulledClass) {
      ++d.culled;
      if (plant == 2) ++d.flips, ++d.lod;
    }
    return;
  }
  ++d.visible;
  if (prev == kCulledClass) {
    ++d.appeared;
    if (plant == 2) ++d.flips, ++d.lod;
    return;
  }
  if (cur == prev) return;
  ++d.flips;
  if ((cur == kCardClass) != (prev == kCardClass)) ++d.card;
  else ++d.lod;
  if (plant == 1 ? true : prev2 == cur) ++d.flip_back;
}

inline void Add(Counts& c, const Delta& d, bool first_latch) {
  ++c.latches;
  c.visible += d.visible;
  if (first_latch) return;
  c.flips += d.flips, c.card += d.card, c.lod += d.lod, c.flip_back += d.flip_back, c.appeared += d.appeared, c.culled += d.culled;
}

// The latch thread owns the arrays. `busy` keeps a second thread that calls the reader from changing them under the first (it skips its census, never waits).
struct Tracker {
  uint8_t cur[kSlots], prev[kSlots], prev2[kSlots];
  uint64_t seen = 0, run = 0, window_run_max = 0;
  Counts total, window;
  std::atomic<bool> busy{false};
  Tracker() { std::memset(cur, 0, sizeof cur), std::memset(prev, 0, sizeof prev), std::memset(prev2, 0, sizeof prev2); }

  bool Begin() {
    if (busy.exchange(true, std::memory_order_acquire)) return false;
    std::memset(cur, 0, sizeof cur);
    return true;
  }
  void Abort() { busy.store(false, std::memory_order_release); }

  // One whole, successfully read latch: cur holds the classes. Returns true when a census line is due (every `cadence` latches).
  bool Commit(uint32_t cadence = 600, int plant = 0) {
    Delta d;
    for (uint32_t i = 0; i < kSlots; ++i) CountMember(prev2[i], prev[i], cur[i], d, plant);
    const bool first = seen == 0;
    Add(total, d, first), Add(window, d, first);
    if (!first && d.flip_back > 0) {
      ++total.frames_with_flip_back, ++window.frames_with_flip_back;
      ++run;
      if (run > total.longest_run) total.longest_run = run;
      if (run > window.longest_run) window.longest_run = run;
    } else {
      run = 0;
    }
    ++seen;
    std::memcpy(prev2, prev, sizeof prev2);
    std::memcpy(prev, cur, sizeof prev);
    busy.store(false, std::memory_order_release);
    return cadence && seen % cadence == 0;
  }

  static int Fields(char* buf, size_t n, const Counts& c) {
    const double avg = c.latches ? double(c.visible) / double(c.latches) : 0.0;
    return std::snprintf(buf, n, "latches %llu visible_avg %.1f flips %llu (card %llu, lod %llu) flip_back %llu frames_with_flip_back %llu longest_flip_back_run %llu appeared %llu culled %llu",
                         (unsigned long long)c.latches, avg, (unsigned long long)c.flips, (unsigned long long)c.card, (unsigned long long)c.lod, (unsigned long long)c.flip_back,
                         (unsigned long long)c.frames_with_flip_back, (unsigned long long)c.longest_run, (unsigned long long)c.appeared, (unsigned long long)c.culled);
  }
  // "cumulative: <fields> | window: <fields>"; then the window restarts. Latch thread only (after a Commit that returned true).
  void Line(char* buf, size_t n) {
    int k = std::snprintf(buf, n, "cumulative: ");
    if (k > 0 && size_t(k) < n) k += Fields(buf + k, n - size_t(k), total);
    if (k > 0 && size_t(k) < n) k += std::snprintf(buf + k, n - size_t(k), " | window: ");
    if (k > 0 && size_t(k) < n) Fields(buf + k, n - size_t(k), window);
    window = Counts{};
  }
};

inline Tracker& The() {
  static Tracker t;
  return t;
}
inline std::atomic<bool> g_line_due{false};   // set by Latch::Finish, taken by the glue that logs (the reader itself is pure: no logging)

// One per ReadSnapshot call: Open() when the lists are about to be read, Set() per member, Finish() once the whole read succeeded (else the destructor drops it).
struct Latch {
  Tracker* t = nullptr;
  bool done = false;
  void Open() {
    Tracker& x = The();
    if (x.Begin()) t = &x;
  }
  void Set(uint32_t type, uint32_t member, uint8_t cls) {
    if (t && type < kTypes && member < kMaxPerType) t->cur[type * kMaxPerType + member] = cls;
  }
  void Finish() {
    if (t && !done) {
      done = true;
      if (t->Commit()) g_line_due.store(true, std::memory_order_release);
    }
  }
  ~Latch() {
    if (t && !done) t->Abort();
  }
};

}  // namespace wwe2k14::crowd_flips
