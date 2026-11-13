// Session 101, lane 34: the post pass. See post_pass.h and docs\renderer\POST_PASS.md.
#include "post_pass.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "../renderer/shaders/spv/fx_registry.h"
#include "../renderer/shaders/spv/rdr_fullscreen_vert.h"
#include "../renderer/shaders/spv/rdr_post_depth_frag.h"

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

}  // namespace

PostPass::~PostPass() {
  for (rdr::ImageId i : images_) renderer_.DestroyImage(i);
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
  {
    rdr::ImageDesc d;  // the game-depth image of the viewer's scenes (R32F: the depth differences are ~0.005)
    d.width = uint16_t(w), d.height = uint16_t(h);
    d.format = rdr::Format::R32F;
    d.usage = rdr::kUsageTarget | rdr::kUsageSampled;
    dconv_ = renderer_.CreateImage(d);
    if (dconv_ != rdr::kNoImage) images_.push_back(dconv_);
  }
  size_w_ = w, size_h_ = h;
  if (q1_ == rdr::kNoImage || gsum_ == rdr::kNoImage) { error = "post pass: could not create the pass images"; return false; }
  return true;
}

bool PostPass::Record(rdr::FrameBuilder& b, rdr::ImageId colour, rdr::ImageId depth, rdr::ImageId out, const PostParams& p, std::string& error) {
  passes_ = 0;
  combined_ = false;  // S101 lane PO
  if (colour == rdr::kNoImage || out == rdr::kNoImage || ((p.dof || p.fog || p.dof_chain) && depth == rdr::kNoImage)) { error = "post pass: a missing input image"; return false; }  // (yGlareEtc_fog reads the depth too)
  if (!Ensure(p.width, p.height, p.scaled_images, error)) return false;  // S101 lane PO: + scaled_images
  const bool bloom = p.bloom || p.glare;
  // S101 lane PS: the dead work. The chain's parts that the combine multiplies by zero are not recorded; the images it still reads are cleared.
  const bool run_blur = bloom && !p.dead_bloom, run_glare = p.glare && !p.dead_glare, run_bright = run_blur || run_glare;
  const bool variant_bloom = bloom || p.bloom_variant;  // the combine has the bloom permutation
  if (p.film && mid_ == rdr::kNoImage) mid_ = Make(p.width, p.height);
  if (p.film && mid_ == rdr::kNoImage) { error = "post pass: could not create the film image"; return false; }

  const int vs_quad = Find("yAfterImage_quad_vertex"), vs_glare = Find("yGlareEtc_corner_uv_vertex");
  if (vs_quad < 0 || vs_glare < 0) { error = "post pass: vertex variants missing from the registry"; return false; }
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
    b.Draw(op);
    ++passes_;
  };

  const uint32_t w = p.width, h = p.height, qw = std::max(1u, w / 4), qh = std::max(1u, h / 4), ew = std::max(1u, w / 8), eh = std::max(1u, h / 8);
  int ps = -1;

  if (p.depth_from_viewer && (p.dof || p.fog)) {  // (S101 lane 34 review A: the haze reads the depth as well) the viewer's depth -> the game's depth (one triangle, R32F)
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
    b.Draw(op);
    ++passes_;
    depth = dconv_;
  }

  if (p.t2_blur) {   // lane 140: the blur is one t2 pass over the stream's own quarter-size picture (no dof_down, no hole_fill)
    if (p.blur_source == rdr::kNoImage) { error = "post pass: the t2 blur has no source image"; return false; }
    if (!need("yBlur_t2_fragment", ps)) return false;
    Regs t2;
    t2.SetN(0, p.blur_t2_offsets, 13);   // lane 238: the blur's own tables (the bloom's t2 has others)
    t2.SetN(16, p.blur_t2_weights, 13);
    pass(q2_, qw, qh, vs_quad, ps, t2, {{0, p.blur_source, false}});
  } else
  if (p.dof || p.dof_chain) {  // S101 lane PS: dof_chain = the bloom's source though the combine has no dof permutation
    if (!need("yDownScale_dof_fragment", ps)) return false;
    Regs r;
    r.SetN(0, p.dof_offsets, 4);
    r.Set(21, p.dof_param2_down);
    pass(q1_, qw, qh, vs_quad, ps, r, {{0, colour, true}, {1, depth, p.dof_depth_linear, p.dof_depth_repeat}});  // (review D: honoured for the viewer's R32F game depth too; the renderer falls back to point where R32F cannot filter)
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
  if (variant_bloom && !run_blur) {  // S101 lane PS: the combine reads slot 7: black where the bloom is dead or not recorded
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
  } else if (variant_bloom) {  // S101 lane PS: was `else if (bloom)`
    rdr::TargetsOp t;  // bloom without glare: the star term is black
    t.color[0].image = gsum_;
    t.color_count = 1;
    b.SetTargets(t);
    rdr::ClearOp c;
    c.flags = rdr::kClearColor, c.full = 1;
    b.Clear(c);
  }

  // The combine.
  if (p.stop_before_combine) return true;  // S101 lane PO planted fault: the combine is left out (combined_ stays false)
  const std::string name = CombineVariantName(p);
  if (!need(name.c_str(), ps)) return false;
  Regs c;
  c.Set(20, p.DOFParam), c.Set(21, p.DOFParam2), c.Set(27, p.RGB), c.Set(29, p.Alpha), c.Set(30, p.ZAdj), c.Set(31, p.BloomScale), c.Set(32, p.StarScale);
  const rdr::ImageId blur = p.dof ? q2_ : colour;
  pass(p.film ? mid_ : out, w, h, vs_glare, ps, c, {{0, colour, true}, {2, blur, true}, {3, depth, false}, {6, p.far_blur != rdr::kNoImage ? p.far_blur : blur, true}, {7, b1_, true}, {8, gsum_, true}});  // S101 lane PS: far_blur
  combined_ = true;  // S101 lane PO
  if (p.film) {
    int mono = -1;
    if (!need("yMonotone_monotone_fragment", mono)) return false;
    Regs m;
    m.Set(0, p.film_cb), m.Set(1, p.film_cr), m.Set(2, p.film_alpha);
    pass(out, w, h, vs_quad, mono, m, {{0, mid_, false}});
  }
  return true;
}

std::string PostPass::CombineVariantName(const PostParams& p) {  // S101 lane PO
  std::string name = "yGlareEtc_";
  if (p.dof) name += "dof_";
  if (p.fog) name += "fog_";
  if (p.bloom || p.glare || p.bloom_variant) name += "bloom_";
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
