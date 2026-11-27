// S111 FADEORDER (docs\research\s111\FADEORDER.md): the order of FrontEnd::Record's opaque list around the wrestler bodies, as the game's colour walk draws them.
// Pure header (<algorithm>, <cstdint>, <numeric>, <utility>, <vector>): front_end.cpp FrontEnd::Record calls OrderOpaque; tools\scene\body_order_unit.cpp checks it (planted twins --plant: the order
// before, record order; --plant-slot: the bodies in record order, not by slot; --plant-effect: the Body turn by the material's effect, the rule before S111 FADECAW).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// [C] The colour walk sub_82C2FA10 draws the render categories in ascending order (the match's two-worker table replays its lists ascending too, S110 WALKRACE W7), each node in its category's turn.
//     A slot character's opaque node (char + 1200, slot 1 sub_825A3908) is registered at 13 + slot (sub_823CF100 -> sub_823E6D18). Its body draws with the category reset's z write 1
//     (sub_82C22E88 +0x50; neither the node nor the colour draw sub_82C04DF0 sets z write) and, since S111 ALPHAWREST, blended (the reset's blend on). So a body whose output alpha is below 1
//     shows only what categories 0..12 drew behind it. Stage group 0 and the ring side object (22), the ropes / ladder / Inferno (23), the ring (24), the CWeapons and own nodes (26) and the
//     crowd (27) draw after it, and fail the depth test wherever it is in front.
// [C] Drawn before the bodies: the Elimination Chamber's category-3 node sub_826F8958 (k 0, 2, 4, 6, 7), and the arena's slot-cycle groups (sub_83420CB0, registered at 3, or at 13 when the slot
//     entry's byte +1228 has bit 64: sub_824E82A0 +0x2F4 / +0x300; 13 is the slot-0 body's own category and comes before every other slot's). A cycled slot is drawn only by its group
//     (read_rule.h sel_cycled: sub_824E82A0 gives it the no-pass bit). Inside 13 the list order is the append order (sub_82C2FFC8 appends; a re-key sub_82C30C10 appends to the new list):
//     the reverse re-key after an entrance (sub_827FC1A8) puts the slot-0 body back at the END of 13, after the groups; before any re-key it is the build order [U].
// [C] Two bodies draw in slot order (13 + slot ascending). Our record writes the wrestlers in the game's category-33 order (wrestler_contrib.cpp EmitItems, SlotDrawRanksOf), which is the
//     translucent nodes' order, not the slot's: so the Body turn is ordered by the character slot, each slot's objects (the body, then its pieces) in record order.
// Ours drew every opaque mesh in record order (record_frame_classes.cpp Classes: ring canvas, arena, screens, ring posts, props, then the wrestlers), so a faded body showed the ring, the arena and
// the props through it, where the game shows what was drawn before category 13.
// [C] S111 FADECAW (docs\research\s111\FADECAW.md): the opaque node draws by the character, never by a material's effect: a stock character's body model in one call (sub_825A3908 +0x34C, model
//     vtable +120), a created one's own SUBM pieces (the SUBM component of its char+1152 list, sub_8259E488 with mode 0) and then its body per layer. Every other part of a character is drawn by its
//     translucent node (assets\char_draw_layer.h Translucent), so every OPAQUE mesh of a character's own object is the node's: a created superstar's yDefault part as much as a yCh_ one.
// The rule: three turns: BeforeBodies (a cycled arena slot, or an object rf_props.cpp marks kObjBeforeBodies: a chamber category-3 row), Body (every mesh of a character's own object: the slot body
// and the pieces linked to it; by slot), After (every other opaque mesh); BeforeBodies and After in record order. At output alpha 1 the order changes no pixel except which of two meshes at exactly
// the same depth wins.
#pragma once
#include <algorithm>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

namespace rdr_scene::body_order {

enum class Turn : uint8_t { BeforeBodies = 0, Body = 1, After = 2 };

// TEST ONLY (tools\scene\body_order_unit.cpp --plant-effect): the Body turn by the material's effect (a yCh_ mesh), the rule before S111 FADECAW. Written only by the unit; the game never sets it.
inline bool g_plant_effect_body = false;

// Is an opaque item drawn by a character's opaque node? `char_slot` = the character slot its object holds (piece_owner.h CharSlotOf, called by front_end.cpp FrontEnd::Record: a body "actor:slot:N",
// or a piece linked to its owner, Object::layer = slot + 1); -1 = not a character's object. `char_effect` = its material is a yCh_ effect: read only by the plant.
inline bool BodyObject(int char_slot, bool char_effect) { return g_plant_effect_body ? char_effect : char_slot >= 0; }

// The turn of one opaque item. `body` = BodyObject; `before_bodies` = its object is drawn by a node before category 13 (above).
inline constexpr Turn TurnOf(bool body, bool before_bodies) { return body ? Turn::Body : before_bodies ? Turn::BeforeBodies : Turn::After; }

// TEST ONLY (tools\scene\body_order_unit.cpp --plant-slot): the bodies in record order (the rule as first landed, before the slot order). Written only by the unit; the game never sets it.
inline bool g_plant_body_record_order = false;

// The place of one opaque item: its turn, and inside the Body turn its character slot (13 + slot ascending); a body whose slot is not known (-1) comes after the known ones, in record order.
inline uint32_t PlaceOf(Turn t, int slot = -1) {
  const uint32_t sub = (t != Turn::Body || slot < 0 || slot >= 0xFFFF || g_plant_body_record_order) ? 0xFFFFu : uint32_t(slot);
  return (uint32_t(t) << 16) | sub;
}

// Is `slot` a slot of one of the arena's slot-cycle groups? `recs` = the model's slot records, sorted by slot (pairs {slot, record with a bool `cycled`}).
template <class Records>
inline bool CycledSlot(const Records& recs, int slot) {
  if (slot < 0) return false;
  const auto it = std::lower_bound(recs.begin(), recs.end(), slot, [](const auto& p, int s) { return p.first < s; });
  return it != recs.end() && it->first == slot && it->second.cycled;
}

// TEST ONLY (tools\scene\body_order_unit.cpp --plant): the order before S111 FADEORDER (the opaque list in record order). Written only by the unit; the game never sets it.
inline bool g_plant_record_order = false;

// Reorders `opaque` by place, items of one place in their record order (a stable sort). `place_of(item)` gives an item's PlaceOf. Returns how many items changed place (0 = already in place
// order: nothing is copied).
template <class Item, class PlaceOfItem>
inline size_t OrderOpaque(std::vector<Item>& opaque, PlaceOfItem&& place_of) {
  if (g_plant_record_order || opaque.empty()) return 0;
  std::vector<uint32_t> place(opaque.size());
  bool in_order = true;
  for (size_t i = 0; i < opaque.size(); ++i) {
    place[i] = place_of(static_cast<const Item&>(opaque[i]));
    if (i && place[i] < place[i - 1]) in_order = false;
  }
  if (in_order) return 0;
  std::vector<size_t> idx(opaque.size());
  std::iota(idx.begin(), idx.end(), size_t(0));
  std::stable_sort(idx.begin(), idx.end(), [&place](size_t a, size_t b) { return place[a] < place[b]; });
  std::vector<Item> out;
  out.reserve(opaque.size());
  size_t moved = 0;
  for (const size_t i : idx) {
    if (out.size() != i) ++moved;
    out.push_back(std::move(opaque[i]));
  }
  opaque.swap(out);
  return moved;
}

}  // namespace rdr_scene::body_order
