// S105 A5b: which meshes answer the front end's per-piece row lookup (SceneFrame::piece_alphas, S103 EYF): a wrestler pac entry mesh (src_kind 2), whose src_key is the piece id
// (0 = the body, which no piece row names). A row names a wrestler's own-skeleton PIECE (its model's alpha: the eyelid layer fades with it).
// S111 GUESTKEY: a guest-built mesh carries the same key (guest_model_draw.h BuildModel: 0 for a body part, the piece id for a piece model; the part index is in src_record), so the old
// guest exception (src_pac "guest" answered nothing) and its plant are gone. Pure, no includes of ours: the front end (front_end.cpp EYF lookup) and tools\scene\guest_model_unit.cpp both call it.
#pragma once
#include <cstdint>

namespace rdr_scene {

inline bool MeshAnswersPieceRow(int src_kind) { return src_kind == 2; }

// S111 PIECEROUTE: the meshes a kObjPiecesOnly object draws: its SUBM piece meshes (a piece-row mesh with a piece id), never the body (src_key 0) or a mesh of no wrestler entry.
inline bool PiecesOnlyMeshDrawn(bool answers_piece_row, int src_key) { return answers_piece_row && src_key > 0; }

// S111 SOCKSHADOW (docs\research\s111\SOCKSHADOW.md): the one per-piece row lookup of every pass. The key a mesh is named by: its piece id when it answers the rows (MeshAnswersPieceRow),
// else -1 (no row names it).
inline int PieceRowKey(bool answers_piece_row, int src_key) { return answers_piece_row ? src_key : -1; }

// The FIRST row of (object, key) in `rows` (SceneFrame::piece_alphas: rows with object / piece fields), or null. FrontEnd::Record (the colour pass) and LiveMapsHook::CasterMeshCasts share it.
template <class Rows>
inline const typename Rows::value_type* FindPieceRow(const Rows& rows, uint32_t object, int key) {
  if (key < 0) return nullptr;
  for (const auto& r : rows)
    if (r.object == object && int(r.piece) == key) return &r;
  return nullptr;
}

// THE caster rule: a mesh casts unless its row says the owner's merged mesh does not draw the piece at all (`undrawn`: the game hides it, its SUBM shown word 0, which the piece loop
// sub_8259E488 skips in every node; or it is detached). The row's alpha does not matter here: the caster passes read no model alpha (the model submit sub_82C13C88 passes [model+252] on its
// colour branch only), so a piece faded to alpha 0 still casts.
template <class Rows>
inline bool PieceRowCasts(const Rows& rows, uint32_t object, int key) {
  const auto* r = FindPieceRow(rows, object, key);
  return !r || !r->undrawn;
}

}  // namespace rdr_scene
