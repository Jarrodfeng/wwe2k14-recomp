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

// `want_route`: the owner test passed (owner visible and a guest-row body, or for an unlinked B6 piece every visible owner is one) and the game did not hide the piece this frame.
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

// S103 SMF (review A4): the owner of an UNLINKED own-skeleton piece (B6: the record names no owner) is unknown, so the guest route is taken when no visible body could own it from its resident model.
// A referee (slot 6) and a manager are named bodies whose model holds no wrestler-piece mesh (src_kind 2): they cannot own a hair / cap piece, so they do not block the route. The rule is settled from the
// resident model's meshes (not from slot numbers: slots 7 / 8's roles are unknown, match_state.md); an unreadable resident model fails closed (it may own pieces).
enum class BodyKind { Guest, NamedWithPieces, NamedNoPieces, NamedUnknown };
inline bool UnlinkedPieceMayGuestRoute(const BodyKind* bodies, size_t n) {
  bool any_guest = false;
  for (size_t i = 0; i < n; ++i) {
    if (bodies[i] == BodyKind::NamedWithPieces || bodies[i] == BodyKind::NamedUnknown) return false;
    any_guest = any_guest || bodies[i] == BodyKind::Guest;
  }
  return any_guest;
}

}  // namespace rdr_scene::rf
