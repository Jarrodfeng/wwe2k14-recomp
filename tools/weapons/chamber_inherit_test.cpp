// chamber_inherit_test: the offline proof of S111 CHAMBERLIGHT2 (docs\research\s111\CHAMBERLIGHT2.md). 0 game runs, no SDK, no game memory.
//   chamber_inherit_test [--plant props_set | first_char | any_row]
// It runs the SHIPPED rule rdr_scene::chamber_light (src\renderer_scene\chamber_inherited_light.h), the one rf_props.cpp ApplyWeaponRows calls.
// Checks (each planted twin must fail exactly its named checks; exit code 1 when any check failed):
//   ch.inherit_of  a chamber (PACH 6) stage row of the category-3 node -> LastCharacter, of CDTB / CDTA (own-node class) -> LateNodes, other PACHs / non-stage rows -> None
//   ch.last        the last character listed in 33 then 43 (41 skipped, body rows skipped); a CWeapon node listed after it -> none                 <- first_char
//   ch.state       the state joins by (skeleton, stamp); another stamp, no stamp or an unusable state -> none
//   ch.set         LastCharacterSet gives the LAST character's lights (directions, colours, weights, key, sky, ground; no falloff)                <- props_set, first_char
//   ch.not_char    a list row that is no character (no light ref at the stamp: the ELBW pad's category-33 node, sub_825D9D78 +0x30, which installs nothing) listed
//                  after the last character is skipped: that character's lights                                                                     <- props_set, first_char, any_row
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../../src/renderer_scene/chamber_inherited_light.h"
#include "../../src/renderer_scene/draw_place_rows.h"

namespace C = rdr_scene::chamber_light;
namespace W = rdr_scene::weapons;
using rdr_scene::LightRef;
using rdr_scene::LightState;
using rdr_scene::StageLookRow;

static int g_fail = 0;
static void Check(bool ok, const char* name, const char* why) {
  std::printf("%s  %-16s %s\n", ok ? "PASS" : "FAIL", name, why);
  if (!ok) g_fail = 1;
}

// A usable state: light k colour (v + k, v + k, v + k, 0.5), direction (0, 1, 0); the key (light 4) (0.6, 0, 0.8) colour v; sky v / 10, ground v / 20.
static LightState MakeState(float v) {
  LightState s;
  std::memset(&s, 0, sizeof s);
  for (int k = 0; k < 4; ++k) {
    s.light[rdr_scene::kLsDir + 4 * k + 1] = 1.f;
    for (int c = 0; c < 3; ++c) s.light[rdr_scene::kLsCol + 4 * k + c] = v + float(k);
    s.light[rdr_scene::kLsCol + 4 * k + 3] = 0.5f;
  }
  s.light[rdr_scene::kLsDir + 16] = 0.6f, s.light[rdr_scene::kLsDir + 18] = 0.8f;
  for (int c = 0; c < 3; ++c) s.light[rdr_scene::kLsCol + 16 + c] = v, s.light[rdr_scene::kLsBlockA + c] = v / 10.f, s.light[rdr_scene::kLsBlockB + c] = v / 20.f;
  return s;
}

static StageLookRow Char(uint32_t skel, int32_t place) { return rdr_scene::draw_place::Pack(skel, place, false); }
static StageLookRow Prop(uint32_t model, int32_t place) { return rdr_scene::draw_place::Pack(model, place, true); }
static StageLookRow Body(uint32_t skel, int32_t place) { return rdr_scene::draw_place::Pack(skel, place, false, false, false, true); }

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--plant") == 0)
      C::g_plant_chamber_light = std::strcmp(argv[i + 1], "props_set") == 0 ? 1 : std::strcmp(argv[i + 1], "first_char") == 0 ? 2 : std::strcmp(argv[i + 1], "any_row") == 0 ? 3 : 0;
  constexpr uint32_t kA = 0x40001000, kB = 0x40002000, kC = 0x40003000, kD = 0x40004000, kProp = 0x40009000;

  const uint32_t own_other = W::PackStageFlags(2, true, false, W::kStageLrOther), own_node = W::PackStageFlags(5, true, false, W::kStageLrOwnNode);
  const bool inh = C::InheritOf(W::kChamberPach, own_other) == C::Inherit::LastCharacter && C::InheritOf(W::kChamberPach, own_node) == C::Inherit::LateNodes &&
                   C::InheritOf(0, own_other) == C::Inherit::None && C::InheritOf(7, own_other) == C::Inherit::None && C::InheritOf(W::kChamberPach, 0u) == C::Inherit::None;
  Check(inh, "ch.inherit_of", "PACH 6 category-3 row -> LastCharacter, own-node row -> LateNodes, others -> None");

  const std::vector<StageLookRow> rows1 = {Char(kA, 2), Char(kB, 5), Char(kC, 300)};                      // C in list 41: skipped
  const std::vector<StageLookRow> rows2 = {Char(kA, 2), Char(kB, 5), Char(kD, 520), Body(kA, 600), Prop(kProp, 3)};   // D in list 43; the body row and an earlier prop do not count
  const std::vector<StageLookRow> rows3 = {Char(kA, 2), Char(kB, 5), Prop(kProp, 9)};                     // a CWeapon node after the last character
  const auto any = [](uint32_t) { return true; };   // ch.last tests the list order alone: every non-CWeapon row a character
  const bool last = C::LastRimCharacter(rows1, any) == kB && C::LastRimCharacter(rows2, any) == kD && C::LastRimCharacter(rows3, any) == 0u && C::LastRimCharacter({}, any) == 0u &&
                    C::LastRimCharacter({Prop(kProp, 1)}, any) == 0u;
  Check(last, "ch.last", "33 then 43, 41 skipped, body rows skipped; a later CWeapon node -> none");

  std::vector<LightState> states = {MakeState(1.f), MakeState(2.f), MakeState(3.f)};
  LightState dead = MakeState(1.f);
  for (int k = 0; k < 20; ++k) dead.light[rdr_scene::kLsCol + k] = 0.f;
  states.push_back(dead);
  const std::vector<LightRef> refs = {{kA, 0, 0, 100}, {kB, 0, 1, 100}, {kD, 0, 2, 100}, {kB, 0, 0, 99}, {kC, 0, 3, 100}};
  const bool st = C::StateOf(refs, states, kB, true, 100) == &states[1] && C::StateOf(refs, states, kB, true, 99) == &states[0] && C::StateOf(refs, states, kB, false, 100) == nullptr &&
                  C::StateOf(refs, states, kB, true, 98) == nullptr && C::StateOf(refs, states, kC, true, 100) == nullptr && C::StateOf(refs, states, 0u, true, 100) == nullptr;
  Check(st, "ch.state", "joined by (skeleton, stamp); no stamp / another stamp / unusable -> none");

  assets::LightSet out;
  const bool got = C::LastCharacterSet(rows2, refs, states, true, 100, out);   // D -> states[2] (v = 3)
  bool set = got && out.id == -1 && out.edit_id == -1 && !out.hl_view && !out.has_pcol;
  for (int k = 0; k < 4 && set; ++k)
    set = out.col[k][0] == 3.f + float(k) && out.col[k][2] == 3.f + float(k) && out.wt[k] == 0.5f && out.dir[k][1] == 1.f && out.fall[k] == 0.f;
  set = set && out.hl_dir[0] == 0.6f && out.hl_dir[2] == 0.8f && out.hl_col[1] == 3.f && std::fabs(out.sky[0] - 0.3f) < 1e-6f && std::fabs(out.ground[2] - 0.15f) < 1e-6f;
  assets::LightSet none;
  set = set && !C::LastCharacterSet(rows3, refs, states, true, 100, none) && !C::LastCharacterSet(rows2, refs, states, false, 0, none);
  Check(set, "ch.set", "the last character's state -> the set (fields as rf_wrestlers copies); none known -> false");

  constexpr uint32_t kPad = 0x40005000;   // a non-character node's address (the ELBW pad): no light ref
  const std::vector<StageLookRow> rows4 = {Char(kA, 2), Char(kB, 5), Char(kPad, 9)};
  assets::LightSet nc;
  const bool nc_ok = C::LastCharacterSet(rows4, refs, states, true, 100, nc) && nc.col[0][0] == 2.f && nc.col[3][0] == 5.f;   // B -> states[1] (v = 2)
  Check(nc_ok, "ch.not_char", "a non-character row after the last character is skipped: that character's state");

  std::printf("%s\n", g_fail ? "chamber_inherit_test: FAILED" : "chamber_inherit_test: all checks passed");
  return g_fail;
}
