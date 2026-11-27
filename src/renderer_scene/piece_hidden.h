// S111 SOCKPIECE (docs\research\s111\SOCKPIECE.md): a wrestler piece the game HIDES while its owner is drawn. The game draws a SUBM piece instance only while its word +20 is non-zero (the SUBM draw
// loop sub_8259E488 skips +20 == 0; the build sub_8259E720 stores 1). Every character show / hide call (sub_8251E510, sub_8251EDD8, sub_826D3CF8) sets entries 28 (the hand item: Mick Foley's
// and Santino's sock) and 29 (neck / head gear) to 0, and the motion event 169 (sub_823F5A00) sets a piece's word from the move: the sock shows only inside the move. A NAMED owner's model is merged (every piece mesh
// of the attire in one drawable), so it draws such a piece unless told not to. The contributor (wrestler_contrib.cpp EmitItems) writes a hidden piece as an invisible object flagged kObjPieceHidden
// (pad = piece id, layer = owner slot + 1, no bones); the draw side (rf_wrestlers.cpp ApplyWrestlers, the pre-pass) turns it into an alpha-0 row on the owner, and the front end skips that piece's
// meshes (FrontEnd::Record, skipped_alpha0), as for a detached piece (detached_piece.h). One rule for every piece id. No game or SDK includes.
#pragma once
#include <cstdint>

#include "detached_piece.h"
#include "piece_owner.h"   // S111 PORTRAITHAIR: PieceOwnerSlot, the one owner rule
#include "scene_record.h"

namespace rdr_scene {

// The contributor's rule: the owner's merged mesh must skip this piece. drawn_word = the latch's read of instance +20 (OwnRowsProbe::drawn; 0xFFFFFFFF = unread, never a guessed hide); piece_id 0 =
// unknown (no pad to name the mesh by); owner_is_link_skel = the slot's drawn body is the skeleton the piece's link was read from; owner_named = the record names that body's model (its merged mesh
// holds the stock piece meshes; a guest-row body's meshes carry src_key 0 and its pieces are objects of their own, which the own-rows route already leaves out when hidden).
inline bool OwnerMeshHidesPiece(uint32_t drawn_word, uint32_t piece_id, bool owner_is_link_skel, bool owner_named) {
  return drawn_word == 0 && piece_id != 0 && owner_is_link_skel && owner_named;
}

}  // namespace rdr_scene

namespace rdr_scene::rf {

// The owner-mesh row a hidden piece asks for: {owner slot, piece id, alpha 0}. false = this object is not a usable hidden piece (not flagged, visible, or no piece id / owner slot).
// The owner slot is piece_owner.h PieceOwnerSlot's (the rule every piece object's owner is found by). `honour_flag` false = the planted twin that ignores the flag
// (rf::Plants().ignore_game_hide): never a row, so the owner's mesh draws the sock again.
inline bool HiddenOwnerRow(uint32_t flags, uint16_t pad, uint8_t layer, bool honour_flag, DetachedOwnerRowOut& row) {
  uint32_t slot = 0;
  if (!honour_flag || !(flags & kObjPieceHidden) || pad == 0 || !PieceOwnerSlot(flags, layer, slot)) return false;
  row.slot = slot, row.piece = unsigned(pad), row.alpha = 0.f;
  return true;
}

}  // namespace rdr_scene::rf
