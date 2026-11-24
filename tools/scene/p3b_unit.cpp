// Lane P3b (Session 103, landing 2, docs\renderer\LANDING2_PLAN.md section 5 row P3b): the offline unit proof of the menu leaves that became 2D rows: sub_82BC8180's plain T_Sprite (technique 0x8206AA0C) (0x82BC8BEC) and
// sub_82BC4450's single flat T_Rect (0x82BC484C). 0 game runs, no GPU, no game file. The picture proof against the stream is tools\scene\p3b_check.py (fview rig).
//
//   p3b_unit                 every check; exit 0 only when all pass
//   p3b_unit --plant NAME    exit 0 only when the check that sees the plant FAILS (plants 30 m_c4_ignored, 31 m_variant_ignored, 32 strip_order_ignored: src\renderer_scene\quads2d_plants.h)
//
//   [P3B SITES]   the site table: 0x82BC484C and 0x82BC8BEC are Row sites (rect / sprite), 0x82BC47C8, 0x82BC8B68, 0x82BC4404, 0x82BC32B8 stay 2D leaves that make no row (holes)
//   [P3B M SITE]  a plain T_Sprite (technique 0x8206AA0C) in the variant leaf's (glyph-kind) scope makes ONE sprite row (site 0x82BC8BEC, strip, key at object + 416, c0 / c1 / c28 from the scope, 4 vertices with uv); the same draw with a
//                 channel selector (c4), a hue shift (c2) or an offsets block (c3) uploaded in the scope, a stride that is not 20, or a primitive that is not 6 is a hole (counted), never a row   (plants 30, 31)
//   [P3B RECT]    the flat T_Rect of sub_82BC4450 in a rect scope: ONE rect row (strip word 6), vertex stride 12; stride 28 (the vertex-colour format) is a hole; the band loop's site 0x82BC47C8 is a hole
//   [P3B BLOCKS]  BlocksOf takes a strip's four vertices in the perimeter order (rect and sprite): corner 2 of the draw is corner 3 of the quad   (plant 32); a hue-shifted M row is refused
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/quads2d_blocks.h"
#include "../../src/renderer_scene/rows_quad2d.h"
#include "../../src/renderer_scene/quads2d_core.h"
#include "sc_support.h"   // S103 SC

using namespace rdr_scene;
using namespace rdr_scene::q2d;

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

static const uint32_t kObject = 0x40001000u, kVerts = 0x40002000u, kPay = 0x40003000u, kKey = 0x00ABCDEFu;
static const uint32_t kC0 = kPay, kC1 = kPay + 16, kC28 = kPay + 32, kC4 = kPay + 48, kC2 = kPay + 64, kC3 = kPay + 80;
static const uint32_t F = 55;

// Vertices of a strip: x y z (u v), the draw's order 0 1 2 3 (a Z): corner 2 is the BOTTOM-LEFT, corner 3 the bottom-right.
static const float kV[4][5] = {{100, 50, 0.1f, 0.0f, 0.0f}, {300, 50, 0.1f, 1.0f, 0.0f}, {100, 150, 0.1f, 0.0f, 1.0f}, {300, 150, 0.1f, 1.0f, 1.0f}};

static void Fill(FakeMem& m, bool sprite, uint32_t stride_floats) {
  for (int v = 0; v < 4; ++v)
    for (uint32_t k = 0; k < stride_floats; ++k) m.PutF(kVerts + 4 * stride_floats * v + 4 * k, k < (sprite ? 5u : 3u) ? kV[v][k] : 0.0f);
  m.Put(kObject + kGlyphKeyOffset, kKey);           // the variant leaf's handle
  m.Put(kObject + kSpriteKeyOffset, 0x11111111u);   // a decoy at the sprite leaf's offset (reading the wrong one shows)
  const float c0[4] = {2.0f / 1280, 2.0f / 720, 0, 0}, c1[4] = {1, 0.5f, 0.25f, 0.75f}, c28[4] = {0, 0, 0, 0}, c4[4] = {0, 0.25f, 0.5f, 0.75f}, c2[4] = {0.1f, 0.2f, 0.3f, 0};
  for (int k = 0; k < 4; ++k) m.PutF(kC0 + 4 * k, c0[k]), m.PutF(kC1 + 4 * k, c1[k]), m.PutF(kC28 + 4 * k, c28[k]), m.PutF(kC4 + 4 * k, c4[k]), m.PutF(kC2 + 4 * k, c2[k]);
  for (int k = 0; k < 32; ++k) m.PutF(kC3 + 4 * k, 0.125f * float(k));
}

struct Run {
  std::vector<Quad2D> rows;
  std::vector<Store::Hole> holes;
  Counters c;
};
// One draw through the tap core: `scope_kind` = the scope's kind (glyph for the variant leaf, rect for sub_82BC4450, sprite for sub_82BC4890), the constants uploaded as the leaf does, then `extra`, then the op-19.
template <class Extra>
static void DrawInto(Run& r, uint8_t scope_kind, uint32_t site, uint32_t prim, uint32_t nverts, uint32_t stride, Extra&& extra) {
  FakeMem m;
  Fill(m, scope_kind != kQkRect, stride / 4);
  if (scope_kind == kQkSprite) m.Put(kObject + kSpriteKeyOffset, kKey);
  Store store;
  Scope s;
  OpenScope(s, scope_kind, kObject);
  OnOp21(s, m, 0, 1, kC0, r.c);
  OnOp21(s, m, 1, 1, kC1, r.c);
  OnOp21(s, m, 28, 1, kC28, r.c);
  extra(s, m, r.c);
  OnOp19(s, m, store, F, site, prim, nverts, stride, kVerts, r.c);
  CloseScope(s, store, F, r.c);
  store.Drain(F, r.rows);
  store.DrainHoles(F, r.holes);
}
static void None(Scope&, const FakeMem&, Counters&) {}

static void Sites() {
  using q2d::SiteClass;
  Check(q2d::ClassOfSite(0x82BC484Cu) == SiteClass::Row && q2d::KindOfSite(0x82BC484Cu) == kQkRect && q2d::ClassOfSite(0x82BC8BECu) == SiteClass::Row && q2d::KindOfSite(0x82BC8BECu) == kQkSprite, "P3B SITES",
        "0x82BC484C is a rect Row site, 0x82BC8BEC a sprite Row site");
  bool leaves = true;
  for (uint32_t s : {0x82BC47C8u, 0x82BC8B68u, 0x82BC32B8u, 0x82BC5408u, 0x82BC3758u}) leaves = leaves && q2d::ClassOfSite(s) == SiteClass::Leaf2D && q2d::KindOfSite(s) == kQkNone;
  Check(leaves, "P3B SITES", "the band loops (0x82BC47C8, 0x82BC8B68), the textured rect (0x82BC32B8) and the other leaf paths stay 2D leaves that make no row (S105 A8: T_RectColor 0x82BC4404 is a Row site now, a8_unit)");
}

static void MSite() {
  Run r;
  DrawInto(r, kQkGlyph, q2d::kSiteGlyphM, 6, 4, 20, None);
  const bool one = r.rows.size() == 1 && r.holes.empty();
  Check(one && r.rows[0].kind == kQkSprite && r.rows[0].site == q2d::kSiteGlyphM && r.rows[0].prim == 6 && r.rows[0].tex_key[0] == kKey && (r.rows[0].have & kQhPlanes) && r.rows[0].nverts == 4, "P3B M SITE",
        "a plain T_Sprite (technique 0x8206AA0C) in the variant leaf's scope makes ONE sprite row (site, prim 6, the picture key from object + 416, not the sprite's +400)");
  bool verts = one;
  for (int v = 0; one && v < 4; ++v)
    for (int k = 0; k < 5; ++k) verts = verts && r.rows[0].verts[v][k] == kV[v][k];
  constexpr uint32_t kNeed = kQhPosScale | kQhColour | kQhProj | kQhVerts;
  Check(verts && (r.rows[0].have & kNeed) == kNeed && r.rows[0].color[1] == 0.5f && r.rows[0].pos_scale[0] == 2.0f / 1280 && r.rows[0].use_proj == 0.0f, "P3B M SITE",
        "its four vertices with uv, c0 / c1 / c28 are the scope's (the sprite constants, no register file needed)");
  Blocks b;
  Check(one && BlocksOf(r.rows[0], b) == Refuse::None && b.program == Program::Sprite, "P3B M SITE", "the drawer draws it with the plain sprite program");
  // the variants: a selector, a hue shift, an offsets block, a wrong stride / primitive: holes
  Run c4, hsl, c3, st, pr, g;
  DrawInto(c4, kQkGlyph, q2d::kSiteGlyphM, 6, 4, 20, [](Scope& s, const FakeMem& m, Counters& c) { OnOp21(s, m, 4, 1, kC4, c); });
  Check(c4.rows.empty() && c4.holes.size() == 1 && c4.holes[0].site == q2d::kSiteGlyphM && c4.c.m_variant == 1, "P3B M SITE", "a channel selector (c4) uploaded in the scope: no row, one hole (the glyph-programmed variant has no capture)");
  DrawInto(hsl, kQkGlyph, q2d::kSiteGlyphM, 6, 4, 20, [](Scope& s, const FakeMem& m, Counters& c) { OnOp28(s, m, 2, 1, kC2, c); });
  DrawInto(c3, kQkGlyph, q2d::kSiteGlyphM, 6, 4, 20, [](Scope& s, const FakeMem& m, Counters& c) { OnOp28(s, m, 3, 8, kC3, c); });
  Check(hsl.rows.empty() && hsl.holes.size() == 1 && c3.rows.empty() && c3.holes.size() == 1, "P3B M SITE", "a hue shift (c2 x 1) or an offsets block (c3 x 8) uploaded in the scope: no row, one hole each");
  DrawInto(st, kQkGlyph, q2d::kSiteGlyphM, 6, 4, 36, None);   // vertex format 7 (colour): 36 bytes
  DrawInto(pr, kQkGlyph, q2d::kSiteGlyphM, 13, 4, 20, None);
  Check(st.rows.empty() && st.holes.size() == 1 && pr.rows.empty() && pr.holes.size() == 1, "P3B M SITE", "a stride of 36 (the vertex-colour format) or a primitive other than the strip: no row, one hole each");
  // S103 P3c review F2: the site drawn inside a scope of another kind (a sprite-kind scope, as sub_82BC4890 opens) is a hole, never a plain sprite row
  Run wrong_scope, wrong_scope_rect;
  DrawInto(wrong_scope, kQkSprite, q2d::kSiteGlyphM, 6, 4, 20, None);
  DrawInto(wrong_scope_rect, kQkRect, q2d::kSiteGlyphM, 6, 4, 20, None);
  Check(wrong_scope.rows.empty() && wrong_scope.holes.size() == 1 && wrong_scope_rect.rows.empty() && wrong_scope_rect.holes.size() == 1, "P3B M SITE", "0x82BC8BEC inside a sprite-kind or a rect-kind scope: no row, one hole (the scope kind must be the leaf's own glyph kind)");
  // S103 P3c review F3: ANY op-28 upload in the variant leaf's scope (any register) holds the draw back; the plain T_Sprite path uploads none
  Run any28;
  DrawInto(any28, kQkGlyph, q2d::kSiteGlyphM, 6, 4, 20, [](Scope& s, const FakeMem& m, Counters& c) { OnOp28(s, m, 9, 1, kC3, c); });
  Check(any28.rows.empty() && any28.holes.size() == 1 && any28.c.m_variant == 1, "P3B M SITE", "an op-28 upload of any register in the variant leaf's scope: no row, one hole (counted in m_variant)");
  // the glyph leaf (sub_82BC3518) in its own scope is untouched
  DrawInto(g, kQkGlyph, q2d::kSiteGlyph, 6, 4, 20, None);
  Check(g.rows.size() == 1 && g.rows[0].kind == kQkGlyph && g.rows[0].tex_key[0] == kKey, "P3B M SITE", "the glyph leaf's own draw (0x82BC3784) is still a glyph row");
}

static void RectSite() {
  Run r, vc, band;
  DrawInto(r, kQkRect, q2d::kSiteRectGrad, 6, 4, 12, None);
  Check(r.rows.size() == 1 && r.holes.empty() && r.rows[0].kind == kQkRect && r.rows[0].site == q2d::kSiteRectGrad && r.rows[0].prim == 6 && r.rows[0].verts[3][0] == kV[3][0] && r.rows[0].verts[3][1] == kV[3][1] && r.rows[0].color[3] == 0.75f, "P3B RECT",
        "the flat T_Rect of sub_82BC4450: ONE rect row (strip word 6, stride 12, the draw's four corners in call order, c1 from the scope)");
  DrawInto(vc, kQkRect, q2d::kSiteRectGrad, 6, 4, 28, None);
  DrawInto(band, kQkRect, 0x82BC47C8u, 6, 4, 20, None);
  Check(vc.rows.empty() && vc.holes.size() == 1 && band.rows.empty() && band.holes.size() == 1, "P3B RECT", "a vertex-colour / gradation vertex format (stride 28) and the band loop's site 0x82BC47C8: no row, one hole each");
  Blocks b;
  Check(r.rows.size() == 1 && BlocksOf(r.rows[0], b) == Refuse::None && b.program == Program::Rect, "P3B RECT", "the drawer draws it with the flat rect program");
}

static void BlocksChecks() {
  Run m, r, q;
  DrawInto(m, kQkGlyph, q2d::kSiteGlyphM, 6, 4, 20, None);
  DrawInto(r, kQkRect, q2d::kSiteRectGrad, 6, 4, 12, None);
  Blocks bm, br;
  const bool ok = m.rows.size() == 1 && r.rows.size() == 1 && BlocksOf(m.rows[0], bm) == Refuse::None && BlocksOf(r.rows[0], br) == Refuse::None;
  // perimeter order of a Z strip 0 1 2 3: 0 1 3 2 (x y z at vs[4 + 4 i], uv at vs[20 + 4 i])
  const int perim[4] = {0, 1, 3, 2};
  bool order = ok;
  for (int i = 0; ok && i < 4; ++i) {
    order = order && bm.vs[4 + 4 * i] == kV[perim[i]][0] && bm.vs[5 + 4 * i] == kV[perim[i]][1] && bm.vs[20 + 4 * i] == kV[perim[i]][3] && bm.vs[21 + 4 * i] == kV[perim[i]][4];
    order = order && br.vs[4 + 4 * i] == kV[perim[i]][0] && br.vs[5 + 4 * i] == kV[perim[i]][1];
  }
  Check(order, "P3B BLOCKS", "a strip row (prim 6) is taken in the perimeter order 0 1 3 2: the sprite's uv follow their corners, the rect's corners too");
  DrawInto(q, kQkSprite, q2d::kSiteSprite, 13, 4, 20, None);   // the plain sprite site: quad list, order as drawn
  Blocks bq;
  bool same = q.rows.size() == 1 && BlocksOf(q.rows[0], bq) == Refuse::None;
  for (int i = 0; same && i < 4; ++i) same = bq.vs[4 + 4 * i] == kV[i][0] && bq.vs[5 + 4 * i] == kV[i][1];
  Check(same, "P3B BLOCKS", "a quad-list row (the T_Sprite site, prim 13) keeps its order");
  Quad2D hue = m.rows.empty() ? Quad2D{} : m.rows[0];
  hue.have |= kQhHsl, hue.hsl[0] = 0.1f;
  Blocks bh;
  Check(!m.rows.empty() && BlocksOf(hue, bh) == Refuse::Variant, "P3B BLOCKS", "a hue-shifted row of the M site is refused (no capture shows that program)");
}

static void All() { Sites(), MSite(), RectSite(), BlocksChecks(); }

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") {
      const std::string p = argv[i + 1];
      const std::map<std::string, int> plants = {{"m_c4_ignored", 30}, {"m_variant_ignored", 31}, {"strip_order_ignored", 32}};
      const std::map<std::string, std::string> expect = {{"m_c4_ignored", "[P3B M SITE]"}, {"m_variant_ignored", "[P3B M SITE]"}, {"strip_order_ignored", "[P3B BLOCKS]"}};
      auto it = plants.find(p);
      if (it == plants.end()) return std::printf("unknown plant %s\n", p.c_str()), 2;
      q2d::g_plant = it->second;
      All();
      const bool caught = g_failed.find(expect.at(p)) != std::string::npos;
      std::printf("plant %s: %s (failed: %s)\n", p.c_str(), caught ? "CAUGHT" : "NOT CAUGHT", g_failed.empty() ? "-" : g_failed.c_str());
      return caught ? 0 : 1;
    }
  All();
  std::printf("%d checks, %d failed%s\n", g_checks, g_failures, g_failed.empty() ? "" : (" " + g_failed).c_str());
  return g_failures ? 1 : 0;
}
