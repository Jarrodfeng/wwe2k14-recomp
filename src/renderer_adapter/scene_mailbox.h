// The latest-wins hand-off (Session 103, lane G; docs\renderer\SCENE_PACING.md 3a). Header-only, no game / SDK / renderer includes (the unit drives it).
//
// ONE ordered queue (live.cpp's `mail`, a deque under mail_mu) stays the only FIFO: nothing is produced into a second place, so no message can overtake another
// and the swap thread holds no extra state. "Latest wins" happens on the CONSUMER side, at the pop, under the lock the consumer already takes:
// TakeRun moves the run of consecutive PLAIN scene frames at the front out of the queue, as {dropped (oldest first), newest}. The consumer handles every dropped
// message through the existing skipped-frame path (images, released ids, display curve, contents, owed lasting copies: all of it, so nothing a message carries is lost)
// and draws the newest. A message the consumer must never skip (a DrawAndRead part, a named picture) ends the run BEFORE it; a message it must draw and stop at
// (a scene sheet frame) ends the run AFTER it.
#pragma once
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace rdr_adapter {

enum class MailKind : uint8_t {
  Plain = 0,    // a scene frame that may be skipped when a newer one follows it
  Barrier = 1,  // never skipped, never passed: the run stops before it (read part, named picture; everything when the scene switch is not 2)
  Last = 2,     // drawn, and the run stops after it (a scene sheet frame): never skipped, never passed
};

struct MailStats {  // render thread only
  uint64_t takes = 0;            // newest messages handed out
  uint64_t dropped = 0;          // messages handed out as dropped
  uint64_t max_run_dropped = 0;  // the most dropped messages one take handed over
  uint64_t barrier_stops = 0;    // runs that ended before a Barrier
};

template <class T>
struct MailRun {
  std::vector<T> dropped;  // older plain frames, oldest first: handle each through the skipped-frame path with its own forced-skip flag
  T newest{};
  bool have = false;
};

// Under the caller's lock. classify(const T&) -> MailKind. Returns false when the queue is empty.
// plant_ignore_barrier / plant_front_only: the unit's planted faults only (a barrier is dropped; the newest is not taken, i.e. a plain FIFO).
template <class T, class Classify>
bool TakeRun(std::deque<T>& q, Classify&& classify, MailRun<T>& out, MailStats* stats = nullptr, bool plant_ignore_barrier = false, bool plant_front_only = false) {
  out.dropped.clear();
  out.have = false;
  if (q.empty()) return false;
  size_t end = 0;  // the run is q[0, end)
  if (!plant_front_only) {
    for (size_t i = 0; i < q.size(); ++i) {
      MailKind k = classify(q[i]);
      if (plant_ignore_barrier) k = MailKind::Plain;
      if (k == MailKind::Barrier) {
        if (i > 0 && stats) ++stats->barrier_stops;
        break;
      }
      end = i + 1;
      if (k == MailKind::Last) break;
    }
  }
  if (end == 0) end = 1;  // the front is a Barrier (or the plant): taken alone, nothing dropped
  out.dropped.reserve(end - 1);
  for (size_t i = 0; i + 1 < end; ++i) out.dropped.push_back(std::move(q[i]));
  out.newest = std::move(q[end - 1]);
  q.erase(q.begin(), q.begin() + std::ptrdiff_t(end));
  out.have = true;
  if (stats) {
    ++stats->takes;
    stats->dropped += out.dropped.size();
    if (out.dropped.size() > stats->max_run_dropped) stats->max_run_dropped = out.dropped.size();
  }
  return true;
}

}  // namespace rdr_adapter
