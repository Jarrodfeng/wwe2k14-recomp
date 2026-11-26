// chamber_light_test: the offline proof of S111 CHAMBERMAP (docs\research\s111\CHAMBERMAP.md). 0 game runs, no SDK, no game memory.
//   chamber_light_test [--plant props_set | no_fallback]
// It runs the SHIPPED pick rdr_scene::weapons::StageLightKeyOf (src\renderer_scene\weapon_rows.h), the one rf_props.cpp StageLights calls.
// Checks (each planted twin must fail exactly its named checks; exit code 1 when any check failed):
//   chm.light_keys      the chamber (PACH 6): k 0 / 1 -> set 35, k 2..8 -> set 36 (sub_826F8958 / sub_826F8F88 install order)          <- props_set
//   chm.light_fallback  a chamber k >= 2 piece keeps set 35 when the record has no set 36; k 0 / 1 without set 35 -> none (caller's props set)   <- props_set, no_fallback
//   chm.others_same     cage / cell 100, Inferno 31, ladder rope / lights 500, belt none, unknown PACH none, a set the record lacks none
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../../src/renderer_scene/weapon_rows.h"

namespace W = rdr_scene::weapons;

static int g_fail = 0;
static void Check(bool ok, const char* name, const char* why) {
  std::printf("%s  %-22s %s\n", ok ? "PASS" : "FAIL", name, why);
  if (!ok) g_fail = 1;
}

static bool g_props_set = false;     // planted: the S103 rule (the chamber had no key: the arena props set)
static bool g_no_fallback = false;   // planted: no 35 fallback for a k >= 2 piece when the record lacks 36

static int32_t Pick(int32_t pach, uint32_t k, std::initializer_list<int32_t> sets) {
  const auto has = [&](int32_t id) {
    for (int32_t s : sets)
      if (s == id) return true;
    return false;
  };
  if (g_props_set && pach == W::kChamberPach) return -1;
  if (g_no_fallback && pach == W::kChamberPach && k >= 2) return has(36) ? 36 : -1;
  return W::StageLightKeyOf(pach, k, has);
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--plant") == 0) {
      g_props_set = std::strcmp(argv[i + 1], "props_set") == 0;
      g_no_fallback = std::strcmp(argv[i + 1], "no_fallback") == 0;
    }
  const std::initializer_list<int32_t> all = {31, 35, 36, 100, 500};

  bool keys = true;
  for (uint32_t k = 0; k <= 8; ++k) keys = keys && Pick(6, k, all) == (k <= 1 ? 35 : 36);
  Check(keys, "chm.light_keys", "PACH 6: k 0 / 1 -> 35, k 2..8 -> 36");

  const bool fb = Pick(6, 4, {35}) == 35 && Pick(6, 8, {35, 100}) == 35 && Pick(6, 2, {35, 36}) == 36 && Pick(6, 0, {36}) == -1 && Pick(6, 1, {36, 100}) == -1 && Pick(6, 3, {100}) == -1;
  Check(fb, "chm.light_fallback", "k >= 2 without 36 -> 35; k 0 / 1 without 35 -> none; neither -> none");

  bool others = true;
  for (int32_t p = 0; p <= 3; ++p) others = others && Pick(p, 0, all) == 100 && Pick(p, 5, all) == 100;
  others = others && Pick(7, 0, all) == 31 && Pick(4, 0, all) == 500 && Pick(15, 0, all) == 500 && Pick(4, 1, all) == -1 && Pick(4, 2, all) == 500 && Pick(8, 0, all) == -1 && Pick(-1, 0, all) == -1;
  others = others && Pick(0, 0, {35, 36}) == -1 && Pick(7, 0, {100}) == -1 && Pick(4, 0, {35}) == -1;
  Check(others, "chm.others_same", "cage / cell 100, Inferno 31, ladder 500 / none, unknown none, missing set none");

  std::printf("%s\n", g_fail ? "chamber_light_test: FAILED" : "chamber_light_test: all checks passed");
  return g_fail;
}
