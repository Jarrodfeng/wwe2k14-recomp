// Our particles, drawn from the scene record (Session 103, lane F; docs\renderer\SEE_THROUGH_ORDER.md section 5). A StageHook at Stage::AfterBlended:
// sparks, pyro, smoke, confetti and fire come from the record's particle tables (particle_tables.h), never from the stream, and are drawn with the
// hand-written effect yParticle (src\renderer\shaders\fx\yParticle.*) into the colour pass, depth test on, depth write off, after the see-through list.
//
// Threads: every call on the render thread. No game, no SDK includes.
//
// Lighting: a batch with use_normal >= 0.9 is lit by its sprite normal, which the effect takes from each particle's own LocalWorld, so such a batch is drawn
// one particle at a time with that LocalWorld (as the game does); every other batch has its LocalWorld folded into the vertices on the CPU and is ONE draw.
// Soft particles (S103 PRT, PNS.md 4): a batch whose `soft` code is non-zero (the game's global switch [0x836EE38C] was on at the node's call; range = code / 4) is drawn with the ySoftParticle_base programs,
// fading by the gap to OUR scene depth. The renderer cannot sample the depth while it is the pass's attachment (Problem sampled_while_attached), so the first soft batch of a frame copies the scene depth
// into a sampled depth image (the copy ends the pass; the next draw opens one that loads) and every soft batch samples that copy as texture 1. The depth is linearised with OUR projection: with clip.z = A * w + B
// (read from the frame's view-projection), scene distance = -B / (A - depth); the effect's own formula P23 / ((1 - P22) - depth) takes P22 = 1 - A, P23 = -B. The sprite's view distance is clip.w (LocalWorldView row 2 = row 3 of the VP).
// A frame whose view-projection is not a perspective one, or with no scene depth, draws the batch hard (`soft_as_hard`, as before). Open [U]: the game's flat-sprite quirk (a sprite lying flat reports view z 0) is not reproduced.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "front_end.h"
#include "particle_tables.h"
#include "stage_hook.h"

namespace rdr_scene {

// S103 PRT fix 2: --wwe2k14_rdr_soft_particles (default on; start_settings.cpp stores it). Off = soft batches are drawn hard (the old picture), no depth copy, no non-transient depth.
inline std::atomic<bool> g_soft_particles{true};

// Planted faults (the acceptance test's twins; each must fail its exact check).
struct ParticleFaults {
  bool additive_off = false;   // an additive batch is drawn with plain alpha blend
  bool ignore_stamp = false;   // a batch stamped for another frame is drawn anyway (the stale-particles fault)
  bool no_mirror = false;      // a viewer-space frame is drawn without the Y mirror (the picture comes out upside down)
  bool Any() const { return additive_off || ignore_stamp || no_mirror; }
};

// S111 PARTZ: a batch's depth compare, in VkCompareOp numbers (renderer.cpp hands depth_compare to VkCompareOp unchanged: 3 = LESS_OR_EQUAL, 7 = ALWAYS). The game draws an entry whose word +220 is 0 with
// ALWAYS (sub_82A0AED8 +84: recorder code 1, which sub_82C237D8 records as hardware compare 7); every other entry with its reversed GEQUAL, our forward LEQUAL (pass_state.h ForwardZ). Depth write stays off for both.
inline constexpr uint8_t kParticleCompareLessOrEqual = 3, kParticleCompareAlways = 7;
inline uint8_t ParticleDepthCompare(const ParticleBatch& b) { return b.depth_always ? kParticleCompareAlways : kParticleCompareLessOrEqual; }

struct LiveParticlesStats {
  uint64_t frames = 0;              // frames the hook drew particles in
  uint64_t batches = 0, particles = 0, draws = 0;
  uint64_t stale_batches = 0;       // batches stamped for another frame: not drawn
  uint64_t soft_as_hard = 0;        // soft batches drawn with the hard effect (no depth to sample / not a perspective view-projection / the soft programs missing)
  uint64_t soft_drawn = 0;          // S103 PRT: soft batches drawn with the soft effect
  uint64_t soft_depth_copies = 0;   // S103 PRT: scene-depth copies made for them (at most one a frame)
  uint64_t no_texture = 0;          // a batch whose texture the lookup could not give: the white image stands in
  uint64_t unnamed_skipped = 0;     // CAWH: batches with no texture that were NOT drawn (SetSkipUnnamed); printed as particles_unnamed_skipped
  uint64_t lit_particles = 0;      // particles drawn one at a time (use_normal >= 0.9)
  uint64_t viewer_space_mirrored = 0;  // frames drawn in the viewer's space (the game-space tables are mirrored by S = diag(1, -1, 1))
};

struct ParticleDrawVertex {  // the census's stride-32 layout, as the effect's inputs read it
  float pos[3];
  uint32_t color;
  float uv[2];
  float uv2[2];
};
static_assert(sizeof(ParticleDrawVertex) == 32, "particle draw vertex");

class LiveParticles final : public StageHook {
 public:
  // `texture_of(asset_index)`: the image of the particle texture the record's asset table names (render thread); kNoImage = not resident (a white stand-in is used, counted).
  using TextureLookup = std::function<rdr::ImageId(uint32_t asset_index)>;
  LiveParticles(rdr::Renderer& renderer, TextureLookup texture_of, const ParticleFaults& faults = ParticleFaults());
  ~LiveParticles() override;
  // True when the tables hold a batch drawn soft AND the soft switch is on (the front-end then keeps the scene depth stored: a non-transient image; with the switch off nothing changes).
  static bool HasSoft(const ParticleTables* pt) {
    if (!pt || !g_soft_particles.load(std::memory_order_relaxed)) return false;
    for (const ParticleBatch& b : pt->batches)
      if (b.soft) return true;
    return false;
  }
  LiveParticles(const LiveParticles&) = delete;
  LiveParticles& operator=(const LiveParticles&) = delete;

  // The guest frame the record being drawn was captured in (its header's guest_frame, low 32 bits). A batch stamped for another frame is not drawn.
  void SetFrame(uint32_t guest_frame) { frame_ = guest_frame; }
  // S103 F5: the lookup is replaced per frame (frame_hooks.cpp: it reads the record's asset table); render thread.
  void SetTextureLookup(TextureLookup t) { texture_of_ = std::move(t); }
  // CAWH: true = a batch with no texture is not drawn (counted particles_unnamed_skipped) instead of drawn as a white quad (the default stays the white stand-in for the unit tests and the rig).
  void SetSkipUnnamed(bool s) { skip_unnamed_ = s; }
  const char* Name() const override { return "particles"; }
  void Record(Stage stage, const StageCtx& ctx, rdr::FrameBuilder& builder) override;
  const LiveParticlesStats& stats() const { return stats_; }
  // "[scene][particles] frames N, batches B, particles P, draws D, stale S, soft_as_hard H, no_texture T, lit L": the census line (counters only).
  std::string CensusLine() const;

 private:
  bool EnsurePrograms();
  bool EnsureSoftPrograms();
  bool EnsureSoftDepth(uint32_t w, uint32_t h, bool scaled);
  bool SceneDepthScaled(const StageCtx& ctx) const;   // S105 A1
  rdr::Renderer& renderer_;
  TextureLookup texture_of_;
  ParticleFaults faults_;
  uint32_t frame_ = 0;
  uint16_t program_ = 0, layout_ = 0;
  bool ready_ = false, failed_ = false, skip_unnamed_ = false;
  rdr::ImageId white_ = rdr::kNoImage;
  uint16_t soft_program_ = 0, soft_layout_ = 0;
  bool soft_ready_ = false, soft_failed_ = false;
  rdr::ImageId soft_depth_ = rdr::kNoImage;   // the scene depth's copy the soft batches sample
  uint32_t soft_w_ = 0, soft_h_ = 0;
  bool soft_scaled_ = false;   // S105 A1: the soft depth follows the scene depth's kUsageScaled (a scaled -> unscaled depth copy is unsupported)
  LiveParticlesStats stats_;
  std::vector<ParticleDrawVertex> verts_;  // reused every frame (no per-frame allocation)
};

}  // namespace rdr_scene
