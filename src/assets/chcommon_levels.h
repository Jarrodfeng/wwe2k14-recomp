// S111 CHCOMMONCUBE (docs\research\s111\CHCOMMONCUBE.md): the mip level count the game's common character load gives chCommon.pac's texture directories
// (assets::Texture::game_levels; renderer_scene\game_levels.h Relevel uploads it, a cube face by face). Header only; pure over bytes (asset_load.h
// load_detail::AddCommonTexDirs calls it; the unit tools\scene\chcommon_levels_unit.cpp builds its own bytes). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game [C]: the common character load (sub_8271CD88) reads chCommon.pac's PACH. Its texture step (sub_8271CB50) sets the loader's level-count word to 4, appends
// entry 20 as a 2D list and entry 21 as a cube list (the list append's kind argument 0 / 1), and restores the word. Entries 10 / 12 are appended by sub_83421CC0, which
// sets no count: they take whatever count is in force when the load runs (not read: a run-time value), so ours keeps their files' chains. Entries 11 / 13 are not texture
// directories.
#pragma once
#include <cstdint>
#include <utility>

#include "dds.h"   // TexSet, Bytes

namespace assets::chcommon_levels {

inline constexpr uint32_t kListLevels = 4;     // sub_8271CB50 +0x48: the count around both appends (restored +0x21C)
inline constexpr uint32_t kDetailEntry = 20;   // appended as a 2D list (sub_8271CB50 +0x1D8, kind 0): the detail / bump maps
inline constexpr uint32_t kCubeEntry = 21;     // appended as a cube list (sub_8271CB50 +0x1EC, kind 1): the environment light cubes

// Adds chCommon's texture directory `dir` (its PACH entry `key`) to `tex` with the game's count: entries 20 and 21 at 4, any other entry the file's chain (count 0).
// Returns the TexSet's answer.
inline bool AddDir(TexSet& tex, Bytes&& dir, uint32_t key) {
  if (key == kDetailEntry || key == kCubeEntry) return tex.AddDirLevels(std::move(dir), kListLevels);
  return tex.AddDir(std::move(dir));
}

}  // namespace assets::chcommon_levels
