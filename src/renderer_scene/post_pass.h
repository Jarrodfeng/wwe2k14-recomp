// Session 101, lane 34: the scene renderer's post pass (docs\renderer\POST_PASS.md).
// Scene colour + scene depth in, the game's final picture out, as OUR sequence of passes built from the proven post effects (src\renderer\shaders\fx:
// yDownScale, yBlur, yGlareEtc, yMonotone). No game draw is replayed: the pass reads a PostParams table (named after the effects' constants) and records
// its own ops into the caller's FrameBuilder.
//
// The chain (match: 1-2 and 6; entrance: all; 30 Years: + 7):
//   1. dof_down    yDownScale_dof        scene colour + depth        -> quarter size (w/4 x h/4): depth-aware 2x2 average, alpha = "in the blur band"
//   2. hole_fill   yBlur_hole_fill       dof_down                    -> quarter size: the blurred picture of the depth of field
//   3. bright      yDownScale_box4_down  hole_fill                   -> eighth size: 2x2 average minus g_fDownColor.x (the bright pass)
//   4. bloom       yBlur_t2, yBlur_t1_t2 x2 (bright -> a -> b -> a)  -> eighth size: the bloom
//   5. glare       yBlur_t1 x8 (4 streak directions, 2 passes each) + yBlur_m1_m2 (average of the 4) -> eighth size: the star glare
//   6. combine     yGlareEtc_[dof_][fog_][bloom]  scene colour, depth, hole_fill (x2), bloom, glare -> the picture
//   7. film        yMonotone (30 Years only)     the picture                  -> the picture, after the combine
// The display-gamma curve is NOT part of this pass: the game applies it at the present (Renderer::SetDisplayCurve), the viewer's rdr backend in its shot
// read-back (renderer_rdr.cpp). Every result line of the proof says so.
#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../renderer/frame_builder.h"
#include "../renderer/renderer.h"
#include "post_cue_fx.h"   // S103 FXH

namespace rdr_scene {

// S103 SLV: whole-run census (relaxed, once per recorded pass, never per pixel): full-size viewer-depth -> game-depth conversion passes recorded (post_pass.cpp, the dconv_ block), and the eighth-size
// clears the combine's no-bloom permutation (the lite lever) did not record. Printed by the stream-free host's [rdr][post_levers] line.
inline std::atomic<uint64_t> g_post_dconv_passes{0}, g_post_bloom_clears_skipped{0};
// S103 SLV2: the depth-convert fold (the `_vd` twins of yGlareEtc / yDownScale convert the raw scene depth where they fetch it): frames that skipped the dconv_ pass because of it (g_post_dconv_passes keeps
// counting the passes that still ran: a linear-DoF frame, or a depth of another size). Always on in the game (rule 6: no reason to keep the pass); g_post_depth_fold = 0 is the rigs' OFF twin (post_rig --depth-fold 0),
// g_post_depth_fold_plant = 1 the planted twin (the fold decodes with a wrong q: the picture must differ).
inline std::atomic<uint64_t> g_post_dconv_skipped{0};
inline std::atomic<int> g_post_depth_fold{1}, g_post_depth_fold_plant{0};
// S103 PST: RIG-ONLY planted twin (post_rig --pst-plant step): 1 = the adapted-luminance step ignores the guest frame and runs on every Record (a twin that steps twice per guest frame when a frame is recorded twice).
// The proof must catch it (the convergence and the repeat-identity checks). Never set by the game.
inline std::atomic<int> g_post_lum_plant_every{0};
// S103 PST fix: RIG-ONLY planted twin (--pst-plant pause): the paused-frames rule is not applied (a long off stretch then composes k = 30 steps: 0.70 instead of 0.0396 for the first step back).
inline std::atomic<int> g_post_lum_plant_nopause{0};
// S103 PST fix (FXF review 8 rule): an image the effect needs could not be made: the effect is dropped for that frame and counted here (never a failed frame). Printed on the [scene][draw] census.
inline std::atomic<uint64_t> g_post_aa_dropped{0}, g_post_lum_dropped{0};
// S103 FXH: the cue effects (defocus + hue, emboss, retro film). Dropped = an image the effect needs could not be made (that effect is left out for the frame, never a failed frame); passes = recorded cue passes;
// RIG-ONLY planted twins (post_rig --fxh-plant): 1 = the retro clock ticks on every Record (twice per guest frame when it is recorded twice), 2 = the noise strips are bound in each other's slot, 3 = post_from_record
// treats the cue words as unsupported again (the old mask), 4 = the cue passes run in the wrong order (retro before emboss before defocus). Never set by the game.
inline std::atomic<uint64_t> g_post_cue_dropped{0}, g_post_cue_passes{0};
inline std::atomic<int> g_post_cue_plant{0};
// S105 A1: RIG-ONLY planted twins (post_rig --a1-plant): dconv_unscaled = the game-depth image is made at the logical size although the scene is scaled (the DoF / haze read a quarter of the depth at scale 2);
// ghost_logical = the afterimage divides gl_FragCoord by the LOGICAL size (the old shader: the sprite lands at half / double scale). Never set by the game.
inline std::atomic<int> g_a1_plant_dconv_unscaled{0}, g_a1_plant_ghost_logical{0};

struct PostParams {
  uint32_t width = 1280, height = 720;  // the scene's size; quarter = /4, eighth = /8 (the game's 320x180 and 160x90 at 1280x720)
  bool dof = true, fog = true, bloom = false, glare = false, film = false;
  // 1 dof_down (yDownScale_dof): g_avSampleOffsets[0..3] (xy), g_f4DOFParam2 (x,y accepted blur-depth range, w depth tolerance)
  float dof_offsets[4][4] = {};
  float dof_param2_down[4] = {};
  // The depth the downsample reads is filtered LINEAR in the match and POINT in entrances (the games' own sampler state, read from the captures:
  // s100_match300 / s100_wm30_300 slot 1 "linear", s100_s89k_a "nearest"). With linear, each tap (placed on the corner of 4 full-size texels) averages them.
  bool dof_depth_linear = false;
  // The viewer's scenes (not the game's frames): `depth` is an ordinary 0..1 depth; the pass first rewrites it into the game's depth, 9.92 / w
  // (rdr_post_depth.frag; q = the projection's m[10], nq = -m[14]) and samples that, unfiltered.
  bool depth_from_viewer = false;
  float proj_q = 1.f, proj_nq = 1.f;
  // S103 SLV2: the size of the depth image the caller binds (0 = unknown). The depth-convert fold (post_pass.cpp) reads the raw depth in the consumers' own shaders at the pixel they draw, so it needs the depth to be
  // the size of the pass (width x height, the conversion target's size); front_end.cpp sets these from the scene target it made both images with. Unknown or different: the dconv_ pass runs, as before.
  uint32_t depth_w = 0, depth_h = 0;
  // 2 hole_fill (yBlur_hole_fill): SampleOffsets[12] (c1..c12), SampleWeights[13] (c16..c28)
  float hole_offsets[12][4] = {};
  float hole_weights[13][4] = {};
  // 3 bright (yDownScale_box4_down): g_avSampleOffsets[4], g_fDownColor
  float bright_offsets[4][4] = {};
  float bright_down_color[4] = {};
  // 4 bloom: yBlur_t2 (13 taps), then yBlur_t1_t2 (15 taps) twice
  float bloom_t2_offsets[13][4] = {};
  float bloom_t2_weights[13][4] = {};
  float bloom_t12_offsets[2][15][4] = {};
  float bloom_t12_weights[2][15][4] = {};
  // 5 glare: yBlur_t1 (8 taps) x 8 (direction d = passes 2d, 2d+1), yBlur_m1_m2 SampleWeights[4]
  float glare_offsets[8][8][4] = {};
  float glare_weights[8][8][4] = {};
  float glare_merge[4][4] = {};
  // 6 combine (yGlareEtc): DOFParam c20, DOFParam2 c21, RGB c27, Alpha c29, ZAdj c30, BloomScale c31, StarScale c32
  float DOFParam[4] = {}, DOFParam2[4] = {}, RGB[4] = {}, Alpha[4] = {}, ZAdj[4] = {}, BloomScale[4] = {}, StarScale[4] = {};
  // 7 film (yMonotone): g_fCb c0, g_fCr c1, g_fAlpha c2
  float film_cb[4] = {}, film_cr[4] = {}, film_alpha[4] = {};
  // S103 FXF: yAfterImage (post effect 4, after the film): the PREVIOUS output of this step, drawn over the new one as a sprite of the display's size, scaled `ghost_scale` about `ghost_centre` (0..1 of the picture),
  // tinted by `ghost_colour`, at `ghost_alpha` (floor(S+228 * 255) / 255), additive when `ghost_additive` (S+272 == 1). The pass keeps the history image itself (a 360 HABIT dropped: the game resolves the frame into a
  // "yAfterImage" texture every frame). `ghost_frame` = the record's guest frame: a draw whose frame is more than kGhostGap past the last ghost draw starts a new effect (no blend, the history is refilled).
  bool ghost = false, ghost_additive = false;
  float ghost_centre[2] = {0.5f, 0.5f}, ghost_scale = 1.f, ghost_alpha = 0.f, ghost_colour[3] = {1.f, 1.f, 1.f};
  uint64_t ghost_frame = 0;
  // S103 PST (COV9_story.md 2a / 2e): TONEMAP (S+180) and NIGHT (S+184), and ANTIALIASING (S+280).
  //   tonemap / night: the combine's yGlareEtc `tonemap` / `night` permutations, which read the ADAPTED luminance (one float) at texture 9. The pass measures the scene (log-average luma: yToneMap initial 9 taps -> 64x64,
  //   iterative 64 -> 16 -> 4, final -> 1x1) and steps a persistent 1x1 (yGlareEtc `adapt`) ONCE PER GUEST FRAME, ElapsedTime = 1/15 per frame (follow 0.0396), idempotent per guest frame like the FXF history:
  //   `guest_frame` = the record's frame; the same frame again re-runs the same step from the same previous value. MiddleGray c33 = 0.18 (0x82004F4C). 360 HABIT dropped: the game's 4 passes into quarter rects of one
  //   EDRAM target + 3 resolves (docs\renderer\360_HABITS.md); ours are 4 passes of the proven shaders into four small R32F images and no copy. Night alone (S+180 off) keeps the last measured value.
  //   aa: the edge-aware blur (yAntialiasing) over the scene colour BEFORE every other pass; aa_offsets = (1/W, 1/H) of the game's picture (never finer than one drawn texel), the `fixed` variant (threshold 0.64).
  bool tonemap = false, night = false, aa = false;
  float middle_gray = 0.18f;
  uint64_t guest_frame = 0;
  float aa_offsets[2] = {1.f / 1280.f, 1.f / 720.f};   // [C] the game always draws pass 0 of T_Antialiasing = `fixed` (threshold 0.64): the ctor sub_82C2BE68 sets [obj+44] = 0, S+284 has no writer, effect_table: p0 = yAntialiasing_fixed_fragment
  // S103 FXH (COV9_story.md 2b / 2c / 2d): the highlight-reel cue effects, drawn AFTER the afterimage in the game's order (7 defocus + hue, 8 emboss, 9 retro film), each over the previous one's picture. All three are 0 in
  // normal play (S+560 / +592 / +604 are set only by the cue player's sequence).
  //   defocus: yDefocus `hue`: mix(frame, the DoF blur (q2_, which the DoF chain makes for it though S+4 is off: set dof_chain), defocus_rate), then the hue shift of defocus_ycbcr = (Y, Cb, Cr, A).
  //   emboss:  yEmboss: the red channel's relief, the step `emboss_step` = S+596 DISPLAY pixels over the display's size, in uv units of the GAME's picture (exact, not rounded to a drawn texel: at a render scale the step stays the same fraction of the picture).
  //   retro:   yRetroFilm: shake / flicker / grain on the luma. retro_range = g_Range (S+640..652), retro_jitter = 4 / the display's height. The three pictures are OUR OWN noise (post_cue_fx.h: the game's are [U]); the clock
  //   (g_Time) steps once per guest frame (cue_fx::RetroAdvance, idempotent) with the game's frame rate retro_rate (dt = 1 / rate).
  bool defocus = false, emboss = false, retro = false;
  float defocus_rate = 0.f, defocus_ycbcr[4] = {};
  float emboss_step[2] = {};
  float retro_range[4] = {}, retro_jitter = 4.f / 720.f, retro_rate = 30.f;
  // S101 lane PS: the game's shapes (post_plan.h fills these from the stream's own chain; the defaults leave the pass as it was)
  bool dof_chain = false;       // run dof_down + hole_fill although the combine has no dof permutation (the bloom chain's source: the editor, the cards)
  bool bloom_variant = false;   // the combine has the bloom permutation (reads slots 7 / 8) even when the bloom / glare chain is not recorded: both images are cleared
  bool dead_bloom = false;      // BloomScale.x == 0: the bloom blur (yBlur_t2, yBlur_t1_t2 x2) is not recorded (the slot 7 image is cleared)
  bool dead_glare = false;      // StarScale.x == 0: the star glare (yBlur_t1 x8, yBlur_m1_m2) is not recorded (the slot 8 image is cleared). Both: the bright pass goes too
  rdr::ImageId far_blur = rdr::kNoImage;  // the combine's slot 6 when an earlier DoF layer made it (Bryan's entrance): that image, not this layer's blur
  // S101 lane PO: the pass's own images (quarter / eighth size, the film's) follow the render scale (kUsageScaled), as the game's 16:9 post_step targets do in the owner renderer.
  // Default false: the viewer's shots and post_proof are unscaled. The planted fault unscaled_steps turns it off at a render scale above 1.
  bool depth_clamped = false;   // S102 R24-E: the depth image bound to the combine holds values in [0, 1] (a depth buffer / UNORM image; the hook sets it from the capture's image format); false = wwe2k14_rdr_gpu_small declines
  bool scaled_images = false;
  // S101 lane PO: the dof depth slot's address mode. false (default): clamp to edge. true: REPEAT, as the stream's sampler is in the 30 Years frames and the match (the taps one game texel off the
  // edge then wrap); at a render scale of 2 the wrapped texel is a real, different texel (a scale artefact of the 360's sampler, rule 6), so the shipped choice is clamp. The planted option
  // dof_depth_address mirrors the stream's mode to prove that this is the whole difference at scale 2.
  bool dof_depth_repeat = false;
  // lane 140: the 30 Years / match frames whose depth-of-field blur is ONE yBlur_t2 pass over an image the stream made (sequence "t2 combine [monotone]"): no dof_down / hole_fill, the blur
  // (the combine's slots 2 and 6) is the t2 of `blur_source` (quarter size, point-filtered), tables = bloom_t2_offsets / bloom_t2_weights. The combine keeps its dof permutation (dof = true).
  bool t2_blur = false;
  rdr::ImageId blur_source = rdr::kNoImage;
  // lane 238: the DoF blur's own t2 tables (the victory scene has a bloom t2 too: 'copy_depth t2 box4_down t2 t1_t2x2 ...'), and the m1_m2 merge's per-slot filter (the stream binds slot 3 POINT in run 7131's fatal-4 frames)
  float blur_t2_offsets[13][4] = {};
  float blur_t2_weights[13][4] = {};
  bool glare_merge_linear[4] = {true, true, true, true};
  bool stop_before_combine = false;   // S101 lane PO planted fault no_combine: Record returns (true) before the combine pass; combined() says so
};

// The viewer's look settings on rdr (match: the S72 / S98 constants of BuildPostPlan, depth of field + haze; entrance: + bloom and glare). `entrance`
// switches the entrance combine's far-blur ramp; the blur tables are the game's (generated from their formulas, checked against a capture by the proof).
PostParams DefaultPostParams(uint32_t width, uint32_t height, bool entrance);

class PostPass {
 public:
  explicit PostPass(rdr::Renderer& renderer) : renderer_(renderer) {}
  ~PostPass();
  PostPass(const PostPass&) = delete;
  PostPass& operator=(const PostPass&) = delete;

  // Appends the pass to `builder`: reads `colour` (RGBA8, sampled) and `depth` (Format::Depth, sampled), writes `out` (RGBA8 target of the scene's size).
  // The images it needs are created here and kept (re-made when the size changes; the caller waits for the GPU before a size change). False + error when
  // an effect variant or an image is missing.
  bool Record(rdr::FrameBuilder& builder, rdr::ImageId colour, rdr::ImageId depth, rdr::ImageId out, const PostParams& params, std::string& error);

  // Live shader reload (FrontEnd::ReloadPrograms): forgets the programs that use one of the changed registry variants (made again at the next Record).
  void Reload(const std::vector<int>& changed_variants);
  // S101 lane PO: the combine's registry variant name for these parameters (one function for the pass and for the hook's presence check).
  static std::string CombineVariantName(const PostParams& p, bool drop_bloom = false);   // S102 R18-B2: drop_bloom = the permutation without the bloom (its images are black)
  uint32_t passes_recorded() const { return passes_; }  // draws recorded by the last Record (the proof prints it)
  bool ghost_first() const { return ghost_first_; }   // S103 FXF
  void DisarmLum() { lum_paused_ = true; }   // S103 PST: a frame with no post walk (S+180 / S+184 not set for the game): the adapted luminance does not step; the next step is ONE guest frame, not the frames in between
  void DisarmGhost() { ghost_armed_ = false; }   // S103 FXF review 4: a frame with no post walk (S+224 = 0 for the game) ends the effect
  // S103 PST: distinct adapt steps recorded since the pass was made (one per guest frame; a repeat of a guest frame is not one).
  uint64_t lum_steps() const { return lum_steps_; }
  bool combined() const { return combined_; }        // S101 lane PO: the last Record drew the combine (the check behind the planted fault no_combine)
  // The pass's own intermediate pictures after Record has rendered (the proof compares each with the game's: a per-pass breakdown).
  rdr::ImageId depth_convert_image() const { return dconv_; }   // S105 A1: the game-depth image (R32F): scaled with the chain, so its pass reads the whole scene depth
  rdr::ImageId dof_blur() const { return q2_; }   // hole_fill: the depth of field's blurred picture (the game's image read by combine slots 2 and 6)
  rdr::ImageId bloom_image() const { return b1_; }  // combine slot 7
  rdr::ImageId glare_image() const { return gsum_; }  // combine slot 8

 private:
  struct Variant;
  int Find(const char* name);
  uint16_t Program(int vs, int ps);
  uint16_t Layout(int vs);
  rdr::ImageId Make(uint32_t w, uint32_t h);
  bool Ensure(uint32_t w, uint32_t h, bool scaled, bool viewer_depth, std::string& error);   // viewer_depth: the frame reads the depth-convert image (S105 A1 review F5: only then is it made at the real size)  // S101 lane PO: + scaled

  rdr::Renderer& renderer_;
  std::map<uint64_t, uint16_t> programs_;
  std::map<int, uint16_t> layouts_;
  std::vector<std::string> names_;
  uint32_t size_w_ = 0, size_h_ = 0;
  std::vector<rdr::ImageId> images_;
  rdr::ImageId q1_ = rdr::kNoImage, q2_ = rdr::kNoImage, b0_ = rdr::kNoImage, b1_ = rdr::kNoImage, b2_ = rdr::kNoImage, gt_ = rdr::kNoImage;
  rdr::ImageId g_[4] = {}, gsum_ = rdr::kNoImage, mid_ = rdr::kNoImage, dconv_ = rdr::kNoImage;
  rdr::ImageId pre_ = rdr::kNoImage, hist_[2] = {rdr::kNoImage, rdr::kNoImage};   // S103 FXF: the chain output before the ghost, and the two history images (made when the ghost first plays; zero cost while it is off)
  uint32_t logged_w_ = 0, logged_h_ = 0;
  int logged_scaled_ = -1, logged_dconv_scaled_ = -1;   // S105 A1 review F5: the last chain the [scene][targets] line printed
  bool viewer_depth_ = false;   // S105 A1: sticky upward per size / scale: the depth-convert image has been asked for (it is made at the real size only then)
  uint16_t depth_program_ = 0, depth_layout_ = 0, ghost_program_ = 0, ghost_layout_ = 0;
  bool depth_program_made_ = false, ghost_program_made_ = false, ghost_armed_ = false;
  uint64_t ghost_last_frame_ = 0;
  uint32_t ghost_read_ = 1, ghost_write_ = 0, ghost_k_ = 1;   // which history image this guest frame reads / writes; guest frames since the last ghost draw (the pace)
  bool ghost_first_ = false;   // the last Record's ghost was a first frame (no blend): the planted twins / the census read it
  // S103 PST: the luminance chain. lum_[0..2] = 64x64 / 16x16 / 4x4 R32F (the size-independent pyramid; made with the first tonemap / night frame), lum_cur_ = this frame's measured 1x1, lum_a_[2] = the adapted 1x1
  // ping-pong (lum_latest_ = the one holding the newest step). These live as long as the pass: a render-size change does not reset the adaptation (they are not in images_).
  rdr::ImageId lum_[3] = {rdr::kNoImage, rdr::kNoImage, rdr::kNoImage}, lum_cur_ = rdr::kNoImage, lum_a_[2] = {rdr::kNoImage, rdr::kNoImage}, aa_img_ = rdr::kNoImage;
  rdr::ImageId lq_ = rdr::kNoImage;   // the quarter-size colour the luminance measure reads when the DoF chain did not make q1_ this frame (in images_)
  uint32_t lum_latest_ = 0, lum_read_ = 1;   // lum_latest_ = the image this guest frame's step WRITES (= the newest adapted value), lum_read_ = the one it reads (the value before the step)
  uint64_t lum_last_frame_ = 0, lum_steps_ = 0;
  float lum_elapsed_ = 1.f / 15.f;   // the current guest frame's ElapsedTime (kept so a repeat of the frame steps the same way)
  bool lum_paused_ = false, lum_made_ = false, lum_just_made_ = false, lum_armed_ = false, lum_measured_ = false;
  bool MakeLum();
  rdr::ImageId MakeF(uint32_t w, uint32_t h, bool in_images);
  // S103 FXH: the cue effects. cue_[0] / cue_[1] = the two full-size ping-pong images (in images_: a size change re-makes them), noise_[0..2] = the retro film's strip 1, strip 2 and grain (ours, made once with the first
  // retro frame, kept for the pass's life: not in images_), retro_ = the clock.
  rdr::ImageId cue_[2] = {rdr::kNoImage, rdr::kNoImage}, noise_[3] = {rdr::kNoImage, rdr::kNoImage, rdr::kNoImage};
  cue_fx::RetroClock retro_;
  bool MakeNoise();
  uint32_t passes_ = 0;
  bool scaled_ = false, combined_ = false;  // S101 lane PO
};

}  // namespace rdr_scene
