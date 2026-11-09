// Session 98 (DLC in the Arena Viewer): the viewer reads the player's DLC packs the way the game does, read in place (CLAUDE.md:
// the packs are the player's own files; nothing is copied or written, the mods rule's "additive, read in place").
//
// How the game finds a DLC file [C: run logs out\build\win-amd64-release\logs\wwe2k14_5933 .. 5976 (Session 98 matches with the
// packs on), docs\systems\dlc.md]:
//   - the packs are enumerated once (XamContentCreateEnumerator, our provider src\dlc.h lists the ready, switched-on packs of the DLC
//     folder in pack-id order: dlc_catalog.h Scan sorts by pack id) and opened as dlc0:, dlc1:, ... in that order;
//   - a pack's files carry its pack id: the disc's name + "_dlcPP" (`DLC2:\PAC\CH\CH385_DLC04.PAC` = Fandango, pack 4 = "DLC Pack 3";
//     `DLC2:\PAC\EVT\ENTRANCE_DLC04.PAC`, `EVTOBJ_DLC04.PAC`, `EVTMOV_DLC04.PAC`, `PRTCLS_DLC04.PAC`, `GM_DLC04.PAC`, `M3_DLC04.PAC`)
//     or a name of its own (`DLC2:\PAC\DLC_MOT04.PAC`, `dlcN:\sound\DLC_0N.pck`); the disc's own files (`D:\PAC\CH\CH255.PAC`,
//     `D:\PAC\EVT\ENTRANCE02.PAC`) stay on the disc: no DLC id is also a disc file (pac\ch has no ch377-396);
//   - a file a pack may or may not hold is probed in every pack in turn, dlc0 first (`dlc0:\sound\DLC_02.pck` FAILED,
//     `dlc1:` FAILED, `dlc2:` opened; the same for every `catalog_hd.arc` and `Entrance_Music_DLC_0N.bnk`): the first pack that has
//     it wins. A wrestler's entrance bank is looked up in the DLC entrance files as well as the disc's (Entrance_dlc03 loaded with
//     the nWo ids 377-381 in run 5933; Entrance_dlc04 with 384-387 in 5934).
// So the viewer: the disc first; then the packs in pack-id order, each tried with the literal name and with its "_dlcPP" name
// (launcher::GameFiles overlays). The entrance and prop lookups add each pack's Entrance_dlcPP / evtobj_dlcPP (EntranceFiles, below;
// asset_load.h PropPach). The folder and the switched-off packs come from the launcher (USE MY DLC: --dlc DIR [--dlc-off 3,5], or
// --no-dlc); with no flag, the usual folder (dlc_catalog.h DefaultDir: `dlc` beside the game data). Packages (STFS files) are not read
// by the viewer [U: the game mounts them; the viewer would need an STFS reader]: listed as skipped.
#pragma once

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../dlc_catalog.h"
#include "../launcher/disc_image.h"

namespace assets {
namespace dlc_files {

namespace fs = std::filesystem;

struct PackInfo {
  int pack_id = -1;
  std::string folder, name, state;  // state: "used" or why not
};
struct State {
  bool on = false;
  fs::path dir;
  std::vector<PackInfo> packs;
};
inline State& St() {
  static State s;
  return s;
}

inline std::string SuffixOf(int pack_id) {
  char b[16];
  std::snprintf(b, sizeof b, "_dlc%02d", pack_id);
  return b;
}

// Finds the packs in `dir` (empty = the usual folder beside `game_root`) and gives `files` the ready, switched-on ones as overlays,
// in the game's order. `on` false = none. Returns a one-line summary.
inline std::string Configure(launcher::GameFiles& files, const fs::path& game_root, bool on, const fs::path& dir, const std::string& off_ids) {
  State& s = St();
  s = State();
  s.on = on;
  std::vector<launcher::GameFiles::Overlay> ov;
  if (!on) {
    files.SetOverlays({});
    return "DLC: off";
  }
  std::error_code ec;
  s.dir = dir.empty() ? wwe2k14::dlc::DefaultDir(game_root, fs::path()) : dir;
  if (s.dir.empty() || !fs::is_directory(s.dir, ec)) {
    files.SetOverlays({});
    return "DLC: no folder at " + wwe2k14::dlc::PathUtf8(s.dir);
  }
  std::vector<wwe2k14::dlc::Pack> packs;
  try {
    packs = wwe2k14::dlc::Scan(s.dir, /*check_files=*/true, /*bytes=*/false);
  } catch (const std::exception& e) {
    files.SetOverlays({});
    return std::string("DLC: could not read the folder: ") + e.what();
  }
  const std::vector<int> off = wwe2k14::dlc::ParseIdList(off_ids);
  for (const auto& p : packs) {
    PackInfo pi;
    pi.pack_id = p.pack_id, pi.folder = p.folder, pi.name = p.DisplayName();
    if (p.status != wwe2k14::dlc::Status::kReady) pi.state = std::string(wwe2k14::dlc::StatusName(p.status)) + " (" + p.reason + ")";
    else if (std::find(off.begin(), off.end(), p.pack_id) != off.end()) pi.state = "switched off";
    else if (p.package) pi.state = "a package (STFS): the viewer reads extracted folders only";
    else {
      pi.state = "used";
      ov.push_back({p.root, SuffixOf(p.pack_id)});
    }
    s.packs.push_back(pi);
  }
  files.SetOverlays(ov);
  return "DLC: " + std::to_string(ov.size()) + " of " + std::to_string(packs.size()) + " pack(s) used from " + wwe2k14::dlc::PathUtf8(s.dir);
}

// The pack ids in use, in order (from the overlays' suffixes).
inline std::vector<int> PackIds(const launcher::GameFiles& files) {
  std::vector<int> ids;
  for (const auto& o : files.overlays())
    if (o.suffix.size() > 4) ids.push_back(std::atoi(o.suffix.c_str() + 4));
  return ids;
}

// The entrance bank files the game looks banks up in: the disc's Entrance00-08, then each pack's Entrance_dlcPP (see above).
inline std::vector<std::string> EntranceFiles(const launcher::GameFiles& files) {
  std::vector<std::string> v;
  char rel[64];
  for (int n = 0; n < 9; ++n) std::snprintf(rel, sizeof rel, "pac/evt/Entrance%02d.pac", n), v.push_back(rel);
  for (int id : PackIds(files)) std::snprintf(rel, sizeof rel, "pac/evt/Entrance_dlc%02d.pac", id), v.push_back(rel);
  return v;
}

// The prop files: the disc's evtobj.pac, then each pack's evtobj_dlcPP.pac. A prop PACH is numbered `pack * kPropPackStride + index`
// in a pack's file (index alone on the disc), so a PACH number names its file.
constexpr int kPropPackStride = 100000;
inline std::string PropFile(int pach) {
  if (pach < kPropPackStride) return "pac\\evt\\evtobj.pac";
  char rel[64];
  std::snprintf(rel, sizeof rel, "pac\\evt\\evtobj_dlc%02d.pac", pach / kPropPackStride);
  return rel;
}
inline int PropIndex(int pach) { return pach % kPropPackStride; }

// The motion pacs of the packs (pac\dlc_motPP.pac, a name of their own) that exist: "dlc_mot04" ...
inline std::vector<std::string> MotionFiles(const launcher::GameFiles& files) {
  std::vector<std::string> v;
  char nm[32];
  for (int id : PackIds(files)) {
    std::snprintf(nm, sizeof nm, "dlc_mot%02d", id);
    if (files.Exists(std::string("pac\\") + nm + ".pac")) v.push_back(nm);
  }
  return v;
}

}  // namespace dlc_files
}  // namespace assets
