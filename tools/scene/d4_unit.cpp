// Lane D4 unit (Session 103; offline, no game, no renderer): the crowd prop / single person rows through the writer API and the record file.   d4_unit [--plant NAME]
//   1. a record with signs, foam hands and single people written through crowd_rows::AppendProp / AppendSingle round-trips through SerializeRecord / ParseRecord field for field
//      (sections 46 / 47 in id order after 45; a record WITHOUT them serialises byte-identically to one built without the tables: older readers and today's records are unchanged).
//   2. the writer API refuses (and counts, with the first reason) a non-finite world, a collapsed scale, an unknown kind, a palette past the cap, a body asset past the table.
//   3. ConstantsFromWorld / WorldFromConstants are inverse (the c76-c78 form), KeyOf resolves the kind's default (40000 / 41016) and an explicit key wins.
//   4. a record whose section 46 has an unknown layout drops THAT section only (c_errors 1) and keeps the rest.
//   5. Recycle() clears both tables; the expanded copy (ExpandedForReading) keeps them.
// Plants (each must FAIL its named check): world_transposed (the row stores the world's transpose: check 3 / 1's c compare), no_serialize (the writer skips the table: check 1),
// accept_nan (the writer accepts a NaN world: check 2).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <algorithm>
#include "../../src/renderer_scene/crowd_picture_cache.h"
#include "../../src/renderer_scene/crowd_prop_rows.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_crowd.h"
#include "../../src/renderer_scene/rows_pictures.h"
#include "../../src/renderer_scene/rows_stage_fx.h"

using namespace rdr_scene;
namespace cr = rdr_scene::crowd_rows;

static int g_fail = 0;
static void Check(bool ok, const char* what, const std::string& detail = "") {
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : "  -- ", detail.c_str());
  if (!ok) ++g_fail;
}

static Mat4 SomeWorld(float x, float y, float z, float yaw) {
  Mat4 w{};
  const float c = std::cos(yaw), s = std::sin(yaw);
  // row vectors: p' = p * M, translation in 12..14; a turn about Y with a scale of 0.9
  w.m[0] = 0.9f * c, w.m[2] = -0.9f * s, w.m[5] = 0.9f, w.m[8] = 0.9f * s, w.m[10] = 0.9f * c, w.m[12] = x, w.m[13] = y, w.m[14] = z, w.m[15] = 1.f;
  return w;
}

static void Fill(SceneTables& t, const std::string& plant, cr::Counts& counts) {
  for (int i = 0; i < 5; ++i) {
    cr::PropIn p;
    p.kind = (i & 1) ? kCpHand : kCpSign;
    p.model_key = i == 4 ? 40100u : 0u;
    p.variant = uint16_t(i);
    p.flags = (i < 3 ? kCpfFree : kCpfBone) | (i == 2 || i == 4 ? kCpfInPlace : 0u);   // S103 SG2: the in-place flag round-trips with the row
    p.picture = 0x1000u + uint32_t(i);
    cr::ConstantsFromWorld(SomeWorld(100.f + 10.f * float(i), -50.f, 300.f - 7.f * float(i), 0.3f * float(i)), p.c);
    if (plant == "world_transposed") {   // the row stores the 3x3 transposed
      float q[12];
      std::memcpy(q, p.c, sizeof q);
      for (int a = 0; a < 3; ++a)
        for (int b = 0; b < 3; ++b) p.c[4 * a + b] = q[4 * b + a];
    }
    if (plant == "no_serialize") continue;
    cr::AppendProp(t, p, &counts);
  }
  const uint32_t body = cr::BodyAsset(t, 4, 7);
  const uint32_t body2 = cr::BodyAsset(t, 4, 7);
  Check(body == body2 && t.assets.size() == 1, "BodyAsset adds the body once and finds it again");
  std::vector<Mat4> pal(5);
  for (int k = 0; k < 5; ++k) pal[size_t(k)] = SomeWorld(float(k), 1.f, 2.f, 0.1f * float(k));
  for (int i = 0; i < 3; ++i) {
    cr::SingleIn s;
    s.asset = body, s.type = 7, s.flags = i == 2 ? uint32_t(kCsfHidden) : 0u;
    cr::ConstantsFromWorld(SomeWorld(-20.f * float(i), 0.f, 40.f, 1.f + float(i)), s.c);
    s.palette = i == 1 ? nullptr : pal.data(), s.nodes = i == 1 ? 0u : 5u;
    cr::AppendSingle(t, s, &counts);
  }
}

static bool SameProp(const CrowdPropRow& a, const CrowdPropRow& b) { return std::memcmp(&a, &b, sizeof a) == 0; }

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") plant = argv[i + 1];

  SceneTables t;
  cr::Counts counts;
  Fill(t, plant, counts);
  Check(t.crowd_props.size() == (plant == "no_serialize" ? 0u : 5u) && t.crowd_singles.size() == 3 && counts.first_refusal == cr::Result::Ok, "the writer API took 5 props and 3 singles with no refusal", counts.Text());
  // 1. round trip
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables u;
  std::string err;
  const bool parsed = ParseRecord(bytes.data(), bytes.size(), u, err);
  Check(parsed, "ParseRecord reads the written record", err);
  bool same = parsed && u.crowd_props.size() == t.crowd_props.size() && u.crowd_singles.size() == t.crowd_singles.size() && t.crowd_props.size() == 5;
  for (size_t i = 0; same && i < t.crowd_props.size(); ++i) same = SameProp(t.crowd_props[i], u.crowd_props[i]);
  for (size_t i = 0; same && i < t.crowd_singles.size(); ++i) same = std::memcmp(&t.crowd_singles[i], &u.crowd_singles[i], sizeof(CrowdSingleRow)) == 0;
  Check(same, "the prop and single rows round-trip field for field (id order: sections 46 / 47 after the others)");
  {   // S103 D6: kScCrowdPictures (48) round-trips (key, size, format, the block bytes), a bad layout drops that section only, Recycle clears it
    SceneTables pt = t;
    CrowdPictureRow pr;
    pr.key = 0x02A10096u, pr.width = 64, pr.height = 64, pr.format = kCpfBc1, pr.nbytes = 2048;
    for (uint32_t i = 0; i < 2048; ++i) pr.bytes[i] = uint8_t(i * 7 + 3);
    pt.crowd_pictures.push_back(pr);
    const std::vector<uint8_t> pb = SerializeRecord(pt);
    SceneTables pu;
    std::string pe;
    const bool pok = ParseRecord(pb.data(), pb.size(), pu, pe);
    Check(pok && pu.crowd_pictures.size() == 1 && std::memcmp(&pu.crowd_pictures[0], &pr, sizeof pr) == 0 && pu.crowd_props.size() == t.crowd_props.size(), "a picture row round-trips (48) beside the others", pe);
    SceneTables bad = pt;
    bad.crowd_pictures[0].layout = 9;
    const std::vector<uint8_t> bb = SerializeRecord(bad);
    SceneTables bu;
    const bool bok = ParseRecord(bb.data(), bb.size(), bu, pe);
    Check(bok && bu.crowd_pictures.empty() && bu.c_errors == 1 && bu.crowd_props.size() == t.crowd_props.size(), "an unknown picture layout drops that section only");
    pu.Recycle();
    Check(pu.crowd_pictures.empty(), "Recycle clears the picture table");
    {   // review 2: only formats 18 / 19 / 20 and EXACTLY the bytes of the size are accepted (by the parser and by the cache)
      auto refused = [&](CrowdPictureRow r) {
        SceneTables w = t;
        w.crowd_pictures.push_back(r);
        const std::vector<uint8_t> wb = SerializeRecord(w);
        SceneTables wu;
        std::string we;
        return ParseRecord(wb.data(), wb.size(), wu, we) && wu.crowd_pictures.empty() && wu.c_errors == 1;
      };
      CrowdPictureRow shortr = pr;
      shortr.nbytes = 1024;             // 64x64 BC1 needs 2048
      CrowdPictureRow fmt = pr;
      fmt.format = 6;                   // not a block-compressed format
      CrowdPictureRow big = pr;
      big.width = 64, big.height = 128, big.nbytes = 2048;   // the size says 4096
      Check(CrowdPictureValid(pr) && !CrowdPictureValid(shortr) && !CrowdPictureValid(fmt) && !CrowdPictureValid(big), "CrowdPictureValid: exact size and BC1/2/3 only");
      Check(refused(shortr) && refused(fmt) && refused(big), "the parser refuses a picture row with a wrong size or format (section dropped, c_errors 1)");
    }
    {   // reviews 1 / 6: the cache looks every key up again each frame, re-makes an image whose bytes changed, releases what no row names; a bad row is the grey
      struct Fake {
        uint32_t next = 10;
        std::vector<uint32_t> live;
        int created = 0, destroyed = 0, updated = 0;
        uint32_t Grey() { return 1; }
        uint32_t Create(const CrowdPictureRow&) { ++created; live.push_back(next); return next++; }
        void Update(uint32_t, const CrowdPictureRow&) { ++updated; }
        void Destroy(uint32_t id) { ++destroyed; live.erase(std::find(live.begin(), live.end(), id)); }
      };
      live::PictureCache cache;
      if (plant == "picture_cache_stale") cache.plant = 1;
      Fake fk;
      std::vector<CrowdPictureRow> none, one{pr};
      cache.BeginFrame();
      const uint32_t f1 = cache.For(pr.key, none, fk);   // frame 1: no row -> grey
      cache.EndFrame(fk);
      cache.BeginFrame();
      const uint32_t f2 = cache.For(pr.key, one, fk);    // frame 2: the row arrives -> a real image
      cache.EndFrame(fk);
      std::vector<CrowdPictureRow> changed{pr};
      changed[0].bytes[5] ^= 0xFF;
      cache.BeginFrame();
      const uint32_t f3 = cache.For(pr.key, changed, fk);   // frame 3: the bytes changed -> re-made
      cache.EndFrame(fk);
      cache.BeginFrame();
      const uint32_t f4 = cache.For(pr.key, changed, fk);   // frame 4: the same -> kept
      cache.EndFrame(fk);
      cache.BeginFrame();
      cache.EndFrame(fk);                                    // frame 5: nothing names it -> released
      Check(f1 == 1 && f2 >= 10 && f3 >= 10 && f3 != f2 && f4 == f3 && fk.created == 2 && fk.live.empty() && cache.Resident() == 0, "the picture cache: grey, then real, re-made on changed bytes, kept when equal, released when unnamed", "f " + std::to_string(f1) + "/" + std::to_string(f2) + "/" + std::to_string(f3) + "/" + std::to_string(f4));
      CrowdPictureRow badr = pr;
      badr.nbytes = 100;
      std::vector<CrowdPictureRow> badv{badr};
      live::PictureCache c2;
      Fake f2k;
      c2.BeginFrame();
      Check(c2.For(pr.key, badv, f2k) == 1 && f2k.created == 0, "an invalid row never makes an image (grey)");
    }
  }
  {   // the rows hold the placement the reader meant (independent of the writer: the world rebuilt from the row's registers against the world that went in)
    bool placed = parsed && u.crowd_props.size() == 5;
    for (int i = 0; placed && i < 5; ++i) {
      const Mat4 want = SomeWorld(100.f + 10.f * float(i), -50.f, 300.f - 7.f * float(i), 0.3f * float(i));
      const Mat4 got = cr::WorldFromConstants(u.crowd_props[size_t(i)].c);
      for (int k = 0; k < 16; ++k) placed = placed && std::fabs(got.m[k] - want.m[k]) < 1e-5f;
    }
    Check(placed, "each prop row's registers describe the world that was given (not its transpose)");
  }
  Check(parsed && u.bones.size() == t.bones.size() && u.bones.size() == 10 && u.crowd_singles.size() == 3 && u.crowd_singles[0].bones_first == 0 && u.crowd_singles[0].bones_count == 5 && u.crowd_singles[2].bones_first == 5 && u.crowd_singles[1].bones_count == 0, "the palettes ride in kScBones: a single's range is [first, first + count), the stored-pose single has none");
  Check(parsed && u.crowd_singles.size() == 3 && std::string(u.Str(u.crowd_singles[1].id)) == "crowd:single:1", "the single's id string survives");
  Check(parsed && u.c_errors == 0, "no section was dropped");
  // a record without the tables is byte-identical to one that never had them
  SceneTables plain;
  plain.AddString("x");
  const std::vector<uint8_t> a = SerializeRecord(plain);
  SceneTables plain2;
  plain2.AddString("x");
  const std::vector<uint8_t> b = SerializeRecord(plain2);
  Check(a == b && a.size() > 0, "a record without the tables is written as before (no empty section)");
  // 2. refusals
  {
    SceneTables r;
    cr::Counts c;
    cr::PropIn p;
    p.c[0] = std::nanf("");
    cr::PlantAcceptBadWorld() = plant == "accept_nan";
    const cr::Result nan_res = cr::AppendProp(r, p, &c);
    cr::PlantAcceptBadWorld() = false;
    Check(nan_res == cr::Result::BadWorld && r.crowd_props.empty(), "a NaN world is refused and not appended");
    cr::PropIn z;
    std::memset(z.c, 0, sizeof z.c);
    Check(cr::AppendProp(r, z, &c) == cr::Result::BadWorld, "a collapsed (zero-scale) world is refused");
    cr::PropIn k;
    k.kind = 9;
    Check(cr::AppendProp(r, k, &c) == cr::Result::BadKind, "an unknown kind is refused");
    cr::SingleIn s;
    s.asset = 12;
    Check(cr::AppendSingle(r, s, &c) == cr::Result::BadAsset, "a body asset past the table is refused");
    s.asset = cr::BodyAsset(r, 4, 0);
    s.nodes = cr::kMaxPalette + 1;
    Mat4 m{};
    s.palette = &m;
    Check(cr::AppendSingle(r, s, &c) == cr::Result::BadPalette, "a palette past the cap is refused");
    s.nodes = 1;
    m.m[3] = INFINITY;
    Check(cr::AppendSingle(r, s, &c) == cr::Result::BadPalette && r.bones.empty() && r.crowd_singles.empty(), "a palette with a non-finite value is refused and nothing is appended");
    Check(c.props_refused == 3 && c.singles_refused == 3 && c.first_refusal == cr::Result::BadWorld, "the refusals are counted, the first reason kept", c.Text());
  }
  // 3. constants <-> world, keys
  {
    const Mat4 w = SomeWorld(3.f, -4.f, 5.f, 0.7f);
    float c12[12];
    cr::ConstantsFromWorld(w, c12);
    const Mat4 back = cr::WorldFromConstants(c12);
    bool inv = true;
    for (int i = 0; i < 16; ++i) inv = inv && std::fabs(back.m[i] - w.m[i]) < 1e-6f;
    Check(inv, "ConstantsFromWorld / WorldFromConstants are inverse");
    Check(c12[3] == 3.f && c12[7] == -4.f && c12[11] == 5.f, "the translation is the fourth column of each register row (c76.w = x, c77.w = y, c78.w = z)");
    CrowdPropRow r0;
    r0.kind = kCpSign;
    CrowdPropRow r1;
    r1.kind = kCpHand;
    CrowdPropRow r2;
    r2.kind = kCpHand, r2.model_key = 41006;
    Check(cr::KeyOf(r0) == 40000 && cr::KeyOf(r1) == 41016 && cr::KeyOf(r2) == 41006, "KeyOf: the kind's default (40000 / 41016), an explicit key wins");
  }
  // 4. an unknown layout drops that section only
  if (parsed) {
    SceneTables bad = t;
    for (CrowdPropRow& r : bad.crowd_props) r.layout = 9;
    const std::vector<uint8_t> bb = SerializeRecord(bad);
    SceneTables v;
    std::string e2;
    const bool ok = ParseRecord(bb.data(), bb.size(), v, e2);
    Check(ok && v.crowd_props.empty() && v.c_errors == 1 && v.crowd_singles.size() == 3, "an unknown prop layout drops that section only (c_errors 1), the singles stay", e2);
  }
  // 5. Recycle, expanded copy
  if (parsed) {
    const SceneTables ex = ExpandCrowdRuns(u);   // (called directly: ExpandedForReading returns its input when there are no runs)
    Check(ex.crowd_props.size() == u.crowd_props.size() && ex.crowd_singles.size() == u.crowd_singles.size() && u.crowd_props.size() == 5 && std::memcmp(ex.crowd_props.data(), u.crowd_props.data(), 5 * sizeof(CrowdPropRow)) == 0,
          "ExpandCrowdRuns keeps both tables", plant == "no_expand_copy" ? "" : "");
    // the single's id is a string field the compactors must visit (StringFields, CompactStrings)
    SceneTables cs = u;
    cs.AddString("orphan string no field names");
    size_t named = 0;
    for (uint32_t f : cs.StringFields()) named += (f == cs.crowd_singles[0].id || f == cs.crowd_singles[1].id) ? 1u : 0u;
    Check(named >= 2, "StringFields visits CrowdSingleRow::id");
    cs.CompactStrings();
    Check(std::string(cs.Str(cs.crowd_singles[0].id)) == "crowd:single:0" && std::string(cs.Str(cs.crowd_singles[1].id)) == "crowd:single:1" && cs.strings.find("orphan") == std::string::npos,
          "CompactStrings keeps the singles' ids and drops the orphan");
    SceneTables rc = u;
    rc.Recycle();
    Check(rc.crowd_props.empty() && rc.crowd_singles.empty(), "Recycle clears both tables");
  }
  {   // S103 SN: the applier draws a single row only when its palette COVERS the body's used palette slots (row nodes >= slots; 0 slots = unknown: refused); a longer palette draws (the old equality rule refused the stands form)
    Check(cr::SingleNodesMatch(17, 17) && cr::SingleNodesMatch(17, 22) && cr::SingleNodesMatch(17, 80) && !cr::SingleNodesMatch(17, 16) && !cr::SingleNodesMatch(17, 0) && !cr::SingleNodesMatch(0, 17) && !cr::SingleNodesMatch(0, 0), "SingleNodesMatch: the palette must cover the used slots, an unknown slot count refuses");
    SceneTables tl;
    const uint32_t a1 = cr::BodyAsset(tl, 5, 7, 1);
    Check(std::string(tl.Str(tl.assets[a1].entry)) == "20047" && cr::BodyAsset(tl, 5, 7, 0) != a1, "BodyAsset LOD 1 type 7 is entry 20047 (20000 + 40 lod + type), apart from LOD 0");
  }
  std::printf("d4_unit: %s%s%s\n", g_fail ? "FAIL (" : "PASS", g_fail ? std::to_string(g_fail).c_str() : "", g_fail ? " checks)" : "");
  return g_fail ? 1 : 0;
}
