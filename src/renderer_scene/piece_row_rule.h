// S105 A5b: which meshes answer the front end's per-piece row lookup (SceneFrame::piece_alphas, S103 EYF).
// A row names a wrestler's own-skeleton PIECE (its model's alpha: the eyelid layer fades with it). A guest-built body (the created superstar's
// 12 parts) is ONE model: its meshes carry src_kind 2 and src_key = the PART INDEX (guest_model_draw.h), which is not a piece id. A piece row for
// piece id N on a guest owner must never hide or fade guest part N (piece id 1 exists: docs\systems\wrestler_pose.md). Pure, no includes of ours:
// the front end (front_end.cpp EYF lookup) and tools\scene\guest_model_unit.cpp both call it.
#pragma once
#include <string>

namespace rdr_scene {

inline int g_piece_row_plant = 0;   // unit plant (guest_model_unit --plant 43): 1 = the old rule (every src_kind 2 mesh answers, guest parts included)

inline bool MeshAnswersPieceRow(int src_kind, const std::string& src_pac) {
  if (src_kind != 2) return false;
  if (g_piece_row_plant == 1) return true;
  return src_pac != "guest";   // a guest part's src_key is its part index: never a piece id
}

}  // namespace rdr_scene
