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
  bool plant = false, expect_fail = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) plant = !std::strcmp(argv[++i], "no_alpha");
    else if (!std::strcmp(argv[i], "--expect-fail")) expect_fail = true;
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
  if (expect_fail) { std::printf(bad ? "planted twin CAUGHT (%d check(s) failed): PASS\n" : "planted twin NOT caught: FAIL\n", bad); return bad ? 0 : 1; }
  std::printf(bad ? "FAIL: %d check(s)\n" : "PASS\n", bad);
  return bad ? 1 : 0;
}
