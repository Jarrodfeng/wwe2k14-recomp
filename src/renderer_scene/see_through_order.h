// The order of the see-through draws in our colour pass (Session 103, lane F; docs\renderer\SEE_THROUGH_ORDER.md). Pure: no renderer, no game.
//
// What the game does [C: tools\scene\f_order_stability.py, brock_losswin + brock_losswin_rt, 536 frames with a moving camera, 8,114 object pairs, 0 flipped]:
// the see-through objects (the stage's glass / net / banner pieces, the wrestlers' hair and translucent gear) are drawn in SUBMIT order (the order the scene
// walk reaches the objects), not far to near. Across the groups [C: tools\scene\f_order_census.py --summary, three arenas]: the stage's alpha-blended pieces
// first, then the wrestlers' translucent parts, then every additive piece (light beams, glows). The viewer's own rule (and the front-end's default) is far to
// near, which flips pairs as the camera moves.
//
// Game mode: five ranks, each in the record's own order (a contributor writes objects in the game's walk order; the stable sort keeps it, and keeps a
// two-pass hair piece's front-face pass before its back-face pass; pass 1 culls back faces, i.e. draws the front faces):
//   rank 0  alpha blend, not a character's object   rank 1  alpha blend, a character's own object (SeeCharacter: a slot body or a piece linked to it, whatever its effect; S111 SEECHAR)   rank 2  additive (src alpha / one), not a late node
//   rank 3  alpha blend of a node drawn after category 33 (S111 PODGLASS review: SeeKey::late_node)   rank 4  additive of such a node
//   rank 5  a rank-1 item (a character's own object, a placed CWeapon node pass, a re-keyed body) whose place is in a list the walk draws after 36 (S111 REKEYORDER2: Rekeyed)
// S111 REKEYORDER2 (docs\research\s111\REKEYORDER2.md): the entrance re-key sub_827FBFE0 moves the own nodes to 41, the bodies to 42 and the DRTR / CWDT / WATR nodes to 43, all after the
// arena's `a` group (35) and the chamber's CDTB / CDTA (35 / 36). Their places (scene_live\cat33_places.h ReadPlaces / BodyPlaceOf: list k's word i = k * kMaxNodes + i over kDrawLists, a body
// in 42 = 2 * kMaxNodes - 1) are from kPlaceAfterLateNodes on, so they take the last rank, ordered by place (41 < 42 < 43, as they were inside rank 1). No re-key = no such place = the order before.
// S111 CHAMBERGLASS (docs\research\s111\CHAMBERGLASS.md): the ranks follow the game's categories. The arena's `a` group is category 35 (sub_825BA7F0), drawn before the chamber's
// CDTA (36, the pod glass), so the additive rank comes before a late node's alpha draws; the late node's own additive (the chamber spotlight k 8, drawn after the glass inside CDTA)
// takes the last rank. A frame with no late node keeps the order from before, byte for byte (the additive rank only changed its number).
// Exception (the loader's own blends, not the game's see-through parts): a material the asset loader made blended from an a / s slot (cutout 2) is not a game
// see-through piece; it stays far to near, after the other items of its rank. A prop cut-out the loader made blended (cutout 1) is drawn WITH depth write
// by the front-end (a cut-out, as the game draws it) and stays in the record order.
// Distance mode: far to near over everything (today's behaviour, byte for byte).
// S111 PROPORDER (docs\research\s111\PROPORDER.md, draw_place_rows.h): the character rank is the game's category-33 block. An entrance prop's pass a CWeapon category-33 node draws
// (SeeKey::place_block) joins it whatever its material (the node sets blend itself; WATR's additive pass too: it draws inside category 33, not after it), and inside the rank the items follow their
// running place in the game's list (SeeKey::place), so a prop registered after a character draws after him. No places (an older record, an unread list) = the rule before, byte for byte.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../scene_live/char_draw_list.h"   // S111 REKEYORDER2: the walked lists' place layout (kMaxNodes, kDrawLists)

namespace rdr_scene {

enum class SeeOrder : uint8_t {
  Distance = 0,  // the viewer's rule: far to near (default; today)
  Game = 1,      // the game's: submit order inside the ranks of SeeRank
};

// The mode the environment asks for, for the offline tools: WWE_RDR_ORDER=game | distance (anything else = the frame's own choice). The variable is read once.
inline bool SeeOrderFromEnv(SeeOrder& out) {
  static const int mode = [] {
    const char* e = std::getenv("WWE_RDR_ORDER");
    if (!e) return -1;
    if (std::strcmp(e, "game") == 0) return int(SeeOrder::Game);
    if (std::strcmp(e, "distance") == 0) return int(SeeOrder::Distance);
    return -1;
  }();
  if (mode < 0) return false;
  out = SeeOrder(mode);
  return true;
}

// What the order reads of one see-through item.
struct SeeKey {
  uint8_t blend = 1;       // 1 alpha, 2 additive (the front-end's Item::blend)
  bool character = false;  // the item's object is a character's own object (SeeCharacter; S111 SEECHAR: the class, not the material's effect)
  uint8_t cutout = 0;      // Material::cutout_blend: 2 = an a / s slot blend (stays far to near)
  float dist2 = 0;         // squared distance of the mesh centre to the eye
  // S105 SZW (docs/research/harness/SZW_contract.md, ZW1): the game's order inside its `s` and `a` groups for a cutout == 2 item: -1 = none (far to near, the rule before); else the position in the game's list =
  // (light-set run, first seen first) * 1024 + stage slot, so the group draws in ascending slot number inside light-set runs (sub_825B9F10 + sub_825B9970), never by distance. Filled by FrontEnd::Record only behind wwe2k14_rdr_stage_zwrite.
  int32_t order = -1;
  // S111 PROPORDER (draw_place_rows.h): place = the item's running place in the game's category-33 list (character rank only; FrontEnd::Record fills it in Game order; -1 = none);
  // place_block = an entrance prop pass a CWeapon category-33 node draws (the game draws it inside category 33 under the node's own blend): its draws, alpha or additive, take the character rank.
  int32_t place = -1;
  bool place_block = false;
  // S111 PODGLASS review (scene_record.h kObjNodeLate): a piece a node of a render category AFTER 33 draws under its own blend (the Elimination Chamber's 35 / 36, sub_826F8F88, z write 0; the gm manager's
  // category-36 rows, gm_node_state.h GmNodeObjectFlags): its alpha draws take rank 3, after the character rank and the additive rank (the game's walk draws them after every category-33 draw and after
  // the arena's `a` group, category 35: S111 CHAMBERGLASS); its additive draws (the chamber spotlight) take rank 4, the last.
  bool late_node = false;
};

// TEST ONLY (tools\weapons\podglass_unit.cpp --plant-order): a late node's alpha draws back in rank 0 (the order before the PODGLASS review). The game never sets it.
inline std::atomic<bool> g_plant_late_node_off{false};

// TEST ONLY (tools\scene\proporder_unit.cpp --plant): the places are ignored (an entrance prop's alpha pass back in rank 0 and its additive pass in rank 2: the order before S111 PROPORDER). The game never sets it.
inline std::atomic<bool> g_plant_draw_place_off{false};

// TEST ONLY (tools\weapons\podglass_unit.cpp --plant-stage-a): the ranks before S111 CHAMBERGLASS (a late node's alpha draws before every additive piece, the arena's `a` group among them). The game never sets it.
inline std::atomic<bool> g_plant_late_before_additive{false};

// The ranks before S111 CHAMBERGLASS: read only under g_plant_late_before_additive (after the place_block rank).
inline int SeeRankBeforeChamberGlass(const SeeKey& k) {
  if (k.blend == 2) return 3;
  if (k.late_node && !g_plant_late_node_off.load(std::memory_order_relaxed)) return 2;
  return k.character ? 1 : 0;
}

// S111 CHAMBERGLASS: a late node's additive draw (the chamber spotlight k 8, sub_826F8F88 blend code 1 after the glass): the rank after the late alpha draws.
inline bool LateAdditive(const SeeKey& k) { return k.blend == 2 && k.late_node && !g_plant_late_node_off.load(std::memory_order_relaxed); }

// TEST ONLY (tools\scene\proporder_unit.cpp --plant-rekey): a re-keyed item back in the character rank, before the `a` group and the late nodes (the order before S111 REKEYORDER2). The game never sets it.
inline std::atomic<bool> g_plant_rekeyed_rank_off{false};

// S111 REKEYORDER2: the first place of a walked list the game draws after the late nodes' last category (36: CDTA sub_826F8398, the gm manager's 36 rows): list k = 1 of kDrawLists (category 41);
// a re-keyed body's place (42) is 2 * kMaxNodes - 1, past it too.
inline constexpr uint32_t kLastLateNodeCat = 36;
inline constexpr int32_t kPlaceAfterLateNodes = int32_t(scene_inputs::char_draw_list::kMaxNodes);
static_assert(scene_inputs::char_draw_list::kDrawLists[0] < kLastLateNodeCat && scene_inputs::char_draw_list::kDrawLists[1] > kLastLateNodeCat,
              "list 0 (33) draws before the late nodes, every later walked list after them");
inline constexpr int kRankRekeyed = 5;   // SeeRank's Rekeyed line returns it as a literal (tools\q\qdraworder.py reads a rank line as `return N;`)

// S111 REKEYORDER2: an item of the character rank's families (a character's own object, SeeCharacter; a placed CWeapon node pass or a re-keyed body: place_block) placed in a list after the late nodes:
// the last rank.
inline bool Rekeyed(const SeeKey& k) { return (k.place_block || k.character) && k.place >= kPlaceAfterLateNodes && !g_plant_rekeyed_rank_off.load(std::memory_order_relaxed) && !g_plant_draw_place_off.load(std::memory_order_relaxed); }

// (tools\q\qdraworder_extra.py parse_see_rank reads this body line by line: each rank line `if (COND) return N;`, COND = `&&` terms of `k.FIELD`, `k.FIELD == N`,
// `k.place >= kPlaceAfterLateNodes`, a `(A || B)` group, `!g_plant_*`, or a `Helper(k)` whose body is `return COND;`; a line it cannot read is an ANCHOR problem. place_ranks reads
// SeeThroughBefore's `(ra == 1 || ra == kRankRekeyed)`.)

// TEST ONLY (tools\scene\body_order_unit.cpp --plant-see-effect): the character rank by the material's effect (a yCh_ mesh), the rule before S111 SEECHAR. The game never sets it.
inline std::atomic<bool> g_plant_see_effect{false};

// [C] S111 SEECHAR (docs\research\s111\SEECHAR.md): is a see-through item one of a character's own draws (SeeKey::character, rank 1)? The game's translucent character node sub_8259FAE8 draws a
// created character's body layers 5..7 and every character's own SUBM pieces by the character alone, never by a material's effect: the same class as the opaque Body turn (body_order.h BodyObject).
// `char_slot` = the character slot the item's object holds (piece_owner.h CharSlotOf, front_end.cpp Item::char_slot); -1 = none. `char_effect` = its material is a yCh_ effect: read only by the plant.
inline bool SeeCharacter(int char_slot, bool char_effect) { return g_plant_see_effect.load(std::memory_order_relaxed) ? char_effect : char_slot >= 0; }

inline int SeeRank(const SeeKey& k) {
  if (Rekeyed(k)) return 5;   // S111 REKEYORDER2 (kRankRekeyed): placed in 41 / 42 / 43 by the entrance re-key: after the `a` group (35) and the late nodes (35 / 36), whatever its blend
  if (k.place_block && !g_plant_draw_place_off.load(std::memory_order_relaxed)) return 1;   // S111 PROPORDER review: a CWeapon node's pass draws inside category 33 whatever its blend code (WATR sub_8272C198: code 1)
  if (g_plant_late_before_additive.load(std::memory_order_relaxed)) return SeeRankBeforeChamberGlass(k);
  if (LateAdditive(k)) return 4;
  if (k.blend == 2) return 2;   // S111 CHAMBERGLASS: the additive rank (the arena's `a` group, category 35) before a late node's 35 / 36 alpha draws
  if (k.late_node && !g_plant_late_node_off.load(std::memory_order_relaxed)) return 3;   // S111 PODGLASS review: after the character rank
  return k.character ? 1 : 0;
}

inline bool SeeThroughBefore(SeeOrder mode, const SeeKey& a, const SeeKey& b) {
  if (mode == SeeOrder::Game) {
    const int ra = SeeRank(a), rb = SeeRank(b);
    if (ra != rb) return ra < rb;
    const bool sa = a.cutout == 2, sb = b.cutout == 2;
    if (sa != sb) return sb;          // the loader's slot blends after the game's own pieces of the rank
    if ((ra == 1 || ra == kRankRekeyed) && !sa && a.place != b.place && !g_plant_draw_place_off.load(std::memory_order_relaxed)) return a.place < b.place;   // S111 PROPORDER: the character rank in the game's category-33 list order (equal places keep the record order); S111 REKEYORDER2: the re-keyed rank in 41 / 42 / 43 order
    if (sa && a.order >= 0 && b.order >= 0) return a.order < b.order;   // S105 SZW: the game's own list order (slot ascending inside light-set runs), not by distance
    return sa && a.dist2 > b.dist2;   // ... far to near among themselves; everything else keeps the record order
  }
  return a.dist2 > b.dist2;
}

// Stable-sorts a list of items; `key_of(item)` gives its SeeKey. The input order is the record's submit order.
template <class T, class KeyOf>
void SortSeeThrough(SeeOrder mode, std::vector<T>& items, KeyOf key_of) {
  std::stable_sort(items.begin(), items.end(), [&](const T& a, const T& b) { return SeeThroughBefore(mode, key_of(a), key_of(b)); });
}

}  // namespace rdr_scene
