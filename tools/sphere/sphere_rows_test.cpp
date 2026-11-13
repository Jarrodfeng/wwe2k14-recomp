// sphere_rows_test: the offline proof of lane 242's inert recorder for the wrestlers' sphere occluders (H3; docs\renderer\SKIP_GAME_WORK_MAP.md 18.8). 0 game runs, no SDK, no game memory.
//   sphere_rows_test [--plant NAME] [--write-record PATH]
// It runs the SHIPPED code: the decode and the tracker (src\scene_live\sphere_hook.h) against a fake guest, the contributor's Emit (sphere_contrib.h) and the record file code (record_io: kScSphereOcc
// round trip, an unknown layout, a count past 2, an older record). THE ORACLE is the listing of sub_826DF8E0 (0x826DF8E0-0x826DFCB8: the component's fields +32 character, +36 ready, +40 count, +44
// table of 52-byte rows, +48 P array, +52 V array; character +48 = skeleton; global word 0x8392DDA4), never the stream: the fake guest is BUILT from those offsets, the "game" rewrites the output
// arrays at every call with values of its own formula (frame and sphere dependent), and every expected value is computed here from what the "game" wrote at that call.
// Plants (each must make a NAMED check fail; exit code 1 when any check failed):
//   first_call       a second call of the same skeleton in a frame does not replace the first              -> "hook.last_call"
//   skeleton_off     the skeleton is [char+52], not [char+48]                                                -> "hook.object"
//   v_from_p         V is read from the P array                                                              -> "hook.arrays"
//   stale_frame      the row is stamped with the previous hook frame                                         -> "hook.frame_stamp"
//   skipped_as_ran   a sphere whose node is -1 (skipped by the game) is marked as run                        -> "hook.skipped_sphere"
//   no_ready_check   the early outs are ignored                                                              -> "hook.early_outs"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <fstream>

#include "../../src/renderer_scene/record_io.h"
#include "../../src/scene_live/sphere_contrib.h"

using namespace rdr_scene;
using namespace rdr_scene::live::spheres;
using rdr_scene::live::Counters;
using rdr_scene::live::FrameInfo;

static_assert(kScSphereOcc == 27 && kScCount == 28 && sizeof(SphereOccRow) == 200 && sizeof(uint32_t) == 4 && kSphereMax == 2 && kSphereRecWords == 13, "section 27, 200 bytes a row; the Python reader pins the same");
static_assert(offsetof(SphereOccRow, p) == 32 && offsetof(SphereOccRow, v) == 64 && offsetof(SphereOccRow, table) == 96, "the layout tools\\skip\\ledger_io.py Record.sphere_occ reads");

static int g_fail = 0, g_checks = 0;
static void Check(bool ok, const char* name, const std::string& why = "") {
  ++g_checks;
  std::printf("%s  %-30s %s\n", ok ? "PASS" : "FAIL", name, why.c_str());
  if (!ok) ++g_fail;
}

struct Guest {
  std::unordered_map<uint32_t, uint32_t> words;
  bool ReadWord(uint32_t a, uint32_t& v) const {
    auto it = words.find(a);
    if (it == words.end()) return false;
    v = it->second;
    return true;
  }
  void PutFloat(uint32_t a, float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    words[a] = b;
  }
  bool ReadBytes(uint32_t a, void* dst, size_t n) const {   // raw BIG-endian bytes as the real ReadGuest returns them
    auto* d = static_cast<uint8_t*>(dst);
    for (size_t i = 0; i < n; i += 4) {
      auto it = words.find(a + uint32_t(i));
      if (it == words.end()) return false;
      const uint32_t w = __builtin_bswap32(it->second);
      std::memcpy(d + i, &w, 4);
    }
    return true;
  }
};

// The fake game: addresses are the test's; the OFFSETS are the listing's.
constexpr uint32_t kG = 0x8392DDA4;
constexpr uint32_t kComp0 = 0x40100000, kComp1 = 0x40110000, kChar0 = 0x40200000, kChar1 = 0x40210000, kSkel0 = 0x40300000, kSkel1 = 0x40310000;
constexpr uint32_t kTab0 = 0x40400000, kTab1 = 0x40410000, kP0 = 0x40500000, kP1 = 0x40510000, kV0 = 0x40600000, kV1 = 0x40610000;

struct Wr { uint32_t comp, ch, skel, tab, pa, va; };
static const Wr kW[2] = {{kComp0, kChar0, kSkel0, kTab0, kP0, kV0}, {kComp1, kChar1, kSkel1, kTab1, kP1, kV1}};

static void BuildComponent(Guest& g, const Wr& w, uint32_t count, uint32_t nodeB1 = 57) {
  g.words[kG] = 0x40FF0000;
  g.words[w.comp + kOffChar] = w.ch;
  g.words[w.comp + kOffReady] = 0x40770000;
  g.words[w.comp + kOffCount] = count;
  g.words[w.comp + kOffTable] = w.tab;
  g.words[w.comp + kOffP] = w.pa;
  g.words[w.comp + kOffV] = w.va;
  g.words[w.ch + kCharSkeleton] = w.skel;
  g.words[w.ch + kCharSkeleton + 4] = 0x40999990;   // the SkeletonOff plant reads this one
  for (uint32_t i = 0; i < 2; ++i) {
    const uint32_t r = w.tab + 52 * i;
    g.words[r + 0] = i == 0 ? 6 : 10;               // roles
    g.words[r + 4] = i == 0 ? 5 : 9;
    g.words[r + 8] = i == 0 ? 59 : 84;              // nodes (listing +8 / +12)
    g.words[r + 12] = i == 1 ? nodeB1 : 57;
    g.words[r + 16] = 721 - i;                      // key
    g.PutFloat(r + 20, 0.f), g.PutFloat(r + 24, i == 0 ? 90.f : -90.f);
    g.PutFloat(r + 28, -0.188f), g.PutFloat(r + 32, i == 0 ? 3.364f : -3.364f), g.PutFloat(r + 36, 0.f);
    g.PutFloat(r + 40, 12.f), g.PutFloat(r + 44, 1.4f), g.PutFloat(r + 48, 0.4f);
  }
}
// What the game's update writes at its call number `n` of frame `f` for wrestler w (the test's own formula).
static void GameUpdate(Guest& g, const Wr& w, uint32_t f, uint32_t n) {
  for (uint32_t i = 0; i < 2; ++i) {
    g.PutFloat(w.pa + 16 * i + 0, 10.f * float(f) + float(i) + 0.25f * float(n));
    g.PutFloat(w.pa + 16 * i + 4, -150.f + float(i) * 2.f + float(n));
    g.PutFloat(w.pa + 16 * i + 8, 660.f + float(f % 7));
    g.PutFloat(w.pa + 16 * i + 12, 12.f);
    g.PutFloat(w.va + 16 * i + 0, 0.5f + 0.01f * float(i));
    g.PutFloat(w.va + 16 * i + 4, 0.25f * float(n));
    g.PutFloat(w.va + 16 * i + 8, 0.125f * float(f % 5));
    g.PutFloat(w.va + 16 * i + 12, 1.0f + float(i) * 0.1f + 0.01f * float(n));
  }
}
struct Exp { float p[2][4], v[2][4]; };
static Exp Expected(const Guest& g, const Wr& w) {   // read back from the memory the game wrote (host floats)
  Exp e{};
  for (uint32_t i = 0; i < 2; ++i)
    for (int k = 0; k < 4; ++k) {
      uint32_t a = 0, b = 0;
      g.ReadWord(w.pa + 16 * i + 4 * k, a), g.ReadWord(w.va + 16 * i + 4 * k, b);
      std::memcpy(&e.p[i][k], &a, 4), std::memcpy(&e.v[i][k], &b, 4);
    }
  return e;
}
static bool Same(const SphereOccRow& r, const Exp& e) { return std::memcmp(r.p, e.p, sizeof e.p) == 0 && std::memcmp(r.v, e.v, sizeof e.v) == 0; }

static auto RW(const Guest& g) { return [&g](uint32_t a, uint32_t& v) { return g.ReadWord(a, v); }; }
static auto RB(const Guest& g) { return [&g](uint32_t a, void* d, size_t n) { return g.ReadBytes(a, d, n); }; }

int main(int argc, char** argv) {
  std::string plant_name, write_record;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--plant" && i + 1 < argc) plant_name = argv[++i];
    else if (a == "--write-record" && i + 1 < argc) write_record = argv[++i];
  }
  Plant plant = Plant::None;
  if (plant_name == "first_call") plant = Plant::FirstCall;
  else if (plant_name == "skeleton_off") plant = Plant::SkeletonOff;
  else if (plant_name == "v_from_p") plant = Plant::VFromP;
  else if (plant_name == "stale_frame") plant = Plant::StaleFrame;
  else if (plant_name == "skipped_as_ran") plant = Plant::SkippedAsRan;
  else if (plant_name == "no_ready_check") plant = Plant::NoReadyCheck;
  else if (!plant_name.empty()) { std::printf("unknown plant %s\n", plant_name.c_str()); return 2; }

  // ---- the decode: arrays, object, flags, table ----
  {
    Guest g;
    BuildComponent(g, kW[0], 2);
    GameUpdate(g, kW[0], 100, 1);
    Tracker tr;
    tr.plant = plant;
    const bool rec = tr.OnComponent(kW[0].comp, 100, RW(g), RB(g));
    tr.CloseBefore(101);
    FrameBuffer fb;
    const bool taken = tr.TakeNext(fb);
    const SphereOccRow* r = taken && fb.rows.size() == 1 ? &fb.rows[0] : nullptr;
    const Exp e = Expected(g, kW[0]);
    Check(rec && r && Same(*r, e) && r->count == 2, "hook.arrays", "P (c168 / c169) and V (c170 / c171) are the component's output arrays, both spheres");
    Check(r && r->object == kSkel0 && (r->flags & kSoSkeleton) && r->character == kChar0 && r->component == kComp0, "hook.object", "the skeleton is [[this+32]+48] = the record's engine object");
    Check(r && r->frame == 100 && fb.frame == 100, "hook.frame_stamp", "the row carries the HOOK's guest frame");
    Check(r && (r->flags & kSoSpheres) && (r->flags & kSoTable) && (r->flags & kSoSphere0Ran) && (r->flags & kSoSphere1Ran) && r->table[0][2] == 59 && r->table[1][3] == 57 && r->table[0][4] == 721, "hook.table", "the 52-byte rows are copied (nodes +8 / +12, key +16)");
  }
  // ---- the last call of a frame wins; calls counted ----
  {
    Guest g;
    BuildComponent(g, kW[0], 2);
    Tracker tr;
    tr.plant = plant;
    GameUpdate(g, kW[0], 200, 1);
    tr.OnComponent(kW[0].comp, 200, RW(g), RB(g));
    GameUpdate(g, kW[0], 200, 2);
    const Exp second = Expected(g, kW[0]);
    tr.OnComponent(kW[0].comp, 200, RW(g), RB(g));
    tr.CloseBefore(201);
    FrameBuffer fb;
    tr.TakeNext(fb);
    Check(fb.rows.size() == 1 && Same(fb.rows[0], second) && fb.rows[0].calls == 2, "hook.last_call", "a skeleton updated twice in a frame: one row, the LAST call's values, calls = 2");
  }
  // ---- early outs: nothing to record when the game did nothing; a count past 2 is refused ----
  {
    Guest g;
    BuildComponent(g, kW[0], 2);
    GameUpdate(g, kW[0], 300, 1);
    Tracker tr;
    tr.plant = plant;
    g.words[kG] = 0;
    const bool a = tr.OnComponent(kW[0].comp, 300, RW(g), RB(g));
    g.words[kG] = 0x40FF0000;
    g.words[kW[0].comp + kOffReady] = 0;
    const bool b = tr.OnComponent(kW[0].comp, 300, RW(g), RB(g));
    g.words[kW[0].comp + kOffReady] = 0x40770000;
    g.words[kW[0].comp + kOffCount] = 0;
    const bool c = tr.OnComponent(kW[0].comp, 300, RW(g), RB(g));
    g.words[kW[0].comp + kOffCount] = 3;
    const bool d = tr.OnComponent(kW[0].comp, 300, RW(g), RB(g));
    const Stats s = tr.GetStats();
    Check(!a && !b && !c && !d && s.early_out == 3 && s.bad_count == 1 && s.recorded == 0, "hook.early_outs", "global word 0 / [this+36] 0 / count 0 record nothing; count 3 is refused (counted)");
    Check(!tr.OnComponent(0x4FFF0000, 301, RW(g), RB(g)) && tr.GetStats().unreadable == 1, "hook.unreadable", "an unreadable component is counted, never a row");
  }
  // ---- a skipped sphere (node < 0) is not marked as run ----
  {
    Guest g;
    BuildComponent(g, kW[0], 2, 0xFFFFFFFFu);
    GameUpdate(g, kW[0], 400, 1);
    Tracker tr;
    tr.plant = plant;
    tr.OnComponent(kW[0].comp, 400, RW(g), RB(g));
    tr.CloseBefore(401);
    FrameBuffer fb;
    tr.TakeNext(fb);
    Check(fb.rows.size() == 1 && (fb.rows[0].flags & kSoSphere0Ran) && !(fb.rows[0].flags & kSoSphere1Ran) && tr.GetStats().skipped_sphere == (plant == Plant::SkippedAsRan ? 0u : 1u), "hook.skipped_sphere", "the game skips a sphere whose node is -1: its bit stays clear, the output is stale");
  }
  // ---- the game's second skip: node*64 + [skel+192] == 0 (node 0 with a null world array) ----
  {
    Guest g;
    BuildComponent(g, kW[0], 2);
    g.words[kW[0].tab + 8] = 0;   // sphere 0: node A = 0
    g.words[kSkel0 + 192] = 0;    // the world array pointer is null
    GameUpdate(g, kW[0], 450, 1);
    Tracker tr;
    tr.OnComponent(kW[0].comp, 450, RW(g), RB(g));
    tr.CloseBefore(451);
    FrameBuffer fb;
    tr.TakeNext(fb);
    Check(fb.rows.size() == 1 && !(fb.rows[0].flags & kSoSphere0Ran) && (fb.rows[0].flags & kSoSphere1Ran), "hook.null_matrix", "node*64 + [skel+192] == 0 is the game's beq skip: the ran bit stays clear");
  }
  // ---- a row without a skeleton is recorded, not counted unknown (N2) ----
  {
    Guest g;
    BuildComponent(g, kW[0], 2);
    g.words[kChar0 + kCharSkeleton] = 0;
    GameUpdate(g, kW[0], 460, 1);
    live::spheres::SphereContributor con;
    con.tracker.OnComponent(kW[0].comp, 460, RW(g), RB(g));
    SceneTables t;
    Counters c{};
    FrameInfo fi;
    fi.guest_frame = 461;
    con.Emit(fi, t, c);
    Check(t.sphere_occ.size() == 1 && t.sphere_occ[0].object == 0 && !(t.sphere_occ[0].flags & kSoSkeleton) && c.unknown == 0, "contrib.no_skeleton_consistent", "recorded with object 0, not counted unknown");
  }
  // ---- live order: the game's frames, then the frame boundary's Emit; two wrestlers; no row twice ----
  {
    Guest g;
    BuildComponent(g, kW[0], 2), BuildComponent(g, kW[1], 2);
    live::spheres::SphereContributor con;
    con.tracker.plant = plant;
    SceneTables t;
    Counters c{};
    std::vector<std::pair<uint32_t, Exp>> want;
    for (uint32_t f = 500; f < 506; ++f) {
      for (int w = 0; w < 2; ++w) {
        GameUpdate(g, kW[w], f, 1);
        con.tracker.OnComponent(kW[w].comp, f, RW(g), RB(g));
        want.push_back({f * 4 + uint32_t(w), Expected(g, kW[w])});
      }
      FrameInfo fi;
      fi.guest_frame = f + 1;   // the record of present f + 1 closes hook frame f
      con.Emit(fi, t, c);
    }
    bool order = t.sphere_occ.size() == 12;   // each boundary closes the hook frame before it
    for (size_t i = 0; i < t.sphere_occ.size() && order; ++i) order = Same(t.sphere_occ[i], want[i].second) && t.sphere_occ[i].frame == 500 + i / 2 && t.sphere_occ[i].object == (i % 2 ? kSkel1 : kSkel0);
    FrameInfo last;
    last.guest_frame = 507;
    con.Emit(last, t, c);
    order = order && t.sphere_occ.size() == 12;   // nothing new: a frame is never emitted twice
    Check(order, "contrib.live_order", "12 rows in order, each with its hook frame and skeleton; a frame never emitted twice");
    std::string census;
    con.Census(census);
    Check(census.find("recorded 12") != std::string::npos, "contrib.census", census);

    // ---- the record file ----
    const std::vector<uint8_t> a = SerializeRecord(t);
    SceneTables back;
    std::string err;
    const bool parsed = ParseRecord(a.data(), a.size(), back, err);
    Check(parsed && back.sphere_occ.size() == 12 && std::memcmp(back.sphere_occ.data(), t.sphere_occ.data(), 12 * sizeof(SphereOccRow)) == 0 && a == SerializeRecord(back) && std::string(SectionName(kScSphereOcc)) == "SphereOcc" && back.sphere_errors == 0,
          "record.roundtrip", "kScSphereOcc: 12 rows byte-exact, serialise(parse(x)) == x");
    SceneTables bad = t;
    bad.sphere_occ[3].layout = 2;
    const std::vector<uint8_t> ab = SerializeRecord(bad);
    SceneTables bback;
    Check(ParseRecord(ab.data(), ab.size(), bback, err) && bback.sphere_occ.empty() && bback.sphere_errors == 1, "record.bad_layout_drops_spheres", "an unknown layout drops the sphere table only (counted)");
    SceneTables big = t;
    big.sphere_occ[0].count = 3;
    const std::vector<uint8_t> ac = SerializeRecord(big);
    SceneTables cback;
    Check(ParseRecord(ac.data(), ac.size(), cback, err) && cback.sphere_occ.empty() && cback.sphere_errors == 1, "record.bad_count_drops_spheres", "a count past 2 drops the sphere table only (counted)");
    SceneTables old;
    const std::vector<uint8_t> ao = SerializeRecord(old);
    SceneTables oback;
    Check(ParseRecord(ao.data(), ao.size(), oback, err) && oback.sphere_occ.empty() && oback.sphere_errors == 0, "record.older_record_opens", "a record without the section opens, no sphere table");
    if (!write_record.empty()) {
      std::ofstream f(write_record, std::ios::binary);
      f.write(reinterpret_cast<const char*>(a.data()), std::streamsize(a.size()));
    }
  }
  std::printf("%d checks, %d failed%s%s\n", g_checks, g_fail, plant_name.empty() ? "" : " (plant ", plant_name.empty() ? "" : (plant_name + ")").c_str());
  return g_fail ? 1 : 0;
}
