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
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rdr_adapter {

// The rank of a position by hash alone (the old rule; what the SDK's CrowdRankOf returns with tracking off).
inline uint32_t CrowdRankKey(uint32_t w3, uint32_t w7, uint32_t w11) { return w3 * 0x9E3779B1u ^ w7 * 0x85EBCA77u ^ w11 * 0xC2B2AE3Du; }
inline int CrowdRankHash(uint32_t key) {
  uint32_t h = key;
  h ^= h >> 16, h *= 0x7FEB352Du, h ^= h >> 15, h *= 0x846CA68Bu, h ^= h >> 16;
  return int(h % 100u);
}

class CrowdRankTracker {
 public:
  static constexpr float kTrackRadius = 24.0f;
  bool guard = true;    // false: rotate on every call (the first lane 234 version; the burst plant)
  bool enabled = true;  // false: the rank of the exact position (the plant, and --wwe2k14_rdr_density_track=false)

  // Once per GUEST frame (Bind passes frame.header.guest_frame: the parts of a frame split at a readback, lasting-only binds of skipped frames and picture isolates repeat it or
  // test nothing, and must not rotate the memory), before its first rank call. A frame in which nothing was tested leaves the memory as it is.
  void NextFrame(uint64_t guest_frame) {
    if (!enabled) return;
    if (guard && (guest_frame == guest_frame_ || (guest_frame_ = guest_frame, now_.empty()))) return;
    guest_frame_ = guest_frame;
    last2_.swap(last_);
    last_.swap(now_);
    now_.clear();
    if (++frame_ % 600 == 0) {  // positions not seen for a window go (the map must not grow over a long match)
      known_prev_.swap(known_);
      known_.clear();
    }
  }

  int Rank(uint32_t w3, uint32_t w7, uint32_t w11) {
    const uint32_t key = CrowdRankKey(w3, w7, w11);
    const int hashed = CrowdRankHash(key);
    if (!enabled) return hashed;
    Known* known = nullptr;
    if (auto it = known_.find(key); it != known_.end()) {
      known = &it->second;
    } else if (auto it2 = known_prev_.find(key); it2 != known_prev_.end()) {
      known = &(known_[key] = it2->second);
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
    float best = kTrackRadius * kTrackRadius;
    int rank = hashed;
    bool found = false;
    for (const std::vector<Entry>* list : {&last_, &last2_}) {
      for (const Entry& e : *list) {
        const float dx = e.x - x, dy = e.y - y, dz = e.z - z, d = dx * dx + dy * dy + dz * dz;
        if (!(d > 0.0f && d < best)) continue;  // (the distance first: the lookup below only for the few that are near)
        if (auto kt = known_.find(e.key); kt != known_.end() && kt->second.last == frame_) continue;
        best = d, rank = e.rank, found = true;
      }
      if (found) break;
    }
    ++(found ? inherited_ : fresh_);
    known_[key] = {uint8_t(rank), frame_};
    now_.push_back({x, y, z, uint8_t(rank), key});
    return rank;
  }

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
  struct Known {
    uint8_t rank = 0;
    uint64_t last = 0;  // the frame this position was last tested
  };
  static float F(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
  }
  uint64_t guest_frame_ = ~0ull, frame_ = 1, inherited_ = 0, fresh_ = 0, followed_ = 0;
  std::unordered_map<uint32_t, Known> known_, known_prev_;
  std::vector<Entry> now_, last_, last2_;
};

inline CrowdRankTracker g_crowd_rank;

}  // namespace rdr_adapter
