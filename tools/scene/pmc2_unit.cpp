// Lane PMC2 (Session 103, 2026-11-19): unit proof of the props-map census fixes from probe PMH (docs\agents\s103\PMH.md). 0 game runs. Fixture memory only (tools\scene\e5_fake_memory.h).
//   1. ReadCircleModule names the branch that answered (CircleModuleWhy) and a non-finite module+2240 is NothingToDraw / Degenerate, not Unreadable; ReadCircleShadows stamps it (module_ok) with no rows.
//   2. ReadPropsLight splits the old Bad into NonFinite and NoRange.
//   3. The circle input classifier says "props light degenerate" (not "no circle mesh") on a degenerate frame, and it is not a hole.
//   4. One missing image is ONE hole; a degenerate frame is counted apart (props_light_degenerate).
//   pmc2_unit                 every line PASS
//   pmc2_unit --plant NAME    exit 0 only when the check that sees the plant FAILS: finite_vp (DEGENERATE), props_finite (PROPS SPLIT), no_degenerate (CLASSIFY), double_count (ONCE)
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "../../src/renderer_scene/maps_census.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_circle.h"
#include "../../src/scene_live/circle_points.h"
#include "../../src/scene_live/props_light_read.h"
#include "e5_fake_memory.h"

using namespace rdr_scene;

static int g_fail = 0;
static std::string g_tokens;
static void Check(bool ok, const char* token, const char* fmt, ...) {
  char b[320];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", ok ? "" : token, ok ? "" : ": ", b);
  if (!ok) ++g_fail, g_tokens += std::string("[") + token + "]";
}

static constexpr uint32_t kMod = 0x40002000u, kCirc = 0x40003000u, kP1 = 0x400B9000, kP2 = 0x400BA000, kE1 = 0x400BB000, kE2 = 0x400BC000;
static constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

// The shadow module as ReadCircleModule reads it (the same words inputs_unit's a3 fixture builds): a circle object, the two off tests, both handle tables, ViewProj, c0's words.
static void BuildCircleModule(bool nan_vp) {
  e5::Mem().clear();
  using namespace scene_inputs;
  e5::PutWord(kShadowModulePtr, kMod);
  e5::PutWord(kMod + kModCircle, kCirc), e5::PutWord(kMod + 288, 1), e5::PutWord(kMod + 22696, 1);
  e5::PutFloat(kCirc + kCircleX, 0.6f), e5::PutFloat(kMod + kModZ, 12.f), e5::PutFloat(kMod + kModW, 3.f);
  for (uint32_t i = 0; i < 16; ++i) e5::PutFloat(kMod + kModViewProj + 4 * i, float(i));
  if (nan_vp) e5::PutFloat(kMod + kModViewProj + 4 * 5, kNaN);
  e5::PutWord(kCirc + 8, 0x00010005), e5::PutWord(kCirc + 28, 0x00020003);
  e5::PutWord(kHandleTab1, kP1), e5::PutWord(kP1 + 8, kE1), e5::PutWord(kP1 + 16, kE1 + 8 * 8);
  e5::PutWord(kHandleTab2, kP2), e5::PutWord(kP2 + 8, kE2), e5::PutWord(kP2 + 16, kE2 + 8 * 8);
  for (uint32_t i = 0; i < 8; ++i) e5::PutWord(kE1 + 8 * i, 0), e5::PutWord(kE1 + 8 * i + 4, 0), e5::PutWord(kE2 + 8 * i, 0), e5::PutWord(kE2 + 8 * i + 4, 0);
  e5::PutWord(kE1 + 8 * 5, 0x00010005), e5::PutWord(kE1 + 8 * 5 + 4, 0x1234);
  e5::PutWord(kE2 + 8 * 3, 0x00020003), e5::PutWord(kE2 + 8 * 3 + 4, 0x5678);
  for (uint32_t s = 0; s < kCircleSlots; ++s) e5::PutWord(kCircleSlotTable + 4 * s, 0);   // no character: the module part is what is under test
}

static void BuildPropsModule() {
  e5::Mem().clear();
  e5::PutWord(scene_inputs::kPropsModulePtr, kMod);
  e5::PutFloat(scene_inputs::kPropsZeroWord, 0.f);
  e5::PutFloat(scene_inputs::kPropsOneWord, 1.f);
  for (uint32_t o = 192; o < 264; o += 4) e5::PutFloat(kMod + o, 0.f);
  e5::PutFloat(kMod + 192, 0.35f), e5::PutFloat(kMod + 200, 0.5f), e5::PutFloat(kMod + 204, 0.75f), e5::PutFloat(kMod + 212, 0.25f), e5::PutFloat(kMod + 216, 0.6f);
  e5::PutFloat(kMod + 220, 0.125f), e5::PutFloat(kMod + 224, 0.9f), e5::PutFloat(kMod + 228, 0.4f), e5::PutFloat(kMod + 260, 2.f);
  e5::PutFloat(kMod + 2304, 80000.f), e5::PutFloat(kMod + 2308, 100.f);
  e5::PutFloat(kMod + 1296, 9000.f), e5::PutFloat(kMod + 1300, 40.f);
  for (uint32_t o = 0; o < 64; o += 4) e5::PutFloat(kMod + 2112 + o, 0.f), e5::PutFloat(kMod + 1104 + o, 0.f);
  e5::PutFloat(kMod + 2112 + 12, 0.f), e5::PutFloat(kMod + 2112 + 28, 1.f), e5::PutFloat(kMod + 2112 + 44, 0.f), e5::PutFloat(kMod + 2112 + 60, 50000.f);
}

using scene_inputs::CircleModuleState;
using scene_inputs::CircleModuleWhy;

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];

  // ---- 1. the circle module read: which branch answered; a non-finite ViewProj is the game's state ----
  {
    BuildCircleModule(plant != "finite_vp");   // plant: the fixture is NOT degenerate, so the degenerate check must fail
    scene_inputs::CircleModuleRead m;
    CircleModuleWhy why = CircleModuleWhy::Count;
    const CircleModuleState st = scene_inputs::ReadCircleModule(&e5::ReadGuest, m, &why);
    Check(st == CircleModuleState::NothingToDraw && why == CircleModuleWhy::Degenerate && !m.active, "DEGENERATE", "a non-finite module+2240 is NothingToDraw / Degenerate (the game's light), not Unreadable");

    std::vector<rdr_scene::CircleShadow> rows;
    scene_inputs::CircleReadInfo ci;
    uint32_t no_height = 9;
    bool module_ok = false;
    const bool ok = scene_inputs::ReadCircleShadows(&e5::ReadGuest, 5, rows, ci, no_height, module_ok);
    Check(ok && module_ok && rows.empty() && ci.light_degenerate && ci.module_why == CircleModuleWhy::Degenerate, "DEGENERATE STAMP", "ReadCircleShadows: module_ok (the frame stamps bit 8) with no rows, light_degenerate set");

    // the twin: a finite ViewProj is Ok and not degenerate
    BuildCircleModule(false);
    why = CircleModuleWhy::Count;
    Check(scene_inputs::ReadCircleModule(&e5::ReadGuest, m, &why) == CircleModuleState::Ok && why == CircleModuleWhy::Ok && m.active, "TWIN OK", "finite ViewProj: Ok, why Ok");
    scene_inputs::CircleReadInfo ci2;
    scene_inputs::ReadCircleShadows(&e5::ReadGuest, 5, rows, ci2, no_height, module_ok);
    Check(module_ok && !ci2.light_degenerate, "TWIN OK", "finite ViewProj: light_degenerate stays false");

    // each Unreadable branch carries its own counter slot (the census prints them apart)
    struct Case { const char* name; uint32_t addr; CircleModuleWhy want; };
    const Case cases[] = {
        {"module ptr", scene_inputs::kShadowModulePtr, CircleModuleWhy::ModulePtr},
        {"circle word", kMod + scene_inputs::kModCircle, CircleModuleWhy::CircleWord},
        {"mode word", kMod + 288, CircleModuleWhy::ModeWords},
        {"handle word", kCirc + 8, CircleModuleWhy::HandleWords},
        {"view proj", kMod + scene_inputs::kModViewProj + 8, CircleModuleWhy::ViewProjRead},
        {"param word", kCirc + scene_inputs::kCircleX, CircleModuleWhy::ParamRead},
    };
    for (const Case& c : cases) {
      BuildCircleModule(false);
      e5::Mem().erase(c.addr);
      why = CircleModuleWhy::Count;
      const CircleModuleState s2 = scene_inputs::ReadCircleModule(&e5::ReadGuest, m, &why);
      Check(s2 == CircleModuleState::Unreadable && why == c.want, "BRANCH", "unreadable %s -> Unreadable with its own branch (%u)", c.name, unsigned(why));
    }
    // a NothingToDraw state keeps why Ok (the game draws none for a legitimate reason): the null module word
    BuildCircleModule(false);
    e5::PutWord(scene_inputs::kShadowModulePtr, 0);
    why = CircleModuleWhy::Count;
    Check(scene_inputs::ReadCircleModule(&e5::ReadGuest, m, &why) == CircleModuleState::NothingToDraw && why == CircleModuleWhy::Ok, "BRANCH", "a null shadow module: NothingToDraw, not counted degenerate");
  }

  // ---- 2. the props light: NonFinite and NoRange apart ----
  {
    BuildPropsModule();
    rdr_scene::PropsLight pl;
    if (plant != "props_finite") e5::PutFloat(kMod + 2304, kNaN);   // plant: nothing non-finite, so the NonFinite check must fail
    const scene_inputs::PropsWhy a = scene_inputs::ReadPropsLight(&e5::ReadGuest, 1, pl);
    Check(a == scene_inputs::PropsWhy::NonFinite && scene_inputs::PropsDegenerate(a), "PROPS SPLIT", "a NaN far word: NonFinite (a degenerate light)");
    BuildPropsModule();
    e5::PutFloat(kMod + 2112 + 60, kNaN);
    const scene_inputs::PropsWhy b = scene_inputs::ReadPropsLight(&e5::ReadGuest, 1, pl);
    Check(b == scene_inputs::PropsWhy::NonFinite, "PROPS SPLIT", "a NaN ViewProj row 3 word: NonFinite");
    BuildPropsModule();
    e5::PutFloat(kMod + 2304, 100.f);
    const scene_inputs::PropsWhy c = scene_inputs::ReadPropsLight(&e5::ReadGuest, 1, pl);
    Check(c == scene_inputs::PropsWhy::NoRange && scene_inputs::PropsDegenerate(c), "PROPS SPLIT", "far == near: NoRange (a degenerate light), apart from NonFinite");
    BuildPropsModule();
    e5::Mem().erase(kMod + 2308);
    const scene_inputs::PropsWhy d = scene_inputs::ReadPropsLight(&e5::ReadGuest, 1, pl);
    BuildPropsModule();
    const scene_inputs::PropsWhy e = scene_inputs::ReadPropsLight(&e5::ReadGuest, 1, pl);
    Check(d == scene_inputs::PropsWhy::Unreadable && !scene_inputs::PropsDegenerate(d) && e == scene_inputs::PropsWhy::Ok && !scene_inputs::PropsDegenerate(e), "PROPS SPLIT", "Unreadable and Ok are not degenerate");
    Check(uint32_t(scene_inputs::PropsWhy::Count) == 5, "PROPS SPLIT", "five reasons (the census array is sized from Count)");
  }

  // ---- 3 / 4. the classifier and the count ----
  {
    namespace mc = maps_census;
    const bool pl_nd = plant == "no_degenerate";
    const mc::CircleSkip deg = mc::ClassifyCircleInput(true, false, true, pl_nd);
    Check(deg == mc::CircleSkip::Degenerate && !mc::IsHole(deg) && std::strstr(mc::CircleSkipText(deg, false, false), "props light is degenerate") && !std::strstr(mc::CircleSkipText(deg, false, false), "no circle mesh"), "CLASSIFY",
          "no rows + degenerate light: its own reason (not 'no circle mesh'), not a hole");
    Check(mc::ClassifyCircleInput(true, false, false) == mc::CircleSkip::NoMesh && mc::IsHole(mc::CircleSkip::NoMesh) && std::strstr(mc::CircleSkipText(mc::CircleSkip::NoMesh, false, false), "no circle mesh"), "CLASSIFY", "twin: no rows, no mesh, light fine = a hole with the mesh reason");
    Check(mc::ClassifyCircleInput(true, true, false) == mc::CircleSkip::NoRows && mc::IsHole(mc::CircleSkip::NoRows) && mc::ClassifyCircleInput(false, true, true) == mc::CircleSkip::None, "CLASSIFY", "twin: no rows with a mesh = NoRows hole; rows present = nothing skipped even with the flag");
    Check(mc::ClassifyCircleInput(false, false, true) == mc::CircleSkip::NoMesh, "CLASSIFY", "rows present but no mesh is still a mesh hole (the flag only explains EMPTY rows)");

    // one missing image = one hole: RecordCircles counted it (before -> before + 1), the props line must not count it again
    uint32_t holes = 0;
    const uint32_t before = holes;
    holes += mc::IsHole(mc::CircleSkip::NoMesh);   // RecordCircles's own count
    holes = mc::HolesWithPropsHole(before, holes, plant == "double_count");
    Check(holes == 1, "ONCE", "a missing circle image counts ONE hole (frame holes %u)", holes);
    uint32_t h2 = 0;   // a props hole whose reason RecordCircles did not count (e.g. no image): the props line counts it, once
    h2 = mc::HolesWithPropsHole(0, h2, plant == "double_count");
    Check(h2 == 1, "ONCE", "a props hole nobody else counted is one hole (%u)", h2);
    // degenerate: no hole, its own outcome
    Check(mc::ClassifyProps(true, true, true) == mc::PropsOutcome::Degenerate && mc::ClassifyProps(true, false, true) == mc::PropsOutcome::None && mc::ClassifyProps(false, true, true) == mc::PropsOutcome::Hole &&
              mc::ClassifyProps(false, true, false) == mc::PropsOutcome::None,
          "COUNT", "degenerate = props_light_degenerate (not a hole), switch off = nothing, real missing = hole, map made = nothing");
  }

  std::printf("%s%s\n", g_fail ? "FAILED " : "all passed", g_tokens.c_str());
  if (!plant.empty()) {
    std::printf("plant %s: %s\n", plant.c_str(), g_fail ? "caught" : "NOT caught");
    return g_fail ? 0 : 1;
  }
  return g_fail ? 1 : 0;
}
