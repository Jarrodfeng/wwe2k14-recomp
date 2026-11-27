// S111 VIEWROWS unit: ResolveChar "View" (VS / PS c52-c54) = the game's g_fm44View = the transpose of the draw camera's world -> view matrix. 0 game runs.
//   The game: sub_82BE6350 (and the model block copy sub_82BD7B50) upload the transpose of [camera +240] (camera = sub_82BA7748()), so register k = column k of the
//   row-vector view V: (view axis k in world space, V[3][k] the translation). The yCh_Reflect vertex shader C3B9D628507CFEB5 writes interpolator 8 = (dot(n, c52.xyz),
//   dot(n, c53.xyz), dot(n, c54.xyz)), n the unit world normal; fx_trans_ps.glsl reads cube slot 10 at (x, -y, z) of it.
//   Synthetic data only: rigid views V and the game's projection pattern P (sub_82BB2EC0: m[0] = cot / aspect, m[5] = cot, m[10], m[11] = 1, m[14]; sub_82BB3748 negates
//   m[4..7]; plus off-centre m[8] / m[9]); ResolveChar gets VP = V x P and the eye, in the game's space (mirror +1) and the viewer's (mirror -1, Y negated).
//   out\tools\viewrows_unit.exe   -> PASS / FAIL, exit 0 / 1 (tools\scene\build_viewrows_unit.ps1 builds and runs it)
// Planted twins that must be caught: the old identity, and V's ROWS instead of its columns (the transpose dropped).
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "../../src/renderer_scene/char_effects.h"
#include "../../src/renderer_scene/draw_lists.h"

using namespace rdr_scene;

namespace {
struct M4 {
  double m[16] = {};
};
M4 Mul(const M4& a, const M4& b) {
  M4 r;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r.m[i * 4 + j] += a.m[i * 4 + k] * b.m[k * 4 + j];
  return r;
}
// A rigid row-vector world -> view: R = Ry(yaw) Rx(pitch) Rz(roll) (3 x 3, orthonormal), V[j][k] = R[j][k], V[3][k] = -(eye . column k of R).
M4 RigidView(double yaw, double pitch, double roll, const double eye[3]) {
  const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
  const double ry[3][3] = {{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}};
  const double rx[3][3] = {{1, 0, 0}, {0, cp, sp}, {0, -sp, cp}};
  const double rz[3][3] = {{cr, sr, 0}, {-sr, cr, 0}, {0, 0, 1}};
  double t[3][3] = {}, r[3][3] = {};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) t[i][j] += ry[i][k] * rx[k][j];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) r[i][j] += t[i][k] * rz[k][j];
  M4 v;
  for (int j = 0; j < 3; ++j)
    for (int k = 0; k < 3; ++k) v.m[j * 4 + k] = r[j][k];
  for (int k = 0; k < 3; ++k) v.m[12 + k] = -(eye[0] * r[0][k] + eye[1] * r[1][k] + eye[2] * r[2][k]);
  v.m[15] = 1.0;
  return v;
}
// The game's perspective (row vectors), with the y flip and an off-centre pair.
M4 GameProj(double fov, double aspect, double n, double f, double ox, double oy) {
  const double c = 1.0 / std::tan(fov / 2.0);
  M4 p;
  p.m[0] = c / aspect, p.m[5] = -c, p.m[8] = ox, p.m[9] = oy, p.m[10] = f / (f - n), p.m[11] = 1.0, p.m[14] = -n * f / (f - n);
  return p;
}
// The shader's lookup: vn_i = dot(n, reg_i.xyz) (fx_trans_vs.glsl outViewNormal), then the cube coordinate (x, -y, z) (fx_trans_ps.glsl FX_ENV_LIGHT).
void Lookup(const double* reg /* 3 rows of 4 */, const double n[3], double out[3]) {
  double vn[3];
  for (int i = 0; i < 3; ++i) vn[i] = n[0] * reg[4 * i] + n[1] * reg[4 * i + 1] + n[2] * reg[4 * i + 2];
  out[0] = vn[0], out[1] = -vn[1], out[2] = vn[2];
}
double Dist(const double a[3], const double b[3]) { return std::fabs(a[0] - b[0]) + std::fabs(a[1] - b[1]) + std::fabs(a[2] - b[2]); }
}  // namespace

int main() {
  assets::Material mat;
  int bad = 0, n_cases = 0, rows_bad = 0, texel_bad = 0, key_bad = 0, twin_identity = 0, twin_rows = 0;
  const double cams[][3] = {{0.0, 0.0, 0.0}, {0.7, -0.3, 0.1}, {2.5, 0.4, -0.2}, {-1.2, 1.1, 0.05}};
  const double eye[3] = {120.0, -150.0, 400.0};
  const M4 P = GameProj(0.296706, 16.0 / 9.0, 10.0, 20000.0, 0.013, -0.021);
  for (const auto& a : cams) {
    const M4 V = RigidView(a[0], a[1], a[2], eye);
    const M4 VP = Mul(V, P);
    for (int mi = 0; mi < 2; ++mi) {
      const float mirror = mi ? -1.f : 1.f;
      float vp[16], e[3];
      for (int i = 0; i < 16; ++i) vp[i] = float(VP.m[i] * ((i / 4 == 1) ? double(mirror) : 1.0));   // a viewer-space record: the y row negated (clip = p_file S VP)
      e[0] = float(eye[0]), e[1] = float(eye[1] * double(mirror)), e[2] = float(eye[2]);
      assets::LightSet ls;
      ls.hl_view = true, ls.hl_dir[0] = 0.31f, ls.hl_dir[1] = 0.643f, ls.hl_dir[2] = -0.7f;
      CharCtx c;
      c.mat = &mat, c.lights = &ls, c.view_proj = vp, c.eye = e, c.mirror = mirror;
      ++n_cases;
      float got[12] = {};
      if (!ResolveChar("View", 3, c, got)) {
        std::printf("FAIL  ResolveChar does not know View\n");
        ++bad;
        continue;
      }
      // 1. the registers: column k of V (xyz) and V[3][k] (w)
      double reg[12], err_xyz = 0, err_w = 0;
      for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 4; ++i) {
          reg[4 * k + i] = got[4 * k + i];
          const double want = i < 3 ? V.m[i * 4 + k] : V.m[12 + k];
          const double d = std::fabs(double(got[4 * k + i]) - want);
          if (i < 3) err_xyz = d > err_xyz ? d : err_xyz;
          else err_w = d > err_w ? d : err_w;
        }
      if (err_xyz > 1e-5 || err_w > 1e-3) {
        std::printf("FAIL  camera (%g %g %g) mirror %g: View rows differ from transpose(V): xyz %.3g, w %.3g\n", a[0], a[1], a[2], mirror, err_xyz, err_w);
        ++rows_bad;
      }
      // 2. the eye's texel: a normal facing the camera (minus the view forward) reads the -Z face centre; the view right reads +X; the view y (screen down) reads -Y
      const double fwd[3] = {V.m[2], V.m[6], V.m[10]}, right[3] = {V.m[0], V.m[4], V.m[8]}, vy[3] = {V.m[1], V.m[5], V.m[9]};
      const double facing[3] = {-fwd[0], -fwd[1], -fwd[2]};
      const double want_f[3] = {0, 0, -1}, want_r[3] = {1, 0, 0}, want_y[3] = {0, -1, 0};
      double lf[3], lr[3], ly[3];
      Lookup(reg, facing, lf), Lookup(reg, right, lr), Lookup(reg, vy, ly);
      if (Dist(lf, want_f) > 1e-4 || Dist(lr, want_r) > 1e-4 || Dist(ly, want_y) > 1e-4) {
        std::printf("FAIL  camera (%g %g %g) mirror %g: the facing normal reads (%.4f %.4f %.4f), want the -Z face centre (0 0 -1)\n", a[0], a[1], a[2], mirror, lf[0], lf[1], lf[2]);
        ++texel_bad;
      }
      // the planted twins: the old identity, and V's rows instead of its columns
      const double ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
      double rows[12];
      for (int k = 0; k < 3; ++k)
        for (int i = 0; i < 3; ++i) rows[4 * k + i] = V.m[k * 4 + i];
      for (int k = 0; k < 3; ++k) rows[4 * k + 3] = 0.0;
      double tf[3];
      Lookup(ident, facing, tf);
      if (Dist(tf, want_f) > 1e-3) ++twin_identity;
      Lookup(rows, facing, tf);
      double tr[3];
      Lookup(rows, right, tr);
      if (Dist(tf, want_f) > 1e-3 || Dist(tr, want_r) > 1e-3) ++twin_rows;
      // 3. KeyLightWorld (now through CameraAxes): KeyLightVec = V d, the view-space key turned into the game's space
      float kv[4] = {};
      ResolveChar("KeyLightVec", 1, c, kv);
      double kerr = 0;
      for (int j = 0; j < 3; ++j) {
        const double want = V.m[j * 4] * ls.hl_dir[0] + V.m[j * 4 + 1] * ls.hl_dir[1] + V.m[j * 4 + 2] * ls.hl_dir[2];
        const double d = std::fabs(double(kv[j]) - want);
        kerr = d > kerr ? d : kerr;
      }
      if (kerr > 1e-5) {
        std::printf("FAIL  camera (%g %g %g) mirror %g: KeyLightVec differs from V d by %.3g\n", a[0], a[1], a[2], mirror, kerr);
        ++key_bad;
      }
    }
  }
  bad += rows_bad + texel_bad + key_bad;
  std::printf("%s  View = transpose(V) rows 0-2 (xyz the view axes, w the translation): %d of %d cameras differ\n", rows_bad ? "FAIL" : "ok  ", rows_bad, n_cases);
  std::printf("%s  the eye's texel: a normal facing the camera reads the -Z face centre (right +X, view y -Y): %d of %d cameras differ\n", texel_bad ? "FAIL" : "ok  ", texel_bad, n_cases);
  std::printf("%s  KeyLightVec = V d through CameraAxes (KeyLightWorld unchanged): %d of %d cameras differ\n", key_bad ? "FAIL" : "ok  ", key_bad, n_cases);
  {   // 4. no camera: the identity (a caller with no view-projection)
    assets::LightSet ls;   // ResolveChar reads the light set before the View branch (the key light): every caller passes one
    CharCtx c;
    c.mat = &mat, c.lights = &ls;
    float got[12] = {9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    const float want[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    bool ok = ResolveChar("View", 3, c, got);
    for (int i = 0; i < 12; ++i) ok = ok && got[i] == want[i];
    std::printf("%s  no view-projection: View is the identity\n", ok ? "ok  " : "FAIL");
    if (!ok) ++bad;
  }
  {   // 6. S111 VIEWROWS review: the CAW portrait camera (sub_8341BF58) keeps its y flip in the VIEW (+240 = the given camera's view with column 1 negated, +134..+174)
      // and builds its projection with sub_82CCB2D8 (m[5] = +cot, not negated, +38C..+398). Its view-projection equals a scene camera's, but the game's View is the transpose
      // of THAT view, so register 1 (and its w) is minus the scene camera's: only the record's own view gives it.
    int portrait_bad = 0, portrait_cases = 0, portrait_twin = 0;
    for (const auto& a : cams) {
      M4 Vp = RigidView(a[0], a[1], a[2], eye);
      for (int i = 0; i < 4; ++i) Vp.m[i * 4 + 1] = -Vp.m[i * 4 + 1];
      M4 Pp = GameProj(0.36959913, 0.8625, 1.0, 65536.0, 0.0, 0.0);
      Pp.m[5] = -Pp.m[5];
      const M4 VPp = Mul(Vp, Pp);
      for (int mi = 0; mi < 2; ++mi) {
        const float mirror = mi ? -1.f : 1.f;
        float vp[16], vw[16], e[3];
        for (int i = 0; i < 16; ++i) vp[i] = float(VPp.m[i] * ((i / 4 == 1) ? double(mirror) : 1.0)), vw[i] = float(Vp.m[i] * ((i / 4 == 1) ? double(mirror) : 1.0));
        e[0] = float(eye[0]), e[1] = float(eye[1] * double(mirror)), e[2] = float(eye[2]);
        assets::LightSet ls;
        CharCtx c;
        c.mat = &mat, c.lights = &ls, c.view_proj = vp, c.eye = e, c.mirror = mirror, c.view = vw;   // a live record: the camera's own view beside the product (front_end.cpp Record)
        ++portrait_cases;
        float got[12] = {};
        ResolveChar("View", 3, c, got);
        double err_xyz = 0, err_w = 0;
        for (int k = 0; k < 3; ++k)
          for (int i = 0; i < 4; ++i) {
            const double d = std::fabs(double(got[4 * k + i]) - (i < 3 ? Vp.m[i * 4 + k] : Vp.m[12 + k]));
            if (i < 3) err_xyz = d > err_xyz ? d : err_xyz;
            else err_w = d > err_w ? d : err_w;
          }
        if (err_xyz > 1e-5 || err_w > 1e-3) {
          std::printf("FAIL  portrait camera (%g %g %g) mirror %g: View differs from transpose(the portrait's view): xyz %.3g, w %.3g\n", a[0], a[1], a[2], mirror, err_xyz, err_w);
          ++portrait_bad;
        }
        // the planted twin: the same record without its view (the axes from the view-projection alone, the landed rule) must miss the game's register 1
        float twin[12] = {};
        c.view = nullptr;
        ResolveChar("View", 3, c, twin);
        double terr = 0;
        for (int k = 0; k < 3; ++k)
          for (int i = 0; i < 3; ++i) {
            const double d = std::fabs(double(twin[4 * k + i]) - Vp.m[i * 4 + k]);
            terr = d > terr ? d : terr;
          }
        if (terr > 1e-3) ++portrait_twin;
      }
    }
    std::printf("%s  the CAW portrait camera (the y flip in its view, m[5] > 0): View = transpose(its view): %d of %d cameras differ\n", portrait_bad ? "FAIL" : "ok  ", portrait_bad, portrait_cases);
    std::printf("%s  planted twin (the portrait's axes from its view-projection alone): %d of %d cameras wrong %s\n", portrait_twin ? "ok  " : "FAIL", portrait_twin, portrait_cases, portrait_twin ? "(caught)" : "(NOT caught)");
    bad += portrait_bad + (portrait_twin ? 0 : 1);
  }
  {   // 5. the kept draw lists re-feed View when the camera changes
    const bool ok = FieldMask("View") == uint32_t(kStCamera);
    std::printf("%s  draw_lists.cpp: View is kStCamera (re-fed when the camera stamp changes)\n", ok ? "ok  " : "FAIL");
    if (!ok) ++bad;
  }
  std::printf("%s  planted twin (the old identity): %d of %d cameras wrong %s\n", twin_identity ? "ok  " : "FAIL", twin_identity, n_cases, twin_identity ? "(caught)" : "(NOT caught)");
  std::printf("%s  planted twin (V's rows, the transpose dropped): %d of %d cameras wrong %s\n", twin_rows ? "ok  " : "FAIL", twin_rows, n_cases, twin_rows ? "(caught)" : "(NOT caught)");
  if (!twin_identity) ++bad;
  if (!twin_rows) ++bad;
  std::printf("%s viewrows_unit: View = the game's transpose of the camera view\n", bad ? "FAIL" : "PASS");
  return bad ? 1 : 0;
}
