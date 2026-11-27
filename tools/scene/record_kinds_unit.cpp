// record_kinds_unit: the offline proof of S111 lane FXSKIP (src\renderer_scene\record_kinds.h KeepKnownKinds, called by ParseRecord in record_io.cpp): a section row
// of a kind this reader does not know is skipped by the section's own stride and counted; the rows around it are kept, in order, byte for byte; a record of known
// kinds (every record written before this lane) reads exactly as before. 0 game files, no SDK.
//   record_kinds_unit [--plant NAME]     (built with -DRDR_RECORD_KIND_PLANTS by tools\scene\record_kinds_check.py, which links record_io.cpp)
// A plant is a FAULT in the reader; each must make its NAMED check fail (exit 1 when any check failed):
//   drop_section  an unknown kind drops the whole section (the rule before FXSKIP)   -> "fx.known_rows_kept"
//   keep_unknown  an unknown kind is kept as a row                                  -> "fx.unknown_skipped"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_all.h"
#include "../../src/renderer_scene/record_kinds.h"

using namespace rdr_scene;

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const std::string& name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-36s %s\n", ok ? "PASS" : "FAIL", name.c_str(), why.c_str());
  if (!ok) ++g_fail;
}

static bool Parse(const SceneTables& t, SceneTables& back, std::string& err) {
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  return ParseRecord(bytes.data(), bytes.size(), back, err);
}

template <class Row> static bool Same(const Row& a, const Row& b) { return std::memcmp(&a, &b, sizeof(Row)) == 0; }

// One family: the written rows are {a, a row of kind `unknown`, b}; the reader must give a then b (byte for byte), no row of the unknown kind, and count 1.
template <class Row, class Table, class Errs>
static void Family(const std::string& name, const Row& a, const Row& b, uint8_t unknown, Table table, Errs errs) {
  SceneTables t;
  Row u = a;
  u.kind = unknown;
  table(t) = {a, u, b};
  SceneTables back;
  std::string err;
  const bool ok = Parse(t, back, err);
  const std::vector<Row>& w = table(t);
  const std::vector<Row>& got = table(back);
  size_t at_a = got.size(), at_b = got.size();
  for (size_t i = 0; i < got.size() && at_a == got.size(); ++i)
    if (Same(got[i], w[0])) at_a = i;
  for (size_t i = at_a + 1; i < got.size() && at_b == got.size(); ++i)
    if (Same(got[i], w[2])) at_b = i;
  bool none_unknown = true;
  for (const Row& r : got) none_unknown = none_unknown && r.kind != unknown;
  Check(ok && at_a < got.size() && at_b < got.size(), name + ".known_rows_kept",
        ok ? std::to_string(got.size()) + " rows read of 3 written (the two known rows, in order)" : err);
  Check(ok && none_unknown && got.size() == 2, name + ".unknown_skipped", "kind " + std::to_string(unknown) + " is not a row of the table");
  Check(ok && errs(back) == 1, name + ".unknown_counted", "counter " + std::to_string(errs(back)) + " (1 = the one skipped row)");
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--plant") == 0 && i + 1 < argc) plant = argv[++i];
  g_kind_plant = plant == "drop_section" ? 1 : plant == "keep_unknown" ? 2 : 0;
  if (!plant.empty() && g_kind_plant == 0) {
    std::printf("unknown plant %s\n", plant.c_str());
    return 2;
  }
  auto c_err = [](const SceneTables& s) { return s.c_errors; };

  // ---- every kind-tagged section: a row of an unknown kind between two known rows
  {
    StageFxRow a, b;
    a.kind = kFxScreen, a.slot = 800, a.w = 64, a.h = 32, a.seq = 7;
    b.kind = kFxFlash, b.pos[0] = 1.5f, b.scale = 2.f, b.alpha = 0.5f;
    Family("fx", a, b, 200, [](SceneTables& s) -> std::vector<StageFxRow>& { return s.stage_fx; }, c_err);
  }
  {
    ModelLightRow a, b;
    a.kind = kMlSet, a.asset = 0, a.set = 500, a.col[0] = 0.25f;
    b.kind = kMlMesh, b.asset = 0, b.mesh = 3, b.set = 0;
    Family("model_lights", a, b, 7, [](SceneTables& s) -> std::vector<ModelLightRow>& { return s.model_lights; }, c_err);
  }
  {
    StageLookRow a, b;
    a.kind = kSlEntrance, a.flags = kSlfEntrance;
    b.kind = kSlRigSlot, b.rig_slot = 2, b.col[0] = 0.75f;
    Family("stage_look", a, b, 250, [](SceneTables& s) -> std::vector<StageLookRow>& { return s.stage_look; }, c_err);
  }
  {
    CrowdPropRow a, b;
    a.kind = kCpSign, a.model_key = 40000, a.flags = kCpfFree;
    b.kind = kCpHand, b.model_key = 41001, b.flags = kCpfBone;
    Family("crowd_props", a, b, 9, [](SceneTables& s) -> std::vector<CrowdPropRow>& { return s.crowd_props; }, c_err);
  }
  {
    CrowdSingleRow a, b;
    a.kind = 1, a.type = 3, a.bones_count = 0;
    b.kind = 1, b.type = 5, b.flags = kCsfHidden;
    Family("crowd_singles", a, b, 4, [](SceneTables& s) -> std::vector<CrowdSingleRow>& { return s.crowd_singles; }, c_err);
  }
  {
    ArenaWorld a, b;
    a.kind = kAwObject, a.object = 0xAB000010u, a.frame = 4000, a.slot = 1;
    b.kind = kAwClock, b.frame = 4000, b.origin = 3988, b.clock = 12.f;
    Family("arena_worlds", a, b, 9, [](SceneTables& s) -> std::vector<ArenaWorld>& { return s.arena_worlds; },
           [](const SceneTables& s) { return s.arena_world_errors; });
  }

  // ---- old records: what every writer made before this lane reads exactly as before
  {
    SceneTables t;
    for (uint8_t k = kFxScreen; k <= kFxLast; ++k) {
      StageFxRow r;
      r.kind = k, r.slot = uint16_t(100 + k);
      t.stage_fx.push_back(r);
    }
    const std::vector<uint8_t> bytes = SerializeRecord(t);
    SceneHeader h;
    std::memcpy(&h, bytes.data(), sizeof h);
    Check(h.version == kSceneVersion, "old.version_unchanged", "a record without crowd run markers is still version " + std::to_string(kSceneVersion));
    SceneTables back;
    std::string err;
    const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
    bool same = ok && back.stage_fx.size() == t.stage_fx.size();
    for (size_t i = 0; same && i < t.stage_fx.size(); ++i) same = Same(back.stage_fx[i], t.stage_fx[i]);
    Check(same && back.c_errors == 0, "old.every_known_fx_kind_reads", ok ? "kinds kFxScreen..kFxLast all kept, c_errors " + std::to_string(back.c_errors) : err);
  }
  {
    SceneTables t;
    t.params.push_back(ParamValue{});
    SceneTables back;
    std::string err;
    const bool ok = Parse(t, back, err);
    Check(ok && back.params.size() == 1 && back.stage_fx.empty() && back.model_lights.empty() && back.stage_look.empty() && back.crowd_props.empty() &&
              back.crowd_singles.empty() && back.arena_worlds.empty() && back.c_errors == 0 && back.arena_world_errors == 0,
          "old.no_sections_reads", ok ? "a record without the sections reads, empty tables, no errors" : err);
  }
  // ---- an unknown LAYOUT is a different row shape: it still drops its section only
  {
    SceneTables t;
    StageFxRow a, b;
    a.kind = kFxScreen, b.kind = kFxFlash, b.layout = 2;
    t.stage_fx = {a, b};
    t.params.push_back(ParamValue{});
    SceneTables back;
    std::string err;
    const bool ok = Parse(t, back, err);
    Check(ok && back.stage_fx.empty() && back.c_errors == 1 && back.params.size() == 1, "fx.unknown_layout_drops_section",
          ok ? "the section is dropped and counted; the rest of the record reads" : err);
  }

  std::printf("record_kinds_unit: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
