// Lane I1 (Session 103, landing 2, docs\renderer\REMNANT_MAP.md section 10): the offline unit proof of the 2D layer's record. 0 game runs, no GPU, no game file.
//
//   i1_unit                 every check; exit 0 only when all pass
//   i1_unit --plant NAME    exit 0 only when the check that sees the plant FAILS (the plants are in the production headers behind -DRDR_Q2D_PLANTS: src\renderer_scene\quads2d_plants.h)
//
// Checks (tag in brackets; a plant names the tag that must fail):
//   [RECORD]          section 58 round trip through SerializeRecord / ParseRecord, equal bytes; the name; an empty table writes no section; a record without it parses as before
//   [RECORD BAD]      a bad row (layout, kind, pass, non-finite, vertices, element size) drops THIS section only (quad2d_errors), the rest of the record is read
//   [RECORD SEQ]      a section whose tickets do not ascend is refused (plant parse_seq)
//   [TAP VERTICES]    the op-19 tap reads the four vertices exactly (a fixture guest memory, big-endian), the right stride per kind; wrong shape / site / stride are counted and make no row (plant tap_vertex_order)
//   [TAP CONSTANTS]   the op-21 tap reads c0 / c1 / c28, in any upload shape (one run, three uploads), before or after the vertices (plant tap_no_colour)
//   [TAP ORDER]       the call-order ticket ascends, Drain returns the rows of frames <= N in seq order across two threads' stores (plants tap_ticket_reuse, drain_unsorted)
//   [TAP FLUSH]       a movie row carries the plane keys (object +400/408/416/424), the flush count and set of the last flush (plant tap_no_flush)
//   [BLOCKS EXACT]    BlocksOf on a known fade and a known movie quad equals an independent transcription of the programs' layouts (plant blocks_no_half); refusals by reason
//   [DRAW STATE]      Append: program, blend factors, alpha test, viewport, texture slots, constants bytes of each draw (plant blocks_blend); refused rows are counted by reason
//   [DRAW ORDER]      Append keeps the rows' order (plant draw_reversed)
//   [RIG]             the offline rig's Make: clean rows pass the state cross-check; a wrong blend raises the mismatch counter; drop plants give a quad of no area; swap_vertices moves the block
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/quads2d_core.h"
#include "sc_support.h"   // S103 SC
#include "../../src/renderer_scene/quads2d_draw.h"
#include "../../src/renderer_scene/quads2d_rig.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_quad2d.h"

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

// ---- a fixture guest memory: big-endian words ---------------------------------------------------------------------------------------------------
struct FakeMem {
  std::map<uint32_t, uint32_t> words;
  void Put(uint32_t a, uint32_t v) { words[a] = v; }
  void PutF(uint32_t a, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    Put(a, v);
  }
  // the live GuestMem's rule (wrestler_hook.h ReadGuest): below 0x10000 and the MMIO heap (0x7F000000-0x7FFFFFFF) are refused
  bool ReadWords(uint32_t a, uint32_t* out, uint32_t n) const {
    if (a < 0x10000u || uint64_t(a) + 4ull * n > 0x7F000000ull) return false;
    for (uint32_t i = 0; i < n; ++i) {
      auto it = words.find(a + 4 * i);
      out[i] = it == words.end() ? 0u : it->second;
    }
    return true;
  }
};

static const float kScaleX = 2.0f / 1280.0f, kScaleY = 2.0f / 720.0f;

static void PutRect(FakeMem& m, uint32_t at, float x0, float y0, float x1, float y1, float z = 0.25f) {
  const float v[4][3] = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y1, z}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 3; ++c) m.PutF(at + 12 * i + 4 * c, v[i][c]);
}
static void PutMovie(FakeMem& m, uint32_t at, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float z = 0.5f) {
  const float v[4][5] = {{x0, y0, z, u0, v0}, {x1, y0, z, u1, v0}, {x1, y1, z, u1, v1}, {x0, y1, z, u0, v1}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) m.PutF(at + 20 * i + 4 * c, v[i][c]);
}
static void PutRegs(FakeMem& m, uint32_t at, std::initializer_list<std::initializer_list<float>> rows) {
  uint32_t a = at;
  for (auto row : rows) {
    for (float f : row) m.PutF(a, f), a += 4;
  }
}

// ---- helpers ---------------------------------------------------------------------------------------------------------------------------------
static Quad2D GoodRect(uint32_t seq) {
  Quad2D q;
  q.kind = kQkRect, q.site = q2d::kSiteRect, q.seq = seq, q.frame = 7, q.nverts = 4, q.stride_bytes = 12;
  q.have = kQhVerts | kQhPosScale | kQhColour | kQhProj | kQhBlend | kQhAlphaTest;
  q.blend = kQbAlpha;
  const float v[4][3] = {{0, 0, 0.25f}, {1280, 0, 0.25f}, {1280, 720, 0.25f}, {0, 720, 0.25f}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 3; ++c) q.verts[i][c] = v[i][c];
  q.pos_scale[0] = kScaleX, q.pos_scale[1] = kScaleY, q.pos_scale[2] = 1.f, q.pos_scale[3] = 1.f;
  q.color[0] = 0.f, q.color[1] = 0.f, q.color[2] = 0.f, q.color[3] = 0.5f;
  ValidSC(q);
  return q;
}
static Quad2D GoodMovie(uint32_t seq) {
  Quad2D q;
  q.kind = kQkYuvAlpha, q.site = q2d::kSiteMovie, q.seq = seq, q.frame = 7, q.nverts = 4, q.stride_bytes = 20;
  q.have = kQhVerts | kQhPosScale | kQhColour | kQhProj | kQhBlend | kQhAlphaTest | kQhPlanes;
  q.blend = kQbAlpha;
  const float x0 = 160, y0 = 600, x1 = 1120, y1 = 680;
  const float v[4][5] = {{x0, y0, 0.5f, 0, 0}, {x1, y0, 0.5f, 1, 0}, {x1, y1, 0.5f, 1, 1}, {x0, y1, 0.5f, 0, 1}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) q.verts[i][c] = v[i][c];
  q.pos_scale[0] = kScaleX, q.pos_scale[1] = kScaleY;
  q.color[0] = q.color[1] = q.color[2] = q.color[3] = 1.f;
  for (int s = 0; s < 4; ++s) q.tex_image[s] = 100 + s;
  ValidSC(q);
  return q;
}
static bool Same(const Quad2D& a, const Quad2D& b) { return std::memcmp(&a, &b, sizeof(Quad2D)) == 0; }

// ---- [RECORD] ----------------------------------------------------------------------------------------------------------------------------------
static void RecordChecks() {
  SceneTables t;
  t.has_camera = true;
  t.quads2d = {GoodRect(1), GoodMovie(2), GoodRect(3)};
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables back;
  std::string err;
  const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
  Check(ok && back.quads2d.size() == 3 && Same(back.quads2d[0], t.quads2d[0]) && Same(back.quads2d[1], t.quads2d[1]) && Same(back.quads2d[2], t.quads2d[2]) && back.quad2d_errors == 0,
        "RECORD", "three rows written to section 58 and parsed back byte-equal (" + err + ")");
  Check(std::string(SectionName(kScQuads2D)) == "Quad2D" && kScQuads2D == 58 && sizeof(Quad2D) == 324, "RECORD", "section id 58, name Quad2D, 324 bytes a row"  /* S103 I2: layout 2 (+ hsl); S103 P3c: layout 3 (+ proj[16]) */);
  SceneTables none;
  const std::vector<uint8_t> plain = SerializeRecord(none);
  SceneTables back2;
  Check(ParseRecord(plain.data(), plain.size(), back2, err) && back2.quads2d.empty() && bytes.size() > plain.size() && plain.size() == SerializeRecord(SceneTables()).size(), "RECORD",
        "an empty table writes no section: a record without 2D rows is written and read as before");

  // a bad row drops the section only
  struct Bad { const char* name; void (*mutate)(Quad2D&); };
  const Bad bads[] = {
      {"layout version 9", [](Quad2D& q) { q.version = 9; }},
      {"kind rect-gradation (not this reader's)", [](Quad2D& q) { q.kind = kQkRectGrad; }},
      {"pass out of range", [](Quad2D& q) { q.pass = 77; }},
      {"blend out of range", [](Quad2D& q) { q.blend = 99; }},
      {"alpha function 9", [](Quad2D& q) { q.alpha_func = 9; }},
      {"a NaN vertex", [](Quad2D& q) { q.verts[2][1] = std::nanf(""); }},
      {"an infinite colour", [](Quad2D& q) { q.color[3] = INFINITY; }},
      {"no vertices read", [](Quad2D& q) { q.have = 0; }},
      {"three vertices", [](Quad2D& q) { q.nverts = 3; }},
  };
  for (const Bad& b : bads) {
    SceneTables bt;
    bt.has_camera = true;
    bt.quads2d = {GoodRect(1), GoodRect(2)};
    b.mutate(bt.quads2d[1]);
    const std::vector<uint8_t> by = SerializeRecord(bt);
    SceneTables out;
    std::string e;
    const bool parsed = ParseRecord(by.data(), by.size(), out, e);
    Check(parsed && out.quads2d.size() == 1 && out.quads2d[0].seq == 1 && out.quad2d_errors == 1 && out.has_camera, "RECORD BAD", std::string("a row with ") + b.name + ": that row alone is refused (counted), the good row and the record are read");
  }
  {  // the section's element size is wrong: a table of 100-byte elements under id 58
    SceneTables bt;
    bt.has_camera = true;
    bt.quads2d = {GoodRect(1)};
    std::vector<uint8_t> by = SerializeRecord(bt);
    // find the section entry for 58 and shrink its element size field by patching: the directory entry layout is the record's own (id, element size, count, offset); locate by scanning for the id followed by 196
    bool patched = false;
    for (size_t i = 0; i + 8 <= by.size() && !patched; i += 4) {
      uint32_t id, es;
      std::memcpy(&id, &by[i], 4), std::memcpy(&es, &by[i + 4], 4);
      if (id == kScQuads2D && es == sizeof(Quad2D)) es = 100, std::memcpy(&by[i + 4], &es, 4), patched = true;
    }
    SceneTables out;
    std::string e;
    Check(patched && ParseRecord(by.data(), by.size(), out, e) && out.quads2d.empty() && out.quad2d_errors == 1 && out.has_camera, "RECORD BAD", "a wrong element size drops the 2D section only");
  }
  {  // [RECORD SEQ]
    SceneTables bt;
    bt.quads2d = {GoodRect(5), GoodRect(5), GoodRect(4)};
    const std::vector<uint8_t> by = SerializeRecord(bt);
    SceneTables out;
    std::string e;
    ParseRecord(by.data(), by.size(), out, e);
    Check(out.quads2d.empty() && out.quad2d_errors == 1, "RECORD SEQ", "tickets that do not ascend (5, 5, 4) are refused: the section is dropped, counted");
  }
}

// ---- the tap core ------------------------------------------------------------------------------------------------------------------------------
static void TapChecks() {
  using namespace q2d;
  // a rect: constants in ONE upload (c0 c1 then c28 by itself), vertices after
  {
    FakeMem m;
    Store store;
    Counters c;
    Scope s;
    PutRect(m, 0x70000000u, 0, 0, 1280, 720);
    PutRegs(m, 0x70001000u, {{kScaleX, kScaleY, 1, 1}, {0.f, 0.f, 0.f, 0.5f}});
    PutRegs(m, 0x70001100u, {{0.f, 0.f, 0.f, 0.f}});
    OpenScope(s, kQkRect, 0x7000AAAAu);
    OnOp21(s, m, 0, 2, 0x70001000u, c);
    OnOp21(s, m, 28, 1, 0x70001100u, c);
    OnOp19(s, m, store, 9, kSiteRect, 4, 4, 12, 0x70000000u, c);
    CloseScope(s, store, 9, c);
    std::vector<Quad2D> rows;
    store.Drain(9, rows);
    bool ok = rows.size() == 1 && c.rows == 1 && c.no_constants == 0;
    if (ok) {
      const Quad2D& q = rows[0];
      ok = q.kind == kQkRect && q.site == kSiteRect && q.object == 0x7000AAAAu && q.frame == 9 && q.nverts == 4 && q.stride_bytes == 12 && q.prim == 4 && q.seq == 1 &&
           (q.have & (kQhVerts | kQhPosScale | kQhColour | kQhProj)) == (kQhVerts | kQhPosScale | kQhColour | kQhProj);
      const float want[4][3] = {{0, 0, 0.25f}, {1280, 0, 0.25f}, {1280, 720, 0.25f}, {0, 720, 0.25f}};
      for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k) ok = ok && q.verts[i][k] == want[i][k];
      Check(ok, "TAP VERTICES", "a T_Rect scope: the four corners exactly (x y z), site, object, frame, stride 12, ticket 1");
      Check(q.pos_scale[0] == kScaleX && q.pos_scale[1] == kScaleY && q.color[3] == 0.5f && q.use_proj == 0.f, "TAP CONSTANTS", "c0 PosScale, c1 Color (alpha 0.5) and c28 read from two uploads");
    } else {
      Check(false, "TAP VERTICES", "a T_Rect scope made one row");
      Check(false, "TAP CONSTANTS", "constants read");
    }
  }
  // the constants AFTER the vertices, as three uploads; and a scope that never saw them is marked, not guessed
  {
    FakeMem m;
    Store store;
    Counters c;
    Scope s;
    PutRect(m, 0x70000000u, 10, 20, 110, 220);
    PutRegs(m, 0x70001000u, {{kScaleX, kScaleY, 1, 1}});
    PutRegs(m, 0x70001010u, {{1.f, 0.f, 0.f, 0.25f}});
    PutRegs(m, 0x70001020u, {{0.f, 7.f, 7.f, 7.f}});
    OpenScope(s, kQkRect, 0x7000AAAAu);
    OnOp19(s, m, store, 3, kSiteRect, 4, 4, 12, 0x70000000u, c);
    OnOp21(s, m, 0, 1, 0x70001000u, c);
    OnOp21(s, m, 1, 1, 0x70001010u, c);
    OnOp21(s, m, 28, 1, 0x70001020u, c);
    CloseScope(s, store, 3, c);
    OpenScope(s, kQkRect, 0x7000BBBBu);
    OnOp19(s, m, store, 3, kSiteRect, 4, 4, 12, 0x70000000u, c);
    CloseScope(s, store, 3, c);
    std::vector<Quad2D> rows;
    store.Drain(3, rows);
    const bool ok = rows.size() == 2 && rows[0].color[0] == 1.f && rows[0].color[3] == 0.25f && rows[0].use_proj == 0.f && (rows[0].have & kQhColour) && (rows[0].have & kQhProj) && rows[0].verts[2][0] == 110.f &&
                    !(rows[1].have & kQhColour) && c.no_constants == 1;
    Check(ok, "TAP CONSTANTS", "constants seen after the vertices still reach the row; a scope that saw none is marked (no_constants) and the row says so");
    Check(rows.size() == 2 && rows[0].seq < rows[1].seq && rows[1].object == 0x7000BBBBu, "TAP ORDER", "two scopes: tickets ascend in call order");
  }
  // shape / site refusals
  {
    FakeMem m;
    Store store;
    Counters c;
    Scope s;
    PutRect(m, 0x70000000u, 0, 0, 1, 1);
    OpenScope(s, kQkRect, 0x7000AAAAu);
    OnOp19(s, m, store, 1, kSiteRect, 4, 3, 12, 0x70000000u, c);    // 3 vertices
    OnOp19(s, m, store, 1, kSiteRect, 4, 4, 20, 0x70000000u, c);    // a sprite's stride
    OnOp19(s, m, store, 1, 0x82BC5688u, 4, 4, 20, 0x70000000u, c);  // a sprite's site (not this piece's)
    OnOp19(s, m, store, 1, kSiteMovie, 4, 4, 20, 0x70000000u, c);   // another kind's site inside this scope
    OnOp19(s, m, store, 1, kSiteRect, 4, 4, 12, 0x00001000u, c);    // unreadable
    OnOp19(s, m, store, 1, kSiteRect, 4, 4, 12, 0x7F000100u, c);    // the MMIO heap: refused like the plane / circle hooks do
    OnOp21(s, m, 0, 1, 0x7F000100u, c);                              // the same for a constant payload
    CloseScope(s, store, 1, c);
    Check(store.Size() == 0 && c.bad_shape == 2 && c.ignored_site == 2 && c.unreadable == 3, "TAP VERTICES", "a wrong vertex count / stride, a site that is not this piece's, another kind's site and an unreadable pointer (below 0x10000, the MMIO heap) make no row (each counted; a constant payload in the MMIO heap too)");
    Counters c2;
    Scope closed;
    OnOp19(closed, m, store, 1, kSiteRect, 4, 4, 12, 0x70000000u, c2);   // no scope open: the 3D draws
    OnOp21(closed, m, 0, 1, 0x70001000u, c2);
    Check(store.Size() == 0 && c2.rows == 0 && c2.op21_in_scope == 0, "TAP VERTICES", "outside a scope (every 3D draw) the taps do nothing");
  }
  // the movie: planes, flush
  {
    FakeMem m;
    Store store;
    Counters c;
    Scope s;
    const uint32_t obj = 0x70002000u;
    PutMovie(m, 0x70000000u, 160, 600, 1120, 680, 0, 0, 1, 1);
    for (int p = 0; p < 4; ++p) m.Put(obj + kMoviePlaneOffsets[p], 0x4000u + p), m.Put(obj + kMoviePlaneOffsets[p] + 4, 0x9000u + p);
    PutRegs(m, 0x70001000u, {{kScaleX, kScaleY, 1, 1}, {1.f, 1.f, 1.f, 1.f}});
    PutRegs(m, 0x70001100u, {{0.f, 0.f, 0.f, 0.f}});
    store.NoteFlush(0x5555u);
    store.NoteFlush(0x6666u);
    OpenScope(s, kQkYuvAlpha, obj);
    OnOp21(s, m, 0, 2, 0x70001000u, c);
    OnOp21(s, m, 28, 1, 0x70001100u, c);
    OnOp19(s, m, store, 12, kSiteMovie, 4, 4, 20, 0x70000000u, c);
    CloseScope(s, store, 12, c);
    std::vector<Quad2D> rows;
    store.Drain(12, rows);
    bool ok = rows.size() == 1;
    if (ok) {
      const Quad2D& q = rows[0];
      ok = q.kind == kQkYuvAlpha && q.verts[1][0] == 1120.f && q.verts[2][4] == 1.f && q.verts[1][3] == 1.f && q.stride_bytes == 20;
      Check(ok, "TAP VERTICES", "a movie scope: four corners with their texture coordinates, stride 20");
      Check(q.tex_key[0] == 0x4000u && q.tex_key[1] == 0x4001u && q.tex_key[2] == 0x4002u && q.tex_key[3] == 0x4003u && (q.have & kQhPlanes), "TAP FLUSH", "the four plane keys are the words at object +400 / +408 / +416 / +424");
      Check(q.flush_seq == 2 && q.plane_set == 0x6666u && (q.have & kQhFlush), "TAP FLUSH", "the row carries the flush count (2) and the display set of the last flush");
    } else {
      Check(false, "TAP VERTICES", "a movie scope made one row");
      Check(false, "TAP FLUSH", "movie row planes and flush");
    }
  }
  // Drain: frames <= N, in seq order; the later frame stays
  {
    FakeMem m;
    Store store;
    Counters c1, c2;
    Scope s1, s2;
    PutRect(m, 0x70000000u, 0, 0, 4, 4);
    PutRegs(m, 0x70001000u, {{kScaleX, kScaleY, 1, 1}, {1.f, 1.f, 1.f, 1.f}});
    PutRegs(m, 0x70001100u, {{0.f, 0.f, 0.f, 0.f}});
    // two "threads" interleave: scope 1 opens first, scope 2 opens and draws, then scope 1 draws (its ticket is later), closing in the other order
    OpenScope(s1, kQkRect, 0x7000AA01u);
    OpenScope(s2, kQkRect, 0x7000AA02u);
    for (Scope* sc : {&s1, &s2}) OnOp21(*sc, m, 0, 2, 0x70001000u, sc == &s1 ? c1 : c2), OnOp21(*sc, m, 28, 1, 0x70001100u, sc == &s1 ? c1 : c2);
    OnOp19(s2, m, store, 4, kSiteRect, 4, 4, 12, 0x70000000u, c2);
    OnOp19(s1, m, store, 4, kSiteRect, 4, 4, 12, 0x70000000u, c1);
    CloseScope(s1, store, 4, c1);   // pushed first (ticket 2), the other second (ticket 1)
    CloseScope(s2, store, 4, c2);
    OpenScope(s1, kQkRect, 0x7000AA03u);
    OnOp21(s1, m, 0, 2, 0x70001000u, c1), OnOp21(s1, m, 28, 1, 0x70001100u, c1);
    OnOp19(s1, m, store, 5, kSiteRect, 4, 4, 12, 0x70000000u, c1);
    CloseScope(s1, store, 5, c1);   // frame 5
    std::vector<Quad2D> rows;
    store.Drain(4, rows);
    const bool a = rows.size() == 2 && rows[0].object == 0x7000AA02u && rows[1].object == 0x7000AA01u && rows[0].seq == 1 && rows[1].seq == 2;
    std::vector<Quad2D> later;
    store.Drain(5, later);
    Check(a && later.size() == 1 && later[0].frame == 5 && later[0].seq == 3, "TAP ORDER", "Drain(4) returns the frame-4 rows in call order (ticket) whichever scope closed first; the frame-5 row waits for its own drain");
    {   // S103 SC review: before the stamp, an unstamped row must be refused for its missing scissor state (so the stamp below is the only thing that makes it valid)
      bool unstamped_refused = !rows.empty();
      for (const Quad2D& r : rows) unstamped_refused = unstamped_refused && Quad2DRowProblem(r) == "no scissor state read";
      Check(unstamped_refused, "TAP ORDER", "an unstamped row (no state object read) returns \"no scissor state read\"");
    }
    for (Quad2D& r : rows) ValidSC(r);   // S103 SC: these rows were made without a state object (no r13): the scissor / target state a layout-4 row needs is stamped here; the check is the ticket order
    Check(Quads2DProblem(rows).empty(), "TAP ORDER", "a drained run passes the section's own validity check");
  }
  // the bounded ring: at the cap the OLDEST frame goes, the newest rows stay
  {
    FakeMem m;
    Store store;
    Counters c;
    PutRect(m, 0x70000000u, 0, 0, 4, 4);
    PutRegs(m, 0x70001000u, {{kScaleX, kScaleY, 1, 1}, {1.f, 1.f, 1.f, 1.f}});
    PutRegs(m, 0x70001100u, {{0.f, 0.f, 0.f, 0.f}});
    Scope s;
    for (uint32_t i = 0; i < kRowCap + 5; ++i) {
      OpenScope(s, kQkRect, 0x7000AA00u);
      OnOp21(s, m, 0, 2, 0x70001000u, c), OnOp21(s, m, 28, 1, 0x70001100u, c);
      OnOp19(s, m, store, 1 + i / 100, kSiteRect, 4, 4, 12, 0x70000000u, c);
      CloseScope(s, store, 1 + i / 100, c);
    }
    std::vector<Quad2D> rows;
    store.Drain(0xFFFFFFFFu, rows);
    Check(rows.size() <= kRowCap && rows.size() > kRowCap - 200 && c.over_cap > 0 && rows.back().seq == kRowCap + 5 && rows.front().frame > 1, "TAP ORDER",
          "with nobody draining, the store is a bounded ring: past the cap the oldest frame's rows are dropped (counted), the newest stay");
  }
  // two quads in one scope take the colour each was drawn with
  {
    FakeMem m;
    Store store;
    Counters c;
    Scope s;
    PutRect(m, 0x70000000u, 0, 0, 4, 4);
    PutRegs(m, 0x70001000u, {{kScaleX, kScaleY, 1, 1}, {1.f, 0.f, 0.f, 1.f}});
    PutRegs(m, 0x70001100u, {{0.f, 0.f, 0.f, 0.f}});
    PutRegs(m, 0x70001200u, {{0.f, 0.f, 1.f, 0.25f}});
    OpenScope(s, kQkRect, 0x7000AA00u);
    OnOp21(s, m, 0, 2, 0x70001000u, c), OnOp21(s, m, 28, 1, 0x70001100u, c);
    OnOp19(s, m, store, 1, kSiteRect, 4, 4, 12, 0x70000000u, c);   // quad 1: red
    OnOp21(s, m, 1, 1, 0x70001200u, c);                              // the colour changes
    OnOp19(s, m, store, 1, kSiteRect, 4, 4, 12, 0x70000000u, c);   // quad 2: blue, alpha 0.25
    CloseScope(s, store, 1, c);
    std::vector<Quad2D> rows;
    store.Drain(1, rows);
    Check(rows.size() == 2 && rows[0].color[0] == 1.f && rows[0].color[2] == 0.f && rows[0].color[3] == 1.f && rows[1].color[0] == 0.f && rows[1].color[2] == 1.f && rows[1].color[3] == 0.25f, "TAP CONSTANTS",
          "two quads in one scope: each row keeps the colour it was drawn with (the first is not given the second's)");
  }
}

// ---- BlocksOf ----------------------------------------------------------------------------------------------------------------------------------
static void BlockChecks() {
  using namespace q2d;
  // an independent transcription of the programs' layouts (rdr_uiquad.vert / rdr_uiquad_flat.frag / rdr_movie.vert / rdr_movie.frag): the numbers, written out
  {
    Blocks b;
    const Refuse r = BlocksOf(GoodRect(1), b);
    float want_vs[36] = {};
    want_vs[0] = kScaleX, want_vs[1] = kScaleY, want_vs[2] = 0.f, want_vs[3] = 0.5f;
    const float pos[4][3] = {{0, 0, 0.25f}, {1280, 0, 0.25f}, {1280, 720, 0.25f}, {0, 720, 0.25f}};
    for (int i = 0; i < 4; ++i)
      for (int c = 0; c < 3; ++c) want_vs[4 + 4 * i + c] = pos[i][c];
    const float want_fs[8] = {0.f, 0.f, 0.f, 0.5f, 1.f, 0.f, 1.f, 0.f};
    Check(r == Refuse::None && b.program == Program::Rect && b.vs_bytes == 144 && b.fs_bytes == 32 && std::memcmp(b.vs, want_vs, sizeof(want_vs)) == 0 && std::memcmp(b.fs, want_fs, sizeof(want_fs)) == 0, "BLOCKS EXACT",
          "a 50% black fade: UnitScale (2/1280, 2/720, 0, 0.5), the four corners, Color then AlphaMix (1, 0, 1, 0) - equal to the written-out layout");
  }
  {
    Blocks b;
    const Refuse r = BlocksOf(GoodMovie(1), b);
    float want_vs[12] = {kScaleX, kScaleY, 0.5f, 0.5f, 160, 600, 1120, 680, 0, 0, 1, 1};
    float want_fs[20] = {1, 1, 1, 1, 1.164f, 0.f, 1.596f, -0.87075f, 1.164f, -0.391f, -0.813f, 0.52925f, 1.164f, 2.018f, 0.f, -1.08175f, 1.f, 0.f, 1.f, 0.f};
    Check(r == Refuse::None && b.program == Program::Movie && b.vs_bytes == 48 && b.fs_bytes == 80 && std::memcmp(b.vs, want_vs, sizeof(want_vs)) == 0 && std::memcmp(b.fs, want_fs, sizeof(want_fs)) == 0, "BLOCKS EXACT",
          "a movie quad: UnitScale + rectangle + uv corners, Color, the BT.601 rows, AlphaMix - equal to the written-out layout");
  }
  struct Case { const char* name; Refuse want; Quad2D row; };
  std::vector<Case> cases;
  { Quad2D q = GoodRect(1); q.version = 99; cases.push_back({"layout", Refuse::Layout, q}); }
  { Quad2D q = GoodRect(1); q.kind = kQkRectGrad; cases.push_back({"kind", Refuse::Kind, q}); }
  { Quad2D q = GoodRect(1); q.have &= ~uint32_t(kQhVerts); cases.push_back({"no vertices", Refuse::MissingVerts, q}); }
  { Quad2D q = GoodRect(1); q.have &= ~uint32_t(kQhColour); cases.push_back({"no colour", Refuse::MissingConstants, q}); }
  { Quad2D q = GoodRect(1); q.use_proj = 1.f; cases.push_back({"projection matrix", Refuse::Projection, q}); }
  { Quad2D q = GoodRect(1); q.verts[1][0] = INFINITY; cases.push_back({"non-finite", Refuse::NotFinite, q}); }
  { Quad2D q = GoodMovie(1); q.verts[2][0] += 3.f; cases.push_back({"a movie quad that is not a rectangle", Refuse::NotRect, q}); }
  for (const Case& c : cases) {
    Blocks b;
    Check(BlocksOf(c.row, b) == c.want, "BLOCKS EXACT", std::string("refused as '") + RefuseName(c.want) + "': " + c.name);
  }
}

// ---- Append ------------------------------------------------------------------------------------------------------------------------------------
static void DrawChecks() {
  using namespace q2d;
  rdr::FrameBuilder fb;
  fb.Begin(1);
  Programs programs;
  programs.rect = 11, programs.movie = 22, programs.layout = 3;
  Context ctx;
  ctx.width = 1920, ctx.height = 1080;
  ctx.plane = [](const Quad2D& row, int slot) {
    rdr::TextureSlot t;
    t.image = rdr::ImageId(row.tex_image[slot] * 10);   // the resolver's own mapping: capture id -> renderer id
    return t;
  };
  std::vector<Quad2D> rows;
  Quad2D fade = GoodRect(1);                               // alpha blend, alpha test off
  Quad2D add = GoodRect(2);  add.blend = kQbAdd; add.have |= kQhAlphaTest; add.alpha_func = 4; add.alpha_ref = 0.5f;
  Quad2D movie = GoodMovie(3);
  Quad2D unknown = GoodRect(4);  unknown.blend = kQbUnknown;
  Quad2D noplanes = GoodMovie(5);  noplanes.tex_image[2] = 0;
  Quad2D bad = GoodRect(6);  bad.use_proj = 1.f;
  rows = {fade, add, movie, unknown, noplanes, bad};
  const Report rep = Append(fb, programs, ctx, rows.data(), rows.size());
  const rdr::Frame frame = fb.Finish();
  Check(rep.drawn == 3 && rep.rect == 2 && rep.movie == 1 && rep.refused[uint8_t(Refuse::Blend)] == 1 && rep.refused[uint8_t(Refuse::Planes)] == 1 && rep.refused[uint8_t(Refuse::Projection)] == 1 && rep.refused_total() == 3 && frame.draws.count == 3,
        "DRAW STATE", "six rows: three draws; the unknown blend, the movie missing a plane and the projection-matrix row are refused, each by its own reason");
  if (frame.draws.count == 3) {
    const rdr::DrawOp& d0 = frame.draws[0];
    const rdr::DrawState& s0 = frame.states[d0.state];
    const rdr::DrawState& s1 = frame.states[frame.draws[1].state];
    const rdr::DrawState& s2 = frame.states[frame.draws[2].state];
    const rdr::DynamicState& y0 = frame.dynamics[d0.dynamic];
    const rdr::DynamicState& y1 = frame.dynamics[frame.draws[1].dynamic];
    Check(s0.program == 11 && s0.vertex_layout == 3 && s0.topology == 3 && s0.blend[0].enable == 1 && s0.blend[0].src_color == 6 && s0.blend[0].dst_color == 7 && s0.blend[0].write_mask == 0xF && s0.depth_test == 0 && s0.depth_write == 0 &&
              s1.blend[0].enable == 1 && s1.blend[0].src_color == 6 && s1.blend[0].dst_color == 1 && s2.program == 22 && d0.count == 6 && d0.instances == 1,
          "DRAW STATE", "the fade is program 11, triangle list, SrcAlpha / InvSrcAlpha, no depth; the additive row is SrcAlpha / One; the movie is program 22");
    Check(y0.viewport[2] == 1920.f && y0.viewport[3] == 1080.f && y0.scissor.width == 1920 && y0.system[0] == 1.f && y0.system[1] == -1.f && y0.system[2] == 1.f && y0.system[4] == 1.0f / 1920.0f && y0.system[5] == 1.0f / 1080.0f && y0.system[9] == 7.f && y1.system[8] == 0.5f && y1.system[9] == 4.f, "DRAW STATE",
          "the whole target as viewport and scissor; ndc scale (1, -1, 1) and the half pixel offset (1/w, 1/h) (S103 P7 H2: the layer was upside down); alpha test 'always' for the fade and (0.5, function 4) for the row that asked for it");
    const rdr::TextureSet& ts = frame.texture_sets[frame.draws[2].textures];
    Check(ts.slots[0].image == 1000 && ts.slots[1].image == 1010 && ts.slots[2].image == 1020 && ts.slots[3].image == 1030, "DRAW STATE", "the movie's four plane slots come from the resolver (picture is an input, not state)");
    Blocks want;
    BlocksOf(fade, want);
    Check(std::memcmp(frame.constants.data + d0.vs_constants, want.vs, want.vs_bytes) == 0 && std::memcmp(frame.constants.data + d0.fs_constants, want.fs, want.fs_bytes) == 0, "DRAW STATE", "the constants pushed for a draw are the row's blocks, byte for byte");
    Check(frame.draws[0].vs_constants < frame.draws[1].vs_constants && frame.draws[1].vs_constants < frame.draws[2].vs_constants && s0.program == 11 && s1.program == 11 && s2.program == 22, "DRAW ORDER",
          "the draws come in the rows' order (fade, additive rect, movie)");
  } else {
    Check(false, "DRAW STATE", "three draws were made");
    Check(false, "DRAW ORDER", "three draws were made");
  }
}

// ---- the rig ------------------------------------------------------------------------------------------------------------------------------------
static void RigChecks() {
  using namespace q2d;
  rdr_adapter::DrawFixed fixed;
  fixed.state.blend[0].enable = 1, fixed.state.blend[0].src_color = 6, fixed.state.blend[0].dst_color = 7, fixed.state.blend[0].color_op = 0;
  const float v[4][5] = {{0, 0, 0.25f, 0, 0}, {1280, 0, 0.25f, 0, 0}, {1280, 720, 0.25f, 0, 0}, {0, 720, 0.25f, 0, 0}};
  const float ps[4] = {kScaleX, kScaleY, 1, 1}, col[4] = {0, 0, 0, 0.5f};
  rig::g_stats = rig::Stats{};
  rig::g_faults = rig::Faults{};
  rig::g_roundtrip = nullptr;
  Quad2D row = rig::Truth(kQkRect, kSiteRect, 0x1234u, 55, v, ps, col, 0.f, fixed);
  Blocks b;
  bool deg = false;
  Check(row.blend == kQbAlpha && (row.have & kQhBlend) && row.alpha_func == 7 && row.frame == 55 && rig::Make(row, fixed, b, deg) == Refuse::None && !deg && rig::g_stats.blend_mismatch == 0 && rig::g_stats.alpha_mismatch == 0, "RIG",
        "the truth row of an alpha-blended stream rect agrees with the stream's state (no mismatch)");
  {
    rig::Faults f;
    std::string e;
    rig::Faults::Parse("wrong_blend", f, e);
    rig::g_faults = f;
    Quad2D r2 = rig::Truth(kQkRect, kSiteRect, 0x1234u, 55, v, ps, col, 0.f, fixed);
    rig::Make(r2, fixed, b, deg);
    Check(rig::g_stats.blend_mismatch == 1, "RIG", "the wrong_blend twin raises the state mismatch counter (the picture cannot see it: the seam keeps the stream's state)");
  }
  {
    rig::g_faults = rig::Faults{};
    rig::g_faults.drop_rect = true;
    Quad2D r3 = rig::Truth(kQkRect, kSiteRect, 0x1234u, 55, v, ps, col, 0.f, fixed);
    rig::Make(r3, fixed, b, deg);
    bool zero = deg;
    for (int i = 1; i < 4; ++i)
      for (int c = 0; c < 3; ++c) zero = zero && b.vs[4 + 4 * i + c] == b.vs[4 + c];
    Check(zero, "RIG", "the drop_rect twin draws a quad of no area (nothing appears in any blend state)");
    rig::g_faults = rig::Faults{};
    rig::g_faults.swap_vertices = true;
    Quad2D r4 = rig::Truth(kQkRect, kSiteRect, 0x1234u, 55, v, ps, col, 0.f, fixed);
    Blocks sw;
    rig::Make(r4, fixed, sw, deg);
    Blocks clean;
    BlocksOf(rig::Truth(kQkRect, kSiteRect, 0x1234u, 55, v, ps, col, 0.f, fixed), clean);
    Check(std::memcmp(sw.vs, clean.vs, sizeof(clean.vs)) != 0 && sw.vs[4 + 4 * 1] == 1280.f && sw.vs[4 + 4 * 1 + 1] == 720.f, "RIG", "the swap_vertices twin exchanges corners 1 and 2 in the block");
  }
  rig::g_faults = rig::Faults{};
  std::string e;
  Check(!rig::Faults::Parse("no_such_plant", rig::g_faults, e) && !e.empty(), "RIG", "an unknown --quads-2d-fault name is refused");
  rig::g_faults = rig::Faults{};
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
#ifdef RDR_Q2D_PLANTS
  if (plant == "tap_no_colour") q2d::g_plant = 1;
  else if (plant == "tap_vertex_order") q2d::g_plant = 2;
  else if (plant == "tap_ticket_reuse") q2d::g_plant = 3;
  else if (plant == "tap_no_flush") q2d::g_plant = 4;
  else if (plant == "drain_unsorted") q2d::g_plant = 5;
  else if (plant == "blocks_no_half") q2d::g_plant = 6;
  else if (plant == "blocks_blend") q2d::g_plant = 7;
  else if (plant == "draw_reversed") q2d::g_plant = 8;
  else if (plant == "parse_seq") q2d::g_plant = 9;
  else if (plant == "tap_late_colour") q2d::g_plant = 10;
  else if (!plant.empty()) {
    std::fprintf(stderr, "unknown plant %s\n", plant.c_str());
    return 2;
  }
#else
  if (!plant.empty()) {
    std::fprintf(stderr, "built without -DRDR_Q2D_PLANTS\n");
    return 2;
  }
#endif
  RecordChecks();
  TapChecks();
  BlockChecks();
  DrawChecks();
  RigChecks();
  if (!plant.empty()) {
    const char* want = plant == "tap_no_colour" || plant == "tap_late_colour" ? "[TAP CONSTANTS]" : plant == "tap_vertex_order" ? "[TAP VERTICES]" : plant == "tap_ticket_reuse" || plant == "drain_unsorted" ? "[TAP ORDER]" : plant == "tap_no_flush" ? "[TAP FLUSH]"
                     : plant == "blocks_no_half" ? "[BLOCKS EXACT]" : plant == "blocks_blend" ? "[DRAW STATE]" : plant == "draw_reversed" ? "[DRAW ORDER]" : "[RECORD SEQ]";
    const bool caught = g_failed.find(want) != std::string::npos;
    std::printf("%s planted %s: %s (failed: %s)\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? (std::string("caught by its named check ") + want).c_str() : (std::string("its named check ") + want + " did NOT fail").c_str(), g_failed.c_str());
    return caught ? 0 : 1;
  }
  std::printf("%s: i1_unit: %d checks, %d failed\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
