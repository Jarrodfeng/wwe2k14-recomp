// S103 SMF (BUG2 finding 1 + 2): the fade and the route of an own-skeleton piece drawn from its OWN guest rows (DLW's guest route).
// The game fades each piece with its own [model+252] row (EYF: the eyelid / lash layer is alpha 0 in entrances). The guest route used to draw the piece at alpha 1 (the black eyes bug, back for CAW / DLC bodies).
// Pure functions: rf_wrestlers.cpp (the route) and tools\scene\guest_route_unit.cpp (the offline proof, with its planted twin) share them.
#pragma once
#include "piece_alpha.h"

namespace rdr_scene::rf {

struct GuestPieceFade {
  bool route = false;   // draw the piece from its own guest rows
  bool hole = false;    // the piece is a hole (the game draws it and nothing of ours does): no readable [model+252] row = fade unknown
  float alpha = 1.f;    // the object's tint alpha when it is drawn
};

// `want_route`: the owner test passed (the piece names its owner and that owner is a visible guest-row body) and the game did not hide the piece this frame.
// `plant_no_alpha` = the bug (every guest-routed piece at alpha 1, no row test); set by the unit's twin, never by the game.
inline GuestPieceFade DecideGuestPieceFade(bool want_route, const PieceAlphaValue& row, bool plant_no_alpha = false) {
  GuestPieceFade r;
  if (!want_route) { r.hole = true; return r; }                     // not ours to draw: stays the hole it was
  if (plant_no_alpha) { r.route = true, r.hole = true; return r; }   // the bug: alpha 1, whatever the row says
  if (!row.found) { r.hole = true; return r; }                       // fade unknown: a named hole, never drawn opaque
  if (!(row.alpha > 0.f)) return r;                                  // faded out by the game: not drawn, not a hole
  r.route = true, r.hole = true, r.alpha = row.alpha;               // (hole stays true: the caller clears it once the guest rows are found)
  return r;
}

// S105 A5a (A5a_ADV.md sites 10 / 12): a LINKED own-skeleton piece of a guest-row owner (kObjOwnRowsPiece: the contributor proved the owner-row copy). It names its owner (layer = slot + 1), so it takes the guest route beside
// whatever other bodies are in the picture. Route = the owner is a visible guest-row body and the game did not hide the piece (a walked frame with no ref for it); hidden = not a hole, not drawn.
// (S111 PIECELINK: the S103 SMF rule for an UNLINKED piece, UnlinkedPieceMayGuestRoute over the visible body kinds, is deleted: every SUBM piece is linked by the reader, the record keeps the link, and the game has no
// piece without an owner, sub_825A3908.)
struct OwnRowsRoute {
  bool hole = false, guest_route = false, not_hole = false;
};
inline OwnRowsRoute DecideOwnRowsRoute(bool owner_visible_guest, bool game_hid_it) {
  OwnRowsRoute r;
  if (game_hid_it) { r.not_hole = true; return r; }
  r.hole = true;
  r.guest_route = owner_visible_guest;
  return r;
}

// S111 PIECELINK: an own-skeleton piece with none of the three route flags (own-rows, detached, drawn-by-owner). `refused` = kObjOwnRowsRefused (a created superstar's SUBM piece with no fresh latch palette of this
// frame, own_rows_rule.h; it carries its owner), `game_draws_it` = SkelPieceIsHole (the game drew it this frame and the ring family does not). A refusal is counted and never a hole (the contributor counted its reason);
// any other such piece is in no character's SUBM list: drawn by the game = a named hole, never a guess at its owner. `plant_refusal_is_hole` = the old outcome (test only): the refused piece went out unflagged.
// S111 PIECEHOLES (docs\research\s111\PIECEHOLES.md): `game_undrawn` = kObjGameUndrawn, a slot body the contributor's quiet gate wrote invisible (no draw ask of the record's period stamped: AskedFor). `walked` =
// the record has walked evidence at the stamp (rf_wrestlers WalkedStamp). With no walk (an OURS frame: synthetic refs only, so `game_draws_it` is true for every such object) the ask is all there is: GameUndrawn,
// never a hole. On a walked frame (a frame the game walked: in the stream-free build too, NotReady / Hold) the walk decides as before: a walked ref for the body = the game drew it although no ask was stamped
// (an ask NoteDrawAsked filtered or could not read): a hole, fail closed. `plant_undrawn_is_hole` = the old outcome (test only): the flag ignored.
enum class UnroutedPiece { Refused, NotHole, Hole, GameUndrawn };
inline UnroutedPiece DecideUnroutedPiece(bool refused, bool game_draws_it, bool plant_refusal_is_hole = false, bool game_undrawn = false, bool walked = false, bool plant_undrawn_is_hole = false) {
  if (refused && !plant_refusal_is_hole) return UnroutedPiece::Refused;
  if (game_undrawn && !walked && !plant_undrawn_is_hole) return UnroutedPiece::GameUndrawn;
  return game_draws_it ? UnroutedPiece::Hole : UnroutedPiece::NotHole;
}

// S105 A5a (site 12): which engine object's light state a guest-routed piece is lit by. The game installs ONE wrestler light per character draw node and draws the SUBM pieces inside it (sub_825A3908), so a guest-routed piece
// (every one names its owner: S111 PIECELINK deleted the unlinked route and its layer-0 case) is lit with the owner's state; the latch's synthetic refs are one per actor:slot, none for a piece's own address. An owner that is
// not in the record keeps the piece's own address. `plant_own_address` = the old rule (test only): every piece asks for its own address (no state: a hole).
inline uint32_t PieceLightObject(bool piece_from_guest, uint32_t own_object, uint32_t owner_object, bool plant_own_address = false) {
  if (plant_own_address || !piece_from_guest || owner_object == 0) return own_object;
  return owner_object;
}

}  // namespace rdr_scene::rf
