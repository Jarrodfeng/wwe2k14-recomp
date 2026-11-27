// S111 PORTRAITHAIR (docs\research\s111\PORTRAITHAIR.md): the ONE rule that links a record's SUBM piece object (hair, caps, accessories) to its owner body. Pure: no renderer, no game.
// The wrestler contributor (scene_live\wrestler_contrib.cpp EmitItems) writes a linked piece as an invisible object under one of the five link flags with Object::layer = the owner's
// character slot + 1, and the owner body as the visible (or pieces-only) object "actor:slot:N". rf_wrestlers.cpp ApplyWrestlers finds a piece's owner by this rule (its owner map and the
// owner's light / draw place), and the roster portrait (live_scene_draw.cpp LiveSceneDraw::DrawPortrait) keeps the edited body's pieces by it: the game's portrait draw (the character's
// slot 22, sub_8259FAE8) draws the character's own SUBM pieces with it.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "crowd_fast.h"   // fast::ScanUInt: what sscanf("%u") reads
#include "scene_record.h"

namespace rdr_scene {

// unit plants (tools\renderer\portrait_pieces_unit.py, -DWWE2K14_PIECE_OWNER_PLANT=N): 1 the kept set holds the bodies only (the old portrait rule); 2 every linked piece is kept, whatever
// its owner slot; 3 the owner slot is read as the layer itself (no "- 1"); 4 (tools\scene\build_body_order_unit.ps1) CharSlotOf reads the layer of ANY object (no link-flag test)
#ifndef WWE2K14_PIECE_OWNER_PLANT
#define WWE2K14_PIECE_OWNER_PLANT 0
#endif

// The flags under which Object::layer names a piece's owner slot + 1 (scene_record.h ObjectFlags). S111 SOCKPIECE: kObjPieceHidden too (piece_hidden.h HiddenOwnerRow: the owner's
// alpha-0 row; a portrait keeps it with its body, so the body skips the hidden piece there as well).
inline constexpr uint32_t kObjPieceLinkFlags = kObjDrawnByOwner | kObjDetachedPiece | kObjOwnRowsPiece | kObjOwnRowsRefused | kObjPieceHidden;

// The owner slot a piece object names: an invisible object under a link flag with layer != 0. False for every other object.
inline bool PieceOwnerSlot(uint32_t flags, uint8_t layer, uint32_t& slot) {
  if ((flags & kObjVisible) || !(flags & kObjPieceLinkFlags) || layer == 0) return false;
  slot = WWE2K14_PIECE_OWNER_PLANT == 3 ? uint32_t(layer) : uint32_t(layer) - 1u;
  return true;
}

// The slot an owner body holds: "actor:slot:N", visible or pieces-only (S111 PIECEROUTE: a pieces-only owner draws its pieces too). False for every other object.
inline bool OwnerBodySlot(std::string_view id, uint32_t flags, uint32_t& slot) {
  constexpr std::string_view kPrefix = "actor:slot:";
  return id.substr(0, kPrefix.size()) == kPrefix && (flags & (kObjVisible | kObjPiecesOnly)) && fast::ScanUInt(id, kPrefix.size(), slot);
}

// S111 FADECAW review: the character slot a drawn object holds (body_order.h BodyObject: the Body turn of the opaque list is exactly the objects with one; S111 SEECHAR: see_through_order.h
// SeeCharacter, the see-through character rank, the same). An owner body "actor:slot:N"
// (OwnerBodySlot; `id` = "" when the frame carries no names: the viewer's own frame), else a piece linked to its owner (a link flag, Object::layer = owner slot + 1; visible once guest-routed,
// so no visibility test here, unlike PieceOwnerSlot). -1 = not a character's object: an arena / crowd object's layer (its blend / LOD) names no slot.
inline int CharSlotOf(std::string_view id, uint32_t flags, uint8_t layer) {
  uint32_t s = 0;
  if (OwnerBodySlot(id, flags, s)) return int(s);
  return ((flags & kObjPieceLinkFlags) || WWE2K14_PIECE_OWNER_PLANT == 4) && layer ? int(layer) - 1 : -1;
}

// One record object as the kept-set rule reads it.
struct KeepRow {
  bool body = false;       // the caller keeps this object for itself (the portrait: its engine object is the requested model)
  std::string_view id;     // the object's id string
  uint32_t flags = 0;      // Object::flags
  uint8_t layer = 0;       // Object::layer
};

// The kept set of a one-character draw: every body row, and every piece row whose owner slot is the slot of a kept body. 1 = kept, per row.
inline std::vector<uint8_t> KeptWithPieces(const std::vector<KeepRow>& rows) {
  std::vector<uint8_t> keep(rows.size(), 0);
  std::vector<uint32_t> owner_slots;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (!rows[i].body) continue;
    keep[i] = 1;
    uint32_t s = 0;
    if (OwnerBodySlot(rows[i].id, rows[i].flags, s)) owner_slots.push_back(s);
  }
  if (WWE2K14_PIECE_OWNER_PLANT == 1) return keep;
  for (size_t i = 0; i < rows.size(); ++i) {
    uint32_t s = 0;
    if (keep[i] || !PieceOwnerSlot(rows[i].flags, rows[i].layer, s)) continue;
    bool owned = WWE2K14_PIECE_OWNER_PLANT == 2;
    for (uint32_t o : owner_slots) owned = owned || o == s;
    if (owned) keep[i] = 1;
  }
  return keep;
}

}  // namespace rdr_scene
