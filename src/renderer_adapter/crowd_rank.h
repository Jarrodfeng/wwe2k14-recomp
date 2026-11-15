// Session 101 lane 234 (docs\renderer\CROWD_WIDE.md "Lane 234"): the crowd density's rank (0-99 per member: a member is drawn while its rank < the CROWD DENSITY percent), with
// the TRACKING the SDK's old path has had since Session 95 (command_processor.cpp CrowdRankOf, gpu_scene_crowd_density_track) and the owner binder never got.
// Without it the rank is a hash of the member's exact world position (c76.w c77.w c78.w): a stands member never moves, so it keeps its rank, but a sign / foam hand
// moves with its holder's arm every frame, so its rank is re-rolled every frame and at density 70 about 42% of the signs flip in or out each frame (the flicker Jarrod saw
// on the phone, whose first-run density is 70). With tracking, a position not seen before inherits the rank of the nearest position of the last two frames within
// kTrackRadius (24 units), so a moving sign keeps its rank. Pure (no SDK, no game): tools\crowd\crowd_rank_test.cpp.
// One thread (the binder's: Bind runs the frame and every CrowdRank call is inside it).
#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <cstring>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rdr_adapter {

// The rank of a position by hash alone (the old rule; what the SDK's CrowdRankOf returns with tracking off).
inline uint32_t CrowdRankKey(uint32_t w3, uint32_t w7, uint32_t w11) { return w3 * 0x9E3779B1u ^ w7 * 0x85EBCA77u ^ w11 * 0xC2B2AE3Du; }
inline int CrowdRankHash(uint32_t key) {
  uint32_t h = key;
  h ^= h >> 16, h *= 0x7FEB352Du, h ^= h >> 15, h *= 0x846CA68Bu, h ^= h >> 16;
  return int(h % 100u);
}

// S102 R21-C (step 2): the tracker's memory of positions, in two shapes with the same four calls. CrowdNodeKnown is the old node-based unordered_map; CrowdFlatKnown is an open-addressing table
// (linear probing, never erased, emptied in O(1) by a generation stamp) keyed by the same 32-bit position key. The tracker's logic is written once (RankT) over either, so the
// results are the same by construction (tools\crowd\crowd_rank_test.cpp feeds both the same calls).
struct CrowdKnown {
  uint8_t rank = 0;
  uint64_t last = 0;  // the frame this position was last tested
};
struct CrowdNodeKnown {
  std::unordered_map<uint32_t, CrowdKnown> m;
  CrowdKnown* Find(uint32_t key) {
    auto it = m.find(key);
    return it == m.end() ? nullptr : &it->second;
  }
  CrowdKnown* Assign(uint32_t key, const CrowdKnown& v) { return &(m[key] = v); }
  void Clear() { m.clear(); }
  void Swap(CrowdNodeKnown& o) { m.swap(o.m); }
  template <class F>
  void ForEach(F&& f) const {
    for (const auto& kv : m) f(kv.first, kv.second);
  }
};
struct CrowdFlatKnown {
  struct Slot {
    CrowdKnown v;
    uint32_t key = 0, gen = 0;
  };
  std::vector<Slot> slots;
  uint32_t gen = 1;
  size_t count = 0;
  static size_t Mix(uint32_t k) { return size_t((k * 0x9E3779B1u) >> 7); }  // (the key is already a hash; a multiply spreads its low bits)
  CrowdKnown* Find(uint32_t key) {
    if (slots.empty()) return nullptr;
    const size_t mask = slots.size() - 1;
    for (size_t i = Mix(key) & mask;; i = (i + 1) & mask) {
      Slot& s = slots[i];
      if (s.gen != gen) return nullptr;
      if (s.key == key) return &s.v;
    }
  }
  CrowdKnown* Assign(uint32_t key, const CrowdKnown& v) {
    if ((count + 1) * 2 > slots.size()) Grow();
    const size_t mask = slots.size() - 1;
    for (size_t i = Mix(key) & mask;; i = (i + 1) & mask) {
      Slot& s = slots[i];
      if (s.gen != gen) {
        s.v = v, s.key = key, s.gen = gen;
        ++count;
        return &s.v;
      }
      if (s.key == key) return s.v = v, &s.v;
    }
  }
  void Clear() {
    count = 0;
    if (++gen == 0) {
      for (Slot& s : slots) s.gen = 0;
      gen = 1;
    }
  }
  void Swap(CrowdFlatKnown& o) {
    slots.swap(o.slots);
    std::swap(gen, o.gen);
    std::swap(count, o.count);
  }
  template <class F>
  void ForEach(F&& f) const {
    for (const Slot& s : slots)
      if (s.gen == gen) f(s.key, s.v);
  }
  void Grow() {
    std::vector<Slot> old;
    old.swap(slots);
    const uint32_t old_gen = gen;
    slots.assign(old.empty() ? 4096 : old.size() * 2, Slot{});
    gen = 1, count = 0;
    for (const Slot& s : old)
      if (s.gen == old_gen) Assign(s.key, s.v);
  }
};

class CrowdRankTracker {
 public:
  static constexpr float kTrackRadius = 24.0f;
  float radius = kTrackRadius;  // the inheritance radius (the unit test and the rank_radius_off twin change it)
  bool guard = true;    // false: rotate on every call (the first lane 234 version; the burst plant)
  bool enabled = true;  // false: the rank of the exact position (the plant, and --wwe2k14_rdr_density_track=false)

  // Once per GUEST frame (Bind passes frame.header.guest_frame: the parts of a frame split at a readback, lasting-only binds of skipped frames and picture isolates repeat it or
  // test nothing, and must not rotate the memory), before its first rank call. A frame in which nothing was tested leaves the memory as it is.
  void NextFrame(uint64_t guest_frame) {
    NoteThread();
    if (!enabled) return;
    if (guard && (guest_frame == guest_frame_ || (guest_frame_ = guest_frame, now_.empty()))) return;
    guest_frame_ = guest_frame;
    last2_.swap(last_);
    last_.swap(now_);
    now_.clear();
    grid_[0].valid = grid_[1].valid = false;   // (the lists the grids index changed)
    if (++frame_ % 600 == 0) {  // positions not seen for a window go (the map must not grow over a long match)
      if (fast_) fk_prev_.Swap(fk_), fk_.Clear();
      else nk_prev_.Swap(nk_), nk_.Clear();
    }
  }

  int Rank(uint32_t w3, uint32_t w7, uint32_t w11) {
    NoteThread();
    return fast_ ? RankT(fk_, fk_prev_, w3, w7, w11) : RankT(nk_, nk_prev_, w3, w7, w11);
  }

  // S102 R21-C (step 2, wwe2k14_rdr_crowd_fast): the flat table instead of the node map. The memory moves across when the choice changes (a switch flip mid-run keeps every rank).
  void SetFast(bool fast) {
    if (fast == fast_) return;
    if (fast) {
      fk_.Clear(), fk_prev_.Clear();
      nk_prev_.ForEach([&](uint32_t k, const CrowdKnown& v) { fk_prev_.Assign(k, v); });
      nk_.ForEach([&](uint32_t k, const CrowdKnown& v) { fk_.Assign(k, v); });
      nk_.Clear(), nk_prev_.Clear();
    } else {
      nk_.Clear(), nk_prev_.Clear();
      fk_prev_.ForEach([&](uint32_t k, const CrowdKnown& v) { nk_prev_.Assign(k, v); });
      fk_.ForEach([&](uint32_t k, const CrowdKnown& v) { nk_.Assign(k, v); });
      fk_.Clear(), fk_prev_.Clear();
    }
    fast_ = fast;
  }
  bool Fast() const { return fast_; }
  // R21-C review F6: the tracker is a single-thread global (BindDraw, InjectBody and cards BuildPlan all call it inside one Bind). The first caller owns it; a call from another thread is COUNTED
  // (printed in the census, and the unit test plants it), never silently racing: R21-B's stage split may move binding and injection to different threads.
  uint64_t ThreadBreaks() const { return thread_breaks_; }
  void ResetOwner() { owner_ = std::thread::id(); }
  uint64_t Calls() const { return followed_ + inherited_ + fresh_; }  // (every call that is not disabled is one of the three)

  uint64_t Frame() const { return frame_; }
  uint64_t Inherited() const { return inherited_; }
  uint64_t Fresh() const { return fresh_; }
  uint64_t Followed() const { return followed_; }

 private:
  struct Entry {
    float x, y, z;
    uint8_t rank;
    uint32_t key;
  };
  using Known = CrowdKnown;
  static float F(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }
  template <class Table>
  int RankT(Table& known_, Table& known_prev_, uint32_t w3, uint32_t w7, uint32_t w11) {
    const uint32_t key = CrowdRankKey(w3, w7, w11);
    const int hashed = CrowdRankHash(key);
    if (!enabled) return hashed;
    Known* known = known_.Find(key);
    if (!known) {
      if (const Known* old = known_prev_.Find(key)) known = known_.Assign(key, *old);
    }
    const float x = F(w3), y = F(w7), z = F(w11);
    if (known) {  // a position seen before: the same member, still
      if (known->last != frame_) {
        known->last = frame_;
        now_.push_back({x, y, z, known->rank, key});
      }
      ++followed_;
      return known->rank;
    }
    // A position not seen before: a member that moved (take the rank of the nearest of the last two frames within the radius that was not tested again at its old
    // position this frame), or one that came into view (the hash).
    float best = radius * radius;
    int rank = hashed;
    bool found = false;
    if constexpr (std::is_same_v<Table, CrowdFlatKnown>) {
      // R21-C review F5 (fast path): the nearest-of-the-last-two-frames scan over the cells the position's neighbours fall in, not over every entry of the lists. The same entries, in the same
      // order (ascending index), pass the same test, so the result is the list scan's, bit for bit (any entry within the radius lies in a neighbouring cell: |dx| < radius).
      const bool finite = std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
      for (int li = 0; li < 2 && finite; ++li) {
        const std::vector<Entry>& list = li == 0 ? last_ : last2_;
        GridOf(li, list);
        Grid& g = grid_[li];
        cand_.clear();
        const int32_t qx = Cell(x), qy = Cell(y), qz = Cell(z);
        for (int ox = -1; ox <= 1; ++ox)
          for (int oy = -1; oy <= 1; ++oy)
            for (int oz = -1; oz <= 1; ++oz) {
              const int32_t cx = qx + ox, cy = qy + oy, cz = qz + oz;
              for (int32_t i = g.head[CellHash(cx, cy, cz) & g.mask]; i >= 0; i = g.next[size_t(i)])
                if (g.cx[size_t(i)] == cx && g.cy[size_t(i)] == cy && g.cz[size_t(i)] == cz) cand_.push_back(i);
            }
        std::sort(cand_.begin(), cand_.end());
        for (int32_t i : cand_) {
          const Entry& e = list[size_t(i)];
          const float dx = e.x - x, dy = e.y - y, dz = e.z - z, d = dx * dx + dy * dy + dz * dz;
          if (!(d > 0.0f && d < best)) continue;
          if (const Known* kt = known_.Find(e.key); kt && kt->last == frame_) continue;
          best = d, rank = e.rank, found = true;
        }
        if (found) break;
      }
    } else {
      for (const std::vector<Entry>* list : {&last_, &last2_}) {
        for (const Entry& e : *list) {
          const float dx = e.x - x, dy = e.y - y, dz = e.z - z, d = dx * dx + dy * dy + dz * dz;
          if (!(d > 0.0f && d < best)) continue;  // (the distance first: the lookup below only for the few that are near)
          if (const Known* kt = known_.Find(e.key); kt && kt->last == frame_) continue;
          best = d, rank = e.rank, found = true;
        }
        if (found) break;
      }
    }
    ++(found ? inherited_ : fresh_);
    known_.Assign(key, {uint8_t(rank), frame_});
    now_.push_back({x, y, z, uint8_t(rank), key});
    return rank;
  }
  // The spatial index of one of the two frame lists (built on the first inherited / fresh test of a frame, dropped when the lists rotate): chains of entry indices by cell of `radius` units.
  struct Grid {
    std::vector<int32_t> head, next, cx, cy, cz;
    size_t mask = 0;
    bool valid = false;
    float radius = 0;
  };
  int32_t Cell(float v) const { return int32_t(std::floor(std::clamp(v / grid_radius_, -1.0e9f, 1.0e9f))); }
  static size_t CellHash(int32_t x, int32_t y, int32_t z) { return size_t((uint32_t(x) * 73856093u) ^ (uint32_t(y) * 19349663u) ^ (uint32_t(z) * 83492791u)); }
  void GridOf(int li, const std::vector<Entry>& list) {
    Grid& g = grid_[li];
    if (g.valid && g.radius == radius) return;
    grid_radius_ = radius;
    size_t buckets = 64;
    while (buckets < list.size() * 2) buckets <<= 1;
    g.head.assign(buckets, -1), g.mask = buckets - 1;
    g.next.assign(list.size(), -1), g.cx.assign(list.size(), 0), g.cy.assign(list.size(), 0), g.cz.assign(list.size(), 0);
    for (size_t i = 0; i < list.size(); ++i) {
      const Entry& e = list[i];
      if (!(std::isfinite(e.x) && std::isfinite(e.y) && std::isfinite(e.z))) continue;   // (never within the radius of anything: d is NaN or infinite)
      g.cx[i] = Cell(e.x), g.cy[i] = Cell(e.y), g.cz[i] = Cell(e.z);
      const size_t b = CellHash(g.cx[i], g.cy[i], g.cz[i]) & g.mask;
      g.next[i] = g.head[b], g.head[b] = int32_t(i);
    }
    g.valid = true, g.radius = radius;
  }
  void NoteThread() {
    const std::thread::id me = std::this_thread::get_id();
    if (owner_ == std::thread::id()) owner_ = me;
    else if (owner_ != me) ++thread_breaks_, owner_ = me;
  }
  Grid grid_[2];
  float grid_radius_ = kTrackRadius;
  std::vector<int32_t> cand_;
  std::thread::id owner_{};
  uint64_t thread_breaks_ = 0;
  bool fast_ = false;
  uint64_t guest_frame_ = ~0ull, frame_ = 1, inherited_ = 0, fresh_ = 0, followed_ = 0;
  CrowdNodeKnown nk_, nk_prev_;
  CrowdFlatKnown fk_, fk_prev_;
  std::vector<Entry> now_, last_, last2_;
};

inline CrowdRankTracker g_crowd_rank;

}  // namespace rdr_adapter
