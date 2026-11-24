// Lane E5 (Session 103): unit proof of the two latch readers: the plane shadow's EyePos / PlanePos (src\scene_live\plane_eye_read.h) and the arena's UV-scroll clock c186 (part_clock_read.h),
// against a fake guest memory (tools\scene\e5_fake_memory.h), the record sections 49 / 50 (round trip, a bad section drops itself only) and BuildE2Input's use of the eyes.
//   e5_unit                 every line PASS
//   e5_unit --plant NAME    exit 0 only when the check that sees the plant FAILS: eye_no_pi / eye_rot_sign / eye_kind_slot / eye_degrees (EYE EXACT), clock_period (CLOCK TIME), clock_stride (CLOCK TIME),
//                           clock_one_run (CLOCK RUNS), eye_stale (EYE LIVE: a reader that remembers the first module), eye_no_loaded_mark (EYE NOT LOADED)
// Marks: the tables are FIXTURES (e5_fake_memory.h); what is [C] is the formula (listing sub_827ED6D0) and that its result is the stream's value for tables of this shape.
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer_scene/live_maps_e2.h"
#include "../../src/renderer_scene/part_clock_lookup.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_part_clock.h"
#include "../../src/renderer_scene/rows_plane.h"
#include "../../src/scene_live/stage_clock_flag.h"
#include "../../src/scene_live/part_clock_read.h"
#include "../../src/scene_live/plane_eye_read.h"
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
static bool Near3(const float* a, float x, float y, float z, float tol = 0.01f) { return std::fabs(a[0] - x) < tol && std::fabs(a[1] - y) < tol && std::fabs(a[2] - z) < tol; }

// ---- an independent transcription of the listing (double precision) for the cross-check ------------------------------------------------------------------------------------------------------------
static void RefEye(const float v[4], float ang, double out[3]) {
  const double th = double(ang) * 3.14159265358979323846 / 180.0 - 3.14159265358979323846;   // 0x827ED710-0x827ED718
  const double c = std::cos(th), s = std::sin(th);
  out[0] = v[0] * c + v[2] * s;   // sub_82C329A8 0x82C329E8: x' = x cos + z sin
  out[1] = v[1];
  out[2] = v[2] * c - v[0] * s;   // 0x82C329F0: z' = z cos - x sin
}

static uint32_t Mod() { return 0x40001000u; }

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  if (plant == "eye_no_pi") scene_inputs::g_eye_plant = 1;
  else if (plant == "eye_rot_sign") scene_inputs::g_eye_plant = 2;
  else if (plant == "eye_kind_slot") scene_inputs::g_eye_plant = 3;
  else if (plant == "eye_degrees") scene_inputs::g_eye_plant = 4;
  else if (plant == "clock_period") rdr_scene::live::stagefx::g_clock_plant = 1;
  else if (plant == "clock_stride") rdr_scene::live::stagefx::g_clock_plant = 2;
  else if (plant == "clock_one_run") rdr_scene::live::stagefx::g_clock_plant = 3;
  else if (plant == "clock_slot_join") rdr_scene::g_clock_lookup_plant = 1;
  else if (plant == "eye_stale") scene_inputs::g_eye_plant = 5;
  else if (plant == "eye_no_loaded_mark") scene_inputs::g_eye_plant = 6;
  else if (!plant.empty()) { std::fprintf(stderr, "unknown plant %s\n", plant.c_str()); return 2; }

  // ============================== the plane eyes ==============================
  e5::BuildBrockModule(Mod());
  PlaneEyes pe;
  scene_inputs::EyeWhy why = scene_inputs::ReadPlaneEyes(&e5::ReadGuest, 110, pe);
  Check(why == scene_inputs::EyeWhy::Ok, "EYE READ", "the module of brock_losswin's arena reads (why %u)", unsigned(why));
  const uint32_t want_have = (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7);
  Check(pe.have == want_have, "EYE HAVE", "kinds loaded = 1, 4, 5, 6, 7 (have 0x%x)", pe.have);
  // the stream's values [C: LIVE_MAPS.md Lane E4, e2_check PLANE EYE EXACT on brock_losswin f73 / f110 / f146]
  Check(Near3(pe.eye[1], -450.f, -1400.f, 0.f) && pe.eye[1][3] == 1.f, "EYE EXACT", "the wrestler proxy's eye %.3f %.3f %.3f (stream: -450 -1400 0)", pe.eye[1][0], pe.eye[1][1], pe.eye[1][2]);
  const float ring[4][3] = {{0, -800, 1000}, {-1000, -800, 0}, {0, -800, -1000}, {1000, -800, 0}};
  int hit[4] = {0, 0, 0, 0};
  bool ring_all_known = true;
  for (uint32_t k = 4; k < 8; ++k) {
    bool known = false;
    for (int r = 0; r < 4; ++r)
      if (Near3(pe.eye[k], ring[r][0], ring[r][1], ring[r][2])) hit[r] += 1, known = true;
    ring_all_known = ring_all_known && known;
  }
  Check(ring_all_known && hit[0] == 1 && hit[1] == 1 && hit[2] == 1 && hit[3] == 1, "EYE EXACT", "the four ring kinds give the stream's four ring points, one each (hits %d %d %d %d)", hit[0], hit[1], hit[2], hit[3]);
  // the independent transcription agrees with the reader on every loaded kind (double vs the reader's float math: < 0.01)
  {
    bool ok = true;
    for (uint32_t k = 1; k < 8; ++k) {
      if (!(pe.have & (1u << k))) continue;
      float v[4];
      float ang = 0.f;
      uint32_t raw[4], a;
      e5::ReadGuest(Mod() + 768 + 16 * k, raw, 16), e5::ReadGuest(Mod() + 896 + 4 * k, &a, 4);
      auto fl = [](uint32_t be) { const uint32_t w = scene_inputs::Swap32(be); float f; std::memcpy(&f, &w, 4); return f; };
      for (int c = 0; c < 4; ++c) v[c] = fl(raw[c]);
      ang = fl(a);
      double r[3];
      RefEye(v, ang, r);
      ok = ok && std::fabs(pe.eye[k][0] - r[0]) < 0.01 && std::fabs(pe.eye[k][1] - r[1]) < 0.01 && std::fabs(pe.eye[k][2] - r[2]) < 0.01;
    }
    Check(ok, "EYE EXACT", "the reader equals an independent transcription of sub_827ED6D0 + sub_82C329A8 on every loaded kind");
  }
  // the rotation sense: z' = z cos - x sin at theta = -pi leaves a tiny NEGATIVE z (the stream's wrestler eye z is -1.718e-5 on z7147 f280; ours is -3.9e-5: the library's sin differs from the game's series in magnitude only)
  Check(pe.eye[1][2] < 0.f && pe.eye[1][2] > -1e-3f, "EYE EXACT", "the wrestler eye's z residue is tiny and NEGATIVE like the stream's (%.2e): it fixes the rotation sense, which the symmetric ring set cannot", pe.eye[1][2]);
  Check((pe.flags & kPeHavePos) && pe.plane_pos[0] == 0.f && pe.plane_pos[1] == -120.f && pe.plane_pos[2] == 0.f && pe.plane_pos[3] == 0.f, "PLANE POS", "PlanePos (0, [module+1440] = -120, 0, 0) read from the module");
  Check(pe.frame == 110, "EYE STAMP", "the row carries the hook frame it was read at (%u)", pe.frame);

  // not loaded: w = 0 is the mark that sub_827F19C0 has not written the kind (a plant: the reader ignores it)
  {
    e5::PutKind(Mod(), 5, 7.f, 7.f, 7.f, 0.f, 0.f);   // kind 5 present with garbage and w == 0
    PlaneEyes q;
    scene_inputs::ReadPlaneEyes(&e5::ReadGuest, 111, q);
    const bool ok = (q.have & (1u << 5)) == 0;
    Check(ok, "EYE NOT LOADED", "a kind whose w is 0 gets no bit and no value (have 0x%x)", q.have);
    e5::BuildBrockModule(Mod());
  }
  // liveness: the next read sees the CURRENT words (an arena change loads other tables); a freed module and an empty module are said
  {
    PlaneEyes a, b;
    scene_inputs::ReadPlaneEyes(&e5::ReadGuest, 200, a);
    e5::PutKind(Mod(), 1, 100.f, -900.f, 0.f, 1.f, 0.f);   // another arena's preset: eye (-100, -900, 0)
    scene_inputs::ReadPlaneEyes(&e5::ReadGuest, 201, b);
    const bool live = Near3(b.eye[1], -100.f, -900.f, 0.f) && !Near3(a.eye[1], -100.f, -900.f, 0.f);
    Check(live, "EYE LIVE", "after another arena's preset loads the next read gives its eye (%.1f %.1f %.1f), not the old one", b.eye[1][0], b.eye[1][1], b.eye[1][2]);
    e5::BuildBrockModule(Mod());
    e5::PutWord(e5::kModulePtrWord, 0);
    PlaneEyes c;
    Check(scene_inputs::ReadPlaneEyes(&e5::ReadGuest, 202, c) == scene_inputs::EyeWhy::NoModule, "EYE NO MODULE", "a freed module (singleton word 0) is NoModule");
    e5::BuildBrockModule(Mod());
    for (uint32_t k = 0; k < 8; ++k) e5::PutKind(Mod(), k, 0, 0, 0, 0, 0);
    Check(scene_inputs::ReadPlaneEyes(&e5::ReadGuest, 203, c) == scene_inputs::EyeWhy::NoneLoaded, "EYE NONE LOADED", "a module no preset has written is NoneLoaded (no row, the plane map stays a counted hole)");
    e5::Mem().erase(Mod() + 768 + 16 * 4);
    e5::BuildBrockModule(Mod());
    e5::Mem().erase(Mod() + 768 + 16 * 4);
    Check(scene_inputs::ReadPlaneEyes(&e5::ReadGuest, 204, c) == scene_inputs::EyeWhy::Unreadable, "EYE UNREADABLE", "an unreadable table word is Unreadable, never a partial row");
    e5::BuildBrockModule(Mod());
  }

  // the record: section 49 round trip, a bad layout drops THAT section only, BuildE2Input takes the wrestler eye and PlanePos
  {
    SceneTables t;
    t.header.guest_frame = 111;
    PlaneEyes row;
    scene_inputs::ReadPlaneEyes(&e5::ReadGuest, 110, row);   // stamp 110 = header - 1
    t.plane_eyes.push_back(row);
    PartClock pc;
    pc.object = 0x50000000u, pc.part = 0, pc.time = 1660.f, pc.period = 4000.f, pc.step = 1.f, pc.slot = 12, pc.frame = 110;
    t.part_clocks.push_back(pc);
    const std::vector<uint8_t> bytes = SerializeRecord(t);
    SceneTables u;
    std::string err;
    const bool parsed = ParseRecord(bytes.data(), bytes.size(), u, err);
    Check(parsed && u.plane_eyes.size() == 1 && std::memcmp(&u.plane_eyes[0], &row, sizeof row) == 0 && u.part_clocks.size() == 1 && std::memcmp(&u.part_clocks[0], &pc, sizeof pc) == 0, "RECORD 49 50",
          "sections 49 / 50 round-trip byte for byte (%s)", parsed ? "parsed" : err.c_str());
    SceneTables bad = t;
    bad.plane_eyes[0].version = 9;
    const std::vector<uint8_t> bb = SerializeRecord(bad);
    SceneTables v;
    const bool pb = ParseRecord(bb.data(), bb.size(), v, err);
    Check(pb && v.plane_eyes.empty() && v.plane_errors == 1 && v.part_clocks.size() == 1 && v.clock_errors == 0, "RECORD DROP", "a PlaneEyes row of an unknown layout drops section 49 only (plane_errors %u), the clocks stay", v.plane_errors);
    E2Input in;
    BuildE2Input(t, E2Faults(), in);
    Check(in.plane_eye_record && Near3(in.plane_eye, -450.f, -1400.f, 0.f) && (in.plane_eye_have & (1u << 1)) && in.have_plane_pos && in.plane_pos[1] == -120.f, "E2 EYE",
          "BuildE2Input takes the record's wrestler eye (%.1f %.1f %.1f) and PlanePos y %.0f without being told", in.plane_eye[0], in.plane_eye[1], in.plane_eye[2], in.plane_pos[1]);
    SceneTables none = t;
    none.plane_eyes.clear();
    E2Input in2;
    BuildE2Input(none, E2Faults(), in2);
    Check(!in2.plane_eye_record && !in2.have_plane_pos && !in2.plane_eye_told && !in2.plane_eye_const, "E2 EYE HOLE", "a record without PlaneEyes leaves the eye missing (never a constant): the plane map stays a counted hole");
  }

  // ============================== the stage clock ==============================
  using namespace rdr_scene::live::stagefx;
  e5::Mem().clear();
  const uint32_t stage = 0x60000000u, modA = 0x50000000u, modB = 0x50001000u, modC = 0x50002000u, partsA = 0x51000000u, partsB = 0x51001000u;
  // model A: three parts, step 1.0; parts 0 and 1 share a clock (1660, period 4000), part 2 has another (500, period 900). The J1 shape: 3 UVScroll objects at 1660 advancing 1 a frame.
  auto put_model = [&](uint32_t m, uint32_t parts, uint32_t count, float step) {
    e5::PutWord(m + 372, count), e5::PutWord(m + 484, parts), e5::PutFloat(m + 248, step);
  };
  auto put_part = [&](uint32_t parts, uint32_t p, float t, float period) { e5::PutFloat(parts + 24 * p + 12, t), e5::PutFloat(parts + 24 * p + 16, period); for (uint32_t o : {0u, 4u, 8u, 20u}) e5::PutWord(parts + 24 * p + o, 0); };
  put_model(modA, partsA, 3, 1.0f);
  put_part(partsA, 0, 1660.f, 4000.f), put_part(partsA, 1, 1660.f, 4000.f), put_part(partsA, 2, 500.f, 900.f);
  put_model(modB, partsB, 1, 2.0f);   // the beam cones: step 2
  put_part(partsB, 0, 3320.f, 8000.f);
  put_model(modC, 0, 4, 1.0f);       // no part array (the update returns at once)
  for (uint32_t s = 0; s < kSlotCount; ++s) e5::PutWord(stage + kStageModels + 4 * s, 0);
  e5::PutWord(stage + kStageModels + 4 * 12, modA), e5::PutWord(stage + kStageModels + 4 * 20, modB), e5::PutWord(stage + kStageModels + 4 * 21, modB), e5::PutWord(stage + kStageModels + 4 * 30, modC);
  std::vector<PartClock> rows;
  ClockCensus cn;
  ClockScratch sc;
  ReadStageClocks(&e5::ReadGuest, stage, 300, rows, cn, sc);
  Check(cn.models == 3 && cn.duplicates == 1 && cn.no_parts == 1, "CLOCK MODELS", "3 distinct models read (a duplicate slot counted once, the model with no part array counted no_parts): models %llu dup %llu no_parts %llu",
        (unsigned long long)cn.models, (unsigned long long)cn.duplicates, (unsigned long long)cn.no_parts);
  Check(rows.size() == 4 && rows[0].object == modA && rows[0].part == 0 && rows[1].object == modA && rows[1].part == 2 && rows[2].object == modB && rows[2].slot == 20 && rows[3].object == modB && rows[3].slot == 21, "CLOCK RUNS", "A's equal parts 0-1 fold into one row, part 2 has its own, B (in slots 20 AND 21) one row set PER SLOT (rows %zu)", rows.size());
  float tt = 0.f;
  Check(ClockOf(rows.data(), rows.size(), modA, 1, tt) && tt == 1660.f && ClockOf(rows.data(), rows.size(), modA, 2, tt) && tt == 500.f && ClockOf(rows.data(), rows.size(), modB, 0, tt) && tt == 3320.f, "CLOCK TIME",
        "c186 of (A, part 1) = 1660, (A, part 2) = 500, (B, part 0) = 3320: the time words, not the period");
  Check(rows.size() >= 1 && rows[0].slot == 12 && rows[0].period == 4000.f && rows[0].step == 1.f, "CLOCK SLOT", "a row names its stage slot (12), the period and the step");
  Check(!ClockOf(rows.data(), rows.size(), 0x70000000u, 0, tt), "CLOCK UNKNOWN", "an object with no row has no clock (the consumer keeps its own, never a guess)");
  // advance by the game's rule (sub_82BD5A98 0x82BD5AE4-0x82BD5B14): t = t + step; if t > period: t -= period. After N updates the closed form must hold, with the J1 numbers: 1660 + N per step 1, 2x for step 2.
  {
    const int N = 380;   // J1: c186 went 1660 -> 2040 over the capture
    auto upd = [&](uint32_t parts, uint32_t count, float step) {
      for (uint32_t p = 0; p < count; ++p) {
        uint32_t raw[2];
        e5::ReadGuest(parts + 24 * p + 12, &raw[0], 4), e5::ReadGuest(parts + 24 * p + 16, &raw[1], 4);
        auto fl = [](uint32_t be) { const uint32_t w = scene_inputs::Swap32(be); float f; std::memcpy(&f, &w, 4); return f; };
        float t = fl(raw[0]) + step;
        const float per = fl(raw[1]);
        if (t > per) t -= per;
        e5::PutFloat(parts + 24 * p + 12, t);
      }
    };
    for (int i = 0; i < N; ++i) upd(partsA, 3, 1.0f), upd(partsB, 1, 2.0f);
    rows.clear();
    ClockCensus c2;
    ReadStageClocks(&e5::ReadGuest, stage, 680, rows, c2, sc);
    float a = 0.f, b = 0.f, p2 = 0.f;
    ClockOf(rows.data(), rows.size(), modA, 0, a), ClockOf(rows.data(), rows.size(), modB, 0, b), ClockOf(rows.data(), rows.size(), modA, 2, p2);
    Check(a == 2040.f && b == 3320.f + 760.f && p2 == 880.f, "CLOCK TIME", "after %d updates: step-1 clock %.0f (J1: 1660 -> 2040), step-2 clock %.0f (twice the advance), part with a 900 period %.0f", N, a, b, p2);
    // and a wrap: 100 more updates carry the 900-period part past its period
    for (int i = 0; i < 100; ++i) upd(partsA, 3, 1.0f);
    rows.clear();
    ReadStageClocks(&e5::ReadGuest, stage, 780, rows, c2, sc);
    ClockOf(rows.data(), rows.size(), modA, 2, p2);
    Check(p2 == 80.f, "CLOCK WRAP", "a clock past its period wraps by subtracting it: 880 + 100 updates over a 900 period -> %.0f (901 -> 1, then 79 more)", p2);
  }
  // the front end's join: a mesh "slot N" with its part (assets::Mesh::src_record) asks ClockOfSlot; the slot is the key (the model address is not known to the front end)
  {
    std::vector<PartClock> r3;
    ClockCensus c3;
    ReadStageClocks(&e5::ReadGuest, stage, 900, r3, c3, sc);
    float t0 = 0.f, t1 = 0.f, t2 = 0.f, tb = 0.f;
    const bool a = ClockOfSlot(r3.data(), r3.size(), 12, -1, t0) && ClockOfSlot(r3.data(), r3.size(), 12, 1, t1) && ClockOfSlot(r3.data(), r3.size(), 12, 2, t2);
    float t21 = 0.f;
    const bool b = ClockOfSlot(r3.data(), r3.size(), 20, 0, tb) && ClockOfSlot(r3.data(), r3.size(), 21, 0, t21) && t21 == tb;   // a model in two slots answers in BOTH
    Check(a && b && t0 == t1 && t0 != t2 && tb != t0 && !ClockOfSlot(r3.data(), r3.size(), 99, 0, tb), "CLOCK SLOT JOIN",
          "slot 12 part -1 / 1 share one clock, part 2 has its own, slot 20 (the step-2 model) another, an unknown slot none (%.0f %.0f %.0f %.0f)", t0, t1, t2, tb);
  }
  Check(rdr_scene::live::stageclock::kStampStageClock && scene_inputs::kInputs[scene_inputs::kInStageClock].moved, "BIT 17 ON", "bit 17 (stage_clock) is stamped (S103 L1C): the flag is on and kInputs says moved; --wwe2k14_rdr_scene_hold=17 holds it at run time");
  // refusals: absurd count, non-finite time, unreadable part array
  {
    put_model(modC, partsA, 5000, 1.0f);
    std::vector<PartClock> r2;
    Check(ReadModelClocks(&e5::ReadGuest, modC, 1, r2, 0xFFFFFFFFu, sc) == ClockWhy::Absurd && r2.empty(), "CLOCK REFUSE", "a part count past the cap is Absurd and appends nothing");
    put_model(modC, partsB, 1, 1.0f);
    e5::PutFloat(partsB + 12, std::nanf(""));
    Check(ReadModelClocks(&e5::ReadGuest, modC, 1, r2, 0xFFFFFFFFu, sc) == ClockWhy::NonFinite && r2.empty(), "CLOCK REFUSE", "a non-finite time word is NonFinite and appends nothing");
    put_model(modC, 0x7F000000u, 2, 1.0f);
    Check(ReadModelClocks(&e5::ReadGuest, modC, 1, r2, 0xFFFFFFFFu, sc) == ClockWhy::Unreadable && r2.empty(), "CLOCK REFUSE", "an unreadable part array is Unreadable and appends nothing");
  }

  if (!plant.empty()) {
    const char* want = plant == "eye_no_pi" || plant == "eye_rot_sign" || plant == "eye_kind_slot" || plant == "eye_degrees" ? "[EYE EXACT]" : plant == "eye_stale" ? "[EYE LIVE]" : plant == "eye_no_loaded_mark" ? "[EYE NOT LOADED]"
                       : plant == "clock_one_run" ? "[CLOCK RUNS]" : plant == "clock_slot_join" ? "[CLOCK SLOT JOIN]" : "[CLOCK TIME]";
    const bool caught = g_tokens.find(want) != std::string::npos;
    std::printf("%s planted %s: %s (failed: %s)\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? (std::string("caught by its named check ") + want).c_str() : (std::string("its named check ") + want + " did NOT fail").c_str(), g_tokens.c_str());
    return caught ? 0 : 1;
  }
  std::printf("%s  e5_unit\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
