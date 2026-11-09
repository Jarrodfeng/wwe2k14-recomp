// Session 98 (phone port, stage A): see render_shared.h. The bodies below moved here unchanged from shadow.cpp (ShadowMaps::Prepare /
// SetRim) and post.cpp (PostChain::Run's constants and pass order).
#include "render_shared.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace view {

static Mat4 OrthoLH(float l, float r, float b, float t, float n, float f) {
  return {{2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, 0, 0, 1 / (f - n), 0, -(r + l) / (r - l), -(t + b) / (t - b), -n / (f - n), 1}};
}

void ShadowSetup::Prepare(const std::vector<ShadowBox>& wr, bool props, bool wrestler_map, bool plane, bool blobs, const float cam_fwd[3]) {
  ShadowCB& c = cb_;
  memset(&c, 0, sizeof c);
  // props map: top-down, the game's fit (+-262 x +-212), centred on the ring (origin) [U: which world axis is which]
  {
    Mat4 v = LookAt({0, 3000, 0}, {0, 0, 0}, {0, 0, 1});
    props_vp_ = Mul(v, OrthoLH(-262, 262, -212, 212, 1, 6000));
    memcpy(c.props_vp, props_vp_.m, sizeof c.props_vp);
    c.props_p[0] = props ? 1.f : 0.f; c.props_p[1] = 0.2f; c.props_p[2] = 8.f; c.props_p[3] = 220.f;  // strength c168.w 0.2 (ShadowTerm); gap / fade: ours [U]
  }
  if (wr.empty()) return;
  float fx = 0, fz = 0, fy = 0, lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
  for (auto& b : wr) {
    fx += (b.mn[0] + b.mx[0]) * 0.5f; fz += (b.mn[2] + b.mx[2]) * 0.5f; fy += b.mn[1];
    lo[0] = std::min(lo[0], b.foot[0]); hi[0] = std::max(hi[0], b.foot[0]); lo[1] = std::min(lo[1], b.foot[2]); hi[1] = std::max(hi[1], b.foot[2]);
  }
  const float n = (float)wr.size();
  fx /= n; fz /= n; fy /= n;
  // wrestlers' light: directional, 31 degrees elevation, travelling DOWN and AWAY from the camera: its azimuth is the main camera's
  // forward direction (Session 98 wrestler fix 2, probe run 6816: c166.xz = 0.857 x the horizontal of the camera's forward row c59 in
  // 8341 / 8941 / 10741 / 11341 / 12841-14041 (exact to 3 digits; 10141 within 0.02), c166.y = 0.515 in the file's -Y-up space). The
  // match's (0.857, 0.515, 0) is its camera looking along +x. Shadows fall behind the wrestlers, the camera-facing side is lit.
  {
    float hx = cam_fwd ? cam_fwd[0] : 1.f, hz = cam_fwd ? cam_fwd[2] : 0.f;
    const float hl = std::sqrt(hx * hx + hz * hz);
    if (hl > 1e-4f) { hx /= hl; hz /= hl; } else { hx = 1.f; hz = 0.f; }
    V3 T = Norm({0.857f * hx, -0.515f, 0.857f * hz});  // direction the light travels (viewer space, Y up)
    V3 ctr = {fx, fy + 90.f, fz};
    V3 eye = {ctr.x - T.x * 2500.f, ctr.y - T.y * 2500.f, ctr.z - T.z * 2500.f};
    Mat4 v = LookAt(eye, ctr, {0, 0, 1});
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (auto& b : wr)
      for (int k = 0; k < 8; k++) {
        float p[3] = {(k & 1 ? b.mx[0] : b.mn[0]), (k & 2 ? b.mx[1] : b.mn[1]), (k & 4 ? b.mx[2] : b.mn[2])};
        for (int a = 0; a < 3; a++) {
          float r = p[0] * v.m[a] + p[1] * v.m[4 + a] + p[2] * v.m[8 + a] + v.m[12 + a];
          mn[a] = std::min(mn[a], r); mx[a] = std::max(mx[a], r);
        }
      }
    const float mg = 40.f;
    // The shadow of the tallest box reaches height / sin(31 deg) along the light: the far plane must cover the floor beyond it.
    float hmax = 0.f;
    for (auto& b : wr) hmax = std::max(hmax, b.mx[1] - b.mn[1]);
    float zn = mn[2] - 150.f, zf = mx[2] + hmax / 0.515f + 150.f;
    wr_vp_ = Mul(v, OrthoLH(mn[0] - mg, mx[0] + mg, mn[1] - mg, mx[1] + mg, zn, zf));
    memcpy(c.wr_vp, wr_vp_.m, sizeof c.wr_vp);
    c.wr_p[0] = 1.f / 1024.f; c.wr_p[1] = 3.0f / (zf - zn); c.wr_p[2] = 0.55f; c.wr_p[3] = wrestler_map ? 1.f : 0.f;  // strength: ours [U]
    c.wr_l[0] = -T.x; c.wr_l[1] = -T.y; c.wr_l[2] = -T.z; c.wr_l[3] = zf - zn;
    // the wrestlers' own receivers: the game's c168.w / c169.y (0.7 / 0.2 in every probed draw of run 6816, entrance and match)
    c.wr_r[0] = 0.7f; c.wr_r[1] = 0.2f;
  }
  // plane shadow: the point light (-450, 1400 above the plane, 0) over the canvas; the plane is where the wrestlers stand
  {
    c.plane_r[0] = (lo[0] + hi[0]) * 0.5f; c.plane_r[1] = (lo[1] + hi[1]) * 0.5f;
    c.plane_r[2] = std::min(std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]) * 0.5f + 160.f, 260.f), 800.f);
    c.plane_r[3] = plane ? 1.f : 0.f;
    // (the light's height above the plane is per model: the plane pass takes each wrestler's own standing height; x, z are the game's)
    c.plane_l[0] = -450.f; c.plane_l[1] = 1280.f; c.plane_l[2] = 0.f; c.plane_l[3] = 0.f;
    // strength 1: the ring mat (yProp_PSMLM 4E1A94BC) writes lit x (c255.x - mask) with c255.x = 1.0 [C: capture rp_ring (run 6818)
    // event 6204, its pixel constants]; the mask's own fade (viewer_hlsl.h vs_plane) is the game's. Was 0.6 x a flat mask (ours, U).
    c.plane_p[0] = 1.f; c.plane_p[1] = 6.f;
  }
  // circle blobs under the true feet; the same list is the set of standing heights the canvas / plane receivers accept
  {
    int k = 0;
    for (auto& b : wr) {
      if (k >= 6) break;
      c.blobs[k][0] = b.foot[0]; c.blobs[k][1] = b.foot[1]; c.blobs[k][2] = b.foot[2]; c.blobs[k][3] = 50.f;
      k++;
    }
    c.blob_p[0] = (float)k; c.blob_p[1] = blobs ? 0.3f : 0.f; c.blob_p[2] = 6.f;  // strength c169.x 0.3 (ShadowTerm)
  }
}

void ShadowSetup::SetRim(bool on, float near_w, float far_w, int w, int h) {
  ShadowCB& c = cb_;
  if (far_w < near_w + 1.f) far_w = near_w + 1.f;
  c.rim_p[0] = near_w; c.rim_p[1] = far_w;
  // c162.z = (c162.y - c162.x) / 1000 in all five probed frames of run 6816 (8341 / 8941 / 10141 / 10741 / 12841, 4 digits): the
  // depth step is in world units / 1000.
  c.rim_p[2] = (far_w - near_w) / 1000.f;
  c.rim_p[3] = on ? 1.f : 0.f;
  c.rim_s[0] = 1.f / (float)std::max(w, 1); c.rim_s[1] = 1.f / (float)std::max(h, 1); c.rim_s[2] = c.rim_s[3] = 0.f;
}

const char* PostShaderEntry(PostShader s) {
  switch (s) {
    case PostShader::kDownNear: return "psDownNear";
    case PostShader::kDownFar: return "psDownFar";
    case PostShader::kBlur13: return "psBlur13";
    case PostShader::kBright: return "psBright";
    case PostShader::kBlur15: return "psBlur15";
    case PostShader::kGlare: return "psGlare";
    default: return "psCombine";
  }
}

std::vector<PostStep> BuildPostPlan(const PostParams& p, int w_, int h_) {
  PostCB c = {};
  // The game's own constants (docs\systems\post_effects.md; match run 4511, entrance Bryan run 4522).
  if (p.entrance) {
    float a[4] = {1e30f, 100.7f, 0, 0.7f}, b[4] = {0, 0.985158f, 0, 0};
    if (p.ent_far_w > 0.f) {
      // Session 98 (stage fix): the game's entrance rule (probe runs 6816 + 6818, every entrance frame; Bryan's run 4522 too): the start
      // follows the shot (renderer.cpp Frame), the blur factor reaches 1.0 exactly 1350 view units past the start (linear in the game's
      // depth between the two), capped at c20.w. The cap is 0.6 in 13 of Orton's 19 probed frames (0.5 / 0.8 in others, Bryan 0.7,
      // Cena 0.7 / 0.8): its rule is unconfirmed, 0.6 kept. (Was: full at 1.88 x the start with cap 0.7: 1.4-1.6x too blurred.)
      const float zs = 1.f - 9.92f / p.ent_far_w, ze = 1.f - 9.92f / (p.ent_far_w + 1350.f);
      b[1] = zs; a[1] = 1.f / std::max(ze - zs, 1e-6f); a[3] = 0.6f;
    }
    memcpy(c.c20, a, 16); memcpy(c.c21, b, 16);
  } else {
    float a[4] = {91.33f, 338.67f, 0.992f, 0.8f}, b[4] = {0.991855f, 0.99509f, 0, 0};
    memcpy(c.c20, a, 16); memcpy(c.c21, b, 16);
  }
  const float hc[4] = {0.4f, 0.55f, 0.55f, 1}, hp[4] = {0.08f, 0.993f, 0, 0};
  memcpy(c.haze_col, hc, 16); memcpy(c.haze_p, hp, 16);
  c.size[0] = 1.f / w_; c.size[1] = 1.f / h_; c.size[2] = (float)w_; c.size[3] = (float)h_;
  c.proj[0] = p.q; c.proj[1] = p.nq; c.proj[2] = 9.92f;
  c.proj[3] = 1.0f - c.c21[1];  // a tap is "far" when its game depth is under 1 - c21.y (view w past ~2000 units)
  c.flags[0] = p.fxaa ? 1.f : 0.f; c.flags[1] = p.dof ? (p.dof_near ? 2.f : 1.f) : 0.f; c.flags[2] = p.haze ? 1.f : 0.f; c.flags[3] = p.gamma ? 1.f : 0.f;
  const bool bl = p.entrance && (p.bloom != 0.f || p.glare != 0.f);
  c.bloom_p[0] = p.bloom; c.bloom_p[1] = p.glare; c.bloom_p[2] = p.bloom_threshold; c.bloom_p[3] = bl ? 1.f : 0.f;
  if (p.entrance) memcpy(c.film, p.film, sizeof c.film);  // (the match has no film quad: probe run 6816, frames 12841-17641)

  using T = PostTarget;
  using S = PostShader;
  std::vector<PostStep> out;
  auto push = [&](float px, float py, float gated) {
    PostCB cur = c;
    cur.pass[0] = px; cur.pass[1] = py; cur.pass[2] = gated;
    return cur;
  };
  auto step = [&](S ps, T dst, std::initializer_list<T> src, const PostCB& cb) {
    PostStep s;
    s.ps = ps, s.dst = dst, s.cb = cb;
    for (T t : src) s.src[s.nsrc++] = t;
    out.push_back(s);
  };
  auto mark = [&](const char* name) { PostStep s; s.mark = name; out.push_back(s); };
  const int qw = PostQuarter(w_), qh = PostQuarter(h_), ew = PostEighth(w_), eh = PostEighth(h_);

  if (p.dof) {
    step(S::kDownNear, T::kNearA, {T::kScene, T::kDepth}, push(0, 0, 0));
    step(S::kDownFar, T::kFarA, {T::kScene, T::kDepth}, push(0, 0, 0));
    step(S::kBlur13, T::kNearB, {T::kNearA}, push(1.f / qw, 1.f / qh, 0));
    step(S::kBlur13, T::kFarB, {T::kFarA}, push(1.f / qw, 1.f / qh, 1));
  }
  mark("dof_blurs");

  if (bl) {
    step(S::kBright, T::kBright, {T::kScene}, push(0, 0, 0));
    // blur chain: 13-tap, then 15-tap horizontal and vertical (the game's order)
    step(S::kBlur13, T::kBlA, {T::kBright}, push(1.f / ew, 1.f / eh, 0));
    step(S::kBlur15, T::kBlB, {T::kBlA}, push(1.f / ew, 0, 0));
    step(S::kBlur15, T::kBlA, {T::kBlB}, push(0, 1.f / eh, 0));  // kBlA = the finished bloom
    if (p.glare != 0.f) {
      PostCB cur = c;
      cur.size[0] = 1.f / ew; cur.size[1] = 1.f / eh;  // (the glare steps in eighth-size texels)
      step(S::kGlare, T::kGlare, {T::kBright}, cur);
    }
  }
  mark("bloom_glare");

  step(S::kCombine, T::kOut, {T::kScene, T::kDepth, T::kNearB, T::kFarB, T::kBlA, T::kGlare}, push(0, 0, 0));
  mark("combine");
  return out;
}

std::string TimingJson(int w, int h, int counted, double total, const std::vector<std::pair<std::string, double>>& sum) {
  char b[64];
  std::string s = "{\"ok\":true,\"w\":" + std::to_string(w) + ",\"h\":" + std::to_string(h) + ",\"frames\":" + std::to_string(counted);
  snprintf(b, sizeof b, "%.3f", total / counted);
  s += std::string(",\"gpu_ms_total\":") + b + ",\"passes_ms\":{";
  for (size_t i = 0; i < sum.size(); i++) {
    snprintf(b, sizeof b, "%.3f", sum[i].second / counted);
    s += std::string(i ? "," : "") + "\"" + sum[i].first + "\":" + b;
  }
  return s + "}}";
}

}  // namespace view
