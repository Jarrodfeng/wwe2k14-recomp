// S110 DLCRANK (docs\research\s110\DLCRANK.md): the PURE half of "the DOWNLOADED tab in one alphabetical order". std only (plus the one collation in
// seat_bind_core.h); src\roster_abc.h is the game side. Unit: tools\roster_abc\roster_abc_unit.cpp (planted twins: roster_abc_selftest.py).
//
// The list the game has just built (list 4) holds entries in rank order. Entries that are DOWNLOADED (a DLC row or an imported superstar) are refilled in
// the positions the downloaded entries already hold, in alphabetical order; every other entry stays exactly where it is.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "seat_bind_core.h"  // seat_bind::FoldedName: the ONE collation (letters by case-folded base letter, digits / symbols first, a leading THE dropped)

namespace wwe2k14::roster_abc {

struct AbcItem {
  std::string name;         // the display name ("" = none: such an entry keeps its place in the order and sorts first among the downloaded)
  bool downloaded = false;  // only these move
};

// A PERMUTATION of 0..n-1: result[p] = the index of the entry that belongs at position p. Non-downloaded indexes map to themselves; the downloaded
// positions (in list order) are refilled with the downloaded entries stable-sorted by FoldedName (ties keep list order).
inline std::vector<int> AbcOrder(const std::vector<AbcItem>& items) {
  const int n = int(items.size());
  std::vector<int> out(n);
  std::vector<int> pos;  // the positions the downloaded entries hold, ascending
  for (int i = 0; i < n; ++i) {
    out[i] = i;
    if (items[i].downloaded) pos.push_back(i);
  }
  std::vector<std::vector<uint32_t>> key(n);
  for (int p : pos) key[p] = seat_bind::FoldedName(items[p].name);
  std::vector<int> order = pos;
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
    return std::lexicographical_compare(key[a].begin(), key[a].end(), key[b].begin(), key[b].end());
  });
  for (size_t k = 0; k < pos.size(); ++k) out[pos[k]] = order[k];
  return out;
}

inline bool IsIdentity(const std::vector<int>& perm) {
  for (size_t i = 0; i < perm.size(); ++i)
    if (perm[i] != int(i)) return false;
  return true;
}

}  // namespace wwe2k14::roster_abc
