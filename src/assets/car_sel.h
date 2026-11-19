// S103 CAR (BCA G1, docs\research\s103\BCA.md; docs\formats\create_an_arena.md): a Create-an-Arena venue (arena ids 200-249) is not a pac\bg\bgNN.pac. The game's loader (sub_8342AA30) opens four PACHs by the
// name "/%s/%04d": CARG/0000 (pac\car\parts_ring.pac), CAEN/n (parts_ent.pac), CASD/n (parts_studium.pac) and the light set CARL/n (car_light.pac, fallback 0000); the three scenery PACHs are ordinary bg PACHs
// that share one slot space (CASD slots 0-199, CAEN 200-399, CARG the ring-side slots 25-94 and the ring keys 952 / 956 / 980 / 1017-1083). This header merges them into ONE synthetic pac whose single PACH has the
// ids assets::LoadArena reads from a bg pac, so every reader that opens the arena through load_detail::OpenCached (the arena loader, the crowd, the ring, the screens, the flash, the blood) gets the venue
// without a second loader. THIS header holds the selection (the three part indices), its registry and the census: no pac / stb dependency (the stage_fx contributor and the record rows include it); the merge is car_arena.h.
//
// Merge rules (all by the files, BCA / asset.db of the player's own pacs):
//   * ids <400, 800-815, 995-999, 1012, 2xxxx, 31000-32010, 4xxxx, 5xxxx, 6xxxx are unique across the three PACHs (a duplicate is counted: first wins, order CASD, CAEN, CARG);
//   * texture directories: CASD 400 -> 400, CAEN 401 -> 401, CARG 402 -> 402, CAEN 400 -> 403, CARG 400 -> 404 (LoadArena reads 400-404 for a created arena);
//   * the light entries 30000 / 30002 / 30003: the CARL PACH's (BCA: the light set the stage takes), the CASD PACH's only when CARL has none; 30001 (the stage motions) is CAEN's;
//   * 50001 (the slot text): the three texts joined, CARG then CAEN then CASD.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>


namespace assets {
namespace car {

inline constexpr int kFirstId = 200, kLastId = 249;
inline bool IsCustomId(int id) { return id >= kFirstId && id <= kLastId; }
inline constexpr const char* kPacRing = "pac\\car\\parts_ring.pac";
inline constexpr const char* kPacEnt = "pac\\car\\parts_ent.pac";
inline constexpr const char* kPacStudium = "pac\\car\\parts_studium.pac";
inline constexpr const char* kPacLight = "pac\\car\\car_light.pac";

// The three part indices of a created arena (read from the game at the latch: stage_fx_contrib.h) and the real bg arena it borrows ([stage + 21192], arena_lm).
struct Sel {
  bool valid = false;
  uint32_t caen = 0, casd = 0, carl = 0;
  int bg = -1;
  int arena = -1;   // S103 CAR fix 4: the created arena id (200-249) the stage was built for ([stage + 1216]); a reader of another id never takes this selection
  std::string Key() const {
    char b[72];
    std::snprintf(b, sizeof b, "car:%d:%u/%u/%u/%d", arena, caen, casd, carl, bg);
    return b;
  }
  bool operator==(const Sel& o) const { return valid == o.valid && arena == o.arena && caen == o.caen && casd == o.casd && carl == o.carl && bg == o.bg; }
  bool operator!=(const Sel& o) const { return !(*this == o); }
};

// The current selection: a thread-local override (the loader sets it for the load it runs) over the process-wide one (published by the applier before it asks for the arena).
inline std::mutex& SelMu() {
  static std::mutex m;
  return m;
}
inline Sel& GlobalSel() {
  static Sel s;
  return s;
}
inline Sel*& ThreadSelPtr() {
  thread_local Sel* p = nullptr;
  return p;
}
inline Sel Current() {
  if (const Sel* t = ThreadSelPtr()) return *t;
  std::lock_guard<std::mutex> lk(SelMu());
  return GlobalSel();
}
inline void Publish(const Sel& s) {
  std::lock_guard<std::mutex> lk(SelMu());
  GlobalSel() = s;
}
inline void Clear() { Publish(Sel()); }
struct ScopedThreadSel {
  Sel s;
  Sel* prev;
  explicit ScopedThreadSel(const Sel& x) : s(x), prev(ThreadSelPtr()) { ThreadSelPtr() = &s; }
  ~ScopedThreadSel() { ThreadSelPtr() = prev; }
  ScopedThreadSel(const ScopedThreadSel&) = delete;
  ScopedThreadSel& operator=(const ScopedThreadSel&) = delete;
};

// "pac\bg\bg200.pac" -> 200 when it is a created arena's name (what ArenaRel gives).
inline bool CustomIdOfRel(const std::string& rel, int& id) {
  int n = 0;
  static const char kPre[] = "pac\\bg\\bg";   // a hand parse: MSVC's CRT marks sscanf deprecated and the units build with -Werror (the same fix as named_load_fail.h, LNT2)
  const size_t pl = sizeof(kPre) - 1;
  if (rel.compare(0, pl, kPre) != 0) return false;
  size_t i = pl, digits = 0;
  for (; i < rel.size() && rel[i] >= '0' && rel[i] <= '9' && digits < 6; ++i, ++digits) n = n * 10 + (rel[i] - '0');
  if (digits == 0 || !IsCustomId(n)) return false;
  id = n;
  return true;
}

// What a merge found (the census / the proof).
struct MergeInfo {
  uint32_t casd = 0, caen = 0, carl = 0;           // the indices actually used (after the fallback to 0)
  bool casd_fallback = false, caen_fallback = false, carl_fallback = false;   // the asked index did not exist
  bool have_casd = false, have_caen = false, have_carg = false, have_carl = false;
  uint32_t slots_casd = 0, slots_caen = 0, slots_carg = 0;   // entries with an id < 400, by the PACH they came from
  uint32_t slot_min_casd = 0xFFFF, slot_max_casd = 0, slot_min_caen = 0xFFFF, slot_max_caen = 0, slot_min_carg = 0xFFFF, slot_max_carg = 0;
  uint32_t entries = 0, duplicates = 0, lights_from_carl = 0, lights_from_casd = 0;
  std::map<uint32_t, uint8_t> part_of;   // S103 CAR fix 2: merged entry id -> the part it came from (0 CASD, 1 CAEN, 2 CARG, 3 CARL): the texture set a mesh of that entry uses
  bool text_joined = false;
};

// Census (the loads this process made).
// The part map of the merged pac OpenCached last built (written under the loader mutex by the one build, read by LoadArena under the same): which part a slot / key entry came from. Unknown id: the band by number.
inline std::map<uint32_t, uint8_t>& PartMap() {
  static std::map<uint32_t, uint8_t> m;
  return m;
}
inline int PartOfKey(uint32_t key) {
  const auto it = PartMap().find(key);
  if (it != PartMap().end()) return it->second;
  return key < 200 ? 0 : key < 400 ? 1 : 2;
}

struct Counters {
  std::atomic<uint64_t> builds{0}, build_failed{0}, missing_part{0}, fallback_index{0};
  std::atomic<uint32_t> last_casd{0}, last_caen{0}, last_carl{0}, last_slots_casd{0}, last_slots_caen{0}, last_slots_carg{0}, last_entries{0};
};
inline Counters& Stats() {
  static Counters c;
  return c;
}
inline void NoteBuilt(const MergeInfo& mi) {
  Counters& c = Stats();
  c.builds.fetch_add(1, std::memory_order_relaxed);
  c.missing_part.fetch_add(uint64_t(!mi.have_casd) + uint64_t(!mi.have_caen) + uint64_t(!mi.have_carg) + uint64_t(!mi.have_carl), std::memory_order_relaxed);
  c.fallback_index.fetch_add(uint64_t(mi.casd_fallback) + uint64_t(mi.caen_fallback) + uint64_t(mi.carl_fallback), std::memory_order_relaxed);
  c.last_casd.store(mi.casd), c.last_caen.store(mi.caen), c.last_carl.store(mi.carl);
  c.last_slots_casd.store(mi.slots_casd), c.last_slots_caen.store(mi.slots_caen), c.last_slots_carg.store(mi.slots_carg), c.last_entries.store(mi.entries);
}

}  // namespace car
}  // namespace assets
