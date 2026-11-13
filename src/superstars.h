// wwe2k14 - imported superstars (Session 98, the Superstar Importer's game side; one switch: --wwe2k14_mods_superstars, the
// launchers' USE MY SUPERSTARS, like USE MY LIGHTING / USE MY VIDEOS).
//
// The importer (src\importer, in the Arena Viewer) builds ONE mod DLC pack from the player's own files into
// <mods>\superstars\pack6\ (a normal DLC pack folder: info\catalog.dlc + arcs, pac\..., movies\titantron\...) and writes
// <mods>\superstars\superstars.txt:
//     pack 6                 the pack id in its catalog (always above the player's real packs)
//     built_on 4 5           the real packs its copied files come from (its chEtc / string / sound are pack 5's + ours; empty
//                            with no DLC: then they are the disc's own, and the pack needs neither USE MY DLC nor a DLC folder)
//     top 5 8A1C33F0         Session 101: the top roster pack it copied + its catalog's FNV-1a (0 0 = the disc)
//     star 397 music 100     per character: the music id to play for him (his entrance wrestler's)
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

namespace wwe2k14::superstars {

inline bool g_found = false;   // the switch is on and a built pack + superstars.txt exist
inline bool g_active = false;  // the pack was offered to the game (src\dlc.h)
inline std::filesystem::path g_pack;  // <mods>\superstars\pack6
inline int g_pack_id = 0;
inline std::vector<int> g_built_on;
inline std::map<int, int> g_music;  // slot -> music id
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

// Reads <mods>\superstars\superstars.txt (src\mods.h Install has set mods::g_dir when the switch is on).
inline void Load() {
  g_found = false;
  if (!mods::g_superstars_on) return;
  const std::filesystem::path root = mods::g_dir / "superstars";
  std::error_code ec;
  g_pack = root / "pack6";
  if (!std::filesystem::is_regular_file(g_pack / "info" / "catalog.dlc", ec)) {
    REXLOG_INFO("[wwe2k14][superstars] USE MY SUPERSTARS on, no pack built yet in {} (the Superstar Importer builds it)",
                mods::U8(g_pack));
    WriteStatus("out the pack is not built yet (the importer's BUILD INTO GAME, or PLAY from the launcher, builds it)");
    return;
  }
  // Session 101: one parser for the game, the importer and the launchers (src\importer\pack_fit.h)
  g_built = superstars_fit::ReadBuilt(root / "superstars.txt");
  if (!g_built.found) {
    REXLOG_WARN("[wwe2k14][superstars] {} has a pack but no superstars.txt: rebuild it in the importer", mods::U8(root));
    WriteStatus("out the pack has no superstars.txt (build it again in the importer)");
    return;
  }
  g_built_on = g_built.built_on, g_music = g_built.music, g_pack_id = g_built.pack_id;
  if (g_pack_id < 6 || g_pack_id > 0xFFFF) {
    REXLOG_WARN("[wwe2k14][superstars] superstars.txt names pack id {} (6.. expected): left out", g_pack_id);
    WriteStatus("out superstars.txt is damaged (build the pack again in the importer)");
    return;
  }
  g_found = true;
  REXLOG_INFO("[wwe2k14][superstars] pack {} at {} ({} character(s), built on pack(s) {}, top {}{}){}", g_pack_id, mods::U8(g_pack),
              g_music.size(), [] {
                std::string s;
                for (int id : g_built_on) s += (s.empty() ? "" : ",") + std::to_string(id);
                return s.empty() ? std::string("-") : s;
              }(), g_built.Top(), g_built.top < 0 ? " (old superstars.txt: no top line)" : "", "");
}

// Music: "Play_MUS_0397_0_0" -> "Play_MUS_0100_0_0" in place (same length) when 397 is an imported character. buf = a
// NUL-terminated copy; returns the new id or -1.
inline int RemapMusic(char* buf, size_t cap) {
  if (!g_active || g_music.empty() || cap < 14 || std::strncmp(buf, "Play_MUS_", 9) != 0) return -1;
  int id = 0;
  for (int k = 9; k < 13; ++k) {
    if (buf[k] < '0' || buf[k] > '9') return -1;
    id = id * 10 + (buf[k] - '0');
  }
  if (buf[13] >= '0' && buf[13] <= '9') return -1;  // (not the 4-digit form)
  const auto it = g_music.find(id);
  if (it == g_music.end()) return -1;
  char d[8];
  std::snprintf(d, sizeof d, "%04d", it->second);
  std::memcpy(buf + 9, d, 4);
  return it->second;
}

}  // namespace wwe2k14::superstars
