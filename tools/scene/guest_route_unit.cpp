// guest_route_unit: the offline proof of lane SMF fixes 1 + 2 (Session 103, docs\research\s103\BUG2.md finding 1): an own-skeleton piece drawn through the guest route takes its OWN [model+252] fade
// (EYF's rule; the eyelid / lash layer is alpha 0 in entrances) instead of alpha 1. No record, no game files: the decision is the pure function in src\renderer_scene\guest_piece_fade.h.
// Usage: guest_route_unit [--plant no_alpha] [--expect-fail]     Exit 0 = every case holds (with --expect-fail: the planted bug was caught).
// Build: tools\scene\build_guest_route_unit.ps1 (header only).
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "../../src/renderer_scene/guest_piece_fade.h"

using namespace rdr_scene::rf;

int main(int argc, char** argv) {
  bool plant = false, expect_fail = false, plant_unlinked_rule = false, plant_own_address = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) {
      const char* p = argv[++i];
      plant = !std::strcmp(p, "no_alpha");
      plant_unlinked_rule = !std::strcmp(p, "own_unlinked_rule");   // S105 A5a: the old rule for a linked CAW piece (the unlinked owner-kinds rule)
      plant_own_address = !std::strcmp(p, "own_light_address");     // S105 A5a: the piece asks for the light state of its own address
    } else if (!std::strcmp(argv[i], "--expect-fail")) expect_fail = true;
  }
  int bad = 0;
  auto check = [&](bool ok, const char* what) { std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++bad; };
  PieceAlphaValue none, zero, half, one;
  zero.found = true, zero.alpha = 0.f;
  half.found = true, half.alpha = 0.5f;
  one.found = true, one.alpha = 1.f;
  GuestPieceFade r = DecideGuestPieceFade(true, zero, plant);
  check(!r.route && !r.hole, "alpha-0 piece (the eyelid / lash layer in entrances): not drawn, not a hole");
  r = DecideGuestPieceFade(true, none, plant);
  check(!r.route && r.hole, "no readable [model+252] row: a hole, never drawn opaque");
  r = DecideGuestPieceFade(true, half, plant);
  check(r.route && r.alpha == 0.5f, "alpha 0.5 piece: drawn at 0.5");
  r = DecideGuestPieceFade(true, one, plant);
  check(r.route && r.alpha == 1.f, "alpha 1 piece: drawn at 1");
  r = DecideGuestPieceFade(false, one, plant);
  check(!r.route && r.hole, "route not wanted (resident named owner, hidden piece, owner not visible): stays a hole, no guest draw");
  // S103 SMF review A4: the unlinked (B6) piece's route over the visible bodies. A plant (the old "every body must be a guest body" rule) makes the referee block the route.
  auto may = [&](std::initializer_list<BodyKind> k) {
    std::vector<BodyKind> v(k);
    if (plant)   // the old rule
      for (BodyKind b : v) if (b != BodyKind::Guest) return false;
    return UnlinkedPieceMayGuestRoute(v.data(), v.size());
  };
  check(may({BodyKind::Guest, BodyKind::Guest, BodyKind::NamedNoPieces}), "CAW vs CAW with a referee (a named body with no piece mesh): the guest route is kept");
  check(!may({BodyKind::Guest, BodyKind::NamedWithPieces, BodyKind::NamedNoPieces}), "CAW vs a disc wrestler (resident model with piece meshes): no guest route (no double draw)");
  check(!may({BodyKind::Guest, BodyKind::NamedUnknown}), "a named body whose model cannot be read: fail closed");
  check(!may({BodyKind::NamedNoPieces}), "no guest body at all: no route");
  // entry: rdr_scene::rf::DecideOwnRowsRoute / PieceLightObject, called directly: rf_wrestlers.cpp ApplyWrestlers calls them with locals of a whole record walk (owner_obj, PieceSeen, the model cache), which an offline unit cannot build; both are pure.
  // S105 A5a: a LINKED own-rows piece (a CAW's hair / necklace / braces, kObjOwnRowsPiece) names its owner, so one stock opponent with hair in the picture must not block it (the match case), and it is lit with the OWNER's state.
  {
    const BodyKind caw_vs_stock[] = {BodyKind::Guest, BodyKind::NamedWithPieces, BodyKind::NamedNoPieces};
    OwnRowsRoute o = DecideOwnRowsRoute(true, false, caw_vs_stock, 3, plant_unlinked_rule);
    check(o.hole && o.guest_route && !o.not_hole, "A5a: a linked piece of a guest-row owner takes the guest route beside a stock opponent that has piece meshes (a match): the unlinked rule would refuse it");
    o = DecideOwnRowsRoute(true, true, caw_vs_stock, 3, plant_unlinked_rule);
    check(!o.hole && !o.guest_route && o.not_hole, "A5a: the game hid the piece this frame (a walked frame with no ref for it): not a hole, not drawn");
    o = DecideOwnRowsRoute(false, false, caw_vs_stock, 3, plant_unlinked_rule);
    check(o.hole && !o.guest_route, "A5a: the owner slot is not a visible guest-row body: no route (a hole, never a draw beside a body that draws it itself)");
    const uint32_t kOwnerObj = 0xA0001000u, kPieceObj = 0xA0002000u;
    const auto lit = [&](uint32_t addr) { return addr == kOwnerObj; };   // the latch's synthetic refs: one per actor:slot (the owner), none for a piece address
    check(lit(PieceLightObject(true, 1, kPieceObj, kOwnerObj, plant_own_address)), "A5a: a guest-routed piece that names its owner is lit with the OWNER's light state (one wrestler light per draw node)");
    // review F8 d: the stock DTP detached piece (The Rock's People's Elbow pad: kObjDetachedPiece, layer = owner slot + 1) and an SMF guest-routed piece of a failed-name owner take the same light change
    check(lit(PieceLightObject(true, 3, kPieceObj, kOwnerObj, plant_own_address)), "A5a F8 d: a detached stock piece (the People's Elbow pad, layer 3) is lit with its owner's state too: that default-path change is intended (the game lights it with the owner)");
    check(!lit(PieceLightObject(true, 0, kPieceObj, kOwnerObj)) && !lit(PieceLightObject(false, 1, kPieceObj, kOwnerObj)) && !lit(PieceLightObject(true, 1, kPieceObj, 0)),
          "A5a: an unlinked piece (layer 0), a piece not routed from guest rows, or an owner not in the record keeps its own address (no state: a hole, as before)");
  }
  if (expect_fail) { std::printf(bad ? "planted twin CAUGHT (%d check(s) failed): PASS\n" : "planted twin NOT caught: FAIL\n", bad); return bad ? 0 : 1; }
  std::printf(bad ? "FAIL: %d check(s)\n" : "PASS\n", bad);
  return bad ? 1 : 0;
}
