// S105 IMP (docs\research\s105\IMP_ADV.md item 4): the importer never re-uses a retired slot.
//
// The game copies an import's row into the save once, into an INACTIVE row; after that the save's row wins (edits in the importer
// never reach it). A slot handed to a NEW import after its old one was removed would inherit that stale row (old ratings,
// moveset, attire ids under the new name and model). So a slot that held an import once is "retired" (pack.json "retired": [..]):
// a new import gets a never-used slot, and only when none is left does it fall back to a retired one (last, never refused).
// Pure and portable (importer core, the viewer's panel, the PC check tools\importer\test_imp.cpp).
#pragma once

#include <algorithm>
#include <vector>

namespace importer::slots {

// The retired list after a manifest write: the old list + every slot in `old_slots` that the new list no longer holds (removed, or
// moved to another slot). S105 IMP review M1: WriteManifest passes the slots of the last BUILT pack (superstars.txt) as `old_slots`:
// a slot no build ever held cannot be in any save, so removing it retires nothing; a slot a character holds NOW is not retired. Sorted, no duplicates.
inline std::vector<int> MergeRetired(const std::vector<int>& old_retired, const std::vector<int>& old_slots,
                                     const std::vector<int>& new_slots) {
  std::vector<int> r = old_retired;
  for (int s : old_slots)
    if (std::find(new_slots.begin(), new_slots.end(), s) == new_slots.end()) r.push_back(s);
  r.erase(std::remove_if(r.begin(), r.end(), [&](int s) { return std::find(new_slots.begin(), new_slots.end(), s) != new_slots.end(); }),
          r.end());
  std::sort(r.begin(), r.end());
  r.erase(std::unique(r.begin(), r.end()), r.end());
  return r;
}

// free slots in the order to hand them out: never-used ones first (their own order), then retired slots no build ever held (no save
// can hold a row there), then retired slots a build held (S105 IMP review M1). `built` = the slots of the last built superstars.txt.
inline std::vector<int> NeverUsedFirst(const std::vector<int>& free_slots, const std::vector<int>& retired,
                                       const std::vector<int>& built = {}) {
  auto has = [](const std::vector<int>& v, int s) { return std::find(v.begin(), v.end(), s) != v.end(); };
  std::vector<int> v, mid, late;
  for (int s : free_slots) (!has(retired, s) ? v : has(built, s) ? late : mid).push_back(s);
  v.insert(v.end(), mid.begin(), mid.end());
  v.insert(v.end(), late.begin(), late.end());
  return v;
}

}  // namespace importer::slots
