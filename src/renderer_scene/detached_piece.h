// S103 DTP (docs\research\s103\PEP.md section 3): a wrestler piece the game DETACHES from its owner. The People's Elbow: the ELBW object sets the SUBM piece's owner-copy flag (instance +24) to 0 and poses the same model
// itself (hand, then floor). The contributor flags such a piece kObjDetachedPiece (pad = piece id, layer = owner slot + 1); the draw side must (1) tell the OWNER's merged mesh to skip the piece (an alpha-0 piece row,
// front_end.cpp piece_alphas) and (2) draw the piece from its own rows (rf_wrestlers.cpp, the DLW guest route). This is (1) as a pure function, so a unit can plant "the flag is ignored" and watch it fail.
// No game or SDK includes.
#pragma once
#include <cstdint>

#include <atomic>
#include "scene_record.h"

namespace rdr_scene {

// S103 DTP: --wwe2k14_rdr_detached_pieces (default on; start_settings.cpp stores it). Off = a piece the game stopped copying the owner's rows into is a hole again (the old picture: the pad stays on the arm).
inline std::atomic<bool> g_detached_pieces{true};

}  // namespace rdr_scene

namespace rdr_scene::rf {

struct DetachedOwnerRowOut { unsigned slot = 0, piece = 0; float alpha = 1.f; };

// The owner-mesh row a detached piece asks for: {owner slot, piece id, alpha 0}. false = this object is not a (usable) detached piece (not flagged, or the record does not name its piece id / owner slot: an older
// record: no row, the piece stays a hole as before). `honour_flag` false = the planted twin that ignores the flag: never a row.
inline bool DetachedOwnerRow(uint32_t flags, uint16_t pad, uint8_t layer, bool honour_flag, DetachedOwnerRowOut& row) {
  if (!honour_flag || !(flags & kObjDetachedPiece) || pad == 0 || layer == 0) return false;
  row.slot = unsigned(layer) - 1u, row.piece = unsigned(pad), row.alpha = 0.f;
  return true;
}

}  // namespace rdr_scene::rf
