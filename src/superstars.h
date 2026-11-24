// wwe2k14 - imported superstars (Session 98, the Superstar Importer's game side; one switch: --wwe2k14_mods_superstars, the
// launchers' USE MY SUPERSTARS, like USE MY LIGHTING / USE MY VIDEOS).
//
// S110 SEAT-B (docs\research\s110\SEAT1.md): the pack below is now the FRAME (<mods>\superstars\frame\: catalog + byte copies, one `seat S bank B rank R
// [was ..]` line per seat in its superstars.txt) and the characters are the LIBRARY's cards (<mods>\superstars\library\<KEY>\), seated by src\seats.h.
// An old pack6\ with no frame is left to the launcher's migration. The text below describes the superstars.txt lines the old build wrote.
//
// The importer (src\importer, in the Arena Viewer) builds ONE mod DLC pack from the player's own files into
// <mods>\superstars\pack6\ (a normal DLC pack folder: info\catalog.dlc + arcs, pac\..., movies\titantron\...) and writes
// <mods>\superstars\superstars.txt:
//     pack 6                 the pack id in its catalog (always above the player's real packs)
//     built_on 4 5           the real packs its copied files come from (its chEtc / string / sound are pack 5's + ours; empty
//                            with no DLC: then they are the disc's own, and the pack needs neither USE MY DLC nor a DLC folder)
//     top 5 8A1C33F0         Session 101: the top roster pack it copied + its catalog's FNV-1a (0 0 = the disc)
//     star 397 music 100 bank 1   per character: the music id to play for him (his entrance wrestler's); S110 BANKID: `bank` = the
//                            dedicated entrance bank number the build gave him (src\importer\asset_ids.h; not his slot)
// Session 101: whether the pack fits the packs the game gets is src\importer\pack_fit.h Problem() - the same rule the importer and
// the launchers use (they rebuild the pack before PLAY when it does not fit). The game's verdict goes to game_status.txt.
// With the switch on, src\dlc.h offers that folder to the game as one more DLC pack (read-only, like the player's own) and
// opens the game's pack-id gate (0x82035E9C, ids 1-5) for its id - the same mechanism the S98 test switches proved in runs
// 073 / 074 (--wwe2k14_dlc_extra_dir + --wwe2k14_dlc_gate_extra). It is left out (logged) when a built_on pack is not offered
// (its copied files would point at a pack the game does not have) or a real pack newer than its base is (that pack's wrestlers
// would vanish): rebuild in the importer.
// Off (or no pack) = nothing here runs: the vanilla game.
//
// Music: the game builds the theme's Wwise event "Play_MUS_%04d_..." from the music object (src\entrance_music.h). For roster
// wrestlers the music id is the character id [C S33: 343 Lesnar, 298 AJ]; for an imported id the game would ask for
// Play_MUS_0397_0_0, which no sound bank holds. So the event name is rewritten to the entrance wrestler's id here. [U] that
// the game's music id for an imported slot IS the slot id (--wwe2k14_music_probe=true at the game test answers it).
#pragma once

#include <rex/logging.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "importer/pack_fit.h"  // Session 101: the one "does the pack fit the DLC the game gets" rule + superstars.txt's parser
#include "mods.h"
#include "seats_core.h"  // S110 SEAT-B: the frame's seat lines + the library's cards (the roster the game seats from)

namespace wwe2k14::superstars {

inline bool g_found = false;   // the switch is on and a built pack + superstars.txt exist
inline bool g_active = false;  // the pack was offered to the game (src\dlc.h)
inline std::filesystem::path g_pack;  // <mods>\superstars\frame (S110 SEAT-B: was pack6)
inline int g_pack_id = 0;
inline std::vector<int> g_built_on;
// seat -> music id (S110 SEAT-B: the residents' cards, `inst ... music M`; S110 SEAT3: a bind sets and clears a guest's seat in session, and the music event
// hook reads it on another thread, so one atomic per seat, -1 = none).
struct MusicMap {
  std::atomic<int> v[500];
  MusicMap() { Clear(); }
  void Clear() { for (auto& a : v) a.store(-1, std::memory_order_relaxed); }
  void Set(int seat, int music) { if (seat >= 0 && seat < 500) v[seat].store(music, std::memory_order_relaxed); }
  int Get(int seat) const { return seat >= 0 && seat < 500 ? v[seat].load(std::memory_order_relaxed) : -1; }
  size_t Count() const { size_t n = 0; for (const auto& a : v) n += a.load(std::memory_order_relaxed) >= 0; return n; }
};
inline MusicMap g_music;
inline superstars_fit::Built g_built;  // Session 101: superstars.txt as read (src\importer\pack_fit.h: what it was built on)

// Session 101 (lane 199: a pack the game left out said so only in the log): the game's verdict for the launchers,
// <mods>\superstars\game_status.txt = "in N" or "out <why>" (pack_fit.h ReadStatus). Written only when it changes; never throws.
inline void WriteStatus(const std::string& line) {
  try {
    if (mods::g_dir.empty()) return;
    const std::filesystem::path f = superstars_fit::StatusFile(mods::g_dir);
    std::error_code ec;
    if (!std::filesystem::is_directory(f.parent_path(), ec)) return;  // (no superstars folder: nothing to report on)
    if (superstars_fit::ReadStatus(mods::g_dir) == line) return;
    std::ofstream(f, std::ios::trunc) << line << "\n";
  } catch (...) {
  }
}

// Reads <mods>\superstars\frame\superstars.txt and the library (src\seats_core.h) (src\mods.h Install has set mods::g_dir when the switch is on).
// S110 SEAT-B: the pack the game is offered is the FRAME (every seat in its catalog, nothing per character); the characters are the LIBRARY's cards.
inline void Load() {
  g_found = false;
  seats::g_roster = seats::Roster();
  g_music.Clear();
  if (!mods::g_superstars_on) return;
  const std::filesystem::path root = mods::g_dir / "superstars";
  std::error_code ec;
  g_pack = root / "frame";
  if (!std::filesystem::is_regular_file(g_pack / "info" / "catalog.dlc", ec)) {
    if (seats::OldPackPresent(root)) {  // (the old pack is never MOUNTED: the launcher moves it into the library)
      // S110 D1: but its stars' saved state must survive this boot: their seats are HELD (hidden; the Universe roster check keeps them, seats.h).
      // Their seats come from the old superstars.txt `star S ...` lines, read by the one parser (pack_fit.h).
      const superstars_fit::Built old = superstars_fit::ReadBuilt(root / "superstars.txt");
      std::vector<int> stars;
      for (const auto& kv : old.music) stars.push_back(kv.first);
      seats::HoldOldPack(stars, seats::g_roster);
      REXLOG_INFO("[wwe2k14][superstars] an old pack (pack6) was found in {} and there is no frame yet: the launcher migrates it into the library; the game "
                  "boots with no imports this time ({} old star seat(s) held, their saved state kept)", mods::U8(root), seats::g_roster.held.size());
      for (const std::string& l : seats::RosterLog(seats::g_roster)) REXLOG_INFO("{}", l);
      WriteStatus(stars.empty() ? std::string("out an old pack was found: the launcher migrates it (start the game from the launcher once)")
                                : "out " + std::to_string(seats::g_roster.held.size()) +
                                      " imported superstar(s) need converting: start the game from the launcher once");
    } else {
      REXLOG_INFO("[wwe2k14][superstars] USE MY SUPERSTARS on, no frame built yet in {} (the Superstar Importer builds it)", mods::U8(g_pack));
      WriteStatus("out the pack is not built yet (the importer's BUILD INTO GAME, or PLAY from the launcher, builds it)");
    }
    return;
  }
  // Session 101: one parser for the game, the importer and the launchers (src\importer\pack_fit.h)
  g_built = superstars_fit::ReadBuilt(g_pack / "superstars.txt");
  if (!g_built.found) {
    REXLOG_WARN("[wwe2k14][superstars] {} has a frame but no superstars.txt: rebuild it in the importer", mods::U8(root));
    WriteStatus("out the pack has no superstars.txt (build it again in the importer)");
    return;
  }
  g_built_on = g_built.built_on, g_pack_id = g_built.pack_id;
  if (g_pack_id < 6 || g_pack_id > 0xFFFF) {
    REXLOG_WARN("[wwe2k14][superstars] superstars.txt names pack id {} (6.. expected): left out", g_pack_id);
    WriteStatus("out superstars.txt is damaged (build the pack again in the importer)");
    return;
  }
  {  // the library: every card that matches the frame is a resident (src\seats_core.h); music = his entrance wrestler's (the card's inst line)
    std::string text;
    {
      std::ifstream f(g_pack / "superstars.txt", std::ios::binary);
      std::ostringstream ss;
      ss << f.rdbuf();
      text = ss.str();
    }
    seats::LoadRoster(root, g_pack_id, text, seats::g_roster);
    for (const seats::Resident& r : seats::g_roster.res)
      if (r.card.inst.music >= 0 && r.card.inst.music < 10000) g_music.Set(r.seat, r.card.inst.music);
    seats::PublishResidents(seats::g_roster);  // S110 SEAT3: the occupant table (src\seat_occ.h): every reader of the hooks asks it
    for (const std::string& l : seats::RosterLog(seats::g_roster)) REXLOG_INFO("{}", l);
  }
  g_found = true;
  REXLOG_INFO("[wwe2k14][superstars] pack {} at {} ({} character(s), built on pack(s) {}, top {}{}){}", g_pack_id, mods::U8(g_pack),
              g_music.Count(), [] {
                std::string s;
                for (int id : g_built_on) s += (s.empty() ? "" : ",") + std::to_string(id);
                return s.empty() ? std::string("-") : s;
              }(), g_built.Top(), g_built.top < 0 ? " (old superstars.txt: no top line)" : "", "");
}

// Music: "Play_MUS_0397_0_0" -> "Play_MUS_0100_0_0" in place (same length) when 397 is an imported character. buf = a
// NUL-terminated copy; returns the new id or -1.
inline int RemapMusic(char* buf, size_t cap) {
  if (!g_active || cap < 14 || std::strncmp(buf, "Play_MUS_", 9) != 0) return -1;
  int id = 0;
  for (int k = 9; k < 13; ++k) {
    if (buf[k] < '0' || buf[k] > '9') return -1;
    id = id * 10 + (buf[k] - '0');
  }
  if (buf[13] >= '0' && buf[13] <= '9') return -1;  // (not the 4-digit form)
  const int music = g_music.Get(id);
  if (music < 0) return -1;
  char d[8];
  std::snprintf(d, sizeof d, "%04d", music);
  std::memcpy(buf + 9, d, 4);
  return music;
}

}  // namespace wwe2k14::superstars
