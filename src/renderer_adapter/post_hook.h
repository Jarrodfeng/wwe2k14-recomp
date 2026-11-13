// The post chain's promotion, PO1 (Session 101, lane PO; docs\renderer\POST_PROMOTION_MAP.md sections 2, 4, 7): the seam between the binder's walk of a replayed
// frame and the code that draws the post chain (depth of field, bloom, glare, the combine, the 30 Years film pass) as OUR pass. Modelled on crowd_hook.h, with one
// addition: the draws the hook takes over are not found by a call site but by the structure of the frame (post_plan.h), and the copies the claimed draws feed go too.
//
// The protocol, once per Bind of a frame:
//   1. PlanFrame(in, claim): the hook reads the frame's post draws (the whole frame is there), decides whether it knows the chain's shape, and answers which draws
//      it claims (per frame draw index), which ops it drops (copies the claimed draws feed, clears of their targets, the passes' target switches), where the
//      chain begins (the last depth-of-field layer's dof draw: ours is recorded THERE) and where it ends (the combine, or the film pass after it). Not ok: nothing
//      is claimed, the stream's chain draws as before (a fallback frame, counted by reason).
//   2. At the first claimed draw the binder calls Inject: the hook appends its whole chain to `in.builder`, reading the images that dof draw declares (not the scene
//      target: the particles and, in a two-layer entrance, the wrestlers sit in between) and writing the image the stream's combine targets, so the HUD draws after it
//      and the copy to the presented image run unchanged. Every other claimed draw is dropped.
//   3. At the last claimed draw the binder sets what the stream's combine did to the rest of the frame (the HUD marker `after_post_`, the DLSS hand-over).
// No game or SDK includes.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../renderer/api.h"
#include "../renderer/frame_builder.h"
#include "capture_reader.h"

namespace rdr_adapter {

// The switch: default off on PC and phone (live: --wwe2k14_rdr_post_ours; fview / rplay: --post-ours).
inline bool g_post_ours = false;

// What the binder knows about an image of the frame: its size in the game's pixels, whether it follows the render scale (kUsageScaled), and whether the renderer has it.
struct PostImageInfo {
  uint32_t width = 0, height = 0;
  bool scaled = false;
  bool known = false;     // the size is known (a capture / the live table has the record)
};

struct PostPlanIn {
  const LoadedFrame* frame = nullptr;
  std::function<PostImageInfo(uint32_t image)> info;           // by capture image id
  std::function<rdr::ImageId(uint32_t image)> map;             // capture image id -> the renderer's image (kNoImage: none)
  bool draw_origins = false;                                   // the frame carries draw origins (informational)
  // The player's settings the stream applies by swapping the draws' variants (binder.h): DEPTH OF FIELD off draws the combine without its dof permutation (the dof + hole_fill passes
  // stay when the bloom reads them), FILM GRAIN off draws the 30 Years filter clean. Ours takes them as plan flags instead, with the same picture.
  bool depth_of_field = true, film_grain = true;
};

// The hook's answer for one frame. Vectors are sized to the frame's draws / ops when ok.
struct PostClaim {
  bool ok = false;
  bool no_chain = false;          // not ok because the frame has no post draws at all (a load screen, a menu): not a fallback frame
  std::vector<uint8_t> draw;      // per frame draw: 1 = claimed (dropped, the first one is where Inject runs)
  // per frame op: 0 = kept, 1 = left out (a copy a claimed draw feeds, a colour-only clear of a claimed target, a target switch of a claimed pass),
  // 2 = a clear whose colour part is stripped (a clear of the combine's target that also clears depth / stencil: the depth part is the HUD's and stays)
  std::vector<uint8_t> drop;
  uint32_t first_draw = 0;        // frame draw index of the Inject point
  uint32_t last_draw = 0;         // frame draw index of the chain's end (after it: after_post_)
  uint32_t draws = 0, copies = 0, clears = 0, targets = 0;   // counts of what is claimed / dropped (clears: dropped + colour-stripped)
  uint32_t depth_clears_dropped = 0;   // clears with depth / stencil bits dropped whole: must stay 0 (the planted drop_depth_clear makes it 1)
  uint32_t copies_kept = 0;            // copies from a claimed target that no claimed draw reads: kept
};

// What Inject hands the hook: the builder, the binder's image table.
struct PostInject {
  const LoadedFrame* frame = nullptr;
  uint32_t draw_index = 0;
  rdr::FrameBuilder* builder = nullptr;
  const rdr::ImageId* images = nullptr;   // capture image id -> renderer image
  uint32_t image_count = 0;
};

class PostHook {
 public:
  virtual ~PostHook() = default;
  // Once per Bind of a frame (not for a velocity or isolate pass). True: `claim` is filled and the binder follows it. False: the stream's chain stays (`claim` is cleared).
  virtual bool PlanFrame(const PostPlanIn& in, PostClaim& claim) = 0;
  // At the first claimed draw. Appends the chain to in.builder. Returns the number of passes recorded. 0: nothing was appended and the frame falls back: the binder draws this
  // draw and every later claimed draw / op the stream's way (everything the claim drops lies after the Inject point, so the frame stays exactly as the stream drew it).
  virtual uint32_t Inject(PostInject& in) = 0;
  // A bound frame with no post chain at all (a load screen, a menu).
  virtual void NoPostFrame() {}
  // S101 lane 224: a bind that is a skipped frame's lasting work (never shown, its per-frame copies left out): no plan is made, the stream keeps its draws. Not a fallback.
  virtual void PersistOnlyFrame() {}
  // The census line, from the binder's counters ("[rdr][post] ours frames N, fallback F, ...").
  virtual std::string Census(uint64_t dropped_draws, uint64_t dropped_copies, uint64_t dropped_clears, uint64_t dropped_targets) const = 0;
};

// The binder's own numbers (the hook keeps the reasons and the shapes).
struct PostCounters {
  uint64_t ours_frames = 0;       // frames whose chain we drew
  uint64_t fallback_frames = 0;   // frames with post draws where the stream's chain stayed (the hook counts the reasons)
  uint64_t draws_dropped = 0;     // stream post draws left out
  uint64_t copies_dropped = 0;    // copies the claimed draws fed, left out
  uint64_t clears_dropped = 0;    // clears of claimed targets (colour stripped or dropped), left out
  uint64_t targets_dropped = 0;   // target switches of claimed passes, left out
  double ms_ours = 0;             // time inside PlanFrame and Inject
};

}  // namespace rdr_adapter
