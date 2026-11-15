// S102 lane T11b (switch wwe2k14_rdr_scene_strings_once): the strings every frame's scene record repeats are made once.
//
// What it removes: the live record's strings (the crowd's ~1,240 member ids, the arena's slot ids) were interned again every frame: per string an FNV-1a over 25-30
// bytes, an open-addressing probe and a `memcmp` (R20 section 5a: 0.92 ms of the crowd contributor, the arena's `to_string` / concatenations / heap map the other 0.5).
// The ids do not change from frame to frame, so they live in ONE process-wide pool, made when a string is first seen: a blob (NUL-separated, like the record's own) and
// the flat index over it (the same slots SceneTables::UseFlatStringIndex builds). A frame's table STARTS as a copy of the pool (SceneTables::SeedStrings: one blob copy and
// one slots copy, no hashing); a contributor then writes a member's id as the pool field it kept the last time (`PooledField`), with no AddString at all. A string the pool does
// not hold yet is added the old way this frame (AddString into the seeded table: equal strings still share a field, the pool's strings are found through the copied index) and
// queued; the pool takes the queue at the START of the next frame (Commit), never in the middle of one, so a field a contributor holds is always inside the table it writes.
//
// The record's string blob is therefore the pool's strings first (all of them, used this frame or not) and then this frame's own: the same strings, the same Str() of every
// name field, other field numbers. SerializeRecord writes a seeded table compacted (SceneTables::CompactStrings), so no file carries the pool.
// Threading: the writer's thread only (live::EndFrame holds the writer mutex over Commit, the seed and every Emit); the offline tools drive it from one thread.
// No game or SDK includes.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rdr_scene {

// The flat index's hash (FNV-1a, folded): one definition, shared by record_io.cpp's flat index and the pool, so the pool's slots ARE a valid index of the table seeded from it.
inline uint64_t FlatStringHash(const char* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ uint8_t(p[i])) * 0x100000001B3ull;
  return h ^ (h >> 32);
}

// The fallback of the switch for tools that have no cvar table (the game's table overrides it once it is live).
inline std::atomic<bool> g_scene_strings_once{false};

class StringPool {
 public:
  static constexpr size_t kMaxBytes = 384u << 10;   // a pool that would pass this stops taking strings (counted); the old path serves them. Under ~512 KB a seeded blob stays in the heap (no fresh VirtualAlloc + page faults a frame)
  static constexpr size_t kMaxStrings = 1u << 14;
  static constexpr size_t kHeadroom = 512;       // strings a seeded table can add before its copied index would have to grow

  struct Stats {   // plain counters: the writer's thread only; the census line reads them every 600 frames
    uint64_t frames = 0, seeded = 0;
    uint64_t crowd_fields = 0, crowd_fallback = 0, crowd_stale = 0;
    uint64_t arena_fields = 0, arena_fallback = 0;
    uint64_t pending_max = 0, refused_full = 0, resets = 0;
  };

  StringPool() : id_(NextId()) { slots_.assign(64, 0u); }
  StringPool(const StringPool&) = delete;
  StringPool& operator=(const StringPool&) = delete;

  uint32_t id() const { return id_; }           // another pool = another set of fields: a contributor's kept fields are dropped when this changes
  uint32_t serial() const { return serial_; }   // +1 for every Commit that added a string (starts at 1)
  const std::string& Blob() const { return blob_; }
  const std::vector<uint32_t>& Slots() const { return slots_; }
  size_t Count() const { return count_; }
  Stats stats;

  // The field (offset + 1) of a committed string, 0 when the pool does not hold it.
  uint32_t Find(std::string_view s) const { return s.empty() ? 0u : Find(s, FlatStringHash(s.data(), s.size())); }
  uint32_t Find(std::string_view s, uint64_t h) const {   // (the hash made once by a caller that also probes its own slots)
    if (s.empty()) return 0;
    const size_t mask = slots_.size() - 1;
    for (size_t i = size_t(h) & mask; slots_[i]; i = (i + 1) & mask) {
      const size_t at = slots_[i] - 1;
      if (at + s.size() < blob_.size() && blob_.compare(at, s.size(), s) == 0 && blob_[at + s.size()] == '\0') return slots_[i];
    }
    return 0;
  }

  // The arena the record belongs to (the arena contributor tells every record): another arena starts a NEW pool at the next Commit (blob and index emptied, a new id, so every contributor drops
  // the fields it kept). The crowd ids of a past arena / match would otherwise stay for the whole run (R20 review F2: the pool is bounded by the arena, not by the session).
  void NoteArena(int arena) {
    if (arena == arena_seen_) return;
#ifdef RDR_CROWD_IDS_PLANTS
    if (plant_no_arena_reset) { arena_seen_ = arena; return; }
#endif
    if (arena_seen_ != kNoArena) reset_pending_ = true;
    arena_seen_ = arena;
  }
#ifdef RDR_CROWD_IDS_PLANTS
  bool plant_no_arena_reset = false;   // test only: the pool keeps every arena's strings
#endif

  // Asks for a string to be in the pool from the next Commit. False = the pool is full (the caller keeps using AddString for it).
  bool Queue(std::string_view s) {
    if (s.empty()) return true;
    if (blob_.size() + pending_.size() + s.size() + 1 > kMaxBytes || count_ + pending_count_ >= kMaxStrings) {
      ++stats.refused_full;
      return false;
    }
    pending_.append(s.data(), s.size());
    pending_.push_back('\0');
    ++pending_count_;
    if (pending_count_ > stats.pending_max) stats.pending_max = pending_count_;
    return true;
  }

  // Takes the queued strings into the blob and the index (equal strings once). The start of a frame, before any table is seeded. True when anything was added.
  bool Commit() {
    bool added = false;
    if (reset_pending_) {   // a new arena: a new pool (the strings queued in the frame of the change go into it)
      reset_pending_ = false;
      blob_.clear();
      slots_.assign(64, 0u);
      count_ = 0;
      id_ = NextId();
      ++serial_;
      added = true;
      ++stats.resets;
    }
    if (pending_.empty()) return added;
    size_t pos = 0;
    while (pos < pending_.size()) {
      const size_t end = pending_.find('\0', pos);
      const std::string_view s(pending_.data() + pos, end - pos);
      pos = end + 1;
      if (Find(s)) continue;
      while ((count_ + 1 + kHeadroom) * 2 > slots_.size()) Grow();   // (room kept for the strings a frame adds on top of a seeded copy: no rehash in the middle of a record)
      const uint32_t field = uint32_t(blob_.size() + 1);
      blob_.append(s.data(), s.size());
      blob_.push_back('\0');
      Insert(field, FlatStringHash(s.data(), s.size()));
      added = true;
    }
    pending_.clear();
    pending_count_ = 0;
    if (added) ++serial_;
    return added;
  }

 private:
  static uint32_t NextId() {
    static std::atomic<uint32_t> n{1};
    return n.fetch_add(1, std::memory_order_relaxed);
  }
  void Insert(uint32_t field, uint64_t h) {
    const size_t mask = slots_.size() - 1;
    for (size_t i = size_t(h) & mask;; i = (i + 1) & mask)
      if (!slots_[i]) {
        slots_[i] = field;
        ++count_;
        return;
      }
  }
  void Grow() {
    std::vector<uint32_t> old;
    old.swap(slots_);
    slots_.assign(old.size() * 2, 0u);
    count_ = 0;
    for (uint32_t f : old)
      if (f) {
        const char* q = blob_.data() + (f - 1);
        size_t n = 0;
        while (q[n]) ++n;
        Insert(f, FlatStringHash(q, n));
      }
  }
  static constexpr int kNoArena = -0x7FFFFFFF;
  int arena_seen_ = kNoArena;
  bool reset_pending_ = false;
  uint32_t id_;
  uint32_t serial_ = 1;
  std::string blob_;
  std::vector<uint32_t> slots_;   // fields (0 = empty), a power of two long, at most half full: the layout SceneTables' flat index uses
  size_t count_ = 0;
  std::string pending_;           // NUL-separated, not yet in the blob
  size_t pending_count_ = 0;
};

// What a contributor keeps for one of its strings: the pool field once the pool holds it. `queued_at` = the pool serial at which it was asked for (so it asks once per pool state).
struct PooledField {
  uint32_t field = 0;
  uint32_t queued_at = 0;
};

// The string's pool field, or 0 when the pool does not hold it yet (the caller adds it the old way this frame). `make` builds the text; it runs only when the field is not kept.
template <class Make>
inline uint32_t PooledFieldOf(StringPool& pool, PooledField& pf, Make&& make) {
  if (pf.field) return pf.field;
  const uint32_t ser = pool.serial();
  if (pf.queued_at == ser) return 0;   // asked at this pool state already (pending, or the pool is full)
  pf.queued_at = ser;
  const std::string text = make();
  if (const uint32_t f = pool.Find(text)) return pf.field = f;
  pool.Queue(text);
  return 0;
}

}  // namespace rdr_scene
