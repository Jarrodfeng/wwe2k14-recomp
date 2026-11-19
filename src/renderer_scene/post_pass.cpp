// Session 101, lane 34: the post pass. See post_pass.h and docs\renderer\POST_PASS.md.
#include "post_pass.h"
#include "../keep_stream.h"  // S103 GT: WWE2K14_STREAM()

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../renderer/combine_lite.h"  // S102 R18-B2
#include "../renderer/gpu_small.h"    // S102 R24-E
#include "../renderer/shaders/spv/fx_registry.h"
#include "../renderer/shaders/spv/rdr_fullscreen_vert.h"
#include "../renderer/shaders/spv/rdr_post_depth_frag.h"
#include "../renderer/shaders/spv/rdr_post_ghost_frag.h"   // S103 FXF

namespace rdr_scene {
namespace {

using rdr::fx::kVariants;
constexpr int kVariantCount = int(sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]));

struct Regs {  // a pixel stage's constant registers, as the game's bank is laid out (the registry places ranges by register)
  float v[256][4];
  Regs() { std::memset(v, 0, sizeof v); }
  void Set(int reg, const float* four) { std::memcpy(v[reg], four, 16); }
  void SetN(int reg, const float (*many)[4], int count) {
    for (int i = 0; i < count; ++i) std::memcpy(v[reg + i], many[i], 16);
  }
};

constexpr uint16_t kRGBA32F = 109, kRGB32F = 106, kRG32F = 103;
constexpr uint64_t kLumGap = 30;     // S103 PST: guest frames one adapt step may compose (follow = 1 - (1 - 0.0396)^k); a frame number that goes BACK by more than this starts the adaptation again (a new scene)
constexpr float kLumElapsed = 1.f / 15.f;   // the game's ElapsedTime (c7, 0x8200B0EC): per GUEST frame, not per real second
constexpr float kMiddleGrayDefault = 0.18f;
constexpr uint64_t kGhostGap = 30;   // S103 FXF: a ghost draw more than this many guest frames after the last one starts a new effect (the first frame only fills the history)
constexpr int VD_REGISTER = 40;  // S103 SLV2: the `_vd` twins' VDepthK = (q, nq, 9.92): the register tools\renderer\compile_shaders.py (VD_REGISTER) gives it, one no game shader of these effects reads

}  // namespace

PostPass::~PostPass() {
  for (rdr::ImageId i : images_) renderer_.DestroyImage(i);
  for (rdr::ImageId i : lum_) if (i != rdr::kNoImage) renderer_.DestroyImage(i);   // S103 PST
  for (rdr::ImageId i : lum_a_) if (i != rdr::kNoImage) renderer_.DestroyImage(i);
  if (lum_cur_ != rdr::kNoImage) renderer_.DestroyImage(lum_cur_);
  for (rdr::ImageId i : noise_) if (i != rdr::kNoImage) renderer_.DestroyImage(i);   // S103 FXH
}

// S103 FXH: the retro film's three pictures, OUR OWN noise (post_cue_fx.h): strips 1 x 256 (slots 1 / 2, point, clamp) and a 256 x 256 grain (slot 3, linear, wrap). Made once, kept for the pass's life; only the ids still
// missing are made, so a partial failure retries the rest on the next frame.
bool PostPass::MakeNoise() {
  const auto mk = [&](rdr::ImageId& id, uint32_t w, uint32_t h, const std::vector<uint8_t>& px) {
    if (id != rdr::kNoImage) return;
    rdr::ImageDesc d;
    d.width = uint16_t(w), d.height = uint16_t(h);
    d.format = rdr::Format::RGBA8;
    d.usage = rdr::kUsageSampled;
    id = renderer_.CreateImage(d);
    if (id != rdr::kNoImage) renderer_.UpdateImage(id, 0, 0, px.data(), px.size());
  };
  mk(noise_[0], 1, cue_fx::kStripH, cue_fx::MakeStrip(cue_fx::kSeedStrip1));
  mk(noise_[1], 1, cue_fx::kStripH, cue_fx::MakeStrip(cue_fx::kSeedStrip2));
  mk(noise_[2], cue_fx::kGrainW, cue_fx::kGrainH, cue_fx::MakeGrain(cue_fx::kSeedGrain));
  return noise_[0] != rdr::kNoImage && noise_[1] != rdr::kNoImage && noise_[2] != rdr::kNoImage;
}

// S103 PST: a one-channel float image (the luminance chain's pyramid: the log-luma values are negative and not in 0..1, so RGBA8 cannot hold them). in_images = kept in images_ (destroyed on a size change);
// the luminance images are not (the adaptation must survive a render-size change).
rdr::ImageId PostPass::MakeF(uint32_t w, uint32_t h, bool in_images) {
  rdr::ImageDesc d;
  d.width = uint16_t(w);
  d.height = uint16_t(h);
  d.format = rdr::Format::R32F;
  d.usage = rdr::kUsageTarget | rdr::kUsageSampled;
  const rdr::ImageId id = renderer_.CreateImage(d);
  if (id != rdr::kNoImage && in_images) images_.push_back(id);
#if !WWE2K14_STREAM()
  if (id != rdr::kNoImage) renderer_.SetImageClass(id, rdr::kGpuPost);
#endif
  return id;
}

bool PostPass::MakeLum() {
  if (lum_made_) return true;
  // only the ids still missing are made (a partial failure keeps what was made; the next frame retries the rest: nothing leaks)
  const auto mk = [&](rdr::ImageId& id, uint32_t w, uint32_t h) { if (id == rdr::kNoImage) id = MakeF(w, h, false); };
  mk(lum_[0], 64, 64), mk(lum_[1], 16, 16), mk(lum_[2], 4, 4), mk(lum_cur_, 1, 1), mk(lum_a_[0], 1, 1), mk(lum_a_[1], 1, 1);
  if (lum_[0] == rdr::kNoImage || lum_[1] == rdr::kNoImage || lum_[2] == rdr::kNoImage || lum_cur_ == rdr::kNoImage || lum_a_[0] == rdr::kNoImage || lum_a_[1] == rdr::kNoImage) return false;
  lum_made_ = true;
  lum_just_made_ = true;   // Record clears the 1x1 images once (an image's first contents are undefined: a NaN there would survive the step's mix)
  return true;
}

void PostPass::Reload(const std::vector<int>& changed) {
  for (auto it = programs_.begin(); it != programs_.end();) {
    const int vs = int(it->first >> 32), ps = int(it->first & 0xFFFFFFFFu);
    if (std::find(changed.begin(), changed.end(), vs) != changed.end() || std::find(changed.begin(), changed.end(), ps) != changed.end()) it = programs_.erase(it);
    else ++it;
  }
}

int PostPass::Find(const char* name) {
  for (int i = 0; i < kVariantCount; ++i)
    if (kVariants[i].name && std::strcmp(kVariants[i].name, name) == 0) return i;
  return -1;
}

uint16_t PostPass::Program(int vs, int ps) {
  const uint64_t key = uint64_t(uint32_t(vs)) << 32 | uint32_t(ps);
  const auto found = programs_.find(key);
  if (found != programs_.end()) return found->second;
  names_.push_back(std::string("post: ") + kVariants[vs].name + " + " + kVariants[ps].name);
  rdr::ProgramDesc desc;
  desc.name = names_.back().c_str();
  desc.vertex_spirv = kVariants[vs].spirv;
  desc.vertex_bytes = kVariants[vs].bytes;
  desc.fragment_spirv = kVariants[ps].spirv;
  desc.fragment_bytes = kVariants[ps].bytes;
  const uint16_t id = renderer_.AddProgram(desc);
  programs_.emplace(key, id);
  return id;
}

// All the post quads are 6 vertices of 24 bytes: position (x, y, 0, 1) and texture coordinate; the yGlareEtc vertex stage reads a vec3 position.
uint16_t PostPass::Layout(int vs) {
  const auto found = layouts_.find(vs);
  if (found != layouts_.end()) return found->second;
  const bool vec3 = std::strncmp(kVariants[vs].name, "yGlareEtc", 9) == 0;
  rdr::VertexLayoutDesc d{};
  d.strides[0] = 24;
  d.attributes[0] = rdr::VertexAttribute{0, 0, vec3 ? kRGB32F : kRGBA32F, 0};
  d.attributes[1] = rdr::VertexAttribute{1, 0, kRG32F, 16};
  d.attribute_count = 2;
  const uint16_t id = renderer_.AddVertexLayout(d);
  layouts_.emplace(vs, id);
  return id;
}

rdr::ImageId PostPass::Make(uint32_t w, uint32_t h) {
  rdr::ImageDesc d;
  d.width = uint16_t(w);
  d.height = uint16_t(h);
  d.format = rdr::Format::RGBA8;
  d.usage = rdr::kUsageTarget | rdr::kUsageSampled | (scaled_ ? rdr::kUsageScaled : 0);  // S101 lane PO: scaled
  const rdr::ImageId id = renderer_.CreateImage(d);
  if (id != rdr::kNoImage) images_.push_back(id);
#if !WWE2K14_STREAM()
  if (id != rdr::kNoImage) renderer_.SetImageClass(id, rdr::kGpuPost);   // S103 GT: the post chain's images are the post role (stream-free build only: see front_end.cpp)
#endif
  return id;
}

bool PostPass::Ensure(uint32_t w, uint32_t h, bool scaled, std::string& error) {
  if (size_w_ == w && size_h_ == h && scaled_ == scaled && q1_ != rdr::kNoImage) return true;
  scaled_ = scaled;  // S101 lane PO
  mid_ = rdr::kNoImage;
  // S101 lane PO: no WaitIdle (DestroyImage already defers past the frames in flight): a chain whose size alternates must not stall every frame.
  for (rdr::ImageId i : images_) renderer_.DestroyImage(i);
  images_.clear();
  const uint32_t qw = std::max(1u, w / 4), qh = std::max(1u, h / 4), ew = std::max(1u, w / 8), eh = std::max(1u, h / 8);
  q1_ = Make(qw, qh), q2_ = Make(qw, qh);
  b0_ = Make(ew, eh), b1_ = Make(ew, eh), b2_ = Make(ew, eh), gt_ = Make(ew, eh);
  for (auto& g : g_) g = Make(ew, eh);
  gsum_ = Make(ew, eh);
  mid_ = rdr::kNoImage;  // (made when a film filter needs it)
  aa_img_ = lq_ = rdr::kNoImage;   // S103 PST (made when the antialiasing / the luminance measure first need them)
  cue_[0] = cue_[1] = rdr::kNoImage;   // S103 FXH (made when a cue effect first runs)
  pre_ = hist_[0] = hist_[1] = rdr::kNoImage, ghost_armed_ = false;   // S103 FXF: (made when the ghost first runs; a new size starts a new effect)
  {
    rdr::ImageDesc d;  // the game-depth image of the viewer's scenes (R32F: the depth differences are ~0.005)
    d.width = uint16_t(w), d.height = uint16_t(h);
    d.format = rdr::Format::R32F;
    d.usage = rdr::kUsageTarget | rdr::kUsageSampled;
    dconv_ = renderer_.CreateImage(d);
    if (dconv_ != rdr::kNoImage) images_.push_back(dconv_);
#if !WWE2K14_STREAM()
    if (dconv_ != rdr::kNoImage) renderer_.SetImageClass(dconv_, rdr::kGpuPost);   // S103 GT
#endif
  }
  size_w_ = w, size_h_ = h;
  if (q1_ == rdr::kNoImage || gsum_ == rdr::kNoImage) { error = "post pass: could not create the pass images"; return false; }
  return true;
}

bool PostPass::Record(rdr::FrameBuilder& b, rdr::ImageId colour, rdr::ImageId depth, rdr::ImageId out, const PostParams& p_in, std::string& error) {
  PostParams forced_copy;   // S102 R24-E (tool only): rplay arm kind 7 forces DOFParam2.x so a capture can cross x = 0
  const PostParams* use = &p_in;
  if (rdr::g_gpu_small_force.load(std::memory_order_relaxed)) {
    forced_copy = p_in;
    forced_copy.DOFParam2[0] = rdr::CombineLiteFloat(rdr::g_gpu_small_force_x.load(std::memory_order_relaxed));
    use = &forced_copy;
  }
  // S103 PST (FXF review 8 rule): an image the antialiasing / the luminance chain needs that cannot be made drops THAT effect for this frame (counted, retried next frame); the frame itself never fails.
  PostParams drop_copy;
  if (use->tonemap || use->night || use->aa) {
    if (colour == rdr::kNoImage || out == rdr::kNoImage) { error = "post pass: a missing input image"; return false; }
    if (!Ensure(use->width, use->height, use->scaled_images, error)) return false;
    bool drop_l = false, drop_a = false;
    if (use->tonemap || use->night) {
      drop_l = !MakeLum();
      // the measure reads the DoF quarter downsample (q1_, [B948]+12); a frame that does not record it makes its own quarter picture (lq_)
      if (!drop_l && (use->t2_blur || !(use->dof || use->dof_chain)) && lq_ == rdr::kNoImage) {
        lq_ = Make(std::max(1u, use->width / 4), std::max(1u, use->height / 4));
        drop_l = lq_ == rdr::kNoImage;
      }
    }
    if (use->aa && aa_img_ == rdr::kNoImage) {
      aa_img_ = Make(use->width, use->height);
      drop_a = aa_img_ == rdr::kNoImage;
    }
    if (drop_l || drop_a) {
      drop_copy = *use;
      if (drop_l) drop_copy.tonemap = drop_copy.night = false, g_post_lum_dropped.fetch_add(1, std::memory_order_relaxed);
      if (drop_a) drop_copy.aa = false, g_post_aa_dropped.fetch_add(1, std::memory_order_relaxed);
      use = &drop_copy;
    }
  }
  // S103 FXH: the cue effects need two full-size images (the ping-pong between them) and the retro film its noise pictures. A missing image drops THAT effect for this frame (counted, retried next frame).
  PostParams cue_copy;
  if (use->defocus || use->emboss || use->retro) {
    if (colour == rdr::kNoImage || out == rdr::kNoImage) { error = "post pass: a missing input image"; return false; }
    if (!Ensure(use->width, use->height, use->scaled_images, error)) return false;
    for (rdr::ImageId& ci : cue_)
      if (ci == rdr::kNoImage) ci = Make(use->width, use->height);
    const bool images_ok = cue_[0] != rdr::kNoImage && cue_[1] != rdr::kNoImage;
    const bool noise_ok = !use->retro || (images_ok && MakeNoise());
    if (!images_ok || !noise_ok) {
      cue_copy = *use;
      if (!images_ok) cue_copy.defocus = cue_copy.emboss = cue_copy.retro = false;
      else cue_copy.retro = false;
      g_post_cue_dropped.fetch_add(1, std::memory_order_relaxed);
      use = &cue_copy;
    }
  }
  const PostParams& p = *use;
  passes_ = 0;
  combined_ = false;  // S101 lane PO
  if (!(p.tonemap || p.night)) lum_paused_ = true;   // S103 PST: the game steps the adapted luminance only while S+180 | S+184 is set: the next step after a pause is ONE guest frame
  if (colour == rdr::kNoImage || out == rdr::kNoImage || ((p.dof || p.fog || p.dof_chain) && depth == rdr::kNoImage)) { error = "post pass: a missing input image"; return false; }  // (yGlareEtc_fog reads the depth too)
  if (!Ensure(p.width, p.height, p.scaled_images, error)) return false;  // S101 lane PO: + scaled_images
  const bool bloom = p.bloom || p.glare;
  // S101 lane PS: the dead work. The chain's parts that the combine multiplies by zero are not recorded; the images it still reads are cleared.
  const bool run_blur = bloom && !p.dead_bloom, run_glare = p.glare && !p.dead_glare, run_bright = run_blur || run_glare;
  const bool variant_bloom = bloom || p.bloom_variant;  // the combine has the bloom permutation
  // S102 R18-B2 (level 1 of wwe2k14_rdr_combine_lite, a REMOVAL): when neither the bloom blur nor the glare is recorded, the combine's bloom and star images are black (cleared just below), so the bloom
  // permutation adds 0 * scale: the permutation WITHOUT bloom is picked instead, and the two clears are not made (2 fetches a pixel and 2 passes less). Exact: x + 0 * c is x for a finite scale.
  const bool lite_on = rdr::g_combine_lite.load(std::memory_order_relaxed) >= 1;
  const bool drop_bloom = lite_on && variant_bloom && !run_blur && !run_glare;
  // S102 R24-E (wwe2k14_rdr_gpu_small bit 1, gpu_small.h): the quarter-size DoF chain nothing reads is not recorded. The combine reads q2_ only in slot 2 (the near blur: only where DOFParam2.x > 1 - depth)
  // and in slot 6 when no earlier layer made the far blur; the bright pass (bloom / glare) reads it too. A removal: slot 2 is bound to the scene colour (a valid image the shader never samples then).
  const bool gs_has_chain = p.dof;
  const rdr::GpuSmallWhy gs_why = rdr::GpuSmallDecide(rdr::g_gpu_small.load(std::memory_order_relaxed), p.DOFParam2[0], p.far_blur != rdr::kNoImage, run_bright || p.dof_chain || p.tonemap || p.night /* S103 PST: the luminance measure reads q1_ */ || p.defocus /* S103 FXH: the defocus mixes the blur (q2_) */, p.depth_from_viewer, gs_has_chain, p.depth_clamped);
  const bool skip_near = gs_why == rdr::kGsTaken;
  if (gs_why != rdr::kGsOff) rdr::GpuSmallNote(gs_why, p.DOFParam2[0], skip_near ? (p.t2_blur ? 1u : 2u) : 0u);   // the census: relaxed counters, once per record
  if (p.film && mid_ == rdr::kNoImage) mid_ = Make(p.width, p.height);
  if (p.film && mid_ == rdr::kNoImage) { error = "post pass: could not create the film image"; return false; }
  // S103 FXF: the ghost needs this frame's picture as a texture (pre_) and the last output (hist_): the chain writes pre_ instead of `out`, the ghost pass writes `out`, then out is copied into hist_. Zero cost while it is off.
  // (review 8: each image is made on its own, retried the next frame; a failure turns the ghost off for this frame, it never errors the frame.)
  bool ghost = p.ghost;
  if (ghost) {
    if (pre_ == rdr::kNoImage) pre_ = Make(p.width, p.height);
    for (rdr::ImageId& hi : hist_)
      if (hi == rdr::kNoImage) hi = Make(p.width, p.height);
    if (pre_ == rdr::kNoImage || hist_[0] == rdr::kNoImage || hist_[1] == rdr::kNoImage) ghost = false;
  }
  if (!ghost) ghost_armed_ = false;   // the effect is off (or could not be made): the next one starts fresh
  // S103 FXH: with a cue effect on, the chain (and the ghost) write cue_[0]; the cue passes then ping-pong cue_[0] / cue_[1] and the last one writes `out` (the game's order: 7 defocus, 8 emboss, 9 retro film, after the afterimage).
  const bool cue_on = p.defocus || p.emboss || p.retro;
  const rdr::ImageId final_out = cue_on ? cue_[0] : out;
  const rdr::ImageId chain_out = ghost ? pre_ : final_out;

  const int vs_quad = Find("yAfterImage_quad_vertex"), vs_glare = Find("yGlareEtc_corner_uv_vertex");
  if (vs_quad < 0 || vs_glare < 0) { error = "post pass: vertex variants missing from the registry"; return false; }
  // S103 SLV2: the depth-convert fold. A frame whose depth is the viewer's own (depth_from_viewer) normally gets one full-size pass (dconv_: viewer depth -> game depth) before its two consumers (the DoF downsample's
  // slot 1, the combine's slot 3). The `_vd` twins of those two shaders convert where they fetch (FX_VDEPTH: gameDepth, the same two divisions and two max as rdr_post_depth.frag), so the pass is not recorded.
  // Exact only where a fetch then convert is the same float as convert then fetch: every depth sample NEAREST. The combine's slot 3 always is; the downsample's slot 1 is where !dof_depth_linear. A frame whose DoF
  // reads the depth linearly (the match) keeps the pass: filtering the converted values is not converting the filtered ones. The depth must also be the pass's own size (dconv_ is width x height and the conversion
  // reads texel (x, y) of it; the twins sample at the same uv). The depth's format needs no "point where R32F cannot filter" fallback: no fold frame filters it (nearest everywhere).
  const bool needs_dconv = p.depth_from_viewer && (p.dof || p.fog || p.dof_chain);
  const bool linear_dof = p.dof_depth_linear && (p.dof || p.dof_chain);
  const bool fold = needs_dconv && g_post_depth_fold.load(std::memory_order_relaxed) != 0 && !linear_dof && p.depth_w == p.width && p.depth_h == p.height && depth != rdr::kNoImage;
  const auto vd_name = [](const std::string& base /* "..._fragment" */) { return base.substr(0, base.size() - 9) + "_vd_fragment"; };
  const std::string combine_base = CombineVariantName(p, drop_bloom);
  const bool fold_combine = fold && (p.dof || p.fog), fold_down = fold && (p.dof || p.dof_chain) && !p.t2_blur;
  if (fold && ((fold_combine && Find(vd_name(combine_base).c_str()) < 0) || (fold_down && Find("yDownScale_dof_vd_fragment") < 0))) { error = "post pass: the _vd (depth fold) variants are missing from the registry"; return false; }
  float vdk[4] = {p.proj_q + (g_post_depth_fold_plant.load(std::memory_order_relaxed) == 1 ? 0.25f : 0.f), p.proj_nq, 9.92f, 0.f};   // (the plant: a wrong q, the picture must differ)
  const auto need = [&](const char* name, int& id) {
    id = Find(name);
    if (id < 0) error = std::string("post pass: fragment variant missing from the registry: ") + name;
    return id >= 0;
  };

  // The two quads: NDC, y up (top left = (-1, +1) = texture (0, 0); the draw's system.y = -1 turns it into Vulkan's).
  const float verts[6][6] = {{-1, 1, 0, 1, 0, 0}, {1, 1, 0, 1, 1, 0}, {1, -1, 0, 1, 1, 1}, {-1, 1, 0, 1, 0, 0}, {1, -1, 0, 1, 1, 1}, {-1, -1, 0, 1, 0, 1}};
  const uint32_t geometry = b.PushGeometry(verts, sizeof verts, 4);
  const float zeros[4] = {};
  const uint32_t vs_constants = b.PushConstants(zeros, 16);

  const auto sampler = [&](bool linear, bool repeat) {
    rdr::SamplerDesc s{};
    s.mag_filter = s.min_filter = linear ? 1 : 0;
    s.mip_mode = 0;
    s.address_u = s.address_v = s.address_w = repeat ? 0 : 2;  // clamp to edge (S101 lane PO: repeat for the planted dof_depth_address)
    s.max_anisotropy = 1;
    s.max_lod = 1000.f;
    return b.Sampler(s);
  };
  struct Tex { int slot; rdr::ImageId image; bool linear; bool repeat = false; };

  // One full-target quad: targets `target` (size tw x th), program vs + ps, the registers, the textures (only the slots the pixel variant declares).
  const auto pass = [&](rdr::ImageId target, uint32_t tw, uint32_t th, int vs, int ps, const Regs& regs, std::initializer_list<Tex> texs) {
    rdr::TargetsOp t;
    t.color[0].image = target;
    t.color_count = 1;
    b.SetTargets(t);
    const rdr::fx::Variant& fv = kVariants[ps];
    std::vector<uint8_t> block(std::max<uint32_t>(fv.block_bytes, 16), 0);
    for (uint32_t k = 0; k < fv.constant_count; ++k) {
      const rdr::fx::ConstantRange& r = fv.constants[k];
      if (uint32_t(r.offset) + uint32_t(r.count) * 16u > block.size() || r.reg + r.count > 256) continue;
      std::memcpy(block.data() + r.offset, regs.v[r.reg], size_t(r.count) * 16);
    }
    rdr::TextureSet set{};
    for (uint32_t s = 0; s < fv.sampler_count; ++s) {
      const int slot = fv.samplers[s].slot;
      for (const Tex& tx : texs)
        if (tx.slot == slot) {
          set.slots[slot].image = tx.image;
          set.slots[slot].sampler = sampler(tx.linear, tx.repeat);
          set.slots[slot].kind = fv.samplers[s].kind;
        }
    }
    rdr::DrawState st{};
    st.program = Program(vs, ps);
    st.vertex_layout = Layout(vs);
    st.topology = 3;
    st.front_face = 1;
    st.blend[0].write_mask = 0xF;
    rdr::DynamicState dyn{};
    dyn.viewport[2] = float(tw), dyn.viewport[3] = float(th), dyn.viewport[5] = 1.f;
    dyn.scissor.width = tw, dyn.scissor.height = th;
    dyn.system[0] = 1.f, dyn.system[1] = -1.f, dyn.system[2] = 1.f;
    dyn.system[9] = 7.f;
    rdr::DrawOp op{};
    op.state = b.State(st);
    op.dynamic = b.Dynamic(dyn);
    op.textures = b.Textures(set);
    op.vs_constants = vs_constants;
    op.fs_constants = b.PushConstants(block.data(), uint32_t(block.size()));
    op.vertex[0] = {rdr::kFrameGeometry, geometry};
    op.count = 6;
    op.instances = 1;
    b.Draw(op, rdr::kGrpPost);   // S103 GSTP
    ++passes_;
  };

  const uint32_t w = p.width, h = p.height, qw = std::max(1u, w / 4), qh = std::max(1u, h / 4), ew = std::max(1u, w / 8), eh = std::max(1u, h / 8);
  int ps = -1;

  if (p.aa) {   // S103 PST (COV9_story.md 2e): effect 0 runs FIRST: the edge-aware blur of the scene colour; every later pass (the DoF downsample, the luminance, the combine) reads its result, as the game's do (it resolves the frame
                // into the post copy and draws over it). Always pass 0 = `fixed` (threshold 0.64) [C: ctor sub_82C2BE68 sets [obj+44] = 0; effect_table T_Antialiasing p0 = yAntialiasing_fixed_fragment, p1 = uniform; S+284 has no writer].
                // (aa_img_ was made at the top of Record: a failure there dropped the effect.)
    const int vs_aa = Find("yAntialiasing_quad_vertex");
    int aaps = -1;
    if (vs_aa < 0 || !need("yAntialiasing_fixed_fragment", aaps)) { if (error.empty()) error = "post pass: the antialiasing variants are missing from the registry"; return false; }
    Regs r;
    const float c0[4] = {p.aa_offsets[0], p.aa_offsets[1], 0.f, 0.f};
    r.Set(0, c0);
    pass(aa_img_, w, h, vs_aa, aaps, r, {{0, colour, false}});
    colour = aa_img_;
  }

  if (fold) {
    g_post_dconv_skipped.fetch_add(1, std::memory_order_relaxed);   // S103 SLV2: the census: the dconv_ pass this frame did not record
  } else
  if (needs_dconv) {   // S103 P: + dof_chain (bloom with the DoF permutation off still reads the converted depth)  // (S101 lane 34 review A: the haze reads the depth as well) the viewer's depth -> the game's depth (one triangle, R32F)
    if (dconv_ == rdr::kNoImage) { error = "post pass: could not create the game-depth image"; return false; }
    if (!depth_program_made_) {
      rdr::ProgramDesc pd;
      pd.name = "post: viewer depth to game depth";
      pd.vertex_spirv = kSpv_rdr_fullscreen_vert, pd.vertex_bytes = sizeof(kSpv_rdr_fullscreen_vert);
      pd.fragment_spirv = kSpv_rdr_post_depth_frag, pd.fragment_bytes = sizeof(kSpv_rdr_post_depth_frag);
      depth_program_ = renderer_.AddProgram(pd);
      depth_layout_ = renderer_.AddVertexLayout(rdr::VertexLayoutDesc{});
      depth_program_made_ = true;
    }
    rdr::TargetsOp t;
    t.color[0].image = dconv_;
    t.color_count = 1;
    b.SetTargets(t);
    const float proj[4] = {p.proj_q, p.proj_nq, 9.92f, 0.f};
    rdr::TextureSet set{};
    set.slots[0].image = depth;
    set.slots[0].sampler = sampler(false, false);
    rdr::DrawState st{};
    st.program = depth_program_, st.vertex_layout = depth_layout_, st.topology = 3, st.front_face = 1;
    st.blend[0].write_mask = 0xF;
    rdr::DynamicState dyn{};
    dyn.viewport[2] = float(w), dyn.viewport[3] = float(h), dyn.viewport[5] = 1.f;
    dyn.scissor.width = w, dyn.scissor.height = h;
    dyn.system[0] = 1.f, dyn.system[1] = 1.f, dyn.system[2] = 1.f;  // (the triangle is generated in Vulkan's clip space: no flip)
    dyn.system[9] = 7.f;
    rdr::DrawOp op{};
    op.state = b.State(st);
    op.dynamic = b.Dynamic(dyn);
    op.textures = b.Textures(set);
    op.vs_constants = vs_constants;
    op.fs_constants = b.PushConstants(proj, 16);
    op.count = 3;
    op.instances = 1;
    b.Draw(op, rdr::kGrpPost);   // S103 GSTP
    ++passes_;
    g_post_dconv_passes.fetch_add(1, std::memory_order_relaxed);   // S103 SLV: the census
    depth = dconv_;
  }

  if (skip_near) {   // S102 R24-E: nothing reads the quarter-size chain (see above)
  } else
  if (p.t2_blur) {   // lane 140: the blur is one t2 pass over the stream's own quarter-size picture (no dof_down, no hole_fill)
    if (p.blur_source == rdr::kNoImage) { error = "post pass: the t2 blur has no source image"; return false; }
    if (!need("yBlur_t2_fragment", ps)) return false;
    Regs t2;
    t2.SetN(0, p.blur_t2_offsets, 13);   // lane 238: the blur's own tables (the bloom's t2 has others)
    t2.SetN(16, p.blur_t2_weights, 13);
    pass(q2_, qw, qh, vs_quad, ps, t2, {{0, p.blur_source, false}});
  } else
  if (p.dof || p.dof_chain) {  // S101 lane PS: dof_chain = the bloom's source though the combine has no dof permutation
    if (!need(fold_down ? "yDownScale_dof_vd_fragment" : "yDownScale_dof_fragment", ps)) return false;
    Regs r;
    r.SetN(0, p.dof_offsets, 4);
    r.Set(21, p.dof_param2_down);
    if (fold_down) r.Set(VD_REGISTER, vdk);   // S103 SLV2: (q, nq, 9.92) for gameDepth; the depth is the raw scene depth, sampled nearest (fold implies !dof_depth_linear here)
    pass(q1_, qw, qh, vs_quad, ps, r, {{0, colour, true}, {1, depth, fold_down ? false : p.dof_depth_linear, p.dof_depth_repeat}});  // (review D: honoured for the viewer's R32F game depth too; the renderer falls back to point where R32F cannot filter)
    if (!need("yBlur_hole_fill_fragment", ps)) return false;
    Regs h2;
    h2.SetN(1, p.hole_offsets, 12);
    h2.SetN(16, p.hole_weights, 13);
    pass(q2_, qw, qh, vs_quad, ps, h2, {{0, q1_, false}});
  }
  if (run_bright) {  // S101 lane PS: was `if (bloom)`; the bright pass is needed by either live half
    if (!p.dof && !p.dof_chain && !p.t2_blur) { error = "post pass: the bloom chain starts from the depth of field's blurred picture (dof must be on)"; return false; }   // (lane 238: the t2 blur is that picture too)
    if (!need("yDownScale_box4_down_fragment", ps)) return false;
    Regs r;
    r.SetN(0, p.bright_offsets, 4);
    r.Set(16, p.bright_down_color);
    pass(b0_, ew, eh, vs_quad, ps, r, {{0, q2_, false}});
    if (run_blur && !need("yBlur_t2_fragment", ps)) return false;  // S101 lane PS: the blur only when the bloom is live
    if (run_blur) {
    Regs t2;
    t2.SetN(0, p.bloom_t2_offsets, 13);
    t2.SetN(16, p.bloom_t2_weights, 13);
    pass(b1_, ew, eh, vs_quad, ps, t2, {{0, b0_, false}});
    if (!need("yBlur_t1_t2_fragment", ps)) return false;
    for (int k = 0; k < 2; ++k) {
      Regs t12;
      t12.SetN(0, p.bloom_t12_offsets[k], 15);
      t12.SetN(16, p.bloom_t12_weights[k], 15);
      pass(k == 0 ? b2_ : b1_, ew, eh, vs_quad, ps, t12, {{0, k == 0 ? b1_ : b2_, false}});
    }
    // the bloom is b1_ now
    }
  }
  if (variant_bloom && !run_blur && !drop_bloom) {  // S101 lane PS (S102 R18-B2: not when the permutation without bloom is used): the combine reads slot 7: black where the bloom is dead or not recorded
    rdr::TargetsOp t;
    t.color[0].image = b1_;
    t.color_count = 1;
    b.SetTargets(t);
    rdr::ClearOp c;
    c.flags = rdr::kClearColor, c.full = 1;
    b.Clear(c);
  }
  if (run_glare) {  // S101 lane PS: was `if (p.glare)`
    int t1 = -1, m12 = -1;
    if (!need("yBlur_t1_fragment", t1) || !need("yBlur_m1_m2_fragment", m12)) return false;
    for (int d = 0; d < 4; ++d) {
      Regs a, c;
      a.SetN(0, p.glare_offsets[2 * d], 8), a.SetN(16, p.glare_weights[2 * d], 8);
      c.SetN(0, p.glare_offsets[2 * d + 1], 8), c.SetN(16, p.glare_weights[2 * d + 1], 8);
      pass(gt_, ew, eh, vs_quad, t1, a, {{0, b0_, true}});
      pass(g_[d], ew, eh, vs_quad, t1, c, {{0, gt_, true}});
    }
    Regs m;
    m.SetN(16, p.glare_merge, 4);
    pass(gsum_, ew, eh, vs_quad, m12, m, {{0, g_[0], p.glare_merge_linear[0]}, {1, g_[1], p.glare_merge_linear[1]}, {2, g_[2], p.glare_merge_linear[2]}, {3, g_[3], p.glare_merge_linear[3]}});
  } else if (variant_bloom && !drop_bloom) {  // S101 lane PS: was `else if (bloom)` (S102 R18-B2: not when the permutation without bloom is used)
    rdr::TargetsOp t;  // bloom without glare: the star term is black
    t.color[0].image = gsum_;
    t.color_count = 1;
    b.SetTargets(t);
    rdr::ClearOp c;
    c.flags = rdr::kClearColor, c.full = 1;
    b.Clear(c);
  }

  // S103 PST (COV9_story.md 2a): the luminance chain for the combine's TONEMAP / NIGHT permutations (the game's sub_82C1E398: measure when S+180, step the adapted 1x1 when S+180 | S+184).
  rdr::ImageId adapted = rdr::kNoImage;
  if (p.tonemap || p.night) {
    // (the luminance images and lq_ were made at the top of Record: a failure there dropped the effect for the frame)
    int ini = -1, itr = -1, fin = -1, adp = -1;
    const int vs_tm = Find("yToneMap_quad_vertex");
    if (vs_tm < 0 || !need("yToneMap_initial_fragment", ini) || !need("yToneMap_iterative_fragment", itr) || !need("yToneMap_final_fragment", fin) || !need("yGlareEtc_adapt_fragment", adp)) { if (error.empty()) error = "post pass: the luminance variants are missing from the registry"; return false; }
    if (lum_just_made_) {   // once: the three 1x1 images start at 0 (the first step then snaps to the measured value: follow = 1)
      for (rdr::ImageId im : {lum_a_[0], lum_a_[1], lum_cur_}) {
        rdr::TargetsOp t;
        t.color[0].image = im;
        t.color_count = 1;
        b.SetTargets(t);
        rdr::ClearOp cl;
        cl.flags = rdr::kClearColor, cl.full = 1;
        b.Clear(cl);
      }
      lum_just_made_ = false;
    }
    // MEASURE (only when TONEMAP is on, as the game's; night alone keeps the last value, and the very first use of either measures once so the 1x1 is never the cleared 0).
    // The frame's log-average luma: yToneMap initial (9 taps at +-1/192 of the picture, linear) -> 64x64, iterative 64 -> 16 and 16 -> 4 (the 4x4 box of the larger image's texel centres, point), final -> 1x1 (the
    // 16 texel centres of the 4x4, exp). The game's 360 form (quarter rects of one EDRAM target, 3 resolves) is a habit not kept: four small float images, no copies.
    if (p.tonemap || !lum_measured_) {
      Regs r0, r1, r2, r3;
      const float o = 1.f / 192.f;
      for (int i = 0; i < 9; ++i) { const float v[4] = {float(i % 3 - 1) * o, float(i / 3 - 1) * o, 0.f, 0.f}; r0.Set(i, v); }
      for (int i = 0; i < 16; ++i) {
        const float kx = float(i % 4), ky = float(i / 4);
        const float a[4] = {(kx - 1.5f) / 64.f, (ky - 1.5f) / 64.f, 0.f, 0.f}, c[4] = {(kx - 1.5f) / 16.f, (ky - 1.5f) / 16.f, 0.f, 0.f}, d[4] = {(kx + 0.5f) / 4.f - 0.03125f, (ky + 0.5f) / 4.f - 0.03125f, 0.f, 0.f};
        r1.Set(i, a), r2.Set(i, c), r3.Set(i, d);
      }
      // The source is the DoF quarter downsample (q1_ = [B948]+12, COV9 2a [C]) when this frame recorded it (the DoF chain ran and was not skipped), else a quarter picture of our own (yDownScale box4: 4 linear taps one drawn
      // texel apart = the 4x4 block), so the measure never reads the full-size colour and does not depend on the render scale.
      const bool q1_recorded = !skip_near && !p.t2_blur && (p.dof || p.dof_chain);
      rdr::ImageId lsrc = q1_;
      if (!q1_recorded) {
        int bx = -1;
        const int vs_q = Find("yAfterImage_quad_vertex");
        if (vs_q < 0 || !need("yDownScale_box4_fragment", bx)) { if (error.empty()) error = "post pass: the quarter downsample variant is missing from the registry"; return false; }
        Regs rq;
        const float dx[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (int i = 0; i < 4; ++i) { const float v[4] = {dx[i][0] / float(w), dx[i][1] / float(h), 0.f, 0.f}; rq.Set(i, v); }
        pass(lq_, qw, qh, vs_q, bx, rq, {{0, colour, true}});
        lsrc = lq_;
      }
      pass(lum_[0], 64, 64, vs_tm, ini, r0, {{0, lsrc, true}});
      pass(lum_[1], 16, 16, vs_tm, itr, r1, {{0, lum_[0], false}});
      pass(lum_[2], 4, 4, vs_tm, itr, r2, {{0, lum_[1], false}});
      pass(lum_cur_, 1, 1, vs_tm, fin, r3, {{0, lum_[2], false}});
      lum_measured_ = true;
    }
    // STEP the adapted 1x1 ONCE PER GUEST FRAME (idempotent like the FXF history: the 120 fps tween records the same frame twice, the dev mirror draws it again; the repeat reads the same previous value and writes the
    // same image, so it gives the same result). A frame number more than kLumGap behind the last, or the first use, starts again (follow = 1: the 1x1 snaps to the measured value).
    const uint64_t gf = p.guest_frame;
    const bool fresh = !lum_armed_ || gf + kLumGap < lum_last_frame_;
    const bool same = lum_armed_ && !fresh && gf <= lum_last_frame_ && g_post_lum_plant_every.load(std::memory_order_relaxed) == 0;   // (the planted twin: every Record is a new step)
    if (fresh) lum_read_ = 1, lum_latest_ = 0, lum_elapsed_ = 1000.f;
    else if (!same) {
      // k = the guest frames we did not DRAW since the last step; but the game steps only while S+180 | S+184 is set, so after a stretch with the effect off (lum_paused_: Record without it, or no post at all) the
      // step is ONE guest frame (planted twin --pst-plant pause: the pause is ignored and k composes up to 30).
      const bool paused = lum_paused_ && g_post_lum_plant_nopause.load(std::memory_order_relaxed) == 0;
      const uint64_t k = paused ? 1 : std::min<uint64_t>(std::max<uint64_t>(gf > lum_last_frame_ ? gf - lum_last_frame_ : 1, 1), kLumGap);
      lum_read_ = lum_latest_, lum_latest_ = 1 - lum_read_, lum_elapsed_ = float(k) * kLumElapsed;
    }
    if (!same) lum_last_frame_ = gf, lum_paused_ = false;
    lum_armed_ = true;
    Regs ar;
    const float et[4] = {lum_elapsed_, 0.f, 0.f, 0.f};
    ar.Set(7, et);
    pass(lum_a_[lum_latest_], 1, 1, vs_glare, adp, ar, {{0, lum_a_[lum_read_], false}, {1, lum_cur_, false}});
    if (!same) ++lum_steps_;   // distinct steps: a repeat of the guest frame is not one
    adapted = lum_a_[lum_latest_];
  }

  // The combine.
  if (p.stop_before_combine) return true;  // S101 lane PO planted fault: the combine is left out (combined_ stays false)
  if (drop_bloom) g_post_bloom_clears_skipped.fetch_add(2, std::memory_order_relaxed);   // S103 SLV: the two eighth-size clears (b1_, gsum_) this frame did not record
  if (lite_on) rdr::CombineLiteNote(drop_bloom, p.DOFParam2[2], p.BloomScale[0], p.StarScale[0]);   // S102 R18-B2: the census's per-draw facts (relaxed stores / adds, no string, no lock)
  const std::string name = rdr::CombineLiteName(fold_combine ? vd_name(combine_base) : combine_base, [&](const char* n) { return Find(n) >= 0; });  // S102 R18-B2: the lite twin at the per-frame switch (combine_lite.h)
  if (!need(name.c_str(), ps)) return false;
  Regs c;
  c.Set(20, p.DOFParam), c.Set(21, p.DOFParam2), c.Set(27, p.RGB), c.Set(29, p.Alpha), c.Set(30, p.ZAdj), c.Set(31, p.BloomScale), c.Set(32, p.StarScale);
  if (fold_combine) c.Set(VD_REGISTER, vdk);   // S103 SLV2
  if (p.tonemap) {   // S103 PST: MiddleGray c33 (the game writes it only when S+180 is on, sub_82C1C768)
    const float mg[4] = {p.middle_gray > 0.f ? p.middle_gray : kMiddleGrayDefault, 0.f, 0.f, 0.f};
    c.Set(33, mg);
  }
  const rdr::ImageId blur = skip_near ? colour : (p.dof ? q2_ : colour);   // S102 R24-E: skip_near binds slot 2 to the scene colour (never sampled then)
  pass(p.film ? mid_ : chain_out, w, h, vs_glare, ps, c, {{0, colour, true}, {2, blur, true}, {3, depth, false}, {6, p.far_blur != rdr::kNoImage ? p.far_blur : blur, true}, {7, b1_, true}, {8, gsum_, true}, {9, adapted, false}});  // S101 lane PS: far_blur; S103 PST: slot 9 = the adapted luminance (point; read at (0.01, 0.01))
  combined_ = true;  // S101 lane PO
  if (p.film) {
    int mono = -1;
    if (!need("yMonotone_monotone_fragment", mono)) return false;
    Regs m;
    m.Set(0, p.film_cb), m.Set(1, p.film_cr), m.Set(2, p.film_alpha);
    pass(chain_out, w, h, vs_quad, mono, m, {{0, mid_, false}});
  }
  if (ghost) {   // S103 FXF: yAfterImage, effect 4, after the film (FXP.md adversary E)
    // The history is idempotent per GUEST frame (review 1: the 120 fps tween records the same guest frame twice, the dev mirror draws it again): two history images, the roles swap only when the guest frame advances;
    // a repeat of the same frame reads the same image and writes the same one. The ghost pass writes `out` AND hist_[write] (two colour targets): no copy.
    const bool same = ghost_armed_ && p.ghost_frame <= ghost_last_frame_ && p.ghost_frame + kGhostGap >= ghost_last_frame_;
    const bool fresh = !ghost_armed_ || p.ghost_frame > ghost_last_frame_ + kGhostGap || p.ghost_frame + kGhostGap < ghost_last_frame_;
    if (fresh) ghost_read_ = 1, ghost_write_ = 0, ghost_k_ = 1, ghost_first_ = true;   // a new effect: no blend this frame, the history is filled
    else if (!same) {
      ghost_k_ = uint32_t(std::min<uint64_t>(std::max<uint64_t>(p.ghost_frame - ghost_last_frame_, 1), kGhostGap));   // review 5: guest frames since the last ghost draw
      ghost_read_ = ghost_write_, ghost_write_ = 1 - ghost_read_, ghost_first_ = false;
    }
    const bool first = ghost_first_;
    if (!same) ghost_last_frame_ = p.ghost_frame;
    ghost_armed_ = true;
    // Pace (review 5): a skipped / decoupled guest frame must not slow the game's real-time trail: k frames of the same ghost in one step: alpha 1 - (1 - a)^k (additive: a k), scale s^k.
    const float kf = float(ghost_k_);
    const float alpha_eff = p.ghost_additive ? std::fmin(p.ghost_alpha * kf, 1.f) : 1.f - std::pow(1.f - p.ghost_alpha, kf);
    const float scale_eff = std::pow(p.ghost_scale, kf);
    if (!ghost_program_made_) {
      rdr::ProgramDesc pd;
      pd.name = "post: afterimage";
      pd.vertex_spirv = kSpv_rdr_fullscreen_vert, pd.vertex_bytes = sizeof(kSpv_rdr_fullscreen_vert);
      pd.fragment_spirv = kSpv_rdr_post_ghost_frag, pd.fragment_bytes = sizeof(kSpv_rdr_post_ghost_frag);
      ghost_program_ = renderer_.AddProgram(pd);
      ghost_layout_ = renderer_.AddVertexLayout(rdr::VertexLayoutDesc{});   // (empty: the triangle has no vertex inputs)
      ghost_program_made_ = true;
    }
    rdr::TargetsOp t;
    t.color[0].image = final_out;   // S103 FXH: cue_[0] when a cue effect follows
    t.color[1].image = hist_[ghost_write_];
    t.color_count = 2;
    b.SetTargets(t);
    const float k[12] = {p.ghost_centre[0], p.ghost_centre[1], scale_eff, first ? 0.f : alpha_eff,
                         p.ghost_colour[0], p.ghost_colour[1], p.ghost_colour[2], p.ghost_additive ? 1.f : 0.f,
                         float(w), float(h), 0.f, 0.f};
    rdr::TextureSet set{};
    set.slots[0].image = pre_, set.slots[0].sampler = sampler(false, false);
    set.slots[1].image = hist_[ghost_read_], set.slots[1].sampler = sampler(true, false);
    rdr::DrawState st{};
    st.program = ghost_program_, st.vertex_layout = ghost_layout_, st.topology = 3, st.front_face = 1;
    st.blend[0].write_mask = 0xF, st.blend[1].write_mask = 0xF;
    rdr::DynamicState dyn{};
    dyn.viewport[2] = float(w), dyn.viewport[3] = float(h), dyn.viewport[5] = 1.f;
    dyn.scissor.width = w, dyn.scissor.height = h;
    dyn.system[0] = 1.f, dyn.system[1] = 1.f, dyn.system[2] = 1.f;  // (the triangle is generated in Vulkan's clip space: no flip, as the depth triangle)
    dyn.system[9] = 7.f;
    rdr::DrawOp op{};
    op.state = b.State(st);
    op.dynamic = b.Dynamic(dyn);
    op.textures = b.Textures(set);
    op.vs_constants = vs_constants;
    op.fs_constants = b.PushConstants(k, sizeof k);
    op.count = 3;
    op.instances = 1;
    b.Draw(op, rdr::kGrpPost);
    ++passes_;
  }

  // S103 FXH (COV9_story.md 2b / 2c / 2d): the cue effects, in the game's order. Each reads the previous picture (cue_[0] after the chain / ghost) and the last one writes `out`; between them the two images swap.
  // Each of the game's three resolves the whole frame into [B948]+4 first (a 360 HABIT: a full-screen copy per effect); ours is the ping-pong, no copy (docs\renderer\360_HABITS.md).
  if (cue_on) {
    int order[3], n = 0;   // 0 defocus, 1 emboss, 2 retro
    if (p.defocus) order[n++] = 0;
    if (p.emboss) order[n++] = 1;
    if (p.retro) order[n++] = 2;
    if (g_post_cue_plant.load(std::memory_order_relaxed) == 4) std::reverse(order, order + n);   // RIG-ONLY twin: the passes in the wrong order (the composite check must differ)
    const int vs_cue = Find("yAfterImage_quad_vertex");   // the shared full-screen quad (hash 088496E2D7848F6D: yDefocus / yEmboss / yRetroFilm `quad` are the same stage)
    if (vs_cue < 0) { error = "post pass: the quad vertex variant is missing from the registry"; return false; }
    rdr::ImageId src = cue_[0];
    for (int i = 0; i < n; ++i) {
      const rdr::ImageId dst = (i == n - 1) ? out : (src == cue_[0] ? cue_[1] : cue_[0]);
      int cps = -1;
      Regs r;
      if (order[i] == 0) {   // yDefocus `hue`: slot 0 = the frame (clamp, point), slot 1 = the DoF blur q2_ (clamp, linear); c0.x = g_f1DefocusRate, c1 = (Y, Cb, Cr, A)
        if (!need("yDefocus_hue_fragment", cps)) return false;
        const float c0[4] = {p.defocus_rate, 0.f, 0.f, 0.f};
        r.Set(0, c0), r.Set(1, p.defocus_ycbcr);
        pass(dst, w, h, vs_cue, cps, r, {{0, src, false}, {1, q2_, true}});
      } else if (order[i] == 1) {   // yEmboss: c0 = the step in uv (S+596 display pixels over the display size); slot 0 = the frame (clamp, point)
        if (!need("yEmboss_emboss_fragment", cps)) return false;
        const float c0[4] = {p.emboss_step[0], p.emboss_step[1], 0.f, 0.f};
        r.Set(0, c0);
        pass(dst, w, h, vs_cue, cps, r, {{0, src, false}});
      } else {   // yRetroFilm `retro`: slot 0 = the frame (clamp, point), 1 / 2 = the noise strips (clamp, point), 3 = the grain (wrap, linear); c0 = g_Time (t44, t48, t52, 4 / display height), c4 = g_Range
        if (!need("yRetroFilm_retro_fragment", cps)) return false;
        cue_fx::RetroAdvance(retro_, p.guest_frame, p.retro_rate >= 1.f ? p.retro_rate : cue_fx::kRetroRateDefault, g_post_cue_plant.load(std::memory_order_relaxed) == 1);
        const float c0[4] = {retro_.t44, retro_.t48, retro_.t52, p.retro_jitter};
        r.Set(0, c0), r.Set(4, p.retro_range);
        const bool swap = g_post_cue_plant.load(std::memory_order_relaxed) == 2;   // RIG-ONLY twin: the strips in each other's slot
        pass(dst, w, h, vs_cue, cps, r, {{0, src, false}, {1, noise_[swap ? 1 : 0], false}, {2, noise_[swap ? 0 : 1], false}, {3, noise_[2], true, true}});
      }
      g_post_cue_passes.fetch_add(1, std::memory_order_relaxed);
      src = dst;
    }
  }
  return true;
}

std::string PostPass::CombineVariantName(const PostParams& p, bool drop_bloom) {  // S101 lane PO (S102 R18-B2: drop_bloom)
  std::string name = "yGlareEtc_";
  if (p.dof) name += "dof_";
  if (p.fog) name += "fog_";
  if (p.night) name += "night_";      // S103 PST: the registry's order is dof, fog, night, tonemap, bloom
  if (p.tonemap) name += "tonemap_";
  if (!drop_bloom && (p.bloom || p.glare || p.bloom_variant)) name += "bloom_";
  if (name == "yGlareEtc_") name += "base_";
  return name + "fragment";
}

// ---- the defaults -----------------------------------------------------------------------------------------------------------------------

PostParams DefaultPostParams(uint32_t width, uint32_t height, bool entrance) {
  PostParams p;
  p.width = width, p.height = height;
  const float w = float(width), h = float(height);
  const float qx = 4.f / w, qy = 4.f / h, ex = 8.f / w, ey = 8.f / h;  // one texel of the quarter / eighth-size images
  p.dof = true, p.fog = true, p.bloom = entrance, p.glare = false, p.film = false;
  // combine constants (post_effects.md, measured: match run 4511, entrance Bryan run 4522 / S98 rule)
  if (entrance) {
    const float a[4] = {INFINITY, 100.7f, 0, 0.7f}, bb[4] = {0, 0.985158f, 0, 0};
    std::memcpy(p.DOFParam, a, 16), std::memcpy(p.DOFParam2, bb, 16);
  } else {
    const float a[4] = {91.33f, 338.67f, 0.992f, 0.8f}, bb[4] = {0.991855f, 0.99509f, 0, 0};
    std::memcpy(p.DOFParam, a, 16), std::memcpy(p.DOFParam2, bb, 16);
  }
  const float rgb[4] = {0.4f, 0.55f, 0.55f, 1.f}, alpha[4] = {0.08f, 0.993f, 0, 0}, zadj[4] = {0.993f, 0, 0, 0};
  std::memcpy(p.RGB, rgb, 16), std::memcpy(p.Alpha, alpha, 16), std::memcpy(p.ZAdj, zadj, 16);
  p.BloomScale[0] = 0.4f;
  p.StarScale[0] = 0.f;
  // 1: the 2x2 depth-aware downsample: taps one full-size texel away
  const float dx[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  for (int i = 0; i < 4; ++i) p.dof_offsets[i][0] = dx[i][0] / w, p.dof_offsets[i][1] = dx[i][1] / h;
  p.dof_depth_linear = !entrance;
  p.dof_param2_down[0] = p.DOFParam2[0], p.dof_param2_down[1] = p.DOFParam2[1], p.dof_param2_down[3] = 0.0003f;
  // 2: hole fill: 12 neighbours of the quarter-size image, gaussian (sigma 1) weights
  const int hn[12][2] = {{-1, -1}, {-1, 0}, {-1, 1}, {0, -2}, {0, -1}, {-2, 0}, {0, 1}, {0, 2}, {1, -1}, {1, 0}, {1, 1}, {2, 0}};
  const float g0 = 0.3989423f;  // 1 / sqrt(2 pi)
  p.hole_weights[0][0] = p.hole_weights[0][1] = p.hole_weights[0][2] = g0, p.hole_weights[0][3] = 1.f;
  for (int i = 0; i < 12; ++i) {
    p.hole_offsets[i][0] = float(hn[i][0]) * qx, p.hole_offsets[i][1] = float(hn[i][1]) * qy;
    const float wt = g0 * std::exp(-0.5f * float(hn[i][0] * hn[i][0] + hn[i][1] * hn[i][1]));
    p.hole_weights[i + 1][0] = p.hole_weights[i + 1][1] = p.hole_weights[i + 1][2] = wt, p.hole_weights[i + 1][3] = 1.f;
  }
  // 3: the bright pass: taps half a texel of the quarter-size source apart; the threshold subtracted
  // (measured: 0.0015528 = 0.5 / 322 and 0.0027473 = 0.5 / 182 at 1280x720: the game steps by half a texel of the quarter-size image plus a two-texel border)
  for (int i = 0; i < 4; ++i) p.bright_offsets[i][0] = dx[i][0] * 0.5f / (w / 4.f + 2.f), p.bright_offsets[i][1] = dx[i][1] * 0.5f / (h / 4.f + 2.f);
  (void)qx, (void)qy;
  p.bright_down_color[0] = 0.96f;
  // 4: bloom: 13 taps on a plus-shaped diamond (|dx| + |dy| <= 2), then a 15-tap gaussian (sigma 3) twice
  const int bn[13][2] = {{-2, 0}, {-1, -1}, {-1, 0}, {-1, 1}, {0, -2}, {0, -1}, {0, 0}, {0, 1}, {0, 2}, {1, -1}, {1, 0}, {1, 1}, {2, 0}};
  float total = 0;
  for (int i = 0; i < 13; ++i) total += std::exp(-0.5f * float(bn[i][0] * bn[i][0] + bn[i][1] * bn[i][1]));
  for (int i = 0; i < 13; ++i) {
    p.bloom_t2_offsets[i][0] = float(bn[i][0]) * ex, p.bloom_t2_offsets[i][1] = float(bn[i][1]) * ey;
    const float wt = std::exp(-0.5f * float(bn[i][0] * bn[i][0] + bn[i][1] * bn[i][1])) / total;
    p.bloom_t2_weights[i][0] = p.bloom_t2_weights[i][1] = p.bloom_t2_weights[i][2] = wt, p.bloom_t2_weights[i][3] = 1.f;
  }
  const float norm = 1.f / 0.2659615f;  // (measured centre weight; the game's normaliser is not the plain sum of the 15 taps)
  const float steps[2][2] = {{ex, 0.f}, {8.f / h * 1.f, 0.f}};  // the game's second pass steps by 1/90 in x as well (post_effects.md: measured)
  for (int pass = 0; pass < 2; ++pass)
    for (int i = 0; i < 15; ++i) {
      const int k = i < 8 ? i : -(i - 7);
      const float wt = std::exp(-0.5f * float(k * k) / 9.f) / norm;
      p.bloom_t12_offsets[pass][i][0] = float(k) * steps[pass][0], p.bloom_t12_offsets[pass][i][1] = float(k) * steps[pass][1], p.bloom_t12_offsets[pass][i][3] = i < 8 ? 0.f : 1.f;
      p.bloom_t12_weights[pass][i][0] = p.bloom_t12_weights[pass][i][1] = p.bloom_t12_weights[pass][i][2] = wt, p.bloom_t12_weights[pass][i][3] = 1.f;
    }
  // 5: glare: four streak directions (two passes each; the second steps 8x as far), colour-tinted decaying weights, the four averaged.
  // Measured from the entrance capture s100_s89k_a (ops 1150-1166): the directions are the game's fixed star pattern, the weights its tuning.
  const float dirs[4][2] = {{0.0004079777f, -0.003777816f}, {-0.002135287f, -0.0007218073f}, {-0.0004079777f, 0.003777816f}, {0.002135287f, 0.0007218073f}};
  const float w1[8][3] = {{.2933334f, .2933334f, .2933334f}, {.3378109f, .255818f, .255818f}, {.364647f, .2355684f, .2355684f}, {.2794053f, .2317794f, .2952806f},
                          {.2280513f, .2280513f, .3530109f}, {.2243832f, .2397519f, .3012267f}, {.2207741f, .2812601f, .2207741f}, {.2321012f, .2618578f, .2321012f}};
  const float w2[8][3] = {{.6083333f, .6083333f, .6083333f}, {.5782394f, .5050446f, .5050446f}, {.5336059f, .4307421f, .4307421f}, {.4122183f, .3783374f, .423512f},
                          {.3323083f, .3323083f, .4116654f}, {.2918791f, .3005919f, .3354431f}, {.2563686f, .2869798f, .2563686f}, {.2319002f, .2453437f, .2319002f}};
  for (int d = 0; d < 4; ++d)
    for (int k = 0; k < 8; ++k) {
      for (int c = 0; c < 2; ++c) {
        p.glare_offsets[2 * d][k][c] = dirs[d][c] * float(k);
        p.glare_offsets[2 * d + 1][k][c] = dirs[d][c] * 8.f * float(k);
      }
      for (int c = 0; c < 3; ++c) p.glare_weights[2 * d][k][c] = w1[k][c], p.glare_weights[2 * d + 1][k][c] = w2[k][c];
    }
  for (int i = 0; i < 4; ++i) p.glare_merge[i][0] = p.glare_merge[i][1] = p.glare_merge[i][2] = 0.25f, p.glare_merge[i][3] = 1.f;
  return p;
}

}  // namespace rdr_scene
