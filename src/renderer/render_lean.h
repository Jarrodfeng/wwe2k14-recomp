// S102 RT1 (docs\renderer\RENDER_LEAN.md): per-frame work on our render thread whose answer does not change, one switch `wwe2k14_rdr_render_lean` as a bit mask
// (1 = Q5 texture descriptor sets kept across frames, 2 = Q7 per-frame facts on the hooks side keyed by the scene record, 4 = Q9 hand-off pieces).
// Default 0 = today's code. This header holds the switch globals, the census counters (relaxed atomics in fixed slots: nothing is built per call) and the PURE
// texture-set cache the renderer and tools\render_lean\lean_test.cpp share. No Vulkan types here: handles are opaque 64-bit words.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rdr {

constexpr int kLeanSets = 1, kLeanFacts = 2, kLeanHandoff = 4;
// The render thread's copy (bits 1 and 4): stored per frame by the live sink / the split record stage (stage_split.h Packet::render_lean) / rplay --render-lean.
inline std::atomic<int> g_render_lean{0};
// The hooks side's copy (bits 2 and 4): read on the bind thread, stored by the live sink before the frame is bound. rplay / fview have no hooks of their own to flip.
inline std::atomic<int> g_lean_hooks{0};
// Planted faults (tools only): 1 = a destroyed image's cached sets are NOT evicted; 2 = the hooks' record-generation key ignores a changed record;
// 3 = the staging pool hands a buffer back while its copy is still in flight.
inline std::atomic<int> g_lean_plant{0};
// Tool-only knobs of the Q5 proof (rplay only; the game never sets them): capacity of the kept-set cache (0 = the default), the idle age that frees a set when the cache is full (0 = 600),
// every Nth frame one sampled view is replaced under the cache (a "view change"; 0 = never), and the audit of the cache against the live views every N frames (0 = every 600).
inline std::atomic<int> g_lean_capacity{0}, g_lean_sweep_age{0}, g_lean_churn_every{0}, g_lean_audit_every{0};

enum LeanCount : int {
  kLcSetHit, kLcSetHitOld, kLcSetMiss, kLcSetFull, kLcSetCollide, kLcSetEvict,  // Q5
  kLcShadowSkip, kLcShadowRun, kLcWrestlerSkip, kLcWrestlerRun,   // Q7
  kLcChurn, kLcAudit, kLcAuditBad,   // tools only: views replaced under the cache (rplay --lean-churn), audits of the cache, kept sets naming a view of no live image
  kLcStageHit, kLcStageMiss, kLcSetRecordSkip, kLcSetRecordRun, kLcSortSkip, kLcSortRun, kLcNoteFlat,  // Q9
  kLcCount
};
inline std::atomic<uint64_t> g_lean_count[kLcCount];
inline void LeanAdd(LeanCount c, uint64_t n = 1) { g_lean_count[c].fetch_add(n, std::memory_order_relaxed); }
inline uint64_t LeanGet(LeanCount c) { return g_lean_count[c].load(std::memory_order_relaxed); }

namespace lean {

// A texture set's identity: what its descriptors point at (view and sampler of every slot). Equal key = equal descriptor contents.
template <size_t N>
struct SetKey {
  uint64_t view[N];
  uint64_t sampler[N];
  uint64_t Hash() const {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < N; ++i) {
      h = (h ^ view[i]) * 0xFF51AFD7ED558CCDull;
      h ^= h >> 32;
      h = (h ^ sampler[i]) * 0xC4CEB9FE1A85EC53ull;
      h ^= h >> 29;
    }
    return h;
  }
  bool operator==(const SetKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};

// Sets kept across frames, by key. Set = the renderer's descriptor-set handle. Render thread only (no locks).
template <size_t N, class Set>
class SetCache {
 public:
  struct Entry {
    SetKey<N> key;
    Set set;
    uint64_t last_use;
  };
  explicit SetCache(size_t capacity = 4096) : capacity_(capacity) {}
  // Hit: the kept set (its last_use refreshed). nullptr = no such key; `collide` says the slot is held by a different key (do not insert).
  Entry* Find(const SetKey<N>& key, uint64_t hash, uint64_t serial, bool& collide, uint64_t* prev_use = nullptr) {
    collide = false;
    const auto it = map_.find(hash);
    if (it == map_.end()) return nullptr;
    if (!(it->second.key == key)) return collide = true, nullptr;
    if (prev_use) *prev_use = it->second.last_use;
    it->second.last_use = serial;
    return &it->second;
  }
  bool Insert(const SetKey<N>& key, uint64_t hash, Set set, uint64_t serial) {
    if (map_.size() >= capacity_ || map_.count(hash)) return false;
    map_.emplace(hash, Entry{key, set, serial});
    return true;
  }
  // The views of a destroyed image: every set that names one is dropped (its handle goes into `freed`). plant_skip = the stale-set fault.
  size_t EvictViews(const std::vector<uint64_t>& dying, std::vector<Set>& freed, bool plant_skip = false) {
    if (plant_skip || dying.empty() || map_.empty()) return 0;
    size_t n = 0;
    for (auto it = map_.begin(); it != map_.end();) {
      bool uses = false;
      for (size_t i = 0; i < N && !uses; ++i)
        for (uint64_t v : dying) uses |= v != 0 && it->second.key.view[i] == v;
      if (uses) freed.push_back(it->second.set), it = map_.erase(it), ++n;
      else ++it;
    }
    return n;
  }
  // Sets not used for more than `age` frames (called when the cache is full).
  size_t Sweep(uint64_t serial, uint64_t age, std::vector<Set>& freed) {
    size_t n = 0;
    for (auto it = map_.begin(); it != map_.end();) {
      if (serial - it->second.last_use > age) freed.push_back(it->second.set), it = map_.erase(it), ++n;
      else ++it;
    }
    return n;
  }
  size_t size() const { return map_.size(); }
  size_t capacity() const { return capacity_; }
  void set_capacity(size_t c) { capacity_ = c; }
  template <class F>
  void ForEach(F&& f) const {
    for (const auto& kv : map_) f(kv.second);
  }

 private:
  size_t capacity_;
  std::unordered_map<uint64_t, Entry> map_;
};

// Q9: staging buffers kept for the next upload instead of a vkCreateBuffer + memory allocation + bind per UpdateImage. A buffer goes back (Give) only when the render thread has
// drained the slot that carried its copy (the slot's fence has signalled): never while the GPU may still read it. Size classes are powers of two from 4 KiB; `Item` = the buffer
// and its memory (opaque here). Take runs on the uploading thread, Give on the render thread: one mutex, held for a vector push / pop.
// Classes are 1.0, 1.25, 1.5, 1.75 x 2^k from 4 KiB, so a pooled buffer is at most 25 % over the upload (staged_bytes / max_staged count the buffer's size). 0 = not pooled (over 16 MiB).
inline uint64_t StagingClassBytes(uint32_t cls) { return (4ull + ((cls - 1) & 3)) << (10 + ((cls - 1) >> 2)); }   // class 1 = 4 KiB, 2 = 5 KiB, 3 = 6 KiB, 4 = 7 KiB, 5 = 8 KiB ...
inline uint32_t StagingClass(uint64_t bytes) {
  if (bytes == 0 || bytes > (16ull << 20)) return 0;
  for (uint32_t c = 1; c < 60; ++c)
    if (StagingClassBytes(c) >= bytes) return c;
  return 0;
}

template <class Item>
class StagingPool {
 public:
  explicit StagingPool(uint64_t budget_bytes = 48ull << 20) : budget_(budget_bytes) {}
  bool Take(uint32_t cls, Item& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& list = free_[cls];
    if (list.empty()) return false;
    out = list.back();
    list.pop_back();
    held_ -= StagingClassBytes(cls);
    return true;
  }
  // False = the pool is full: the caller destroys the item as before.
  bool Give(uint32_t cls, const Item& item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (held_ + StagingClassBytes(cls) > budget_) return false;
    free_[cls].push_back(item);
    held_ += StagingClassBytes(cls);
    return true;
  }
  template <class F>
  void Drain(F&& destroy) {  // shutdown
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& list : free_) {
      for (const Item& item : list) destroy(item);
      list.clear();
    }
    held_ = 0;
  }
  uint64_t held() const { return held_; }

 private:
  std::mutex mutex_;
  std::vector<Item> free_[64];
  uint64_t held_ = 0, budget_;
};

}  // namespace lean
}  // namespace rdr
