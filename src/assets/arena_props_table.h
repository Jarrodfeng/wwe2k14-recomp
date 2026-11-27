// S111 ARENAFIX (docs\research\s111\ARENAFIX.md): the game's per-arena choice of the arena's own props (which ones, and the ring steps' colour), read from the player's files.
// Pure: bytes in, a plan out (asset_load.h load_detail::ArenaPropsPlan reads the two leaves; the unit tools\assets\arena_props_test.cpp builds its own bytes).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game [C]:
//   * The steps' colour: SetWeaponData (sub_8258FF90) passes the ring-step ids 29 / 32 / 35 through sub_825DDAB0: 29 when the arena record's byte 28 is 1 (sub_824E7128), 35 when it
//     is 2 (sub_824E70F0), else 32. The record = the 108-byte row of pac\misc01_Start.pac p0/2 (12-byte head, LE u32 count at +4, rows from +12) whose byte 4 is the arena id
//     (sub_822C3440 over the table [0x836F2C34]; ids 200..249 look up 200); the id is the word [0x8376AB34], the one the live build loads bgNN by. No record: byte 28 reads 0
//     (sub_822C3E50 returns the finder's null).
//   * Which props: the match's weapon placement sub_8258D1D8 looks the same id (200..249 -> 200) up in the arena block of the SubH table 45010 (the table's +28 / +32 / +36,
//     sub_8258CA78: keys below 1000, the first match), follows that chunk's reference to the first chunk keyed by it (sub_8258CA18, the same list) and makes each of THAT chunk's
//     records (sub_825E4E88). The arena chunk's own records are not read; a negative reference places nothing. The loader sub_8258F698 reads pac\bg\bgEtc.pac p10/45010 in order:
//     a SubH block (the table's +4 / +8 / +12, match-keyed), an 'ASH ' block (skipped here), then the arena SubH block, to the last byte. A SubH block is u8 1, u8 0xCC, i16 count,
//     then `count` 12-byte 'SubH' chunks (i16 key, i16 n, i16 ref, u16 pad), each followed by its n 36-byte 'Data' records (the id = byte 31, sub_8258CD40 copies it to +23);
//     an 'ASH ' block is u8 1, u8 0xCC, i16 count, then `count` 8-byte heads ('ASH ', u16 id, i16 n), each followed by n 8-byte entries. S111 PLACESIGNED: every count is
//     signed, and 0 or less is none, the loader building on (Count16).
//     The player's files: group 1000 (arenas 0-9, 11-20, 23, 28-56, 200) = desk 6, top 7, steps 32 twice; 1002 (21, 22) = desk and top only; 1003 / 1004 (78, 80) = backstage weapons.
//   * Each record's id passes through sub_8258CE20 before it is made: the steps 29 / 32 / 35 through sub_825DDAB0 (above); the desk 6 / 106 / 105 becomes [0x82020080 + 4 * record
//     byte 64] and the top 7 / 107 / 104 becomes [0x8202008C + 4 * record byte 65] (sub_825DE388 / sub_825DE3C0; both tables hold 6 / 106 / 105 and 7 / 107 / 104). The player's
//     records: byte 64 = 1 for arenas 28-42 (desk 106), 2 for 43-45 (desk 105); byte 65 = 1 for 28-51 (top 107). S111 DESKMODEL: Plan::desk_key / top_key (DeskKey / TopKey).
//   The places in those records are the loader's fixed ones (desk (0, 0, -760), steps (328, 0, -+328) turned -45 / 225), so only WHICH props and the steps' colour are read here.
//   * S111 DESKMODEL (docs\research\s111\DESKMODEL.md), where each desk / top MESH sits (DeskMeshAt): the weapon maker sub_825E4E88 takes the record's place with no per-model offset,
//     and the draw sub_82C04DF0 loads one row per palette slot (matrix palette[k] - 1 of the call's rows) and each VERTEX takes the row of its own slot (its S1 pair) [C]. Desks
//     6 / 105 / 106 have one skeleton (vector, root, bone04, bone02, bone03, bone01, the same binds) and tops 7 / 107 another (vector, root, bone01, bone02, bone03) [C file], so one
//     row per NODE serves every model. S111 DESKSKIN (docs\research\s111\DESKSKIN.md): ours moves each VERTEX by its own slot's node row (DeskVertexShift; asset_load.h
//     PlaceDeskMeshes), as the game draws it: desk 6 mesh 0's 113 plate vertices (bone04) at the place, its front panel (bone01) at bone01's row. The rows are a NAMED STAND-IN for
//     the weapon's run-time node pose, measured once for 6 / 7 (S97 swap 20911: the desk's bone01 at (0, 0, -775.4), its bone04 at (0, 0, -760), the top's bone01 / bone03 at
//     (0, -48.25, -760): the top resting on the desk) [L measured: what writes that pose is not read; the bind pose plus the place would sink the top 48 into the desk, so it is not
//     the rest pose either]. The rows nobody measured [L, DESKSKIN from the files]: the desk's bone02 / bone03 (the side panels) = the place, as bone04 (moved by bone01's -15.4 the
//     front panel's face, file z 101.4, lands at 86.0, on the side panels' front edge 86.19, only if they stay); the top's bone02 = bone03's row (its 49 vertices are bone03's
//     mirrored in x, the same y / z).
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "load_plants.h"   // S111 PLACECOUNT0: load_detail::PlacementGateOffPlant (the unit's planted twin)

namespace assets::arena_props {

// The key both tables use for an arena id: a created arena (200..249) is 200 (sub_822C3440, sub_8258D1D8 +0x2C..+0x40).
inline int RecordKey(int arena) { return arena >= 200 && arena <= 249 ? 200 : arena; }

// sub_825DDAB0: the bgEtc PACH 9 entry of the ring steps for the record's byte 28.
inline uint32_t StepsVariant(int byte28) { return byte28 == 1 ? 29u : byte28 == 2 ? 35u : 32u; }

inline uint32_t Rd32(const std::vector<uint8_t>& b, size_t o) {
  uint32_t v = 0;
  if (o + 4 <= b.size()) std::memcpy(&v, b.data() + o, 4);
  return v;
}
inline uint16_t Rd16(const std::vector<uint8_t>& b, size_t o) { return o + 2 <= b.size() ? uint16_t(b[o] | (b[o + 1] << 8)) : uint16_t(0); }

// Byte `off` of the arena's record in misc01_Start p0/2: 0 when no record matches (the game reads 0: sub_822C3E50 / sub_822C3EC0 / sub_822C3F00), -1 when the leaf is not the table.
inline int RecordByte(const std::vector<uint8_t>& leaf, int arena, size_t off) {
  if (leaf.size() < 12 || off >= 108) return -1;
  const uint32_t n = Rd32(leaf, 4);
  const int key = RecordKey(arena);
  for (uint32_t i = 0; i < n; ++i) {
    const size_t r = 12 + 108 * size_t(i);
    if (r + 108 > leaf.size()) return -1;
    if (leaf[r + 4] == key) return leaf[r + off];   // the first match (a linear scan)
  }
  return 0;
}
inline int RecordByte28(const std::vector<uint8_t>& leaf, int arena) { return RecordByte(leaf, arena, 28); }

// One chunk of a SubH block: its key, its reference and the ids of its own records.
struct Chunk {
  int key = 0, ref = -1;
  std::vector<int> ids;
};

// S111 PLACESIGNED [C]: a count of the placement table as the loader sub_8258F698 reads it: a SIGNED halfword (extsh), 0 or less = none (the loop it heads is
// skipped and the loader builds on). Block 1's own count is the gate (PlacementIds), read there. UnsignedCountsPlant: the unit's planted twin, the counts unsigned.
inline int Count16(const std::vector<uint8_t>& b, size_t o) {
  if (load_detail::UnsignedCountsPlant()) return Rd16(b, o);
  const int v = int16_t(Rd16(b, o));
  return v > 0 ? v : 0;
}

// One block of the placement table at b[p] (u8 1, u8 0xCC, i16 count); p moves past it. `subh`: SubH chunks into `out`; else an 'ASH ' block, skipped over.
// false = the bytes are not that layout.
inline bool ReadBlock(const std::vector<uint8_t>& b, size_t& p, bool subh, std::vector<Chunk>& out) {
  if (p + 4 > b.size() || b[p] != 0x01 || b[p + 1] != 0xCC) return false;
  const int count = Count16(b, p + 2);   // the 'ASH ' block +0x3D4 / +0x3DC, the arena block +0x598
  size_t q = p + 4;
  for (int c = 0; c < count; ++c) {
    if (q + (subh ? 12 : 8) > b.size() || std::memcmp(b.data() + q, subh ? "SubH" : "ASH ", 4) != 0) return false;
    if (!subh) {
      q += 8 + 8 * size_t(Count16(b, q + 6));   // an 'ASH ' head's entries (+0x40C)
      if (q > b.size()) return false;
      continue;
    }
    Chunk ch;
    ch.key = int16_t(Rd16(b, q + 4)), ch.ref = int16_t(Rd16(b, q + 8));
    const int n = Count16(b, q + 6);   // a SubH chunk's records (+0x268 block 1, +0x608 the arena block): none, the chunk kept
    q += 12;
    for (int j = 0; j < n; ++j) {
      if (q + 36 > b.size() || std::memcmp(b.data() + q, "Data", 4) != 0 || Rd32(b, q + 4) != 36) return false;
      ch.ids.push_back(b[q + 31]);
      q += 36;
    }
    out.push_back(std::move(ch));
  }
  p = q;
  return true;
}

// S111 PLACECOUNT0 [C]: the loader sub_8258F698 builds no block at all unless block 1's count, a SIGNED halfword, is above 0 and its byte 0 is 1 (byte 1 is not checked); the
// table it fills is a fresh one (sub_8258FE08 frees the old, sub_8258C878 zeroes the new: counts 0, block pointers null), so sub_8258CA78 finds no key and the arena places
// nothing. The player's bgEtc.pac p10 / p64 45010: count 125, byte 0 = 1 (the gate passes). true = the bytes hold block 1's head and the gate refuses them (no table);
// S111 EQUIVTIER: PlacementIds' answer for it, and the equivalence tester's placement_table test (equiv\equiv_tests.cpp placement::Ours) reads "no table" from it.
inline bool PlacementGateRefuses(const std::vector<uint8_t>& b) {
  return b.size() >= 4 && !load_detail::PlacementGateOffPlant() && (int16_t(Rd16(b, 2)) <= 0 || b[0] != 0x01);
}

// The ids of the arena's placement group (the arena block, the third block in the file). false = the bytes are not the loader's three blocks to the last byte (the caller keeps
// its old rule). `found` = the arena has a key (no key: the game places nothing from this block). A table the loader's gate refuses (PlacementGateRefuses): true, not found.
inline bool PlacementIds(const std::vector<uint8_t>& b, int arena, std::vector<int>& ids, bool& found) {
  ids.clear();
  found = false;
  if (PlacementGateRefuses(b)) return true;
  std::vector<Chunk> match_block, arena_block;
  size_t p = 0;
  if (!ReadBlock(b, p, true, match_block) || !ReadBlock(b, p, false, match_block) || !ReadBlock(b, p, true, arena_block) || p != b.size()) return false;
  const int want = RecordKey(arena);
  if (want >= 1000) return true;   // sub_8258CA78 looks up keys below 1000 only
  for (const Chunk& k : arena_block)
    if (k.key == want) {   // the first match (sub_8258CA78 scans from index 0)
      found = true;
      if (k.ref >= 0)
        for (const Chunk& g : arena_block)
          if (g.key == k.ref) {   // the first chunk with that key (sub_8258CA18 scans the same list): ITS records
            ids = g.ids;
            break;
          }
      break;
    }
  return true;
}

inline bool IsStepsId(int id) { return id == 29 || id == 32 || id == 35; }   // the whole step blocks a placement names (sub_825DDAB0's three)
// sub_8258CE20 maps every desk id 6 / 106 / 105 through one table and every top id 7 / 107 / 104 through another (0x82020080 / 0x8202008C), so each set is one slot.
// (Its other skip, a record whose file byte 14 is 1 while the match byte [0x8376AAF4 + 56] is set and [0x8376F384] != 5, is a match state: no desk / step record of the
// player's file has byte 14 set, so it is not modelled here.)
inline bool IsDeskId(int id) { return id == 6 || id == 106 || id == 105; }
inline bool IsTopId(int id) { return id == 7 || id == 107 || id == 104; }

// sub_825DE388 / sub_825DE3C0: the bgEtc PACH 9 entry of the desk / its top for the record's byte 64 / 65 (the image tables 0x82020080 {6, 106, 105} and 0x8202008C {7, 107, 104}).
// The game indexes them with no bound; an index above 2 is on no record of the player's files, and keeps 6 / 7 here.
inline uint32_t DeskKey(int byte64) {
  static const uint32_t t[3] = {6, 106, 105};
  return byte64 >= 0 && byte64 < 3 ? t[byte64] : 6u;
}
inline uint32_t TopKey(int byte65) {
  static const uint32_t t[3] = {7, 107, 104};
  return byte65 >= 0 && byte65 < 3 ? t[byte65] : 7u;
}

// S111 DESKMODEL: a desk / top mesh's place in FILE space (-Y up; the loader mirrors it), by the node its palette slot 0 names (the row the game draws it with, header above).
struct DeskAt {
  float x = 0.f, y = 0.f, z = -760.f;   // the placement records' (0, 0, -760)
};
// The first name of Mesh::palette (the node names of the palette slots joined by ';', slot 0 first).
inline std::string Slot0Node(const std::string& palette) { return palette.substr(0, palette.find(';')); }
inline DeskAt DeskMeshAt(bool top, const std::string& slot0_node) {
  DeskAt a;
  if (top) a.y = -48.25f;                          // the top's bone01 / bone03 row, resting on the desk [L measured, S97: a stand-in]; bone02 the same [L, DESKSKIN: bone03 mirrored]
  else if (slot0_node == "bone01") a.z = -775.4f;  // the desk's bone01 row; bone04 is the place itself [L measured, S97: a stand-in]; bone02 / bone03 too [L, DESKSKIN geometric]
  return a;
}

// S111 DESKSKIN: the node name of palette slot `slot` (Mesh::palette: the slots' node names joined by ';', slot 0 first); empty past the end.
inline std::string SlotNode(const std::string& palette, size_t slot) {
  size_t b = 0;
  for (size_t k = 0; k < slot; ++k) {
    b = palette.find(';', b);
    if (b == std::string::npos) return std::string();
    ++b;
  }
  return palette.substr(b, palette.find(';', b) - b);
}

// S111 DESKSKIN: how far ONE desk / top vertex moves from the place, in file space. The game draws it with the row of its own palette slot's node (sub_82C04DF0 writes one row
// per palette word; the packer keeps the vertex's slot byte), and every row here is a translation, so the move is the weight-mean of its pairs' node rows less the place.
// `slots` / `weights` = its 4 raw pairs (Mesh::src_skin_slots / src_skin_weights, slot 255 = none), `pairs` = Mesh::src_skin_pairs (a one-pair vertex weighs 1, as
// vdecode::GamePairWeight); null `slots`, or no usable pair: its palette slot 0's node (a mesh with no skin stream is drawn on slot 0's row).
inline DeskAt DeskVertexShift(bool top, const std::string& palette, const uint8_t* slots, const float* weights, uint32_t pairs) {
  const DeskAt place;
  float sx = 0.f, sy = 0.f, sz = 0.f, sw = 0.f;
  for (uint32_t k = 0; slots && weights && k < 4 && k < pairs; ++k) {
    if (slots[k] == 255) continue;
    const std::string node = SlotNode(palette, slots[k]);
    const float w = pairs == 1 ? 1.f : weights[k];
    if (node.empty() || !(w > 0.f)) continue;
    const DeskAt a = DeskMeshAt(top, node);
    sx += w * (a.x - place.x), sy += w * (a.y - place.y), sz += w * (a.z - place.z), sw += w;
  }
  DeskAt d;
  if (sw > 0.f) {
    d.x = sx / sw, d.y = sy / sw, d.z = sz / sw;
  } else {
    const DeskAt a = DeskMeshAt(top, Slot0Node(palette));
    d.x = a.x - place.x, d.y = a.y - place.y, d.z = a.z - place.z;
  }
  return d;
}

struct Plan {
  bool read = false;              // both tables read: the fields below are the game's choice
  bool steps = true, desk = true, top = true;   // unread: the old fixed choice (every ring arena)
  uint32_t steps_key = 32;        // bgEtc PACH 9 entry of the steps
  uint32_t desk_key = 6, top_key = 7;   // S111 DESKMODEL: bgEtc PACH 9 entries of the desk and its top (DeskKey / TopKey; unread: 6 / 7)
};

// The arena's plan from misc01_Start p0/2 (`stg`) and bgEtc p10/45010 (`subh`).
inline Plan PlanOf(const std::vector<uint8_t>& stg, const std::vector<uint8_t>& subh, int arena) {
  Plan p;
  std::vector<int> ids;
  bool found = false;
  const int b28 = RecordByte28(stg, arena);
  if (b28 < 0 || !PlacementIds(subh, arena, ids, found)) return p;
  p.read = true;
  p.steps = p.desk = p.top = false;
  for (int v : ids) {
    if (IsStepsId(v)) p.steps = true;
    if (IsDeskId(v)) p.desk = true;
    if (IsTopId(v)) p.top = true;
  }
  p.steps_key = StepsVariant(b28);
  p.desk_key = DeskKey(RecordByte(stg, arena, 64));
  p.top_key = TopKey(RecordByte(stg, arena, 65));
  return p;
}

}  // namespace assets::arena_props
