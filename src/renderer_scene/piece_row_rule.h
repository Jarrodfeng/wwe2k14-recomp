// S105 A5b: which meshes answer the front end's per-piece row lookup (SceneFrame::piece_alphas, S103 EYF): a wrestler pac entry mesh (src_kind 2), whose src_key is the piece id
// (0 = the body, which no piece row names). A row names a wrestler's own-skeleton PIECE (its model's alpha: the eyelid layer fades with it).
// S111 GUESTKEY: a guest-built mesh carries the same key (guest_model_draw.h BuildModel: 0 for a body part, the piece id for a piece model; the part index is in src_record), so the old
// guest exception (src_pac "guest" answered nothing) and its plant are gone. Pure, no includes of ours: the front end (front_end.cpp EYF lookup) and tools\scene\guest_model_unit.cpp both call it.
#pragma once

namespace rdr_scene {

inline bool MeshAnswersPieceRow(int src_kind) { return src_kind == 2; }

// S111 PIECEROUTE: the meshes a kObjPiecesOnly object draws: its SUBM piece meshes (a piece-row mesh with a piece id), never the body (src_key 0) or a mesh of no wrestler entry.
inline bool PiecesOnlyMeshDrawn(bool answers_piece_row, int src_key) { return answers_piece_row && src_key > 0; }

}  // namespace rdr_scene
