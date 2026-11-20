// S105 A5a (docs\research\s105\A5a_ADV.md sites 6-7): may a linked own-skeleton piece of a GUEST-ROW owner (a created superstar's hair / necklace / braces) be drawn from its own latch-read palette?
// The game poses such a piece with its OWN [model+200]: sub_8259D230 copies the owner's rows into it through the node map (verbatim), then the spring (sub_8259D390) runs. Our merged-body route (PieceWhy::Ok)
// cannot reproduce a customised model, so the piece is drawn as a model of its own (the guest route, kObjOwnRowsPiece). What the record must never carry is a piece palette that is not the game's: a
// bind (identity) palette of a piece built after the latch's last SUBM pass, a derived (update-exit) palette kept when the latch read was refused, a palette of another latch than its owner's. Each is a
// refusal here, counted by reason; the piece then stays the plain hole it was (never a wrong pose). Pure: only <cstdint> / <cmath> and the shared piece type; the contributor's EmitItems and the unit call THIS code.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "slot_piece.h"

namespace scene_inputs {

// X(Name, "text"); Hidden is not a refusal: the game itself does not draw the instance (inst +20 == 0), so there is no object and no hole.
#define SCENE_OWN_ROWS_LIST(X) \
  X(Ok, "ok") X(Hidden, "hidden_by_game") X(ProbeUnread, "probe_unread") X(Special, "special_id") X(NoCopyWord, "no_copy_word") X(NoOwnerItem, "no_owner_item") X(OwnerMismatch, "owner_mismatch") \
  X(OwnerNamed, "owner_named") X(NotConsumer, "not_consumer") X(TagDiffers, "tag_differs") X(Stale, "stale") X(NoMappedNode, "no_mapped_node") X(NodeRange, "node_out_of_range") X(RowsDiffer, "rows_differ")

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

// The facts of one (piece, owner) pair in ONE record. The two palettes are `float[16]` rows, `count` rows each (a Mat4 array).
struct OwnRowsCase {
  OwnRowsProbe probe;                // the piece link's latch read of the copy words + one mapped node pair
  bool owner_found = false;          // the slot's drawn owner body is an item of this record
  bool owner_skel_matches = false;   // ... and it is the skeleton the link was read from ([char+48]; the NRBF identity)
  bool owner_named = false;          // the owner is drawn from the file model (a stock body): that is the merged-body route's, not this one's
  bool piece_consumer = false, owner_consumer = false;   // both palettes are latch reads (the game's own source), never the update-exit derived palette
  bool piece_stale = false;          // the piece's palette is older than this record
  uint64_t piece_tag = 0, owner_tag = 0;   // the palettes' frame tags: a latch read carries hook + kPaletteLead, so equal tags = the same latch
  const float* piece_rows = nullptr;       // count x 16 floats
  size_t piece_count = 0;
  const float* owner_rows = nullptr;
  size_t owner_count = 0;
  // S105 A5a F5: the owner's PREVIOUS latch read (the game's copy may run before the owner's own update of the pass: the piece then holds one copy behind) and the translation shift the game adds to +200 after the copy
  const float* prev_owner_rows = nullptr;
  size_t prev_owner_count = 0;
  float shift_tol[3] = {0.f, 0.f, 0.f};   // |recorded shift| of the owner's current + previous palettes, per axis
};

inline constexpr float kOwnRowsTransTol = 1e-3f;   // S105 A5a F5
inline constexpr float kOwnRowsTol = 1e-4f;   // absolute + relative, on the rotation floats (0-11) only: sub_82BD3620 shifts the owner's translation after the copy

// `planted_no_proof` (test only): the old rule, no proof: every piece linked to a guest-row owner is drawn from whatever palette it has (a hidden one included).
inline OwnRowsWhy JudgeOwnRows(const OwnRowsCase& c, bool planted_no_proof = false) {
  if (!c.owner_found) return OwnRowsWhy::NoOwnerItem;
  if (!c.owner_skel_matches) return OwnRowsWhy::OwnerMismatch;
  if (c.owner_named) return OwnRowsWhy::OwnerNamed;
  if (planted_no_proof) return OwnRowsWhy::Ok;   // test only: the old rule, no proof at all: every guest-owner piece with a link is drawn from whatever palette it has
  if (c.probe.special == 0xFFFFFFFFu || c.probe.copy == 0xFFFFFFFFu || c.probe.drawn == 0xFFFFFFFFu) return OwnRowsWhy::ProbeUnread;   // unreadable = never guessed
  if (c.probe.drawn == 0) return OwnRowsWhy::Hidden;   // the SUBM draw loop skips it (sub_8259E488: inst +20 == 0): the game does not draw it
  if (c.probe.special != 0) return OwnRowsWhy::Special;
  if (c.probe.copy == 0) return OwnRowsWhy::NoCopyWord;   // sub_8259D230 does not run: the piece is posed from its own palette (DTP's detached route), not an owner copy
  if (c.piece_stale) return OwnRowsWhy::Stale;
  if (!c.piece_consumer || !c.owner_consumer) return OwnRowsWhy::NotConsumer;
  if (c.piece_tag != c.owner_tag) return OwnRowsWhy::TagDiffers;
  if (c.probe.piece_node == ~0u || c.probe.owner_node == ~0u) return OwnRowsWhy::NoMappedNode;
  if (c.probe.piece_node >= c.piece_count || c.probe.owner_node >= c.owner_count || !c.piece_rows || !c.owner_rows) return OwnRowsWhy::NodeRange;
  const float* a = c.piece_rows + 16 * size_t(c.probe.piece_node);
  // the owner's row k of THIS latch, or of the PREVIOUS one; rotation exactly, translation within the owner's recorded shift (a hair one frame of root motion off is not the game's copy)
  const auto matches = [&](const float* b) {
    for (int k = 0; k < 12; ++k)
      if (!(std::fabs(a[k] - b[k]) <= kOwnRowsTol + kOwnRowsTol * std::fabs(b[k]))) return false;
    for (int k = 0; k < 3; ++k)
      if (!(std::fabs(a[12 + k] - b[12 + k]) <= c.shift_tol[k] + kOwnRowsTransTol)) return false;
    return true;
  };
  if (matches(c.owner_rows + 16 * size_t(c.probe.owner_node))) return OwnRowsWhy::Ok;
  if (c.prev_owner_rows && c.probe.owner_node < c.prev_owner_count && matches(c.prev_owner_rows + 16 * size_t(c.probe.owner_node))) return OwnRowsWhy::Ok;
  return OwnRowsWhy::RowsDiffer;   // a bind palette, a pose of another frame, another owner: not the game's copy
}

}  // namespace scene_inputs
