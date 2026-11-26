// S103 W2B / ENT (docs\agents\s103\ENT.md): which pac entries of a wrestler attire are models the game builds as SUBM pieces. The game's own key list is the descriptor builder sub_8271EF80
// (0x8271EFA4-0x8271F18C): 32, 26, 27, 28, 29, 33, 1, 20000-20002, 21000-21002, 21010, 21011, 21020, 21030, 30000, 30001 and 10000-10999, plus the body (0); a key is built whenever the attire PACH holds it.
// The old filter here (0, 26, 27, 10000-39999) never loaded 28 / 29 (Foley's Mr. Socko, Savage's glasses, Hogan's neck pieces, Del Rio p3 / Cena Retro with kami_b nodes). Key 1 is a body entry of a PROP
// pac (prop slots 1..9), so it is taken only as the game's piece when the pac is not a prop pac. Pure.
// S111 CHARORDER: PieceDrawRank / OrderPieceMeshes = the descriptor's ORDER (the order the translucent draw draws a character's pieces in).
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace assets {

inline constexpr bool WrestlerEntryIsModel(uint32_t id, bool prop_slots) {
  if (id == 0) return true;
  if (prop_slots) return id >= 1 && id <= 9 ? true : (id == 26 || id == 27 || (id >= 10000 && id < 40000));   // a prop pac keeps its slot ids; the old range for the rest
  return (id >= 26 && id <= 29) || id == 32 || id == 33 || id == 1 || (id >= 20000 && id <= 20002) || (id >= 21000 && id <= 21002) || id == 21010 || id == 21011 || id == 21020 || id == 21030 ||
         id == 30000 || id == 30001 || (id >= 10000 && id <= 10999);
}
// The filter before W2B / ENT (kept for the unit's planted twin).
inline constexpr bool WrestlerEntryIsModelOld(uint32_t id, bool prop_slots) { return id == 0 || id == 26 || id == 27 || (id >= 10000 && id < 40000) || (prop_slots && id >= 1 && id <= 9); }

// S111 CHARORDER (docs\research\s111\CHARORDER.md): a piece key's place in the game's SUBM descriptor = the order the translucent draw draws a character's pieces in (sub_8259ECB0
// builds instance i from entry i; sub_8259E488 walks the instances in index order). sub_8271EF80 writes the entries: the words at 0x82029214 {26, 27, 28, 29, 32}, 33, 1, the
// halfwords at 0x820291A4 {10500, 10501}, 10000..10999 without 10500 / 10501, the words at 0x836247B0 {20000, 20001, 20002, 21000, 21001, 21002, 21010, 21011, 21020, 21030, 30000,
// 30001}. 0 = the body (key 0: first); a key outside the list (the old filter's 10000-39999 extras) after the list, by key.
inline constexpr uint32_t PieceDrawRank(uint32_t id) {
  switch (id) {
    case 0: return 0;
    case 26: return 1;
    case 27: return 2;
    case 28: return 3;
    case 29: return 4;
    case 32: return 5;
    case 33: return 6;
    case 1: return 7;
    case 10500: return 8;
    case 10501: return 9;
    case 20000: return 1010;
    case 20001: return 1011;
    case 20002: return 1012;
    case 21000: return 1013;
    case 21001: return 1014;
    case 21002: return 1015;
    case 21010: return 1016;
    case 21011: return 1017;
    case 21020: return 1018;
    case 21030: return 1019;
    case 30000: return 1020;
    case 30001: return 1021;
    default: break;
  }
  if (id >= 10000 && id <= 10999) return 10 + (id - 10000);   // 10..1009 (10500 / 10501 are placed above)
  return 2000 + id;
}

// A wrestler model's meshes in the game's draw order: the body's, then each piece's by PieceDrawRank (stable: a piece keeps its own mesh order). The loader appends the pieces in id
// order (LoadWrestlerPac); only the meshes move, the merged bones keep the load order. Mesh = any type with src_kind / src_key (2 = a wrestler entry; anything else ranks as the body).
template <class Mesh>
inline void OrderPieceMeshes(std::vector<Mesh>& meshes) {
  std::stable_sort(meshes.begin(), meshes.end(), [](const Mesh& a, const Mesh& b) {
    return PieceDrawRank(a.src_kind == 2 ? uint32_t(a.src_key) : 0u) < PieceDrawRank(b.src_kind == 2 ? uint32_t(b.src_key) : 0u);
  });
}

}  // namespace assets
