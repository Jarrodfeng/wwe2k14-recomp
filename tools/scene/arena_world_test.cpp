// arena_world_test: the unit test of lane 130's writer (Session 101; docs\renderer\ARENA_OURS.md "D3 gaps"). 0 game runs, no SDK, no game memory.
//   arena_world_test [--plant NAME] [--write-record PATH]
// It runs the SHIPPED code: the pure functions (src\scene_live\arena_world.h), the model-entry tracker (arena_world_hook.h) against a fake guest memory, the
// contributor's Emit (arena_world_contrib.h) and the record file code (record_io: kScArenaWorlds round trip, an older record without the section, a layout this reader
// does not know). Plants (each must make a NAMED check fail; the exit code is 1 when any check failed):
//   placed_identity        the placed slots' world written as identity (the record's identity kept)          -> "hook.placed_translation"
//   moving_static          a moving object's frame-0 transform held                                           -> "hook.moving_changes"
//   clock_frozen           the clock held at its first value                                                  -> "contrib.clock_advances"
//   stamp_off_by_one       the entry joined at the neighbour frame (g - 1)                                    -> "hook.moving_exact", "join.by_stamp"
//   refuse_foreign_palette a call whose r5 is not [model+200] dropped                                         -> "hook.foreign_palette_recorded"
//   node1_assumed          bone 0 taken as node 1 whatever the part descriptor says                           -> "hook.node_from_part"
//   origin_after_reload    the clock of a closed frame computed with the origin of a LATER stage set-up       -> "contrib.clock_origin_snapshot"
// --write-record PATH writes the sample record the Python reader test reads (tools\skip\ledger_io.py Record.arena_worlds).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/arena_world_contrib.h"

using namespace rdr_scene;
using namespace rdr_scene::live::arenaworld;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;

static_assert(kScArenaWorlds == 15 && sizeof(ArenaWorld) == 68, "kScArenaWorlds is section 15 and ArenaWorld is 68 bytes (the Python reader pins the same)");

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* name, const char* why = "") {
  ++g_checks;
  std::printf("%s  %-30s %s\n", ok ? "PASS" : "FAIL", name, why);
  if (!ok) ++g_fail;
}

// ---- a fake guest: a model has [model+200] (its own palette), optionally [model+824] -> part descriptors (count at +8, first bone word at +12) ----
struct Guest {
  std::map<uint32_t, uint32_t> words;
  std::map<uint32_t, std::vector<float>> mats;  // address -> 16 floats
  void Palette(uint32_t pal, int node, const float m[16]) { mats[pal + 64u * uint32_t(node - 1)].assign(m, m + 16); }
  void Model(uint32_t model, uint32_t own_pal, uint32_t desc, uint32_t bone0) {
    words[model + 200] = own_pal;
    if (desc) {
      words[model + 824] = desc;
      words[desc + 8] = 1;
      words[desc + 12] = bone0;
    }
  }
  bool ReadWord(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  bool ReadMat(uint32_t a, float* m) const {
    auto it = mats.find(a);
    if (it == mats.end()) return false;
    std::memcpy(m, it->second.data(), 64);
    return true;
  }
};

// A row-vector 4x4 with the rotation about Y by `a` and the translation (x, y, z) in m[12..14].
static void Mat(float* m, float a, float x, float y, float z) {
  const float c = std::cos(a), s = std::sin(a);
  const float v[16] = {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, x, y, z, 1};
  std::memcpy(m, v, sizeof v);
}

int main(int argc, char** argv) {
  std::string plant, write_record;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (std::string(argv[i]) == "--write-record" && i + 1 < argc) write_record = argv[++i];
  }

  // ---- pure functions ----
  {
    float m[16], rows[12], back[16];
    Mat(m, 0.7f, 561.f, 0.f, -560.f);
    RowsFromMatrix(m, rows);
    MatrixFromRows(rows, back);
    bool same = true;
    for (int i = 0; i < 16; ++i) same = same && (i == 3 || i == 7 || i == 11 || i == 15 ? true : back[i] == m[i]);
    Check(same, "pure.rows_roundtrip", "RowsFromMatrix / MatrixFromRows");
    Check(rows[3] == 561.f && rows[11] == -560.f && rows[0] == m[0] && rows[1] == m[4] && rows[2] == m[8], "pure.rows_layout", "row k = column k of the matrix, translation last");
    float p[12];
    bool all = true;
    const float kx[4] = {561.f, -561.f, 532.9f, -532.9f}, kz[4] = {-560.f, -560.f, 560.f, 560.f};
    for (uint32_t s = 390; s <= 397; ++s) all = all && PlacedRows(4, s, p) && p[3] == kx[(s - 390) & 3] && p[11] == kz[(s - 390) & 3] && p[0] == 1.f && p[5] == 1.f && p[10] == 1.f && p[7] == 0.f;
    Check(all, "pure.placed_table", "slots 390-397 of arena 4: the four corners, identity rotation");
    Check(!PlacedRows(4, 389, p) && !PlacedRows(4, 398, p) && !PlacedRows(29, 390, p), "pure.placed_unknown", "other slot / unmeasured arena = unknown (false), never a guess");
    float c = -1.f;
    Check(ClockAt(2000, 1700, true, c) && c == 300.f && !ClockAt(2000, 1700, false, c) && ClockAt(5, 0xFFFFFFFEu, true, c) && c == 7.f, "pure.clock", "frame - origin, wrap-safe, unknown without an origin");
  }

  // ---- the tracker against a fake guest ----
  Guest g;
  float mp[16], mm[16], mi[16], mf[16], mn1[16], mn2[16];
  Mat(mp, 0.f, 561.f, 0.f, -560.f);   // a placed prop
  Mat(mi, 0.f, 0.f, 0.f, 0.f);        // a static slot (identity)
  Mat(mf, 0.f, 7.f, 0.f, 0.f);        // a foreign palette (sub_82C13EC8 passes the caller's array)
  Mat(mn1, 0.f, 1.f, 1.f, 1.f);       // node 1 of the two-node model
  Mat(mn2, 0.f, 100.f, 0.f, 0.f);     // node 2 = the part's bone 0
  const uint32_t D = 0x60000000;      // part descriptors
  g.Model(0xAB1196E0, 0x50000000, D + 0x100, 1);
  g.Palette(0x50000000, 1, mp);
  g.Model(0xAB0F1390, 0x50001000, D + 0x200, 1);
  g.Palette(0x50001000, 1, mi);
  g.Model(0xAB0F3310, 0x50002000, D + 0x300, 1);
  g.Model(0xAB0F9999, 0x50003000, D + 0x400, 1);  // r5 below is another array than [model+200]
  g.Palette(0x50003800, 1, mf);
  g.Model(0xAB0FAAAA, 0x50004000, D + 0x500, 2);  // bone 0 = node 2
  g.Palette(0x50004000, 1, mn1);
  g.Palette(0x50004000, 2, mn2);
  g.Model(0xAB0FBBBB, 0x50005000, 0, 0);          // no part descriptor: node 1 assumed
  g.Palette(0x50005000, 1, mi);
  auto rw = [&](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); };
  auto rm = [&](uint32_t a, float* m) { return g.ReadMat(a, m); };
  WorldContributor C;
  C.tracker.plant = plant == "placed_identity" ? Plant::PlacedIdentity : plant == "moving_static" ? Plant::MovingStatic : plant == "clock_frozen" ? Plant::ClockFrozen
                    : plant == "refuse_foreign_palette" ? Plant::RefuseForeignPalette : plant == "node1_assumed" ? Plant::Node1Assumed : plant == "origin_after_reload" ? Plant::OriginAfterReload : Plant::None;
  const bool off_by_one = plant == "stamp_off_by_one";
  C.tracker.OnStageSetup(3805);
  for (uint64_t frame = 4000; frame < 4004; ++frame) {
    Mat(mm, 0.1f * float(frame - 4000), 1507.4595f, -581.9564f, 3052.5491f);  // the moving piece turns each frame
    g.Palette(0x50002000, 1, mm);
    C.tracker.OnModelCall(0xAB1196E0, 0x50000000, frame, rw, rm);
    C.tracker.OnModelCall(0xAB1196E0, 0x50000000, frame, rw, rm);  // a second part of the same object: one entry
    C.tracker.OnModelCall(0xAB0F1390, 0x50001000, frame, rw, rm);
    C.tracker.OnModelCall(0xAB0F3310, 0x50002000, frame, rw, rm);
    C.tracker.OnModelCall(0xAB0F9999, 0x50003800, frame, rw, rm);  // r5 != [model+200]: recorded, flagged
    C.tracker.OnModelCall(0xAB0FAAAA, 0x50004000, frame, rw, rm);
    C.tracker.OnModelCall(0xAB0FBBBB, 0x50005000, frame, rw, rm);
    C.tracker.OnModelCall(0xAB0F1390, 0, frame, rw, rm);  // null palette
  }
  C.tracker.OnModelCall(0xAB0F1390, 0x50001000, 4010, rw, rm);  // closes frame 4003
  C.tracker.OnStageSetup(5000);                                  // a later stage set-up (a reload): must not reach the frames already closed
  Stats st = C.tracker.GetStats();
  Check(st.recorded == 25 && st.foreign_palette == 4 && st.node_assumed == 4 && st.unreadable == 4, "hook.counts",
        ("recorded " + std::to_string(st.recorded) + " foreign " + std::to_string(st.foreign_palette) + " node_assumed " + std::to_string(st.node_assumed) + " unreadable " + std::to_string(st.unreadable)).c_str());
  SceneTables t;
  Counters cn;
  FrameInfo fi;
  C.Emit(fi, t, cn);
  size_t objs = 0, clocks = 0;
  for (const ArenaWorld& w : t.arena_worlds) {
    if (w.kind == kAwObject) ++objs;
    if (w.kind == kAwClock) ++clocks;
  }
  auto find = [&](uint32_t frame, uint32_t obj, uint8_t kind) {
    const uint32_t jf = off_by_one ? frame - 1 : frame;  // stamp_off_by_one: the consumer joins at the neighbour frame
    return FindEntry(t.arena_worlds.data(), t.arena_worlds.size(), jf, obj, kind);
  };
  Check(objs == 24 && clocks == 4 && cn.objects == 0, "contrib.entries", ("objects " + std::to_string(objs) + " clocks " + std::to_string(clocks) + " (scene Objects touched: " + std::to_string(cn.objects) + ")").c_str());
  const ArenaWorld* placed = find(4001, 0xAB1196E0, kAwObject);
  Check(placed && placed->world[3] == 561.f && placed->world[11] == -560.f && placed->slot == 1 && placed->origin == 0, "hook.placed_translation", placed ? "the placed prop's translation reaches the field, node 1" : "no entry");
  const ArenaWorld* a = find(4000, 0xAB0F3310, kAwObject);
  const ArenaWorld* b = find(4003, 0xAB0F3310, kAwObject);
  Check(a && b && !RowsEqual(a->world, b->world), "hook.moving_changes", "a turning piece's rows differ between frame 4000 and 4003");
  float e[12];
  Mat(mm, 0.3f, 1507.4595f, -581.9564f, 3052.5491f);
  RowsFromMatrix(mm, e);
  Check(b && RowsEqual(b->world, e), "hook.moving_exact", "the rows are the game's matrix, transposed");
  const ArenaWorld* fo = FindEntry(t.arena_worlds.data(), t.arena_worlds.size(), 4001, 0xAB0F9999, kAwObject);
  Check(fo && fo->world[3] == 7.f && (fo->origin & kAwForeignPalette), "hook.foreign_palette_recorded", fo ? "r5 is read even when it is not [model+200]; the flag is set" : "the foreign-palette call was dropped");
  const ArenaWorld* n2 = FindEntry(t.arena_worlds.data(), t.arena_worlds.size(), 4001, 0xAB0FAAAA, kAwObject);
  Check(n2 && n2->world[3] == 100.f && n2->slot == 2 && !(n2->origin & kAwNodeAssumed), "hook.node_from_part", n2 ? "bone 0 = node 2 from the part descriptor" : "no entry");
  const ArenaWorld* na = FindEntry(t.arena_worlds.data(), t.arena_worlds.size(), 4001, 0xAB0FBBBB, kAwObject);
  Check(na && na->slot == 1 && (na->origin & kAwNodeAssumed), "hook.node_assumed_flag", "no descriptor: node 1 with kAwNodeAssumed");
  const ArenaWorld* c0 = find(4000, 0, kAwClock);
  const ArenaWorld* c3 = find(4003, 0, kAwClock);
  Check(c0 && c3 && c0->clock == 195.f && c3->clock == 198.f, "contrib.clock_advances", c0 && c3 ? ("clock 4000: " + std::to_string(c0->clock) + ", 4003: " + std::to_string(c3->clock)).c_str() : "no clock entry");
  const ArenaWorld* k3 = FindEntry(t.arena_worlds.data(), t.arena_worlds.size(), 4003, 0, kAwClock);
  Check(k3 && k3->origin == 3805 && k3->clock == 198.f, "contrib.clock_origin_snapshot", k3 ? ("origin used " + std::to_string(k3->origin) + " after a stage set-up at 5000").c_str() : "no clock entry");
  const ArenaWorld* by_stamp = find(4002, 0xAB0F3310, kAwObject);
  float want[12];
  Mat(mm, 0.2f, 1507.4595f, -581.9564f, 3052.5491f);
  RowsFromMatrix(mm, want);
  Check(by_stamp && by_stamp->frame == 4002 && by_stamp->object == 0xAB0F3310 && RowsEqual(by_stamp->world, want), "join.by_stamp", by_stamp ? "joined by the hook's frame stamp" : "no entry");

  // ---- the record file code ----
  {
    std::string err;
    std::vector<uint8_t> bytes = SerializeRecord(t);
    SceneTables back;
    const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
    bool same = ok && back.arena_worlds.size() == t.arena_worlds.size() &&
                std::memcmp(back.arena_worlds.data(), t.arena_worlds.data(), t.arena_worlds.size() * sizeof(ArenaWorld)) == 0;
    Check(same, "record.roundtrip", ok ? "kScArenaWorlds round trip is byte-identical" : err.c_str());
    if (!write_record.empty()) {
      SceneTables s = t;
      s.header.source = uint8_t(Source::Live);
      s.header.guest_frame = 4003;
      std::string werr;
      Check(WriteRecord(write_record, s, werr), "record.write_sample", werr.c_str());
    }
    SceneTables old;
    old.header.source = uint8_t(Source::Live);
    std::vector<uint8_t> ob = SerializeRecord(old);
    SceneTables oback;
    Check(ParseRecord(ob.data(), ob.size(), oback, err) && oback.arena_worlds.empty() && oback.arena_world_errors == 0, "record.old_opens", "a record without section 15 reads, with an empty table");
    // a layout this reader does not know drops THIS section only, counted; the rest of the record is read
    SceneTables bad = t;
    bad.header.source = uint8_t(Source::Live);
    bad.params.push_back(ParamValue{});
    bad.arena_worlds[0].layout = 2;
    std::vector<uint8_t> bb = SerializeRecord(bad);
    SceneTables bback;
    err.clear();
    const bool parsed = ParseRecord(bb.data(), bb.size(), bback, err);
    Check(parsed && bback.arena_worlds.empty() && bback.arena_world_errors == 1 && bback.params.size() == 1, "record.unknown_layout_dropped", parsed ? "the section is dropped and counted; the params table is intact" : err.c_str());
    std::string verr;
    const bool v_bad = !rdr_scene::live::Validate(bad, verr);  // the live writer stays strict: its own bad entry is a bug, not data
    const std::string bad_msg = verr;
    verr.clear();
    const bool v_good = rdr_scene::live::Validate(t, verr);
    Check(v_bad && v_good, "record.validate", bad_msg.c_str());
  }
  std::printf("arena_world_test: %d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
