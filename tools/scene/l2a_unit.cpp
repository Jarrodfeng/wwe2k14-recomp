// Lane L2a (Session 103, landing 2 pieces P0 + P1, docs\renderer\LANDING2_PLAN.md): the offline unit proof of the section id check, the 36-site op-19 table, the state fill (blend / alpha test
// from the game's per-thread state object) and the frame rule of the 2D contributor (Drain(j)). 0 game runs, no GPU, no game file.
//
//   l2a_unit                 every check; exit 0 only when all pass
//   l2a_unit --plant NAME    exit 0 only when the check that sees the plant FAILS (plants: src\renderer_scene\quads2d_plants.h behind -DRDR_Q2D_PLANTS)
//
// Checks (tag in brackets; a plant names the tag that must fail):
//   [SECTION IDS]  every scene-record section id constant is unique and inside its lane's range of FRAME_CONTRACT.md 8 (as resolved by L2a: H 59, I 58 + 60-64); kScQuads2D = 58 is lane I's
//   [SITE TABLE]   the 36 op-19 sites: ascending, unique, every one classified (Row / Leaf2D / Particle / Unknown), the seven Row sites = KindOfSite's kinds, SiteOf finds each, 18 functions
//                  (plant site_unclassified)
//   [HOLE SITES]   an op-19 with no scope from a 2D leaf site or a [U] site notes a hole (counted), a particle site and a row site do not
//   [STATE FILL]   a rect row drawn through the tap core with r13 -> [r13] -> +1036 -> a fixture state object: codes 0 / 1 / 5 -> alpha / add / off, blend enable 0 -> off, the alpha test
//                  (code 5 -> GREATER_OR_EQUAL 6, ref 1 -> 1/255), the state byte; +1036 == 0 falls back to +1040; codes 7 / 13 and alpha func 9 stay 'blend unknown' and the drawer refuses them
//                  (plants state_ignored, state_wrong_slot, code_any)
//   [PAIR SCAN]    ScanStream (review F1 / F2 / pairing): row-site draws on the frame image, no kept op between them, count = rows, row i blend = i-th draw blend; 4 refusal reasons each with a planted twin
//   [FRAME RULE]   TakeFrame(j): the rows of frame j only (a late j-1 row and a j+1 row are not drawn in record j; the late one is a hole), the pair census 0 / -1 / +1 (plant drain_prev)
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../src/renderer_scene/particle_tables.h"
#include "../../src/renderer_scene/quads2d_core.h"
#include "sc_support.h"   // S103 SC
#include "../../src/renderer_scene/quads2d_draw.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_crowd.h"
#include "../../src/renderer_scene/rows_liquid.h"
#include "../../src/renderer_scene/rows_part_clock.h"
#include "../../src/renderer_scene/rows_pictures.h"
#include "../../src/renderer_scene/rows_plane.h"
#include "../../src/renderer_scene/rows_quad2d.h"
#include "../../src/renderer_scene/rows_stage_fx.h"
#include "../../src/renderer_scene/rows_stage_look.h"

using namespace rdr_scene;

static std::string g_failed;
static int g_checks = 0, g_failures = 0;
static void Check(bool ok, const char* tag, const std::string& what) {
  ++g_checks;
  std::printf("%s [%s] %s\n", ok ? "PASS" : "FAIL", tag, what.c_str());
  if (!ok) {
    ++g_failures;
    if (g_failed.find(tag) == std::string::npos) g_failed += std::string(g_failed.empty() ? "" : ", ") + "[" + tag + "]";
  }
}

struct FakeMem {
  std::map<uint32_t, uint32_t> words;
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  void PutF(uint32_t a, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    Put(a, v);
  }
  bool ReadWords(uint32_t a, uint32_t* out, uint32_t n) const {
    if (a < 0x10000u || uint64_t(a) + 4ull * n > 0x7F000000ull) return false;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = words.find(a + 4 * i);
      out[i] = it == words.end() ? 0u : it->second;
    }
    return true;
  }
};

// ---- [SECTION IDS] ----
static void SectionIds() {
  struct Id { const char* name; uint32_t id; };
  const Id ids[] = {
      {"kScStrings", kScStrings}, {"kScCamera", kScCamera}, {"kScAssets", kScAssets}, {"kScObjects", kScObjects}, {"kScBones", kScBones}, {"kScParams", kScParams}, {"kScLights", kScLights},
      {"kScSprites", kScSprites}, {"kScTexts", kScTexts}, {"kScMovies", kScMovies}, {"kScTargets", kScTargets}, {"kScEngineObjects", kScEngineObjects}, {"kScLightStates", kScLightStates},
      {"kScLightRefs", kScLightRefs}, {"kScArenaWorlds", kScArenaWorlds}, {"kScCircleShadows", kScCircleShadows}, {"kScCircleMesh", kScCircleMesh}, {"kScPlaneFrames", kScPlaneFrames},
      {"kScPlaneDraws", kScPlaneDraws}, {"kScRingRows", kScRingRows}, {"kScRingNodes", kScRingNodes}, {"kScCasterCalls", kScCasterCalls}, {"kScCasterStates", kScCasterStates},
      {"kScCasterNodes", kScCasterNodes}, {"kScCasterExtras", kScCasterExtras}, {"kScCasterMats", kScCasterMats}, {"kScSphereOcc", kScSphereOcc}, {"kScOwnedPasses", kScOwnedPasses},
      {"kScPostParams", kScPostParams}, {"kScModelRuntime", kScModelRuntime}, {"kScInputStamps", kScInputStamps}, {"kScLightObjects", kScLightObjects}, {"kScParamOverrides", kScParamOverrides},
      {"kScModelLights", kScModelLights}, {"kScStageFx", kScStageFx}, {"kScCrowdRuns", kScCrowdRuns}, {"kScCrowdMembers", kScCrowdMembers}, {"kScStageLook", kScStageLook},
      {"kScCrowdProps", kScCrowdProps}, {"kScCrowdSingles", kScCrowdSingles}, {"kScCrowdPictures", kScCrowdPictures}, {"kScPlaneEyes", kScPlaneEyes}, {"kScPartClocks", kScPartClocks},
      {"kScLiquidMaps", kScLiquidMaps}, {"kScLiquidItems", kScLiquidItems}, {"kScLiquidPictures", kScLiquidPictures},   // S103 SWF (lane E's range 49-53)
      {"kScParticleBatches", kScParticleBatches}, {"kScParticles", kScParticles}, {"kScParticleStatus", kScParticleStatus}, {"kScParticleScreen", kScParticleScreen}, {"kScQuads2D", kScQuads2D},
  };
  // FRAME_CONTRACT.md 8 (S103 L2a): owner of each id. Before S103: 1-28 (the enum), 40-41 (S102 T19). S103: A 29-34, B 35-37, C 38-39 + 42-45, D 46-48, E 49-53, F 54-57, H 59, I 58 + 60-64.
  const auto owner = [](uint32_t id) -> std::string {
    if (id >= 1 && id <= 28) return "pre-S103";
    if (id == 40 || id == 41) return "T19";
    if (id >= 29 && id <= 34) return "A";
    if (id >= 35 && id <= 37) return "B";
    if ((id >= 38 && id <= 39) || (id >= 42 && id <= 45)) return "C";
    if (id >= 46 && id <= 48) return "D";
    if (id >= 49 && id <= 53) return "E";
    if (id >= 54 && id <= 57) return "F";
    if (id == 59) return "H";
    if (id == 58 || (id >= 60 && id <= 64)) return "I";
    return "";
  };
  std::map<uint32_t, std::string> seen;
  bool unique = true, owned = true;
  std::string why;
  for (const Id& i : ids) {
    if (seen.count(i.id)) unique = false, why += std::string(i.name) + " = " + std::to_string(i.id) + " repeats " + seen[i.id] + "; ";
    seen[i.id] = i.name;
    if (owner(i.id).empty()) owned = false, why += std::string(i.name) + " = " + std::to_string(i.id) + " is in no lane's range; ";
  }
  Check(unique && owned, "SECTION IDS", std::to_string(sizeof(ids) / sizeof(ids[0])) + " section id constants: unique, each inside a lane's range" + (why.empty() ? "" : " (" + why + ")"));
  Check(kScQuads2D == 58 && owner(kScQuads2D) == "I" && owner(59) == "H", "SECTION IDS", "kScQuads2D = 58 belongs to lane I (the resolved table: H 59, I 58 + 60-64)");
}

// ---- [SITE TABLE] / [HOLE SITES] ----
static void SiteTable() {
  using q2d::SiteClass;
  bool ascending = true, classified = true, found = true;
  std::set<uint32_t> fns;
  int rows = 0, leaf = 0, part = 0, unk = 0;
  bool row_kinds = true;
  for (size_t i = 0; i < q2d::kOp19SiteCount; ++i) {
    const q2d::Op19Site& s = q2d::kOp19Sites[i];
    if (i && q2d::kOp19Sites[i - 1].site >= s.site) ascending = false;
    const SiteClass c = q2d::ClassOfSite(s.site);
    if (c == SiteClass::None || c != s.cls) classified = false;
    if (q2d::SiteOf(s.site) != &s) found = false;
    fns.insert(s.fn);
    rows += s.cls == SiteClass::Row, leaf += s.cls == SiteClass::Leaf2D, part += s.cls == SiteClass::Particle, unk += s.cls == SiteClass::Unknown;
    if ((s.cls == SiteClass::Row) != (q2d::KindOfSite(s.site) != kQkNone) || (s.cls == SiteClass::Row && s.kind != q2d::KindOfSite(s.site))) row_kinds = false;
  }
  Check(q2d::kOp19SiteCount == 36 && fns.size() == 18 && ascending && found, "SITE TABLE", "36 sites in 18 functions, ascending, each found by SiteOf");
  Check(classified, "SITE TABLE", "every site classifies to its table class (none unclassified)");
  Check(rows == 8 && leaf == 11 && part == 3 && unk == 14 && row_kinds, "SITE TABLE",
        "rows 8 (= KindOfSite's kinds; S103 P3b: + 0x82BC484C, 0x82BC8BEC; S103 SVY: + 0x82BC799C; S105 A8: + 0x82BC4404), 2D leaves not recorded 11, particles 3, [U] 14: got " + std::to_string(rows) + " / " + std::to_string(leaf) + " / " + std::to_string(part) + " / " + std::to_string(unk));
  bool scope_kinds = true;   // S103 P3c review F2: the scope kind each Row site's leaf opens: its own kind, except 0x82BC8BEC (sub_82BC8180: the glyph-kind scope)
  for (size_t i = 0; i < q2d::kOp19SiteCount; ++i) {
    const q2d::Op19Site& s = q2d::kOp19Sites[i];
    if (s.cls == SiteClass::Row) scope_kinds = scope_kinds && q2d::ScopeKindOfSite(s.site) == (s.site == 0x82BC8BECu ? uint8_t(kQkGlyph) : s.kind);
  }
  Check(scope_kinds, "SITE TABLE", "every Row site has the scope kind its leaf opens (0x82BC8BEC: glyph-kind; the others their own kind)");
  Check(q2d::ClassOfSite(0x82BC3EE4u) == SiteClass::None && q2d::SiteOf(0x82C1B350u) == nullptr, "SITE TABLE", "an address that is not a call site classifies to none");

  // Holes from sites with no scope open
  FakeMem m;
  q2d::Store store;
  q2d::Counters c;
  q2d::Scope s;
  const auto unscoped = [&](uint32_t site) { q2d::OnOp19(s, m, store, 7, site, 13, 4, 20, 0x40002000u, c); };
  unscoped(0x82BC4404u);   // T_RectColor (S105 A8: a Row site now: with no scope open it is a hole like every Row site, review F3)
  unscoped(0x82756E98u);   // sub_827568E0 [U]
  unscoped(0x82A0DA58u);   // particles: lane F's
  unscoped(0x82BC3EE8u);   // a row site with no scope: a hole too (review F3: it makes no row)
  std::vector<q2d::Store::Hole> holes;
  store.DrainHoles(7, holes);
  bool want = holes.size() == 3 && c.hole_unscoped.load() == 3 && c.unknown_site.load() == 1;
  for (const auto& h : holes) want = want && (h.site == 0x82BC4404u || h.site == 0x82756E98u || h.site == 0x82BC3EE8u) && h.count == 1 && h.frame == 7;
  Check(want, "HOLE SITES", "no scope: a 2D leaf site and a [U] site and a ROW site each note a hole (the [U] one counted as unknown_site); a particle site notes none");
}

// ---- [STATE FILL] ----
static const uint32_t kR13 = 0x50000000u, kBlock = 0x50001000u, kObj = 0x50002000u, kDecoy = 0x50003000u, kVerts = 0x40002000u, kPayload = 0x40003000u, kObject = 0x40001000u;
struct StateFix {
  uint32_t blend_enable = 1, alpha_test = 1, code = 0, func = 5, ref = 1, z = 0, zw = 0;
};
static void PutState(FakeMem& m, uint32_t obj, const StateFix& f) {
  m.Put(obj + 8, f.blend_enable), m.Put(obj + 12, f.alpha_test), m.Put(obj + 16, f.code), m.Put(obj + 20, f.func), m.Put(obj + 24, f.ref), m.Put(obj + 80, f.z), m.Put(obj + 84, f.zw);
}
// One rect drawn through the tap core with the state object at [[r13] + slot]; the drained row.
static Quad2D RectWithState(const StateFix& f, bool primary_slot_empty = false) {
  FakeMem m;
  m.Put(kR13, kBlock);
  m.Put(kBlock + 1036, primary_slot_empty ? 0u : kObj);
  m.Put(kBlock + 1040, primary_slot_empty ? kObj : kDecoy);
  PutState(m, kObj, f);
  StateFix decoy;   // the other slot holds a different state: reading the wrong slot shows
  decoy.code = 12, decoy.func = 0, decoy.ref = 200, decoy.blend_enable = 1, decoy.alpha_test = 1;
  PutState(m, kDecoy, decoy);
  const float v[4][3] = {{100, 50, 0}, {300, 50, 0}, {300, 150, 0}, {100, 150, 0}};
  for (int i = 0; i < 4; ++i)
    for (int k = 0; k < 3; ++k) m.PutF(kVerts + 12 * i + 4 * k, v[i][k]);
  const float c0[4] = {2.0f / 1280, 2.0f / 720, 0, 0}, c1[4] = {1, 1, 1, 0.5f};
  for (int k = 0; k < 4; ++k) m.PutF(kPayload + 4 * k, c0[k]), m.PutF(kPayload + 16 + 4 * k, c1[k]);
  m.PutF(kPayload + 0x200, 0.0f);   // c28 (fUseProjMatrix = 0)
  q2d::Store store;
  q2d::Counters c;
  q2d::Scope s;
  q2d::OpenScope(s, kQkRect, kObject);
  q2d::OnOp21(s, m, 0, 2, kPayload, c);
  q2d::OnOp21(s, m, 28, 1, kPayload + 0x200, c);
  q2d::OnOp19(s, m, store, 9, q2d::kSiteRect, 4, 4, 12, kVerts, c, nullptr, kR13);
  q2d::CloseScope(s, store, 9, c);
  std::vector<Quad2D> rows;
  store.Drain(9, rows);
  return rows.size() == 1 ? rows[0] : Quad2D{};
}
static q2d::Refuse DrawCheck(const Quad2D& row) {
  q2d::Context ctx;
  ctx.width = 1280, ctx.height = 720;
  const q2d::Report r = q2d::Check(ctx, &row, 1);
  if (r.drawn == 1) return q2d::Refuse::None;
  for (uint8_t i = 1; i < uint8_t(q2d::Refuse::Count); ++i)
    if (r.refused[i]) return q2d::Refuse(i);
  return q2d::Refuse::Count;
}
static void StateFill() {
  StateFix a;   // code 0, alpha test GEQUAL (code 5) ref 1
  const Quad2D r0 = RectWithState(a);
  Check((r0.have & (kQhState | kQhBlend | kQhAlphaTest)) == (kQhState | kQhBlend | kQhAlphaTest) && r0.blend == kQbAlpha && r0.alpha_func == 6 && r0.alpha_ref == 1.0f / 255.0f &&
            r0.state == (0 | 64 | 128) && DrawCheck(r0) == q2d::Refuse::None,
        "STATE FILL", "code 0 -> SRC_ALPHA / INV_SRC_ALPHA (alpha), alpha func code 5 -> GREATER_OR_EQUAL (6), ref 1 -> 1/255, state byte = code | blend on | test on; the drawer draws it");
  StateFix b = a;
  b.code = 1, b.alpha_test = 0, b.z = 1;
  const Quad2D r1 = RectWithState(b);
  Check((r1.have & kQhBlend) && r1.blend == kQbAdd && r1.alpha_func == 7 && r1.alpha_ref == 0.0f && r1.state == (1 | 16 | 64) && DrawCheck(r1) == q2d::Refuse::None, "STATE FILL",
        "code 1 -> SRC_ALPHA / ONE (add); alpha test off -> ALWAYS (7), ref 0; ZENABLE in the state byte");
  StateFix c = a;
  c.code = 5;
  const Quad2D r5 = RectWithState(c);
  Check((r5.have & kQhBlend) && r5.blend == kQbOff && DrawCheck(r5) == q2d::Refuse::None, "STATE FILL", "code 5 -> ONE / ZERO (the source replaces: blend off)");
  StateFix d = a;
  d.blend_enable = 0, d.code = 7;
  const Quad2D roff = RectWithState(d);
  Check((roff.have & kQhBlend) && roff.blend == kQbOff && (roff.state & 64) == 0, "STATE FILL", "ALPHABLENDENABLE 0 -> off, whatever the code");
  const Quad2D rslot = RectWithState(a, /*primary_slot_empty=*/true);
  Check((rslot.have & kQhBlend) && rslot.blend == kQbAlpha && rslot.alpha_func == 6, "STATE FILL", "[[r13] + 1036] == 0: the state object is [[r13] + 1040] (sub_82BA7238)");
  StateFix e = a;
  e.code = 7;
  const Quad2D r7 = RectWithState(e);
  Check((r7.have & kQhState) && !(r7.have & kQhBlend) && r7.blend == kQbUnknown && (r7.state & 15) == 7 && DrawCheck(r7) == q2d::Refuse::Blend, "STATE FILL",
        "code 7 (records nothing, R8) stays 'blend unknown': the drawer refuses it");
  StateFix e2 = a;
  e2.code = 13;
  const Quad2D r13 = RectWithState(e2);
  Check(!(r13.have & kQhBlend) && (r13.state & 15) == q2d::kStateCodeOutside && DrawCheck(r13) == q2d::Refuse::Blend, "STATE FILL", "code 13 (outside the table) -> state code 15, refused");
  StateFix f = a;
  f.func = 9;
  const Quad2D rf = RectWithState(f);
  Check(!(rf.have & kQhBlend) && DrawCheck(rf) == q2d::Refuse::Blend, "STATE FILL", "an alpha function code outside 0..7 leaves the row 'blend unknown' (never drawn with a guessed test)");
  StateFix g = a;
  g.code = 3;
  const Quad2D r3 = RectWithState(g);
  Check((r3.have & kQhBlend) && r3.blend == kQbOther && DrawCheck(r3) == q2d::Refuse::Blend, "STATE FILL", "code 3 (ZERO / SRC_COLOR: known, no drawer program) -> other, refused by reason");
}

// ---- [FRAME RULE] ----
static Quad2D Row(uint32_t frame, uint32_t seq) {
  Quad2D q;
  q.kind = kQkRect, q.site = q2d::kSiteRect, q.frame = frame, q.seq = seq, q.nverts = 4, q.stride_bytes = 12;
  q.have = kQhVerts | kQhPosScale | kQhColour | kQhProj;
  q.pos_scale[0] = 2.0f / 1280, q.pos_scale[1] = 2.0f / 720;
  q.color[0] = q.color[1] = q.color[2] = q.color[3] = 1.0f;
  ValidSC(q);
  return q;
}
static void FrameRule() {
  q2d::Store store;
  q2d::Counters c;
  const uint32_t j = 100;
  store.Push(Row(j - 1, 1), c);   // a draw of frame j - 1 that arrived after record j - 1's latch
  store.Push(Row(j, 2), c), store.Push(Row(j, 3), c), store.Push(Row(j, 4), c);
  store.Push(Row(j + 1, 5), c);   // the next frame's draw (the update thread released early): never record j's
  q2d::FrameTake take;
  q2d::TakeFrame(store, j, [](const Quad2D& q) { return Quad2DRowProblem(q).empty(); }, take);
  bool only_j = take.rows.size() == 3;
  for (const Quad2D& q : take.rows) only_j = only_j && q.frame == j;
  Check(only_j && take.rows.size() == 3 && take.rows[0].seq == 2 && take.rows[2].seq == 4, "FRAME RULE", "record j takes the rows of frame j (Drain(j)), in call order");
  Check(take.pairs.same == 3 && take.pairs.prev == 1 && take.pairs.next == 1 && take.pairs.other == 0 && take.hole_count == 1, "FRAME RULE",
        "pair census 0 = 3, -1 = 1, +1 = 1 (left for record j + 1); the late j - 1 row is dropped and counted as a hole");
  q2d::FrameTake next;
  q2d::TakeFrame(store, j + 1, [](const Quad2D& q) { return Quad2DRowProblem(q).empty(); }, next);
  Check(next.rows.size() == 1 && next.rows[0].frame == j + 1 && next.hole_count == 0, "FRAME RULE", "record j + 1 takes its own row");
  // A bad row is dropped and counted as a hole
  q2d::Store s2;
  Quad2D bad = Row(j, 1);
  bad.verts[0][0] = NAN;
  s2.Push(bad, c), s2.Push(Row(j, 2), c);
  q2d::FrameTake t2;
  q2d::TakeFrame(s2, j, [](const Quad2D& q) { return Quad2DRowProblem(q).empty(); }, t2);
  Check(t2.rows.size() == 1 && t2.hole_count == 1, "FRAME RULE", "a row that fails its own check is dropped and counted as a hole (that frame keeps the stream's 2D)");
}

// ---- [PAIR SCAN] ----
static q2d::StreamOp Draw3D(uint32_t target) { q2d::StreamOp o; o.kind = 0, o.target = target; return o; }
static q2d::StreamOp RowDraw(uint32_t target, uint8_t blend) { q2d::StreamOp o; o.kind = 0, o.op19 = true, o.site = q2d::kSiteRect, o.target = target, o.blend = blend; return o; }
static q2d::StreamOp ParticleDraw(uint32_t target) { q2d::StreamOp o; o.kind = 0, o.op19 = true, o.site = 0x82A0DA58u, o.target = target; return o; }
static q2d::StreamOp CopyOp() { q2d::StreamOp o; o.kind = 1; return o; }
static Quad2D BlendRow(uint8_t blend) { Quad2D q = Row(1, 1); q.have |= kQhBlend, q.blend = blend; return q; }
static void PairScanChecks() {
  const uint32_t F = 8;
  const Quad2D rows2[2] = {BlendRow(kQbAlpha), BlendRow(kQbAdd)};
  const auto scan = [&](std::vector<q2d::StreamOp> ops, const Quad2D* rows, size_t n) { return q2d::ScanStream(ops.data(), ops.size(), F, rows, n); };
  const q2d::PairScan ok = scan({Draw3D(F), Draw3D(F), RowDraw(F, kQbAlpha), RowDraw(F, kQbAdd), ParticleDraw(F), CopyOp()}, rows2, 2);
  Check(ok.why == q2d::PairWhy::None && ok.row_draws == 2, "PAIR SCAN", "kept draws before the first and after the last row-site draw (a particle site, a copy after): the frame pairs");
  const q2d::PairScan t = scan({RowDraw(F, kQbAlpha), RowDraw(99, kQbAdd)}, rows2, 2);
  Check(t.why == q2d::PairWhy::Target, "PAIR SCAN", "a row-site draw whose colour target is not the frame image is refused: stream_target");
  const q2d::PairScan i = scan({RowDraw(F, kQbAlpha), ParticleDraw(F), RowDraw(F, kQbAdd)}, rows2, 2);
  Check(i.why == q2d::PairWhy::Interleaved, "PAIR SCAN", "a kept draw between two row-site draws is refused: interleaved");
  const q2d::PairScan i2 = scan({RowDraw(F, kQbAlpha), CopyOp(), RowDraw(F, kQbAdd)}, rows2, 2);
  Check(i2.why == q2d::PairWhy::Interleaved, "PAIR SCAN", "a copy between two row-site draws is refused too: interleaved");
  const q2d::PairScan c = scan({RowDraw(F, kQbAlpha), RowDraw(F, kQbAdd), RowDraw(F, kQbAdd)}, rows2, 2);
  Check(c.why == q2d::PairWhy::Count && c.row_draws == 3, "PAIR SCAN", "three row-site draws against two rows is refused: count");
  const q2d::PairScan b = scan({RowDraw(F, kQbAlpha), RowDraw(F, kQbAlpha)}, rows2, 2);
  Check(b.why == q2d::PairWhy::Blend, "PAIR SCAN", "row 1 is additive but the 2nd row-site draw is alpha (a frame off by one looks like this): blend_pair");
}

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") {
      const std::string p = argv[i + 1];
      const std::map<std::string, int> plants = {{"drain_prev", 20}, {"state_ignored", 21}, {"state_wrong_slot", 22}, {"code_any", 23}, {"site_unclassified", 24}, {"row_unscoped_ok", 25}, {"pair_target_ignored", 26}, {"pair_interleave_ignored", 27}, {"pair_count_ignored", 28}, {"pair_blend_ignored", 29}};
      auto it = plants.find(p);
      if (it == plants.end()) return std::printf("unknown plant %s\n", p.c_str()), 2;
      q2d::g_plant = it->second;
      const std::map<std::string, std::string> expect = {{"drain_prev", "[FRAME RULE]"}, {"state_ignored", "[STATE FILL]"}, {"state_wrong_slot", "[STATE FILL]"}, {"code_any", "[STATE FILL]"}, {"site_unclassified", "[SITE TABLE]"}, {"row_unscoped_ok", "[HOLE SITES]"}, {"pair_target_ignored", "[PAIR SCAN]"}, {"pair_interleave_ignored", "[PAIR SCAN]"}, {"pair_count_ignored", "[PAIR SCAN]"}, {"pair_blend_ignored", "[PAIR SCAN]"}};
      SectionIds(), SiteTable(), StateFill(), FrameRule(), PairScanChecks();
      const bool caught = g_failed.find(expect.at(p)) != std::string::npos;
      std::printf("plant %s: %s (failed: %s)\n", p.c_str(), caught ? "CAUGHT" : "NOT CAUGHT", g_failed.empty() ? "-" : g_failed.c_str());
      return caught ? 0 : 1;
    }
  SectionIds(), SiteTable(), StateFill(), FrameRule(), PairScanChecks();
  std::printf("%d checks, %d failed%s\n", g_checks, g_failures, g_failed.empty() ? "" : (" " + g_failed).c_str());
  return g_failures ? 1 : 0;
}
