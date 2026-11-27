// Lane E4 (Session 103): unit proof of the light install resolver (light_install.h), the bit 7 latch decision (scene_live\caster_stamp_flag.h StampFromLatch) and the plane constants.
// Synthetic rows built to the layout of scene_record.h (A2's manager row): no capture on disk carries them yet [U].
// S111 KEYDEAD: the key-light-from-objects checks (KeyLightOfModel / KeyLightOfWrestler / CasterInputsReady / KeyLightFromRecord) went with that code; the install rule is checked through ResolveInstallRow.
//   e4_unit                 every line PASS
//   e4_unit --plant NAME    exit 0 only when the check that sees the plant FAILS: ignore_remap (INSTALL REMAP), ignore_gate (INSTALL GATE), ignore_override (INSTALL OVERRIDE)
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer_scene/light_install.h"
#include "../../src/renderer_scene/live_maps_e2.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_inputs.h"
#include "../../src/renderer_scene/rows_lights.h"
#include "../../src/scene_live/caster_stamp_flag.h"

using namespace rdr_scene;

static int g_fail = 0;
static std::string g_tokens;
static void Check(bool ok, const char* token, const char* fmt, ...) {
  char b[300];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", ok ? "" : token, ok ? "" : ": ", b);
  if (!ok) ++g_fail, g_tokens += std::string("[") + token + "]";
}

// rows: 0 manager; 1 set 0 block; 2 set 1 block; 3 set 2 block; 4 pool 0 block; 5 override block
struct World {
  std::vector<LightObjectRow> rows;
  World() {
    rows.resize(6);
    LightObjectRow& m = rows[0];
    m.kind = kLoManager, m.flags = kLoBlockRead, m.aux0 = 3, m.remap = 1;
    m.words[kLmOverrideMode0] = m.words[kLmOverrideMode1] = 0xFFFFFFFFu;
    for (uint32_t k = 0; k < kLmRefCount; ++k) m.words[kLmRefBase + k] = kLightNoRow;
    m.words[kLmRefBase + 0] = 1, m.words[kLmRefBase + 1] = 2, m.words[kLmRefBase + 2] = 3;
    m.words[kLmRefBase + kLightMaxSets + 0] = 4;
    m.words[kLmRefBase + kLmRefOverride] = 5;
    m.words[kLmSetInfoBase + 0] = 0, m.words[kLmSetInfoBase + 1] = 0, m.words[kLmSetInfoBase + 2] = (1u << 16);   // set 2 is remapped to pool 0
    for (int i = 1; i <= 5; ++i) rows[size_t(i)].kind = (i <= 3 ? kLoSet : i == 4 ? kLoPool : kLoOverride), rows[size_t(i)].flags = kLoBlockRead;
  }
};

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  InstallFaults ft;
  if (plant == "ignore_remap") ft.ignore_remap = true;
  else if (plant == "ignore_gate") ft.ignore_gate = true;
  else if (plant == "ignore_override") ft.ignore_override = true;
  else if (!plant.empty()) { std::fprintf(stderr, "unknown plant %s\n", plant.c_str()); return 2; }
  const InstallFaults* f = plant.empty() ? nullptr : &ft;
  const char* why = "";

  {  // the generic install: the plain set, the remap, the override words (modes 0 and 1 ONLY), the gate, the out-of-range remap
    World w;
    const LightObjectRow* obj = nullptr;
    Check(ResolveInstallRow(w.rows, 0, 0, obj, why, f) && obj == &w.rows[1], "INSTALL SET", "index 0 -> its own block (row 1)");
    Check(ResolveInstallRow(w.rows, 2, 0, obj, why, f) && obj == &w.rows[4], "INSTALL REMAP", "a set remapped to the pool installs the POOL object (row 4)");
    bool r = ResolveInstallRow(w.rows, 7, 0, obj, why, f);
    Check(!r, "INSTALL NONE", "an index past the sets installs nothing (%s)", why);
    w.rows[0].words[kLmOverrideMode1] = 1;   // mode 1 forces index 1
    Check(ResolveInstallRow(w.rows, 0, 1, obj, why, f) && obj == &w.rows[2], "INSTALL OVERRIDE", "the mode-1 override word 1 replaces index 0: set 1's block (row 2)");
    w.rows[0].words[kLmOverrideMode0] = w.rows[0].words[kLmOverrideMode1] = 1;
    bool ok_m2 = ResolveInstallRow(w.rows, 0, 2, obj, why, f) && obj == &w.rows[1];   // no override word exists for mode 2 (review F3)
    Check(ok_m2, "INSTALL MODE", "mode 2 has no override word: index 0 stays the set's own block (row 1)");
    w.rows[0].words[kLmGateObject] = 0x83000000u, w.rows[0].words[kLmGateFlag] = 1;   // the gate: the override is skipped
    bool ok_gate = ResolveInstallRow(w.rows, 0, 1, obj, why, f) && obj == &w.rows[1];
    Check(ok_gate, "INSTALL GATE", "under the gate the mode-1 override word (1) is skipped: index 0 stays set 0 (row 1)");
    w.rows[0].words[kLmGateObject] = 0, w.rows[0].words[kLmGateFlag] = 0;
    w.rows[0].words[kLmSetInfoBase + 0] = (kLightMaxPool + 5u) << 16;   // set 0's remap + 1 is out of range
    bool hole_range = !ResolveInstallRow(w.rows, 0, 2, obj, why, f);
    Check(hole_range, "INSTALL REMAP RANGE", "a remap past the pool is a hole, never the set's own object (%s)", why);
    w.rows[0].words[kLmSetInfoBase + 0] = 0;
    Check(ResolveInstallRow(w.rows, kInstallOverrideId, 2, obj, why, nullptr) && obj == &w.rows[5], "INSTALL 10010", "id 10010 with remap != 0 selects the override object");
    w.rows[0].remap = 0;
    r = ResolveInstallRow(w.rows, kInstallOverrideId, 2, obj, why, nullptr);
    Check(!r, "INSTALL 10010", "id 10010 with remap == 0 selects nothing (%s)", why);
  }
  {  // bit 7's switch and its wired stamp (StampFromLatch): every wrestler actor has its latch entry, at least one actor, A8's synthesis on
    static_assert(rdr_scene::live::caster_stamp::kStampCasterEntries, "bit 7's stamp switch ships ON (S103 L1C)");
    Check(rdr_scene::live::caster_stamp::kStampCasterEntries, "STAMP ON", "the stamp switch ships ON (L1C)");
    using rdr_scene::live::caster_stamp::StampFromLatch;
    Check(StampFromLatch(true, 2, 2), "L1C STAMP READY", "two wrestlers, both with a latch entry, synthesis on: stamped (no lit key needed: S111 SHADOWKEY)");
    Check(!StampFromLatch(true, 2, 1) && !StampFromLatch(true, 0, 0) && !StampFromLatch(false, 2, 2), "L1C STAMP HOLDS", "one wrestler without an entry / no wrestler / synthesis off: not stamped");
  }
  {  // the plane fixture values: brock_losswin's arena only (EyePos is per-arena module data: LIVE_MAPS.md E4)
    Check(kPlaneEyeWrestler[0] == -450.f && kPlaneEyeWrestler[1] == -1400.f && kPlaneEyeWrestler[2] == 0.f && kPlaneEyeWrestler[3] == 1.f, "PLANE EYE", "the fixture's wrestler proxy eye point (-450, -1400, 0, 1)");
    Check(kPlanePosDefault[1] == -120.f && kPlanePosDefault[0] == 0.f && kPlanePosDefault[2] == 0.f, "PLANE POS", "the fixture's PlanePos (0, -120, 0, 0)");
    E2Input e2;
    Check(!e2.plane_eye_const && !e2.plane_pos_const && !e2.have_plane_pos, "PLANE NOT LIVE", "an E2Input starts with no eye / PlanePos: the constants are never applied outside the rig");
  }
  if (!plant.empty()) {
    const char* want = plant == "ignore_remap" ? "[INSTALL REMAP]" : plant == "ignore_gate" ? "[INSTALL GATE]" : "[INSTALL OVERRIDE]";
    const bool caught = g_tokens.find(want) != std::string::npos;
    std::printf("%s planted %s: %s (failed: %s)\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? (std::string("caught by its named check ") + want).c_str() : (std::string("its named check ") + want + " did NOT fail").c_str(), g_tokens.c_str());
    return caught ? 0 : 1;
  }
  std::printf("%s  e4_unit\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
