// S103 lane NRBF: the ONE type for a character's SUBM piece instance, shared by the latch reader (inputs_contrib.h), the glue (inputs_glue.h) and the wrestlers contributor (wrestler_contrib.h):
// the three used to keep hand copies of a SlotPiece struct (review minor). Pure: only <cstdint>.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cstdint>
#include <vector>

namespace scene_inputs {

// Why a SUBM piece is (or is not) known to be drawn by its owner's body model. Ok = proven; every other value keeps the piece a HOLE (never guessed). Read from the listings
// (docs\research\s103\NRB_pieces.md section 6): the component update sub_8259DA88 copies, per instance, the owner skeleton's rows into the piece through the node map at inst+12
// (sub_8259D230), and computes the nodes the map lacks (-1) from their parents (sub_8259D390); inst+8 / inst+28 say how sub_8259E720 built the model.
// ONE table (S103 PZF2, review lint): the enum and the log / counter names come from it, so their order cannot drift. X(Name, "text"); the meaning of each:
//   Ok              proven drawn by the owner's body
//   NoCount         the scene core never recorded this model as an own skeleton: nothing to draw or link
//   MapNull         inst+12 == 0: sub_8259D230 and sub_8259D390 skip the instance (the owner never poses it)
//   MapUnreadable   the map words could not be read
//   Unmapped        a node whose map word is -1: computed from its parent (sub_8259D390, mode byte inst+37), not copied: not proven that the owner body draws it
//   Special         inst+8 != 0: the special-id path (ids from 0x8201E5E8 with [char+1220] < 6; 1120-byte object, sub_82648090)
//   NotDeferred     inst+28 != 1: not built from the deferred PACH data. PZ F2: returned only when EVERYTHING ELSE passed (special, copy, map, no -1 node); then ProveBuildPath upgrades path B to Ok
//   NoCopy          inst+24 == 0: sub_8259DA88 skips the owner copy (sub_8259D230)
//   PathA           [desc+144] != 0: sub_822FA3D8 built a CUSTOMISED model (CAW / custom attire): our merged body does not reproduce it
//   WrongDesc       no declared / verified link for the owner body at the latch, the latch's re-read of [desc+4] / [desc+28] disagrees (PZF2 B-2), or the descriptor words are unreadable
//   EntryMismatch   [entry+0] != inst+4 (this instance did not come from this descriptor entry), or inst+0 is neither the entry's model (B1) nor a fresh model of an empty entry (B2)
//   KindUnproven    path B, but the owner's model kind (key % 10) is not one a rig has proven (0 referee, 2 match body, 4 entrance body: piece_link.h kProvenKinds). Fail closed
//   B2Unproven      path B2 ([entry+4] == 0: a fresh model built from the entry): no run has shown a B2 piece yet, so it is refused until one does (PZF2 B-3)
//   CountMismatch   the scene core's wanted node count differs from the game's own count ([model+300], vtable 0x8206AF90): the draw object is not the model we counted
//   OwnerNotDrawn   (contributor) the slot's owner body is not an emitted visible object in this record
//   OwnerMismatch   (contributor) the slot's drawn owner skeleton is not the [char+48] skeleton the link was read from (review F1)
//   Stale           (contributor) the piece's palette is not this record's
#define SCENE_PIECE_WHY_LIST(X) \
  X(Ok, "ok") X(NoCount, "no_count") X(MapNull, "map_null") X(MapUnreadable, "map_unreadable") X(Unmapped, "unmapped_nodes") X(Special, "special_id") X(NotDeferred, "not_deferred") X(NoCopy, "no_copy") \
  X(PathA, "path_a") X(WrongDesc, "wrong_desc") X(EntryMismatch, "entry_mismatch") X(KindUnproven, "kind_unproven") X(B2Unproven, "b2_unproven") X(CountMismatch, "count_mismatch") \
  X(OwnerNotDrawn, "owner_not_drawn") X(OwnerMismatch, "owner_mismatch") X(Stale, "stale")

enum class PieceWhy : uint8_t {
#define SCENE_PIECE_WHY_ENUM(name, text) name,
  SCENE_PIECE_WHY_LIST(SCENE_PIECE_WHY_ENUM)
#undef SCENE_PIECE_WHY_ENUM
  kCount
};

inline constexpr const char* kPieceWhyNames[] = {
#define SCENE_PIECE_WHY_NAME(name, text) text,
    SCENE_PIECE_WHY_LIST(SCENE_PIECE_WHY_NAME)
#undef SCENE_PIECE_WHY_NAME
};
static_assert(sizeof kPieceWhyNames / sizeof kPieceWhyNames[0] == size_t(PieceWhy::kCount), "one name per PieceWhy");

// How the game built the instance's model (sub_8259E720), as ProveBuildPath read it: the one-time `piece link` line prints it (PZF2 B-3).
enum class PieceBuild : uint8_t { Unknown = 0, Deferred = 1, B1 = 2, B2 = 3 };

// S105 A5a (docs\research\s105\A5a_ADV.md sites 1-2): what the latch read of the instance says about whether the game has COPIED the owner's rows into this piece (a CAW hair / necklace: an own-skeleton
// piece of a guest-row body, drawn from its own palette [model+200]). All 0xFFFFFFFF / ~0 = not read. The contributor never trusts a piece's palette without the copy proof (EmitItems: the
// piece's latch row `piece_node` must equal the owner's latch row `owner_node` of the SAME latch).
struct OwnRowsProbe {
  uint32_t drawn = 0xFFFFFFFFu;     // inst +20: 0 = the SUBM draw loop skips the instance (the game does not draw it: no object, not a hole; DTP's ELBW sets only this)
  uint32_t special = 0xFFFFFFFFu;   // inst +8: must be 0 (the special-id path builds another object)
  uint32_t copy = 0xFFFFFFFFu;      // inst +24: the owner-copy word, must be != 0 (sub_8259D230 runs)
  uint32_t piece_node = ~0u;        // the LAST node j of the instance whose map word is not -1 (a deep node: the root's rotation is often identity, which would prove nothing) ...
  uint32_t owner_node = ~0u;        // ... and its map word k: sub_8259D230 copies the owner's row k into the piece's row j verbatim
};

struct CharPiece {
  uint32_t model = 0, id = 0;
  int32_t slot = -1;
  PieceWhy why = PieceWhy::Ok;   // the reader's part of the proof (every reason before OwnerNotDrawn); the contributor adds the rest
  PieceBuild build = PieceBuild::Unknown;   // B1 / B2 / deferred, when the reader got that far
  uint32_t copy = 0xFFFFFFFFu;              // S103 DTP fix: the RAW owner-copy word (instance +24) ProvePiece read; 0xFFFFFFFF = not read (NoCount / CountMismatch). 0 = the game stopped copying the owner's rows into the piece
  uint32_t kind = 0xFFFFFFFFu;              // the model kind (key % 10) from the latch's own descriptor read (B-2); 0xFFFFFFFF = not read
  // S103 W2B (docs\agents\s103\W2.md): the nodes whose map word is -1 (the spring-hair chains: `kami_b*` / `spring*`), ascending, and their finished palette rows read at the latch from [[model+200] + 64 * j]
  // (16 host floats each, memory order). Filled only for a piece the reader proved Ok; empty for a piece with no -1 node. Names are not visible here: the draw side checks the count and the (id, node) tags.
  std::vector<uint32_t> free_nodes;
  std::vector<float> rows;
  uint32_t nodes = 0;      // S103 W2B (review G): the piece model's node count the proof used (the scene core's or the game's own): carried into the BoneTag
  bool rows_unposed = false;   // S103 HGF: the piece was proven and its rows were read, but every one still equalled the bind palette (identity: the game had not run its first spring / SUBM update): the rows are dropped (the piece is absent this latch), why stays Ok
  bool rows_bad = false;   // the piece was proven and has -1 nodes but their rows could not be read: why == MapUnreadable (census only)
  OwnRowsProbe probe;      // S105 A5a: the copy words + one mapped node pair (last member: the aggregate initialisers {model, id, slot, why} stay valid)
};

}  // namespace scene_inputs
