// Session 103, lane P: our post pass's parameters from the scene record (docs\renderer\POST_FROM_RECORD.md).
// Lane A's PostParamsRow (kScPostParams, the post settings object S as the frame's latch read it) -> rdr_scene::PostParams (post_pass.h). Per-frame values
// (the DoF ramps c20 / c21, the haze colour c27, the bloom / star strengths c31 / c32, the film colour) come from S; the per-look tables (blur taps and weights,
// the glare streaks) are DefaultPostParams' (POST_PASS.md: generated, proved to 6e-8 against the game's draws). Header-light: no renderer image, no game.
#pragma once
#include <cstdint>
#include <string>

#include "post_pass.h"
#include "record_io.h"
#include "scene_record.h"

namespace rdr_scene::post_from_record {

// What Build decided, for the census line and the planted-fault tests.
struct Info {
  bool row = false;          // the record carries a post row at all
  bool read = false;         // ... and S was readable at the latch (kPpRead)
  bool post_on = false;      // S+0 non-zero (the post module draws)
  bool dof = false, fog = false, bloom = false, glare = false, film = false;   // what the pass will run
  // S103 FXF: yAfterImage (the stun / comeback / signature / finisher ghost) and the monotone's two kinds. afterimage = the pass draws the ghost; blend = S+272 (0 alpha, 1 additive);
  // film_dropped_sepia = VINTAGE LOOK off removed the arena's own sepia; film_kept_tint = VINTAGE LOOK off, yet a moment tint (gGsf index >= 0) stayed on.
  bool afterimage = false, film_dropped_sepia = false, film_kept_tint = false, disp_default = false;   // disp_default: the display size was unreadable at the latch (1280 x 720 used)
  uint32_t blend = 0;
  // S103 PST: tonemap (S+180) / night (S+184) are drawn through the adapted luminance, aa = antialiasing (S+280) runs first (aa_skipped: the word is set but the game skips it: DoF + bloom + monotone, no HDR);
  // dead_flags = PostParamsFlags of the halation / flare / radial-blur words that are set (kPpHala300 = effect 5 switch, kPpHalation = the game's halation gate S+0 && S+304 && S+308, kPpRadial = gate && S+524 > 0; S103 HCN).
  bool tonemap = false, night = false, aa = false, aa_skipped = false;
  uint32_t dead_flags = 0;
  // S103 FXH: the highlight-reel cue effects (defocus + hue S+560, emboss S+592, retro film S+604): drawn when set, with the cue switch on (wwe2k14_rdr_cue_post, default on). retro_off_setting = the word is set but the player's
  // FILM GRAIN setting is off (the pass is left out, as the stream build draws the CLEAN twin); cue_garbage = a word's numbers are not finite (that effect is left out).
  bool defocus = false, emboss = false, retro = false, retro_off_setting = false, cue_garbage = false;
  bool missing = false;     // Build returned false because an INPUT is missing (no row, S unreadable, a stale row, no perspective camera): the caller counts a miss / blocks READY.
                             // false + !missing = the frame genuinely has no post (module off, no effect on): drawn without post
  bool strength_default = false;   // S+172 was outside the range the capture's yDownScale_dof c21.w sits in: the default 0.0003 is used
  uint32_t unsupported = 0;  // PostParamsFlags the pass cannot draw (tonemap, night, defocus, emboss, retro): none is in any captured frame (post_effects.md "Colour grade and film grain")
  float q = 0, nq = 0;       // the projection's depth terms read from the camera (ProjQ)
  const char* why = "";     // a string literal (no heap per frame): set when Build returns false
};

// The projection's q and nq from the frame's world -> clip matrix (view * proj, row vectors): clip.z = q * clip.w - nq for a perspective matrix whatever the view's rotation
// (column 2 = q * column 3). The same maths as the viewer's Renderer::ProjQ, on the front-end's own product. False when the matrix is not a perspective projection.
bool ProjQ(const Camera& cam, float& q, float& nq);

// Fills `out` for a frame of size w x h. Returns false (out untouched) when there is no post to draw: no row, S unreadable, the module off, or no effect on; Info::why says which.
// A false return is NOT a miss: the frame is drawn without post, as the game's frame would be.
// S103 FXH: `cue_post` = the stream-free switch wwe2k14_rdr_cue_post (default ON): off = the three cue words are named unsupported and not drawn (the state before this lane).
bool Build(const SceneTables& rec, const Camera& cam, uint32_t w, uint32_t h, PostParams& out, Info& info, bool cue_post = true);

// One census line (cheap; strings built here only). Counters are kept by the caller (LiveSceneDraw) with relaxed adds.
std::string Describe(const Info& info);

}  // namespace rdr_scene::post_from_record
