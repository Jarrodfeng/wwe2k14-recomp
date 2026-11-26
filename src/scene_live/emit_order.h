// S111 CHARORDER (docs\research\s111\CHARORDER.md): the order the wrestler contributor writes its objects in. The colour pass keeps the record order inside the character rank
// (see_through_order.h, Game mode), so this order IS the order the translucent parts are drawn in. Pure: the contributor's EmitItems / StoreVisible and the unit call THIS code.
// The game [C]: every character's translucent node (DRTR, category 33: sub_825ABB98 registers it through sub_82C30A10, an append) is drawn by the category walker sub_82C2FA10 in the
// list's order, i.e. registration order; the match load requests the characters in slot order 0..8 (sub_827E54D0 -> sub_827E4448), so the nodes run in slot order. One node
// (sub_8259FAE8) draws its body's layers 5..7, then the SUBM pieces (sub_8259E488, in instance order = the descriptor order, assets::PieceDrawRank).
// The key: owner slot (the slot a skeleton claims, else the slot of the piece's owner), then inside the slot each claimant by address (the first is the visible body: the claimant
// rule before is unchanged) followed by ITS OWN pieces (the link's owner skeleton, EmitItem::piece_owner; PieceDrawRank, then address), then the skeletons of no slot by address.
// A piece follows its owner, not the slot (S111 CHARORDER review): a draw node is one character (sub_8259FAE8 draws its own character's SUBM component, [char+1152]), and a slot can
// hold two drawn bodies (S111 KEPTPIECES: the editor's kept old character beside the table's new one). A piece whose owner is not named sorts after the slot's claimants.
// EmitBeforeOld = the order before CHARORDER (every claimant before every piece, by address), kept for the unit's planted twin (tools\scene\charorder_unit.cpp --plant).
// S111 CAT33 (docs\research\s111\CAT33.md): the characters run in the order of the game's own category-33 list (char_draw_list.h, read at the latch): EmitOrderKey::rank = the slot's
// rank in it (SlotDrawRanksOf); the listed slots first by rank, then the unlisted ones in slot order. rank -1 everywhere (the one-argument EmitOrderOf, an unread list) = slot order.
#pragma once
#include <cstdint>
#include <vector>

#include "../assets/piece_keys.h"

namespace rdr_scene::live {

struct EmitOrderKey {
  int32_t slot = -1;         // the character slot the skeleton claims; -1 = none
  int32_t piece_slot = -1;   // a linked SUBM piece: its owner's slot; -1 = not a linked piece
  uint32_t piece_id = 0;     // the piece key (SUBM instance +16)
  uint32_t skel = 0;         // the skeleton's address
  uint32_t piece_owner = 0;  // a linked piece: the skeleton of the character whose SUBM list holds it (the link's owner); 0 = not named
  int32_t rank = -1;         // S111 CAT33: the owner slot's place in the game's category-33 list; -1 = not listed (slot order, after the listed)
};

template <class Item>
inline EmitOrderKey EmitOrderOf(const Item& i) { return EmitOrderKey{i.slot, i.piece_slot, i.piece_id, i.skel, i.piece_owner}; }

// S111 CAT33: a slot's rank = the lowest listed place (Item::draw_pos >= 0, the category-33 list position of the claimant's character) among the slot's claimants; -1 = none listed.
// Per slot, so a whole slot moves and the order inside it (the claimant rule) is untouched. A fixed array over the character table's 9 slots (0x836F38D0, lightlatch::kCharSlots):
// no allocation per record (S111 CAT33 review); a slot outside the table has no rank (slot order).
struct SlotDrawRanks {
  static constexpr int32_t kTableSlots = 9;
  int32_t rank[kTableSlots] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
  int32_t Of(int32_t slot) const { return slot >= 0 && slot < kTableSlots ? rank[slot] : -1; }
};

template <class Item>
inline SlotDrawRanks SlotDrawRanksOf(const std::vector<Item>& items) {
  SlotDrawRanks r;
  for (const Item& i : items) {
    if (i.slot < 0 || i.slot >= SlotDrawRanks::kTableSlots || i.draw_pos < 0) continue;
    int32_t& k = r.rank[i.slot];
    if (k < 0 || i.draw_pos < k) k = i.draw_pos;
  }
  return r;
}

template <class Item>
inline EmitOrderKey EmitOrderOf(const Item& i, const SlotDrawRanks& ranks) {
  EmitOrderKey k = EmitOrderOf(i);
  const int32_t o = i.slot >= 0 ? i.slot : i.piece_slot;
  k.rank = o >= 0 ? ranks.Of(o) : -1;
  return k;
}

inline bool EmitBefore(const EmitOrderKey& a, const EmitOrderKey& b) {
  const int32_t oa = a.slot >= 0 ? a.slot : a.piece_slot, ob = b.slot >= 0 ? b.slot : b.piece_slot;
  const bool ha = oa >= 0, hb = ob >= 0;
  if (ha != hb) return ha;                    // the skeletons of no slot last
  if (!ha) return a.skel < b.skel;            // the skeletons of no slot, by address
  if (oa != ob) {                             // character by character
    const bool la = a.rank >= 0, lb = b.rank >= 0;
    if (la != lb) return la;                  // S111 CAT33: the characters the game's category-33 list names first
    if (la && a.rank != b.rank) return a.rank < b.rank;   // in the list's order (the game's draw order)
    return oa < ob;                           // the rest in slot order
  }
  const bool ca = a.slot >= 0, cb = b.slot >= 0;
  const uint32_t ka = ca ? a.skel : (a.piece_owner ? a.piece_owner : 0xFFFFFFFFu), kb = cb ? b.skel : (b.piece_owner ? b.piece_owner : 0xFFFFFFFFu);
  if (ka != kb) return ka < kb;               // inside the slot: each body (by address), its own pieces with it
  if (ca != cb) return ca;                    // the body before its pieces
  if (!ca) {
    const uint32_t ra = assets::PieceDrawRank(a.piece_id), rb = assets::PieceDrawRank(b.piece_id);
    if (ra != rb) return ra < rb;             // the pieces in the game's descriptor order
  }
  return a.skel < b.skel;
}

inline bool EmitBeforeOld(const EmitOrderKey& a, const EmitOrderKey& b) {
  const bool sa = a.slot >= 0, sb = b.slot >= 0;
  if (sa != sb) return sa;
  if (sa && a.slot != b.slot) return a.slot < b.slot;
  return a.skel < b.skel;
}

}  // namespace rdr_scene::live
