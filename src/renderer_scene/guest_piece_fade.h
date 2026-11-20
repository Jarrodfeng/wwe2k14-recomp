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

// S105 A5a (A5a_ADV.md sites 10 / 12): a LINKED own-skeleton piece of a guest-row owner (kObjOwnRowsPiece: the contributor proved the owner-row copy). It names its owner (layer = slot + 1), so the unlinked rule above (every visible
// body must be able to own it: one stock opponent with hair blocked EVERY CAW piece in a match) never applies. Route = the owner is a visible guest-row body and the game did not hide the piece (a walked frame with no ref for it);
// hidden = not a hole, not drawn. `plant_unlinked_rule` = the old rule (test only): the route follows UnlinkedPieceMayGuestRoute over the visible bodies.
struct OwnRowsRoute {
  bool hole = false, guest_route = false, not_hole = false;
};
inline OwnRowsRoute DecideOwnRowsRoute(bool owner_visible_guest, bool game_hid_it, const BodyKind* bodies, size_t n, bool plant_unlinked_rule = false) {
  OwnRowsRoute r;
  if (game_hid_it) { r.not_hole = true; return r; }
  r.hole = true;
  r.guest_route = plant_unlinked_rule ? UnlinkedPieceMayGuestRoute(bodies, n) : owner_visible_guest;
  return r;
}

// S105 A5a (site 12): which engine object's light state a guest-routed piece is lit by. The game installs ONE wrestler light per character draw node and draws the SUBM pieces inside it (sub_825A3908), so a piece that names its
// owner (layer != 0) is lit with the owner's state; the latch's synthetic refs are one per actor:slot, none for a piece's own address. An unlinked piece (layer 0) or an owner that is not in the record keeps its own address.
// `plant_own_address` = the old rule (test only): every piece asks for its own address (no state: a hole).
inline uint32_t PieceLightObject(bool piece_from_guest, uint32_t layer, uint32_t own_object, uint32_t owner_object, bool plant_own_address = false) {
  if (plant_own_address || !piece_from_guest || layer == 0 || owner_object == 0) return own_object;
  return owner_object;
}

}  // namespace rdr_scene::rf
