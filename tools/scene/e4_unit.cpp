// Lane E4 (Session 103): unit proof of the light install resolver (light_install.h), the key light from the record's light OBJECTS (shadow_fit_record.h), the bit 7 decision (scene_live\caster_stamp.h)
// and the plane constants. Synthetic rows built to the layout of scene_record.h (A2's manager row, A3's kLoModelMap): no capture on disk carries them yet [U].
//   e4_unit                 every line PASS
//   e4_unit --plant NAME    exit 0 only when the check that sees the plant FAILS: ignore_remap (INSTALL REMAP), ignore_gate (WRESTLER GATE), ignore_override (INSTALL OVERRIDE), first_model_only (STAMP ALL MODELS)
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
#include "../../src/renderer_scene/shadow_fit_record.h"
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
static uint32_t Bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
static void Unit3(float& x, float& y, float& z);
static void SetKey(LightObjectRow& r, float x, float y, float z) {
  Unit3(x, y, z);
  r.words[kLsDir + 16] = Bits(x), r.words[kLsDir + 17] = Bits(y), r.words[kLsDir + 18] = Bits(z), r.words[kLsDir + 19] = Bits(1.f);
}

// rows: 0 manager; 1 set 0 block (key A); 2 set 1 block (key B); 3 set 2 block (key C); 4 pool 0 block (key P); 5 override block (key O); 6 the model map
struct World {
  std::vector<LightObjectRow> rows;
  World() {
    rows.resize(7);
    LightObjectRow& m = rows[0];
    m.kind = kLoManager, m.flags = kLoBlockRead, m.aux0 = 3, m.remap = 1;
    m.words[kLmOverrideMode0] = m.words[kLmOverrideMode1] = 0xFFFFFFFFu;
    for (uint32_t k = 0; k < kLmRefCount; ++k) m.words[kLmRefBase + k] = kLightNoRow;
    m.words[kLmRefBase + 0] = 1, m.words[kLmRefBase + 1] = 2, m.words[kLmRefBase + 2] = 3;
    m.words[kLmRefBase + kLightMaxSets + 0] = 4;
    m.words[kLmRefBase + kLmRefOverride] = 5;
    m.words[kLmSetInfoBase + 0] = 0, m.words[kLmSetInfoBase + 1] = 0, m.words[kLmSetInfoBase + 2] = (1u << 16);   // set 2 is remapped to pool 0
    for (int i = 1; i <= 5; ++i) rows[size_t(i)].kind = (i <= 3 ? kLoSet : i == 4 ? kLoPool : kLoOverride), rows[size_t(i)].flags = kLoBlockRead;
    SetKey(rows[1], -0.30f, 0.5f, -0.80f), SetKey(rows[2], -0.50f, 0.5f, -0.60f), SetKey(rows[3], 0.10f, 0.5f, 0.90f), SetKey(rows[4], -0.70f, 0.5f, -0.40f), SetKey(rows[5], 0.60f, 0.5f, 0.50f);
    rows[6].kind = kLoModelMap, rows[6].flags = kLoBlockRead, rows[6].aux0 = 4;
    const uint32_t t[4][3] = {{0xA0000010u, 0, 1}, {0xA0000020u, 1, 1}, {0xA0000030u, 2, 1}, {0xA0000040u, 7, 1}};   // (model, index, mode); the object column is 0 here (E4 resolves by index)   // models: set 0, set 1, set 2 (remapped), no object (7 >= n)
    for (int k = 0; k < 4; ++k) SetModelMapEntry(rows[6].words, uint32_t(k), ModelMapEntry{t[k][0], 0u, t[k][1], t[k][2]});   // the shared accessor (scene_record.h), never hand-indexed words
  }
};

static void Unit3(float& x, float& y, float& z) { const float n = std::sqrt(x * x + y * y + z * z); if (n > 0.f) x /= n, y /= n, z /= n; }   // S103 L1C: the fixtures are normalised (IsKeyDirection wants a unit direction; key B had len2 0.86)
static bool Near(const float* a, float x, float y, float z) { Unit3(x, y, z); return std::fabs(a[0] - x) < 1e-5f && std::fabs(a[1] - y) < 1e-5f && std::fabs(a[2] - z) < 1e-5f; }

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  InstallFaults ft;
  if (plant == "ignore_remap") ft.ignore_remap = true;
  else if (plant == "ignore_gate") ft.ignore_gate = true;
  else if (plant == "ignore_override") ft.ignore_override = true;
  else if (!plant.empty() && plant != "first_model_only") { std::fprintf(stderr, "unknown plant %s\n", plant.c_str()); return 2; }
  const InstallFaults* f = plant.empty() || plant == "first_model_only" ? nullptr : &ft;
  const char* why = "";
  float key[3];

  {  // the generic install: the plain set, the remap, the override words (modes 0 and 1 ONLY), the gate, the out-of-range remap
    World w;
    Check(KeyLightOfModel(w.rows, 0xA0000010u, key, why, f) && Near(key, -0.30f, 0.5f, -0.80f), "INSTALL SET", "model on set 0 -> its own block's key");
    const bool ok_remap = KeyLightOfModel(w.rows, 0xA0000030u, key, why, f) && Near(key, -0.70f, 0.5f, -0.40f);
    Check(ok_remap, "INSTALL REMAP", "a set remapped to the pool installs the POOL object (key %.2f %.2f %.2f, want -0.70 0.50 -0.40)", key[0], key[1], key[2]);
    bool r = KeyLightOfModel(w.rows, 0xA0000040u, key, why, f);
    Check(!r, "INSTALL NONE", "an index past the sets installs nothing (%s)", why);
    r = KeyLightOfModel(w.rows, 0xDEAD0000u, key, why, f);
    Check(!r, "INSTALL UNKNOWN", "a model with no learned install is a hole (%s)", why);
    w.rows[0].words[kLmOverrideMode1] = 1;   // mode 1 forces index 1
    bool ok_ov = KeyLightOfModel(w.rows, 0xA0000010u, key, why, f) && Near(key, -0.50f, 0.5f, -0.60f);
    Check(ok_ov, "INSTALL OVERRIDE", "the mode-1 override word 1 replaces the model's index 0: key %.2f %.2f %.2f, want -0.50 0.50 -0.60", key[0], key[1], key[2]);
    w.rows[0].words[kLmOverrideMode0] = w.rows[0].words[kLmOverrideMode1] = 1;
    w.rows[6].words[1] = 0, w.rows[6].words[2] = 2;   // the first model installs in mode 2: no override word exists for it (review F3)
    const LightObjectRow* obj = nullptr;
    bool ok_m2 = ResolveInstallRow(w.rows, 0, 2, obj, why, f) && obj == &w.rows[1];
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
  {  // the WRESTLER path (review F1): sub_82620488(embedded, mode 0): the override word only; else the node's embedded object (a hole)
    World w;
    bool r = KeyLightOfWrestler(w.rows, key, why, f);
    Check(!r, "WRESTLER EMBEDDED", "no mode-0 override word: the wrestler node installs its EMBEDDED object, not in the record (%s)", why);
    w.rows[0].words[kLmOverrideMode0] = 1;
    r = KeyLightOfWrestler(w.rows, key, why, f) && Near(key, -0.50f, 0.5f, -0.60f);
    Check(r, "WRESTLER OVERRIDE", "mode-0 override word 1 -> set 1's key (%.2f %.2f %.2f)", key[0], key[1], key[2]);
    w.rows[0].words[kLmOverrideMode0] = 2;   // set 2 is remapped to the pool
    r = KeyLightOfWrestler(w.rows, key, why, f) && Near(key, -0.70f, 0.5f, -0.40f);
    Check(r, "INSTALL REMAP", "override word 2 -> set 2 remapped to the pool object (key %.2f %.2f %.2f)", key[0], key[1], key[2]);
    w.rows[0].words[kLmOverrideMode0] = 1, w.rows[0].words[kLmGateObject] = 0x83000000u, w.rows[0].words[kLmGateFlag] = 1;
    r = KeyLightOfWrestler(w.rows, key, why, f);
    Check(!r, "WRESTLER GATE", "under the gate the override is skipped: embedded object, a hole (%s)", why);
  }
  {  // the bit 7 decision
    World w;
    w.rows[0].words[kLmOverrideMode0] = 1;
    std::vector<uint32_t> models = {0xA0000010u, 0xA0000020u};
    float k0[3];
    bool ready = CasterInputsReady(w.rows, models.data(), models.size(), k0, why);
    Check(ready, "STAMP READY", "wrestler models with a resolvable key: ready");
    w.rows[0].words[kLmOverrideMode0] = 0xFFFFFFFFu;   // no mode-0 override: the embedded object
    ready = CasterInputsReady(w.rows, models.data(), models.size(), k0, why);
    Check(!ready, "STAMP EMBEDDED", "no override word: the wrestlers' embedded object blocks the stamp (%s)", why);
    w.rows[0].words[kLmOverrideMode0] = 1;
    size_t n = plant == "first_model_only" ? models.size() : 0;   // the plant: an empty model list is passed as the full list (the 'no wrestler' refusal is lost)
    ready = CasterInputsReady(w.rows, models.data(), n, k0, why);
    Check(!ready, "STAMP ALL MODELS", "no wrestler model known: not ready (%s)", why);
    static_assert(rdr_scene::live::caster_stamp::kStampCasterEntries, "bit 7's stamp switch ships ON (S103 L1C)");
    Check(rdr_scene::live::caster_stamp::kStampCasterEntries && !kKeyFromLightObjects, "STAMP ON", "the stamp switch ships ON (L1C) and the key-from-objects switch stays OFF (the key comes from A8's light state)");
    {   // S103 L1C: bit 7's wired stamp (StampFromLatch): every wrestler actor has its latch entry, at least one actor, A8's synthesis on
      using rdr_scene::live::caster_stamp::StampFromLatch;
      Check(StampFromLatch(true, 2, 2, 1), "L1C STAMP READY", "two wrestlers, both with a latch entry, synthesis on: stamped");
      Check(!StampFromLatch(true, 2, 1, 1) && !StampFromLatch(true, 0, 0, 0) && !StampFromLatch(false, 2, 2, 2) && !StampFromLatch(true, 2, 2, 0), "L1C STAMP HOLDS", "one wrestler without an entry / no wrestler / synthesis off: not stamped");
    }
  }
  {  // KeyLightFromRecord: states first, objects only behind the switch and only without a wrestler state (review F1)
    World w;
    w.rows[0].words[kLmOverrideMode0] = 2;   // the objects would say the pool's key (-0.70 0.50 -0.40)
    SceneTables t;
    t.header.guest_frame = 500;
    Object o;
    o.flags = kObjVisible | kObjSkinned;
    o.bones_count = 99;
    o.id = t.AddString("actor:slot:0");
    t.objects.push_back(o);
    t.engine_objects.push_back(0xA0000030u);
    t.light_objects = w.rows;
    bool r = KeyLightFromRecord(t, key, why);   // the default: the objects route is OFF
    Check(!r, "KEY OBJECTS OFF", "with the switch off the light objects give no key (%s)", why);
    r = KeyLightFromRecord(t, key, why, true) && Near(key, -0.70f, 0.5f, -0.40f);
    Check(r, "RECORD KEY", "with the switch on and no wrestler light state, the key comes from the objects (%s)", why);
    LightState ls = {};
    ls.light[kLsDir + 16] = -0.2f, ls.light[kLsDir + 17] = 0.4f, ls.light[kLsDir + 18] = -0.9f, ls.light[kLsDir + 19] = 1.f;
    t.light_states.push_back(ls);
    LightRef lr;
    lr.object = 0xA0000030u, lr.state = 0, lr.frame = 499;
    t.light_refs.push_back(lr);
    r = KeyLightFromRecord(t, key, why, true) && (std::fabs(key[0] + 0.2f) < 1e-6f && std::fabs(key[1] - 0.4f) < 1e-6f && std::fabs(key[2] + 0.9f) < 1e-6f);
    Check(r, "KEY STATE FIRST", "a walked wrestler light state is NEVER overridden by the objects: key %.2f %.2f %.2f (%s)", key[0], key[1], key[2], why);
    Check(RecordHasWrestler(t), "RECORD WRESTLER", "a skinned actor with a 99-node palette is a wrestler");
    t.objects[0].bones_count = 79;
    Check(!RecordHasWrestler(t), "RECORD WRESTLER", "a 79-node palette (referee / manager) is not");
  }
  {  // the plane fixture values: brock_losswin's arena only (EyePos is per-arena module data: LIVE_MAPS.md E4)
    Check(kPlaneEyeWrestler[0] == -450.f && kPlaneEyeWrestler[1] == -1400.f && kPlaneEyeWrestler[2] == 0.f && kPlaneEyeWrestler[3] == 1.f, "PLANE EYE", "the fixture's wrestler proxy eye point (-450, -1400, 0, 1)");
    Check(kPlanePosDefault[1] == -120.f && kPlanePosDefault[0] == 0.f && kPlanePosDefault[2] == 0.f, "PLANE POS", "the fixture's PlanePos (0, -120, 0, 0)");
    E2Input e2;
    Check(!e2.plane_eye_const && !e2.plane_pos_const && !e2.have_plane_pos, "PLANE NOT LIVE", "an E2Input starts with no eye / PlanePos: the constants are never applied outside the rig");
  }
  if (!plant.empty()) {
    const char* want = plant == "ignore_remap" ? "[INSTALL REMAP]" : plant == "ignore_gate" ? "[WRESTLER GATE]" : plant == "ignore_override" ? "[WRESTLER OVERRIDE]" : "[STAMP ALL MODELS]";
    const bool caught = g_tokens.find(want) != std::string::npos;
    std::printf("%s planted %s: %s (failed: %s)\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? (std::string("caught by its named check ") + want).c_str() : (std::string("its named check ") + want + " did NOT fail").c_str(), g_tokens.c_str());
    return caught ? 0 : 1;
  }
  std::printf("%s  e4_unit\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
