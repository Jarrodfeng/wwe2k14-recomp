// The arena contributor's static part from the player's own files (Session 101, lane 28, review F5): the slot list with each slot's text row, and the
// screens, read from pac\bg\bgNN.pac with our pac reader (the same code path as the viewer's loader: src\assets\pac.h). Host only; no SDK.
// Needs stb_image.h's zlib decoder declared before this header (src\assets\pac.h says so): the game build has it from native_zlib.h, tools from stb_image.h.
//
// What it lists (the keys the loader of the viewer tries: asset_load.h LoadArena): the stage slots 0-399 and the ring pieces 951 952 953 960 964 973 974
// that exist in the pac's directory, and the screens 800-809 that exist. Each slot carries its text row (pac entry 50001) or, with no row, light_set
// = kNone. NOT applied here (the viewer's own choices, not the game's): leaving out 301 / 302 / 304 when the entrance stage is present, the e / g look
// filter (the contributor does that from the live look), slots whose entry is not a model (the viewer drops those after decoding). So the list is a
// superset of what the viewer shows; arena_rec checks that (provider_missing must be 0).
#pragma once
#include <string>
#include <vector>

#include "../assets/pac.h"
#include "arena_contrib.h"

namespace rdr_scene::live::arena {

inline bool SlotsFromPac(const assets::pac::Pac& pc, std::vector<SlotSnap>& slots, std::vector<ScreenSnap>& screens, std::string& err) {
  slots.clear(), screens.clear();
  if (pc.pachs.empty()) return err = "the pac has no PACH", false;
  const assets::pac::Pach& h = pc.pachs[0];
  const assets::Bytes text = assets::pac::Get(pc.bytes, h, 50001);
  if (text.empty()) return err = "no slot text (entry 50001)", false;
  const std::vector<SlotSnap> rows = ParseSlotText(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()));
  const auto row_of = [&](uint32_t key) -> const SlotSnap* {
    for (const SlotSnap& r : rows)
      if (r.slot == key) return &r;
    return nullptr;
  };
  std::vector<uint32_t> keys;
  for (uint32_t k = 0; k < 400; ++k) keys.push_back(k);
  for (uint32_t k : {951u, 952u, 953u, 960u, 964u, 973u, 974u}) keys.push_back(k);
  for (uint32_t k = 800; k <= 809; ++k) keys.push_back(k);
  for (uint32_t k : keys) {
    if (!assets::pac::Find(h, k)) continue;
    SlotSnap s;
    if (const SlotSnap* r = row_of(k)) s = *r;
    else s.slot = uint16_t(k);  // no text row: light_set stays kNone
    slots.push_back(s);
    if (k >= 800 && k <= 809) {
      ScreenSnap sc;
      sc.key = uint16_t(k);
      screens.push_back(sc);
    }
  }
  return !slots.empty() || (err = "no slots found", false);
}

inline bool SlotsFromFiles(launcher::GameFiles& files, int arena, std::vector<SlotSnap>& slots, std::vector<ScreenSnap>& screens, std::string& err) {
  char rel[64];
  std::snprintf(rel, sizeof rel, "pac\\bg\\bg%02d.pac", arena);
  assets::pac::Pac pc;
  if (!assets::pac::Open(files, rel, pc, err)) return false;
  return SlotsFromPac(pc, slots, screens, err);
}

}  // namespace rdr_scene::live::arena
