// The weapon manager's slot table, read from guest memory (Session 103, lane PW-1; contract src\renderer_scene\weapon_rows.h, evidence docs\research\s103\PW_adversary.md section 2).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. PURE: no game, SDK or renderer includes; a Mem type with `bool read32(uint32_t addr, uint32_t& v) const` (false = the address is not
// readable; v is then left alone). In the game the glue's guest reader, in tools\weapons\weapon_slots_test.cpp forged memory (the same code runs in both, as crowd_mgr_reader.h).
//
// THE ONE READER of the slot table (the PW-1/2 review, S103): lane PW-2's per-call tracker names its objects from ReadSlots too (weapon_rows_core.h); no other file may use kOffSlotId / kOffSlotMain / kOffSlotSecondary
// (tools\weapons\weapon_lints.py). The slot rules live here once: id < 0 or main == 0 = an empty slot (no row); any unreadable word = no rows at all (Unreadable, never a half table); a count above the game's cap = not a
// table of this manager (Refused, no rows); a manager pointer below 0x10000 = no manager.
// Read EVERY frame, never cached across frames: a reload (sub_82592608 -> sub_82590ED8) frees every slot model and resets the table, and a freed address can come back for another id.
// The layout is a Layout value (the shipped one is kLayout, the contract's constants); the unit test plants a wrong offset / stride / cap in a copy and requires the rows to differ.
#pragma once
#include <cstdint>
#include <utility>
#include <vector>

#include "../renderer_scene/weapon_rows.h"

namespace rdr_scene::live::weapons {

using rdr_scene::weapons::WeaponSlot;

struct Layout {
  uint32_t mgr_global = rdr_scene::weapons::kMgrGlobal;
  uint32_t off_count = rdr_scene::weapons::kOffSlotCount;
  uint32_t off_id = rdr_scene::weapons::kOffSlotId;               // + 4i
  uint32_t off_main = rdr_scene::weapons::kOffSlotMain;           // + 4i
  uint32_t off_secondary = rdr_scene::weapons::kOffSlotSecondary; // + stride_secondary * i + 4k
  uint32_t stride_secondary = 16;                                 // bytes per slot block {data0, data1, obj0, obj1}; the MODEL objects are the last two words (k < kMaxSecondary = 2)
  uint32_t sec_count = rdr_scene::weapons::kMaxSecondary;         // secondary models per slot; a twin sets 4 (it then reads the NEXT slot's data words: the first contract's mistake)
  uint32_t cap = rdr_scene::weapons::kMaxSlots;                   // the game's own table limit: a count above it is not a table (refused)
  uint32_t min_mgr = 0x10000;                                     // a manager pointer below this is no manager
  uint32_t vtable = rdr_scene::weapons::kSkinnedModelVtable;
};
inline constexpr Layout kLayout{};

enum class Why : uint8_t { None = 0, NoManager, Unreadable, Refused };

struct Stats {          // out: what one read saw (the census sums these)
  uint32_t mgr = 0;
  uint32_t count = 0;        // the slot count word, as read (before the clamp)
  uint32_t scanned = 0;      // slots examined (count clamped to the cap)
  uint32_t empty = 0;        // id < 0 or main == 0: no row
  uint32_t bad_vtable = 0;   // rows written with flags bit 0 clear
  uint32_t refused = 0;      // 1 when the count word was above the cap (no rows: not a table of this manager)
};

// Rows in slot order. Returns None with `out` filled (possibly empty: no slots), NoManager when the global is below min_mgr (0 = none), Refused when the count is above the cap, Unreadable when a manager word could not be
// read (out is then empty: a half-read table is never handed on).
template <class M>
inline Why ReadSlots(const M& mem, std::vector<WeaponSlot>& out, Stats* st = nullptr, const Layout& L = kLayout) {
  out.clear();
  Stats local;
  Stats& s = st ? *st : local;
  s = Stats{};
  uint32_t mgr = 0;
  if (!mem.read32(L.mgr_global, mgr)) return Why::Unreadable;
  if (mgr < L.min_mgr) return Why::NoManager;
  s.mgr = mgr;
  uint32_t n = 0;
  if (!mem.read32(mgr + L.off_count, n)) return Why::Unreadable;
  s.count = n;
  if (n > L.cap) return s.refused = 1, Why::Refused;   // not a table of this manager: nothing is read past it (a count of 60 would run into the model words)
  s.scanned = n;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t idw = 0, main = 0;
    if (!mem.read32(mgr + L.off_id + 4 * i, idw) || !mem.read32(mgr + L.off_main + 4 * i, main)) return out.clear(), Why::Unreadable;
    const int32_t id = int32_t(idw);
    if (id < 0 || main == 0) { ++s.empty; continue; }
    WeaponSlot r;
    r.id = id;
    r.slot = i;
    r.main = main;
    uint32_t vt = 0;
    if (mem.read32(main, vt) && vt == L.vtable) r.flags |= 1u;
    else ++s.bad_vtable;
    for (uint32_t k = 0; k < L.sec_count; ++k) {
      uint32_t sec = 0;
      if (!mem.read32(mgr + L.off_secondary + L.stride_secondary * i + 4 * k, sec)) return out.clear(), Why::Unreadable;
      if (k < rdr_scene::weapons::kMaxSecondary) r.secondary[k] = sec;
      else if (k - rdr_scene::weapons::kMaxSecondary < 2) r.reserved[k - rdr_scene::weapons::kMaxSecondary] = sec;   // only a twin's sec_count > 2 gets here; `reserved` is 0 in every shipped row
    }
    out.push_back(r);
  }
  return Why::None;
}

// S103 LDR (COV10 B2; review fixes 3 / 4): the belt (weapon id 14) is nested under a sub-key the game takes from the TITLE in SetWeaponData (asm 0x82590094..0x825900D8): `o ? [o+8] : (M ? [M+189208] : 1)`,
// M = [0x836F2D88], o = [[0x8376AAF0] + 72428], low 16 bits. The game reads it ONCE, at match load, so it is cached per belt object address (BeltKeyCache) and dropped when the object leaves the slots.
// An unreadable word keeps the value so far (the loader falls back to sub-key 0 for a key the pac lacks, as the game).
template <class ReadFn>
inline uint32_t BeltSubKey(ReadFn& read_word) {
  namespace w = rdr_scene::weapons;
  uint32_t sub = 1, m = 0, v = 0, g = 0, o = 0;
  if (read_word(w::kTitleMgrGlobal, m) && m >= 0x10000 && read_word(m + w::kTitleMgrOffKey, v)) sub = v;
  if (read_word(w::kSettingsGlobal, g) && g >= 0x10000 && read_word(g + w::kSettingsOffTitleObj, o) && o >= 0x10000 && read_word(o + w::kTitleObjOffKey, v)) sub = v;
  return sub & 0xFFFFu;
}
struct BeltKeyCache {
  std::vector<std::pair<uint32_t, uint32_t>> e;   // (belt main model address, sub-key)
  uint64_t reads = 0;                              // sub-key reads (the unit checks it stays at one per belt object)
};
// Rewrites every id-14 row to BeltId(sub-key) (the section-59 row and the tracker's named list both come through here), reading the sub-key once per belt object; entries of objects no longer in the slots are dropped.
template <class ReadFn>
inline void ApplyBeltIds(ReadFn& read_word, BeltKeyCache& cache, std::vector<WeaponSlot>& slots) {
  std::vector<std::pair<uint32_t, uint32_t>> keep;
  for (WeaponSlot& s : slots) {
    if (s.id != rdr_scene::weapons::kWeaponIdBelt) continue;
    uint32_t sub = 0;
    bool have = false;
    for (const auto& c : cache.e)
      if (c.first == s.main) sub = c.second, have = true;
    if (!have) sub = BeltSubKey(read_word), ++cache.reads;
    keep.push_back({s.main, sub});
    s.id = rdr_scene::weapons::BeltId(sub);
  }
  cache.e.swap(keep);
}

}  // namespace rdr_scene::live::weapons
