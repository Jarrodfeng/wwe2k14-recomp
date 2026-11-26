// guest_route_unit: the offline proof of lane SMF fixes 1 + 2 (Session 103, docs\research\s103\BUG2.md finding 1): an own-skeleton piece drawn through the guest route takes its OWN [model+252] fade
// (EYF's rule; the eyelid / lash layer is alpha 0 in entrances) instead of alpha 1. No record, no game files: the decision is the pure function in src\renderer_scene\guest_piece_fade.h.
// Usage: guest_route_unit [--plant no_alpha|own_light_address|refusal_is_hole|undrawn_is_hole] [--expect-fail]     Exit 0 = every case holds (with --expect-fail: the planted bug was caught).
// S111 PIECELINK: the unlinked owner-kinds rule (UnlinkedPieceMayGuestRoute) and its cases are deleted; a refused created-superstar piece is a counted refusal, never a hole (DecideUnroutedPiece).
// Build: tools\scene\build_guest_route_unit.ps1 (header only).
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "../../src/renderer_scene/guest_piece_fade.h"

using namespace rdr_scene::rf;

int main(int argc, char** argv) {
  bool plant = false, expect_fail = false, plant_refusal_hole = false, plant_own_address = false, plant_undrawn_hole = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) {
      const char* p = argv[++i];
      plant = !std::strcmp(p, "no_alpha");
      plant_refusal_hole = !std::strcmp(p, "refusal_is_hole");      // S111 PIECELINK: the old outcome, a refused created-superstar piece went out unflagged (a hole when the game drew it)
      plant_own_address = !std::strcmp(p, "own_light_address");     // S105 A5a: the piece asks for the light state of its own address
      plant_undrawn_hole = !std::strcmp(p, "undrawn_is_hole");      // S111 PIECEHOLES: the old outcome, a slot body the game did not ask to draw was a hole (no walk = "drawn")
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
  // entry: rdr_scene::rf::DecideOwnRowsRoute / DecideUnroutedPiece / PieceLightObject, called directly: rf_wrestlers.cpp ApplyWrestlers calls them with locals of a whole record walk (owner_obj, PieceSeen, the model cache), which an offline unit cannot build; all three are pure.
  // S105 A5a: a LINKED own-rows piece (a CAW's hair / necklace / braces, kObjOwnRowsPiece) names its owner: its route follows that owner alone (a stock opponent with hair in the picture cannot block it), and it is lit with the OWNER's state.
  {
    OwnRowsRoute o = DecideOwnRowsRoute(true, false);
    check(o.hole && o.guest_route && !o.not_hole, "A5a: a linked piece of a visible guest-row owner takes the guest route, whatever other bodies the picture holds");
    o = DecideOwnRowsRoute(true, true);
    check(!o.hole && !o.guest_route && o.not_hole, "A5a: the game hid the piece this frame (a walked frame with no ref for it): not a hole, not drawn");
    o = DecideOwnRowsRoute(false, false);
    check(o.hole && !o.guest_route, "A5a: the owner slot is not a visible guest-row body: no route (a hole, never a draw beside a body that draws it itself)");
    const uint32_t kOwnerObj = 0xA0001000u, kPieceObj = 0xA0002000u;
    const auto lit = [&](uint32_t addr) { return addr == kOwnerObj; };   // the latch's synthetic refs: one per actor:slot (the owner), none for a piece address
    check(lit(PieceLightObject(true, kPieceObj, kOwnerObj, plant_own_address)), "A5a: a guest-routed piece (it names its owner) is lit with the OWNER's light state (one wrestler light per draw node; the People's Elbow pad too, review F8 d)");
    check(!lit(PieceLightObject(false, kPieceObj, kOwnerObj)) && !lit(PieceLightObject(true, kPieceObj, 0)), "A5a: a piece not routed from guest rows, or an owner not in the record, keeps its own address");
  }
  // S111 PIECELINK: an own-skeleton piece with no route flag. A created superstar's piece whose palette the copy proof refused carries its owner and kObjOwnRowsRefused: counted, never a hole.
  {
    check(DecideUnroutedPiece(true, true, plant_refusal_hole) == UnroutedPiece::Refused, "PIECELINK: a refused created-superstar piece (kObjOwnRowsRefused) on a frame the game draws it: a counted refusal, NOT a hole");
    check(DecideUnroutedPiece(true, false, plant_refusal_hole) == UnroutedPiece::Refused, "PIECELINK: the same refused piece on a frame the game does not draw it: the same counted refusal");
    check(DecideUnroutedPiece(false, true, plant_refusal_hole) == UnroutedPiece::Hole, "PIECELINK: a piece in no character's SUBM list, drawn by the game: a named hole (no owner is guessed: the unlinked guest route is gone)");
    check(DecideUnroutedPiece(false, false, plant_refusal_hole) == UnroutedPiece::NotHole, "PIECELINK: such a piece not drawn by the game this frame (or drawn by the ring family): not a hole");
  }
  // S111 PIECEHOLES: a slot body the game did not ask to draw this period (kObjGameUndrawn) is not a hole where no walk can say "not drawn" (an OURS frame: game_draws_it is true for every such object there).
  // Review: on a walked frame the walk decides (a walked ref for the body = the game drew it although no ask was stamped: a hole). nr_unit runs the same cases through BuildFrame.
  {
    check(DecideUnroutedPiece(false, true, false, true, false, plant_undrawn_hole) == UnroutedPiece::GameUndrawn, "PIECEHOLES: a body the game did not ask to draw, on a frame with no walked evidence: not drawn by the game, NOT a hole");
    check(DecideUnroutedPiece(false, true, false, false, false, plant_undrawn_hole) == UnroutedPiece::Hole, "PIECEHOLES: the same object without the flag (an unknown own-skeleton piece, no walk): still a hole (fail closed)");
    check(DecideUnroutedPiece(false, true, false, true, true, plant_undrawn_hole) == UnroutedPiece::Hole, "PIECEHOLES review: the flagged body on a walked frame whose walk names it: the game drew it, a hole (the walk beats a missed ask)");
    check(DecideUnroutedPiece(false, false, false, true, true, plant_undrawn_hole) == UnroutedPiece::NotHole, "PIECEHOLES review: the flagged body on a walked frame with no ref for it: not a hole");
  }
  if (expect_fail) { std::printf(bad ? "planted twin CAUGHT (%d check(s) failed): PASS\n" : "planted twin NOT caught: FAIL\n", bad); return bad ? 0 : 1; }
  std::printf(bad ? "FAIL: %d check(s)\n" : "PASS\n", bad);
  return bad ? 1 : 0;
}
