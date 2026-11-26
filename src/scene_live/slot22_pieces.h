// S111 PIECEROUTE (docs\research\s111\PIECEROUTE.md): a slot character's SUBM pieces drawn by its translucent node with no body.
// The game [C listings]: the DRTR node (sub_825ABB98, render category 33, its own enable word +8) calls slot 22 of the character. Slot 22 of kind 4 (sub_829D2750, vtable 0x82054EA0) and
// kind 2 (sub_833683F8, vtable 0x821B73F0) gates only on char+3888 == 0, char+224 != 0, char+232 != 0 and draws the SUBM pieces (sub_8259E488): each piece's OWN model, whole (the owner's
// slot 29 predicate is 0: sub_8293A178 -> sub_8259E008 +28C..+31C), posed from the owner's rows the SUBM update copies in (sub_8259DA88). It reads no char+1208 and draws no body. So when
// the opaque node drew no body (no draw ask: char+1208 off, or kind 4 with char+6192 in {1, 2}) and the DRTR node was on, the picture holds the pieces alone.
// Ours: the owner's merged model holds those piece meshes (src_kind 2, src_key = the piece id) and draws them with the owner's palette (kObjDrawnByOwner). The owner object is written
// kObjPiecesOnly: the colour pass draws its piece meshes only (piece_row_rule.h PiecesOnlyMeshDrawn), map B and the rim map cast them (renderer_scene\wrestler_caster.h ObjectMayCast: both walk the
// DRTR node), every other map loop skips it (they test kObjVisible). Slot 22 also installs the character's light-set object (sub_827F6910 +44); ours lights the pieces with the owner's latched block.
// The base class's slot 22 (sub_8259FAE8) has three more gates we do not read: not routed (a counted hole), so the class is part of Gates.
// Pure: the latch (inputs_contrib.h) and the contributor (wrestler_contrib.cpp Collect) call it; tools\scene\wrestler_emit.cpp proves it, with the planted twin kFaultPiecesNoGates.
#pragma once
#include <cstdint>

namespace rdr_scene::live::slot22 {

inline constexpr uint32_t kKind4Vtable = 0x82054EA0u;   // [char+0] of a kind-4 character (sub_829D2278 +38)
inline constexpr uint32_t kKind2Vtable = 0x821B73F0u;   // [char+0] of a kind-2 character (sub_83367DA0 +34)

// Slot 22 of this character draws its SUBM pieces: a kind-4 / kind-2 class and slot 22's own gate (sub_829D2750 +1C / +28 / +34; sub_833683F8 the same words).
inline bool Gates(uint32_t vtable, uint32_t w3888, uint32_t w224, uint32_t w232) {
  return (vtable == kKind4Vtable || vtable == kKind2Vtable) && w3888 == 0 && w224 != 0 && w232 != 0;
}

// One slot body the contributor's quiet gate wrote invisible (no draw ask in the record's period).
struct OwnerCase {
  int32_t hid_slot = -1;     // the slot the quiet gate took from it (-1 = not quiet-hidden)
  bool shown_read = false;   // the latch labelled with the record's hook read the category-33 list (wrestler_contrib.h DrtrOn::read)
  bool drtr_on = false;      // its DRTR node was listed in that walk (DrtrOn::on)
  bool gates = false;        // Gates held at that latch (DrtrOn::pieces)
  bool named = false;        // the record names its model: a resident merged model holds the piece meshes (a guest-row owner's pieces take the own-rows route)
  bool slot_taken = false;   // another item claims that slot in this record
};

// The owner goes out as its slot's body, pieces only. `plant_no_gates` (test only) = the twin: slot 22's gate ignored (pieces drawn although the game's slot 22 returns at once).
inline bool PiecesOnly(const OwnerCase& c, bool plant_no_gates = false) {
  return c.hid_slot >= 0 && c.shown_read && c.drtr_on && (c.gates || plant_no_gates) && c.named && !c.slot_taken;
}

}  // namespace rdr_scene::live::slot22
