// Lane I3 (Session 103, landing 2, docs\renderer\REMNANT_MAP.md section 12): the offline unit proof of the 2D layer's GLYPH rows (the text). 0 game runs, no GPU, no game file.
//
//   i3_unit                 every check; exit 0 only when all pass
//   i3_unit --plant NAME    exit 0 only when the check that sees the plant FAILS (plants: src\renderer_scene\quads2d_plants.h behind -DRDR_Q2D_PLANTS)
//
// Checks (tag in brackets; a plant names the tag that must fail):
//   [GLYPH RECORD]  a glyph row (the channel selector c4 in the row's 16-byte hsl field, kQhChannel, the primitive word, the sheet key) round-trips byte-equal through section 58; a NaN selector refuses THIS row only
//   [GLYPH TAP]     the taps over a fixture guest memory: the thread's REGISTER FILE (c0 / c28 uploaded by an earlier draw, c4 by an earlier glyph, c1 by this glyph's leaf) makes a complete row; the sheet key is the word at
//                   object + 416; four vertices with uv at stride 20; the primitive word 6; a second glyph with another colour keeps its own; the leaf's other draw sites (0x82BC3758, 0x82BC8BEC) make no row (counted)
//                   (plants glyph_no_selector, glyph_stale_regs, glyph_key_off)
//   [GLYPH BLOCKS]  BlocksOf on a glyph row = a written-out transcription of rdr_uiquad.vert / rdr_uiglyph.frag's constant layout (corners with uv, Color, AlphaMix, Selector); a row with no selector, a primitive that is not a
//                   fan / quad list, a non-finite selector is refused by its own reason (plants glyph_blocks_sel, glyph_prim_any)
//   [GLYPH DRAW]    Append: the glyph program, slot 0 = the sheet the row's KEY resolves to, blend and alpha test from the row, the glyph counter; an unknown sheet and an unknown blend are counted by reason
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

static const float kScaleX = 2.0f / 1280.0f, kScaleY = 2.0f / 720.0f;
static const uint32_t kObject = 0x40001000u, kVerts = 0x40002000u, kPayload = 0x40003000u, kKey = 0x00060009u;

static void PutGlyphVerts(FakeMem& m, uint32_t at, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, float z = 0.5f) {
  const float v[4][5] = {{x0, y0, z, u0, v0}, {x1, y0, z, u1, v0}, {x1, y1, z, u1, v1}, {x0, y1, z, u0, v1}};
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) m.PutF(at + 20 * i + 4 * c, v[i][c]);
}

static Quad2D GoodGlyph(uint32_t seq) {
  Quad2D q;
  q.kind = kQkGlyph, q.site = q2d::kSiteGlyph, q.seq = seq, q.frame = 7, q.nverts = 4, q.stride_bytes = 20, q.prim = 6;
  q.have = kQhVerts | kQhPosScale | kQhColour | kQhProj | kQhBlend | kQhAlphaTest | kQhPlanes | kQhChannel;
  q.blend = kQbAlpha;
  const float v[4][5] = {{100, 50, 0.5f, 0.25f, 0.125f}, {116, 50, 0.5f, 0.375f, 0.125f}, {100, 82, 0.5f, 0.25f, 0.25f}, {116, 82, 0.5f, 0.375f, 0.25f}};   // a strip: the Z order 0 1 2 3
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 5; ++c) q.verts[i][c] = v[i][c];
  q.pos_scale[0] = kScaleX, q.pos_scale[1] = kScaleY, q.pos_scale[2] = NAN, q.pos_scale[3] = NAN;
  q.color[0] = 1.f, q.color[1] = 0.5f, q.color[2] = 0.25f, q.color[3] = 0.75f;
  q.hsl[0] = 0.f, q.hsl[1] = 0.f, q.hsl[2] = 0.f, q.hsl[3] = 0.4f;   // the selector: the alpha channel is an alpha-mask channel
  q.tex_key[0] = kKey;
  ValidSC(q);
  return q;
}
static bool Same(const Quad2D& a, const Quad2D& b) { return std::memcmp(&a, &b, sizeof(Quad2D)) == 0; }

// ---- [GLYPH RECORD] ------------------------------------------------------------------------------------------------------------------------------
static void RecordChecks() {
  SceneTables t;
  t.has_camera = true;
  t.quads2d = {GoodGlyph(1), GoodGlyph(2)};
  const std::vector<uint8_t> bytes = SerializeRecord(t);
  SceneTables back;
  std::string err;
  const bool ok = ParseRecord(bytes.data(), bytes.size(), back, err);
  Check(sizeof(Quad2D) == 324 && ok && back.quads2d.size() == 2 && Same(back.quads2d[0], t.quads2d[0]) && Same(back.quads2d[1], t.quads2d[1]) && back.quad2d_errors == 0 && back.quads2d[0].prim == 6 && (back.quads2d[0].have & kQhChannel),
        "GLYPH RECORD", "two glyph rows round-trip byte-equal (the selector in hsl[], kQhChannel, prim 6, the sheet key); the row size is 324 bytes (S103 SC layout 4)");
  SceneTables bad = t;
  bad.quads2d[0].hsl[3] = NAN;
  const std::vector<uint8_t> b2 = SerializeRecord(bad);
  SceneTables back2;
  const bool ok2 = ParseRecord(b2.data(), b2.size(), back2, err);
  Check(ok2 && back2.quads2d.size() == 1 && Same(back2.quads2d[0], t.quads2d[1]) && back2.quad2d_errors == 1, "GLYPH RECORD", "a glyph row with a non-finite selector is refused alone (counted); the other row and the record are read");
}

// ---- [GLYPH TAP] ---------------------------------------------------------------------------------------------------------------------------------
static void TapChecks() {
  using namespace q2d;
  FakeMem m;
  m.Put(kObject + kGlyphKeyOffset, kKey);
  m.Put(kObject + kSpriteKeyOffset, 0x00011111u);   // (the sprite's offset holds another key: a glyph row must not read it)
  PutGlyphVerts(m, kVerts, 100, 50, 116, 82, 0.25f, 0.125f, 0.375f, 0.25f);
  // the earlier draw's uploads (outside any glyph scope): c0 PosScale (two floats + the stack), c28 flag 0; an earlier glyph's selector c4 (x y z w)
  m.PutF(kPayload, kScaleX), m.PutF(kPayload + 4, kScaleY), m.Put(kPayload + 8, 0x7FC00000u), m.Put(kPayload + 12, 0x7FC00000u);   // c0
  m.PutF(kPayload + 16, 0.f), m.PutF(kPayload + 20, 0.f), m.PutF(kPayload + 24, 0.f), m.PutF(kPayload + 28, 0.f);                  // c28 (flag 0)
  m.PutF(kPayload + 32, 0.f), m.PutF(kPayload + 36, 0.f), m.PutF(kPayload + 40, 0.f), m.PutF(kPayload + 44, 0.4f);                // c4
  // this glyph's leaf: c1 colour
  m.PutF(kPayload + 64, 1.f), m.PutF(kPayload + 68, 0.5f), m.PutF(kPayload + 72, 0.25f), m.PutF(kPayload + 76, 0.75f);
  // a second glyph: another colour
  m.PutF(kPayload + 96, 0.f), m.PutF(kPayload + 100, 0.f), m.PutF(kPayload + 104, 0.f), m.PutF(kPayload + 108, 1.f);
  // a upload of c0..c4 in ONE call: c0 at +128, c4 at +128 + 64
  m.PutF(kPayload + 128, kScaleX * 2), m.PutF(kPayload + 132, kScaleY * 2);
  for (int r = 1; r < 5; ++r)
    for (int c = 0; c < 4; ++c) m.PutF(kPayload + 128 + 16 * r + 4 * c, r == 4 ? float(c) * 0.25f : 9.f);
  Store store;
  Counters c;
  Shadow sh;
  Scope s;
  const uint32_t F = 9;
  struct M2 {
    const FakeMem& m;
    bool ReadWords(uint32_t a, uint32_t* o, uint32_t n) const { return m.ReadWords(a, o, n); }
  } mem{m};
  OnOp21Shadow(sh, mem, 0, 1, kPayload, c, F);          // an earlier draw: c0
  OnOp21Shadow(sh, mem, 28, 1, kPayload + 16, c, F);    // c28
  OnOp21Shadow(sh, mem, 4, 1, kPayload + 32, c, F);     // an earlier glyph's c4
  OpenScope(s, kQkGlyph, kObject);
  OnOp21Shadow(sh, mem, 1, 1, kPayload + 64, c, F);     // this leaf's c1 (the hook runs the shadow for every upload, in scope or not)
  OnOp21(s, mem, 1, 1, kPayload + 64, c);
  OnOp19(s, mem, store, F, kSiteGlyph, 6, 4, 20, kVerts, c, &sh);
  CloseScope(s, store, F, c);
  std::vector<Quad2D> rows;
  store.Drain(F, rows);
  const bool one = rows.size() == 1;
  const Quad2D r = one ? rows[0] : Quad2D{};
  Check(one && r.kind == kQkGlyph && r.site == kSiteGlyph && r.object == kObject && r.nverts == 4 && r.stride_bytes == 20 && r.prim == 6 && r.verts[0][0] == 100 && r.verts[0][3] == 0.25f && r.verts[0][4] == 0.125f && r.verts[2][0] == 116 &&
            r.verts[2][1] == 82 && r.verts[2][3] == 0.375f && r.verts[2][4] == 0.25f,
        "GLYPH TAP", "four vertices with their uv exactly as the guest wrote them (stride 20), the primitive word 6");
  Check(one && r.tex_key[0] == kKey && (r.have & kQhPlanes), "GLYPH TAP", "the sheet's key is the word at object + 416 (not the sprite's + 400)");
  Check(one && r.pos_scale[0] == kScaleX && r.pos_scale[1] == kScaleY && r.use_proj == 0.f && r.color[0] == 1.f && r.color[1] == 0.5f && r.color[3] == 0.75f && (r.have & (kQhPosScale | kQhColour | kQhProj)) == (kQhPosScale | kQhColour | kQhProj),
        "GLYPH TAP", "c0 and c28 come from the thread's register file (an earlier draw uploaded them), c1 from this glyph's own upload");
  Check(one && (r.have & kQhChannel) && r.hsl[0] == 0.f && r.hsl[3] == 0.4f && c.glyph_no_shadow == 0 && c.glyph_rows == 1, "GLYPH TAP", "the channel selector c4 is the register's last value (an earlier glyph's upload): (0, 0, 0, 0.4)");
  // a second glyph, another colour, no new c4: it keeps the register's c4 and its own colour; the first row is unchanged
  PutGlyphVerts(m, kVerts, 116, 50, 132, 82, 0.375f, 0.125f, 0.5f, 0.25f);
  OpenScope(s, kQkGlyph, kObject);
  OnOp21Shadow(sh, mem, 1, 1, kPayload + 96, c, F);
  OnOp21(s, mem, 1, 1, kPayload + 96, c);
  OnOp19(s, mem, store, F, kSiteGlyph, 6, 4, 20, kVerts, c, &sh);
  CloseScope(s, store, F, c);
  rows.clear();
  store.Drain(F, rows);
  Check(rows.size() == 1 && rows[0].color[3] == 1.f && rows[0].color[0] == 0.f && rows[0].hsl[3] == 0.4f && rows[0].seq == r.seq + 1 && rows[0].verts[0][0] == 116, "GLYPH TAP", "a second glyph has its own colour and vertices, the same selector, the next ticket");
  // an upload of c0..c4 in one call: c4 is read at payload + 64
  OnOp21Shadow(sh, mem, 0, 5, kPayload + 128, c, F);
  Check((sh.have & kQhChannel) && sh.sel[0] == 0.f && sh.sel[1] == 0.25f && sh.sel[2] == 0.5f && sh.sel[3] == 0.75f && sh.pos_scale[0] == kScaleX * 2 && sh.color[0] == 9.f, "GLYPH TAP", "one upload of c0..c4 sets c4 from its fifth float4 (and c0, c1)");
  // a thread that never saw c4: the row is made but incomplete (counted), and the drawer refuses it
  Shadow empty;
  OpenScope(s, kQkGlyph, kObject);
  OnOp21Shadow(empty, mem, 1, 1, kPayload + 64, c, F);
  OnOp21(s, mem, 1, 1, kPayload + 64, c);
  OnOp19(s, mem, store, F, kSiteGlyph, 6, 4, 20, kVerts, c, &empty);
  CloseScope(s, store, F, c);
  rows.clear();
  store.Drain(F, rows);
  Blocks b;
  Check(rows.size() == 1 && !(rows[0].have & kQhChannel) && c.glyph_no_shadow >= 1 && BlocksOf(rows[0], b) != Refuse::None, "GLYPH TAP", "a thread whose register file lacks c0 / c4 / c28 makes an incomplete row (counted) that the drawer refuses");
  // the leaf's other draw sites
  const uint64_t ignored = c.ignored_site, m_before = c.glyph_m_site;
  OpenScope(s, kQkGlyph, kObject);
  OnOp21(s, mem, 4, 1, kPayload + 32, c);   // S103 P3b: the channel selector uploaded in the scope = not a plain T_Sprite (technique 0x8206AA0C) (a plain one is a sprite row now: tools\scene\p3b_unit.cpp)
  OnOp19(s, mem, store, F, 0x82BC3758u, 6, 4, 20, kVerts, c, &sh);
  OnOp19(s, mem, store, F, kSiteGlyphM, 6, 4, 20, kVerts, c, &sh);
  CloseScope(s, store, F, c);
  rows.clear();
  store.Drain(F, rows);
  Check(rows.empty() && c.ignored_site == ignored + 2 && c.glyph_m_site == m_before + 1, "GLYPH TAP", "the glyph leaf's per-part draw (0x82BC3758) and the sprite-variant leaf's site (0x82BC8BEC) make no row; both are counted, the second apart");
  // a wrong shape: 6 vertices, a stride of 24
  const uint64_t bad = c.bad_shape;
  OpenScope(s, kQkGlyph, kObject);
  OnOp19(s, mem, store, F, kSiteGlyph, 6, 6, 20, kVerts, c, &sh);
  OnOp19(s, mem, store, F, kSiteGlyph, 6, 4, 24, kVerts, c, &sh);
  CloseScope(s, store, F, c);
  rows.clear();
  store.Drain(F, rows);
  Check(rows.empty() && c.bad_shape == bad + 2, "GLYPH TAP", "a glyph draw that is not four vertices of stride 20 makes no row (counted)");
}

// ---- [GLYPH STALE] / [GLYPH HOLES] ---------------------------------------------------------------------------------------------------------------
static void StaleAndHoleChecks() {
  using namespace q2d;
  FakeMem m;
  m.Put(kObject + kGlyphKeyOffset, kKey);
  PutGlyphVerts(m, kVerts, 100, 50, 116, 82, 0.25f, 0.125f, 0.375f, 0.25f);
  m.PutF(kPayload, kScaleX), m.PutF(kPayload + 4, kScaleY);
  m.PutF(kPayload + 16, 0.f);
  m.PutF(kPayload + 44, 0.4f);
  m.PutF(kPayload + 64, 1.f), m.PutF(kPayload + 68, 1.f), m.PutF(kPayload + 72, 1.f), m.PutF(kPayload + 76, 1.f);
  struct M2 {
    const FakeMem& m;
    bool ReadWords(uint32_t a, uint32_t* o, uint32_t n) const { return m.ReadWords(a, o, n); }
  } mem{m};
  Store store;
  Counters c;
  Scope s;
  const uint32_t F = 20;
  Shadow sh;   // c0 / c28 / c4 uploaded in the PREVIOUS frame (a 3D draw, an earlier menu), c1 in this one
  OnOp21Shadow(sh, mem, 0, 1, kPayload, c, F - 1);
  OnOp21Shadow(sh, mem, 28, 1, kPayload + 16, c, F - 1);
  OnOp21Shadow(sh, mem, 4, 1, kPayload + 32, c, F - 1);
  OpenScope(s, kQkGlyph, kObject);
  OnOp21Shadow(sh, mem, 1, 1, kPayload + 64, c, F);
  OnOp19(s, mem, store, F, kSiteGlyph, 6, 4, 20, kVerts, c, &sh);
  CloseScope(s, store, F, c);
  std::vector<Quad2D> rows;
  store.Drain(F, rows);
  Blocks b;
  Check(rows.size() == 1 && !(rows[0].have & (kQhPosScale | kQhProj | kQhChannel)) && (rows[0].have & kQhColour) && c.glyph_stale == 3 && c.glyph_no_shadow == 1 && BlocksOf(rows[0], b) != Refuse::None, "GLYPH STALE",
        "a c0 / c28 / c4 uploaded in the previous frame is refused (counted 3) and the row is incomplete: the drawer refuses it; this frame's c1 is kept");
  // the same uploads in this frame: complete
  Shadow now;
  OnOp21Shadow(now, mem, 0, 1, kPayload, c, F);
  OnOp21Shadow(now, mem, 28, 1, kPayload + 16, c, F);
  OnOp21Shadow(now, mem, 4, 1, kPayload + 32, c, F);
  OnOp21Shadow(now, mem, 1, 1, kPayload + 64, c, F);
  OpenScope(s, kQkGlyph, kObject);
  OnOp19(s, mem, store, F, kSiteGlyph, 6, 4, 20, kVerts, c, &now);
  CloseScope(s, store, F, c);
  rows.clear();
  store.Drain(F, rows);
  Check(rows.size() == 1 && (rows[0].have & (kQhPosScale | kQhProj | kQhChannel | kQhColour)) == (kQhPosScale | kQhProj | kQhChannel | kQhColour) && BlocksOf(rows[0], b) == Refuse::None, "GLYPH STALE", "uploads stamped with this frame make a complete, drawable row");
  // holes: per frame and site
  std::vector<Store::Hole> holes;
  Store hs;
  Counters hc;
  const uint32_t G = 30;
  Scope t;
  OpenScope(t, kQkGlyph, kObject);
  OnOp21(t, mem, 4, 1, kPayload + 32, hc);   // S103 P3b: a selector-carrying variant (a plain T_Sprite (technique 0x8206AA0C) is a row)
  OnOp19(t, mem, hs, G, 0x82BC3758u, 6, 4, 20, kVerts, hc, &now);       // the per-part path
  OnOp19(t, mem, hs, G, kSiteGlyphM, 6, 4, 20, kVerts, hc, &now);       // the variant leaf's site
  OnOp19(t, mem, hs, G, kSiteGlyph, 6, 6, 20, kVerts, hc, &now);        // a wrong shape
  CloseScope(t, hs, G, hc);
  Scope none;
  OnOp19(none, mem, hs, G + 1, kSiteRectGrad, 6, 4, 12, kVerts, hc);   // a known 2D site with no scope
  OnOp19(none, mem, hs, G + 1, 0x82123456u, 6, 4, 12, kVerts, hc);     // not a 2D site: nothing
  hs.DrainHoles(G, holes);
  uint32_t n3758 = 0, n8bec = 0, n3784 = 0, total = 0;
  for (const auto& h : holes) total += h.count, n3758 += h.site == 0x82BC3758u ? h.count : 0, n8bec += h.site == kSiteGlyphM ? h.count : 0, n3784 += h.site == kSiteGlyph ? h.count : 0;
  std::vector<Store::Hole> later;
  hs.DrainHoles(G + 1, later);
  Check(holes.size() == 3 && total == 3 && n3758 == 1 && n8bec == 1 && n3784 == 1 && holes[0].frame == G && later.size() == 1 && later[0].site == kSiteRectGrad && later[0].frame == G + 1 && hs.HoleCount() == 0, "GLYPH HOLES",
        "draws seen but not recorded are counted per frame and per site (the per-part site 0x82BC3758 included; a wrong shape; a gradation rect with no scope); a frame's holes drain with that frame; a non-2D site is not a hole");
}

// ---- [GLYPH BLOCKS] ------------------------------------------------------------------------------------------------------------------------------
static void BlockChecks() {
  using namespace q2d;
  const Quad2D g = GoodGlyph(1);
  Blocks b;
  const Refuse rg = BlocksOf(g, b);
  float vs[36] = {}, fs[12] = {};
  vs[0] = kScaleX, vs[1] = kScaleY, vs[3] = 0.5f;
  const float uv[4][2] = {{0.25f, 0.125f}, {0.375f, 0.125f}, {0.375f, 0.25f}, {0.25f, 0.25f}};
  const float pos[4][3] = {{100, 50, 0.5f}, {116, 50, 0.5f}, {116, 82, 0.5f}, {100, 82, 0.5f}};
  for (int i = 0; i < 4; ++i) {
    for (int c = 0; c < 3; ++c) vs[4 + 4 * i + c] = pos[i][c];
    vs[20 + 4 * i] = uv[i][0], vs[20 + 4 * i + 1] = uv[i][1];
  }
  fs[0] = 1.f, fs[1] = 0.5f, fs[2] = 0.25f, fs[3] = 0.75f, fs[4] = 1.f, fs[5] = 0.f, fs[6] = 1.f, fs[7] = 0.f, fs[8] = 0.f, fs[9] = 0.f, fs[10] = 0.f, fs[11] = 0.4f;
  Check(rg == Refuse::None && b.program == Program::Glyph && b.vs_bytes == 144 && b.fs_bytes == 48 && std::memcmp(b.vs, vs, 144) == 0 && std::memcmp(b.fs, fs, 48) == 0, "GLYPH BLOCKS",
        "a glyph (a strip, prim 6): UnitScale + corners in PERIMETER order + uv, Color, AlphaMix, then the Selector (0, 0, 0, 0.4) - equal to the written-out layout; a NaN z / w of c0 does not matter");
  Quad2D nosel = GoodGlyph(2);
  nosel.have &= ~uint32_t(kQhChannel);
  Quad2D prim5 = GoodGlyph(3);
  prim5.prim = 5;   // a fan (Xbox 360 D3DPT_TRIANGLEFAN): not the glyph program's primitive
  Quad2D quadlist = GoodGlyph(4);
  quadlist.prim = 13;   // a quad list: the vertices are already in perimeter order, taken as they are
  Blocks ql;
  BlocksOf(quadlist, ql);
  float ql_first[4] = {ql.vs[4], ql.vs[5], ql.vs[8], ql.vs[9]};   // corner 0 = vertex 0, corner 1 = vertex 1
  Quad2D inf = GoodGlyph(5);
  inf.hsl[1] = NAN;
  Blocks x;
  Check(BlocksOf(nosel, x) == Refuse::MissingConstants && BlocksOf(prim5, x) == Refuse::Prim && BlocksOf(quadlist, x) == Refuse::None && BlocksOf(inf, x) == Refuse::NotFinite && ql_first[0] == 100 && ql_first[1] == 50 && ql_first[2] == 116 && ql_first[3] == 50 && ql.vs[12] == 100 && ql.vs[13] == 82 && ql.vs[16] == 116 && ql.vs[17] == 82, "GLYPH BLOCKS",
        "no selector, a non-finite selector and a fan / unknown primitive are each refused by their own reason; a quad list takes its vertices as they are (the strip's go in perimeter order)");
}

// ---- [GLYPH DRAW] --------------------------------------------------------------------------------------------------------------------------------
static void DrawChecks() {
  using namespace q2d;
  rdr::FrameBuilder fb;
  fb.Begin(1);
  Programs programs;
  programs.rect = 11, programs.movie = 22, programs.layout = 3, programs.sprite = 33, programs.sprite_hsl = 44, programs.glyph = 55;
  Context ctx;
  ctx.width = 1920, ctx.height = 1080;
  ctx.picture = [](const Quad2D& row) {
    rdr::TextureSlot t;
    t.image = row.tex_key[0] == kKey ? rdr::ImageId(900) : row.tex_key[0] == kKey + 1 ? rdr::ImageId(901) : rdr::kNoImage;
    return t;
  };
  Quad2D a = GoodGlyph(1);
  Quad2D b = GoodGlyph(2);  b.tex_key[0] = kKey + 1;  b.blend = kQbAdd;  b.have |= kQhAlphaTest;  b.alpha_func = 6;  b.alpha_ref = 1.f / 255.f;
  Quad2D nopic = GoodGlyph(3);  nopic.tex_key[0] = 0x777;
  Quad2D unknown = GoodGlyph(4);  unknown.blend = kQbUnknown;
  std::vector<Quad2D> rows = {a, b, nopic, unknown};
  const Report rep = Append(fb, programs, ctx, rows.data(), rows.size());
  const rdr::Frame frame = fb.Finish();
  Check(rep.drawn == 2 && rep.glyph == 2 && rep.sprite == 0 && rep.refused[uint8_t(Refuse::Picture)] == 1 && rep.refused[uint8_t(Refuse::Blend)] == 1 && frame.draws.count == 2, "GLYPH DRAW",
        "four glyph rows: two draws; the unknown sheet key and the unknown blend are refused, each by its own reason");
  if (frame.draws.count == 2) {
    const rdr::DrawState& s0 = frame.states[frame.draws[0].state];
    const rdr::DrawState& s1 = frame.states[frame.draws[1].state];
    Check(s0.program == 55 && s1.program == 55 && s0.blend[0].enable == 1 && s0.blend[0].dst_color == 7 && s1.blend[0].dst_color == 1 && s0.depth_test == 0, "GLYPH DRAW", "the glyph program (55), blend from the row (alpha, additive), no depth");
    const rdr::TextureSet& t0 = frame.texture_sets[frame.draws[0].textures];
    const rdr::TextureSet& t1 = frame.texture_sets[frame.draws[1].textures];
    Check(t0.slots[0].image == 900 && t1.slots[0].image == 901, "GLYPH DRAW", "slot 0 is the sheet the row's KEY resolves to (900 / 901)");
    Check(frame.dynamics[frame.draws[1].dynamic].system[9] == 6.f && frame.dynamics[frame.draws[0].dynamic].system[9] == 7.f, "GLYPH DRAW", "the alpha test is the row's (greater-or-equal) or 'always'");
    Blocks want;
    BlocksOf(a, want);
    const rdr::DrawOp& d0 = frame.draws[0];
    Check(std::memcmp(frame.constants.data + d0.vs_constants, want.vs, want.vs_bytes) == 0 && std::memcmp(frame.constants.data + d0.fs_constants, want.fs, want.fs_bytes) == 0, "GLYPH DRAW", "the constants pushed for the glyph draw are the row's blocks, selector included, byte for byte");
  } else {
    Check(false, "GLYPH DRAW", "two draws were made");
  }
}

int main(int argc, char** argv) {
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
#ifdef RDR_Q2D_PLANTS
  if (plant == "glyph_no_selector") q2d::g_plant = 13;
  else if (plant == "glyph_stale_regs") q2d::g_plant = 14;
  else if (plant == "glyph_key_off") q2d::g_plant = 15;
  else if (plant == "glyph_blocks_sel") q2d::g_plant = 16;
  else if (plant == "glyph_prim_any") q2d::g_plant = 17;
  else if (plant == "glyph_stamp_ignored") q2d::g_plant = 18;
  else if (plant == "hole_dropped") q2d::g_plant = 19;
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
  StaleAndHoleChecks();
  BlockChecks();
  DrawChecks();
  if (!plant.empty()) {
    const char* want = (plant == "glyph_blocks_sel" || plant == "glyph_prim_any") ? "[GLYPH BLOCKS]" : plant == "glyph_stamp_ignored" ? "[GLYPH STALE]" : plant == "hole_dropped" ? "[GLYPH HOLES]" : "[GLYPH TAP]";
    const bool caught = g_failed.find(want) != std::string::npos;
    std::printf("%s planted %s: %s (failed: %s)\n", caught ? "PASS" : "FAIL", plant.c_str(), caught ? (std::string("caught by its named check ") + want).c_str() : (std::string("its named check ") + want + " did NOT fail").c_str(), g_failed.c_str());
    return caught ? 0 : 1;
  }
  std::printf("%s: i3_unit: %d checks, %d failed\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
