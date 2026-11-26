// S111 TECHPASS (docs\research\s111\TECHPASS.md section B): unit of the wrestler pass rule's +16 term (src\renderer_scene\technique_pick.h CharClassOf / CharPassOf / CharPass), with the planted twin
// of the rule before the lane (model +620 recorded, never added). Pure: no game, no SDK, no GPU. Build + run: pwsh -NoProfile -File tools\scene\build_techpass_unit.ps1
#include <cstdio>
#include <string>

#include "renderer_scene/technique_pick.h"

namespace tp = rdr_scene::technique_pick;

using PassFn = int (*)(const tp::CharClass&, bool, bool, int, bool, int);

static int RealPass(const tp::CharClass& c, bool skinned, bool shadow_ok, int mode, bool spec, int m620) { return tp::CharPassOf(c, skinned, shadow_ok, mode, spec, m620); }
// The planted twin: the call before S111 TECHPASS handed no model +620 (the default -1 = no term).
static int OldPass(const tp::CharClass& c, bool skinned, bool shadow_ok, int mode, bool spec, int /*m620*/) { return tp::CharPassOf(c, skinned, shadow_ok, mode, spec); }

static int Cases(PassFn f, bool print) {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    if (print) std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
  };
  const tp::CharClass hair = tp::CharClassOf("ych_aniso_trans"), skin = tp::CharClassOf("ych_skin"), trans = tp::CharClassOf("ych_trans");
  // A created wrestler's hair: model +620 = 1 -> the game's ss_ row p17 (the caller clears shadow_ok for the hair class), in both modes (no E+12 for this name).
  check(f(hair, true, false, 0, false, 1) == 17, "CAW hair (m620 = 1, mode 0) takes pass 17");
  check(f(hair, true, false, 2, false, 1) == 17, "CAW hair (m620 = 1, mode 2) takes pass 17");
  check(f(hair, true, true, 0, false, 1) == 19, "the rule alone with a shadow map: +2 +16 = 19");
  // Stock / unknown: no term.
  check(f(hair, true, false, 0, false, 0) == 1, "stock hair (m620 = 0) keeps pass 1");
  check(f(hair, true, false, 0, false, -1) == 1, "no latched word (-1) keeps pass 1");
  // Only E+20 takes the term.
  check(f(skin, true, false, 0, true, 1) == 1, "skin with m620 = 1: no +16 (no E+20)");
  check(f(trans, true, true, 2, false, 1) == 11, "yCh_trans with m620 = 1: base + 8 + 2, no +16");
  return fails;
}

int main() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++fails;
  };
  check(tp::CharClassOf("ych_aniso_trans").e20, "E+20 is set for ych_aniso_trans");
  check(!tp::CharClassOf("ych_trans").e20 && !tp::CharClassOf("ych_skin").e20 && !tp::CharClassOf("ych_cloth_trans").e20, "E+20 is clear for every other name");
  check(tp::CharPass("ych_aniso_trans", true, false, 0, false, 1) == 17, "CharPass hands model +620 through");
  check(tp::CharPass("ych_aniso_trans", true, false, 0, false) == 1, "CharPass without the word = no term");
  fails += Cases(RealPass, true);
  const int twin = Cases(OldPass, false);
  check(twin > 0, "planted twin (the rule before TECHPASS: no +16) is caught");
  std::printf("%s (%d failed; the twin failed %d cases)\n", fails ? "TECHPASS UNIT FAILED" : "TECHPASS UNIT OK", fails, twin);
  return fails ? 1 : 0;
}
