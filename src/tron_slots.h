// wwe2k14 - S110 TRONSLOT: custom titantrons in the game's MOVIE list (docs\research\s110\TRONSLOT.md, parts 2 and 4). The rule (the band,
// custom.txt, which videos count) is src\tron_slots_core.h; this file is the game side: one row per FILLED slot appended to list 0 (MOVIE)
// of /ENEX/LSD at run time, the way src\music_slots.h appends new songs to list 2 (MUSIC).
//
// List 0 (MOVIE): object +128 = records pointer, +208 = u16 record count (the same layout as every list: sub_82928B08, music_slots.h). A record
// is 8 bytes, big-endian: s16 value (the movie id), s16 unlock (-1 = always shown), u32 name text id. Record 0 is 999 / text 8030 (the
// note, measured on pack 5's list): anything else and the list is left alone (another version), said once. Rows added: {N, -1,
// 0x7C020000 + N}; the name is answered by the GetString hook (src\gfx_menu.h) through NameFor. The game's own records and raw copy stay.
// The rows are listed whenever the mods folder is known (mods::g_dir: any of USE MY VIDEOS / SUPERSTARS / LIGHTING) and the slot is FILLED
// (line and file), also with USE MY VIDEOS off (main session D1): the entrance editor's save writes the value of the row it shows
// (sub_83352EB8 +7C..+A0; a value with no row shows row 0, sub_83355A30 +608), so a missing row would overwrite N with 999 for good. With
// videos off the redirect plays nothing (mods::FindTron), so that entrance's big screen is idle (one INFO line). A slot whose file was
// deleted has no row (the owner's "only filled ones"). Disc wrestlers' editors show the rows too: list 0 is one list for every character.
#pragma once

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "guest_read.h"
#include "mods.h"
#include "tron_slots_core.h"

namespace wwe2k14::tron_slots {

using core_slot = tron_slots_core::Slot;
constexpr uint32_t kTextBase = 0x7C020000u;  // name text id = kTextBase + N (music's is 0x7C010000, gfx_menu's 0x7C0000xx)
constexpr uint32_t kMovieList = 0;           // list index in /ENEX/LSD
constexpr uint32_t kOffRecords = 128, kOffCount = 208;
constexpr int kFirstRecordValue = 999, kFirstRecordText = 8030;

inline bool IsSlotId(int n) { return tron_slots_core::IsSlotId(n); }
// The rows exist when the mods folder is known (not gated by USE MY VIDEOS: see the top of this file).
inline bool Active() { return !mods::g_dir.empty(); }

inline std::mutex g_mutex;
inline std::map<int, std::string> g_names;               // N -> shown name (upper case, the list's style), the last scan's filled slots
inline std::map<std::string, uint32_t> g_guest_names;    // shown name -> guest string
inline std::map<std::string, uint32_t> g_guest_arrays;   // record bytes -> guest copy (reused while unchanged)
inline bool g_warned_layout = false, g_warned_off = false;

inline std::string Upper(std::string s) {
  for (auto& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline void PutBe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
inline void PutBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

inline uint32_t AllocGuestBytes(const void* data, uint32_t size) {
  const uint32_t a = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  if (a) std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(a), data, size);
  return a;
}

// The text manager hook (src/gfx_menu.h): the guest string for one of our name ids, else 0.
inline uint32_t NameFor(uint32_t text_id) {
  if (text_id < kTextBase + 823 || text_id > kTextBase + 949 || !Active()) return 0;
  const int n = int(text_id - kTextBase);
  if (!IsSlotId(n)) return 0;
  std::lock_guard lock(g_mutex);
  auto it = g_names.find(n);
  if (it == g_names.end()) return 0;
  uint32_t& g = g_guest_names[it->second];
  if (!g) g = AllocGuestBytes(it->second.c_str(), uint32_t(it->second.size() + 1));
  return g;
}

// After the load callback (sub_82928B08, music_slots.h's hook): append one record per FILLED slot to list 0. `self` = the list object.
inline void OnListsLoaded(uint8_t* base, uint32_t self) {
  if (!self || !Active()) return;
  const auto dir = mods::g_dir / "titantron";
  const uint32_t count_at = self + kOffCount + 2 * kMovieList, recs_at = self + kOffRecords + 4 * kMovieList;
  if (!GuestWordReadableFast(count_at & ~3u) || !GuestWordReadableFast(recs_at)) return;
  const uint16_t count = Be16(rex::memory::GuestPtr(base, count_at));
  const uint32_t recs = Be32(rex::memory::GuestPtr(base, recs_at));
  if (!recs || count < 1 || count > 4000 || !GuestWordReadableFast(recs & ~3u) ||
      !GuestWordReadableFast((recs + 8u * count - 1) & ~3u)) {
    return;
  }
  const uint8_t* r = rex::memory::GuestPtr(base, recs);
  if (Be16(r) != kFirstRecordValue || Be32(r + 4) != kFirstRecordText) {  // the layout check: record 0 = 999 / 8030
    if (!g_warned_layout) {
      g_warned_layout = true;
      REXLOG_WARN("[wwe2k14][tron_slots] list {} does not start with 999 / text 8030 ({} / {}): left as it is", kMovieList, Be16(r), Be32(r + 4));
    }
    return;
  }
  const auto lib = tron_slots_core::ReadSlots(dir, false);  // (review: no unlisted-video scan at every list load)
  if (!mods::g_on && !lib.listed.empty() && !g_warned_off) {
    g_warned_off = true;
    REXLOG_INFO("[wwe2k14][tron_slots] USE MY VIDEOS is off: {} custom tron(s) stay in the MOVIE list (an entrance editor save keeps them), "
                "but their videos are not played: the big screen is idle for a wrestler holding one",
                lib.listed.size());
  }
  std::map<int, std::string> names;
  std::vector<int> add;
  for (const core_slot& s : lib.listed) {
    bool dup = false;  // a stock record with this value already (the band is free on the disc and the packs: never seen)
    for (uint32_t i = 0; i < count && !dup; ++i) dup = int16_t(Be16(r + 8 * i)) == s.id;
    if (dup) continue;
    names[s.id] = Upper(s.name);
    add.push_back(s.id);
  }
  {
    std::lock_guard lock(g_mutex);
    g_names = std::move(names);
  }
  if (add.empty()) return;
  std::string bytes(reinterpret_cast<const char*>(r), 8u * count);
  bytes.resize(8u * (count + add.size()));
  for (size_t i = 0; i < add.size(); ++i) {
    uint8_t* p = reinterpret_cast<uint8_t*>(bytes.data()) + 8u * (count + i);
    PutBe16(p, uint16_t(add[i]));
    PutBe16(p + 2, 0xFFFF);
    PutBe32(p + 4, kTextBase + uint32_t(add[i]));
  }
  std::lock_guard lock(g_mutex);
  uint32_t& copy = g_guest_arrays[bytes];
  const bool fresh = !copy;
  if (!copy) copy = AllocGuestBytes(bytes.data(), uint32_t(bytes.size()));
  if (!copy) return;
  PutBe32(rex::memory::GuestPtr(base, recs_at), copy);
  PutBe16(rex::memory::GuestPtr(base, count_at), uint16_t(count + add.size()));
  if (fresh)
    REXLOG_INFO("[wwe2k14][tron_slots] MOVIE list: {} stock + {} custom tron(s) (object 0x{:08X}, records 0x{:08X})", count, add.size(), self, copy);
}

}  // namespace wwe2k14::tron_slots
