// WGS6: the unit of ExpectPx (tools\scene\arena_cams.h): the expected footprint and the composite colour / alpha of a screen piece, with the key's draw order. Synthetic quads only: no game
// data, no GPU, ~0.1 s.   pwsh -NoProfile -File tools\scene\build_arena_cams_unit.ps1  ->  out\tools\arena_cams_unit.exe
// Rows: a piece drawn FIRST and hidden by a nearer piece loses those pixels (hidden_px counts them); the same pieces in the other order lose nothing (the later nearer one blends over);
// a nearer piece whose alpha the game's test discards (< 1 / 255) hides nothing; a nearer piece at alpha 100 does; the vertex colour is per channel (R 255 G 216 B 159); two half-alpha
// layers composite to 1 - (1 - a)^2; a shared triangle edge counts once; a back-facing piece has no footprint; a piece crossing the near plane is clipped, not dropped.
// (a tool unit: ExpectPx is the check's own function, no production hook enters it)
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "arena_cams.h"

using namespace wgs;

static int g_fail = 0;
static void Check(const char* name, bool ok) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) ++g_fail;
}

// A quad (two triangles, wound so the geometric normal faces the camera at z < 0) at depth z, x from x0 to x0 + 20, y -10..10, one vertex colour word (bytes: bits 0-7 R, 8-15 G, 16-23 B, 24-31 A).
static Piece Quad(int index, size_t first_tri, double x0, double z, uint32_t col, double y0 = -10, double y1 = 10) {
  Piece p;
  p.key = 808, p.index = index;
  const V3 q0 = {x0, y0, z}, q1 = {x0, y1, z}, q2 = {x0 + 20, y1, z}, q3 = {x0 + 20, y0, z};
  const V3 tri[6] = {q0, q1, q2, q0, q2, q3};
  for (const V3& v : tri) p.pts.push_back(v), p.uvs.push_back({0.5f, 0.5f}), p.cols.push_back(col);
  p.tris = 2;
  p.tri_id = {first_tri, first_tri + 1};
  p.blended = ((col >> 24) & 0xFFu) < 0xFFu;
  return p;
}

static Cam TestCam() {
  Cam c;
  c.eye = {0, 0, -100};
  c.target = {0, 0, 0};
  c.fov = 30, c.near_z = 1, c.far_z = 1000;
  MakeVP(c, {0, 1, 0});
  return c;
}

static uint32_t Word(int r, int g, int b, int a) { return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24; }

static uint64_t Run(const Piece& judged, const std::vector<Piece>& all, std::vector<uint8_t>* fp = nullptr, std::vector<uint8_t>* vc = nullptr, std::vector<uint8_t>* va = nullptr, uint64_t* hidden = nullptr) {
  const std::vector<TriRef> order = KeyDrawOrder(all);
  return ExpectPx(judged, TestCam(), 640, 360, fp, vc, va, &order, hidden);
}

int main(int argc, char** argv) {
  // --plant order | depth | alpha: the planted twins (ExpectPx's draw order reversed / depth test reversed / alpha test off): the rows must FAIL; exit 0 = the plant was caught
  const char* plant = nullptr;
  for (int i = 1; i + 1 < argc; ++i)
    if (std::string(argv[i]) == "--plant") plant = argv[i + 1];
  if (plant) g_expect_plant = std::string(plant) == "order" ? 1 : std::string(plant) == "depth" ? 2 : std::string(plant) == "alpha" ? 3 : 0;
  const uint32_t white = Word(255, 255, 255, 255);
  // F = the far quad (z 0), N = the nearer one (z -10), overlapping on x 0..10
  const Piece F_first = Quad(0, 0, -10, 0, white), N_second = Quad(1, 2, 0, -10, white);   // F drawn first, N later (nearer): N blends over F, hides nothing of it
  const uint64_t n_alone = Run(F_first, {F_first});
  uint64_t hid = 99;
  Check("a piece alone: a footprint", n_alone > 1000);
  Check("a nearer piece drawn LATER hides nothing of the judged piece (it blends over it)", Run(F_first, {F_first, N_second}, nullptr, nullptr, nullptr, &hid) == n_alone && hid == 0);
  const Piece N_first = Quad(1, 0, 0, -10, white), F_second = Quad(0, 2, -10, 0, white);   // N drawn first: the depth test rejects F behind it
  const uint64_t n_hidden = Run(F_second, {N_first, F_second}, nullptr, nullptr, nullptr, &hid);
  Check("a nearer piece drawn EARLIER hides the judged piece where it covers it", n_hidden < n_alone && n_hidden > n_alone / 4);
  Check("hidden_px counts exactly the pixels left out", hid == n_alone - n_hidden);
  Check("the nearer piece itself is whole", Run(N_first, {N_first, F_second}) == Run(N_first, {N_first}));
  // the game's alpha test: a fragment below 1 / 255 is discarded (draws nothing, writes no depth)
  const Piece N_clear = Quad(1, 0, 0, -10, Word(255, 255, 255, 0));
  Check("a nearer piece at vertex alpha 0 (discarded by the game's alpha test) hides nothing", Run(F_second, {N_clear, F_second}) == n_alone);
  const Piece N_faint = Quad(1, 0, 0, -10, Word(255, 255, 255, 100));
  Check("a nearer piece at vertex alpha 100 hides the judged piece (it writes depth)", Run(F_second, {N_faint, F_second}) == n_hidden);
  // the nearer piece's own planes are not blended with the farther one it hides: N (half alpha, red) drawn first, F (opaque green) behind it is rejected by the depth test
  const Piece N_half = Quad(1, 0, 0, -10, Word(255, 0, 0, 128)), F_green = Quad(0, 2, -10, 0, Word(0, 255, 0, 255));
  std::vector<uint8_t> nfp, nvc, nva;
  Run(N_half, {N_half, F_green}, &nfp, &nvc, &nva);
  bool clean = true;
  size_t nn = 0;
  for (size_t i = 0; i < nfp.size(); ++i)
    if (nfp[i]) {
      ++nn;
      if (std::abs(int(nva[i]) - 128) > 1 || nvc[i * 3] != 255 || nvc[i * 3 + 1] != 0) clean = false;
    }
  Check("the nearer piece's colour and alpha planes are its own where it hides the farther piece (not blended with it)", nn > 1000 && clean);
  // per-channel vertex colour and the composite planes
  std::vector<uint8_t> fp, vc, va;
  const Piece T = Quad(0, 0, -10, 0, Word(255, 216, 159, 255));
  Run(T, {T}, &fp, &vc, &va);
  size_t cx = 0;
  for (size_t i = 0; i < fp.size(); ++i)
    if (fp[i]) { cx = i; break; }
  Check("the vertex colour is per channel: R 255 G 216 B 159 reaches the plane as written", vc.size() == fp.size() * 3 && vc[cx * 3] == 255 && vc[cx * 3 + 1] == 216 && vc[cx * 3 + 2] == 159 && va[cx] == 255);
  // two half-alpha layers (the near one later) composite to 1 - (1 - a)^2 = 0.75, and the colour is the alpha-weighted mix of the two layers
  const Piece Fa = Quad(0, 0, -10, 0, Word(255, 0, 0, 128)), Na = Quad(1, 2, 0, -10, Word(0, 255, 0, 128));
  Run(Fa, {Fa, Na}, &fp, &vc, &va);
  size_t ov = 0, solo = 0;
  for (size_t i = 0; i < fp.size(); ++i) {
    if (!fp[i]) continue;
    if (va[i] > 170) ov = i;   // an overlap pixel
    else solo = i;
  }
  const double a = 128.0 / 255.0, A = 1.0 - (1.0 - a) * (1.0 - a);
  Check("two half-alpha layers composite to 1 - (1 - a)^2", ov && std::abs(int(va[ov]) - int(std::lround(A * 255.0))) <= 1);
  Check("the composite colour weights the later layer more (G over R)", ov && vc[ov * 3 + 1] > vc[ov * 3 + 0]);
  Check("a pixel of the far layer alone keeps its own alpha and colour", solo && std::abs(int(va[solo]) - 128) <= 1 && vc[solo * 3] == 255 && vc[solo * 3 + 1] == 0);
  // a shared triangle edge counts once: one half-alpha quad (two triangles) never composites above its own alpha on the diagonal
  Run(Fa, {Fa}, &fp, &vc, &va);
  int mx = 0;
  for (size_t i = 0; i < fp.size(); ++i)
    if (fp[i]) mx = std::max(mx, int(va[i]));
  Check("a shared edge is drawn once (no pixel above the quad's own alpha)", mx <= 129);
  // a back-facing piece: no footprint; a piece crossing the near plane is clipped, not dropped
  Piece B = Quad(0, 0, -10, 0, white);
  std::swap(B.pts[1], B.pts[2]);
  std::swap(B.pts[4], B.pts[5]);
  Check("a back-facing piece has no footprint", Run(B, {B}) == 0);
  Piece Cn = Quad(0, 0, -10, 0, white);   // a floor under the camera (y -10) from z -150 (behind the eye at -100) to +50, facing up: two corners behind the eye
  const V3 fa = {-20, -10, -150}, fb = {-20, -10, 50}, fc = {20, -10, 50}, fd = {20, -10, -150};
  Cn.pts = {fa, fb, fc, fa, fc, fd};
  Check("a floor crossing the near plane is clipped (its part in front is drawn), not dropped", Run(Cn, {Cn}) > 1000);
  if (plant) {
    std::printf("arena_cams_unit --plant %s: %s (%d row(s) failed)\n", plant, g_fail ? "PLANT caught" : "PLANT NOT CAUGHT", g_fail);
    return g_fail ? 0 : 1;
  }
  std::printf("arena_cams_unit: %s (%d failed)\n", g_fail ? "FAIL" : "PASS", g_fail);
  return g_fail ? 1 : 0;
}
