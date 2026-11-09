// Session 98 (phone port, stage A): what every renderer backend of the Arena Viewer shares - the constant-buffer layouts of the
// viewer HLSL (viewer_hlsl.h), the shadow set-up maths, the post chain's constants and pass list. No graphics API here: a look
// fix made in these files (or in viewer_hlsl.h) lands in D3D11 and Vulkan at once.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "camera.h"

namespace view {

// cbuffer Frame (b0) and cbuffer Draw (b1) of hlsl::kScene.
struct FrameCB { Mat4 viewProj; float cam[4]; float l0[4]; float l1[4]; float amb[4]; float look[4]; float outc[4]; float lm[4]; };
struct DrawCB {
  float offset[4]; float dif[4]; float flags[4]; float flags2[4];
  float mat_amb[4]; float mat_dif[4]; float spec[4]; float misc[4];
  float ldir[4][4]; float lcol[4][4]; float sky[4]; float ground[4]; float limit[4]; float xform[4];
  float hl_dir[4]; float hl_col[4]; float sss1[4]; float sss2[4]; float refc[4]; float wspec[4]; float wmisc[4];  // Session 98 (skin fix)
  float wnrm[4]; float wdet[4]; float wdet_a[4]; float wdet_b[4];  // Session 98 (wrestler fix 3)
  float plcol[4][4]; float plscale[4];  // Session 98 (replay S5): the ambient cube's (one frame late) colours and the read-back ratio
};
static_assert(sizeof(DrawCB) == 36 * 16, "Renderer::CbSlot's table follows DrawCB");

// Session 98 (flash fix): cbuffer FlashCB of hlsl::kFlash (one crowd camera flash).
struct FlashCB { float col[4][4]; float centre[4]; float ax[4]; float ay[4]; float az[4]; float colour[4]; };

struct PickCB {  // cbuffer PickCB (b3) in the viewer HLSL
  uint32_t ids[4];   // x = (model << 20 | mesh) + 1, y = unused, zw = 0
  float mode[4];     // x = 1: the material is alpha blended / tested (clip below 0.5)
};
struct CompCB {  // cbuffer CompCB (b0) of the highlight composite (hlsl::kComp)
  float texel[4];    // xy = 1 / size
  float sel[4];      // rgb, a = strength (0 = off)
  float hov[4];
  float misc[4];     // x = glow radius px, y = outline radius px
};

// ---- shadows (Session 98 plan step 10; the maths moved here from shadow.cpp unchanged) ---------------------------------------
struct ShadowBox { float mn[3], mx[3]; float foot[3]; };  // a wrestler caster: world fit box (padded; turned + posed-feet aware) and the TRUE foot point (x, standing height, z)

struct ShadowCB {  // mirrors cbuffer Shadow (b2) in the viewer HLSL; row-major, row vectors
  float wr_vp[16];
  float props_vp[16];
  float wr_p[4];     // x = 1 / map size, y = depth bias (normalised), z = strength, w = enabled
  float wr_l[4];     // xyz = direction TO the light (viewer space), w = depth range in world units
  float props_p[4];  // x = enabled, y = strength, z = min height gap, w = fade range
  float plane_r[4];  // centre x, centre z, half extent, enabled
  float plane_l[4];  // xyz = point light, w = plane height
  float plane_p[4];  // x = strength, y = band half-height
  float blobs[6][4]; // xyz = foot position, w = sigma
  float blob_p[4];   // x = count, y = strength, z = band half-height
  float rim_p[4];    // Session 98 (wrestler fix 2): the rim light map: x, y = view-depth range (c162.x, c162.y), z = gain (c162.z), w = enabled
  float rim_s[4];    // xy = 1 / the scene size (screen position -> map coordinate)
  float wr_r[4];     // the wrestlers as receivers of their own map (the game's skin / gear shaders): x = strength (c168.w 0.7), y = N.L fade (c169.y 0.2)
};

// The light matrices and parameters of the shadow set (see shadow.h for the passes). Map sizes: props 512^2, wrestler 1024^2,
// plane 512^2 -> 256^2, rim 512x256.
class ShadowSetup {
 public:
  // From the wrestlers' world boxes: the light matrices and parameters. `props` = any ring piece casts.
  // `cam_fwd` = the main camera's forward direction (viewer space): the wrestlers' light takes its azimuth (Session 98 wrestler fix 2).
  void Prepare(const std::vector<ShadowBox>& wrestlers, bool props, bool wrestler_map, bool plane, bool blobs, const float cam_fwd[3]);
  // Session 98 (wrestler fix 2): the game's rim light map (yRimLightMap) range.
  void SetRim(bool on, float near_w, float far_w, int w, int h);
  const ShadowCB& Cb() const { return cb_; }
  bool PropsActive() const { return cb_.props_p[0] > 0.5f; }
  bool WrestlerMapActive() const { return cb_.wr_p[3] > 0.5f; }
  bool PlaneActive() const { return cb_.plane_r[3] > 0.5f; }
  bool RimActive() const { return cb_.rim_p[3] > 0.5f; }
  const Mat4& PropsVP() const { return props_vp_; }
  const Mat4& WrestlerVP() const { return wr_vp_; }

 private:
  ShadowCB cb_{};
  Mat4 props_vp_{}, wr_vp_{};
};

// ---- the post chain (Session 98 plan step 10; post.h) ----------------------------------------------------------------------
struct PostParams {
  bool fxaa = true, dof = true, dof_near = false, haze = true, gamma = true;  // dof_near: the game's near blur (right in front of the camera); off: the viewer's cameras get close
  bool entrance = false;       // the entrance combine: its DoF constants and the bloom / glare terms
  float bloom = 0.4f, glare = 0.f, bloom_threshold = 0.45f;  // (glare 1.6 is Brock's only; threshold 0.45 is a guess: the game's c16.x comes from auto-exposure, untraced)
  float q = 1, nq = 1;         // the scene projection's m[10] and -m[14]: view w = nq / (q - depth)
  float ent_far_w = 0;         // Session 98 (wrestler fix 3): entrance far-DoF start as a view depth (0 = the fixed constants below)
  float film[4] = {0, 0, 0, 0};  // Session 98 (arena fix 2): the entrant's film filter (entrance_light::Rig::film_rgba), entrance only; a = 0 off
};

struct PostCB {  // cbuffer Post (b0) of hlsl::kPost
  float c20[4], c21[4], haze_col[4], haze_p[4], size[4], proj[4], flags[4], bloom_p[4], pass[4], film[4];
};

// The chain's render targets (one set per size; quarter = w/4 x h/4, eighth = w/8 x h/8) and pixel shaders.
enum class PostTarget : uint8_t { kScene, kDepth, kNearA, kNearB, kFarA, kFarB, kBright, kBlA, kBlB, kGlare, kOut, kNone };
enum class PostShader : uint8_t { kDownNear, kDownFar, kBlur13, kBright, kBlur15, kGlare, kCombine, kCount };
const char* PostShaderEntry(PostShader s);  // the HLSL entry point name ("psDownNear", ...)
// One step of the chain: a full-screen pass of `ps` from `src[0..nsrc)` (bound at t0..) into `dst` with constants `cb`; or, when
// `mark` is set, only a GPU timer mark of that name.
struct PostStep {
  PostShader ps = PostShader::kCombine;
  PostTarget dst = PostTarget::kNone;
  PostTarget src[6] = {PostTarget::kNone, PostTarget::kNone, PostTarget::kNone, PostTarget::kNone, PostTarget::kNone, PostTarget::kNone};
  int nsrc = 0;
  PostCB cb{};
  const char* mark = nullptr;
};
// Every pass of the chain for a w x h scene, in order (the game's constants: docs\systems\post_effects.md).
std::vector<PostStep> BuildPostPlan(const PostParams& p, int w, int h);
inline int PostQuarter(int v) { return v / 4 > 1 ? v / 4 : 1; }
inline int PostEighth(int v) { return v / 8 > 1 ? v / 8 : 1; }

// ---- the GPU timer's JSON (both backends' Timing) ------------------------------------------------------------------------
std::string TimingJson(int w, int h, int counted, double total, const std::vector<std::pair<std::string, double>>& sum);

}  // namespace view
