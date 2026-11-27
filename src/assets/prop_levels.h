// S111 PROPLEVELS (docs\research\s111\PROPLEVELS.md): the mip level count the game's prop builders give an entrance prop's and an m2 weapon / arena prop's textures
// (assets::Texture::game_levels; renderer_scene\game_levels.h Relevel uploads it). Header only; pure over bytes (tex_dir_order.h Add and asset_load.h LoadM2Weapon call it;
// the unit tools\scene\prop_levels_unit.cpp builds its own bytes). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game [C]:
//   * An entrance prop (CWeaponEventSD::MakeWeapon, sub_8272D710) reads its PACH's entry 100 into a per-name table (sub_827FE468: the reader of the arena's 50010, the same XML
//     MipmapData form), sets the loader's level-count word to 3, builds entry 10 texture by texture with the table's override (sub_827FDB78: the first row whose name equals the
//     directory record's bytes up to the NUL), appends entry 20 at 3 (sub_82BA9D68, no per-name step) and restores the word.
//   * A weapon / arena prop of the match's weapon set (pac\m2.pac p0/1000/<id>): SetWeaponData (sub_8258FF90) reads the weapon PACH's entry 100 into the same kind of table and
//     keeps it per slot; the set's model builder (sub_8258E270) sets 3, builds entry 10 per name with that slot's table, appends entry 20 at 3 and restores the word.
//   Neither builder has the stage's record byte or name lists (arena_levels.h).
#pragma once
#include <cstdint>
#include <utility>

#include "arena_levels.h"   // Rule, ParseMipmapData (the same table and lookup)
#include "dds.h"            // TexSet
#include "pac.h"

namespace assets::prop_levels {

inline constexpr uint32_t kPropListLevels = 3;     // sub_8272D710 +0x3F0, sub_8258E270 +0x2D8: both prop builders' lists
inline constexpr uint32_t kPropTableEntry = 100;   // the prop PACH's per-name table (sub_8272D710 +0x80, sub_8258FF90 +0x724)
inline constexpr uint32_t kPerNameEntry = 10;      // the directory the table applies to (sub_8272D710 +0x42C, sub_8258E270 +0x318)
inline constexpr uint32_t kListEntry = 20;         // appended at 3 with no per-name step (sub_8272D710 +0x4A8, sub_8258E270 +0x3A8)

// The rule of the prop PACH `h` (its entries in `buf`): the entry-100 table over a base of 3 (no entry 100: every name 3).
inline arena_levels::Rule TableRule(const Bytes& buf, const pac::Pach& h) {
  arena_levels::Rule r;
  r.base = kPropListLevels;
  if (const pac::Ent* e = pac::Find(h, kPropTableEntry)) {
    const Bytes t = pac::Decode(buf.data() + e->off, e->size);
    r.rows = arena_levels::ParseMipmapData(t.data(), t.size());
  }
  return r;
}

// Adds the texture directory `dir` (the PACH's entry `key`) to `tex` with the builder's count: entry 10 per name by `r`, entry 20 at 3; any other entry (ours' m2 loader also
// reads 11..19, which the game's builder does not) keeps the file's chain (count 0). Returns the TexSet's answer.
inline bool AddDir(TexSet& tex, Bytes&& dir, uint32_t key, const arena_levels::Rule& r) {
  if (key == kPerNameEntry) return tex.AddDirNamed(std::move(dir), r.Fn());
  if (key == kListEntry) return tex.AddDirLevels(std::move(dir), kPropListLevels);
  return tex.AddDir(std::move(dir));
}

// LoadM2Weapon's texture directories of the weapon leaf `h` (its entries in `buf`): entries `tex_first`..19, and 20 when `tex_first` <= 10, in stored order, each with the
// set builder's count (AddDir over the leaf's own entry-100 table, SetWeaponData's per-slot table). `tex_first` = 10 is the game's (weapon_draw_unit plants 11). Returns how
// many were added. One walk for the loader and its unit (tools\scene\prop_levels_unit.cpp runs it).
inline int AddWeaponDirs(TexSet& tex, const Bytes& buf, const pac::Pach& h, uint32_t tex_first = kPerNameEntry) {
  const arena_levels::Rule r = TableRule(buf, h);
  int added = 0;
  for (const pac::Ent& e : h.ents)
    if (((e.id >= tex_first && e.id <= 19) || (e.id == kListEntry && tex_first <= kPerNameEntry)) && e.size >= 0x30 &&
        AddDir(tex, pac::Decode(buf.data() + e.off, e.size), e.id, r))
      ++added;
  return added;
}

}  // namespace assets::prop_levels
