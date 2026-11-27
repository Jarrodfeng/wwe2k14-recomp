// S111 CAATABLE: the announce table's run-time textures are its live texture KEYS, not the file's (docs\research\s111\CAATABLE.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Pure: <cstdint> only. belt_plate.h includes it (the key store's callers: the drawer rf_props.cpp ApplyWeaponRows and the raw hook's gate weapon_rows_core.h ReadsKeys); the unit
// tools\scene\belt_plate_unit.cpp runs it.
//
// The announce table of a match is a pair of weapon-manager models from m2.pac p0/1000: a created arena places desk 6 and desk top 7 (SubH 45010, arena key 200 -> group 1000; group 1001 holds
// 106 / 107). Two game writers make what these models draw differ from their file's textures, both by writing the model's texture KEY table ([model+260], one 8-byte key per entry of the
// texture-name table), which the game's draw binds at each sampler's name-table index:
//   1. the weapon maker sub_825E2DE0 sends a desk TOP (7 / 104 / 107: sub_822A1868) to sub_824ED3F0, which rewrites every key below the count of the stage's list [stage+14456] to the list's texture
//      of the key's name (a name the list lacks empties the key: sub_82BA9B60's miss is {0, 0}; no file does that today, every list holds the tops' names below its count): the stage
//      pac's texture directory 402 as 2D textures (sub_824ECAD8, called by the stage setup sub_824EDDB0), which holds ji_bl / ji_mon / ji_top (+ ji_mark) in 29 stock arenas and in a created arena;
//   2. the Create-an-Arena paint (sub_822B3868 -> sub_822A4100 cases 13 / 14) writes category 13 (ji_top) into the tops' key and category 14 (ji_bl) into the desks' (6 / 105 / 106), in place
//      (lock, write, unlock flush sub_82CA6BE0: guest_tex_dirty sees it).
// So a weapon row of one of these ids draws, at every diffuse / normal / specular sampler, the texture the live model's key holds at that sampler's name-table index, OVER the file's (belt_plate.h
// BindKeys `over_file`); a key not read (yet, or refused) keeps the file's texture, and so does a key that still holds the file's own picture (BindKeys KeyIsFile: the file's mip chain stands). A key
// that differs is read with its whole mip chain (Store::Want `chain`, guest_texture_mips.h ReadChain).
#pragma once
#include <cstdint>

namespace rdr_scene::live::beltplate {

// A weapon-manager id whose live keys the game rewrites: the six ids sub_822A4100 walks for paint categories 13 / 14 (7 / 104 / 107 are also sub_822A1868's rebind set).
inline bool KeysOverFile(int32_t weapon_id) { return weapon_id == 6 || weapon_id == 7 || (weapon_id >= 104 && weapon_id <= 107); }

}  // namespace rdr_scene::live::beltplate
