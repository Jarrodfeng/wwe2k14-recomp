// Lane A8 (Session 105; docs\research\s105\A8.md): the offline unit proof of T_RectColor as a 2D row: sub_82BC3F30's draw (op-19 site 0x82BC4404), the 30 Years narration caption's dark box (and every other vertex-colour
// rect). 0 game runs, no GPU, no game file. Before this lane the site was a Leaf2D hole: the box never drew in the stream-free host and every frame it was up counted in frames_2d_dropped.
//
//   a8_unit                 every check; exit 0 only when all pass
//   a8_unit --plant NAME    exit 0 only when the check that sees the plant FAILS (plants 65 rectcolor_leaf_hole, 66 vcol_alpha_dropped: src\renderer_scene\quads2d_plants.h)
//
//   [A8 SITE]    0x82BC4404 is a Row site of kind kQkRectColor (scope kind the same, stride 28); the OLD classification (plant 65) is a Leaf2D hole
//   [A8 TAP]     the leaf's uploads in PRODUCTION order (op-21 c0 at +15C, c28 + c29..c32 through sub_82BC2A30, c1 at +200, then the op-19: r4 13, r5 4, r6 28) make ONE row with the four corner colours, c0 / c1 / c28 from the scope;
//                a colour that is not a whole byte / 255, a primitive other than 13, a stride that is not 28 are holes (counted), never rows
//   [A8 BLOCKS]  BlocksOf draws it with the vertex-colour program: Col[i] = the corner's rgba (alpha 30 / 180 of the caption box), Pos = the corners as drawn; a matrix-placed row takes the matrix block (the box is a widget: its c28 may be 1)
//   [A8 RECORD]  the row is valid for the record (Quad2DRowProblem), a corner byte out of range makes it bad
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/quads2d_blocks.h"
#include "../../src/renderer_scene/quads2d_core.h"
#include "../../src/renderer/frame_builder.h"
#include "../../src/renderer_scene/quads2d_draw.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_quad2d.h"
#include "sc_support.h"

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

static const uint32_t kObject = 0x40001000u, kVerts = 0x40002000u, kPay = 0x40003000u;
static const uint32_t kC0 = kPay, kC1 = kPay + 16, kC28 = kPay + 32, kMat = kPay + 48;   // c29..c32 = four registers (64 bytes) at kMat
static const uint32_t F = 55;

// The caption box's corners as the leaf writes them (listing sub_82BC3F30 0x82BC4170..0x82BC4404; docs\research\s105\A8.md "Leaf format"): vertex order 0 2 3 1 of the object's corners, i.e. the PERIMETER of a quad list
// (top-left, top-right, bottom-right, bottom-left: corner 1 of the object = (0, h), sub_82BCA190). Corner colours of the box (sub_823450E8: black, alpha 30 on corners 0 and 2, 180 on corners 1 and 3): the vertices (0, 2, 3, 1) = alpha 30, 30, 180, 180.
// x y z, then the corner's r g b a ints; the leaf stores float(int) * float(1/255).
struct VertSpec {
  float x, y, z;
  int r, g, b, a;
};
static const VertSpec kBox[4] = {{100, 400, 0.0f, 0, 0, 0, 30}, {900, 400, 0.0f, 0, 0, 0, 30}, {900, 500, 0.0f, 0, 0, 0, 180}, {100, 500, 0.0f, 0, 0, 0, 180}};

static void FillVerts(FakeMem& m, const VertSpec* v, float colour_bias = 0.0f) {
  for (int i = 0; i < 4; ++i) {
    const uint32_t at = kVerts + 28u * i;
    m.PutF(at + 0, v[i].x), m.PutF(at + 4, v[i].y), m.PutF(at + 8, v[i].z);
    m.PutF(at + 12, float(v[i].r) * kInv255 + colour_bias), m.PutF(at + 16, float(v[i].g) * kInv255), m.PutF(at + 20, float(v[i].b) * kInv255), m.PutF(at + 24, float(v[i].a) * kInv255);
  }
}
static void FillConstants(FakeMem& m, bool matrix) {
  const float c0[4] = {2.0f / 1280, 2.0f / 720, 1280, 720}, c1[4] = {1.0f, 1.0f, 1.0f, 1.0f}, c28[4] = {matrix ? 1.0f : 0.0f, 0, 0, 0};
  for (int k = 0; k < 4; ++k) m.PutF(kC0 + 4 * k, c0[k]), m.PutF(kC1 + 4 * k, c1[k]), m.PutF(kC28 + 4 * k, c28[k]);
  for (int k = 0; k < 16; ++k) m.PutF(kMat + 4 * k, 0.25f * float(k + 1));   // a recognisable matrix: register 29 + r = 0.25 * (4 r + 1 .. 4 r + 4)
}

struct Run {
  std::vector<Quad2D> rows;
  std::vector<Store::Hole> holes;
  Counters c;
};
// One leaf call as sub_82BC3F30 makes it: the scope opens (sub_82BC3F30's hook), c0 (+15C: op 21, r4 0, 1 register), sub_82BC2A30's c28 and, when the object is matrix-placed (+280 != 0), c29..c32 as ONE four-register upload
// (r4 29), c1 (+200: op 21, r4 1), then the op-19 (r4 13, r5 4, r6 28, r7 the vertices), then the scope closes (the leaf returns).
static void LeafCall(Run& r, FakeMem& m, bool matrix, uint32_t prim, uint32_t stride, uint32_t site = kSiteRectColor) {
  Store store;
  Scope s;
  OpenScope(s, kQkRectColor, kObject);
  OnOp21(s, m, 0, 1, kC0, r.c);
  OnOp21(s, m, 28, 1, kC28, r.c);
  if (matrix) OnOp21(s, m, 29, 4, kMat, r.c);
  OnOp21(s, m, 1, 1, kC1, r.c);
  OnOp19(s, m, store, F, site, prim, 4, stride, kVerts, r.c);
  CloseScope(s, store, F, r.c);
  store.Drain(F, r.rows);
  store.DrainHoles(F, r.holes);
}

static Quad2D Good(Run& r) {
  FakeMem m;
  FillVerts(m, kBox);
  FillConstants(m, false);
  LeafCall(r, m, false, kSiteRectColorPrim, 28);
  return r.rows.empty() ? Quad2D{} : r.rows[0];
}

static void Sites() {
  Check(ClassOfSite(kSiteRectColor) == SiteClass::Row && KindOfSite(kSiteRectColor) == kQkRectColor && ScopeKindOfSite(kSiteRectColor) == kQkRectColor && StrideOfKind(kQkRectColor) == 28u && SiteOf(kSiteRectColor) &&
            SiteOf(kSiteRectColor)->fn == 0x82BC3F30u,
        "A8 SITE", "0x82BC4404 (sub_82BC3F30, T_RectColor) is a Row site of kind kQkRectColor, scope kind the same, vertex stride 28 (the old classification, a Leaf2D hole, is gone)");
}

static void Tap() {
  Run r;
  const Quad2D q = Good(r);
  const bool one = r.rows.size() == 1 && r.holes.empty();
  Check(one && q.kind == kQkRectColor && q.site == kSiteRectColor && q.prim == 13 && q.nverts == 4 && q.stride_bytes == 28 && q.object == kObject && (q.have & kQhVerts), "A8 TAP",
        "the leaf's uploads in production order + its op-19 make ONE kQkRectColor row (site 0x82BC4404, prim 13, 4 vertices, stride 28), no hole");
  constexpr uint32_t kNeed = kQhPosScale | kQhColour | kQhProj;
  Check(one && (q.have & kNeed) == kNeed && q.pos_scale[0] == 2.0f / 1280 && q.pos_scale[1] == 2.0f / 720 && q.color[0] == 1.0f && q.color[3] == 1.0f && q.use_proj == 0.0f, "A8 TAP", "c0 / c1 / c28 are the scope's");
  bool pos = one, col = one;
  static const int alpha[4] = {30, 30, 180, 180};
  for (int v = 0; one && v < 4; ++v) {
    pos = pos && q.verts[v][0] == kBox[v].x && q.verts[v][1] == kBox[v].y && q.verts[v][2] == kBox[v].z;
    float c[4];
    UnpackCorner(q.verts[v][3], q.verts[v][4], c);
    col = col && c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == float(alpha[v]) * kInv255 && q.verts[v][4] == float(alpha[v]);
  }
  Check(pos && col, "A8 TAP", "the four corners keep their position and their colour: black, alpha 30 / 30 / 180 / 180 in the leaf's vertex order (the byte values, the leaf's own float(int) * 1/255 back)");
  // a coloured corner survives the pack (r | g << 8 | b << 16 with every byte different)
  {
    VertSpec v[4] = {{0, 0, 0, 255, 128, 1, 7}, {1, 0, 0, 17, 255, 200, 255}, {1, 1, 0, 0, 0, 255, 0}, {0, 1, 0, 3, 4, 5, 6}};
    FakeMem m;
    FillVerts(m, v);
    FillConstants(m, false);
    Run rr;
    LeafCall(rr, m, false, 13, 28);
    bool ok = rr.rows.size() == 1;
    for (int i = 0; ok && i < 4; ++i) {
      float c[4];
      UnpackCorner(rr.rows[0].verts[i][3], rr.rows[0].verts[i][4], c);
      ok = c[0] == float(v[i].r) * kInv255 && c[1] == float(v[i].g) * kInv255 && c[2] == float(v[i].b) * kInv255 && c[3] == float(v[i].a) * kInv255;
    }
    Check(ok, "A8 TAP", "every channel of every corner round-trips exactly (255 128 1 7 / 17 255 200 255 / 0 0 255 0 / 3 4 5 6)");
  }
  // fail closed
  Run notbyte, prim6, stride20, noscope;
  {
    FakeMem m;
    FillVerts(m, kBox, 0.5f * kInv255);   // the red channel half a byte off: not the game's integer colour / 255
    FillConstants(m, false);
    LeafCall(notbyte, m, false, 13, 28);
  }
  {
    FakeMem m;
    FillVerts(m, kBox);
    FillConstants(m, false);
    LeafCall(prim6, m, false, 6, 28);
    LeafCall(stride20, m, false, 13, 20);
    Store store;
    Scope closed;
    OnOp19(closed, m, store, F, kSiteRectColor, 13, 4, 28, kVerts, noscope.c);   // no scope open: a hole like every Row site
    store.DrainHoles(F, noscope.holes);
  }
  Check(notbyte.rows.empty() && notbyte.holes.size() == 1 && notbyte.c.bad_corner == 1, "A8 TAP", "a corner colour that is not a whole byte / 255: no row, one hole, counted (bad_corner)");
  Check(prim6.rows.empty() && prim6.holes.size() == 1 && stride20.rows.empty() && stride20.holes.size() == 1 && noscope.rows.empty() && noscope.holes.size() == 1, "A8 TAP",
        "a primitive other than the quad list (6), a stride of 20, or the site drawn with no scope open: no row, one hole each");
  // the OLD classification: the same draw is a hole (the planted twin makes this whole check fail)
  Check(r.holes.empty() && r.rows.size() == 1, "A8 TAP", "with the old classification (a Leaf2D hole, plant rectcolor_leaf_hole) the box would be a hole, not a row");
}

static void BlocksChecks() {
  Run r;
  const Quad2D q = Good(r);
  Blocks b;
  const bool ok = r.rows.size() == 1 && BlocksOf(q, b) == Refuse::None;
  Check(ok && b.program == Program::RectColor && !b.proj && b.vs_bytes == 36 * 4 && b.fs_bytes == 8 * 4, "A8 BLOCKS", "the pixel-placed row draws with the vertex-colour program, 36 floats of vertex block, 8 of fragment block");
  static const int alpha[4] = {30, 30, 180, 180};
  bool cols = ok, pos = ok;
  for (int i = 0; ok && i < 4; ++i) {
    cols = cols && b.vs[20 + 4 * i + 0] == 0.0f && b.vs[20 + 4 * i + 1] == 0.0f && b.vs[20 + 4 * i + 2] == 0.0f && b.vs[20 + 4 * i + 3] == float(alpha[i]) * kInv255;
    pos = pos && b.vs[4 + 4 * i] == kBox[i].x && b.vs[5 + 4 * i] == kBox[i].y && b.vs[6 + 4 * i] == kBox[i].z;
  }
  Check(cols && pos, "A8 BLOCKS", "Col[i] = the corner's rgba (alpha 30 / 255, 30 / 255, 180 / 255, 180 / 255: the box's gradient), Pos[i] = the corner as drawn (quad list, perimeter order)");
  Check(ok && b.vs[0] == 2.0f / 1280 && b.vs[1] == 2.0f / 720 && b.vs[3] == 0.5f && b.fs[0] == 1.0f && b.fs[3] == 1.0f && b.fs[4] == 1.0f && b.fs[5] == 0.0f, "A8 BLOCKS", "UnitScale = c0, the half unit, Color = c1, AlphaMix (1, 0)");
  // matrix-placed
  FakeMem m;
  FillVerts(m, kBox);
  FillConstants(m, true);
  Run pr;
  LeafCall(pr, m, true, 13, 28);
  Blocks bp;
  const bool pok = pr.rows.size() == 1 && (pr.rows[0].have & kQhMatrix) && pr.rows[0].use_proj == 1.0f && BlocksOf(pr.rows[0], bp) == Refuse::None;
  bool mat = pok && bp.proj && bp.program == Program::RectColor && bp.vs_bytes == 52 * 4;
  for (int i = 0; pok && i < 16; ++i) mat = mat && bp.vs[36 + i] == 0.25f * float(i + 1);
  Check(mat, "A8 BLOCKS", "a matrix-placed row (c28 = 1, c29..c32 uploaded) takes the matrix block (52 floats, Mat[k] = register 29 + k as uploaded) and keeps its corner colours");
  // a matrix-placed row without its matrix is refused (a hole), never guessed
  FakeMem m2;
  FillVerts(m2, kBox);
  FillConstants(m2, true);
  Run nm;
  LeafCall(nm, m2, false, 13, 28);   // c28 = 1 uploaded, the matrix not
  Blocks bn;
  nm.rows.empty() ? (void)0 : (void)(nm.rows[0].use_proj = 1.0f);
  Check(nm.rows.size() == 1 && BlocksOf(nm.rows[0], bn) == Refuse::Projection, "A8 BLOCKS", "c28 = 1 with no matrix block: refused (projection), never drawn guessed");
  // a corner byte out of range is refused by the drawer
  Quad2D bad = q;
  bad.verts[2][4] = 300.0f;
  Blocks bb;
  Check(ok && BlocksOf(bad, bb) == Refuse::NotFinite, "A8 BLOCKS", "a packed corner alpha of 300 is refused");
}

// S105 A8 review F1: the leaf passes the quad list (13); a row with any other primitive is refused by the drawer and by the record check (a strip would be laid out wrong).
static void PrimChecks() {
  Run r;
  Quad2D q = Good(r);
  Quad2D strip = q;
  strip.prim = 6;
  Blocks b;
  Check(!r.rows.empty() && BlocksOf(q, b) == Refuse::None && BlocksOf(strip, b) == Refuse::Prim, "A8 PRIM", "BlocksOf refuses a T_RectColor row whose primitive is the strip (6): only the quad list (13) is drawn");
  ValidSC(q);
  ValidSC(strip);
  Check(Quad2DRowProblem(q).empty() && !Quad2DRowProblem(strip).empty(), "A8 PRIM", "Quad2DRowProblem refuses the same row (primitive 6), accepts the quad list");
}

// S105 A8 review F3: through the drawer (q2d::Append / Check): the pixel-placed row is the rect_color program, the matrix-placed row rect_color_proj, with their blocks pushed.
static void DrawChecks() {
  rdr::FrameBuilder fb;
  fb.Begin(1);
  Programs programs;
  programs.rect = 11, programs.rect_proj = 12, programs.rect_color = 41, programs.rect_color_proj = 42, programs.layout = 3;
  Context ctx;
  ctx.width = 1920, ctx.height = 1080;
  Run r0, r1;
  Quad2D pix = Good(r0);
  FakeMem m;
  FillVerts(m, kBox);
  FillConstants(m, true);
  LeafCall(r1, m, true, 13, 28);
  Quad2D mat = r1.rows.empty() ? Quad2D{} : r1.rows[0];
  pix.blend = mat.blend = kQbAlpha;
  std::vector<Quad2D> rows = {pix, mat};
  const Report chk = Check(ctx, rows.data(), rows.size());
  const Report rep = Append(fb, programs, ctx, rows.data(), rows.size());
  const rdr::Frame frame = fb.Finish();
  Check(chk.drawn == 2 && chk.rect == 2 && rep.drawn == 2 && rep.rect == 2 && rep.refused_total() == 0 && frame.draws.count == 2, "A8 DRAW", "Check and Append take both T_RectColor rows (counted as rects), none refused");
  if (frame.draws.count == 2) {
    const rdr::DrawState& s0 = frame.states[frame.draws[0].state];
    const rdr::DrawState& s1 = frame.states[frame.draws[1].state];
    Check(s0.program == 41 && s1.program == 42 && s0.blend[0].enable == 1 && s0.blend[0].src_color == 6 && s0.blend[0].dst_color == 7 && frame.draws[0].count == 6, "A8 DRAW",
          "the pixel-placed row (c28 = 0) is program rect_color (41), the matrix-placed row (c28 = 1) rect_color_proj (42), alpha blend, 6 vertices");
    Blocks want0, want1;
    BlocksOf(pix, want0);
    BlocksOf(mat, want1);
    Check(std::memcmp(frame.constants.data + frame.draws[0].vs_constants, want0.vs, want0.vs_bytes) == 0 && std::memcmp(frame.constants.data + frame.draws[1].vs_constants, want1.vs, want1.vs_bytes) == 0 &&
              std::memcmp(frame.constants.data + frame.draws[0].fs_constants, want0.fs, want0.fs_bytes) == 0,
          "A8 DRAW", "the constants pushed are the rows' blocks (the corner colours and, for the matrix row, c29..c32)");
  } else {
    Check(false, "A8 DRAW", "two draws were made");
    Check(false, "A8 DRAW", "two draws were made");
  }
}

static void Record() {
  Run r;
  Quad2D q = Good(r);
  ValidSC(q);
  q.blend = kQbAlpha, q.have |= kQhBlend;
  Check(!r.rows.empty() && Quad2DRowProblem(q).empty(), "A8 RECORD", "the T_RectColor row is a valid 2D record row (kind known, corner bytes in range)");
  Quad2D bad = q;
  bad.verts[1][3] = 17000000.0f;   // above 2^24 - 1: not a packed r | g << 8 | b << 16
  Quad2D bad2 = q;
  bad2.verts[3][4] = 0.5f;   // not a whole byte
  Check(!Quad2DRowProblem(bad).empty() && !Quad2DRowProblem(bad2).empty(), "A8 RECORD", "a packed corner outside 0..255 / not whole is a bad row (dropped on its own, the frame then has a hole)");
}

static void All() { Sites(), Tap(), BlocksChecks(), PrimChecks(), DrawChecks(), Record(); }

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") {
      const std::string p = argv[i + 1];
      const std::map<std::string, int> plants = {{"rectcolor_leaf_hole", 65}, {"vcol_alpha_dropped", 66}, {"vcol_prim_any", 67}};
      const std::map<std::string, std::string> expect = {{"rectcolor_leaf_hole", "[A8 SITE]"}, {"vcol_alpha_dropped", "[A8 BLOCKS]"}, {"vcol_prim_any", "[A8 PRIM]"}};
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
