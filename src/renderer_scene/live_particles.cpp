// Our particles from the scene record: see live_particles.h.
#include "live_particles.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "../renderer/problem.h"   // S103 LVF: rdr::Log (the run log)
#include "../renderer/shaders/spv/fx_registry.h"
#include "sampler_table.h"   // S103 B1
#include "flat_particles.h"   // S111 TITLESMOKE: a flat frame's group position (RecordFlat)

namespace rdr_scene {
namespace {

using rdr::fx::kVariants;
constexpr int kVariantCount = int(sizeof(rdr::fx::kVariants) / sizeof(rdr::fx::kVariants[0]));

int FindVariant(const char* name) {
  for (int i = 0; i < kVariantCount; ++i)
    if (kVariants[i].name && std::strcmp(kVariants[i].name, name) == 0) return i;
  return -1;
}

// The vertex constants block of yParticle_base_vertex (fx_registry: ViewProj c56 @0, LocalWorld c228 @64, ZOffset c200 @128, UseNormal c201 @144, LightVector c202 @160).
constexpr uint32_t kBlockBytes = 176;
constexpr uint32_t kOffViewProj = 0, kOffLocalWorld = 64, kOffZOffset = 128, kOffUseNormal = 144, kOffLight = 160;

}  // namespace

LiveParticles::LiveParticles(rdr::Renderer& renderer, TextureLookup texture_of, const ParticleFaults& faults)
    : renderer_(renderer), texture_of_(std::move(texture_of)), faults_(faults) {}

LiveParticles::~LiveParticles() {
  if (white_ != rdr::kNoImage) renderer_.DestroyImage(white_);
  if (soft_depth_ != rdr::kNoImage) renderer_.DestroyImage(soft_depth_);
}

bool LiveParticles::EnsurePrograms() {
  if (ready_) return true;
  if (failed_) return false;
  const int vs = FindVariant("yParticle_base_vertex"), ps = FindVariant("yParticle_base_fragment");
  if (vs < 0 || ps < 0) {
    rdr::Log("[scene][particles] ERROR: the yParticle programs are not in the registry: no particles will be drawn");  // once: failed_ latches
    return failed_ = true, false;
  }
  rdr::ProgramDesc desc;
  desc.name = "yParticle_base (live particles)";
  desc.vertex_spirv = rdr::fx::Words(size_t(vs), false), desc.vertex_bytes = kVariants[vs].bytes;
  desc.fragment_spirv = rdr::fx::Words(size_t(ps), false), desc.fragment_bytes = kVariants[ps].bytes;
  program_ = renderer_.AddProgram(desc);
  constexpr uint16_t kRGB32F = 106, kRG32F = 103, kRGBA8Unorm = 37;  // VkFormat numbers (as front_end.cpp LayoutFor)
  rdr::VertexLayoutDesc ld{};
  ld.strides[0] = sizeof(ParticleDrawVertex);
  ld.attributes[0] = {0, 0, kRGB32F, uint32_t(offsetof(ParticleDrawVertex, pos))};
  ld.attributes[1] = {1, 0, kRGBA8Unorm, uint32_t(offsetof(ParticleDrawVertex, color))};
  ld.attributes[2] = {2, 0, kRG32F, uint32_t(offsetof(ParticleDrawVertex, uv))};
  ld.attributes[3] = {3, 0, kRG32F, uint32_t(offsetof(ParticleDrawVertex, uv2))};
  ld.attribute_count = 4;
  layout_ = renderer_.AddVertexLayout(ld);
  rdr::ImageDesc d;
  d.width = d.height = 1;
  d.format = rdr::Format::RGBA8;
  d.usage = rdr::kUsageSampled;
  white_ = renderer_.CreateImage(d);
  const uint8_t px[4] = {255, 255, 255, 255};
  renderer_.UpdateImage(white_, 0, 0, px, 4);
  return ready_ = true;
}

bool LiveParticles::EnsureSoftPrograms() {
  if (soft_ready_) return true;
  if (soft_failed_) return false;
  const int vs = FindVariant("ySoftParticle_base_vertex"), ps = FindVariant("ySoftParticle_base_fragment");
  if (vs < 0 || ps < 0) {
    rdr::Log("[scene][particles] ERROR: the ySoftParticle programs are not in the registry: soft batches are drawn hard");  // once: soft_failed_ latches
    return soft_failed_ = true, false;
  }
  rdr::ProgramDesc desc;
  desc.name = "ySoftParticle_base (live particles)";
  desc.vertex_spirv = rdr::fx::Words(size_t(vs), false), desc.vertex_bytes = kVariants[vs].bytes;
  desc.fragment_spirv = rdr::fx::Words(size_t(ps), false), desc.fragment_bytes = kVariants[ps].bytes;
  soft_program_ = renderer_.AddProgram(desc);
  constexpr uint16_t kRGB32F = 106, kRG32F = 103, kRGBA8Unorm = 37;   // the soft vertex stage reads position, colour and ONE uv (no uv2): a 3-attribute layout (an unread attribute is a validation warning)
  rdr::VertexLayoutDesc ld{};
  ld.strides[0] = sizeof(ParticleDrawVertex);
  ld.attributes[0] = {0, 0, kRGB32F, uint32_t(offsetof(ParticleDrawVertex, pos))};
  ld.attributes[1] = {1, 0, kRGBA8Unorm, uint32_t(offsetof(ParticleDrawVertex, color))};
  ld.attributes[2] = {2, 0, kRG32F, uint32_t(offsetof(ParticleDrawVertex, uv))};
  ld.attribute_count = 3;
  soft_layout_ = renderer_.AddVertexLayout(ld);
  return soft_ready_ = true;
}

// S105 A1: true when the scene depth the copy reads carries kUsageScaled (the stream-free host's scaled scene): the copy's destination must be the same size class.
bool LiveParticles::SceneDepthScaled(const StageCtx& ctx) const {
  uint32_t w = 0, h = 0;
  bool scaled = false;
  return renderer_.ImageExtent(ctx.depth, w, h, scaled) && scaled;
}

// The sampled copy of the scene depth (same size, Depth format). Re-made when the frame size changes (DestroyImage is deferred past the frames in flight).
bool LiveParticles::EnsureSoftDepth(uint32_t w, uint32_t h, bool scaled) {
  if (soft_depth_ != rdr::kNoImage && soft_w_ == w && soft_h_ == h && soft_scaled_ == scaled) return true;
  if (soft_depth_ != rdr::kNoImage) renderer_.DestroyImage(soft_depth_), soft_depth_ = rdr::kNoImage;
  rdr::ImageDesc d;
  d.width = uint16_t(w);
  d.height = uint16_t(h);
  d.format = rdr::Format::Depth;
  d.usage = uint8_t(rdr::kUsageTarget | rdr::kUsageSampled | (scaled ? rdr::kUsageScaled : 0));   // S105 A1: same size class as the scene depth it is copied from
  soft_depth_ = renderer_.CreateImage(d);
  soft_w_ = w, soft_h_ = h, soft_scaled_ = scaled;
  return soft_depth_ != rdr::kNoImage;
}

void LiveParticles::Record(Stage stage, const StageCtx& ctx, rdr::FrameBuilder& fb) {
  if (stage != Stage::AfterBlended || !ctx.frame || !ctx.frame->particles || ctx.frame->particles->empty()) return;
  const bool mirror = ctx.frame->viewer_space && !faults_.no_mirror;  // the frame is in the viewer's space (S = diag(1, -1, 1) on worlds, view' = S * view): the game-space tables are mirrored once here
  if (mirror) ++stats_.viewer_space_mirrored;
  rdr::DynamicState dynamic{};
  dynamic.viewport[2] = float(ctx.width);
  dynamic.viewport[3] = float(ctx.height);
  dynamic.viewport[5] = 1.0f;
  dynamic.scissor.width = ctx.width;
  dynamic.scissor.height = ctx.height;
  dynamic.system[0] = 1.f, dynamic.system[1] = -1.f, dynamic.system[2] = 1.f;  // as the front-end's: clip y up, Vulkan viewport
  dynamic.system[9] = 7.f;                                                       // alpha test: always
  Draw(*ctx.frame->particles, ctx, dynamic, mirror, false, 0, nullptr, 0, fb);
}

// S111 TITLESMOKE: see live_particles.h RecordFlat. No frame and no depth in the context: a soft batch draws hard (counted soft_as_hard), the depth test is off.
void LiveParticles::RecordFlat(const ParticleTables& pt, const FlatView& v, size_t pos, const Quad2D* rows, size_t nrows, rdr::FrameBuilder& fb) {
  if (pt.empty() || !v.view_proj) return;
  StageCtx ctx;
  ctx.view_proj = v.view_proj;
  ctx.width = v.viewport.width, ctx.height = v.viewport.height;
  rdr::DynamicState dynamic{};
  dynamic.viewport[0] = float(v.viewport.x), dynamic.viewport[1] = float(v.viewport.y);
  dynamic.viewport[2] = float(v.viewport.width), dynamic.viewport[3] = float(v.viewport.height);
  dynamic.viewport[5] = 1.0f;
  dynamic.scissor = v.scissor;
  dynamic.system[0] = 1.f, dynamic.system[1] = -1.f, dynamic.system[2] = 1.f;  // as Record and the 2D rows (quads2d_draw.cpp Append): the game's clip y up, Vulkan viewport
  dynamic.system[9] = 7.f;                                                       // alpha test: always
  Draw(pt, ctx, dynamic, false, true, pos, rows, nrows, fb);
}

void LiveParticles::Draw(const ParticleTables& pt, const StageCtx& ctx, const rdr::DynamicState& dynamic, bool mirror, bool flat, size_t pos, const Quad2D* rows, size_t nrows, rdr::FrameBuilder& fb) {
  if (!EnsurePrograms()) return;

  // Frame-constant parts of the vertex block: ViewProj as the effect wants it (row k = column k of the row-vector matrix, as front_end.cpp Resolve "ViewProj").
  float block[kBlockBytes / 4];
  std::memset(block, 0, sizeof block);
  for (int k = 0; k < 4; ++k)
    for (int i = 0; i < 4; ++i) block[kOffViewProj / 4 + 4 * k + i] = ctx.view_proj[i * 4 + k];

  const uint32_t dyn = fb.Dynamic(dynamic);
  const uint8_t zero16[16] = {};
  const uint32_t fs_constants = fb.PushConstants(zero16, 16);

  rdr::SamplerDesc sd{};
  sd.mag_filter = sd.min_filter = 1;
  sd.mip_mode = 1;
  sd.address_u = sd.address_v = sd.address_w = 2;  // clamp to edge [U: the game's particle sampler state is not read here]
  sd.max_anisotropy = 1;
  sd.max_lod = 1000.f;
  const uint32_t sampler = fb.Sampler(sd);

  // S103 PRT: OUR projection's depth terms from the view-projection (row vectors: clip_j = sum_i p_i * VP[i * 4 + j]). A perspective projection has clip.w = (view z) and clip.z = A * clip.w + B, so
  // column 2 = A * column 3 + B * (0, 0, 0, 1). Anything else (an orthographic or degenerate matrix) has no distance to linearise: soft batches then draw hard.
  bool soft_basis = false;
  float soft_a = 0.f, soft_b = 0.f, c3[4] = {};
  {
    const float* vp = ctx.view_proj;
    int best = -1;
    float bm = 0.f;
    for (int i = 0; i < 3; ++i)
      if (std::fabs(vp[i * 4 + 3]) > bm) bm = std::fabs(vp[i * 4 + 3]), best = i;
    if (best >= 0 && bm > 1e-6f) {
      soft_a = vp[best * 4 + 2] / vp[best * 4 + 3];
      soft_b = vp[12 + 2] - soft_a * vp[12 + 3];
      bool ok = std::isfinite(soft_a) && std::isfinite(soft_b) && soft_b != 0.f;
      for (int i = 0; i < 3 && ok; ++i) ok = std::fabs(vp[i * 4 + 2] - soft_a * vp[i * 4 + 3]) <= 1e-3f * (std::fabs(vp[i * 4 + 2]) + std::fabs(soft_a * vp[i * 4 + 3]) + 1e-6f);
      soft_basis = ok;
      for (int i = 0; i < 4; ++i) c3[i] = vp[i * 4 + 3];
    }
  }
  const bool soft_possible = g_soft_particles.load(std::memory_order_relaxed) && soft_basis && ctx.depth != rdr::kNoImage && HasSoft(&pt) && EnsureSoftPrograms() && EnsureSoftDepth(ctx.width, ctx.height, SceneDepthScaled(ctx));
  bool depth_copied = false;

  bool any = false;
  std::vector<ParticleDrawVertex>& verts = verts_;
  for (const ParticleBatch& b : pt.batches) {
    if (flat && (!flat_particles::IsFlat(b) || flat_particles::PosOf(pt, b, rows, nrows) != pos)) continue;   // S111 TITLESMOKE: a flat frame's group (a screen call's batches) is drawn at its own 2D row position (the host calls once per position); S111 FLATSEQ: by the rows' seq
    if (b.frame != frame_ && !faults_.ignore_stamp) { ++stats_.stale_batches; continue; }
    if (b.first + uint64_t(b.count) > pt.particles.size()) continue;
    rdr::ImageId tex = b.texture == kNoParticleTexture || !texture_of_ ? rdr::kNoImage : texture_of_(b.texture);
    if (tex == rdr::kNoImage) {
      ++stats_.no_texture;
      // S108 PRT: since the namer reads the game's own texture on an index miss, a batch reaching here unnamed is a fault (the game's texture was unreadable: namer guest_failed; or a guest name
      // replayed offline: frame_hooks.cpp says so by key).
      // PRT->AL1 DrawShortfall: rdr::DrawShortfall("particle_texture", b.texture, b.count, 0, "batch of %u particles skipped: no texture", b.count);   (connect after AL1's problem.h lands)
      if (skip_unnamed_) { ++stats_.unnamed_skipped; continue; }   // CAWH: the game always has its texture; a white quad is never right
      tex = white_;
    }
    ++stats_.batches;
    const bool soft = b.soft != 0 && soft_possible;
    if (b.soft && !soft) ++stats_.soft_as_hard;
    if (soft && !depth_copied) {   // the first soft batch: the scene depth as it is now (opaque + the see-through list so far; particles never write depth) -> the sampled copy. The copy ends the pass; the next draw opens one that loads.
      rdr::CopyOp cp{};
      cp.source.image = ctx.depth, cp.dest.image = soft_depth_;
      cp.source_rect = {0, 0, ctx.width, ctx.height};
      cp.dest_rect = cp.source_rect;
      fb.Copy(cp);
      depth_copied = true;
      ++stats_.soft_depth_copies;
    }
    if (soft) ++stats_.soft_drawn;

    rdr::DrawState state{};
    state.program = soft ? soft_program_ : program_;
    state.vertex_layout = soft ? soft_layout_ : layout_;
    state.topology = 3;  // triangle list
    state.cull = 0;      // both faces: a camera-facing sprite's winding is whatever its LocalWorld makes it
    state.front_face = 1;
    state.depth_test = flat ? 0 : 1;   // S111 TITLESMOKE: F (the 2D layer's target) has no depth; the game draws the menu groups with its 2D camera among the 2D rows
    state.depth_write = 0;
    state.depth_compare = ParticleDepthCompare(b);  // S111 PARTZ: the game's per-entry ALWAYS (sub_82A0AED8 +84) stays ALWAYS; its reversed GEQUAL is our forward LEQUAL (live_particles.h)
    state.blend[0].write_mask = 0xF;
    state.blend[0].enable = 1;
    state.blend[0].src_color = 6;  // src alpha
    state.blend[0].dst_color = ((b.blend == kParticleAdditive && !faults_.additive_off) || b.blend == kParticleReverseSubtract) ? 1 : 7;  // one / one minus src alpha
    state.blend[0].color_op = b.blend == kParticleReverseSubtract ? 2 : 0;   // S103 F5: rdr::BlendState is VULKAN numbered (renderer.cpp passes it to VkBlendOp): the setter's Xenos op 4 -> VK_BLEND_OP_REVERSE_SUBTRACT = 2 (the adapter's map {0,1,3,4,2}); 4 would be MAX [L for the Xenos meaning]
    state.blend[0].src_alpha = 1;
    state.blend[0].dst_alpha = 7;
    rdr::TextureSet set{};
    set.slots[0].image = tex;
    uint32_t batch_sampler = sampler;
    if (b.addr >= 1 && b.addr <= 4 && !sampler_table::g_plant_old_samplers) {   // S103 B1: the sprite's address byte b = addr - 1 through the game's wrapper codes {2, 5, 6, 1} (sampler_table.h; T1 1.4); b 0 = clamp = the default above
      const sampler_table::Addr a = sampler_table::AddrFromCode(sampler_table::ParticleAddrCode(unsigned(b.addr - 1)));
      if (a.set) {
        rdr::SamplerDesc bsd = sd;
        bsd.address_u = sampler_table::VkAddress(a.u), bsd.address_v = sampler_table::VkAddress(a.v);
        batch_sampler = fb.Sampler(bsd);
      }
    }
    set.slots[0].sampler = batch_sampler;
    set.slots[0].kind = 0;
    if (soft) {   // texture 1 = the scene depth copy, nearest, clamped
      rdr::SamplerDesc dsd{};
      dsd.mag_filter = dsd.min_filter = 0;
      dsd.mip_mode = 0;
      dsd.address_u = dsd.address_v = dsd.address_w = 2;
      dsd.max_anisotropy = 1;
      dsd.max_lod = 0.f;
      set.slots[1].image = soft_depth_, set.slots[1].sampler = fb.Sampler(dsd), set.slots[1].kind = 0;
    }
    const uint32_t state_id = fb.State(state);
    const uint32_t tex_id = fb.Textures(set);

    uint32_t batch_fs = fs_constants;
    if (soft) {   // pixel constants: FadeOutDistance.x = the range (code / 4); Proj row 2 = (., ., P22 = 1 - A, P23 = -B), so the effect's P23 / ((1 - P22) - depth) is -B / (A - depth)
      float fsb[16] = {};
      fsb[0] = float(b.soft) * 0.25f;
      fsb[4 + 8 + 2] = 1.f - soft_a, fsb[4 + 8 + 3] = -soft_b;
      batch_fs = fb.PushConstants(fsb, sizeof fsb);
    }
    const bool lit = b.use_normal >= 0.9f;
    float* zoff = block + kOffZOffset / 4;
    zoff[0] = b.zoffset;
    block[kOffUseNormal / 4] = b.use_normal;
    std::memcpy(block + kOffLight / 4, b.light, 16);
    if (mirror) block[kOffLight / 4 + 1] = -b.light[1];  // the light vector is a game-space direction

    // Per draw: either one particle (lit: its own LocalWorld) or the whole batch (LocalWorld folded into the vertices, the constants identity).
    const auto emit = [&](uint32_t first, uint32_t count, bool fold) {
      verts.clear();
      verts.reserve(size_t(count) * 6);
      for (uint32_t k = 0; k < count; ++k) {
        const Particle& p = pt.particles[first + k];
        ParticleDrawVertex v4[4];
        for (int i = 0; i < 4; ++i) {
          const ParticleVertex& s = p.v[i];
          ParticleDrawVertex& d = v4[i];
          if (fold) {  // world_k = dot(LocalWorld row k, (pos, 1)): the effect's own product, done here
            for (int r = 0; r < 3; ++r) {
              const float* row = p.local_world + 4 * r;
              d.pos[r] = row[0] * s.pos[0] + row[1] * s.pos[1] + row[2] * s.pos[2] + row[3];
              if (r == 1 && mirror) d.pos[1] = -d.pos[1];
            }
          } else {
            d.pos[0] = s.pos[0], d.pos[1] = s.pos[1], d.pos[2] = s.pos[2];
          }
          d.color = s.color;
          d.uv[0] = s.uv[0], d.uv[1] = s.uv[1];
          d.uv2[0] = s.uv2[0], d.uv2[1] = s.uv2[1];
        }
        static const int kTri[6] = {0, 1, 2, 2, 1, 3};  // the game's strip of four as two triangles
        for (int t = 0; t < 6; ++t) verts.push_back(v4[kTri[t]]);
      }
      float* lw = block + kOffLocalWorld / 4;
      if (fold) {
        const float ident[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::memcpy(lw, ident, sizeof ident);
      } else {
        std::memcpy(lw, pt.particles[first].local_world, 12 * sizeof(float));
        lw[12] = 0, lw[13] = 0, lw[14] = 0, lw[15] = 1;  // c231 is (0, 0, 0, 1) in every draw measured
        if (mirror) {  // world y mirrored: the y output row (c229) and the sprite normal's y (c230.y) change sign
          for (int i = 4; i < 8; ++i) lw[i] = -lw[i];
          lw[9] = -lw[9];
        }
      }
      rdr::DrawOp op{};
      op.state = state_id;
      op.dynamic = dyn;
      op.textures = tex_id;
      if (soft) {   // the soft vertex block: ViewProj, LocalWorld, LocalWorldView (only row 2 is read: the sprite's view distance = clip.w = c3 . world), ZOffset, UseNormal, Light
        float sb[60] = {};
        std::memcpy(sb, block, 32 * sizeof(float));
        for (int k = 0; k < 4; ++k) sb[32 + 8 + k] = k == 3 ? c3[3] : 0.f;   // row 2: starts as c3_3 * (0, 0, 0, 1)
        for (int r = 0; r < 3; ++r)
          for (int k = 0; k < 4; ++k) sb[32 + 8 + k] += c3[r] * lw[4 * r + k];
        sb[32 + 12 + 3] = 1.f;                                          // row 3
        std::memcpy(sb + 48, block + kOffZOffset / 4, 12 * sizeof(float));   // ZOffset, UseNormal, Light
        op.vs_constants = fb.PushConstants(sb, sizeof sb);
      } else {
        op.vs_constants = fb.PushConstants(block, kBlockBytes);
      }
      op.fs_constants = batch_fs;
      op.vertex[0] = {rdr::kFrameGeometry, fb.PushGeometry(verts.data(), uint32_t(verts.size() * sizeof(ParticleDrawVertex)), 4)};
      op.count = uint32_t(verts.size());
      op.instances = 1;
      fb.Draw(op, rdr::kGrpParticle);   // S103 GSTP
      ++stats_.draws;
    };
    if (lit) {
      for (uint32_t k = 0; k < b.count; ++k) emit(b.first + k, 1, false);
      stats_.lit_particles += b.count;
    } else {
      emit(b.first, b.count, true);
    }
    stats_.particles += b.count;
    any = true;
  }
  if (any) ++(flat ? stats_.flat_groups : stats_.frames);
}

std::string LiveParticles::CensusLine() const {
  char line[512];
  std::snprintf(line, sizeof line, "[scene][particles] frames %llu, batches %llu, particles %llu, draws %llu, stale %llu, soft_as_hard %llu, no_texture %llu, lit %llu, viewer_space_mirrored %llu, particles_unnamed_skipped %llu, soft_drawn %llu, soft_depth_copies %llu, flat_groups %llu",
                (unsigned long long)stats_.frames, (unsigned long long)stats_.batches, (unsigned long long)stats_.particles, (unsigned long long)stats_.draws,
                (unsigned long long)stats_.stale_batches, (unsigned long long)stats_.soft_as_hard, (unsigned long long)stats_.no_texture, (unsigned long long)stats_.lit_particles, (unsigned long long)stats_.viewer_space_mirrored, (unsigned long long)stats_.unnamed_skipped, (unsigned long long)stats_.soft_drawn, (unsigned long long)stats_.soft_depth_copies, (unsigned long long)stats_.flat_groups);
  return line;
}

}  // namespace rdr_scene
