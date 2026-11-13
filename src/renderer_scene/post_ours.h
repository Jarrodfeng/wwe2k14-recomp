// Our own post chain, drawn in the game's frame (Session 101, lane PO, PO1; docs\renderer\POST_OURS.md, POST_PROMOTION_MAP.md sections 2, 4, 5).
// PostOurs is the binder's PostHook (src\renderer_adapter\post_hook.h): the frame's post draws are read by structure (post_plan.h, decided by post_ours_core.h); from the last
// depth-of-field layer's dof draw to the chain's end the stream's draws, the copies they feed and the clears of their targets are left out, and rdr_scene::PostPass records OUR chain
// there: reading the images that dof draw declares (the scene colour and depth copies, after the particles), writing the image the stream's combine writes, so the HUD and the copy
// to the presented image run unchanged. Constants, tap tables and filters come from the stream's own draws. A frame whose chain the plan cannot read stays the stream's.
// No game or SDK includes. Render thread only (the one that binds frames).
#pragma once
#include <cstdint>
#include <string>

#include "../renderer/renderer.h"
#include "../renderer_adapter/post_hook.h"
#include "post_ours_core.h"
#include "post_pass.h"

namespace rdr_scene {

struct PostOursOptions {
  PostFaults faults;
  PostLimits limits;
};

class PostOurs final : public rdr_adapter::PostHook {
 public:
  PostOurs(rdr::Renderer& renderer, const PostOursOptions& options);
  ~PostOurs() override;
  PostOurs(const PostOurs&) = delete;
  PostOurs& operator=(const PostOurs&) = delete;

  bool PlanFrame(const rdr_adapter::PostPlanIn& in, rdr_adapter::PostClaim& claim) override;
  uint32_t Inject(rdr_adapter::PostInject& in) override;
  void NoPostFrame() override { ++no_chain_frames_; }
  void PersistOnlyFrame() override { ++persist_only_frames_; }   // S101 lane 224
  std::string Census(uint64_t dropped_draws, uint64_t dropped_copies, uint64_t dropped_clears, uint64_t dropped_targets) const override;

  // The numbers the offline check reads (also in the census line).
  struct Tally {
    uint64_t ours = 0, planted_unchecked = 0, combine_only = 0;   // lane 224: combine_only = ours frames that were the combine alone (also counted in shapes {match})
    uint64_t shapes[uint8_t(PostShape::Other) + 1] = {};
    uint64_t kept[uint8_t(PostKeep::Count)] = {};
    uint64_t dead_draws = 0, dead_copies = 0, bloom0 = 0, glare0 = 0, dof0 = 0;
    uint64_t passes = 0;
    uint64_t t2_far = 0;   // lane 296b: ours frames that were the two-layer t2 form (also counted in shapes {film})
  };
  const Tally& tally() const { return tally_; }

 private:
  rdr::ImageId NullImage();
  rdr::ImageId FixedImage(uint32_t w, uint32_t h);

  rdr::Renderer& renderer_;
  PostOursOptions options_;
  PostPass pass_;
  PostDecision decision_;
  Tally tally_;
  uint64_t no_chain_frames_ = 0;
  uint64_t persist_only_frames_ = 0;   // S101 lane 224: lasting-work binds of skipped frames (not planned, not fallbacks)
  uint32_t refused_named_ = 0;    // lane 140: the log cap
  bool refused_capped_ = false;
  std::string refused_seen_;      // sequences already logged
  bool said_record_error_ = false;
  bool said_t2_far_ = false;   // lane 296b
  rdr::ImageId null_ = rdr::kNoImage, fixed_ = rdr::kNoImage;
  uint32_t fixed_w_ = 0, fixed_h_ = 0;
};

}  // namespace rdr_scene
