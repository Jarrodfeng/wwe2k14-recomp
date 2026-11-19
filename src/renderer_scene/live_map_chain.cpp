// The filter chains behind the wrestlers' shadow map and rim map (Session 103, lane L7; live_maps.h, live_maps_chain.h, docs\renderer\LIVE_MAPS.md "Lane L7").
// Lane E / E2 make the RAW maps; the game's readers sample the maps AFTER these passes, which the game runs as yBlur full-target quads (brock_losswin f110 stream, ops 253-263 and 320-326):
//   shadow  op 260  yBlur_min_pair  (textures 0 and 1 = the two caster layers, taps at -2 / -1 / +1 / +2 texels in x, weights c16 = (0.07985, 0.24305, 0.24337, 0.08017), centre c17.x = 0.35355)
//           op 263  yBlur_min_rgba  (the same taps and weights in y, over the previous result)  -> RGBA = (blurred mean, min, blurred mean of the larger layer, its min), the layout ch_shadow_amount reads
//   rim     op 326  yBlur_t2        (13 taps, a diamond of spacing 2 texels whose centre sits at (-1, -1) texels, gaussian weights: rgb 0.1839 at the centre, alpha 0.3989)
// The game also runs two yBlur_flag_any reductions (512 -> 128 -> 32) whose result switches off the quads of the blur where the map is empty. They are NOT built here: where the map is empty the blur of
// an empty map is the empty map (white 1,1,1,1 for the shadow; (1,0,1,1) for the rim since S103 B1F), so a full-map blur gives the same picture, and the reductions only saved work (we own the renderer; Jarrod, S100).
// The map's second layer: the game runs two caster passes (images 214 and 216 at f110) and min_pair keeps the larger of the two in z / w. Our map has ONE caster pass (the map-A collapse rule is not
// built, LIVE_MAPS.md "What this first version does NOT do"), so both layers are our raw map: z / w equal x / y [U: what the second game layer holds].
// Nothing here reads the stream; the taps are the game's constants in texel units, scaled by the map's own size.
#include <algorithm>
#include <cstdlib>
#include <cstring>

#include <cmath>

#include "../renderer/output_switches.h"   // rdr::g_low_tier
#include "char_effects.h"
#include "front_end.h"
#include "live_maps.h"
#include "live_maps_chain.h"
#include "live_maps_internal.h"

namespace rdr_scene {
namespace md = maps_detail;

namespace {
constexpr uint16_t kRGBA32F = 109, kRG32F = 103;   // the post quad's vertex layout (post_pass.cpp)

// the game's constants (brock_losswin f110 op 260 / 263 / 326), in texel units
constexpr float kBlurTap[4] = {0.07985f, 0.24305f, 0.24337f, 0.08017f};   // c16 of min_pair / min_rgba (taps -2, -1, +1, +2)
constexpr float kBlurCentre = 0.35355f;                                     // c17.x
constexpr int kRimTaps[13][2] = {{-5, -1}, {-3, -3}, {-3, -1}, {-3, 1}, {-1, -5}, {-1, -3}, {-1, -1}, {-1, 1}, {-1, 3}, {1, -3}, {1, -1}, {1, 1}, {3, -1}};   // (dx, dy) texels, c0..c12.xy
constexpr float kRimRgb[13] = {0.024882f, 0.067638f, 0.111515f, 0.067638f, 0.024882f, 0.111515f, 0.183858f, 0.111515f, 0.024882f, 0.067638f, 0.111515f, 0.067638f, 0.024882f};   // c16..c28.xyz
constexpr float kRimAlpha[13] = {0.053991f, 0.146763f, 0.241971f, 0.146763f, 0.053991f, 0.241971f, 0.398942f, 0.241971f, 0.053991f, 0.146763f, 0.241971f, 0.146763f, 0.053991f};   // c16..c28.w
}  // namespace

// One full-target quad of an yBlur fragment variant: `target` (tw x th), reading `tex0` (and `tex1` when non-null) with nearest filtering and clamped edges, the registers `offsets` (reg 0) and `weights` (reg 16).
static void ChainQuad(rdr::FrameBuilder& fb, ChainState& s, rdr::ImageId target, uint32_t tw, uint32_t th, uint16_t program, int fs, rdr::ImageId tex0, rdr::ImageId tex1, const float* offsets,
                      uint32_t offset_vec4s, const float* weights, uint32_t weight_vec4s, const uint32_t geometry, const uint32_t vs_constants) {
  rdr::TargetsOp t;
  t.color[0].image = target;
  t.color_count = 1;
  fb.SetTargets(t);
  rdr::ClearOp clr;   // review 5: the quad covers the whole target, so the old contents are never loaded: a clear is the pass's load op
  clr.flags = rdr::kClearColor;
  clr.full = 1;
  fb.Clear(clr);
  const rdr::fx::Variant& fv = rdr::fx::kVariants[fs];
  std::vector<uint8_t> block(std::max<uint32_t>(fv.block_bytes, 16), 0);
  md::PutRegisters(block, fv, 0, offsets, offset_vec4s);
  md::PutRegisters(block, fv, 16, weights, weight_vec4s);
  rdr::SamplerDesc sd{};
  sd.mag_filter = sd.min_filter = 0;   // the taps are whole texels: nearest = linear, and the 16F maps need no filtering support
  sd.mip_mode = 0;
  sd.address_u = sd.address_v = sd.address_w = 2;   // clamp to edge
  sd.max_anisotropy = 1;
  sd.max_lod = 1000.f;
  const uint32_t sampler = fb.Sampler(sd);
  rdr::TextureSet set{};
  for (uint32_t k = 0; k < fv.sampler_count; ++k) {
    const int slot = fv.samplers[k].slot;
    const rdr::ImageId img = slot == 0 ? tex0 : (slot == 1 && tex1 != rdr::kNoImage ? tex1 : tex0);
    set.slots[slot].image = img;
    set.slots[slot].sampler = sampler;
    set.slots[slot].kind = fv.samplers[k].kind;
  }
  rdr::DrawState st{};
  st.program = program;
  st.vertex_layout = s.layout;
  st.topology = 3;
  st.front_face = 1;
  st.blend[0].write_mask = 0xF;
  rdr::DynamicState dyn{};
  dyn.viewport[2] = float(tw), dyn.viewport[3] = float(th), dyn.viewport[5] = 1.f;
  dyn.scissor.width = tw, dyn.scissor.height = th;
  dyn.system[0] = 1.f, dyn.system[1] = -1.f, dyn.system[2] = 1.f;
  dyn.system[9] = 7.f;
  rdr::DrawOp op{};
  op.state = fb.State(st);
  op.dynamic = fb.Dynamic(dyn);
  op.textures = fb.Textures(set);
  op.vs_constants = vs_constants;
  op.fs_constants = fb.PushConstants(block.data(), uint32_t(block.size()));
  op.vertex[0] = {rdr::kFrameGeometry, geometry};
  op.count = 6;
  op.instances = 1;
  fb.Draw(op, rdr::kGrpOther);   // S103 GSTP
}

// The depth range (clip w) of the sample points of the casters that are ON SCREEN (|x| <= w, |y| <= w, w > 0): the rim pass's RimLightDepth is the wrestlers' range, and a caster outside the picture
// (a referee, a manager, a wrestler out of the shot) must not stretch it (run 27 f500: the whole-box fit gave near 333, the game's 679.5). vp = the game-space row-vector view-projection (FitRimInput's).
bool LiveMapsHook::MeasureRimDepth(const SceneFrame& frame, const float vp[16], float& near_w, float& far_w, uint32_t& samples_out) {
  const float mirror = frame.viewer_space ? -1.f : 1.f;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  uint32_t n = 0;
  for (const Object& obj : frame.objects) {
    if (!(obj.flags & kObjVisible)) continue;
    const CasterModel* cm = CasterOf(obj.asset);
    if (!cm || cm->meshes.empty()) continue;
    assets::Matrix world;
    std::memcpy(world.data(), obj.world.m, sizeof(float) * 16);
    const Mat4* skin = nullptr;
    uint32_t skin_count = 0;
    if ((obj.flags & kObjSkinned) && obj.bones_count && size_t(obj.bones_first) + obj.bones_count <= frame.bones.size()) skin = &frame.bones[obj.bones_first], skin_count = obj.bones_count;
    for (const CasterMesh& m : cm->meshes) {
      if (m.src < 32 ? !((obj.mesh_mask_lo >> m.src) & 1u) : (m.src < 64 && !((obj.mesh_mask_hi >> (m.src - 32)) & 1u))) continue;
      if (m.chunk && rdr_scene::SkinChunkAbsent(m.nodes, skin, skin_count)) continue;   // S103 W2B (review C): a chunk that skins to an absent spring-hair piece (zero rows) is not part of the fit / mask
      std::vector<assets::Matrix>& slots = slots_scratch_;
      slots.clear();
      if (m.chunk)
        for (uint16_t node : m.nodes) {
          assets::Matrix sk = assets::Identity();
          if (skin && node < skin_count) std::memcpy(sk.data(), skin[node].m, sizeof(float) * 16);
          slots.push_back(assets::Mul(sk, world));
        }
      if (slots.empty()) slots.push_back(world);
      for (uint32_t i = 0; i < m.sample_count; ++i) {
        const SamplePt& sp = cm->samples[size_t(m.sample_first) + i];
        const assets::Matrix& mt = slots[std::min<size_t>(sp.slot, slots.size() - 1)];
        float q[3];
        for (int k = 0; k < 3; ++k) q[k] = sp.p[0] * mt[size_t(k)] + sp.p[1] * mt[size_t(4 + k)] + sp.p[2] * mt[size_t(8 + k)] + mt[size_t(12 + k)];
        q[1] *= mirror;
        float c[4];
        for (int k = 0; k < 4; ++k) c[k] = q[0] * vp[k] + q[1] * vp[4 + k] + q[2] * vp[8 + k] + vp[12 + k];
        if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[3]) || !(c[3] > 0.f) || std::fabs(c[0]) > c[3] || std::fabs(c[1]) > c[3]) continue;
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], q[k]), hi[k] = std::max(hi[k], q[k]);
        ++n;
      }
    }
  }
  samples_out = n;
  if (n == 0) return false;
  // the range = the depth extremes of the 8 corners of the on-screen casters' sample box (the whole-model box of before, restricted to what is in the picture): brock_losswin f110 (604, 766) beside the
  // game's (597.4, 776.7); a sphere about the box was wider than the game's at f110 and the samples alone narrower at f200 (857, 944 beside 823.5, 986.2)
  float nw = 1e30f, fw = -1e30f;
  for (int corner = 0; corner < 8; ++corner) {
    const float x = (corner & 1) ? hi[0] : lo[0], y = (corner & 2) ? hi[1] : lo[1], z = (corner & 4) ? hi[2] : lo[2];
    const float w = x * vp[3] + y * vp[7] + z * vp[11] + vp[15];
    nw = std::min(nw, w), fw = std::max(fw, w);
  }
  if (!std::isfinite(nw) || !std::isfinite(fw) || !(fw > nw + 1.f)) return false;
  near_w = nw, far_w = fw;
  return true;
}

void LiveMapsHook::ChainMaps(rdr::FrameBuilder& fb) {
  ChainState& s = *chain_;
  s.shadow_passes = s.rim_passes = 0;
  s.skip = "";
  // S103 L7 (review 5): the LOW tier (rdr::g_low_tier) runs NO chain: three full-target passes plus their clears are real cost on the weak GPUs LOW is for, and what they buy is soft shadow
  // edges and a rim highlight, both fine detail. Under LOW the shadow reader keeps the raw map (hard edges, shadow_raw_bound counts it) and the rim slot keeps the black stand-in (no rim: the old look).
  if (rdr::g_low_tier.load(std::memory_order_relaxed)) {
    s.skip = "LOW tier: no chains";
    return;
  }
  // One chain per frame graph: a second Record into the same builder after the chain (a mirror hook) publishes the first one's outputs instead of drawing them again.
  const bool again = (&fb == s.last_fb && fb.DrawCount() >= s.last_draws && s.last_draws != 0);
  s.last_fb = nullptr;
  static const bool off = std::getenv("WWE_RDR_NO_MAP_CHAIN") != nullptr;   // dev plant: the raw maps stay raw (the twin of the old behaviour)
  if (off) {
    s.skip = "WWE_RDR_NO_MAP_CHAIN";
    return;
  }
  MapImage* shadow = nullptr;
  MapImage* rim = nullptr;
  for (MapImage& m : maps_.maps) {
    if (m.role == MapRole::ShadowMap && m.raw && m.image != rdr::kNoImage) shadow = &m;
    if (m.role == MapRole::RimMap && m.raw && m.image != rdr::kNoImage) rim = &m;
  }
  if (!shadow && !rim) return;
  if (again) {   // the same graph already holds this frame's chain: publish its outputs
    if (shadow && s.sh_out != rdr::kNoImage && s.sh_mid != rdr::kNoImage && s.sh_w == uint32_t(shadow->params[0]) && s.sh_h == uint32_t(shadow->params[1])) shadow->image = s.sh_out, shadow->raw = false;
    if (rim && s.rim_out != rdr::kNoImage && s.rim_w == uint32_t(rim->params[0]) && s.rim_h == uint32_t(rim->params[1])) rim->image = s.rim_out, rim->raw = false;
    s.last_fb = &fb, s.last_draws = fb.DrawCount();
    s.skip = "second Record into this graph: the first chain stands";
    return;
  }
  if (!s.ready) {
    s.vs_quad = md::FindVariant("yAfterImage_quad_vertex");
    s.fs_pair = md::FindVariant("yBlur_min_pair_fragment");
    s.fs_rgba = md::FindVariant("yBlur_min_rgba_fragment");
    s.fs_t2 = md::FindVariant("yBlur_t2_fragment");
    if (s.vs_quad < 0 || s.fs_pair < 0 || s.fs_rgba < 0 || s.fs_t2 < 0) {
      s.skip = "the yBlur variants are not in the registry";
      return;
    }
    const auto make = [&](int fs, uint16_t& program) {
      rdr::ProgramDesc d;
      d.name = rdr::fx::kVariants[fs].name;
      d.vertex_spirv = rdr::fx::kVariants[s.vs_quad].spirv, d.vertex_bytes = rdr::fx::kVariants[s.vs_quad].bytes;
      d.fragment_spirv = rdr::fx::kVariants[fs].spirv, d.fragment_bytes = rdr::fx::kVariants[fs].bytes;
      return renderer_.AddProgramChecked(d, program);
    };
    if (!make(s.fs_pair, s.prog_pair) || !make(s.fs_rgba, s.prog_rgba) || !make(s.fs_t2, s.prog_t2)) {
      s.skip = "the renderer refused a chain program";
      return;
    }
    rdr::VertexLayoutDesc d{};
    d.strides[0] = 24;
    d.attributes[0] = rdr::VertexAttribute{0, 0, kRGBA32F, 0};
    d.attributes[1] = rdr::VertexAttribute{1, 0, kRG32F, 16};
    d.attribute_count = 2;
    s.layout = renderer_.AddVertexLayout(d);
    s.ready = true;
  }
  // the quad: NDC, y up (top left = texture (0, 0); the draw's system.y = -1 turns it into Vulkan's), as post_pass.cpp
  const float verts[6][6] = {{-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {1, -1, 0, 1, 1, 1}, {-1, 1, 0, 1, 0, 0}, {1, -1, 0, 1, 1, 1}, {-1, -1, 0, 1, 0, 1}};
  const uint32_t geometry = fb.PushGeometry(verts, sizeof verts, 4);
  const float zeros[4] = {};
  const uint32_t vs_constants = fb.PushConstants(zeros, 16);

  const auto make_image = [&](rdr::ImageId& img, uint32_t& cw, uint32_t& ch, uint32_t w, uint32_t h, rdr::Format fmt, bool second, rdr::ImageId* mid) {
    if (img != rdr::kNoImage && cw == w && ch == h && (!second || (mid && *mid != rdr::kNoImage))) return true;   // review 2: a half-built pair is built again, never reported good
    Retire(img);
    if (mid) Retire(*mid);
    rdr::ImageDesc c;
    c.width = uint16_t(w), c.height = uint16_t(h);
    c.format = fmt;
    c.usage = rdr::kUsageTarget | rdr::kUsageSampled;
    img = renderer_.CreateImage(c);
    if (second && mid) *mid = renderer_.CreateImage(c);
#ifdef RDR_DEV
    static const bool fail_mid = std::getenv("WWE_RDR_CHAIN_FAIL_MID") != nullptr;   // dev plant (l7_check --plant fail_mid): the intermediate image "could not be created"
    if (fail_mid && second && mid) Retire(*mid), *mid = rdr::kNoImage;
#endif
    cw = w, ch = h;
    if (img != rdr::kNoImage) renderer_.SetImageClass(img, rdr::kGpuShadow);
    if (second && mid && *mid != rdr::kNoImage) renderer_.SetImageClass(*mid, rdr::kGpuShadow);
    return img != rdr::kNoImage && (!second || *mid != rdr::kNoImage);
  };

  if (shadow) {
    const uint32_t w = uint32_t(shadow->params[0]), h = uint32_t(shadow->params[1]);
    if (w >= 8 && h >= 8 && make_image(s.sh_out, s.sh_w, s.sh_h, w, h, rdr::Format::RGBA16F, true, &s.sh_mid)) {
      const float tx = 1.f / float(w), ty = 1.f / float(h);
      float w0[8] = {kBlurTap[0], kBlurTap[1], kBlurTap[2], kBlurTap[3], kBlurCentre, 0.f, 0.f, 0.f};
      // x pass: both layers = our raw map; offsets (-2tx, 0, -tx, 0), (tx, 0, 2tx, 0) for texture 0, the same again for texture 1
      const float ox[16] = {-2 * tx, 0, -tx, 0, tx, 0, 2 * tx, 0, -2 * tx, 0, -tx, 0, tx, 0, 2 * tx, 0};
      ChainQuad(fb, s, s.sh_mid, w, h, s.prog_pair, s.fs_pair, shadow->image, shadow->image, ox, 4, w0, 2, geometry, vs_constants);
      const float oy[8] = {0, -2 * ty, 0, -ty, 0, ty, 0, 2 * ty};
      ChainQuad(fb, s, s.sh_out, w, h, s.prog_rgba, s.fs_rgba, s.sh_mid, rdr::kNoImage, oy, 2, w0, 2, geometry, vs_constants);
      shadow->image = s.sh_out;
      shadow->raw = false;   // the game's min-pair + blur chain is behind this map now (the map-A second layer aside: header)
      ++s.shadow_made;
      s.shadow_passes = 2;
    } else {
      s.skip = "could not create the shadow chain images";
    }
  }
  if (rim) {
    const uint32_t w = uint32_t(rim->params[0]), h = uint32_t(rim->params[1]);
    if (w >= 8 && h >= 8 && make_image(s.rim_out, s.rim_w, s.rim_h, w, h, rdr::Format::RGBA8, false, nullptr)) {
      const float tx = 1.f / float(w), ty = 1.f / float(h);
      float off[13 * 4] = {}, wt[13 * 4] = {};
      for (int k = 0; k < 13; ++k) {
        off[4 * k] = float(kRimTaps[k][0]) * tx, off[4 * k + 1] = float(kRimTaps[k][1]) * ty;
        wt[4 * k] = wt[4 * k + 1] = wt[4 * k + 2] = kRimRgb[k], wt[4 * k + 3] = kRimAlpha[k];
      }
      ChainQuad(fb, s, s.rim_out, w, h, s.prog_t2, s.fs_t2, rim->image, rdr::kNoImage, off, 13, wt, 13, geometry, vs_constants);
      rim->image = s.rim_out;
      rim->raw = false;   // the game's 13-tap blur is behind this map now
      ++s.rim_made;
      s.rim_passes = 1;
    } else {
      s.skip = "could not create the rim chain image";
    }
  }
  s.last_fb = &fb, s.last_draws = fb.DrawCount();
}

}  // namespace rdr_scene
