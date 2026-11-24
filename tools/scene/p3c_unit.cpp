// Lane P3c (Session 103, landing 2; docs\renderer\P3B_MENU_LEAVES.md section 5): the offline unit proof of the MATRIX VARIANT of the 2D rows: the menu's text, M sprites and rects are placed by the object's matrix
// (c28 = 1, c29..c32 uploaded by sub_82BC2A30), not in pixel units. 0 game runs, no GPU, no game file. The picture proof against the stream is tools\scene\p3c_check.py (fview rig).
//
//   p3c_unit                 every check; exit 0 only when all pass
//   p3c_unit --plant NAME    exit 0 only when the check that sees the plant FAILS (plants 33 proj_not_taken, 34 proj_transposed, 35 proj_c28_ignored, 36 matrix_partial_kept, 37 glyph_matrix_always, and 32 strip_order_ignored: src\renderer_scene\quads2d_plants.h)
//
//   [P3C TAP]     the scope's op-21 reads c29..c32 (alone, or inside one upload from c28), the sub_82BC2A30 pair (c28 then c29..c32) included; the register file keeps them with the frame stamp (a glyph row takes
//                 the matrix of ITS frame only); a matrix-placed row whose matrix was never seen has no kQhMatrix (a hole, never a guess)
//   [P3C BLOCKS]  BlocksOf: a row with c28 != 0 and its matrix is a matrix-placed block (vs_bytes 208, Mat[k] = register 29 + k as uploaded, corners in the DRAW's order, strip flag), whose CPU image of the vertex program
//                 (clip[k] = dot(Mat[k], (x y z 1))) is the game's own formula; refused without the matrix (Projection), for a movie row, for a hue-shifted sprite (Variant); a c28 = 0 row is unchanged
//   [P3C RECORD]  layout 5 (324 bytes a row): a matrix row round-trips byte-equal through the section writer / parser; a non-finite matrix refuses that row only
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../src/renderer_scene/quads2d_blocks.h"
#include "../../src/renderer_scene/quads2d_core.h"
#include "sc_support.h"   // S103 SC
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rows_quad2d.h"

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
  void PutF(uint32_t a, float f) {   // the guest holds big-endian words: the host word read back is the byte-swapped one (ReadWords returns "host words" as F32FromBe expects: the BE bits as a host integer)
    uint32_t v;
    std::memcpy(&v, &f, 4);
    words[a] = v;
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
static const uint32_t F = 71;
static const float kV[4][5] = {{100, 50, 0.1f, 0.0f, 0.0f}, {300, 50, 0.1f, 1.0f, 0.0f}, {100, 150, 0.1f, 0.0f, 1.0f}, {300, 150, 0.1f, 1.0f, 1.0f}};   // a Z strip: 0 1 2 3
// A matrix as the game uploads it: register 29 + k = (column k of the object's row-major matrix). Here a pixel -> clip map with a skew term, so a transposed or an ignored matrix gives other numbers:
// clip.x = 2 x / 1280 - 1 + 0.0005 y, clip.y = 1 - 2 y / 720, clip.z = z, clip.w = 1 + 0.0002 x
static const float kM[16] = {2.0f / 1280, 0.0005f, 0.0f, -1.0f,     // register 29 -> clip.x = dot(c29, (x y z 1))
                             0.0f, -2.0f / 720, 0.0f, 1.0f,         // register 30 -> clip.y
                             0.0f, 0.0f, 1.0f, 0.0f,                // register 31 -> clip.z
                             0.0002f, 0.0f, 0.0f, 1.0f};            // register 32 -> clip.w

static void PutMatrix(FakeMem& m, uint32_t at) {
  for (int i = 0; i < 16; ++i) m.PutF(at + 4u * i, kM[i]);
}
static void FillVerts(FakeMem& m, bool sprite) {
  const uint32_t fl = sprite ? 5u : 3u;
  for (int v = 0; v < 4; ++v)
    for (uint32_t k = 0; k < fl; ++k) m.PutF(kVerts + 4 * fl * v + 4 * k, kV[v][k]);
  m.words[kObject + kGlyphKeyOffset] = kKey;
}

struct Run {
  std::vector<Quad2D> rows;
  std::vector<Store::Hole> holes;
  Counters c;
};
// How the leaf uploads (sub_82BC2A30): c28 alone, then c29..c32 in a second upload; `joined` = one upload from register 28 (five registers).
enum class Up { Pair, Joined, NoMatrix, MatrixOnlyOnce };
static void DrawM(Run& r, uint8_t scope_kind, uint32_t site, uint32_t stride, Up up, float c28) {
  FakeMem m;
  FillVerts(m, stride == 20);
  for (int k = 0; k < 4; ++k) m.PutF(kPay + 4u * k, k == 0 ? 2.0f / 1280 : k == 1 ? 2.0f / 720 : 0.0f), m.PutF(kPay + 16u + 4u * k, 0.5f + 0.1f * k);   // c0, c1
  for (int k = 0; k < 4; ++k) m.PutF(kPay + 32u + 4u * k, k == 0 ? c28 : 0.0f);                                                                          // c28 (then the matrix right after it: one upload from 28 is c28 + c29..c32)
  PutMatrix(m, kPay + 48);
  Store store;
  Scope s;
  OpenScope(s, scope_kind, kObject);
  OnOp21(s, m, 0, 1, kPay, r.c);
  OnOp21(s, m, 1, 1, kPay + 16, r.c);
  if (up == Up::Joined) OnOp21(s, m, 28, 5, kPay + 32, r.c);
  else {
    OnOp21(s, m, 28, 1, kPay + 32, r.c);
    if (up == Up::Pair) OnOp21(s, m, 29, 4, kPay + 48, r.c);
  }
  OnOp19(s, m, store, F, site, 6, 4, stride, kVerts, r.c);
  CloseScope(s, store, F, r.c);
  store.Drain(F, r.rows);
  store.DrainHoles(F, r.holes);
}

// The game's vertex program on the CPU: clip[k] = dot(register 29 + k, (x y z 1)) (src\renderer\shaders\fx\ySprite.vert / yRect.vert); our program's image: Mat[k] from the block.
static void ClipOfGame(const float reg[16], const float p[3], float out[4]) {
  for (int k = 0; k < 4; ++k) out[k] = reg[4 * k] * p[0] + reg[4 * k + 1] * p[1] + reg[4 * k + 2] * p[2] + reg[4 * k + 3];
}
static void ClipOfBlock(const Blocks& b, int corner, float out[4]) {
  const float* p = b.vs + 4 + 4 * corner;
  for (int k = 0; k < 4; ++k) {
    const float* mk = b.vs + 36 + 4 * k;
    out[k] = mk[0] * p[0] + mk[1] * p[1] + mk[2] * p[2] + mk[3];
  }
}
static bool Near(const float* a, const float* b, int n) {
  for (int i = 0; i < n; ++i)
    if (std::fabs(a[i] - b[i]) > 1e-6f * (1.0f + std::fabs(a[i]))) return false;
  return true;
}

static void Tap() {
  Run pair, joined, none, glyph_none, rect;
  DrawM(pair, kQkGlyph, kSiteGlyphM, 20, Up::Pair, 1.0f);
  DrawM(joined, kQkGlyph, kSiteGlyphM, 20, Up::Joined, 1.0f);
  DrawM(none, kQkGlyph, kSiteGlyphM, 20, Up::NoMatrix, 1.0f);
  DrawM(rect, kQkRect, kSiteRectGrad, 12, Up::Pair, 1.0f);
  const bool one = pair.rows.size() == 1 && pair.holes.empty();
  Check(one && (pair.rows[0].have & kQhMatrix) && pair.rows[0].use_proj == 1.0f && std::memcmp(pair.rows[0].proj, kM, sizeof(kM)) == 0, "P3C TAP",
        "the M site's scope reads c28 = 1 and the matrix c29..c32 (a second upload, as sub_82BC2A30 makes it) exactly as uploaded");
  Check(joined.rows.size() == 1 && (joined.rows[0].have & kQhMatrix) && std::memcmp(joined.rows[0].proj, kM, sizeof(kM)) == 0, "P3C TAP", "the same when one upload carries c28 and c29..c32 (first register 28, five registers)");
  Check(rect.rows.size() == 1 && (rect.rows[0].have & kQhMatrix) && std::memcmp(rect.rows[0].proj, kM, sizeof(kM)) == 0 && rect.rows[0].kind == kQkRect, "P3C TAP", "the flat T_Rect strip's scope reads the matrix too");
  Blocks b;
  Check(none.rows.size() == 1 && !(none.rows[0].have & kQhMatrix) && BlocksOf(none.rows[0], b) == Refuse::Projection, "P3C TAP", "c28 = 1 and no matrix upload in the scope: the row carries no matrix and the drawer refuses it (a hole), never a guess");
  // the register file (glyph rows): c28 and the matrix are uploaded OUTSIDE the glyph scope, by the text object's draw start
  FakeMem m;
  FillVerts(m, true);
  for (int k = 0; k < 4; ++k) m.PutF(kPay + 4u * k, k == 0 ? 2.0f / 1280 : k == 1 ? 2.0f / 720 : 0.0f), m.PutF(kPay + 16u + 4u * k, 1.0f), m.PutF(kPay + 32u + 4u * k, k == 0 ? 0.25f : 0.0f), m.PutF(kPay + 48u + 4u * k, k == 0 ? 1.0f : 0.0f);
  PutMatrix(m, kPay + 64);
  Counters c;
  const auto glyph = [&](uint32_t stamp_matrix, bool upload_matrix) {
    Shadow sh;
    Store store;
    Scope s;
    OnOp21Shadow(sh, m, 0, 1, kPay, c, F);
    OnOp21Shadow(sh, m, 1, 1, kPay + 16, c, F);
    OnOp21Shadow(sh, m, kSelectorReg, 1, kPay + 32, c, F);
    OnOp21Shadow(sh, m, 28, 1, kPay + 48, c, F);
    if (upload_matrix) OnOp21Shadow(sh, m, 29, 4, kPay + 64, c, stamp_matrix);
    OpenScope(s, kQkGlyph, kObject);
    OnOp19(s, m, store, F, kSiteGlyph, 6, 4, 20, kVerts, c, &sh);
    CloseScope(s, store, F, c);
    std::vector<Quad2D> rows;
    store.Drain(F, rows);
    return rows;
  };
  const std::vector<Quad2D> fresh = glyph(F, true), stale = glyph(F - 1, true), missing = glyph(F, false);
  Check(fresh.size() == 1 && (fresh[0].have & kQhMatrix) && std::memcmp(fresh[0].proj, kM, sizeof(kM)) == 0 && fresh[0].use_proj == 1.0f, "P3C TAP", "a glyph row takes the register file's matrix of its own frame");
  Check(stale.size() == 1 && !(stale[0].have & kQhMatrix), "P3C TAP", "a matrix uploaded in ANOTHER frame is refused (the row has none: a hole)");
  Check(missing.size() == 1 && !(missing[0].have & kQhMatrix), "P3C TAP", "no matrix upload at all: the glyph row has none");
  // S103 P3c review F1: a partial upload over c29..c32 invalidates the matrix (register file and scope); the row has none: a hole
  {
    FakeMem pm = m;
    Counters pc;
    Shadow sh;
    Store store;
    Scope s;
    OnOp21Shadow(sh, pm, 0, 1, kPay, pc, F), OnOp21Shadow(sh, pm, 1, 1, kPay + 16, pc, F), OnOp21Shadow(sh, pm, kSelectorReg, 1, kPay + 32, pc, F), OnOp21Shadow(sh, pm, 28, 1, kPay + 48, pc, F);
    OnOp21Shadow(sh, pm, 29, 4, kPay + 64, pc, F);
    OnOp21Shadow(sh, pm, 31, 1, kPay + 64, pc, F);   // a partial upload of c31 alone
    OpenScope(s, kQkGlyph, kObject);
    OnOp19(s, pm, store, F, kSiteGlyph, 6, 4, 20, kVerts, pc, &sh);
    CloseScope(s, store, F, pc);
    std::vector<Quad2D> rows;
    store.Drain(F, rows);
    Blocks b;
    Check(rows.size() == 1 && !(sh.have & kQhMatrix) && sh.stamp[kShC29] == 0 && !(rows[0].have & kQhMatrix) && BlocksOf(rows[0], b) == Refuse::Projection && pc.glyph_stale == 1, "P3C TAP",
          "F1: a partial upload (c31 alone) after a full one drops the register file's matrix: the glyph row has none (a hole, counted in glyph_stale)");
    for (const uint32_t first : {28u, 30u, 32u}) {   // c28 + c29 (a partial from below), c30.., c32 alone (a partial from above): the scope's matrix goes too
      Run r;
      FakeMem fm = m;
      Store st;
      Scope sc;
      OpenScope(sc, kQkGlyph, kObject);
      OnOp21(sc, fm, 0, 1, kPay, r.c), OnOp21(sc, fm, 1, 1, kPay + 16, r.c), OnOp21(sc, fm, 28, 1, kPay + 48, r.c), OnOp21(sc, fm, 29, 4, kPay + 64, r.c);
      OnOp21(sc, fm, first, 2, kPay + 64, r.c);
      OnOp19(sc, fm, st, F, kSiteGlyphM, 6, 4, 20, kVerts, r.c);
      CloseScope(sc, st, F, r.c);
      st.Drain(F, r.rows);
      Check(r.rows.size() == 1 && !(r.rows[0].have & kQhMatrix) && BlocksOf(r.rows[0], b) == Refuse::Projection, "P3C TAP", "F1: a partial upload (first register " + std::to_string(first) + ", two registers) drops the scope's matrix: a hole");
    }
  }
  // S103 P3c review F2: a matrix uploaded AFTER the draw is not back-filled (the row stays a hole)
  {
    Run r;
    FakeMem fm = m;
    Store st;
    Scope sc;
    OpenScope(sc, kQkGlyph, kObject);
    OnOp21(sc, fm, 0, 1, kPay, r.c), OnOp21(sc, fm, 1, 1, kPay + 16, r.c), OnOp21(sc, fm, 28, 1, kPay + 48, r.c);
    OnOp19(sc, fm, st, F, kSiteGlyphM, 6, 4, 20, kVerts, r.c);
    OnOp21(sc, fm, 29, 4, kPay + 64, r.c);   // after the vertices
    CloseScope(sc, st, F, r.c);
    st.Drain(F, r.rows);
    Blocks b;
    Check(r.rows.size() == 1 && !(r.rows[0].have & kQhMatrix) && BlocksOf(r.rows[0], b) == Refuse::Projection, "P3C TAP", "F2: a matrix uploaded after the draw is never back-filled: the row has none (a hole)");
  }
  // S103 P3c review F3: a glyph row with c28 = 0 takes no matrix (a stale or non-finite one in the register file cannot make it a bad row)
  {
    FakeMem fm = m;
    fm.PutF(kPay + 48, 0.0f);   // c28 = 0
    fm.PutF(kPay + 64 + 12, std::nanf(""));   // a non-finite matrix in the register file
    Counters fc;
    Shadow sh;
    Store st;
    Scope sc;
    OnOp21Shadow(sh, fm, 0, 1, kPay, fc, F), OnOp21Shadow(sh, fm, 1, 1, kPay + 16, fc, F), OnOp21Shadow(sh, fm, kSelectorReg, 1, kPay + 32, fc, F), OnOp21Shadow(sh, fm, 28, 1, kPay + 48, fc, F);
    OnOp21Shadow(sh, fm, 29, 4, kPay + 64, fc, F);
    OpenScope(sc, kQkGlyph, kObject);
    OnOp19(sc, fm, st, F, kSiteGlyph, 6, 4, 20, kVerts, fc, &sh);
    CloseScope(sc, st, F, fc);
    std::vector<Quad2D> rows;
    st.Drain(F, rows);
    if (rows.size() == 1) Check(Quad2DRowProblem(rows[0]) == "no scissor state read", "P3C TAP", "F3: the unstamped glyph row returns \"no scissor state read\" before the stamp");   // S103 SC review
    if (rows.size() == 1) ValidSC(rows[0]);   // S103 SC: made without a state object: the layout-4 scissor / target state is stamped; the check is about the matrix
    Check(rows.size() == 1 && rows[0].use_proj == 0.0f && !(rows[0].have & kQhMatrix) && Quad2DRowProblem(rows[0]).empty() && fc.glyph_stale == 0, "P3C TAP", "F3: a glyph row with c28 = 0 takes no matrix and stays a valid row");
  }
}

static void BlocksChecks() {
  Run m, r;
  DrawM(m, kQkGlyph, kSiteGlyphM, 20, Up::Pair, 1.0f);
  DrawM(r, kQkRect, kSiteRectGrad, 12, Up::Pair, 1.0f);
  Blocks bm, br;
  const bool ok = m.rows.size() == 1 && r.rows.size() == 1 && BlocksOf(m.rows[0], bm) == Refuse::None && BlocksOf(r.rows[0], br) == Refuse::None;
  Check(ok && bm.proj && br.proj && bm.vs_bytes == 208 && br.vs_bytes == 208 && bm.program == Program::Sprite && br.program == Program::Rect, "P3C BLOCKS", "a matrix-placed row is a matrix block (208 bytes) of its own program kind (sprite / rect)");
  bool mats = ok;
  for (int i = 0; ok && i < 16; ++i) mats = mats && bm.vs[36 + i] == kM[i] && br.vs[36 + i] == kM[i];
  Check(mats, "P3C BLOCKS", "Mat[k] is register 29 + k as uploaded (no transposition: the game's program dots the register with (x y z 1))");
  bool corners = ok, game = ok;
  for (int v = 0; ok && v < 4; ++v) {
    corners = corners && bm.vs[4 + 4 * v] == kV[v][0] && bm.vs[5 + 4 * v] == kV[v][1] && bm.vs[6 + 4 * v] == kV[v][2] && bm.vs[20 + 4 * v] == kV[v][3] && bm.vs[21 + 4 * v] == kV[v][4];
    float a[4], b[4];
    ClipOfGame(kM, kV[v], a);
    ClipOfBlock(bm, v, b);
    game = game && Near(a, b, 4);
  }
  Check(corners && bm.vs[2] == 1.0f && br.vs[2] == 1.0f, "P3C BLOCKS", "the corners are the draw's own vertices in the DRAW's order (no perimeter reordering) and the strip flag is set (the shader draws the stream's triangles 0 1 2 / 2 1 3)");
  Check(game, "P3C BLOCKS", "the CPU image of our vertex program on the block equals the game's formula on the upload, for all four corners");
  // not a strip: the quad-list site keeps the perimeter order and no strip flag
  Run q;
  {
    FakeMem fm;
    FillVerts(fm, true);
    for (int k = 0; k < 4; ++k) fm.PutF(kPay + 4u * k, k == 0 ? 2.0f / 1280 : k == 1 ? 2.0f / 720 : 0.0f), fm.PutF(kPay + 16u + 4u * k, 1.0f), fm.PutF(kPay + 32u + 4u * k, k == 0 ? 1.0f : 0.0f);
    PutMatrix(fm, kPay + 48);
    Store store;
    Scope s;
    OpenScope(s, kQkSprite, kObject);
    fm.words[kObject + kSpriteKeyOffset] = kKey;
    OnOp21(s, fm, 0, 1, kPay, q.c), OnOp21(s, fm, 1, 1, kPay + 16, q.c), OnOp21(s, fm, 28, 1, kPay + 32, q.c), OnOp21(s, fm, 29, 4, kPay + 48, q.c);
    OnOp19(s, fm, store, F, kSiteSprite, 13, 4, 20, kVerts, q.c);
    CloseScope(s, store, F, q.c);
    store.Drain(F, q.rows);
  }
  Blocks bq;
  Check(q.rows.size() == 1 && BlocksOf(q.rows[0], bq) == Refuse::None && bq.proj && bq.vs[2] == 0.0f, "P3C BLOCKS", "a quad-list row (the T_Sprite site, prim 13) is matrix-placed without the strip flag");
  // refusals
  if (ok) {
    Quad2D nomat = m.rows[0];
    nomat.have &= ~uint32_t(kQhMatrix);
    Blocks b;
    Check(BlocksOf(nomat, b) == Refuse::Projection, "P3C BLOCKS", "c28 != 0 without a matrix: refused (Projection)");
    Quad2D hue = m.rows[0];
    hue.have |= kQhHsl;
    hue.hsl[0] = 0.1f;
    Check(BlocksOf(hue, b) == Refuse::Variant, "P3C BLOCKS", "a hue-shifted matrix-placed sprite: refused (Variant): no matrix variant of that program");
    Quad2D movie = m.rows[0];
    movie.kind = kQkYuvAlpha;
    Check(BlocksOf(movie, b) == Refuse::Projection, "P3C BLOCKS", "a matrix-placed movie row: refused (Projection)");
    Quad2D bad = m.rows[0];
    bad.proj[5] = std::nanf("");
    Check(BlocksOf(bad, b) == Refuse::NotFinite, "P3C BLOCKS", "a non-finite matrix: refused (NotFinite)");
    Quad2D prim = m.rows[0];
    prim.prim = 4;
    Check(BlocksOf(prim, b) == Refuse::Prim, "P3C BLOCKS", "a matrix-placed sprite row of another primitive: refused (Prim)");
    Quad2D flat = m.rows[0];
    flat.use_proj = 0.0f;
    flat.prim = 0;
    Check(BlocksOf(flat, b) == Refuse::None && !b.proj && b.vs_bytes == 144 && b.vs[0] == 2.0f / 1280 && b.vs[3] == 0.5f, "P3C BLOCKS", "c28 = 0: the pixel-unit block as before (144 bytes, the half unit)");
  }
}

static void Record() {
  Run m;
  DrawM(m, kQkGlyph, kSiteGlyphM, 20, Up::Pair, 1.0f);
  SceneTables t;
  t.has_camera = true;
  if (m.rows.size() == 1) {
    Quad2D a = m.rows[0];
    ValidSC(a);   // S103 SC
    Quad2D b = a;
    b.seq = a.seq + 1;
    t.quads2d = {a, b};
  }
  std::vector<uint8_t> by;
  std::string e;
  if (!t.quads2d.empty()) by = SerializeRecord(t);
  const bool wrote = !by.empty();
  SceneTables back;
  const bool parsed = wrote && ParseRecord(by.data(), by.size(), back, e) && back.quads2d.size() == 2 && back.quad2d_errors == 0;
  Check(sizeof(Quad2D) == 324 && kQuad2DLayout == 5 && parsed && std::memcmp(&back.quads2d[0], &t.quads2d[0], sizeof(Quad2D)) == 0 && (back.quads2d[0].have & kQhMatrix) && back.quads2d[0].proj[0] == kM[0], "P3C RECORD",
        "layout 5, 324 bytes a row: a matrix row round-trips byte-equal (the matrix, kQhMatrix)");
  Quad2D bad = t.quads2d.empty() ? Quad2D{} : t.quads2d[0];
  bad.proj[3] = std::nanf("");
  Check(!t.quads2d.empty() && !Quad2DRowProblem(bad).empty() && Quad2DRowProblem(t.quads2d[0]).empty(), "P3C RECORD", "a non-finite matrix refuses THAT row (Quad2DRowProblem)");
}

static void All() { Tap(), BlocksChecks(), Record(); }

int main(int argc, char** argv) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") {
      const std::string p = argv[i + 1];
      const std::map<std::string, int> plants = {{"proj_not_taken", 33}, {"proj_transposed", 34}, {"proj_c28_ignored", 35}, {"matrix_partial_kept", 36}, {"glyph_matrix_always", 37}};
      const std::map<std::string, std::string> expect = {{"proj_not_taken", "[P3C TAP]"}, {"proj_transposed", "[P3C BLOCKS]"}, {"proj_c28_ignored", "[P3C BLOCKS]"}, {"matrix_partial_kept", "[P3C TAP]"}, {"glyph_matrix_always", "[P3C TAP]"}};
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
