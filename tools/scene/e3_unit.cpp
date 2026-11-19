// Lane E3 (Session 103): unit proof of the fitted wrestler shadow camera (src\renderer_scene\shadow_fit.h). Pure maths, 0 game runs.
//   e3_unit                 the true cases: every line PASS, exit 0
//   e3_unit --plant NAME    a planted fault; exit 0 only when the check that sees it FAILS with its token: view_not_light (FIT USES VIEW), key_not_negated (FIT KEY SIGN), no_bounds (FIT BOUNDS)
// Reference numbers [C], read from the stream (fview --save-case, wrestler colour draw): brock_losswin frames 20 / 110 / 200: KeyLightVec and ViewLightDepth (= the light's fwd, 59909.9883) as the game holds them.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../../src/renderer_scene/light_state_rows.h"
#include "../../src/renderer_scene/shadow_fit.h"

using namespace rdr_scene;

static int g_fail = 0;
static const char* g_token = nullptr;
static void Check(bool ok, const char* token, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
#include <cstdarg>
static void Check(bool ok, const char* token, const char* fmt, ...) {
  char b[400];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", ok ? "" : token, ok ? "" : ": ", b);
  if (!ok) ++g_fail, g_token = token;
}

struct Ref { int frame; float key[3]; float fwd[3]; };
// key = VS KeyLightVec, fwd = the first three of TexProjLightW / ViewLightDepth, from the same colour draw
static const Ref kRefs[] = {
    {20, {-0.4035f, 0.5777f, -0.7095f}, {0.4248f, 0.5150f, 0.7445f}},
    {110, {-0.2483f, 0.5777f, -0.7776f}, {0.2615f, 0.5150f, 0.8163f}},
    {200, {-0.4326f, 0.5673f, -0.7007f}, {0.4518f, 0.5150f, 0.7285f}},
};

int main(int argc, char** argv) {
  ShadowFitFaults faults;
  std::string plant;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--plant" && i + 1 < argc) plant = argv[++i];
  if (plant == "view_not_light") faults.view_not_light = true;
  else if (plant == "key_not_negated") faults.key_not_negated = true;
  else if (plant == "no_bounds") faults.no_bounds = true;
  else if (!plant.empty()) { std::fprintf(stderr, "unknown plant %s\n", plant.c_str()); return 2; }
  const float main_forward[3] = {0.3031f, 0.0822f, 0.9494f};   // brock f110's main camera forward (stream c59 row): the view_not_light plant uses it as the light

  // 1. the light's direction against the stream, three frames (0.01 = 0.6 degrees)
  for (const Ref& r : kRefs) {
    ShadowFitInput in;
    std::memcpy(in.key_vec, r.key, sizeof in.key_vec);
    in.center[0] = 23.f, in.center[1] = -100.f, in.center[2] = 2696.f, in.radius = 150.f;
    ShadowFit f;
    const char* why = "";
    if (!FitShadowLight(in, f, why, faults, main_forward)) { Check(false, "FIT FAILED", "frame %d: %s", r.frame, why); continue; }
    const float err = std::max({std::fabs(f.fwd[0] - r.fwd[0]), std::fabs(f.fwd[1] - r.fwd[1]), std::fabs(f.fwd[2] - r.fwd[2])});
    const char* token = plant == "key_not_negated" ? "FIT KEY SIGN" : plant == "view_not_light" ? "FIT USES VIEW" : "FIT DIRECTION";
    Check(err < 0.01f, token, "frame %d: fitted fwd (%.4f %.4f %.4f) vs the stream's (%.4f %.4f %.4f): max |diff| %.4f (bar 0.01)", r.frame, f.fwd[0], f.fwd[1], f.fwd[2], r.fwd[0], r.fwd[1], r.fwd[2], err);
  }

  // 2. the structure: the readers' registers built from the fitted state
  {
    ShadowFitInput in;
    std::memcpy(in.key_vec, kRefs[1].key, sizeof in.key_vec);
    in.center[0] = 23.f, in.center[1] = -100.f, in.center[2] = 2696.f, in.radius = 150.f;
    ShadowFit f;
    const char* why = "";
    if (!FitShadowLight(in, f, why, faults, main_forward)) { Check(false, "FIT FAILED", "%s", why); return 1; }
    const CasterRows rows = BuildCasterRows(f.state);
    const WrestlerCameraRows cam = BuildWrestlerCameraRows(f.state);
    Check(cam.valid, "FIT READERS", "the readers' projection is valid (map size known)");
    // ViewDepth (c54) of the casters' centre = the eye distance (the centre sits at view z = D)
    const float p[4] = {in.center[0], in.center[1], in.center[2], 1.f};
    float d = 0.f;
    for (int k = 0; k < 4; ++k) d += rows.view_depth[k] * p[k];
    Check(std::fabs(d - kShadowFitEyeDistance) < 0.5f, "FIT DEPTH", "the casters' centre is at distance %.2f from the eye (D = %.2f)", d, kShadowFitEyeDistance);
    // ShadowPrm = (near, far, 4/(far-near), 0.7) as the game's F[0..4]
    Check(std::fabs(rows.shadow_prm[0][2] - 4.f / (f.far_d - f.near_d)) < 1e-6f && rows.shadow_prm[0][0] == f.near_d && rows.shadow_prm[0][1] == f.far_d, "FIT PRM", "ShadowPrm row 0 = (%.2f, %.2f, %.5f, %.2f)", rows.shadow_prm[0][0], rows.shadow_prm[0][1], rows.shadow_prm[0][2], rows.shadow_prm[0][3]);
    // the centre lands in the middle of the map; the sphere's rim lands inside it (the squeeze is the zoom to the bounds)
    const auto project = [&](const float q[3], float& u, float& v, float& depth01) {
      float lit[4], clip[4];
      for (int i = 0; i < 4; ++i) lit[i] = rows.view_proj[i][0] * q[0] + rows.view_proj[i][1] * q[1] + rows.view_proj[i][2] * q[2] + rows.view_proj[i][3];
      for (int i = 0; i < 4; ++i) clip[i] = rows.trapezoid[i][0] * lit[0] + rows.trapezoid[i][1] * lit[1] + rows.trapezoid[i][2] * lit[2] + rows.trapezoid[i][3] * lit[3];
      u = clip[0] / clip[3], v = clip[1] / clip[3];
      depth01 = clip[2] / clip[3];
    };
    float u, v, z;
    project(in.center, u, v, z);
    Check(std::fabs(u) < 1e-3f && std::fabs(v) < 1e-3f && z > 0.f && z < 1.f, "FIT CENTRE", "the centre projects to NDC (%.5f %.5f), depth %.3f", u, v, z);
    // points on the bounding sphere along the light's right / up axes must be inside the map but fill most of it (zoomed to the bounds)
    const float R = in.radius;
    const float rl = std::sqrt(f.fwd[0] * f.fwd[0] + f.fwd[2] * f.fwd[2]);
    const float right[3] = {f.fwd[2] / rl, 0.f, -f.fwd[0] / rl};
    const float q[3] = {in.center[0] + right[0] * R, in.center[1] + right[1] * R, in.center[2] + right[2] * R};
    project(q, u, v, z);
    const bool inside = std::fabs(u) < 1.f && std::fabs(v) < 1.f;
    const bool fills = std::fabs(u) > 0.7f;
    Check(inside, "FIT BOUNDS", "a point on the sphere (radius %.0f) lands inside the map: NDC %.3f %.3f", R, u, v);
    Check(fills, "FIT BOUNDS", "... and fills the map (|u| %.3f > 0.7): the squeeze is the zoom to the bounds", std::fabs(u));
    // the readers' lookup: the TexProjLight of the centre = the middle of the texture (bias includes the half texel), w = the eye distance
    float tx = 0, ty = 0, tw = 0;
    for (int k = 0; k < 4; ++k) tx += cam.tex_proj_light[0][k] * p[k], ty += cam.tex_proj_light[1][k] * p[k], tw += cam.tex_proj_light[3][k] * p[k];
    const float uu = tx / tw, vv = ty / tw;
    Check(std::fabs(uu - 0.5f) < 0.01f && std::fabs(vv - 0.5f) < 0.01f, "FIT READERS", "the readers' lookup of the centre is the middle of the map: u %.4f v %.4f (the half texel is in the bias)", uu, vv);
  }

  // 3. refusals: a vertical key light has no azimuth; a NaN is refused; a bound radius of zero still gives a usable zoom
  {
    ShadowFitInput in;
    in.key_vec[0] = 0.f, in.key_vec[1] = 1.f, in.key_vec[2] = 0.f;
    ShadowFit f;
    const char* why = "";
    bool r1 = FitShadowLight(in, f, why, ShadowFitFaults());
    Check(!r1, "FIT REFUSAL", "a vertical key light is refused (%s)", why);
    in.key_vec[0] = NAN;
    r1 = FitShadowLight(in, f, why, ShadowFitFaults());
    Check(!r1, "FIT REFUSAL", "a NaN key light is refused (%s)", why);
    in.key_vec[0] = -0.25f, in.key_vec[2] = -0.78f, in.radius = 0.f;
    r1 = FitShadowLight(in, f, why, ShadowFitFaults());
    Check(r1 && f.scale > 0.f && f.scale <= kShadowFitMaxScale, "FIT BOUNDS", "a zero radius still gives a usable zoom (%.1f)", f.scale);
  }
  // 4. the radius is used: a big scene must zoom out relative to a small one (a no_bounds plant gives both the same zoom)
  {
    ShadowFitInput a, b;
    for (ShadowFitInput* s : {&a, &b}) std::memcpy(s->key_vec, kRefs[1].key, sizeof s->key_vec);
    a.radius = 100.f, b.radius = 400.f;
    ShadowFit fa, fb;
    const char* why = "";
    FitShadowLight(a, fa, why, faults, main_forward), FitShadowLight(b, fb, why, faults, main_forward);
    Check(fa.scale > 1.5f * fb.scale, "FIT BOUNDS", "a 400-unit scene zooms out (squeeze %.1f) against a 100-unit one (%.1f)", fb.scale, fa.scale);
  }

  if (!plant.empty()) {
    std::printf("%s planted %s: %s\n", g_fail ? "PASS" : "FAIL", plant.c_str(), g_fail ? (std::string("caught: ") + g_token).c_str() : "NOT caught: the unit is blind to it");
    return g_fail ? 0 : 1;
  }
  std::printf("%s  e3_unit\n", g_fail ? "FAIL" : "PASS");
  return g_fail ? 1 : 0;
}
