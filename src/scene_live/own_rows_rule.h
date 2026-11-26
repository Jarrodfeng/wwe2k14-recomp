// S105 A5a (docs\research\s105\A5a_ADV.md sites 6-7): may a linked own-skeleton piece of a GUEST-ROW owner (a created superstar's hair / necklace / braces) be drawn from its own latch-read palette?
// The game draws such a piece with its OWN [model+200] as it stands (sub_8259E488: every instance with +20 / +0 set; sub_8259D230 / sub_8259D390 wrote it in the period). Our merged-body route (PieceWhy::Ok)
// cannot reproduce a customised model, so the piece is drawn as a model of its own (the guest route, kObjOwnRowsPiece). S111 PIECELINK review: the latch reads +200 after every writer of the period
// (db\seams\pose_timing.json), so the piece's own fresh latch read IS the game's palette: its rows are not compared with the owner's (the S105 row compare RowsDiffer / NodeRange / NoMappedNode and the
// NoCopyWord split are deleted). What is still refused, counted by reason, is a palette that is not this frame's read: unreadable instance words, a special-id object, a stale or derived palette, or a
// palette of another latch than its owner's. A refusal after the three owner tests keeps the piece's owner link with kObjOwnRowsRefused (OwnRowsRefusalCarried below): counted on the draw side, never a
// hole, never a guessed owner. Pure: only <cstdint> and the shared piece type; the contributor's EmitItems and the unit call THIS code.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cstddef>
#include <cstdint>

#include "slot_piece.h"

namespace scene_inputs {

// X(Name, "text"); Hidden is not a refusal: the game itself does not draw the instance (inst +20 == 0), so there is no object and no hole.
#define SCENE_OWN_ROWS_LIST(X) \
  X(Ok, "ok") X(Hidden, "hidden_by_game") X(ProbeUnread, "probe_unread") X(Special, "special_id") X(NoOwnerItem, "no_owner_item") X(OwnerMismatch, "owner_mismatch") \
  X(OwnerNamed, "owner_named") X(NotConsumer, "not_consumer") X(TagDiffers, "tag_differs") X(Stale, "stale")

enum class OwnRowsWhy : uint8_t {
#define SCENE_OWN_ROWS_ENUM(name, text) name,
  SCENE_OWN_ROWS_LIST(SCENE_OWN_ROWS_ENUM)
#undef SCENE_OWN_ROWS_ENUM
  kCount
};
inline constexpr const char* kOwnRowsNames[] = {
#define SCENE_OWN_ROWS_NAME(name, text) text,
    SCENE_OWN_ROWS_LIST(SCENE_OWN_ROWS_NAME)
#undef SCENE_OWN_ROWS_NAME
};
static_assert(sizeof kOwnRowsNames / sizeof kOwnRowsNames[0] == size_t(OwnRowsWhy::kCount), "one name per OwnRowsWhy");

// The facts of one (piece, owner) pair in ONE record.
struct OwnRowsCase {
  OwnRowsProbe probe;                // the piece link's latch read of the instance words
  bool owner_found = false;          // the slot's drawn owner body is an item of this record
  bool owner_skel_matches = false;   // ... and it is the skeleton the link was read from ([char+48]; the NRBF identity)
  bool owner_named = false;          // the owner is drawn from the file model (a stock body): that is the merged-body route's, not this one's
  bool piece_consumer = false, owner_consumer = false;   // both palettes are latch reads (the game's own source), never the update-exit derived palette
  bool piece_stale = false;          // the piece's palette is older than this record
  uint64_t piece_tag = 0, owner_tag = 0;   // the palettes' frame tags: a latch read carries hook + kPaletteLead, so equal tags = the same latch
};

// `planted_no_proof` (test only): the old rule, no proof: every piece linked to a guest-row owner is drawn from whatever palette it has (a hidden one included).
inline OwnRowsWhy JudgeOwnRows(const OwnRowsCase& c, bool planted_no_proof = false) {
  if (!c.owner_found) return OwnRowsWhy::NoOwnerItem;
  if (!c.owner_skel_matches) return OwnRowsWhy::OwnerMismatch;
  if (c.owner_named) return OwnRowsWhy::OwnerNamed;
  if (planted_no_proof) return OwnRowsWhy::Ok;   // test only: the old rule, no proof at all: every guest-owner piece with a link is drawn from whatever palette it has
  if (c.probe.special == 0xFFFFFFFFu || c.probe.copy == 0xFFFFFFFFu || c.probe.drawn == 0xFFFFFFFFu) return OwnRowsWhy::ProbeUnread;   // unreadable = never guessed
  if (c.probe.drawn == 0) return OwnRowsWhy::Hidden;   // the SUBM draw loop skips it (sub_8259E488: inst +20 == 0): the game does not draw it
  if (c.probe.special != 0) return OwnRowsWhy::Special;   // [L] inst +8 != 0: sub_82648090 builds another (1120-byte) object for the instance: its +200 is not known to be a piece palette
  if (c.piece_stale) return OwnRowsWhy::Stale;   // no read of this frame: the kept palette is an older pose than the one the game draws
  if (!c.piece_consumer || !c.owner_consumer) return OwnRowsWhy::NotConsumer;   // an update-exit (derived) palette is older than the game's +200 (shifts, CHIK); a derived owner carries another thread's tag, so the frame test below cannot pair them
  if (c.piece_tag != c.owner_tag) return OwnRowsWhy::TagDiffers;   // the piece's read is of another latch than its owner's: a pose of another frame beside the body the game draws it with
  return OwnRowsWhy::Ok;   // the piece's own fresh latch read: the palette sub_8259E488 draws (rows, copy word and node map not compared: S111 PIECELINK review)
}

// S111 PIECELINK (docs\research\s111\PIECELINK.md): the verdicts the record CARRIES as a refused own-rows piece (kObjOwnRowsRefused, pad = piece id, layer = owner slot + 1). They come after the three owner
// tests, so the owner was found, is the skeleton the link was read from ([char+48]) and is a guest-row body: the piece is a created superstar's SUBM instance (the game draws it inside its character's node,
// sub_825A3908) with no fresh latch palette of this frame (unread, special, stale, derived, another latch). Ok is the route itself, Hidden writes no object, and NoOwnerItem / OwnerMismatch / OwnerNamed are no guest-row owner's piece of this record.
inline bool OwnRowsRefusalCarried(OwnRowsWhy w) {
  return w != OwnRowsWhy::Ok && w != OwnRowsWhy::Hidden && w != OwnRowsWhy::NoOwnerItem && w != OwnRowsWhy::OwnerMismatch && w != OwnRowsWhy::OwnerNamed && w != OwnRowsWhy::kCount;
}

}  // namespace scene_inputs
