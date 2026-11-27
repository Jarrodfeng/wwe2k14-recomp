// S111 STAGELEVELS (docs\research\s111\STAGELEVELS.md): the texture set the game builds for a bgEtc stage structure (steel cage, Hell in a Cell, Elimination Chamber,
// Inferno, ladder rig) and the level count of each of its two lists (assets::Texture::game_levels; renderer_scene\game_levels.h Relevel uploads it). Header only; pure over
// bytes (asset_load.h LoadStageEntry and LoadGmEntry call it; the unit tools\scene\stage_levels_unit.cpp builds its own bytes). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game [C]:
//   * The holder's completion (sub_825778F8) builds the set with sub_82577300, its one caller. That reads two top-level keys of the structure's PACH and no other: key 0,
//     appended as a 2D list at a count chosen from the holder, and key 10, appended as a cube list with the loader's count word set to 0 (the whole chain).
//   * The key-0 count: the holder's type [H+176] 3 (CELL) -> 7, type 0 (CAGE) -> 4, any other type -> 3 when [H+180] is set, 0 (the whole chain) when it is clear.
//   * The request (sub_82576DD8) stores the type from the path it asks for (CAGE 0, CABU 1, CABA 2, CELL 3, CHAI 5, INFE 7, the ladder paths 4) and sets [H+180] to 1 only
//     on the per-title ladder path /STGL/%04d. The holder's init (sub_82576618) clears it; its unload (sub_82576870) does not write it, so a holder that once loaded a /STGL rig
//     keeps it set for later structures [L: no other writer among the holder's functions; a write through the holder handed to another function is not excluded]. A 360 quirk
//     left unmatched on purpose: ours takes the freshly made holder's clear flag for every other path.
#pragma once
#include <cstdint>
#include <utility>

#include "dds.h"         // TexSet
#include "level_tag.h"   // kWholeChainLevels (the game's 0 = the whole chain, as a Texture::game_levels)
#include "pac.h"

namespace assets::stage_levels {

inline constexpr uint32_t kListEntry = 0;    // the 2D list (sub_82577300 +0x1C, appended +0x200 kind 0)
inline constexpr uint32_t kCubeEntry = 10;   // the cube list (sub_82577300 +0x74, count 0 set +0x218, appended +0x22C kind 1)

// The holder type [H+176] sub_82576DD8 stores for the path whose bgEtc PACH is `pach` (bgetc_stg_structures.md's rows); -1 = a PACH no request names (8 SNCE, 9, 10).
inline constexpr int HolderTypeOfPach(int pach) {
  return pach == 0 ? 0 : pach == 1 ? 1 : pach == 2 ? 2 : pach == 3 ? 3 : (pach == 4 || pach == 5 || (pach >= 11 && pach <= 61)) ? 4 : pach == 6 ? 5 : pach == 7 ? 7 : -1;
}
// [H+180] when the structure of `pach` completes: set for a /STGL/%04d rig (PACH 11..61, sub_82576DD8 +0x34C); clear otherwise (the fresh holder's, sub_82576618).
inline constexpr bool LadderFlagOfPach(int pach) { return pach >= 11 && pach <= 61; }
// The key-0 count sub_82577300 sets at +0x1EC for holder type `type` and [H+180] `flag`.
inline constexpr uint32_t ListLevels(int type, bool flag) { return type == 3 ? 7u : type == 0 ? 4u : flag ? 3u : level_tag::kWholeChainLevels; }

// Adds bgEtc PACH `pach`'s texture set (its top-level entries in `buf`) to `tex` in the game's order: key 0 at ListLevels, then key 10 at the whole chain. A PACH no request
// names keeps the file's chains (count 0). Returns how many directories were added.
inline int AddStructDirs(TexSet& tex, const Bytes& buf, const pac::Pach& h, int pach) {
  const int type = HolderTypeOfPach(pach);
  int added = 0;
  const auto add = [&](uint32_t key, uint32_t levels) {
    const pac::Ent* e = pac::Find(h, key);
    if (!e || e->size < 0x30) return;
    Bytes d = pac::Decode(buf.data() + e->off, e->size);
    if (type < 0 ? tex.AddDir(std::move(d)) : tex.AddDirLevels(std::move(d), levels)) ++added;
  };
  add(kListEntry, ListLevels(type, LadderFlagOfPach(pach)));
  add(kCubeEntry, level_tag::kWholeChainLevels);
  return added;
}

}  // namespace assets::stage_levels
