// wwe2k14 - new entrance-music slots (Session 75). Songs in <mods>\music\new\ (mp3 / m4a / wav / flac / wma / aac)
// become new entries at the end of the game's own MUSIC list (Create-an-Entrance), next to the stock themes; no
// wrestler's theme is replaced. Needs the mods folder (--wwe2k14_mods=true); --wwe2k14_music_slots=false turns it
// off. Windows only (the player is Media Foundation, src/entrance_music.h).
//
// How the game lists entrance music (Session 75, coverage build run 045, Create-an-Entrance > SUPERSTAR > AJ LEE >
// EASY CREATION > MUSIC):
// - The lists of Create-an-Entrance (motions, movies, music, ...) are one resource, "/ENEX/LSD", loaded into an object
//   of the class with vtable 0x82047614 (constructor sub_82928818; eight users: Create-an-Entrance, Create-a-Superstar,
//   match loading, ...). Its load callback sub_82928B08 (vtable slot 2; r3 = the object, r4 / r5 = the archive and its
//   size) unpacks 20 lists from a PACH archive (entry ids 0-19). List k: the raw copy at object +48 + 4k (freed by the
//   destructor sub_82928990, the only thing it frees), the record count at +208 + 2k (u16) and the records at
//   +128 + 4k (= raw copy + 2). A record is 8 bytes, byte-swapped to big-endian on load: s16 value, s16 unlock id
//   (-1 = always there), u32 name text id.
// - List 2 is MUSIC (123 records in this version): value = the music id ("BROCK LESNAR" 343, which the game plays
//   as the Wwise event Play_MUS_0343_0_0: sub_82ACFDE0, see src/entrance_music.h), the names are text ids 9301-9437;
//   the first two records are USER PLAYLIST (value 9998, the Xbox music library) and NONE (9999). The menu builds its
//   rows from the records in order (sub_83358B50, r6 = 2 from sub_83355A30 at 0x83355E08).
//
// So: after the load callback ran, list 2's record pointer and count are pointed at a copy of the records (our guest
// memory) with one record per song appended: value = the song's music id (6001 and up; the game's own ids end at
// 904), unlock -1, name = our text id 0x7C010000 + (id - 6000), which the text manager hook in src/gfx_menu.h answers
// with the song's name. The game's own records and raw copy are left as they were (the destructor frees only the raw
// copy). A wrestler given a new song keeps its music id in the save like any stock theme; at his entrance the game
// asks for Play_MUS_6001_0_0, and src/entrance_music.h mutes that (no such Wwise event) and plays the file instead.
//
// Ids stay with their files: <mods>\music\new\slots.txt ("6001<TAB>Kingdom.mp3[<TAB>SHOWN NAME]", written by the
// game, may be edited) gives each file its id the first time it is seen and keeps it, so adding or removing other
// songs never changes which song a wrestler has. A file that is gone keeps its line (and id) but no list entry.
// The shown name is the file name in capitals (the list's style), or the line's third column. The launcher's MUSIC
// tab reads and writes the same file with the same rules (src\launcher\launcher_core.h NewSong: keep them in step).
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "guest_read.h"
#include "mods.h"

REXCVAR_DEFINE_BOOL(wwe2k14_music_slots, true, "wwe2k14",
                    "Session 75: songs in <mods>\\music\\new\\ (mp3, m4a, wav, flac, wma, aac) are added as new entries "
                    "at the end of the game's MUSIC list (Create-an-Entrance), named after the file; a wrestler given "
                    "one plays it at his entrance. Needs --wwe2k14_mods. Ids are kept in <mods>\\music\\new\\slots.txt. "
                    "See src/music_slots.h");

REX_EXTERN(__imp__sub_82928B08);

namespace wwe2k14::music_slots {

constexpr int kFirstId = 6001;               // the game's own music ids end at 904; 9998 / 9999 are USER PLAYLIST / NONE
constexpr int kLastId = 9000;                // the game writes music ids with four digits (%04d)
constexpr uint32_t kTextBase = 0x7C010000u;  // name text ids = kTextBase + (id - 6000); gfx_menu's are 0x7C0000xx
constexpr uint32_t kMusicList = 2;           // list index in /ENEX/LSD
constexpr uint32_t kOffRecords = 128, kOffCount = 208;
constexpr const char* kExts[] = {".mp3", ".m4a", ".wav", ".flac", ".wma", ".aac"};

struct Slot {
  int id = 0;
  std::string file;  // file name in the folder
  std::string name;  // shown name
  bool present = false;
};

inline std::mutex g_mutex;
inline std::map<int, Slot> g_slots;  // by music id (present or not)
inline std::chrono::steady_clock::time_point g_last_scan{};
inline bool g_scanned = false;
inline std::map<std::string, uint32_t> g_guest_names;      // shown name -> guest string
inline std::map<std::string, uint32_t> g_guest_arrays;     // record bytes -> guest copy (reused while unchanged)
inline bool g_warned_layout = false;

inline bool Active() {
#if !defined(_WIN32)
  return false;  // no player there (ANDROID_PLAN F9): no slot would make a sound
#else
  return REXCVAR_GET(wwe2k14_music_slots) && mods::g_on;
#endif
}

inline std::filesystem::path Dir() { return mods::g_dir / "music" / "new"; }

inline std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

inline bool IsSong(const std::filesystem::path& p) {
  const std::string ext = Lower(p.extension().string());
  return std::any_of(std::begin(kExts), std::end(kExts), [&](const char* e) { return ext == e; });
}

// "Kingdom_remix" -> "KINGDOM REMIX": capitals like the stock names, printable ASCII only (the menu font), at most
// 40 characters.
inline std::string CleanName(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '_') c = ' ';
    if (c < 32 || c > 126) continue;
    out += char(std::toupper(static_cast<unsigned char>(c)));
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  if (out.size() > 40) out.resize(40);
  return out.empty() ? "NEW SONG" : out;
}
inline std::string ShownName(const std::string& file) { return CleanName(std::filesystem::path(file).stem().string()); }

inline std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

// Reads slots.txt and the folder, gives new files the next free ids, rewrites slots.txt when something was added.
// Called under g_mutex.
inline void ScanLocked() {
  namespace fs = std::filesystem;
  const fs::path dir = Dir();
  std::error_code ec;
  fs::create_directories(dir, ec);
  std::map<int, Slot> slots;
  std::map<std::string, int> by_file;  // lower-case file name -> id
  {
    std::ifstream in(dir / "slots.txt");
    std::string line;
    while (std::getline(in, line)) {
      line = Trim(line);
      if (line.empty() || line[0] == '#') continue;
      std::vector<std::string> cols;
      std::stringstream ss(line);
      std::string col;
      while (std::getline(ss, col, '\t')) cols.push_back(Trim(col));
      if (cols.size() < 2 || cols[1].empty()) continue;
      int id = 0;
      try {
        id = std::stoi(cols[0]);
      } catch (...) {
        continue;
      }
      if (id < kFirstId || id > kLastId || slots.count(id) || by_file.count(Lower(cols[1]))) continue;
      Slot s;
      s.id = id, s.file = cols[1];
      s.name = cols.size() >= 3 && !cols[2].empty() ? CleanName(cols[2]) : ShownName(cols[1]);
      slots[id] = s;
      by_file[Lower(s.file)] = id;
    }
  }
  std::vector<std::string> files;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.is_regular_file(ec) && IsSong(e.path())) files.push_back(e.path().filename().string());
  }
  std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
  bool added = false;
  for (const auto& f : files) {
    auto it = by_file.find(Lower(f));
    if (it != by_file.end()) {
      slots[it->second].present = true;
      continue;
    }
    int id = kFirstId;
    while (slots.count(id) && id <= kLastId) ++id;
    if (id > kLastId) break;
    Slot s;
    s.id = id, s.file = f, s.name = ShownName(f), s.present = true;
    slots[id] = s;
    by_file[Lower(f)] = id;
    added = true;
    REXLOG_INFO("[wwe2k14][music_slots] new song '{}' -> music id {} ('{}')", f, id, s.name);
  }
  if (added) {
    std::ofstream out(dir / "slots.txt", std::ios::trunc);
    out << "# WWE 2K14 recomp: new entrance-music slots. Music id <TAB> file name [<TAB> shown name]. Written by the\n"
           "# game when it finds a new song in this folder. A song keeps its id, so wrestlers keep the songs you gave\n"
           "# them; a line whose file is gone keeps its id. Edit the shown name in the third column if you like.\n";
    for (const auto& [id, s] : slots) {
      out << id << '\t' << s.file;
      if (s.name != ShownName(s.file)) out << '\t' << s.name;
      out << '\n';
    }
    if (!out) REXLOG_WARN("[wwe2k14][music_slots] could not write {}", (dir / "slots.txt").string());
  }
  size_t present = 0;
  for (const auto& [id, s] : slots) present += s.present;
  if (!g_scanned || slots.size() != g_slots.size() || added) {
    REXLOG_INFO("[wwe2k14][music_slots] {} song(s) in {} ({} id(s) in slots.txt)", present, dir.string(),
                slots.size());
  }
  g_slots = std::move(slots);
  g_scanned = true;
  g_last_scan = std::chrono::steady_clock::now();
}

// The folder is looked at again at most every 2 s (the list loads several times while the menus change).
inline void Scan() {
  std::lock_guard lock(g_mutex);
  if (g_scanned && std::chrono::steady_clock::now() - g_last_scan < std::chrono::seconds(2)) return;
  ScanLocked();
}

inline bool IsSlotId(int id) { return id >= kFirstId && id <= kLastId; }

// The song file for music id `id`, or an empty path (not a slot id, file gone, or off).
inline std::filesystem::path FileFor(int id) {
  if (!Active() || !IsSlotId(id)) return {};
  Scan();
  std::lock_guard lock(g_mutex);
  auto it = g_slots.find(id);
  if (it == g_slots.end() || !it->second.present) return {};
  return Dir() / it->second.file;
}

inline uint32_t AllocGuestBytes(const void* data, uint32_t size) {
  const uint32_t a = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
  if (a) std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(a), data, size);
  return a;
}

// The text manager hook (src/gfx_menu.h): the guest string for one of our name ids, else 0.
inline uint32_t NameFor(uint32_t text_id) {
  if (text_id < kTextBase + kFirstId - 6000 || text_id > kTextBase + kLastId - 6000 || !Active()) return 0;
  std::lock_guard lock(g_mutex);
  auto it = g_slots.find(int(text_id - kTextBase) + 6000);
  if (it == g_slots.end()) return 0;
  uint32_t& g = g_guest_names[it->second.name];
  if (!g) g = AllocGuestBytes(it->second.name.c_str(), uint32_t(it->second.name.size() + 1));
  return g;
}

inline uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline void PutBe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
inline void PutBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// After the load callback: append one record per song to list 2 (MUSIC). `self` = the list object.
inline void OnListsLoaded(uint8_t* base, uint32_t self) {
  if (!Active() || !self) return;
  const uint32_t count_at = self + kOffCount + 2 * kMusicList, recs_at = self + kOffRecords + 4 * kMusicList;
  if (!GuestWordReadableFast(count_at & ~3u) || !GuestWordReadableFast(recs_at)) return;
  const uint16_t count = Be16(rex::memory::GuestPtr(base, count_at));
  const uint32_t recs = Be32(rex::memory::GuestPtr(base, recs_at));
  if (!recs || count < 2 || count > 4000 || !GuestWordReadableFast(recs & ~3u) ||
      !GuestWordReadableFast((recs + 8u * count - 1) & ~3u)) {
    return;
  }
  const uint8_t* r = rex::memory::GuestPtr(base, recs);
  // The music list starts with USER PLAYLIST (9998) and NONE (9999): anything else is left alone (another version).
  if (Be16(r) != 9998 || Be16(r + 8) != 9999) {
    if (!g_warned_layout) {
      g_warned_layout = true;
      REXLOG_WARN("[wwe2k14][music_slots] list {} does not start with USER PLAYLIST / NONE ({} / {}): left as it is",
                  kMusicList, Be16(r), Be16(r + 8));
    }
    return;
  }
  Scan();
  std::lock_guard lock(g_mutex);
  std::vector<const Slot*> add;
  for (const auto& [id, s] : g_slots) {
    if (!s.present) continue;
    bool dup = false;  // a stock record with this value already (never seen; another version)
    for (uint32_t i = 0; i < count && !dup; ++i) dup = int16_t(Be16(r + 8 * i)) == id;
    if (!dup) add.push_back(&s);
  }
  if (add.empty()) return;
  std::string bytes(reinterpret_cast<const char*>(r), 8u * count);
  bytes.resize(8u * (count + add.size()));
  for (size_t i = 0; i < add.size(); ++i) {
    uint8_t* p = reinterpret_cast<uint8_t*>(bytes.data()) + 8u * (count + i);
    PutBe16(p, uint16_t(add[i]->id));
    PutBe16(p + 2, 0xFFFF);
    PutBe32(p + 4, kTextBase + uint32_t(add[i]->id - 6000));
  }
  uint32_t& copy = g_guest_arrays[bytes];
  const bool fresh = !copy;
  if (!copy) copy = AllocGuestBytes(bytes.data(), uint32_t(bytes.size()));
  if (!copy) return;
  PutBe32(rex::memory::GuestPtr(base, recs_at), copy);
  PutBe16(rex::memory::GuestPtr(base, count_at), uint16_t(count + add.size()));
  if (fresh) {
    REXLOG_INFO("[wwe2k14][music_slots] MUSIC list: {} stock + {} new (object 0x{:08X}, records 0x{:08X})", count,
                add.size(), self, copy);
  }
}

}  // namespace wwe2k14::music_slots

// The /ENEX/LSD list object's load callback (see the top of this file).
REX_HOOK_RAW(sub_82928B08) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82928B08(ctx, base);
  wwe2k14::music_slots::OnListsLoaded(base, self);
}
