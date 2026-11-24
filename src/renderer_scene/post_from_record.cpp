// See post_from_record.h. Session 103, lane P.
#include "post_from_record.h"
#include "rows_inputs.h"   // S106 G4-2
#include "../start_settings.h"   // S103 SPB: PlayerQuality (header-only part)

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../scene_live/post_settings.h"

namespace rdr_scene::post_from_record {

namespace {
// S+664 .. +692 haze values: c27 = S+676.. (post_settings.h [C]); c29 / c30 "from the neighbours" are [U] (post_effects.md): the shipped constants (identical in every captured frame) are used.
// S103 PST: tonemap and night are drawn now (the adapted-luminance chain, post_pass.cpp); antialiasing is drawn (S+280). S103 FXH: defocus + hue, emboss and retro film (the highlight-reel cue effects, COV9 2b-2d) are drawn
// too, so nothing is unsupported any more; kCueMask names the three words only for the cue switch's OFF state (wwe2k14_rdr_cue_post = false: the state before this lane: not drawn, counted unsupported).
constexpr uint32_t kUnsupportedMask = 0;
constexpr uint32_t kCueMask = kPpDefocus | kPpEmboss | kPpRetro;
constexpr uint32_t kDeadMask = kPpHala300 | kPpHalation | kPpRadial;   // census only (COV9 2f; S103 HCN: the game's gate, HRB.md 1)
}  // namespace

bool ProjQ(const Camera& cam, float& q, float& nq) {
  float m[16];
  if (cam.flags & kCamViewIsViewProj) {
    for (int i = 0; i < 16; ++i) m[i] = cam.view.m[i];
  } else {   // the front-end's product (FrontEnd::Record): view * proj, row vectors
    for (int i = 0; i < 4; ++i)
      for (int j = 0; j < 4; ++j) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += cam.view.m[i * 4 + k] * cam.proj.m[k * 4 + j];
        m[i * 4 + j] = s;
      }
  }
  int k = 11;   // the largest of m3 / m7 / m11 (the view's forward vector: any one of them can be ~0)
  if (std::fabs(m[3]) > std::fabs(m[k])) k = 3;
  if (std::fabs(m[7]) > std::fabs(m[k])) k = 7;
  if (!(std::fabs(m[k]) > 1e-9f)) return false;
  q = m[k - 1] / m[k];
  nq = q * m[15] - m[14];
  return std::isfinite(q) && std::isfinite(nq) && nq != 0.f;
}

bool Build(const SceneTables& rec, const Camera& cam, uint32_t w, uint32_t h, PostParams& out, Info& info, bool cue_post) {
  info = Info();
  if (rec.post_params.empty()) { info.why = "no post row in the record", info.missing = true; return false; }
  info.row = true;
  const PostParamsRow& r = rec.post_params[0];
  if (!(r.flags & kPpRead)) { info.why = "post settings unreadable at the latch", info.missing = true; return false; }
  info.read = true;
  // the row is the latch's of THIS record: the record's own frame, or the hook clock's (record j carries hook frame j - 1: FRAME_CONTRACT 3); anything else is a stale row
  const uint64_t gf = rec.header.guest_frame;
  if (r.frame != gf && r.frame + 1 != gf) { info.why = "post row is from another frame", info.missing = true; return false; }
  const uint32_t flags = post_settings::FlagsOf(r, true);   // recomputed from the words: a row whose flags word disagrees with its words is judged by the words
  info.post_on = (flags & kPpOn) != 0;
  if (!info.post_on) { info.why = "post module off (S+0 = 0)"; return false; }
  if (g_post_cue_plant.load(std::memory_order_relaxed) == 3) cue_post = false;   // RIG-ONLY twin (post_rig --fxh-plant oldmask): the cue words are unsupported again; the build cases must catch it
  info.unsupported = (flags & (kUnsupportedMask | (cue_post ? 0u : kCueMask))) | (flags & (kPpHalation | kPpRadial));   // S103 HCN: halation / radial are not drawn: if the gate ever opens, `unsupported flags on N frames` names it
  info.dead_flags = flags & kDeadMask;   // S103 PST: halation / flare / radial blur words (census: a run names them if one ever fires)
  // S103 PST: antialiasing (S+280) runs first, and is SKIPPED when DoF + bloom + monotone are on with no HDR (S+4 == 1, S+176 == 1, S+180 == 0, S+184 == 0, S+208 == 1: COV9 2e [C]).
  const bool aa_word = (flags & kPpAntialias) != 0;
  const bool aa_skipped = aa_word && post_settings::Word(r, post_settings::kDof) == 1 && post_settings::Word(r, post_settings::kBloom) == 1 && post_settings::Word(r, post_settings::kTonemap) == 0 &&
                          post_settings::Word(r, post_settings::kNight) == 0 && post_settings::Word(r, post_settings::kMonotone) == 1;
  const bool aa = aa_word && !aa_skipped;
  const bool tonemap = (flags & kPpTonemap) != 0, night = (flags & kPpNight) != 0;
  info.aa = aa, info.aa_skipped = aa_skipped, info.tonemap = tonemap, info.night = night;
  // S103 SPB (SP2 M3): the player's DEPTH OF FIELD and FILM GRAIN settings (start_settings.h PlayerQuality; the stream binder reads the same struct): off = the combine's permutation without depth of field /
  // the 30 Years old-film pass left out (the scene as it is), exactly what binder.h does for g_depth_of_field / g_film_grain. Defaults are on: nothing changes unless the player turned them off.
  const wwe2k14::start::PlayerQuality& quality = wwe2k14::start::Quality();
  // S103 FXF (the tint rule, FXP.md adversary C): VINTAGE LOOK off drops the monotone ONLY when it is the arena's own sepia. The 'gGsf' node (sub_825B7A38) writes S+208..+220 every frame: index -1 = the arena's row
  // (the 30 Years sepia), index >= 0 = a moment tint. The latch carries the index (PostParamsRow::gsf_code: 1 = -1, >= 2 = a moment tint, 0 = unknown). Unknown counts as the arena sepia (the old rule).
  const bool mono_on = (flags & kPpMonotone) != 0;
  const bool moment_tint = r.gsf_code >= 2;
  const bool film = mono_on && (quality.film_grain || moment_tint);
  info.film_dropped_sepia = mono_on && !film, info.film_kept_tint = mono_on && moment_tint && !quality.film_grain;
  const bool dof = (flags & kPpDof) != 0 && quality.depth_of_field, fog = (flags & kPpFog) != 0;
  const bool hdr = (flags & kPpBloom) != 0;           // bloom + glare chain
  // S103 FXH (COV9 2b / 2c / 2d): the cue effects. A word whose numbers are not finite leaves that effect out (a garbage row draws nothing, as the ghost). The retro film follows the player's FILM GRAIN setting
  // (VINTAGE LOOK): off = the pass is left out, exactly what the stream build's CLEAN twin draws (the scene as it is).
  using namespace post_settings;
  const float disp_w = r.fx[kFxDispW] ? float(r.fx[kFxDispW]) : kDisplayW, disp_h = r.fx[kFxDispH] ? float(r.fx[kFxDispH]) : kDisplayH;
  const auto all_finite = [&](uint32_t off, int n) { for (int i = 0; i < n; ++i) if (!std::isfinite(Float(r, off + 4 * i))) return false; return true; };
  bool defocus = cue_post && (flags & kPpDefocus) != 0, emboss = cue_post && (flags & kPpEmboss) != 0, retro = cue_post && (flags & kPpRetro) != 0;
  if (defocus && !(all_finite(kDefocusRate, 1) && all_finite(kDefocusColour, 4))) defocus = false, info.cue_garbage = true;
  if (emboss && !all_finite(kEmbossStep, 1)) emboss = false, info.cue_garbage = true;
  if (retro && !all_finite(kRetroRange, 4)) retro = false, info.cue_garbage = true;
  if (retro && !quality.film_grain) retro = false, info.retro_off_setting = true;
  const bool dof_chain = (hdr || defocus) && !dof;    // the bloom chain starts from the DoF blur: with the combine having no DoF permutation the chain still runs (PostParams::dof_chain); S103 FXH: the defocus mixes that blur (its enable test includes S+560: COV9 2b)
  const bool ghost = (flags & kPpAfterImage) != 0;    // S+0 != 0 (checked above) and S+224 != 0
  if (!dof && !fog && !hdr && !film && !ghost && !tonemap && !night && !aa && !defocus && !emboss && !retro) { info.why = "no effect on"; return false; }
  float q = 0, nq = 0;
  const bool needs_depth = dof || fog || dof_chain;
  if (needs_depth && !ProjQ(cam, q, nq)) { info.why = "camera is not a perspective projection (no depth terms)", info.missing = true; return false; }
  info.q = q, info.nq = nq;

  // The per-look tables: the viewer's own (generated, checked against the game's draws by post_proof --defaults). `entrance` there = the HDR look: it picks the DoF depth
  // sampler (nearest with bloom in all 4 captures read, linear without; [U] POST_PASS.md; brock_losswin f20 / 110 / 200 are nearest with bloom ON in a MATCH).
  // S103 P2: the tap tables are in uv units of the GAME's picture (720 high: the game's blur offsets are fixed fractions of the screen whatever the render scale, as its post_step
  // targets follow kUsageScaled). Built from the drawn size (2560x1440) every tap was half as wide in uv: the far crowd looked sharp at scale 2 (run 27 sheets). The tables come from
  // the logical size (height 720, width by the aspect, so an ultrawide picture keeps square taps); the images stay the drawn size.
  static const bool plant_drawn_taps = std::getenv("WWE_RDR_PLANT_POST_DRAWN_TAPS") != nullptr;   // dev plant (tools\scene\post_p2_check.py): the old behaviour, taps from the drawn size
  const uint32_t lh = plant_drawn_taps ? h : 720, lw = plant_drawn_taps ? w : uint32_t(std::lround(double(w) * 720.0 / double(h ? h : 720)));
  PostParams p = DefaultPostParams(lw ? lw : 1280, lh, hdr);
  p.width = w, p.height = h;
  const post_settings::Combine c = post_settings::CombineFrom(r);
  p.dof = dof, p.fog = fog, p.film = film;
  p.tonemap = tonemap, p.night = night, p.middle_gray = 0.18f, p.guest_frame = gf;   // S103 PST: MiddleGray c33 = 0.18 (0x82004F4C, COV9 2a)
  if (aa) {   // S103 PST: c0 = (1 / W, 1 / H) of the GAME's picture (the logical size: the same unit as the blur taps below), never finer than one drawn texel (a picture below the game's size); the pass is always `fixed` (post_pass.cpp)
    p.aa = true;
    p.aa_offsets[0] = std::fmax(1.f / float(lw ? lw : 1280), 1.f / float(w ? w : 1280)), p.aa_offsets[1] = std::fmax(1.f / float(lh ? lh : 720), 1.f / float(h ? h : 720));
  }
  for (int i = 0; i < 4; ++i) p.DOFParam[i] = c.c20[i], p.DOFParam2[i] = c.c21[i];
  p.dof_param2_down[0] = c.c21[0], p.dof_param2_down[1] = c.c21[1];
  // S+172 -> the downsample's c21.w (post_effects.md: "strength", sub_82C2B8A8). The captures' yDownScale_dof c21.w is 0.0003 in every frame read; a value in (0, 0.01] is taken as the
  // same unit and used as is, anything else keeps the default (counted). [U] until a capture's S shows a non-default S+172.
  const float strength = post_settings::Float(r, post_settings::kStrength);
  if (strength > 0.f && strength <= 0.01f) p.dof_param2_down[3] = strength;
  else info.strength_default = true;
  if (fog)
    for (int i = 0; i < 4; ++i) p.RGB[i] = c.c27[i];
  p.bloom = p.glare = p.bloom_variant = hdr;
  p.dof_chain = dof_chain;
  p.BloomScale[0] = hdr ? c.c31 : 0.f;
  p.StarScale[0] = hdr ? c.c32 : 0.f;
  p.dead_bloom = hdr && c.c31 == 0.f;
  p.dead_glare = hdr && c.c32 == 0.f;
  if (film) {   // [C] the order: c0 = S+212 (Cb), c1 = S+216 (Cr), c2 = S+220 (Alpha): sub_82C2A6E0 +104 / +118 / +12C (FXP.md adversary C, post_plan.cpp:578's register order)
    float cb = post_settings::Float(r, post_settings::kMonotone + 4), cr = post_settings::Float(r, post_settings::kMonotone + 8), al = post_settings::Float(r, post_settings::kMonotone + 12);
    if (moment_tint && !quality.film_grain && r.fx[kFxArena] == 1) {
      // S103 FXF review 2: VINTAGE LOOK off, a moment tint in an arena that has its own sepia. The node writes lerp(arena row, preset, t) x 0.01: the arena's share (1 - t) is removed so the tint fades in from "no tint"
      // (alpha = preset alpha x t x 0.01, the chroma = the preset's) instead of popping the whole sepia at the tint's start and end.
      float t, acb, acr, aal;
      std::memcpy(&t, &r.fx[kFxGsfT], 4), std::memcpy(&acb, &r.fx[kFxArena + 1], 4), std::memcpy(&acr, &r.fx[kFxArena + 2], 4), std::memcpy(&aal, &r.fx[kFxArena + 3], 4);
      if (std::isfinite(t) && t >= 0.f && t <= 1.f) {
        const float rest = 1.f - t;
        al = std::fmax(al - aal * rest, 0.f);
        if (t > 1e-3f) cb = (cb - acb * rest) / t, cr = (cr - acr * rest) / t;
      }
    }
    p.film_cb[0] = cb, p.film_cr[0] = cr, p.film_alpha[0] = al;
  }
  if (defocus) {   // S103 FXH: yDefocus `hue`: c0 = S+564, c1 = sub_82C292D0(S+576..588) = (Y, Cb, Cr, A)
    p.defocus = true;
    p.defocus_rate = Float(r, kDefocusRate);
    const float col[4] = {Float(r, kDefocusColour), Float(r, kDefocusColour + 4), Float(r, kDefocusColour + 8), Float(r, kDefocusColour + 12)};
    YCbCrFrom(col, p.defocus_ycbcr);
  }
  if (emboss) {    // S103 FXH: yEmboss: the step is S+596 DISPLAY pixels: c0 = (S+596 / W, S+596 / H) of the game's display
    p.emboss = true;
    p.emboss_step[0] = Float(r, kEmbossStep) / disp_w, p.emboss_step[1] = Float(r, kEmbossStep) / disp_h;
  }
  if (retro) {     // S103 FXH: yRetroFilm: g_Range = S+640..652, g_Time.w = 4 / the display's height; the clock is the pass's (cue_fx::RetroAdvance)
    p.retro = true;
    for (int i = 0; i < 4; ++i) p.retro_range[i] = Float(r, kRetroRange + 4 * i);
    p.retro_jitter = 4.f / disp_h;
    float rate;   // the game's frame rate [0x8392DC58] as the latch read it (fx[kFxRate]); unreadable / implausible = the default
    std::memcpy(&rate, &r.fx[kFxRate], 4);
    p.retro_rate = cue_fx::RatePlausible(rate) ? rate : cue_fx::kRetroRateDefault;
  }
  info.defocus = p.defocus, info.emboss = p.emboss, info.retro = p.retro;
  if (ghost) {   // S103 FXF: yAfterImage (FXP.md adversary E): centre = S+232 / +236 over the game's display size, scale S+240, tint S+256..+264 / 255, alpha floor(S+228 * 255) / 255, blend S+272 (1 = additive)
    p.ghost = true;
    const float dw = disp_w, dh = disp_h;   // review 6: the game's display size read at the latch, 1280 x 720 when unreadable
    info.disp_default = r.fx[kFxDispW] == 0 || r.fx[kFxDispH] == 0;
    p.ghost_centre[0] = Float(r, kAfterX) / dw, p.ghost_centre[1] = Float(r, kAfterY) / dh;
    p.ghost_scale = Float(r, kAfterScale);
    p.ghost_alpha = std::floor(std::fmin(std::fmax(Float(r, kAfterAlpha), 0.f), 1.f) * 255.f) / 255.f;
    p.ghost_colour[0] = float(Word(r, kAfterR) & 255u) / 255.f, p.ghost_colour[1] = float(Word(r, kAfterG) & 255u) / 255.f, p.ghost_colour[2] = float(Word(r, kAfterB) & 255u) / 255.f;
    p.ghost_additive = Word(r, kAfterBlend) == 1;
    p.ghost_frame = gf;
    info.afterimage = true, info.blend = Word(r, kAfterBlend);
    if (!(p.ghost_scale > 0.f) || !std::isfinite(p.ghost_scale) || !std::isfinite(p.ghost_centre[0]) || !std::isfinite(p.ghost_centre[1])) p.ghost = false, info.afterimage = false;   // a garbage row draws no ghost
  }
  p.depth_from_viewer = needs_depth, p.proj_q = q, p.proj_nq = nq;   // our scene depth is an ordinary 0..1 depth; the pass rewrites it into the game's (rdr_post_depth.frag)
  info.dof = dof, info.fog = fog, info.bloom = hdr && !p.dead_bloom, info.glare = hdr && !p.dead_glare, info.film = film;
  out = p;
  return true;
}

std::string Describe(const Info& info) {
  char b[520];
  if (!info.row || !info.read || !info.post_on || !(info.dof || info.fog || info.bloom || info.film || info.afterimage || info.tonemap || info.night || info.aa || info.defocus || info.emboss || info.retro))
    std::snprintf(b, sizeof b, "post none (%s)", info.why);
  else
    std::snprintf(b, sizeof b, "post dof %d fog %d bloom %d glare %d film %d afterimage %d (blend %u) tonemap %d night %d aa %d%s, cue defocus %d emboss %d retro %d%s, q %.5g nq %.5g%s%s", info.dof, info.fog, info.bloom, info.glare, info.film, info.afterimage, info.blend,
                  info.tonemap, info.night, info.aa, info.aa_skipped ? " (aa skipped by the game's rule)" : "", info.defocus, info.emboss, info.retro, info.retro_off_setting ? " (retro left out: FILM GRAIN off)" : "", info.q, info.nq, info.unsupported ? ", UNSUPPORTED flags set" : "", info.dead_flags ? ", halation words set" : "");
  return b;
}

}  // namespace rdr_scene::post_from_record
