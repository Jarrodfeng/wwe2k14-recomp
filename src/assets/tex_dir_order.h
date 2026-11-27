// S111 TEXDIRORDER (docs\research\s111\TEXDIRORDER.md): the texture directories of one model PACH, added in the order the game's own reader puts them in the
// model's texture list. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only (asset_load.h includes it after stb_image.h, as pac.h needs).
//
// The game [C, listings; our words]: a model's material names resolve against ONE list and the first entry whose name matches wins (the lookup walks the list
// from the front: sub_82BA9B60); each directory is appended to the list in the order its reader adds it (sub_82BA9D68 fills the next free slots):
//   * a character attire (CCharModelDataReader::Factory_Texture, sub_8271FEB8): entries 10, 11, 39, then the composite's base entry 12 (CEditExSSCompositTexture
//     keeps key 12 as its base and, for a plain load (mode 0), its register step appends it after the reader's three: sub_834175E8, sub_83419478, sub_83418378). The
//     masks (13..16) are read and appended for the Threads editor's load (mode 2) only; a built attire (mode 1) builds its own atlas and its register step appends
//     no directory; 17..19 are looked up by neither function.
//   * an entrance (event) prop (CWeaponEventSD::MakeWeapon, sub_8272D710): entry 10, then entry 20 (key 30, the nameplate, is a texture the game builds). S111 PROPLEVELS:
//     entry 10 at 3 overridden per name by the PACH's entry-100 table, entry 20 at 3 (prop_levels.h).
// TexSet::AddDir keeps the first directory's texture for a name: the game's first-match rule. Adding the entries in this order therefore gives the game's answer.
#pragma once

#include <cstdint>
#include <vector>

#include "dds.h"
#include "level_tag.h"   // S111 LEVELSBYTE: kStockTexLevels
#include "pac.h"
#include "prop_levels.h"   // S111 PROPLEVELS: an event prop's entry-100 table, entry 10 per name, entry 20 at 3

namespace assets::tex_dir_order {

// Whose PACH it is: the reader that builds its texture list.
enum class Owner { kCharacter, kEventProp };

// The entry ids that are texture directories for `o`, in the game's list order.
inline const std::vector<uint32_t>& Keys(Owner o) {
  static const std::vector<uint32_t> character = {10, 11, 39, 12}, event_prop = {10, 20};
  return o == Owner::kCharacter ? character : event_prop;
}

// S111 MIPLEVELS / LEVELSBYTE (docs\research\s111\LEVELSBYTE.md): the level count the reader builds `o`'s textures with (Texture::game_levels; 0 = the file's own chain).
// Factory_Texture (sub_8271FEB8) takes byte +6 of the character's load request: `character_levels`, the game's own byte carried to the loader (level_tag.h; a fresh
// request's 4 = kStockTexLevels, what a stock wrestler keeps in a match, an entrance and a story scene; the fillers raise it to 5 only for a created superstar's key).
// S111 PROPLEVELS: MakeWeapon (sub_8272D710) builds its lists at 3 (prop_levels.h kPropListLevels); Add applies the per-name step of entry 10 (prop_levels.h AddDir).
inline uint32_t Levels(Owner o, uint32_t character_levels) { return o == Owner::kCharacter ? character_levels : prop_levels::kPropListLevels; }

// Adds the texture directories of `hh` (its entries live in `buf`) to `tex` in the game's order for `o`; an absent entry is skipped. Returns how many were added.
// `character_levels` = the load request's count of a character's directories (Levels). An event prop's entry 10 takes the PACH's entry-100 table (prop_levels.h).
inline int Add(TexSet& tex, const Bytes& buf, const pac::Pach& hh, Owner o, uint32_t character_levels = level_tag::kStockTexLevels) {
  const arena_levels::Rule prop_rule = o == Owner::kEventProp ? prop_levels::TableRule(buf, hh) : arena_levels::Rule{};   // S111 PROPLEVELS: MakeWeapon's entry-100 table
  int added = 0;
  for (const uint32_t k : Keys(o))
    if (const pac::Ent* e = pac::Find(hh, k); e && e->size >= 0x30) {
      Bytes d = pac::Decode(buf.data() + e->off, e->size);
      if (o == Owner::kCharacter ? tex.AddDirLevels(std::move(d), Levels(o, character_levels)) : prop_levels::AddDir(tex, std::move(d), k, prop_rule)) ++added;
    }
  return added;
}

}  // namespace assets::tex_dir_order
