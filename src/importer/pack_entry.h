// Session 110 (lane BUNDLE): the importer's pack is ONE item for sale, written the way the real packs write a multi-character item:
// one type-21 catalog entry whose bundle records list every imported slot as (slot, 0, 0). Settled by docs\research\s110\MASK64.md:
// a catalog entry's id is a bit number in the game's 64-bit content masks, and the game drops a type-1 entry whose id is above 63
// (sub_828177D8 +9C), so one type-1 entry per character broke at the 52nd import (every import with no DLC). The type-21 branch
// of that lookup has no 63 test; a record's row count is not bound-checked by the game, so a record holds 64 rows at most.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "pack_io.h"

namespace importer::pack_entry {

constexpr int kMaxEntryId = 63;      // a bit in a 64-bit mask
constexpr int kFirstFreeAbove = 12;  // the player's own packs use ids 1..12 (MASK64 section 4); a machine with no DLC starts at 13

struct PackEntryPlan {
  io::CatEntry entry;
  std::vector<io::CatBundle> bundles;
  std::string error;  // non-empty: the build is refused
};

// The id = max(highest id of any real pack's entry, 12) + 1; refused above 63. Entry: type 21, +16 and +20 = -1 (one availability row
// only), licence bit 0. Bundle records: 64 rows of (slot, 0, 0) at most each.
inline PackEntryPlan PlanPackEntry(const std::vector<io::CatEntry>& real_entries, const std::vector<int>& slots, uint16_t pack_id,
                                   const std::string& name) {
  PackEntryPlan plan;
  int top = kFirstFreeAbove;
  for (const io::CatEntry& e : real_entries) top = std::max<int>(top, e.id);
  if (top + 1 > kMaxEntryId) {
    plan.error = "the DLC packs already use catalog entry ids up to " + std::to_string(top) +
                 ", so the imported characters' item would get an id above 63, which the game cannot use (its content masks are 64 bits)";
    return plan;
  }
  if (slots.empty()) {
    plan.error = "no characters to put in the pack's catalog entry";
    return plan;
  }
  plan.entry.id = uint16_t(top + 1), plan.entry.type = 21, plan.entry.pack = pack_id, plan.entry.bit = 0;
  plan.entry.char_id = -1, plan.entry.attr1 = -1, plan.entry.attr2 = 0;  // +24 = the bundle key its records carry (0, as every real pack)
  plan.entry.name = name.substr(0, 31);
  for (size_t i = 0; i < slots.size(); i += io::kMaxBundleRows) {
    io::CatBundle b;
    b.pack = pack_id, b.key = uint16_t(plan.entry.attr2);
    for (size_t k = i; k < std::min(slots.size(), i + io::kMaxBundleRows); ++k) b.rows.push_back({uint16_t(slots[k]), 0, 0});
    plan.bundles.push_back(std::move(b));
  }
  return plan;
}

// The rows the game's availability table gets from a pack's entries (S110 review: a transcription of sub_82816AE8's jump table
// 0x82816C14, IMP_B row 14): types 9 / 17 / 18 none; type 21 its own row (21, key = +24) plus a type-19 row when +16 != -1 and a
// type-20 row when +20 != -1; every other type 1..20 one row; types outside 1..21 none. The table holds entries + 2 rows
// (sub_82816AE8 +78) and drops the rest; the player's four packs make 14 rows from 12 entries.
inline size_t AvailabilityRows(const io::CatInfo& c) {
  size_t n = 0;
  for (const io::CatEntry& e : c.entries) {
    if (e.type < 1 || e.type > 21 || e.type == 9 || e.type == 17 || e.type == 18) continue;
    n += e.type == 21 ? 1 + (e.char_id != -1) + (e.attr1 != -1) : 1;
  }
  return n;
}

// S110 review: a transcription of the game's availability test for one pack (sub_82817598 -> sub_828173E8), for the self-test: a
// row (1, char) of a type-1 entry, else a type-21 entry whose bundle records with key == its +24 (row +12 vs record +4, sub_828173E8
// +B4..+C0) hold (char, attire 0). A row whose third value is not 0 sends the game to a type-8 row (not modelled: answered false).
// Licence assumed held (our provider answers every bit). EntryIdFor below is the content-mask id, a different function.
inline bool AvailableLikeGame(const io::CatInfo& c, int ch) {
  for (const io::CatEntry& e : c.entries)
    if (e.type == 1 && e.char_id == ch) return true;
  for (const io::CatEntry& e : c.entries) {
    if (e.type != 21) continue;
    for (const io::CatBundle& b : c.bundles) {
      if (int32_t(b.key) != e.attr2) continue;
      for (const io::CatRow& r : b.rows)
        if (r.ch == ch && r.attire == 0 && r.third == 0) return true;
    }
  }
  return false;
}

// A transcription of the game's lookup sub_828177D8 for one pack, for the self-test: the id of the entry that makes (char, attire)
// available, or -1. Type 1 / 2 / 11 with +16 == char: an id above 63 is dropped; attire 0 needs +20 == 0, attire n needs bit n-1 of
// +20. Type 21: any bundle record of the pack holding the row (char, attire) - the game reads neither the record's pack nor its key
// here (+F4..+150); no 63 test there. The first match in entry order wins.
inline int EntryIdFor(const io::CatInfo& c, int ch, int attire) {
  for (const io::CatEntry& e : c.entries) {
    if (e.type == 1 || e.type == 2 || e.type == 11) {
      if (e.char_id != ch || e.id > kMaxEntryId) continue;
      if (attire == 0 ? e.attr1 == 0 : attire <= 32 && ((uint32_t(e.attr1) >> (attire - 1)) & 1)) return e.id;
    } else if (e.type == 21) {
      for (const io::CatBundle& b : c.bundles) {
        for (const io::CatRow& r : b.rows)
          if (r.ch == ch && r.attire == attire) return e.id;
      }
    }
  }
  return -1;
}

}  // namespace importer::pack_entry
