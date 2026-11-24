// S110 ENTFX: the entrance effects of an imported character (docs\research\s110\IMPREACH.md, rows 2 and 3). The game looks the scene
// objects (pyro, stage lights, screen flashes) up by the entrance BANK: /GMEV/%04d in the gm pac (sub_82335AC8 -> sub_827C7038), and
// the particle sets /PCE6/%04d in the prtcls pac. A miss loads none and fails cleanly, so before this every import's entrance played
// without them. Pack 6 now ships its own gm_dlc06.pac and prtcls_dlc06.pac (as the real packs ship gm_dlcPP / prtcls_dlcPP) holding
// the bank `slot` = a byte copy of the bank of the wrestler whose entrance the star uses (GMEV's only id fields are object ids, so
// the copy needs no rewrite; docs\formats\gm_gmev.md). The sections are the real packs' own, in their order (GMEV GMCH GMGA GMST /
// PCE6 PCCH PCGE PCGI: the section rows with no entries are kept, as gm_dlc05.pac / prtcls_dlc05.pac have them).
// Header-only; included by importer_core.cpp after enex_rows.h. The caller supplies where a pac can be read from (packs and disc), so
// this file needs nothing of the builder.
#pragma once
#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "asset_ids.h"
#include "enex_rows.h"
#include "importer_core.h"
#include "pack_io.h"

namespace importer::entrance_fx {

using io::Bytes;

// One file a section can be read from (a real pack's, then the disc's): `key` names it for the cache, `read` loads it.
struct Src {
  std::string key;
  std::function<bool(Bytes&)> read;
};
// The files to search for wrestler `e`'s bank in the pac `stem` ("pac\\gm" / "pac\\prtcls"), best first: his own pack, the other
// packs, then the disc (the game's own order is the highest pack first, then the disc).
using SourcesFn = std::function<std::vector<Src>(int e, const std::string& stem)>;

inline std::string BankName(int id) {
  char n[8];
  std::snprintf(n, sizeof n, "%04d", id);
  return n;
}

// A pac with the real packs' sections, empty.
inline io::Pac NewPac(const std::vector<std::string>& tags) {
  io::Pac p;
  for (const std::string& t : tags) {
    io::Dir d;
    d.tag = t, d.kind = 0;
    p.dirs.push_back(std::move(d));
  }
  return p;
}

// Appends, per star, the bank `%04d slot` of `section` (copied byte for byte from wrestler e's bank in the first source that holds
// it) to `out` (its first directory is the section). Returns the stars that got one; a star whose bank is nowhere gets a warning.
inline int CopySection(const std::vector<Superstar>& stars, const SourcesFn& sources, const std::string& stem, const std::string& section,
                       io::Pac& out, std::vector<std::string>& warnings, std::vector<std::string>& log) {
  std::map<std::string, std::unique_ptr<io::Pac>> cache;  // key -> the pac (null: unreadable / not there)
  io::Dir* dir = out.FindDir(section);
  if (!dir) return 0;
  std::vector<std::pair<int, io::Entry>> got;  // (slot, entry) in slot order below
  for (const Superstar& s : stars) {
    const int e = enex_rows::EntranceOf(s);
    const asset_ids::AssetIds ids = asset_ids::Of(s.slot);  // S110 BANKID: the source is his entrance wrestler's BANK WORD, the copy is named by his own bank
    if (!ids.ok() || s.src_bank <= 0) {
      warnings.push_back(s.name + ": no entrance bank number, so no " + section + " bank is copied");
      continue;
    }
    const std::string name = BankName(s.src_bank);
    const io::Entry* found = nullptr;
    for (const Src& src : sources(e, stem)) {
      auto it = cache.find(src.key);
      if (it == cache.end()) {
        Bytes d;
        auto p = std::make_unique<io::Pac>();
        std::string err;
        it = cache.emplace(src.key, src.read(d) && io::ReadPac(d, *p, err) ? std::move(p) : nullptr).first;
      }
      if (!it->second) continue;
      io::Dir* sd = it->second->FindDir(section);
      if (!sd) continue;
      for (const io::Entry& x : sd->ents)
        if (x.alias < 0 && io::Trim(x.name) == name && !x.blob.empty()) {
          found = &x;
          break;
        }
      if (found) break;
    }
    if (!found) {
      warnings.push_back(s.name + ": no " + section + " bank " + name + " found in " + stem + " (his entrance plays without those effects)");
      continue;
    }
    io::Entry n;
    n.name = ids.FxName();
    n.blob = found->blob;  // byte for byte (the pac's sector padding included)
    got.emplace_back(ids.bank, std::move(n));
  }
  std::sort(got.begin(), got.end(), [](const auto& x, const auto& y) { return x.first < y.first; });  // ids ascending, as the real packs
  for (auto& g : got) dir->ents.push_back(std::move(g.second));
  if (!got.empty()) {
    std::string w;
    for (const io::Entry& x : dir->ents) w += " " + x.name;
    log.push_back(stem + ": " + section + " banks" + w + " (each a byte copy of his entrance wrestler's)");
  }
  return int(got.size());
}

// Both pacs. A pac that got no bank is left empty (the caller then writes none). Returns the number of banks copied in all.
inline int CopyEntranceFx(const std::vector<Superstar>& stars, const SourcesFn& sources, io::Pac& gm, io::Pac& prt,
                          std::vector<std::string>& warnings, std::vector<std::string>& log) {
  gm = NewPac({"GMEV", "GMCH", "GMGA", "GMST"});
  prt = NewPac({"PCE6", "PCCH", "PCGE", "PCGI"});
  return CopySection(stars, sources, "pac\\gm", "GMEV", gm, warnings, log) + CopySection(stars, sources, "pac\\prtcls", "PCE6", prt, warnings, log);
}

}  // namespace importer::entrance_fx
