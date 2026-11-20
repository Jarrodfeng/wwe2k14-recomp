// body-scale: none (tests the post chain; a record wrestler without a scale is a hole, as live)  [S103 RGF: body_scale_feed_lint.py]
// post_rig: lane P's offline rig (Session 103; docs\renderer\POST_FROM_RECORD.md). One scene record drawn from the record alone (as frame_rig does), with a post row
// put INTO the record (a test record: the captured frames carry no row; the rows are made by post_rig.py from the capture's own post draws, in the TEST only, never live),
// written to a file and read back (the record path), then drawn twice: without post and through src\renderer_scene\post_from_record.h -> FrontEnd -> PostPass.
//   post_rig --record F.rsc --game DIR --size WxH --prefix P --camera 16f --eye 3f [--row words.bin] [--plant swap_dof|no_depth_terms] [--no-post]
// Writes P_nopost.png, P_post.png (when a row builds params), P_rec.rsc, and prints one "params ..." line (the mapped PostParams scalars) and one "info ..." line.
// Exit 0 ok, 2 bad input, 3 failure.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "png_out.h"
#include "../../src/renderer/combine_lite.h"   // S103 SLV
#include "../../src/renderer/renderer.h"
#include "../../src/renderer_scene/front_end.h"
#include "../../src/renderer_scene/live_assets.h"
#include "../../src/renderer_scene/post_from_record.h"
#include "../../src/renderer_scene/post_pass.h"   // S103 SLV: the census counters
#include "../../src/renderer_scene/record_frame.h"
#include "../../src/renderer_scene/record_io.h"
#include "../../src/renderer_scene/rf_c.h"
#include "../../src/scene_live/post_settings.h"
#include "../../src/scene_live/gsf_latch.h"   // S103 FXF: Classify
#include "../../src/start_settings.h"  // S103 FXF: g_quality (the VINTAGE LOOK flag of the tint rule)

using namespace rdr_scene;

static std::vector<float> ParseFloats(const char* s) {
  std::vector<float> v;
  for (const char* p = s; *p;) {
    char* e = nullptr;
    v.push_back(std::strtof(p, &e));
    if (e == p) break;
    p = (*e == ',') ? e + 1 : e;
  }
  return v;
}

// ---- S103 PST: the tonemap + night + antialiasing proofs (docs\research\s103\COV9_story.md 2a / 2e). Offline, headless, 0 game runs: planted post records (flags put in PostParams directly, the way post_from_record maps S+180 /
// +184 / +280) drawn through FrontEnd -> PostPass over one scene, a second scene (the same camera zoomed) for the brightness step. The adapted luminance is read back THROUGH THE PICTURE: the tonemap is
// out = s / (s + 1), s = c * 0.18 / (L + 0.001) per channel, so L = c * 0.18 / s - 0.001 on flat pixels of the tonemap-off picture c (averaged over thousands of channel samples).
namespace {
using Px = std::vector<uint8_t>;
bool Flat(const Px& px, uint32_t w, uint32_t h, uint32_t x, uint32_t y, int rad, int tol) {   // every channel of the (2 rad + 1)^2 window within `tol` levels
  if (x < uint32_t(rad) || y < uint32_t(rad) || x + rad >= w || y + rad >= h) return false;
  for (int ch = 0; ch < 3; ++ch) {
    int lo = 255, hi = 0;
    for (int dy = -rad; dy <= rad; ++dy)
      for (int dx = -rad; dx <= rad; ++dx) {
        const int v = px[(size_t(y + dy) * w + (x + dx)) * 4 + ch];
        lo = std::min(lo, v), hi = std::max(hi, v);
      }
    if (hi - lo > tol) return false;
  }
  return true;
}
double LogAvgLuma(const Px& px, uint32_t w, uint32_t h) {   // the CPU's own log-average luma of the QUARTER picture (4x4 box average first, as the DoF quarter downsample the game's measure reads), every quarter texel instead of 9 x 64 x 64 taps
  double s = 0;
  size_t n = 0;
  for (uint32_t by = 0; by + 4 <= h; by += 4)
    for (uint32_t bx = 0; bx + 4 <= w; bx += 4) {
      double c[3] = {0, 0, 0};
      for (uint32_t y = 0; y < 4; ++y)
        for (uint32_t x = 0; x < 4; ++x)
          for (int ch = 0; ch < 3; ++ch) c[ch] += px[(size_t(by + y) * w + (bx + x)) * 4 + ch];
      s += std::log((0.2125 * c[0] + 0.7154 * c[1] + 0.0721 * c[2]) / (16.0 * 255.0) + 1e-4), ++n;
    }
  return std::exp(s / double(n ? n : 1));
}
size_t GoodPixels(const Px& px, uint32_t w, uint32_t h) {   // flat pixels with a mid-range channel: the inversion's samples
  size_t n = 0;
  for (uint32_t y = 1; y + 1 < h; ++y)
    for (uint32_t x = 1; x + 1 < w; ++x)
      if (Flat(px, w, h, x, y, 1, 1))
        for (int ch = 0; ch < 3; ++ch) { const double c = px[(size_t(y) * w + x) * 4 + ch] / 255.0; n += c >= 0.2 && c <= 0.9; }
  return n;
}
double InvertL(const Px& base, const Px& tone, uint32_t w, uint32_t h, size_t& n) {   // the adapted luminance the combine used, from the tonemap-off picture and the tonemap-on one (<0: no samples)
  double sum = 0;
  n = 0;
  for (uint32_t y = 1; y + 1 < h; ++y)
    for (uint32_t x = 1; x + 1 < w; ++x) {
      if (!Flat(base, w, h, x, y, 1, 1)) continue;
      for (int ch = 0; ch < 3; ++ch) {
        const size_t o = (size_t(y) * w + x) * 4 + ch;
        const double c = base[o] / 255.0, out = tone[o] / 255.0;
        if (c < 0.2 || c > 0.9 || out < 0.05 || out > 0.9) continue;
        sum += c * 0.18 / (out / (1.0 - out)) - 0.001, ++n;
      }
    }
  return n ? sum / double(n) : -1.0;
}
double MeanAbs(const Px& a, const Px& b) {
  double s = 0;
  for (size_t i = 0; i < a.size() && i < b.size(); ++i) s += (i % 4 == 3) ? 0 : std::abs(int(a[i]) - int(b[i]));
  return s / (double(a.size()) * 0.75);
}

// S103 PST fix (review gap 7): post_from_record::Build on planted S rows, no GPU: tonemap-only / night-only / AA-only frames, the game's AA skip rule (exact == 1 words), S+284 ignored (the pass is always `fixed`),
// the logical-size tap offsets (1/lw, never finer than one drawn texel), the halation census words.
int PstBuildTest(const std::string& plant) {
  int rc = 0;
  const auto check = [&](bool ok, const char* what) { std::printf("pst-build %s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) rc = 3; };
  struct Case { std::vector<std::pair<uint32_t, uint32_t>> words; uint32_t w, h; };   // (byte offset of S, value)
  const auto build = [&](const std::vector<std::pair<uint32_t, uint32_t>>& words, uint32_t w, uint32_t h, PostParams& pp, post_from_record::Info& info) {
    SceneTables tt;
    tt.header.guest_frame = 100;
    PostParamsRow row;
    row.flags = kPpRead, row.frame = 100;
    row.s[0] = 1;   // S+0: the module on
    for (const auto& kv : words) row.s[kv.first / 4] = kv.second;
    tt.post_params.push_back(row);
    return post_from_record::Build(tt, Camera(), w, h, pp, info);
  };
  PostParams pp;
  post_from_record::Info in;
  bool b = build({{180, 1}}, 1280, 720, pp, in);   // tonemap only (no DoF, fog, bloom: no depth needed)
  check(b && pp.tonemap && !pp.night && !pp.aa && !pp.dof && !pp.fog && !pp.bloom && in.unsupported == 0, "tonemap-only frame builds, only tonemap set, not flagged unsupported");
  b = build({{184, 1}}, 1280, 720, pp, in);
  check(b && !pp.tonemap && pp.night && !pp.aa && in.unsupported == 0, "night-only frame builds, only night set");
  b = build({{280, 1}}, 1280, 720, pp, in);
  check(b && pp.aa && !pp.tonemap && !pp.night && std::fabs(pp.aa_offsets[0] - 1.f / 1280.f) < 1e-9 && std::fabs(pp.aa_offsets[1] - 1.f / 720.f) < 1e-9, "AA-only frame builds, offsets (1/1280, 1/720)");
  b = build({{280, 1}}, 2560, 1440, pp, in);
  check(b && pp.aa && std::fabs(pp.aa_offsets[0] - 1.f / 1280.f) < 1e-9 && std::fabs(pp.aa_offsets[1] - 1.f / 720.f) < 1e-9, "AA at render scale 2: the offsets stay the GAME picture's (1/1280, 1/720), not 1/2560");
  b = build({{280, 1}}, 640, 360, pp, in);
  check(b && pp.aa && std::fabs(pp.aa_offsets[0] - 1.f / 640.f) < 1e-9 && std::fabs(pp.aa_offsets[1] - 1.f / 360.f) < 1e-9, "AA below the game's size: never finer than one drawn texel (1/640, 1/360)");
  post_from_record::Info i2;
  PostParams p2;
  build({{280, 1}, {284, 0x3F000000u}}, 1280, 720, p2, i2);   // S+284 = 0.5
  b = build({{280, 1}}, 1280, 720, pp, in);
  check(i2.aa && in.aa && p2.aa_offsets[0] == pp.aa_offsets[0] && p2.aa_offsets[1] == pp.aa_offsets[1], "S+284 (the threshold word, no writer) does not change the build: the pass is always `fixed`");
  // the skip rule: S+4 == 1, S+176 == 1, S+180 == 0, S+184 == 0, S+208 == 1 (DoF + bloom + monotone without HDR); needs a perspective camera for the DoF, so the rule is read from Info only
  Camera cam;   // identity: ProjQ fails, Build reports missing; the AA decision is made before it (info.aa / aa_skipped are set)
  const auto skip = [&](std::vector<std::pair<uint32_t, uint32_t>> w) {
    w.push_back({280, 1});
    PostParams q;
    post_from_record::Info qi;
    build(w, 1280, 720, q, qi);
    return qi;
  };
  post_from_record::Info s1 = skip({{4, 1}, {176, 1}, {208, 1}});
  check(s1.aa_skipped && !s1.aa, "DoF + bloom + monotone, no HDR: the game skips the antialiasing");
  s1 = skip({{4, 1}, {176, 1}, {208, 1}, {180, 1}});
  check(!s1.aa_skipped && s1.aa, "tonemap on breaks the skip rule: AA runs");
  s1 = skip({{4, 1}, {176, 1}, {208, 1}, {184, 1}});
  check(!s1.aa_skipped && s1.aa, "night on breaks the skip rule: AA runs");
  s1 = skip({{4, 2}, {176, 1}, {208, 1}});
  check(!s1.aa_skipped && s1.aa, "the rule is an exact == 1 test (S+4 == 2 does not skip)");
  s1 = skip({{4, 1}, {176, 1}});
  check(!s1.aa_skipped && s1.aa, "no monotone: AA runs");
  (void)cam;
  // S103 HCN (HRB.md 5): the census counts the game's gate (S+0 && S+304 && S+308; radial = gate && S+524 > 0), not the raw ctor defaults. --pst-plant hala-word = the old rule (S+304 alone): case (a) must FAIL.
  uint32_t rd = 0, e5 = 0, un = 0;
  const auto hala = [&](std::vector<std::pair<uint32_t, uint32_t>> w) {
    PostParams q;
    post_from_record::Info qi;
    build(w, 1280, 720, q, qi);
    rd = (qi.dead_flags & kPpRadial) ? 1 : 0, e5 = (qi.dead_flags & kPpHala300) ? 1 : 0, un = (qi.unsupported & (kPpHalation | kPpRadial)) ? 1 : 0;
    uint32_t s304 = 0;
    for (const auto& kv : w) if (kv.first == 304) s304 = kv.second;
    return plant == "hala-word" ? (s304 != 0 ? 1u : 0u) : ((qi.dead_flags & kPpHalation) ? 1u : 0u);
  };
  uint32_t hr = hala({{304, 1}, {308, 0}, {524, 3}, {300, 0}});   // (a) the ctor row, and run 018's census line (S+304 13909 with S+308 0)
  check(hr == 0 && rd == 0 && un == 0, "(a) ctor row (S+304 = 1, S+308 = 0, S+524 = 3): halation runs 0, radial 0, unsupported 0 (run 018: S+304 13909 with S+308 0 reads 0)");
  hr = hala({{304, 1}, {308, 1}, {524, 3}});
  check(hr == 1 && rd == 1 && un == 1, "(b) S+308 = 1: halation runs 1, radial 1, flag in unsupported");
  hr = hala({{304, 0}, {308, 1}, {524, 3}});
  check(hr == 0 && rd == 0, "(c) S+308 = 1 with S+304 = 0: 0 (both words gate)");
  hr = hala({{0, 0}, {304, 1}, {308, 1}, {524, 3}});
  check(hr == 0 && rd == 0, "(d) S+0 = 0: 0 (the walk is off)");
  hr = hala({{300, 1}, {304, 1}, {308, 0}, {524, 3}});
  check(hr == 0 && e5 == 1, "(e) S+300 = 1 alone: halation 0, effect-5 count 1");
  hr = hala({{304, 1}, {308, 1}, {524, 0}});
  check(hr == 1 && rd == 0, "(f) gate open with S+524 = 0: halation 1, radial 0");
  return rc;
}

int PstTest(FrontEnd& fe, rf::ModelCache& cache, const SceneTables& t, uint32_t w, uint32_t h, const PostParams& pp0, const std::string& plant, const std::string& prefix) {
  std::string err;
  int rc = 0;
  std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not eat the progress lines
  const auto fail = [&](const char* what) { std::printf("pst-test FAIL %s\n", what); rc = 3; };
  std::deque<SceneTables> kept;   // a SceneFrame may point into the tables it was built from: each copy lives to the end of the test
  const auto frame_for = [&](float zoom, SceneFrame& out) {
    kept.push_back(t);
    SceneTables& tt = kept.back();
    if (zoom != 1.f)
      for (int k : {0, 4, 8, 12, 1, 5, 9, 13}) tt.camera.view.m[k] *= zoom;   // clip.x / clip.y scaled about the centre: the same scene zoomed
    std::vector<rf::ClassReport> rr;
    rf::c::SetOfflineLoad(true);
    return rf::BuildFrame(tt, cache, rf::kAllFamilies, w, h, out, rr, err);
  };
  PostParams base = pp0;   // no effect on: the combine's own smoothing only (the tonemap-off picture; the inversion's c)
  base.dof = base.fog = base.bloom = base.glare = base.bloom_variant = base.dof_chain = base.film = base.ghost = false;
  base.tonemap = base.night = base.aa = false;
  base.depth_from_viewer = false;
  uint64_t gf = 0;
  const auto draw = [&](SceneFrame& fr, PostParams pp, uint64_t g, Px& px) {   // pp by value: guest_frame set here
    pp.guest_frame = gf = g;
    fr.post = &pp;
    const bool ok = fe.RenderToPixels(fr, px, err);
    fr.post = nullptr;
    return ok;
  };
  const auto save = [&](const char* name, const Px& px) { scene_png::WritePngRgb(prefix + "_pst_" + name + ".png", px, w, h); };
  // 1. The two scenes. X = the camera zoomed by the factor whose picture's log-average luma differs most from A's (the brightness step the convergence needs), with enough mid-range flat pixels to read the adapted value from.
  SceneFrame A, X;
  Px noA, noX, tmp;
  if (!frame_for(1.f, A)) { std::fprintf(stderr, "post_rig: FAIL pst build A: %s\n", err.c_str()); return 3; }
  if (!fe.RenderToPixels(A, noA, err)) { std::fprintf(stderr, "post_rig: FAIL pst draw A: %s\n", err.c_str()); return 3; }
  const double LA_cpu = LogAvgLuma(noA, w, h);
  float best_zoom = 0.f;
  double best = 0.0;
  for (float z : {2.f, 3.f, 5.f, 8.f, 0.6f}) {
    SceneFrame c;
    if (!frame_for(z, c) || !fe.RenderToPixels(c, tmp, err)) continue;
    const double r = std::abs(std::log(LogAvgLuma(tmp, w, h) / LA_cpu));
    if (GoodPixels(tmp, w, h) >= size_t(w) * h / 400 && r > best) best = r, best_zoom = z;
  }
  if (best_zoom == 0.f || best < 0.2 || GoodPixels(noA, w, h) < size_t(w) * h / 400) { std::printf("pst-test FAIL scenes: no zoom of this frame changes the log-average luma by 20 %% with enough flat mid-range pixels (best %.3f), or A has too few (%zu)\n", best, GoodPixels(noA, w, h)); return 3; }
  if (!frame_for(best_zoom, X) || !fe.RenderToPixels(X, noX, err)) { std::fprintf(stderr, "post_rig: FAIL pst draw X: %s\n", err.c_str()); return 3; }
  const double LX_cpu = LogAvgLuma(noX, w, h);
  std::printf("pst-test scenes: B = zoom %.1f; CPU log-average luma A %.5f B %.5f (ln ratio %.3f); flat mid-range samples A %zu B %zu\n", best_zoom, LA_cpu, LX_cpu, std::log(LX_cpu / LA_cpu), GoodPixels(noA, w, h), GoodPixels(noX, w, h));
  Px bA, bX;   // tonemap-off pictures (the base combine: the same smoothing as every on picture)
  if (!draw(A, base, 1, bA) || !draw(X, base, 1, bX)) { std::fprintf(stderr, "post_rig: FAIL pst base draw: %s\n", err.c_str()); return 3; }
  save("A_off", bA), save("B_off", bX);

  // 2. TONEMAP on vs off, and the measurement's value against the CPU's own log-average luma.
  PostParams tm = base;
  tm.tonemap = plant != "tonemap";   // planted: the flag is ignored on the frame that must show it
  Px tA;
  if (!draw(A, tm, 100000, tA)) { std::fprintf(stderr, "post_rig: FAIL pst tonemap draw: %s\n", err.c_str()); return 3; }
  save("A_tonemap", tA);
  const double d_on_off = MeanAbs(bA, tA);
  size_t nA = 0;
  const double LA = InvertL(bA, tA, w, h, nA);
  std::printf("pst-test tonemap: mean |on - off| %.2f levels; the adapted luminance read from the picture L %.5f (%zu samples) vs the CPU's log-average luma %.5f (ratio %.3f)\n", d_on_off, LA, nA, LA_cpu, LA / LA_cpu);
  if (!(d_on_off >= 1.0)) fail("tonemap on equals tonemap off");
  if (!(LA > 0 && LA / LA_cpu > 0.6 && LA / LA_cpu < 1.6)) fail("the measured luminance is not the scene's log-average luma (pyramid)");

  // 3. The adapted 1x1: it follows 0.0396 per guest frame. A snaps (a frame number far BEHIND the last = a new scene), B at the first frame snaps to its own value, then A again, then B for 20 guest frames:
  //    the fraction of the way from L(A) to L(B) after n frames must be 1 - (1 - 0.0396)^n, ElapsedTime fixed at 1/15 (follow = 1 - 2^(-0.87439 / 15)).
  Px tX0, tA2;
  if (plant == "step") g_post_lum_plant_every.store(1);   // planted twin: every Record steps the 1x1 (a frame recorded twice steps twice)
  if (!draw(X, tm, 9000, tX0)) return 3;
  size_t nX = 0;
  const double LX = InvertL(bX, tX0, w, h, nX);
  if (!draw(A, tm, 2000, tA2)) return 3;
  const bool snap_same = tA2 == tA;
  std::printf("pst-test snap: B alone L %.5f (%zu samples); A again after a jump back is %s the first A picture\n", LX, nX, snap_same ? "identical to" : "DIFFERENT from");
  if (!snap_same && plant.empty()) fail("a fresh adaptation did not snap to the measured value deterministically");
  const double fol = 1.0 - std::exp2(-0.87439 / 15.0), r = 1.0 - fol;
  std::printf("pst-test follow: %.5f per guest frame (1 - 2^(-0.87439 / 15))\n", fol);
  Px prev, cur;
  double worst = 0;
  for (int n = 1; n <= 20; ++n) {
    if (!draw(X, tm, 2000 + n, cur)) return 3;
    if (n == 1 || n == 5 || n == 10 || n == 20) {
      size_t k = 0;
      const double Ln = InvertL(bX, cur, w, h, k);
      const double frac = (Ln - LA) / (LX - LA), want = 1.0 - std::pow(r, n);
      std::printf("pst-test converge n=%d: L %.5f, fraction of the way %.4f, expected %.4f (diff %+.4f)\n", n, Ln, frac, want, frac - want);
      if (n >= 5) worst = std::max(worst, std::abs(frac - want));
      if (n == 10) save("B_n10", cur);
    }
    if (n == 20) prev = cur;
  }
  if (!(worst < 0.05)) fail("the adapted luminance does not follow 0.0396 per guest frame");
  Px again, again2;
  if (!draw(X, tm, 2020, again) || !draw(X, tm, 2020, again2)) return 3;
  const bool idem = again == prev && again2 == prev;
  std::printf("pst-test repeat: guest frame 2020 recorded twice more: %s\n", idem ? "identical" : "DIFFERS");
  if (!idem) fail("a repeat of the same guest frame stepped the adapted luminance again (twin stepping twice per guest frame)");
  g_post_lum_plant_every.store(0);

  // 3b. PAUSED frames: the game steps the adapted luminance only while S+180 | S+184 is set. A snaps (gf 500, far behind), then 40 guest frames with the effect OFF (no flag: the Record pauses the step), then one
  //     frame ON: the step back is ONE guest frame (0.0396 of the way from A's value to B's), not the 41 - 30 composed frames (0.70). Planted twin --pst-plant pause ignores the pause.
  if (plant == "pause") g_post_lum_plant_nopause.store(1);
  Px pA, pOn;
  if (!draw(A, tm, 500, pA)) return 3;
  for (int n = 1; n <= 40; ++n)
    if (!draw(X, base, 500 + n, tmp)) return 3;
  if (!draw(X, tm, 541, pOn)) return 3;
  size_t kp = 0, kA = 0;
  const double LpA = InvertL(bA, pA, w, h, kA), Lp = InvertL(bX, pOn, w, h, kp);
  const double frac_p = (Lp - LpA) / (LX - LpA);
  std::printf("pst-test pause: after 40 guest frames with the effect off, the first step back moved %.4f of the way (expected %.4f; a composed 30-frame step would be %.4f)\n", frac_p, fol, 1.0 - std::pow(r, 30));
  if (!(std::abs(frac_p - fol) < 0.04)) fail("the adapted luminance stepped across the frames the effect was off (paused frames must not step)");
  g_post_lum_plant_nopause.store(0);

  // 4. NIGHT on vs off (a desaturating grade toward grey (1.05, 0.97, 1.27) by how dark the adapted luminance is).
  PostParams nt = base;
  nt.night = plant != "night";
  Px pNight;
  if (!draw(A, nt, 2021, pNight)) return 3;
  save("A_night", pNight);
  const double d_night = MeanAbs(bA, pNight);
  PostParams nm = tm;
  nm.night = true;
  Px tnA, tmA;
  if (!draw(A, nm, 2022, tnA) || !draw(A, tm, 2022, tmA)) return 3;   // (the same guest frame, so the 1x1 is the same for both)
  const double d_nt = MeanAbs(tmA, tnA);
  std::printf("pst-test night: mean |night - off| %.2f levels; night + tonemap vs tonemap alone %.2f levels\n", d_night, d_nt);
  if (!(d_night >= 0.5)) fail("night on equals night off");
  if (!(d_nt >= 0.3)) fail("night + tonemap equals tonemap alone");

  // 5. ANTIALIASING on vs off: edge pixels change, a 5x5-flat region (raw scene) does not. The uniform variant (S+284 = 0.05) and the fixed one (0.64) both draw.
  PostParams ap = base;
  ap.aa = plant != "aa";
  ap.aa_offsets[0] = 1.f / float(w), ap.aa_offsets[1] = 1.f / float(h);   // (the pass is always the `fixed` variant, threshold 0.64: the game's pass 0)
  Px aU, aF;
  if (!draw(A, ap, 2023, aU)) return 3;
  if (!draw(A, ap, 2024, aF)) return 3;   // a second draw: with the effect off the scene's own movement shows (the bar below)
  save("A_aa", aU);
  size_t changed = 0, changed_flat = 0, changed_fixed = 0;
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      bool d = false, df = false;
      for (int ch = 0; ch < 3; ++ch) d |= std::abs(int(aU[(size_t(y) * w + x) * 4 + ch]) - int(bA[(size_t(y) * w + x) * 4 + ch])) > 1, df |= aF[(size_t(y) * w + x) * 4 + ch] != bA[(size_t(y) * w + x) * 4 + ch];
      changed += d, changed_fixed += df;
      if (d && Flat(noA, w, h, x, y, 2, 0)) ++changed_flat;
    }
  std::printf("pst-test aa: pixels changed by the antialiasing (uniform, threshold 0.05) %zu, of those inside 5x5-flat regions of the raw scene %zu; fixed-threshold variant changed %zu\n", changed, changed_flat, changed_fixed);
  // (the scene moves a little between renders: with the effect off a re-draw differs in ~2-85 pixels (measured), with it on in 10-17 thousand: the bar is 500)
  if (changed < 500 || changed_fixed < 500) fail("antialiasing on equals antialiasing off");
  if (changed_flat != 0) fail("antialiasing changed pixels inside flat regions (it must only touch edges)");
  std::printf(rc == 0 ? "pst-test PASS\n" : "pst-test FAIL\n");
  return rc;
}

// ---- S103 FXH: the highlight-reel cue effects (defocus + hue S+560, emboss S+592, retro film S+604; docs\research\s103\COV9_story.md 2b / 2c / 2d). Offline, headless, 0 game runs.
// Every case goes through the production path: S words planted in a post row -> post_from_record::Build -> FrontEnd -> PostPass::Record (the passes in the game's order, the retro clock stepped by the pass on each
// guest frame it draws). The pictures are checked against CPU references written from the game's shader formulas (yEmboss.frag / yDefocus.frag / yRetroFilm.frag, proven against the game's microcode), never against the pass.
constexpr uint32_t kCueMaskAll = kPpDefocus | kPpEmboss | kPpRetro;
using Words = std::vector<std::pair<uint32_t, uint32_t>>;   // (byte offset of S, value)
uint32_t FxBits(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; }
Camera FxPerspCam() {   // a perspective world -> clip matrix: q = m10 / m11 = 1, nq = q m15 - m14 = -0.1 (post_from_record::ProjQ accepts it)
  Camera c;
  c.view = IdentityMat4();
  c.view.m[10] = 1.f, c.view.m[11] = 1.f, c.view.m[14] = 0.1f, c.view.m[15] = 0.f;
  c.flags = kCamViewIsViewProj;
  return c;
}
Words DefocusWords(float rate, float r, float g, float b, float a) { return {{560, 1}, {564, FxBits(rate)}, {576, FxBits(r)}, {580, FxBits(g)}, {584, FxBits(b)}, {588, FxBits(a)}}; }
Words EmbossWords(float step) { return {{592, 1}, {596, FxBits(step)}}; }
Words RetroWords() { return {{604, 1}, {640, FxBits(0.7f)}, {644, FxBits(0.3f)}, {648, FxBits(0.3f)}, {652, FxBits(0.9f)}}; }   // cue 13's g_Range except max .9
Words Cat(Words a, const Words& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

// Build cases (no GPU): the production Build on planted rows.
int FxhBuildTest(const std::string& plant) {
  int rc = 0;
  const auto check = [&](bool ok, const char* what) { std::printf("fxh-build %s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) rc = 3; };
  if (plant == "oldmask") g_post_cue_plant.store(3);   // RIG-ONLY twin: the cue words unsupported again
  const Camera cam = FxPerspCam();
  const auto build = [&](const Words& words, uint32_t w, uint32_t h, PostParams& pp, post_from_record::Info& info, bool cue = true, uint32_t dw = 0, uint32_t dh = 0) {
    SceneTables tt;
    tt.header.guest_frame = 100;
    PostParamsRow row;
    row.flags = kPpRead, row.frame = 100;
    row.s[0] = 1;
    row.fx[kFxDispW] = dw, row.fx[kFxDispH] = dh;
    for (const auto& kv : words) row.s[kv.first / 4] = kv.second;
    tt.post_params.push_back(row);
    return post_from_record::Build(tt, cam, w, h, pp, info, cue);
  };
  PostParams pp;
  post_from_record::Info in;
  // (a) defocus + hue: the colour (0.56, 0.43, 0.19, 1) (cue 12's sepia) -> (Y, Cb, Cr, A) = (0.44138, -0.14194, 0.08451, 1); the blur chain runs though the combine has no DoF (S+4 = 0)
  bool b = build(DefocusWords(0.667f, 0.56f, 0.43f, 0.19f, 1.f), 1280, 720, pp, in);
  check(b && pp.defocus && !pp.dof && pp.dof_chain && !pp.emboss && !pp.retro && in.defocus && in.unsupported == 0 && pp.defocus_rate == 0.667f, "defocus-only frame builds: defocus + the DoF blur chain (S+4 off), rate S+564, not unsupported");
  check(b && std::fabs(pp.defocus_ycbcr[0] - 0.44138f) < 1e-4f && std::fabs(pp.defocus_ycbcr[1] + 0.14194f) < 1e-4f && std::fabs(pp.defocus_ycbcr[2] - 0.08451f) < 1e-4f && pp.defocus_ycbcr[3] == 1.f, "the hue colour S+576..588 -> (Y, Cb, Cr, A) as sub_82C292D0 (BT.601, by hand 0.44138 / -0.14194 / 0.08451 / 1)");
  // (b) the word 0 draws nothing, whatever stale numbers sit beside it (production: the cue reset clears the flag, not the numbers)
  Words stale = {{564, FxBits(0.667f)}, {576, FxBits(0.56f)}, {596, FxBits(1.f)}, {640, FxBits(0.7f)}};
  b = build(stale, 1280, 720, pp, in);
  check(!b && !in.missing && in.unsupported == 0, "words 0 (stale numbers set): no effect on, nothing drawn, not a miss");
  b = build(Cat(stale, {{180, 1}}), 1280, 720, pp, in);
  check(b && pp.tonemap && !pp.defocus && !pp.emboss && !pp.retro && !in.defocus && !in.emboss && !in.retro, "words 0 beside a real effect (tonemap): none of the three cue effects is on");
  // (c) emboss: the step is S+596 DISPLAY pixels over the display's own size
  b = build(EmbossWords(0.5f), 1280, 720, pp, in);
  check(b && pp.emboss && !pp.defocus && !pp.retro && std::fabs(pp.emboss_step[0] - 0.5f / 1280.f) < 1e-9f && std::fabs(pp.emboss_step[1] - 0.5f / 720.f) < 1e-9f && in.unsupported == 0, "emboss-only frame: step (0.5 / 1280, 0.5 / 720)");
  b = build(EmbossWords(2.f), 2560, 1440, pp, in);
  check(b && std::fabs(pp.emboss_step[0] - 2.f / 1280.f) < 1e-9f && std::fabs(pp.emboss_step[1] - 2.f / 720.f) < 1e-9f, "emboss at render scale 2: the step stays the GAME picture's (2 / 1280, 2 / 720)");
  b = build(EmbossWords(1.f), 1280, 720, pp, in, true, 1920, 1080);
  check(b && std::fabs(pp.emboss_step[0] - 1.f / 1920.f) < 1e-9f && std::fabs(pp.emboss_step[1] - 1.f / 1080.f) < 1e-9f, "emboss with the display size the latch read (1920 x 1080)");
  // (d) retro film: g_Range = S+640..652, g_Time.w = 4 / the display height; FILM GRAIN (VINTAGE LOOK) off leaves it out
  b = build(RetroWords(), 1280, 720, pp, in);
  check(b && pp.retro && !pp.defocus && !pp.emboss && pp.retro_range[0] == 0.7f && pp.retro_range[1] == 0.3f && pp.retro_range[2] == 0.3f && pp.retro_range[3] == 0.9f && std::fabs(pp.retro_jitter - 4.f / 720.f) < 1e-9f && in.unsupported == 0,
        "retro-only frame: range (0.7, 0.3, 0.3, 0.9), shake 4 / 720");
  b = build(RetroWords(), 1280, 720, pp, in, true, 1920, 1080);
  check(b && std::fabs(pp.retro_jitter - 4.f / 1080.f) < 1e-9f, "retro: the shake is 4 / the display's height (1080)");
  {   // the game's frame rate [0x8392DC58] carried in fx[kFxRate]: 60 -> rate 60; unreadable (0) or implausible -> the default 30
    const auto rate_of = [&](uint32_t bits) {
      SceneTables tt;
      tt.header.guest_frame = 100;
      PostParamsRow row;
      row.flags = kPpRead, row.frame = 100, row.s[0] = 1;
      for (const auto& kv : RetroWords()) row.s[kv.first / 4] = kv.second;
      row.fx[kFxRate] = bits;
      tt.post_params.push_back(row);
      PostParams q;
      post_from_record::Info qi;
      post_from_record::Build(tt, cam, 1280, 720, q, qi);
      return q.retro_rate;
    };
    check(rate_of(FxBits(60.f)) == 60.f && rate_of(FxBits(30.f)) == 30.f && rate_of(0) == 30.f && rate_of(FxBits(1e9f)) == 30.f && rate_of(0x7FC00000u) == 30.f, "the retro clock's rate is the game's frame rate from the latch (60 / 30); unreadable or implausible = 30");
  }
  const bool was_grain = wwe2k14::start::g_quality.film_grain;
  wwe2k14::start::g_quality.film_grain = false;
  b = build(RetroWords(), 1280, 720, pp, in);
  check(!b && in.retro_off_setting && !in.retro, "FILM GRAIN off: the retro film is left out (the stream build's CLEAN twin draws the scene as it is)");
  b = build(Cat(RetroWords(), EmbossWords(1.f)), 1280, 720, pp, in);
  check(b && pp.emboss && !pp.retro && in.retro_off_setting, "FILM GRAIN off: only the retro film goes; the emboss beside it stays");
  wwe2k14::start::g_quality.film_grain = was_grain;
  // (e) garbage numbers leave that effect out
  const uint32_t nan_bits = 0x7FC00000u;
  b = build({{560, 1}, {564, nan_bits}}, 1280, 720, pp, in);
  check(!b && in.cue_garbage && !in.defocus, "a non-finite rate: the defocus is left out (counted), not drawn");
  // (f) the stream-free switch: off = the three words are named unsupported and nothing is drawn (the state before this lane)
  b = build(Cat(Cat(DefocusWords(0.5f, 1.f, 0.98f, 0.88f, 1.f), EmbossWords(1.f)), RetroWords()), 1280, 720, pp, in, false);
  check(!b && (in.unsupported & kCueMaskAll) == kCueMaskAll && !in.defocus && !in.emboss && !in.retro, "switch off: all three words named unsupported, none drawn");
  // (g) all three together (production order is a pass-time rule: the flags are all set)
  b = build(Cat(Cat(DefocusWords(0.5f, 1.f, 0.98f, 0.88f, 1.f), EmbossWords(1.f)), RetroWords()), 1280, 720, pp, in);
  check(b && pp.defocus && pp.emboss && pp.retro && in.unsupported == 0, "all three words set (switch on): all three drawn, nothing unsupported");
  g_post_cue_plant.store(0);
  return rc;
}

// The retro clock (sub_82C288B0) against an independent double-precision simulation, and its once-per-guest-frame discipline.
int FxhUnitTest(const std::string& plant) {
  int rc = 0;
  const auto check = [&](bool ok, const char* what) { std::printf("fxh-unit %s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) rc = 3; };
  const float rate = 57.8f, dt = 1.f / rate;   // (dt not a divisor of 0.05: no tick lands on the 0.05 boundary, where float and double could differ)
  cue_fx::RetroClock c;
  double t44 = 0, t48 = 0, t52 = 0, acc = 0;
  for (int i = 0; i < 90; ++i) {
    cue_fx::RetroTick(c, rate);
    t44 = std::fmod(t44 + double(dt), 1.0), acc += double(dt);
    if (acc >= 0.05) acc -= 0.05, t48 = std::fmod(t48 + double(dt), 1.0), t52 = std::fmod(t52 + 35.2517 / double(rate), 1.0);
  }
  const auto near1 = [](double a, double b) { const double d = std::fabs(a - b); return std::min(d, 1.0 - d) < 2e-3; };   // (circular: a value near 1 and one near 0 are close)
  std::printf("fxh-unit clock after 90 ticks at rate %.2f: t44 %.5f (sim %.5f) t48 %.5f (sim %.5f) t52 %.5f (sim %.5f)\n", rate, c.t44, t44, c.t48, t48, c.t52, t52);
  check(near1(c.t44, t44) && near1(c.t48, t48) && near1(c.t52, t52), "the clock = fmod(t + dt, 1) each frame; t48 / t52 step every 0.05 s by dt and 35.2517 dt (0x8362A06C / 0x8206D76C)");
  const bool twin = plant == "retro-step";   // RIG-ONLY: the planted twin ticks on every Record
  cue_fx::RetroClock k;
  cue_fx::RetroAdvance(k, 100, rate, twin);   // first use: starts at 0
  check(k.armed && k.t44 == 0.f && k.steps == 0, "the first guest frame starts the clock at 0 (no tick)");
  cue_fx::RetroAdvance(k, 101, rate, twin);
  const float after1 = k.t44;
  check(after1 > 0.f && k.steps == 1, "the next guest frame ticks once");
  cue_fx::RetroAdvance(k, 101, rate, twin);
  cue_fx::RetroAdvance(k, 101, rate, twin);
  check(k.t44 == after1 && k.steps == 1, "a repeat of the same guest frame ticks nothing (a retro clock ticked twice per guest frame is wrong)");
  cue_fx::RetroAdvance(k, 99, rate, twin);   // a step back inside the gap: no tick either
  check(k.t44 == after1 && k.steps == 1, "a step back inside the gap ticks nothing");
  cue_fx::RetroAdvance(k, 105, rate, twin);
  cue_fx::RetroClock ref;
  for (int i = 0; i < 5; ++i) cue_fx::RetroTick(ref, rate);   // 101 .. 105 = four more ticks after the first: 5 in all
  check(std::fabs(k.t44 - ref.t44) < 1e-6f && std::fabs(k.t52 - ref.t52) < 1e-6f, "a forward gap composes the frames not drawn (4 ticks for 101 -> 105)");
  cue_fx::RetroAdvance(k, 105 + cue_fx::kRetroGap + 5, rate, twin);
  check(k.steps == 3, "a forward gap past kRetroGap is one step of at most kRetroGap ticks");
  cue_fx::RetroAdvance(k, 20, rate, twin);   // far behind: a new scene
  check(k.armed && k.t44 == 0.f && k.steps == 0, "a frame number far behind restarts the clock at 0");
  return rc;
}

using Px8 = std::vector<uint8_t>;
double FxFracWithin(const Px8& a, const Px8& b, int tol) {   // fraction of pixels whose three channels are all within `tol` levels
  size_t n = 0, ok = 0;
  for (size_t i = 0; i + 3 < a.size() && i + 3 < b.size(); i += 4, ++n) ok += std::abs(int(a[i]) - int(b[i])) <= tol && std::abs(int(a[i + 1]) - int(b[i + 1])) <= tol && std::abs(int(a[i + 2]) - int(b[i + 2])) <= tol;
  return n ? double(ok) / double(n) : 0.0;
}
double FxGrad(const Px8& s, int w, int h) {   // the mean horizontal + vertical step of the red channel (a blur lowers it)
  double sum = 0;
  for (int y = 0; y + 1 < h; ++y)
    for (int x = 0; x + 1 < w; ++x) sum += std::abs(int(s[(size_t(y) * w + x + 1) * 4]) - int(s[(size_t(y) * w + x) * 4])) + std::abs(int(s[(size_t(y + 1) * w + x) * 4]) - int(s[(size_t(y) * w + x) * 4]));
  return sum / (double(w) * h);
}
uint8_t FxByte(double v) { return uint8_t(std::lround(std::fmin(std::fmax(v, 0.0), 1.0) * 255.0)); }
Px8 RefEmboss(const Px8& s, int w, int h, int step_px) {   // yEmboss.frag: 0.5 - (left + above-left + above - right - below - below-right) of the red channel, clamp addressing, point sampling, alpha 1
  Px8 o(s.size(), 255);
  const auto R = [&](int x, int y) { x = std::min(std::max(x, 0), w - 1), y = std::min(std::max(y, 0), h - 1); return s[(size_t(y) * w + x) * 4] / 255.0; };
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double relief = R(x - step_px, y) + R(x - step_px, y - step_px) + R(x, y - step_px) - R(x + step_px, y) - R(x, y + step_px) - R(x + step_px, y + step_px);
      const uint8_t v = FxByte(0.5 - relief);
      for (int c = 0; c < 3; ++c) o[(size_t(y) * w + x) * 4 + c] = v;
    }
  return o;
}
Px8 RefHue(const Px8& s, int w, int h, float cb, float cr) {   // yDefocus.frag with rate 0 and amount 1: p + (p.r + tint - p) = (r + 1.402 cr, r - 0.71414 cr - 0.34414 cb, r + 1.772 cb)
  Px8 o(s.size(), 255);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    const double r = s[i * 4] / 255.0;
    o[i * 4] = FxByte(r + 1.402 * cr), o[i * 4 + 1] = FxByte(r - 0.71414 * cr - 0.34414 * cb), o[i * 4 + 2] = FxByte(r + 1.772 * cb);
  }
  return o;
}
Px8 RefRetro(const Px8& s, int w, int h, const cue_fx::RetroClock& c, const float range[4], float jitter_scale) {   // yRetroFilm.frag with OUR strips / grain (post_cue_fx.h), point strips, wrap + linear grain
  const std::vector<uint8_t> s1 = cue_fx::MakeStrip(cue_fx::kSeedStrip1), s2 = cue_fx::MakeStrip(cue_fx::kSeedStrip2), grain = cue_fx::MakeGrain(cue_fx::kSeedGrain);
  const int row = std::min(std::max(int(std::floor(double(c.t44) * cue_fx::kStripH)), 0), int(cue_fx::kStripH) - 1);
  const double flick = s1[size_t(row) * 4] / 255.0, jn = 2.0 * (s2[size_t(row) * 4] / 255.0) - 1.0, jit = jn * jn * jn * double(jitter_scale);
  const int gw = int(cue_fx::kGrainW), gh = int(cue_fx::kGrainH);
  const auto G = [&](int x, int y) { x = ((x % gw) + gw) % gw, y = ((y % gh) + gh) % gh; return grain[(size_t(y) * gw + x) * 4 + 3] / 255.0; };
  Px8 o(s.size(), 255);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const double u = (x + 0.5) / w, v = (y + 0.5) / h;
      const int sy = std::min(std::max(int(std::floor((v + jit) * h)), 0), h - 1);
      const uint8_t* px = &s[(size_t(sy) * w + x) * 4];
      const double r = px[0] / 255.0, g = px[1] / 255.0, b = px[2] / 255.0;
      const double luma = 0.29891 * r + 0.58661 * g + 0.11448 * b, cr = 0.5 * r - 0.41869 * g - 0.08131 * b, cb = -0.16874 * r - 0.33126 * g + 0.5 * b;
      const double bright = std::fmin(std::fmax(double(range[0]) * (1.0 + 0.4 * flick) * luma + double(range[1]), double(range[2])), double(range[3]));
      const double gx = (u + 9.0 * c.t48) * 0.333333 * gw - 0.5, gy = (v + 3.0 * c.t52) * 0.133333 * gh - 0.5;
      const int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
      const double fx = gx - x0, fy = gy - y0;
      const double gr = (G(x0, y0) * (1 - fx) + G(x0 + 1, y0) * fx) * (1 - fy) + (G(x0, y0 + 1) * (1 - fx) + G(x0 + 1, y0 + 1) * fx) * fy;
      const double nl = bright * gr;
      uint8_t* q = &o[(size_t(y) * w + x) * 4];
      q[0] = FxByte(nl + 1.402 * cr), q[1] = FxByte(nl - 0.34414 * cb - 0.71414 * cr), q[2] = FxByte(nl + 1.772 * cb);
    }
  return o;
}

int FxhTest(FrontEnd& fe, rf::ModelCache& cache, const SceneTables& t, uint32_t w, uint32_t h, const std::string& plant, const std::string& prefix) {
  std::string err;
  int rc = 0;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const auto fail = [&](const char* what) { std::printf("fxh-test FAIL %s\n", what); rc = 3; };
  if (t.post_params.empty()) { std::printf("fxh-test FAIL no post row in the record (the base row)\n"); return 3; }
  if (uint64_t(w) * 720 != uint64_t(h) * 1280) { std::printf("fxh-test FAIL needs a 16:9 size (the emboss step is whole drawn pixels), got %ux%u\n", w, h); return 3; }
  SceneFrame A;
  {
    std::vector<rf::ClassReport> rr;
    rf::c::SetOfflineLoad(true);
    if (!rf::BuildFrame(t, cache, rf::kAllFamilies, w, h, A, rr, err)) { std::fprintf(stderr, "post_rig: FAIL fxh build: %s\n", err.c_str()); return 3; }
  }
  const PostParamsRow base_row = t.post_params[0];
  const auto params = [&](const Words& words, uint64_t g, PostParams& pp, post_from_record::Info& info) {   // the production path: the planted row -> Build
    SceneTables tt = t;
    PostParamsRow r = base_row;
    for (const auto& kv : words) r.s[kv.first / 4] = kv.second;
    r.flags = post_settings::FlagsOf(r, true), r.frame = g;
    tt.header.guest_frame = g;
    tt.post_params.clear();
    tt.post_params.push_back(r);
    const bool ok = post_from_record::Build(tt, A.camera, w, h, pp, info);
    pp.guest_frame = g;
    return ok;
  };
  const auto draw = [&](const Words& words, uint64_t g, Px8& px, bool ignore_defocus = false, bool ignore_emboss = false, bool ignore_retro = false) {
    PostParams pp;
    post_from_record::Info info;
    if (!params(words, g, pp, info)) { std::fprintf(stderr, "post_rig: FAIL fxh Build (%s)\n", info.why); return false; }
    if (ignore_defocus) pp.defocus = false;   // planted twins: the flag ignored on the frame that must show it
    if (ignore_emboss) pp.emboss = false;
    if (ignore_retro) pp.retro = false;
    A.post = &pp;
    const bool ok = fe.RenderToPixels(A, px, err);
    A.post = nullptr;
    if (!ok) std::fprintf(stderr, "post_rig: FAIL fxh draw: %s\n", err.c_str());
    return ok;
  };
  const auto save = [&](const char* name, const Px8& px) { scene_png::WritePngRgb(prefix + "_fxh_" + name + ".png", px, w, h); };
  if (plant == "strips") g_post_cue_plant.store(2);
  if (plant == "retro-step") g_post_cue_plant.store(1);
  if (plant == "pass-order") g_post_cue_plant.store(4);
  const float step_px = 1.f;   // emboss: ONE drawn pixel = 1280 / w display pixels (the 16:9 size check above): exact whole-pixel taps
  const float step_disp = 1280.f / float(w);
  Px8 bR;
  if (!draw({}, 1000, bR)) return 3;
  save("base", bR);
  const uint64_t passes0 = g_post_cue_passes.load();

  // 1. Words 0 (with stale numbers beside them): nothing is drawn, no cue pass is recorded, the picture is the base one.
  {
    Px8 z;
    if (!draw({{564, FxBits(0.667f)}, {576, FxBits(0.56f)}, {596, FxBits(1.f)}, {640, FxBits(0.7f)}}, 1001, z)) return 3;
    const double f = FxFracWithin(z, bR, 1);
    std::printf("fxh-test zero: words 0 -> %.4f of the pixels equal the base picture, cue passes recorded %llu\n", f, (unsigned long long)(g_post_cue_passes.load() - passes0));
    if (!(f > 0.999) || g_post_cue_passes.load() != passes0) fail("a cue effect drew with its word 0");
  }
  // 2. Emboss against the CPU reference of yEmboss.frag, and against off.
  {
    Px8 e;
    if (!draw(EmbossWords(step_disp), 1010, e, false, plant == "emboss")) return 3;
    const Px8 ref = RefEmboss(bR, int(w), int(h), int(step_px));
    const double f = FxFracWithin(e, ref, 3), d_off = FxFracWithin(e, bR, 3);
    save("emboss", e), save("emboss_ref", ref);
    std::printf("fxh-test emboss: %.4f of the pixels within 3 levels of the CPU relief of the base picture (step %.2f display px); %.4f within 3 of the base picture\n", f, step_disp, d_off);
    if (d_off > 0.9) fail("emboss on equals emboss off");
    else if (!(f > 0.99)) fail("the emboss picture does not match yEmboss's relief of the red channel");
  }
  // 3. Defocus + hue: rate 0 / amount 1 is the exact hue shift of the base picture; rate 1 / amount 0 is the blur (lower gradient energy), also with the combine's DoF off (S+4 = 0: the blur chain still runs for it).
  {
    const float cr_col[4] = {0.56f, 0.43f, 0.19f, 1.f};
    float ycc[4];
    post_settings::YCbCrFrom(cr_col, ycc);
    Px8 d;
    if (!draw(DefocusWords(0.f, cr_col[0], cr_col[1], cr_col[2], cr_col[3]), 1020, d, plant == "defocus")) return 3;
    const Px8 ref = RefHue(bR, int(w), int(h), ycc[1], ycc[2]);
    const double f = FxFracWithin(d, ref, 3), d_off = FxFracWithin(d, bR, 3);
    save("defocus_hue", d), save("defocus_hue_ref", ref);
    std::printf("fxh-test defocus-hue: %.4f of the pixels within 3 levels of the CPU hue shift (Y %.4f Cb %.4f Cr %.4f); %.4f within 3 of the base picture\n", f, ycc[0], ycc[1], ycc[2], d_off);
    if (d_off > 0.9) fail("defocus on equals defocus off");
    else if (!(f > 0.99)) fail("the defocus hue picture does not match the hue shift of the base picture");
    Px8 blur, none, blur_nodof, base_nodof;
    if (!draw(DefocusWords(1.f, 0.f, 0.f, 0.f, 0.f), 1021, blur, plant == "defocus")) return 3;
    const double g0 = FxGrad(bR, int(w), int(h)), g1 = FxGrad(blur, int(w), int(h));
    save("defocus_blur", blur);
    std::printf("fxh-test defocus-blur: rate 1, amount 0: red-channel gradient %.3f -> %.3f; %.4f of the pixels within 1 of the base picture\n", g0, g1, FxFracWithin(blur, bR, 1));
    if (!(g1 < 0.9 * g0) || FxFracWithin(blur, bR, 1) > 0.9) fail("defocus rate 1 does not show the DoF blur");
    if (!draw({{4, 0}}, 1022, base_nodof) || !draw(Cat({{4, 0}}, DefocusWords(1.f, 0.f, 0.f, 0.f, 0.f)), 1023, blur_nodof, plant == "defocus")) return 3;
    const double h0 = FxGrad(base_nodof, int(w), int(h)), h1 = FxGrad(blur_nodof, int(w), int(h));
    save("defocus_blur_nodof", blur_nodof);
    std::printf("fxh-test defocus-blur with S+4 = 0: gradient %.3f -> %.3f\n", h0, h1);
    if (!(h1 < 0.9 * h0)) fail("with the combine's DoF off the defocus did not get its blur (the DoF chain must run for it)");
  }
  // 4. Retro film against the CPU reference of yRetroFilm.frag with OUR noise pictures, over a run of guest frames; a repeat of a guest frame is identical; the clock moves between guest frames.
  {
    const float range[4] = {0.7f, 0.3f, 0.3f, 0.9f}, jscale = 4.f / 720.f;
    cue_fx::RetroClock mirror;   // the test's model of the game's clock: advanced once per guest frame drawn with the retro film
    uint64_t last_g = 0;
    Px8 prev, first;
    double worst = 1.0;
    for (uint64_t g : {uint64_t(3000), uint64_t(3001), uint64_t(3004), uint64_t(3004), uint64_t(3005)}) {
      Px8 r;
      if (!draw(RetroWords(), g, r, false, false, plant == "retro")) return 3;
      if (g != last_g) cue_fx::RetroAdvance(mirror, g, cue_fx::kRetroRateDefault);
      const Px8 ref = RefRetro(bR, int(w), int(h), mirror, range, jscale);
      const double f = FxFracWithin(r, ref, 4);
      worst = std::min(worst, f);
      std::printf("fxh-test retro g %llu: clock t44 %.4f t48 %.4f t52 %.4f; %.4f of the pixels within 4 levels of the CPU reference; %.4f within 4 of the base picture\n", (unsigned long long)g, mirror.t44, mirror.t48, mirror.t52, f, FxFracWithin(r, bR, 4));
      if (g == 3000) first = r, save("retro_a", r), save("retro_a_ref", ref);
      if (g == 3004 && last_g == 3004) {
        const double same = FxFracWithin(r, prev, 0);
        std::printf("fxh-test retro repeat: guest frame 3004 recorded twice: %.4f of the pixels identical\n", same);
        if (!(same > 0.9999)) fail("a repeat of the same guest frame ticked the retro clock (twin ticking twice per guest frame)");
      }
      if (g == 3005) save("retro_e", r);
      prev = r, last_g = g;
    }
    if (!(worst > 0.97)) fail("the retro picture does not match yRetroFilm's formula on our noise pictures (strips / grain / clock mapping)");
    const double d_off = FxFracWithin(first, bR, 4);
    if (d_off > 0.9) fail("retro on equals retro off");
    Px8 later;
    if (!draw(RetroWords(), 3030, later)) return 3;
    cue_fx::RetroAdvance(mirror, 3030, cue_fx::kRetroRateDefault);
    const double moved = FxFracWithin(later, first, 0);
    std::printf("fxh-test retro motion: guest frame 3030 vs 3000: %.4f of the pixels identical (the clock moved: grain and flicker)\n", moved);
    if (moved > 0.9) fail("the retro film does not move between guest frames");
  }
  // 5. All three together: the passes run defocus -> emboss -> retro and the last one lands in the output (ping-pong); the defocus here is the identity (rate 0, amount 0).
  {
    const float range[4] = {0.7f, 0.3f, 0.3f, 0.9f};
    cue_fx::RetroClock mirror;   // (the pass's clock is fresh again: this frame is far behind the retro run above)
    cue_fx::RetroAdvance(mirror, 1100, cue_fx::kRetroRateDefault);
    Px8 all;
    const uint64_t p0 = g_post_cue_passes.load();
    if (!draw(Cat(Cat(DefocusWords(0.f, 0.f, 0.f, 0.f, 0.f), EmbossWords(step_disp)), RetroWords()), 1100, all)) return 3;
    const uint64_t used = g_post_cue_passes.load() - p0;
    const Px8 ref = RefRetro(RefEmboss(bR, int(w), int(h), int(step_px)), int(w), int(h), mirror, range, 4.f / 720.f);
    const double f = FxFracWithin(all, ref, 4);
    save("all3", all), save("all3_ref", ref);
    std::printf("fxh-test all three: cue passes %llu; %.4f of the pixels within 4 levels of retro(emboss(defocus-identity(base)))\n", (unsigned long long)used, f);
    if (used != 3) fail("three cue effects did not record three passes");
    if (!(f > 0.97)) fail("the three cue effects are not applied in the game's order (defocus, emboss, retro film) into the output");
  }
  g_post_cue_plant.store(0);
  std::printf(rc == 0 ? "fxh-test PASS\n" : "fxh-test FAIL\n");
  return rc;
}
}  // namespace

int main(int argc, char** argv) {
  std::string rec, game, prefix, row_file, plant, ghost_mode, ghost_plant, pst_plant, fxh_plant;
  bool pst = false, fxh = false;
  uint32_t w = 1280, h = 720;
  std::vector<float> cam, eye;
  bool no_post = false, fov_test = false;
  float a1_scale = 1.f;   // S105 A1: --scale S = the renderer's render scale; the scene targets are drawn at S x the logical size (the stream-free player build with the launcher's 1440p / 4K / 360p-648p)
  bool a1_no_dof = false;   // S105 A1: --a1-no-dof: the post row with its depth of field switched off (the DoF-only effect map = the picture with DoF minus this one)
  std::string a1_plant;   // S105 A1: dconv | ghost | unscaled (each must make a1_check.py FAIL)
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--record" && i + 1 < argc) rec = argv[++i];
    else if (a == "--game" && i + 1 < argc) game = argv[++i];
    else if (a == "--prefix" && i + 1 < argc) prefix = argv[++i];
    else if (a == "--size" && i + 1 < argc) { if (std::sscanf(argv[++i], "%ux%u", &w, &h) != 2) return 1; }
    else if (a == "--camera" && i + 1 < argc) cam = ParseFloats(argv[++i]);
    else if (a == "--eye" && i + 1 < argc) eye = ParseFloats(argv[++i]);
    else if (a == "--row" && i + 1 < argc) row_file = argv[++i];
    else if (a == "--plant" && i + 1 < argc) plant = argv[++i];
    else if (a == "--no-post") no_post = true;
    else if (a == "--scale" && i + 1 < argc) a1_scale = std::strtof(argv[++i], nullptr);   // S105 A1
    else if (a == "--a1-plant" && i + 1 < argc) a1_plant = argv[++i];   // S105 A1
    else if (a == "--a1-no-dof") a1_no_dof = true;   // S105 A1
    else if (a == "--ghost" && i + 1 < argc) ghost_mode = argv[++i];                 // S103 FXF: alpha | additive: the ghost twin test (see below)
    else if (a == "--ghost-plant" && i + 1 < argc) ghost_plant = argv[++i];          // S103 FXF: ignored = the second ghost frame draws without the ghost (must be caught)
    else if (a == "--fov-test") fov_test = true;
    else if (a == "--pst") pst = true;                                                // S103 PST: the tonemap / night / antialiasing proofs (PstTest above)
    else if (a == "--pst-plant" && i + 1 < argc) pst_plant = argv[++i];                // RIG-ONLY: step | tonemap | night | aa (each must make PstTest FAIL)
    else if (a == "--fxh") fxh = true;                                                // S103 FXH: the cue effects' proofs (FxhBuildTest / FxhUnitTest / FxhTest above)
    else if (a == "--fxh-plant" && i + 1 < argc) fxh_plant = argv[++i];                // RIG-ONLY: oldmask | emboss | defocus | retro | retro-step | strips | pass-order (each must make the proof FAIL)
    else if (a == "--combine-lite-plant"&& i + 1 < argc) rdr::g_combine_lite_plant.store(std::atoi(argv[++i]));   // RIG-ONLY: S103 SLV2: combine_lite.h's planted twins (2: the bloom is never fetched: a live-bloom row must differ)
    else if (a == "--depth-fold" && i + 1 < argc) g_post_depth_fold.store(std::atoi(argv[++i]));   // RIG-ONLY: S103 SLV2: 0 = the dconv_ pass as before (the fold's OFF twin), default 1
    else if (a == "--depth-fold-plant" && i + 1 < argc) g_post_depth_fold_plant.store(std::atoi(argv[++i]));   // RIG-ONLY: S103 SLV2: 1 = the fold decodes with a wrong q (the picture must differ)
    else if (a == "--combine-lite" && i + 1 < argc) rdr::g_combine_lite.store(rdr::ClampCombineLite(std::atoi(argv[++i])));   // RIG-ONLY: S103 SLV (slv_check.py): the lever the game start stores from wwe2k14_rdr_combine_lite (start_settings.cpp)
  }
  if (pst) {   // S103 PST fix: the Build cases need no record, no GPU
    const int bt = PstBuildTest(pst_plant);
    if (bt != 0) { std::printf("pst-test FAIL build cases\n"); return bt; }
  }
  if (fxh) {   // S103 FXH: the Build cases and the clock unit need no record, no GPU
    int bt = FxhBuildTest(fxh_plant);
    if (bt == 0) bt = FxhUnitTest(fxh_plant);
    if (bt != 0) { std::printf("fxh-test FAIL build / unit cases\n"); return bt; }
  }
  if (rec.empty() || game.empty() || prefix.empty()) { std::fprintf(stderr, "usage: post_rig --record F.rsc --game DIR --prefix P --camera 16f --eye 3f [--row words.bin]\n"); return 1; }
  SceneTables t;
  std::string err;
  if (!ReadRecord(rec, t, err)) { std::fprintf(stderr, "post_rig: FAIL bad record: %s\n", err.c_str()); return 2; }
  if (cam.size() == 16) {
    std::memcpy(t.camera.view.m, cam.data(), sizeof(float) * 16);
    t.camera.proj = IdentityMat4();
    t.camera.flags = kCamViewIsViewProj;
    if (eye.size() == 3) std::memcpy(t.camera.eye, eye.data(), sizeof(float) * 3);
    t.has_camera = true;
  }
  if (!t.has_camera && !fov_test) { std::fprintf(stderr, "post_rig: FAIL no camera\n"); return 2; }
  if (!row_file.empty()) {   // the test row: the words of S, little-endian u32 x kPostSettingsWords
    std::FILE* f = std::fopen(row_file.c_str(), "rb");
    if (!f) { std::fprintf(stderr, "post_rig: FAIL cannot read %s\n", row_file.c_str()); return 2; }
    PostParamsRow row;
    const size_t n = std::fread(row.s, 4, kPostSettingsWords, f);
    std::fclose(f);
    if (n != kPostSettingsWords) { std::fprintf(stderr, "post_rig: FAIL %s holds %zu words, need %u\n", row_file.c_str(), n, kPostSettingsWords); return 2; }
    row.settings_addr = 0x8392DD88;   // (the pointer's own address as the writer stores it: any non-zero value)
    row.flags = post_settings::FlagsOf(row, true);
    row.frame = t.header.guest_frame;
    t.post_params.clear();
    t.post_params.push_back(row);
    // the record path: written and read back (the post row survives the file: section 29)
    const std::string rec_path = prefix + "_rec.rsc";
    SceneTables back;
    if (!WriteRecord(rec_path, t, err) || !ReadRecord(rec_path, back, err)) { std::fprintf(stderr, "post_rig: FAIL record round trip: %s\n", err.c_str()); return 3; }
    if (back.post_params.size() != 1 || std::memcmp(back.post_params[0].s, row.s, sizeof row.s) != 0) { std::fprintf(stderr, "post_rig: FAIL the post row did not survive the record file\n"); return 3; }
    back.camera = t.camera, back.has_camera = true;   // (the camera section is not under test here)
    t = back;
    std::puts("record round trip: post row survives (section 29)");
  }

  GameFilesSource source;
  if (!source.SetRoot(game)) { std::fprintf(stderr, "post_rig: FAIL no game files under %s\n", game.c_str()); return 3; }
  rdr::RendererOptions ro;
  if (const char* v = std::getenv("WWE_RDR_VALIDATION")) ro.validation = std::atoi(v);
  ro.render_scale = a1_scale;   // S105 A1
  g_a1_plant_dconv_unscaled.store(a1_plant == "dconv" ? 1 : 0), g_a1_plant_ghost_logical.store(a1_plant == "ghost" ? 1 : 0), rdr_scene::g_a1_plant_scaled_alloc_fail.store(a1_plant == "alloc" ? 1 : 0);
  if (!a1_plant.empty() && a1_plant != "dconv" && a1_plant != "ghost" && a1_plant != "unscaled" && a1_plant != "alloc") { std::fprintf(stderr, "post_rig: unknown --a1-plant %s\n", a1_plant.c_str()); return 1; }
  auto renderer = rdr::Renderer::Create(ro, err);
  if (!renderer) { std::fprintf(stderr, "post_rig: FAIL renderer: %s\n", err.c_str()); return 3; }
  int rc = 0;
  {
    FrontEnd fe(*renderer);
    fe.SetScaledTargets(a1_scale != 1.f && a1_plant != "unscaled");   // S105 A1 ("unscaled" = the planted twin: the scene targets left at the logical size = the bug)
    rf::ModelCache cache(fe, source, true);
    if (fov_test) {   // S103 P: a camera as the composer's copy gives it (view + FOV, kCamProjFromFov): BuildFrame builds the projection; ProjQ must read (f/(f-n), n f/(f-n))
      const float cases[2][2] = {{0.f, 0.f}, {5.f, 1000.f}};   // near / far 0 = the reader's default (10 / 300000)
      for (const auto& cs : cases) {
        SceneTables ft = t;
        ft.camera = Camera();
        ft.camera.view = IdentityMat4();
        ft.camera.view.m[12] = 3.f, ft.camera.view.m[13] = -2.f, ft.camera.view.m[14] = 40.f;
        ft.camera.proj = IdentityMat4();
        ft.camera.fov_y_deg = 43.f, ft.camera.near_z = cs[0], ft.camera.far_z = cs[1];
        ft.camera.flags = kCamProjFromFov;
        ft.has_camera = true;
        SceneFrame ff;
        std::vector<rf::ClassReport> rr;
        rf::c::SetOfflineLoad(true);
        if (!rf::BuildFrame(ft, cache, 0u, w, h, ff, rr, err)) { std::fprintf(stderr, "post_rig: FAIL fov build: %s\n", err.c_str()); return 3; }
        const double n = cs[0] > 0 ? cs[0] : 10.0, far_z = cs[1] > 0 ? cs[1] : 300000.0;
        float q = 0, nq = 0;
        const bool ok = post_from_record::ProjQ(ff.camera, q, nq);
        if (plant == "no_depth_terms") q = 1.f, nq = 1.f;   // planted fault: the depth terms at the identity
        std::printf("fov-test near %g far %g: ok %d q-1 %.9g expect %.9g nq %.9g expect %.9g\n", n, far_z, ok, double(q) - 1.0, far_z / (far_z - n) - 1.0, double(nq), n * far_z / (far_z - n));
      }
      {  // the inputs: which absences are MISSING (the frame must not be ours / READY) and which are genuinely "no post"
        SceneTables ft = t;
        ft.camera = Camera();
        ft.camera.view = IdentityMat4();
        ft.camera.view.m[14] = 40.f;
        ft.camera.fov_y_deg = 43.f, ft.camera.flags = kCamProjFromFov, ft.has_camera = true;
        ft.header.guest_frame = 500;
        SceneFrame ff;
        std::vector<rf::ClassReport> rr;
        rf::c::SetOfflineLoad(true);
        if (!rf::BuildFrame(ft, cache, 0u, w, h, ff, rr, err)) return 3;
        PostParamsRow row;
        row.flags = kPpRead, row.frame = 500;
        row.s[0] = 1, row.s[1] = 1;   // on, dof
        const struct { const char* name; int kind; } kinds[] = {{"no_row", 0}, {"unreadable", 1}, {"stale", 2}, {"module_off", 3}, {"no_effect", 4}, {"ok", 5}, {"bad_camera", 6}, {"bloom_no_dof", 7}};
        for (const auto& k : kinds) {
          SceneTables tt = ft;
          Camera cam = ff.camera;
          PostParamsRow r = row;
          if (k.kind != 0) tt.post_params.push_back(r);
          if (k.kind == 1) tt.post_params[0].flags = 0;
          if (k.kind == 2) tt.post_params[0].frame = 100;
          if (k.kind == 3) tt.post_params[0].s[0] = 0;
          if (k.kind == 4) tt.post_params[0].s[1] = 0;
          if (k.kind == 6) cam = Camera();
          if (k.kind == 7) tt.post_params[0].s[1] = 0, tt.post_params[0].s[176 / 4 - 0] = 0, tt.post_params[0].s[44] = 1;   // S+176 = word 44: bloom on, dof off
          PostParams pp;
          post_from_record::Info info;
          const bool built = post_from_record::Build(tt, cam, w, h, pp, info);
          std::printf("info-test %s: built %d missing %d dof_chain %d why %s\n", k.name, built, info.missing, built ? int(pp.dof_chain) : -1, info.why);
        }
        {   // S103 FXF: the afterimage mapping (S+224..+272 -> PostParams::ghost*) and the tint rule (VINTAGE LOOK off drops only the arena's own sepia: gsf_code 1; a moment tint, code >= 2, stays)
          const auto bits = [](float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; };
          struct Fx { const char* name; uint32_t s0, after; uint32_t mono, gsf; bool vintage; uint32_t blend; };
          const Fx cases[] = {{"ghost_additive", 1, 1, 0, 0, true, 1}, {"ghost_alpha", 1, 1, 0, 0, true, 0}, {"ghost_module_off", 0, 1, 0, 0, true, 1}, {"ghost_off", 1, 0, 0, 0, true, 0},
                              {"sepia_vintage_off", 1, 0, 1, 1, false, 0}, {"moment_tint_vintage_off", 1, 0, 1, 2, false, 0}, {"unknown_gsf_vintage_off", 1, 0, 1, 0, false, 0}, {"sepia_vintage_on", 1, 0, 1, 1, true, 0}};
          const bool was_vintage = wwe2k14::start::g_quality.film_grain;
          for (const Fx& c : cases) {
            SceneTables tt = ft;
            PostParamsRow r = row;
            r.s[0] = c.s0, r.s[1] = 0;
            r.s[post_settings::kAfterOn / 4] = c.after, r.s[post_settings::kAfterAlpha / 4] = bits(0.6f), r.s[post_settings::kAfterX / 4] = bits(800.f), r.s[post_settings::kAfterY / 4] = bits(300.f),
            r.s[post_settings::kAfterScale / 4] = bits(1.07f), r.s[post_settings::kAfterR / 4] = 250, r.s[post_settings::kAfterG / 4] = 220, r.s[post_settings::kAfterB / 4] = 200, r.s[post_settings::kAfterBlend / 4] = c.blend;
            r.s[post_settings::kMonotone / 4] = c.mono, r.gsf_code = c.gsf;
            tt.post_params.push_back(r);
            wwe2k14::start::g_quality.film_grain = c.vintage;
            Camera cam = ff.camera;
            PostParams pp;
            post_from_record::Info info;
            const bool built = post_from_record::Build(tt, cam, w, h, pp, info);
            std::printf("fx-test %s: built %d afterimage %d blend %u film %d sepia_dropped %d tint_kept %d | ghost %d additive %d centre %.5f %.5f scale %.4f alpha %.6f colour %.5f %.5f %.5f frame %llu\n", c.name, built, info.afterimage, info.blend,
                        info.film, info.film_dropped_sepia, info.film_kept_tint, built ? int(pp.ghost) : -1, built ? int(pp.ghost_additive) : -1, built ? pp.ghost_centre[0] : 0.f, built ? pp.ghost_centre[1] : 0.f,
                        built ? pp.ghost_scale : 0.f, built ? pp.ghost_alpha : 0.f, built ? pp.ghost_colour[0] : 0.f, built ? pp.ghost_colour[1] : 0.f, built ? pp.ghost_colour[2] : 0.f, built ? (unsigned long long)pp.ghost_frame : 0ull);
          }
          {   // review 2: VINTAGE LOOK off + a moment tint in a 30 Years arena: arena row (flag 1, Cb .003, Cr .004, A .008) lerped with the preset (.006, .007, .009) at t = .5: S = (.0045, .0055, .0085);
              // expected film_alpha = presetA * t = .0045 and the preset's chroma (.006 / .007). A display size of 1920 x 1080 centres S+232 / +236 = 960 / 540 at 0.5 / 0.5.
            SceneTables tt = ft;
            PostParamsRow r = row;
            r.s[0] = 1, r.s[1] = 0, r.s[post_settings::kMonotone / 4] = 1, r.s[post_settings::kMonotone / 4 + 1] = bits(0.0045f), r.s[post_settings::kMonotone / 4 + 2] = bits(0.0055f), r.s[post_settings::kMonotone / 4 + 3] = bits(0.0085f);
            r.gsf_code = 2, r.fx[kFxGsfT] = bits(0.5f), r.fx[kFxArena] = 1, r.fx[kFxArena + 1] = bits(0.003f), r.fx[kFxArena + 2] = bits(0.004f), r.fx[kFxArena + 3] = bits(0.008f);
            r.s[post_settings::kAfterOn / 4] = 1, r.s[post_settings::kAfterAlpha / 4] = bits(0.3f), r.s[post_settings::kAfterX / 4] = bits(960.f), r.s[post_settings::kAfterY / 4] = bits(540.f), r.s[post_settings::kAfterScale / 4] = bits(1.f);
            r.fx[kFxDispW] = 1920, r.fx[kFxDispH] = 1080;
            tt.post_params.push_back(r);
            wwe2k14::start::g_quality.film_grain = false;
            Camera cam = ff.camera;
            PostParams pp;
            post_from_record::Info info;
            const bool built = post_from_record::Build(tt, cam, w, h, pp, info);
            std::printf("fx-test tint_arena_adjust: built %d film %d tint_kept %d cb %.5f cr %.5f alpha %.5f | centre %.4f %.4f disp_default %d\n", built, info.film, info.film_kept_tint, built ? pp.film_cb[0] : 0.f, built ? pp.film_cr[0] : 0.f,
                        built ? pp.film_alpha[0] : 0.f, built ? pp.ghost_centre[0] : 0.f, built ? pp.ghost_centre[1] : 0.f, info.disp_default);
            r.fx[kFxDispW] = 0;   // unreadable at the latch: the 1280 x 720 fallback, counted
            tt.post_params[0] = r;
            post_from_record::Build(tt, cam, w, h, pp, info);
            std::printf("fx-test disp_unreadable: centre %.4f %.4f disp_default %d\n", pp.ghost_centre[0], pp.ghost_centre[1], info.disp_default);
          }
          {   // review 3: the latch's classification (scene_inputs::gsf::Classify). S = the node's own words -> the arena sepia (1); another writer after the node (S differs) -> a tint (2); a moment index -> index + 2;
              // no node call: unknown (0), but with the arena's flag known OFF a monotone is a tint (2).
            scene_inputs::gsf::State st;
            st.calls = 1, st.index = -1, st.t = 0.f;
            const uint32_t node[4] = {1, bits(0.003f), bits(0.004f), bits(0.008f)}, other[4] = {1, bits(0.9f), bits(0.9f), bits(0.5f)};
            std::memcpy(st.node, node, 16), std::memcpy(st.arena, node, 16), st.arena_known = true;
            const auto c = [&](const scene_inputs::gsf::State& s, bool fresh, const uint32_t* w4) { return scene_inputs::gsf::Classify(s, fresh, w4).code; };
            scene_inputs::gsf::State moment = st;
            moment.index = 3;
            scene_inputs::gsf::State off = st;
            off.arena[0] = 0;
            scene_inputs::gsf::State none;
            std::printf("gsf-test sepia %u foreign_writer %u moment_idx3 %u unknown %u unknown_arena_flag_off %u\n", c(st, true, node), c(st, true, other), c(moment, true, other), c(none, false, node), c(off, false, node));
          }
          wwe2k14::start::g_quality.film_grain = was_vintage;
        }
      }
      renderer->WaitIdle();
      return 0;
    }
    SceneFrame f;
    std::vector<rf::ClassReport> reports;
    rf::c::SetOfflineLoad(true);   // offline: lane C loads the arena through the cache's source (live it comes from the loader thread)
    if (!rf::BuildFrame(t, cache, rf::kAllFamilies, w, h, f, reports, err)) { std::fprintf(stderr, "post_rig: FAIL build: %s\n", err.c_str()); return 3; }
    std::vector<uint8_t> rgba;
    fe.ResetCoverage();
    uint32_t rw = w, rh = h;   // S105 A1: the read-back's real size (the logical size x the render scale)
    if (!fe.RenderToPixels(f, rgba, err, &rw, &rh) || !scene_png::WritePngRgb(prefix + "_nopost.png", rgba, rw, rh)) { std::fprintf(stderr, "post_rig: FAIL draw (no post): %s\n", err.c_str()); return 3; }
    PostParams pp;
    post_from_record::Info info;
    const bool built = !no_post && post_from_record::Build(t, f.camera, w, h, pp, info);
    std::printf("info: %s\n", post_from_record::Describe(info).c_str());
    if (built) {
      if (plant == "swap_dof") {   // planted fault: the DoF ramps swapped (c20.x <-> c20.y, c21.x <-> c21.y)
        std::swap(pp.DOFParam[0], pp.DOFParam[1]);
        std::swap(pp.DOFParam2[0], pp.DOFParam2[1]);
        pp.dof_param2_down[0] = pp.DOFParam2[0], pp.dof_param2_down[1] = pp.DOFParam2[1];
      } else if (plant == "no_depth_terms") {   // planted fault: the depth conversion's q / nq left at the identity
        pp.proj_q = 1.f, pp.proj_nq = 1.f;
      } else if (!plant.empty()) { std::fprintf(stderr, "post_rig: unknown --plant %s\n", plant.c_str()); return 1; }
      std::printf("params: DOFParam %.7g %.7g %.7g %.7g | DOFParam2 %.7g %.7g %.7g %.7g | RGB %.7g %.7g %.7g %.7g | BloomScale.x %.7g | StarScale.x %.7g | dof %d fog %d bloom %d glare %d film %d | "
                  "depth_linear %d | dead_bloom %d dead_glare %d | q %.9g nq %.9g | down_w %.7g\n",
                  pp.DOFParam[0], pp.DOFParam[1], pp.DOFParam[2], pp.DOFParam[3], pp.DOFParam2[0], pp.DOFParam2[1], pp.DOFParam2[2], pp.DOFParam2[3], pp.RGB[0], pp.RGB[1], pp.RGB[2], pp.RGB[3],
                  pp.BloomScale[0], pp.StarScale[0], pp.dof, pp.fog, pp.bloom, pp.glare, pp.film, pp.dof_depth_linear, pp.dead_bloom, pp.dead_glare, pp.proj_q, pp.proj_nq, pp.dof_param2_down[3]);
      if (a1_no_dof) pp.dof = false, pp.dof_chain = false;   // S105 A1
      f.post = &pp;
      if (!fe.RenderToPixels(f, rgba, err, &rw, &rh) || !scene_png::WritePngRgb(prefix + "_post.png", rgba, rw, rh)) { std::fprintf(stderr, "post_rig: FAIL draw (post): %s\n", err.c_str()); rc = 3; }
      {   // S105 A1: the real sizes of the scene targets and of every live image with its kUsageScaled flag (a1_check.py reads these lines)
        uint32_t cw = 0, ch = 0, dw = 0, dh = 0, vw = 0, vh = 0;
        bool cs = false, ds = false, vs = false;
        fe.TargetExtent(fe.scene_colour_image(), cw, ch, cs), fe.TargetExtent(fe.scene_depth_image(), dw, dh, ds);
        if (fe.post_pass_for_test()) fe.TargetExtent(fe.post_pass_for_test()->depth_convert_image(), vw, vh, vs);
        std::printf("a1-sizes: scale %.4f logical %ux%u picture %ux%u colour %ux%u scaled %d depth %ux%u scaled %d dconv %ux%u scaled %d\n", a1_scale, w, h, rw, rh, cw, ch, int(cs), dw, dh, int(ds), vw, vh, int(vs));
        for (const rdr::Renderer::LiveImageInfo& li : renderer->LiveImageInfos()) std::printf("a1-image: id %u %ux%u format %u scaled %d\n", li.id, li.width, li.height, li.format, int(li.scaled));
      }
      if (!ghost_mode.empty() && rc == 0) {   // S103 FXF: the yAfterImage twin test (FXP.md adversary E). The static scene's off twin = the post picture just drawn. Ghost frame 1 (a new effect: no blend, the history is filled)
        // must equal it; ghost frame 2 draws the previous output over this one: it must differ INSIDE the sprite's rectangle only. Rect = centre +- scale / 2 of the picture (here 0.5 wide, centred at 0.62 / 0.4).
        // S105 A1: a second draw of the same post frame is the off twin: the first render of a record can differ from the next ones (a movie screen's stand-in picture appears from the second render on), which made this test fail at any scale.
        if (!fe.RenderToPixels(f, rgba, err, &rw, &rh)) { std::fprintf(stderr, "post_rig: FAIL draw (ghost off twin): %s\n", err.c_str()); return 3; }
        const std::vector<uint8_t> off = rgba;
        PostParams gp = pp;
        gp.ghost = true, gp.ghost_additive = ghost_mode == "additive";
        gp.ghost_centre[0] = 0.62f, gp.ghost_centre[1] = 0.4f, gp.ghost_scale = 0.5f, gp.ghost_alpha = std::floor(0.5f * 255.f) / 255.f;
        gp.ghost_colour[0] = 1.f, gp.ghost_colour[1] = 0.8f, gp.ghost_colour[2] = 0.6f;
        gp.ghost_frame = 1000;
        f.post = &gp;
        std::vector<uint8_t> g1, g2;
        if (!fe.RenderToPixels(f, g1, err, &rw, &rh)) { std::fprintf(stderr, "post_rig: FAIL draw (ghost 1): %s\n", err.c_str()); return 3; }
        gp.ghost_frame = 1001;
        if (ghost_plant == "ignored") gp.ghost = false;   // planted twin: the effect is ignored on the frame that must show it
        if (!fe.RenderToPixels(f, g2, err, &rw, &rh)) { std::fprintf(stderr, "post_rig: FAIL draw (ghost 2): %s\n", err.c_str()); return 3; }
        scene_png::WritePngRgb(prefix + "_ghost1.png", g1, rw, rh), scene_png::WritePngRgb(prefix + "_ghost2.png", g2, rw, rh);
        // review 1: recording the SAME guest frame again (the 120 fps tween, the dev mirror) must give the same picture as once. The planted twin (--ghost-plant advance) records the repeat as the next guest frame.
        std::vector<uint8_t> g2b;
        if (ghost_plant == "advance") gp.ghost_frame = 1002;
        if (!fe.RenderToPixels(f, g2b, err, &rw, &rh)) { std::fprintf(stderr, "post_rig: FAIL draw (ghost 2 again): %s\n", err.c_str()); return 3; }
        const bool idem = g2b == g2;
        std::printf("ghost-test repeat of the same guest frame: %s\n", idem ? "identical" : "DIFFERS");
        if (!idem) rc = 3;
        const size_t stride = size_t(rw) * 4;   // (RenderToPixels: RGBA8, rows top first; S105 A1: the real read-back size)
        const float x0 = (gp.ghost_centre[0] - gp.ghost_scale * 0.5f) * float(rw), x1 = (gp.ghost_centre[0] + gp.ghost_scale * 0.5f) * float(rw);
        const float y0 = (gp.ghost_centre[1] - gp.ghost_scale * 0.5f) * float(rh), y1 = (gp.ghost_centre[1] + gp.ghost_scale * 0.5f) * float(rh);
        uint64_t first_diff = 0, inside_diff = 0, outside_diff = 0, inside_less = 0;
        for (uint32_t y = 0; y < rh; ++y)
          for (uint32_t x = 0; x < rw; ++x) {
            const size_t o = y * stride + x * 4;
            if (o + 2 >= off.size() || o + 2 >= g1.size() || o + 2 >= g2.size()) continue;
            for (int c = 0; c < 3; ++c) first_diff += off[o + c] != g1[o + c];
            const bool inside = float(x) + 0.5f > x0 + 1.5f && float(x) + 0.5f < x1 - 1.5f && float(y) + 0.5f > y0 + 1.5f && float(y) + 0.5f < y1 - 1.5f;   // (1.5 px off the edge: the rect's border is filtered)
            const bool outside = float(x) + 0.5f < x0 - 1.5f || float(x) + 0.5f > x1 + 1.5f || float(y) + 0.5f < y0 - 1.5f || float(y) + 0.5f > y1 + 1.5f;
            bool d = false, less = false;
            for (int c = 0; c < 3; ++c) d |= off[o + c] != g2[o + c], less |= g2[o + c] + 1 < off[o + c];
            if (inside) inside_diff += d, inside_less += less;
            else if (outside) outside_diff += d;
          }
        std::printf("ghost-test %s%s: first_frame_diff %llu outside_diff %llu inside_diff %llu inside_darker %llu\n", ghost_mode.c_str(), ghost_plant.empty() ? "" : (" plant " + ghost_plant).c_str(),
                    (unsigned long long)first_diff, (unsigned long long)outside_diff, (unsigned long long)inside_diff, (unsigned long long)inside_less);
        if (first_diff != 0 || outside_diff != 0 || inside_diff == 0 || (ghost_mode == "additive" && inside_less != 0)) { std::printf("ghost-test FAIL\n"); rc = 3; }
        else std::printf(rc == 0 ? "ghost-test PASS\n" : "ghost-test FAIL (repeat)\n");
      }
    }
    if (pst && rc == 0 && built) rc = PstTest(fe, cache, t, w, h, pp, pst_plant, prefix);   // S103 PST
    if (fxh && rc == 0 && built) rc = FxhTest(fe, cache, t, w, h, fxh_plant, prefix);   // S103 FXH
    {   // S103 SLV: the lever census of this run (two renders: without and with post; only the post one runs the post pass)
      char lite[512];
      rdr::CombineLiteCensusLine(1, lite, sizeof lite);
      std::printf("slv: combine_lite level %d, bloom clears skipped %llu, depth-convert passes %llu, depth-convert passes skipped %llu\n%s\n", rdr::g_combine_lite.load(), (unsigned long long)g_post_bloom_clears_skipped.load(), (unsigned long long)g_post_dconv_passes.load(), (unsigned long long)g_post_dconv_skipped.load(), lite);
    }
    std::printf("validation_errors %llu\n", (unsigned long long)fe.validation_errors());
    if (fe.validation_errors()) rc = 3;
    renderer->WaitIdle();
  }
  return rc;
}
