// Session 101 (lane 199, "why imports fail"): THE one rule for "does the Superstar Importer's pack fit the DLC the game gets?",
// shared by the game (src\dlc.h: leaves the pack out when it does not), the importer core (PackOutOfDate, BuildPack's record) and
// both launchers (they rebuild the pack before PLAY when it does not). Before this, the builder and the game looked at different
// DLC sets (the builder ignored switched-off packs and Xbox 360 packages, and nothing rebuilt), so a pack the game then left out
// only said so in the game's log (docs\systems\dlc.md "Session 101: why imports fail").
//
// Why a pack can stop fitting: the game takes the roster (chEtc) and the names (string) from the HIGHEST pack it gets (Session 98,
// run 072), and ours (id 6) is the highest. Its chEtc / string are copies of the top real roster pack's (or the disc's, with no
// DLC) with our rows added, so they are right only for that one pack: another top pack (DLC added, removed, switched off, or a
// different copy of it) would hide that pack's wrestlers or point at files the game does not have. superstars.txt records what it
// was built on:
//     pack 6
//     built_on 4 5        every real pack it copies files from (the top roster pack + the packs of DLC base / entrance wrestlers)
//     top 5 8A1C33F0      the top roster pack's id and its catalog's FNV-1a (0 0 = the disc's own files, no DLC)   [Session 101]
//     star 397 music 100
// An older superstars.txt has no "top" line: the top is then max(built_on) (0 when empty) and the copy is not compared.
// Portable C++ (game, launchers, viewer, the PC check tools\importer\test_core.cpp).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../dlc_catalog.h"

namespace wwe2k14::superstars_fit {

namespace fs = std::filesystem;
using dlc::Pack;

// A pack whose chEtc and string replace the game's whole roster and names (packs 3, 4, 5 on Jarrod's files; pack 2 holds no files).
inline bool IsRosterPack(const Pack& p) { return p.has_chetc && !p.string_pack.empty(); }

// The real packs the game gets from a DLC folder, in pack-id order, by src\dlc.h Install's rule: dir empty = USE MY DLC off (none);
// ready (every listed file present), not switched off (the launcher's dlc_off, "3,5"), and an id + revision the game's own gate
// accepts (dlc::GateAccepts: ids 1..5; any other id is refused by the game's catalog parser). Folders and Xbox 360 packages alike.
inline std::vector<Pack> GamePacks(const fs::path& dir, const std::string& off) {
  std::vector<Pack> out;
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) return out;
  const std::vector<int> off_ids = dlc::ParseIdList(off);
  for (Pack& p : dlc::Scan(dir, /*check_files=*/true, /*bytes=*/false)) {
    if (p.status != dlc::Status::kReady || !dlc::GateAccepts(p.pack_id, p.revision)) continue;
    if (std::find(off_ids.begin(), off_ids.end(), p.pack_id) != off_ids.end()) continue;
    out.push_back(std::move(p));
  }
  return out;
}

struct Built {
  bool found = false;           // superstars.txt was read
  int pack_id = 0;
  std::vector<int> built_on;
  int top = -1;                 // the top roster pack it copied (0 = the disc); -1 = not recorded (an importer before Session 101)
  uint32_t top_sig = 0;         // that pack's catalog FNV-1a; 0 = not recorded / the disc
  std::map<int, int> music;     // slot -> music id
  int Top() const {
    if (top >= 0) return top;
    int t = 0;
    for (int id : built_on) t = std::max(t, id);
    return t;
  }
};

inline Built ParseBuilt(std::istream& f) {
  Built b;
  b.found = true;
  for (std::string line; std::getline(f, line);) {
    std::istringstream in(line);
    std::string key;
    in >> key;
    if (key == "pack") {
      in >> b.pack_id;
    } else if (key == "built_on") {
      for (int id; in >> id;) b.built_on.push_back(id);
    } else if (key == "top") {
      int id = -1;
      std::string sig;
      if (in >> id) b.top = id >= 0 && id <= 0xFFFF ? id : -1;
      if (in >> sig) b.top_sig = uint32_t(std::strtoul(sig.c_str(), nullptr, 16));
    } else if (key == "star") {
      int slot = -1, music = -1;
      std::string m;
      in >> slot >> m >> music;
      if (m == "music" && slot >= 397 && slot <= 499 && music >= 0 && music < 10000) b.music[slot] = music;
    }
  }
  return b;
}
inline Built ReadBuilt(const fs::path& superstars_txt) {
  std::ifstream f(superstars_txt);
  if (!f) return Built();
  return ParseBuilt(f);
}

// A pack's name for a sentence: the DLC's own name when its catalog has one ("Legends Pack"), else "DLC pack N".
inline std::string NameOf(const Pack* p, int id) {
  if (!p) return "DLC pack " + std::to_string(id);
  const std::string n = p->ShortName();
  return n.empty() || n.rfind("Pack ", 0) == 0 ? "DLC pack " + std::to_string(id) : "the DLC \"" + n + "\"";
}

// "" = the pack fits the real packs the game gets (`offered`: real packs only, ours left out). Else WHY not, as the middle of a
// sentence for the player ("it was built with ..."): the caller adds what to do (the launchers rebuild, the importer says BUILD
// INTO GAME, the game logs it).
inline std::string Problem(const Built& b, const std::vector<const Pack*>& offered) {
  auto find = [&](int id) -> const Pack* {
    for (const Pack* p : offered)
      if (p && p->pack_id == id) return p;
    return nullptr;
  };
  for (int id : b.built_on)
    if (!find(id)) return "it was built with " + NameOf(nullptr, id) + ", which the game does not get now (USE MY DLC off, that pack switched off, or removed)";
  if (b.top < 0) {
    // (review SF1) an older superstars.txt (no top line): exactly the Session 98 rule - a real pack above max(built_on) with its own
    // string file - and nothing more, so an import that worked before keeps working (the next build writes the top line)
    const int then = b.Top();
    for (const Pack* p : offered)
      if (p && p->pack_id > then && !p->string_pack.empty())
        return then ? "your DLC changed since it was built (" + NameOf(p, p->pack_id) + " is newer than the DLC it was built on)"
                    : "it was built without DLC, and the game now gets " + NameOf(p, p->pack_id);
    return {};
  }
  const Pack* top_now = nullptr;
  for (const Pack* p : offered)
    if (p && IsRosterPack(*p) && (!top_now || p->pack_id > top_now->pack_id)) top_now = p;
  const int now = top_now ? top_now->pack_id : 0, then = b.Top();
  if (now != then) {
    if (now > then) return then ? "your DLC changed since it was built (" + NameOf(top_now, now) + " is newer than the DLC it was built on)"
                                : "it was built without DLC, and the game now gets " + NameOf(top_now, now);
    return "your DLC changed since it was built (it was built on DLC pack " + std::to_string(then) + ")";
  }
  if (top_now && b.top_sig && top_now->sig && top_now->sig != b.top_sig)
    return "your copy of " + NameOf(top_now, now) + " is not the one it was built on";
  return {};
}

// The same check from the files: <mods>\superstars\superstars.txt against the packs GamePacks(dlc_dir, dlc_off) gives. "" also when
// nothing is built yet (no superstars.txt).
inline std::string ProblemFor(const fs::path& mods_dir, const fs::path& dlc_dir, const std::string& dlc_off) {
  const Built b = ReadBuilt(mods_dir / "superstars" / "superstars.txt");
  if (!b.found) return {};
  const std::vector<Pack> packs = GamePacks(dlc_dir, dlc_off);
  std::vector<const Pack*> offered;
  for (const Pack& p : packs) offered.push_back(&p);
  return Problem(b, offered);
}

// The game's own verdict at its last boot (src\dlc.h writes it; the launchers show it): <mods>\superstars\game_status.txt,
// "in N" or "out <why>". Empty = never written.
inline fs::path StatusFile(const fs::path& mods_dir) { return mods_dir / "superstars" / "game_status.txt"; }
inline std::string ReadStatus(const fs::path& mods_dir) {
  std::ifstream f(StatusFile(mods_dir));
  std::string line;
  std::getline(f, line);
  return line;
}

}  // namespace wwe2k14::superstars_fit
