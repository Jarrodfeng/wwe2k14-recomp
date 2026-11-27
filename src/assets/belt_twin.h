// S111 TEXDIRORDER (B2; docs\research\s111\TEXDIRORDER.md): which title belt model is the same title in the other hand / at the waist, by the game's own data.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes (asset_load.h BeltTwin and tools\scene\texdirorder_unit.cpp use it).
//
// The game [C, listings; our words]: the entrance preload rewrites a belt act's id by evtdata.pac table 103 (sub_8230B4C0 -> sub_82310720; evt_props.h
// Rewrite103): a row is one title, its columns 3 / 4 / 5 the title's waist / left-hand / right-hand ids; an id's model is its "/BOBJ/%04d" row of the evtobj
// pac directory (sub_825AE720). Ours (the viewer: a belt chosen by its PACH) asks the inverse: the PACH's BOBJ id (IdAtStart) -> the side's id (TwinId) ->
// that id's PACH (asset_load.h PropPachEvt). No PACH numbers: a pack's or a mod's evtobj answers by its ids.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace assets::belt_twin {

using Table = std::vector<std::vector<int32_t>>;

// Table 103's column for `side` (2 left hand, 3 right hand, 5 waist: entrance.h PropAttachKind); -1 for any other side.
inline int Column(int side) { return side == 5 ? 3 : side == 2 ? 4 : side == 3 ? 5 : -1; }

// The id of belt `id`'s title on `side`: `id` itself when it is that side's (its thousands digit, PropAttachKind's rule; Rewrite103 leaves an id no row names
// as it is), else the side's column of the first row that names `id` in column 3, 4 or 5. -1: no row names it, the row's column is empty (that title has no
// model on that side), or `side` is not 2 / 3 / 5.
inline int32_t TwinId(const Table& t103, int32_t id, int side) {
  const int col = Column(side);
  if (col < 0 || id <= 0) return -1;
  if (id / 1000 == side) return id;
  for (const auto& row : t103) {
    bool named = false;
    for (size_t c = 3; c <= 5; ++c) named = named || (c < row.size() && row[c] == id);
    if (named) return size_t(col) < row.size() && row[size_t(col)] > 0 ? row[size_t(col)] : -1;
  }
  return -1;
}

// The id of the row of directory section `section` ("BOBJ") whose PACH starts at byte `at`, in an EPAC / EPK8 pac's 16 KB header `b` (12-byte records from
// 0x80C: char[4] name, LE32 start in 2 KB sectors from 0x4000, LE32 size; a record that is not 4 digits opens a section, "LIST" excepted; an all-zero record
// ends it). The inverse of asset_load.h PropDirStartIn. -1: no such row.
inline int32_t IdAtStart(const std::vector<uint8_t>& b, size_t at, const char* section) {
  if (b.size() < 0x4000 || (std::memcmp(b.data(), "EPAC", 4) != 0 && std::memcmp(b.data(), "EPK8", 4) != 0)) return -1;
  auto digit = [](uint8_t c) { return c >= '0' && c <= '9'; };
  char cur[4];
  std::memcpy(cur, b.data() + 0x800, 4);   // (the first section's name sits at +0x800)
  for (size_t o = 0x80C; o + 12 <= 0x4000; o += 12) {
    const uint8_t* r = b.data() + o;
    uint32_t st = 0;
    std::memcpy(&st, r + 4, 4);   // little-endian, as pac.h Le32
    if (!(digit(r[0]) && digit(r[1]) && digit(r[2]) && digit(r[3]))) {
      if (r[0] == 0 && st == 0) break;
      if (std::memcmp(r, "LIST", 4) != 0) std::memcpy(cur, r, 4);
      continue;
    }
    if (std::memcmp(cur, section, 4) != 0 || 0x4000 + size_t(st) * 0x800 != at) continue;
    return int32_t((r[0] - '0') * 1000 + (r[1] - '0') * 100 + (r[2] - '0') * 10 + (r[3] - '0'));
  }
  return -1;
}

}  // namespace assets::belt_twin
