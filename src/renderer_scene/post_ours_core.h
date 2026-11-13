// The post chain's promotion PO1, the GPU-free half (Session 101, lane PO; docs\renderer\POST_PROMOTION_MAP.md sections 2, 4, 5; docs\renderer\POST_OURS.md).
// A frame's post draws -> the decision: is this chain ours (the plan of post_plan.h, then the checks below), which stream draws / copies / clears / target switches
// are dropped, what the pass reads and writes. No GPU, no game or SDK includes: tools\post\post_ours_core_test.cpp runs it over every frame of the captures.
//
// Frames are recognised by STRUCTURE (the plan), never by a shader hash. The checks (each one has a planted twin, PostFaults, that fails exactly it):
//   shape         the plan knows the chain's variant sequence             (PostKeep::Shape / Permutation / Slot)
//   chain_end     the chain's last draw was found structurally             (plant last_pass_by_hash)
//   null_input    no chain fetch reads image 0, the dof's inputs are real  (plant null_input, the S100 07:15 / 11:39 fault)
//   inputs        the combine reads the dof's colour and depth (the pass uses one image for both)
//   filter        the dof depth filter equals the stream's own sampler     (plants dof_depth_filter, r32f_point)
//   scaled        at a render scale above 1 the pass's own images follow it (plant unscaled_steps, the S100 13:37 ScreenShaped family)
//   texel         the combine reads a picture of the extent it writes      (plant fixed_texel, lane 34's review: a 1/1280 texel at scale 2)
//   interleaved   no stream draw sits between the claimed ones
//   reads         no unclaimed draw after the chain binds an image a dropped copy / claimed pass made
//   combine       (the hook, post_ours.cpp) the recorded chain holds the combine (plant no_combine, S100 07:25 "the washed-out picture")
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../renderer_adapter/post_hook.h"
#include "post_plan.h"

namespace rdr_scene {

enum class PostKeep : uint8_t { Shape, Permutation, Slot, NullInput, ChainEnd, Filter, Unscaled, Texel, Interleaved, ReadsChain, NotReady, Combine, DepthClear, WritesInputs, Count };
const char* PostKeepName(PostKeep k);   // "shape", "permutation", "slot", "null_input", "chain_end", "filter", "unscaled", "texel", "interleaved", "reads_chain", "not_ready", "combine", "depth_clear", "writes_inputs"

// Planted faults (the acceptance test's twins). checks_off: the checks do not stop the frame, so the run shows how far the picture moves.
struct PostFaults {
  bool null_input = false, no_combine = false, dof_depth_filter = false, fixed_texel = false, unscaled_steps = false, r32f_point = false, last_pass_by_hash = false;
  // S101 lane PO review: drop_depth_clear (any clear of a claimed target goes, depth bits too: the old behaviour that lost the 30 Years HUD's depth clear), dof_depth_address (the dof depth slot takes the
  // STREAM's address mode: REPEAT in 30 Years / the match; proves the scale-2 difference is that and nothing else; not a fault, no check fails), inject_fail_late (one pass is recorded, then the injection
  // fails: the frame must equal the stream), writes_input_inside (a kept copy inside the range writes the pass's colour input), interleaved / reads_chain (a claimed draw is left to the stream; the
  // twins of those two checks), drop_out_targets (the combine's own target switch is dropped too: the HUD then lands on the wrong attachments).
  bool drop_depth_clear = false, dof_depth_address = false, inject_fail_late = false, writes_input_inside = false, interleaved = false, reads_chain = false, drop_out_targets = false;
  // lane 140: filter_forced_point (our pass ignores the stream's LINEAR dof depth filter: the plan's rule is overridden to point) and shape_misclassified (a frame with a t2 blur is
  // treated as having the hole_fill chain's inputs: the t2's source is taken for the dof's colour). Each fails its check; with ,nocheck the picture moves.
  // (review F8: filter_forced_point was a duplicate of dof_depth_filter and is gone) old_restore_rule (the plan's depth filter = restores == 0, the rule before the stream's sampler was followed), t2_source_stale (the t2 source is
  // the dropped copy's destination), t2_slots_foreign (the slot 2 / 6 writer check skipped, slot 6 fed another image), t2_default_tables (the t2's tables from DefaultPostParams: only the picture sees it),
  // claim_from_restore (the claim starts at the copy_depth in front of the dof: the writes / reads checks refuse)
  bool old_restore_rule = false, t2_source_stale = false, t2_slots_foreign = false, t2_default_tables = false, claim_from_restore = false, shape_misclassified = false;
  // lane 238: merge_filter_linear (the m1_m2 merge binds every slot linear whatever the stream does: run 7131's slot 3 POINT), t2_tables_shared (the DoF blur takes the bloom t2's tables: the victory scene's two t2 draws)
  bool merge_filter_linear = false, t2_tables_shared = false, film_dropped = false, dead_filters_checked = false;   // dead_filters_checked: the old rule, the filter check also reads draws our pass does not record (run 7131's m1_m2 slot 3)
  // (film_dropped: the chain's claimed film draw is dropped and the pass does not record it)
  // lane 277: t2_needs_dof (the plan takes a leading t2 as the DoF blur only when the combine has a dof permutation: the rule before lane 277, run 7183's 'chain draws before the known sequence (t2)')
  bool t2_needs_dof = false;
  // lane 296: t2_one_only (the plan takes at most ONE t2 in front of the HDR chain: the rule before lane 296, run 7201's 'chain draws before the known sequence (t2)' on copy_depth t2 copy_depthx2 t2 ...)
  bool t2_one_only = false;
  // lane 296: t2_far_ignored (planted: the far layer's blur image is not passed on, the pass feeds slot 6 its own blur: the layers merge; no check sees it, the picture does)
  bool t2_far_ignored = false;
  // lane 296b: far_copy_unchecked (the rule before the SF1 fix: a far-blur copy inside the claimed range is neither refused by the plan nor counted as a write by the core)
  bool far_copy_unchecked = false;
  bool checks_off = false;
  bool Any() const {
    return null_input || no_combine || dof_depth_filter || fixed_texel || unscaled_steps || r32f_point || last_pass_by_hash || t2_default_tables || old_restore_rule || t2_source_stale || t2_slots_foreign || claim_from_restore || shape_misclassified || drop_depth_clear || inject_fail_late || writes_input_inside || interleaved || reads_chain ||
           drop_out_targets || merge_filter_linear || t2_tables_shared || film_dropped || dead_filters_checked || t2_needs_dof || t2_one_only || t2_far_ignored || far_copy_unchecked;   // (dof_depth_address is an experiment, not a fault)
  }
  // "null_input,nocheck": a comma list of the names above and "nocheck". False + error for an unknown name.
  static bool Parse(const std::string& list, PostFaults& out, std::string& error);
};

// The run facts the checks need (live: render_scale from the options; fview / rplay: --scale and --post-limits). no_r32f_linear is TEST-ONLY: live, a device that cannot filter R32F makes the renderer drop the depth sampler to point for the stream AND for ours (renderer.cpp, every texture binding), so the picture equals the stream's and nothing needs refusing.
struct PostLimits {
  bool no_r32f_linear = false;   // the device cannot filter the R32F depth copy: a frame whose depth is read linearly stays the stream's (the renderer would drop it to point)
  float render_scale = 1.0f;
};

struct PostDecision {
  bool ok = false;               // the chain is ours (also true under checks_off when a check failed: failed says so)
  bool no_chain = false;         // the frame has no post draws
  bool failed = false;           // a check failed (the first one is keep / detail)
  PostKeep keep = PostKeep::Shape;
  std::string detail;
  PostShape shape = PostShape::None;
  std::string sequence;          // the stream chain's variant sequence (run-length)
  PostPlan plan;
  rdr_adapter::PostClaim claim;
  uint32_t colour = 0, depth = 0, out = 0, far_blur = 0;  // capture image ids: what the dof reads, the combine's target, slot 6's earlier layer (0: none)
  PostParams params;             // the effective parameters the pass is given
  bool null_depth = false, fixed_texel_copy = false, no_combine = false, inject_fail_late = false;   // planted-fault instructions for the hook
  uint32_t slot_repeat = 0;   // chain slots the stream binds with a non-clamp address mode (informational: the pass clamps; see POST_OURS.md)
  uint32_t dead_draws = 0, dead_copies = 0;   // dead work not recorded (draws / copies that the stream does and ours does not)
  bool bloom0 = false, glare0 = false, dof0 = false;
  bool t2_default_tables = false;  // lane 140 planted: the hook takes the t2 tables from DefaultPostParams (no check sees it; the picture does)
  bool dof_depth_repeat = false;   // lane 140: the claimed dof's depth slot is bound with a non-clamp address (the scale-2 edge band applies to these frames only)
  bool restores_before = false;    // lane 140: a copy_depth draw sits in front of the claim
  uint32_t blur_src = 0;   // lane 140: plan.t2_blur: the capture image the t2 blur reads (the stream's own quarter-size picture; it stays the stream's)
  uint32_t stream_chain_draws = 0;   // the stream chain's draws in the frame (the restores and an earlier layer are among those that stay)
};

// The decision for one frame. `in.info` and `in.map` are required (map may answer kNoImage for an image that does not exist; that is a NotReady frame).
PostDecision DecidePost(const rdr_adapter::PostPlanIn& in, const PostFaults& faults, const PostLimits& limits);

}  // namespace rdr_scene
