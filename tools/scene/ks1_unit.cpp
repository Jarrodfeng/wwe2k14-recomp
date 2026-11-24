// Lane KS1 (Session 103): unit proof of the props light read directly from the shadow module's light-1 record (src\scene_live\props_light_read.h, src\renderer_scene\props_light_rows.h), the record
// section 65 (kScPropsLight) and PrmFromRecord preferring it (props_map_rows.h). 0 game runs. The module is a FIXTURE (tools\scene\e5_fake_memory.h): no capture holds the shadow module's words.
//   ks1_unit                 every line PASS
//   ks1_unit --plant NAME    exit 0 only when the check that sees the plant FAILS: light2_words / no_scale / row_swap (DIRECT EXACT), no_direct (PREFER), no_dropped_fallback (FALLBACK), no_pairing (PAIRING)
// Marks: the formula is [C] (listing sub_827E74F8 light-1 branch, props_light_rows.h); the fixture numbers are those of a props light straight down at y = 50,000 with a z7147-like depth range.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer_scene/light_state_rows.h"
#include "../../src/renderer_scene/props_map_rows.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_lights.h"
#include "../../src/renderer_scene/props_light_rows.h"
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

static constexpr uint32_t kMod = 0x40002000u;
// the light-1 record's inputs (module words), and the light-2 branch's (different) words so a wrong-light read cannot pass
static void BuildModule() {
  e5::Mem().clear();
  e5::PutWord(scene_inputs::kPropsModulePtr, kMod);
  e5::PutFloat(scene_inputs::kPropsZeroWord, 0.f);
  e5::PutFloat(scene_inputs::kPropsOneWord, 1.f);
  for (uint32_t o = 192; o < 264; o += 4) e5::PutFloat(kMod + o, 0.f);
  e5::PutFloat(kMod + 192, 0.35f), e5::PutFloat(kMod + 200, 0.5f), e5::PutFloat(kMod + 204, 0.75f), e5::PutFloat(kMod + 212, 0.25f), e5::PutFloat(kMod + 216, 0.6f);
  e5::PutFloat(kMod + 220, 0.125f), e5::PutFloat(kMod + 224, 0.9f), e5::PutFloat(kMod + 228, 0.4f), e5::PutFloat(kMod + 260, 2.f);
  e5::PutFloat(kMod + 2304, 80000.f), e5::PutFloat(kMod + 2308, 100.f);   // light 1: far, near
  e5::PutFloat(kMod + 1296, 9000.f), e5::PutFloat(kMod + 1300, 40.f);     // light 2's pair (not ours)
  for (uint32_t o = 0; o < 64; o += 4) e5::PutFloat(kMod + 2112 + o, 0.f), e5::PutFloat(kMod + 1104 + o, 0.f);   // M(R+912): light 1 at +2112, light 2 at +1104
  e5::PutFloat(kMod + 2112 + 12, 0.f), e5::PutFloat(kMod + 2112 + 28, 1.f), e5::PutFloat(kMod + 2112 + 44, 0.f), e5::PutFloat(kMod + 2112 + 60, 50000.f);   // light 1: ViewProj row 3 = straight down, y 50,000
  e5::PutFloat(kMod + 1104 + 28, 1.f), e5::PutFloat(kMod + 1104 + 60, 1200.f);   // light 2: another row 3
}

// An independent transcription of the listing in double precision.
static void Ref(double prm[3][4]) {
  const double far_z = 80000, near_z = 100;
  const double f30 = 1.0 / (far_z - near_z);
  const double t = double(0.6f) * f30;
  const double r0[4] = {near_z, far_z, double(0.35f) * f30, 0.5}, r1[4] = {2.0, 0.25, 1.0 / t, 0.125};
  for (int k = 0; k < 4; ++k) prm[0][k] = r0[k], prm[1][k] = r1[k], prm[2][k] = 0;
}
static bool Near(const float a[3][4], const double b[3][4], double tol) {
  for (int i = 0; i < 12; ++i) {
    const double x = a[i / 4][i % 4], y = b[i / 4][i % 4];
    if (!(std::fabs(x - y) <= tol * std::fmax(1.0, std::fabs(y)))) return false;
  }
  return true;
}

// A LightState holding the props camera (ViewProj row 3 = (0, 1, 0, 50000), as the props states do) with the shadow floats the game wrote into R for light 1: R+1104.. from the listing.
static LightState StateFor(const float prm[3][4]) {
  LightState s;
  std::memset(&s, 0, sizeof s);
  float* f = s.shadow_f;   // f[(byte - 1104) / 4]
  f[(1108 - 1104) / 4] = prm[0][0], f[(1112 - 1104) / 4] = prm[0][1], f[0] = prm[0][2], f[(1120 - 1104) / 4] = prm[0][3];
  f[(1128 - 1104) / 4] = prm[1][0], f[(1136 - 1104) / 4] = prm[1][1], f[(1140 - 1104) / 4] = prm[1][2], f[(1144 - 1104) / 4] = prm[1][3];
  f[(1148 - 1104) / 4] = prm[2][0], f[(1152 - 1104) / 4] = prm[2][1], f[(1156 - 1104) / 4] = prm[2][2], f[(1160 - 1104) / 4] = prm[2][3];
  // M(R+912) = ViewProj in the record's layout: register row 3 = column 3 of M = (0, 1, 0, 50000) -> m[k * 4 + 3] over k is the register row; set it so BuildCasterRows' view_proj[3] = (0, 1, 0, 50000)
  s.shadow_m[0][3] = 0.f, s.shadow_m[0][7] = 1.f, s.shadow_m[0][11] = 0.f, s.shadow_m[0][15] = 50000.f;
  for (int m = 0; m < 3; ++m)
    for (int i = 0; i < 4; ++i) s.shadow_m[m][i * 4 + i] += (m == 0 && i == 3) ? 0.f : 1.f;   // identity-ish (the rows under test are row 3 and the prm floats)
  return s;
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  if (plant == "light2_words") g_props_light_plant = 1;
  if (plant == "no_scale") g_props_light_plant = 2;
  if (plant == "row_swap") g_props_light_plant = 3;

  BuildModule();
  PropsLight pl;
  const scene_inputs::PropsWhy why = scene_inputs::ReadPropsLight(&e5::ReadGuest, 77, pl);
  Check(why == scene_inputs::PropsWhy::Ok && (pl.flags & kPlRead) && pl.frame == 77, "DIRECT READ", "the reader answers Ok on a module with a depth range (frame %u)", pl.frame);
  double ref[3][4];
  Ref(ref);
  Check(why == scene_inputs::PropsWhy::Ok && Near(pl.prm, ref, 1e-6), "DIRECT EXACT", "ShadowPrm c168-c170 equal the double-precision transcription of the listing (near %g far %g, 1/range %g, row 2 zero)", pl.prm[0][0], pl.prm[0][1], pl.prm[0][2]);

  // the light-state path: the same floats as R+1104.. through BuildLightRows / BuildCasterRows must give the same three rows within 1e-5 (the stream's floats ARE these R words)
  const LightState st = StateFor(pl.prm);
  const LightRows lr = BuildLightRows(st);
  double d[3][4];
  for (int i = 0; i < 12; ++i) d[i / 4][i % 4] = lr.shadow_prm[i / 4][i % 4];
  Check(Near(pl.prm, d, 1e-5), "STATE EQUAL", "the direct rows equal the light-state path's rows (BuildLightRows) within 1e-5");
  const double eq = std::fabs(double(pl.prm[0][2]) - 0.35 / 79900.0);
  Check(eq < 1e-9, "DIRECT EXACT", "c168.z = [m+192] / (far - near) = %g", double(pl.prm[0][2]));

  Check(why == scene_inputs::PropsWhy::Ok && (pl.flags & kPlHaveVp) && pl.vp_row3[0] == 0.f && pl.vp_row3[1] == 1.f && pl.vp_row3[2] == 0.f && pl.vp_row3[3] == 50000.f, "DIRECT EXACT", "light 1's ViewProj row 3 = (0, 1, 0, 50000) from module+2112 words 3 / 7 / 11 / 15");
  // PrmFromRecord prefers the record's row, and a walked record (no row) still takes the search
  const float circle_vp[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 50000.f};
  SceneTables t;
  LightState other = StateFor(pl.prm);   // the search would find this state: give it a DIFFERENT first row so a preference for it is visible
  other.shadow_f[(1108 - 1104) / 4] = 7.f, other.shadow_f[(1112 - 1104) / 4] = 9000.f;
  t.light_states.push_back(other);
  float prm[3][4] = {};
  const char* w = "";
  bool ok = props_map::PrmFromRecord(t, circle_vp, prm, w);
  Check(ok && prm[0][0] == 7.f, "FALLBACK", "no direct row (an older record): the state search answers (%s)", w);
  t.props_light.push_back(pl);
  ok = props_map::PrmFromRecord(t, circle_vp, prm, w);
  if (plant == "no_direct") props_map::g_prefer_direct_off = true, ok = props_map::PrmFromRecord(t, circle_vp, prm, w);
  Check(ok && prm[0][0] == 100.f && std::strstr(w, "directly") != nullptr, "PREFER", "with a direct row PrmFromRecord returns it, not the search's state (c168.x %g: %s)", prm[0][0], w);
  props_map::g_prefer_direct_off = false;
  // pairing (review): a row whose ViewProj row 3 is NOT the circles' (stale frame, the other light) is not used; the search answers
  SceneTables t3;
  t3.light_states.push_back(other);
  PropsLight stale = pl;
  stale.vp_row3[3] = 1200.f;
  t3.props_light.push_back(stale);
  if (plant == "no_pairing") props_map::g_skip_pairing = true;
  ok = props_map::PrmFromRecord(t3, circle_vp, prm, w);
  props_map::g_skip_pairing = false;
  Check(ok && prm[0][0] == 7.f, "PAIRING", "a direct row whose ViewProj row 3 differs from the circles' falls back to the search (%s)", w);
  PropsLight novp = pl;
  novp.flags = kPlRead;
  SceneTables t4;
  t4.light_states.push_back(other);
  t4.props_light.push_back(novp);
  ok = props_map::PrmFromRecord(t4, circle_vp, prm, w);
  Check(ok && prm[0][0] == 7.f, "PAIRING", "a row without the ViewProj read is never trusted (%s)", w);
  // an unusable row (far <= near, or a layout this build does not know) falls back, never a hole
  SceneTables t2;
  t2.light_states.push_back(other);
  PropsLight bad = pl;
  bad.prm[0][1] = bad.prm[0][0];
  t2.props_light.push_back(bad);
  ok = props_map::PrmFromRecord(t2, circle_vp, prm, w);
  Check(plant == "no_dropped_fallback" ? false : (ok && prm[0][0] == 7.f), "FALLBACK", "a direct row without a depth range falls back to the search (%s)", w);

  // record: round trip, an older record (no section) parses with none, a bad row drops itself only
  SceneTables rec;
  rec.props_light.push_back(pl);
  rec.light_states.push_back(other);
  std::vector<uint8_t> bytes = SerializeRecord(rec);
  SceneTables back;
  std::string err;
  const bool parsed = ParseRecord(bytes.data(), bytes.size(), back, err);
  Check(parsed && back.props_light.size() == 1 && std::memcmp(back.props_light[0].prm, pl.prm, sizeof pl.prm) == 0 && back.props_light[0].frame == 77 && back.light_states.size() == 1, "RECORD", "section 65 round trips (%s)", err.c_str());
  SceneTables old;
  old.light_states.push_back(other);
  bytes = SerializeRecord(old);
  SceneTables oldback;
  Check(ParseRecord(bytes.data(), bytes.size(), oldback, err) && oldback.props_light.empty(), "RECORD", "an older record without the section parses with no props light");
  SceneTables nan_rec;
  PropsLight nanrow = pl;
  nanrow.prm[1][2] = NAN;
  nan_rec.props_light.push_back(nanrow);
  nan_rec.light_states.push_back(other);
  bytes = SerializeRecord(nan_rec);
  SceneTables nanback;
  Check(ParseRecord(bytes.data(), bytes.size(), nanback, err) && nanback.props_light.empty() && nanback.light_states.size() == 1, "RECORD", "a non-finite row drops that section only, the record is read");
  SceneTables copy;
  SceneTables src2;
  src2.props_light.push_back(pl);
  src2.Recycle();
  Check(src2.props_light.empty(), "RECORD", "Recycle clears the props light");

  // liveness / page checks: no module, an unreadable word, no depth range, a non-finite word
  e5::PutWord(scene_inputs::kPropsModulePtr, 0);
  Check(scene_inputs::ReadPropsLight(&e5::ReadGuest, 1, pl) == scene_inputs::PropsWhy::NoModule, "LIVE", "a null module word: NoModule (no row, no guess)");
  BuildModule();
  e5::Mem().erase(kMod + 2308);
  Check(scene_inputs::ReadPropsLight(&e5::ReadGuest, 1, pl) == scene_inputs::PropsWhy::Unreadable, "LIVE", "an unreadable light-1 word: Unreadable (every guest read is checked)");
  BuildModule();
  e5::PutFloat(kMod + 2304, 100.f);
  Check(scene_inputs::ReadPropsLight(&e5::ReadGuest, 1, pl) == scene_inputs::PropsWhy::NoRange, "LIVE", "far == near: NoRange (no depth range; S103 PMC2: was Bad)");

  std::printf("%s%s\n", g_fail ? "FAILED " : "all passed", g_tokens.c_str());
  if (!plant.empty()) {
    std::printf("plant %s: %s\n", plant.c_str(), g_fail ? "caught" : "NOT caught");
    return g_fail ? 0 : 1;
  }
  return g_fail ? 1 : 0;
}
