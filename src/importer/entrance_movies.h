// S110 ENTMOV: the name-plate movie of an imported character (docs\research\s110\ENTFX2.md, row 3). The game plays the plate as
// MOVN/M%03d of (the model key's character + 500); an import's model key is his own slot, and nothing exists under 500 + slot, so the
// generic plate (M000) played. The plate (and the burst at its first frames) is a BIKi movie in the disc's evtmov2_2.pac (23 wrestlers)
// or a real pack's evtmov_dlcPP.pac (pack 4: Fandango's M885) of the wrestlers who have one. Pack 6 now ships pac\evt\evtmov_dlc06.pac (the real packs' evtmov_dlcPP: EPAC v7, sections MVSW and MVSB empty, here with MOVN
// added) holding M<500 + slot> = a byte copy of M<500 + e>, e = the wrestler whose entrance the star uses.
// Header-only; included by importer_core.cpp after entrance_fx.h (it reuses its SourcesFn / NewPac).
#pragma once
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "enex_rows.h"
#include "entrance_fx.h"
#include "importer_core.h"
#include "pack_io.h"

namespace importer::entrance_movies {

using io::Bytes;

inline std::string PlateNo(int plate) {  // "M<plate>" (asset_ids::AssetIds::plate)
  char n[8];
  std::snprintf(n, sizeof n, "M%03d", plate);
  return n;
}
inline std::string PlateName(int character) { return PlateNo(500 + character); }  // a wrestler's own plate (the source)

// Fills `mov` (sections MVSW, MVSB, MOVN) and returns the number of plates copied. 0 = nothing to write.
inline int CopyNameplateMovies(const std::vector<Superstar>& stars, const entrance_fx::SourcesFn& sources, io::Pac& mov,
                               std::vector<std::string>& warnings, std::vector<std::string>& log) {
  mov = entrance_fx::NewPac({"MVSW", "MVSB", "MOVN"});
  io::Dir* dir = mov.FindDir("MOVN");
  std::map<std::string, std::unique_ptr<io::Pac>> cache;  // key -> the pac (null: unreadable / not there)
  std::vector<std::pair<int, io::Entry>> got;
  for (const Superstar& s : stars) {
    const asset_ids::AssetIds ids = asset_ids::Of(s.slot);
    if (!ids.own_plate) {  // M998 / M999 are the disc's own fallback plates: a copy would replace them for everyone
      warnings.push_back(s.name + ": slot " + std::to_string(s.slot) + " has no name-plate movie of his own (" + PlateNo(ids.plate) +
                         " is a plate the game itself uses)");
      continue;
    }
    const int e = enex_rows::EntranceOf(s);
    const std::string want = PlateName(e);
    const io::Entry* found = nullptr;
    // (review) where a plate can be: a real pack's own evtmov_dlcPP.pac (pack 4's evtmov_dlc04 holds MOVN/M885, Fandango's) or the
    // disc's evtmov2_2.pac; the game finds /MOVN/M%03d in any mounted pac. The packs' list ends with the disc's evtmov.pac (sources'
    // order: packs, then the disc), which holds no MOVN: dropped, not read (211 MB).
    std::vector<entrance_fx::Src> where = sources(e, "pac\\evt\\evtmov");
    if (!where.empty()) where.pop_back();
    for (entrance_fx::Src& d : sources(e, "pac\\evt\\evtmov2_2")) where.push_back(std::move(d));
    for (const entrance_fx::Src& src : where) {
      auto it = cache.find(src.key);
      if (it == cache.end()) {
        Bytes d;
        auto p = std::make_unique<io::Pac>();
        std::string err;
        it = cache.emplace(src.key, src.read(d) && io::ReadPac(d, *p, err) ? std::move(p) : nullptr).first;
      }
      if (!it->second) continue;
      io::Dir* sd = it->second->FindDir("MOVN");
      if (!sd) continue;
      for (const io::Entry& x : sd->ents)
        if (x.alias < 0 && io::Trim(x.name) == want && !x.blob.empty()) {
          found = &x;
          break;
        }
      if (found) break;
    }
    if (!found) {  // most wrestlers have no plate movie of their own (23 ids in evtmov2_2.pac): the generic plate is what he had
      log.push_back(s.name + ": wrestler " + std::to_string(e) + " has no name-plate movie " + want + " (the generic plate plays, as for him)");
      continue;
    }
    io::Entry n;
    n.name = PlateNo(ids.plate);
    n.blob = found->blob;  // byte for byte
    got.emplace_back(s.slot, std::move(n));
  }
  std::sort(got.begin(), got.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
  for (auto& g : got) dir->ents.push_back(std::move(g.second));
  if (!got.empty()) {
    std::string w;
    for (const io::Entry& x : dir->ents) w += " " + x.name;
    log.push_back("pac\\evt\\evtmov: MOVN plates" + w + " (each a byte copy of his entrance wrestler's)");
  }
  return int(got.size());
}

}  // namespace importer::entrance_movies
