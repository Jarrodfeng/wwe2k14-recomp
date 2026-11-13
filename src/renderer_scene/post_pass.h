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
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../renderer/frame_builder.h"
#include "../renderer/renderer.h"

namespace rdr_scene {

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
  // S101 lane PS: the game's shapes (post_plan.h fills these from the stream's own chain; the defaults leave the pass as it was)
  bool dof_chain = false;       // run dof_down + hole_fill although the combine has no dof permutation (the bloom chain's source: the editor, the cards)
  bool bloom_variant = false;   // the combine has the bloom permutation (reads slots 7 / 8) even when the bloom / glare chain is not recorded: both images are cleared
  bool dead_bloom = false;      // BloomScale.x == 0: the bloom blur (yBlur_t2, yBlur_t1_t2 x2) is not recorded (the slot 7 image is cleared)
  bool dead_glare = false;      // StarScale.x == 0: the star glare (yBlur_t1 x8, yBlur_m1_m2) is not recorded (the slot 8 image is cleared). Both: the bright pass goes too
  rdr::ImageId far_blur = rdr::kNoImage;  // the combine's slot 6 when an earlier DoF layer made it (Bryan's entrance): that image, not this layer's blur
  // S101 lane PO: the pass's own images (quarter / eighth size, the film's) follow the render scale (kUsageScaled), as the game's 16:9 post_step targets do in the owner renderer.
  // Default false: the viewer's shots and post_proof are unscaled. The planted fault unscaled_steps turns it off at a render scale above 1.
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
  static std::string CombineVariantName(const PostParams& p);
  uint32_t passes_recorded() const { return passes_; }  // draws recorded by the last Record (the proof prints it)
  bool combined() const { return combined_; }           // S101 lane PO: the last Record drew the combine (the check behind the planted fault no_combine)
  // The pass's own intermediate pictures after Record has rendered (the proof compares each with the game's: a per-pass breakdown).
  rdr::ImageId dof_blur() const { return q2_; }   // hole_fill: the depth of field's blurred picture (the game's image read by combine slots 2 and 6)
  rdr::ImageId bloom_image() const { return b1_; }  // combine slot 7
  rdr::ImageId glare_image() const { return gsum_; }  // combine slot 8

 private:
  struct Variant;
  int Find(const char* name);
  uint16_t Program(int vs, int ps);
  uint16_t Layout(int vs);
  rdr::ImageId Make(uint32_t w, uint32_t h);
  bool Ensure(uint32_t w, uint32_t h, bool scaled, std::string& error);  // S101 lane PO: + scaled

  rdr::Renderer& renderer_;
  std::map<uint64_t, uint16_t> programs_;
  std::map<int, uint16_t> layouts_;
  std::vector<std::string> names_;
  uint32_t size_w_ = 0, size_h_ = 0;
  std::vector<rdr::ImageId> images_;
  rdr::ImageId q1_ = rdr::kNoImage, q2_ = rdr::kNoImage, b0_ = rdr::kNoImage, b1_ = rdr::kNoImage, b2_ = rdr::kNoImage, gt_ = rdr::kNoImage;
  rdr::ImageId g_[4] = {}, gsum_ = rdr::kNoImage, mid_ = rdr::kNoImage, dconv_ = rdr::kNoImage;
  uint16_t depth_program_ = 0, depth_layout_ = 0;
  bool depth_program_made_ = false;
  uint32_t passes_ = 0;
  bool scaled_ = false, combined_ = false;  // S101 lane PO
};

}  // namespace rdr_scene
