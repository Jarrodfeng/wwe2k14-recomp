// lane 266: forgetting stored geometry a little every frame instead of all of it every 600th frame.
//
// Before: LiveForgetGeometry walked the whole g_geo_used map once per 600 recorded frames and erased every key unused for more than 600
// frames (one present, the game's swap thread). In owner mode with animated content (an entrance: 170-900 inline geometry builds a frame,
// each a NEW content key) that was a few hundred thousand keys, each a hash erase in three or four stores plus a free: 210-597 ms in the
// frame 600k+2 (docs\renderer\SKIP_GAME_WORK_MAP.md section 19.6).
//
// Now: every key is queued once, in order of the frame that stored it. Each frame the sweep looks only at the head of the queue, while the
// head is older than the keep window, within a count and a time budget. A key whose used mark was refreshed since is queued again at that
// mark (it lives on); a key still stale is erased by the caller's callback and from the used map. A key lives at least 601 frames, up to ~1200
// after a requeue (the queue is not re-sorted after a requeue, so a due key can wait behind a younger head: only ever later, never earlier).
//
// Pure (no game, no renderer): src\renderer_adapter\geo_forget_test.cpp is the offline proof.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>

namespace rdr_adapter::geoforget {

struct SweepResult {
  uint64_t looked = 0;        // queue items taken from the head
  uint64_t erased = 0;        // keys forgotten
  uint64_t requeued = 0;      // keys used again since: queued again at their newer mark
  bool time_capped = false;   // stopped by the time budget with due work left
  bool count_capped = false;  // stopped by the count budget with due work left
};

// A memo entry's "still stored" shortcut (reader.h GeoStillStored): the stores were not cleared whole since the entry was set, and either
// nothing was forgotten since (same forget epoch) or its used mark is younger than the keep window while live (Sweep erases only when
// now - used > keep and the used mark is never older than the entry's marked_frame). It must never return true for an erased key.
inline bool StillStored(uint64_t entry_clear, uint64_t clear_epoch, uint64_t entry_stored, uint64_t forget_epoch, uint64_t marked_frame, uint64_t frame,
                        uint64_t keep, bool live) {
  if (entry_clear != clear_epoch) return false;
  if (entry_stored == forget_epoch) return true;
  return live && marked_frame <= frame && frame - marked_frame <= keep;
}

// lane 293 (sign flicker): a memo entry not on the shortcut asks the store map (g_cap.geo: content key -> the ref with the sink offsets). Absent:
// rebuild. Present: the entry takes the offsets found there (another entry may have stored the same content again under a new sink key after
// the old one was dropped; handing out the entry's own old ref gave the zero-filled missing key, the board invisible every second frame).
template <class Geo, class Ref>
inline bool ResolveStored(bool shortcut, const Geo& geo, uint64_t content_key, Ref& ref) {
  if (shortcut) return true;
  auto it = geo.find(content_key);
  if (it == geo.end()) return false;
  ref = it->second;
  return true;
}

// The mark refresh of a memo hit: the used mark (and the entry's marked_frame, which StillStored's second clause trusts) is written only when
// the key is in the used map. False = the key is not there: the caller rebuilds.
inline bool MarkRefresh(bool key_found, uint64_t& marked_frame, uint64_t frame) {
  if (!key_found) return false;
  marked_frame = frame;
  return true;
}

class Expiry {
 public:
  // A key stored for the first time (its used mark is `frame`).
  void Note(uint64_t key, uint64_t frame) { queue_.push_back({key, frame}); }
  void Clear() { queue_.clear(); }
  size_t Size() const { return queue_.size(); }

  // How many queue items one frame may look at: enough to keep up with the arrival rate (queue / 256 is ~2.3x the new keys a frame at a
  // 600-frame window), never fewer than `floor`.
  size_t DefaultItems(size_t floor = 1024) const { return queue_.size() / 256 > floor ? queue_.size() / 256 : floor; }

  // used: key -> last frame used (the caller's map). erase_cb(key) removes the key from the caller's other stores. `max_items` 0 = no
  // count limit; `budget_ns` 0 = no time limit (the time is read every 64 items, after the first 64).
  template <class Erase>
  SweepResult Sweep(std::unordered_map<uint64_t, uint64_t>& used, uint64_t now, uint64_t keep, size_t max_items, int64_t budget_ns, Erase&& erase_cb) {
    SweepResult r;
    const auto t0 = budget_ns > 0 ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    while (!queue_.empty()) {
      const Item head = queue_.front();
      if (now - head.frame <= keep) break;  // not due (the queue is in store order: later items are younger)
      if (max_items && r.looked >= max_items) {
        r.count_capped = true;
        break;
      }
      if (budget_ns > 0 && r.looked >= 64 && (r.looked & 63) == 0 &&
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count() > budget_ns) {
        r.time_capped = true;
        break;
      }
      queue_.pop_front();
      ++r.looked;
      auto it = used.find(head.key);
      if (it == used.end()) continue;  // already gone (cleared at a live start)
      if (now - it->second <= keep) {
        queue_.push_back({head.key, it->second});
        ++r.requeued;
        continue;
      }
      erase_cb(head.key);
      used.erase(it);
      ++r.erased;
    }
    return r;
  }

 private:
  struct Item {
    uint64_t key, frame;
  };
  std::deque<Item> queue_;
};

}  // namespace rdr_adapter::geoforget
