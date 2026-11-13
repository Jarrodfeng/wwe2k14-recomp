// Session 101, lane PO: the post chain's promotion, the GPU-free half. See post_ours_core.h.
#include "post_ours_core.h"

#include <cstdio>
#include <cstring>
#include <unordered_set>

namespace rdr_scene {

const char* PostKeepName(PostKeep k) {
  static const char* const n[] = {"shape", "permutation", "slot", "null_input", "chain_end", "filter", "unscaled", "texel", "interleaved", "reads_chain", "not_ready", "combine", "depth_clear", "writes_inputs"};
  return uint8_t(k) < uint8_t(PostKeep::Count) ? n[uint8_t(k)] : "?";
}

bool PostFaults::Parse(const std::string& list, PostFaults& out, std::string& error) {
  out = PostFaults{};
  size_t at = 0;
  while (at <= list.size()) {
    size_t comma = list.find(',', at);
    if (comma == std::string::npos) comma = list.size();
    const std::string name = list.substr(at, comma - at);
    at = comma + 1;
    if (name.empty()) continue;
    if (name == "null_input") out.null_input = true;
    else if (name == "no_combine") out.no_combine = true;
    else if (name == "dof_depth_filter") out.dof_depth_filter = true;
    else if (name == "fixed_texel") out.fixed_texel = true;
    else if (name == "unscaled_steps") out.unscaled_steps = true;
    else if (name == "r32f_point") out.r32f_point = true;
    else if (name == "last_pass_by_hash") out.last_pass_by_hash = true;
    else if (name == "drop_depth_clear") out.drop_depth_clear = true;
    else if (name == "dof_depth_address") out.dof_depth_address = true;
    else if (name == "inject_fail_late") out.inject_fail_late = true;
    else if (name == "writes_input_inside") out.writes_input_inside = true;
    else if (name == "interleaved") out.interleaved = true;
    else if (name == "reads_chain") out.reads_chain = true;
    else if (name == "drop_out_targets") out.drop_out_targets = true;
    else if (name == "old_restore_rule") out.old_restore_rule = true;   // lane 140
    else if (name == "t2_source_stale") out.t2_source_stale = true;
    else if (name == "t2_slots_foreign") out.t2_slots_foreign = true;
    else if (name == "t2_default_tables") out.t2_default_tables = true;
    else if (name == "claim_from_restore") out.claim_from_restore = true;
    else if (name == "shape_misclassified") out.shape_misclassified = true;   // lane 140
    else if (name == "merge_filter_linear") out.merge_filter_linear = true;   // lane 238
    else if (name == "t2_tables_shared") out.t2_tables_shared = true;
    else if (name == "film_dropped") out.film_dropped = true;
    else if (name == "t2_needs_dof") out.t2_needs_dof = true;   // lane 277
    else if (name == "t2_one_only") out.t2_one_only = true;     // lane 296
    else if (name == "t2_far_ignored") out.t2_far_ignored = true;   // lane 296
    else if (name == "far_copy_unchecked") out.far_copy_unchecked = true;   // lane 296b: the rule before the SF1 fix (neither the plan nor the core sees a far copy inside the claim)
    else if (name == "dead_filters_checked") out.dead_filters_checked = true;
    else if (name == "nocheck") out.checks_off = true;
    else {
      error = "unknown --post-fault '" + name + "' (null_input, no_combine, dof_depth_filter, fixed_texel, unscaled_steps, r32f_point, last_pass_by_hash, drop_depth_clear, dof_depth_address, inject_fail_late, writes_input_inside, interleaved, reads_chain, drop_out_targets, old_restore_rule, t2_source_stale, t2_slots_foreign, t2_default_tables, claim_from_restore, shape_misclassified, merge_filter_linear, t2_tables_shared, film_dropped, dead_filters_checked, t2_needs_dof, t2_one_only, t2_far_ignored, far_copy_unchecked, nocheck)";
      return false;
    }
  }
  return true;
}

namespace {

// Records the first failed check; true = stop (the checks are on).
// The filter the pass binds at each chain slot (post_pass.cpp Record), by the stream draw's kind: 1 linear, 0 point, -1 not compared. Source: the map's 1.3 and PostPass::Record.
int ExpectedFilter(ChainKind kind, int slot, const bool* merge_linear) {   // lane 238: merge_linear = what the pass binds on the m1_m2 merge's slots
  switch (kind) {
    case ChainKind::Dof: return slot == 0 ? 1 : -1;
    case ChainKind::HoleFill: case ChainKind::Box4Down: case ChainKind::T2: case ChainKind::T12: return slot == 0 ? 0 : -1;
    case ChainKind::T1: return slot <= 3 ? 1 : -1;
    case ChainKind::M1M2: return slot <= 3 ? (merge_linear[slot] ? 1 : 0) : -1;
    case ChainKind::Combine: return slot == 3 ? 0 : (slot == 0 || slot == 2 || slot == 6 || slot == 7 || slot == 8) ? 1 : -1;
    default: return -1;
  }
}

struct Gate {
  PostDecision& d;
  const PostFaults& faults;
  bool Stop(PostKeep k, const std::string& why) {
    if (!d.failed) d.failed = true, d.keep = k, d.detail = why;
    return !faults.checks_off;
  }
};

}  // namespace

PostDecision DecidePost(const rdr_adapter::PostPlanIn& in, const PostFaults& faults, const PostLimits& limits) {
  PostDecision d;
  Gate gate{d, faults};
  const rdr_adapter::LoadedFrame& f = *in.frame;
  StreamChain sc;
  std::string error;
  const ChainImageSize size = [&in](uint32_t id, uint32_t& w, uint32_t& h) {
    const rdr_adapter::PostImageInfo i = in.info(id);
    if (!i.known) return false;
    w = i.width, h = i.height;
    return true;
  };
  if (!ReadStreamChain(size, f, sc, error)) {
    d.keep = PostKeep::Shape, d.detail = "the frame's post draws could not be read: " + error, d.failed = true;
    return d;
  }
  if (sc.draws.empty()) {
    d.no_chain = true;
    return d;
  }
  const PlanPlant saved = g_plan_plant;
  g_plan_plant = faults.last_pass_by_hash ? PlanPlant::LastPassByHash : faults.old_restore_rule ? PlanPlant::OldRestoreRule : faults.t2_source_stale ? PlanPlant::T2SourceStale :
                 faults.t2_slots_foreign ? PlanPlant::T2SlotsForeign : faults.t2_needs_dof ? PlanPlant::T2NeedsDof : faults.t2_one_only ? PlanPlant::T2OneOnly : faults.far_copy_unchecked ? PlanPlant::FarCopyUnchecked : PlanPlant::None;
  d.stream_chain_draws = uint32_t(sc.draws.size());
  d.plan = BuildPostPlan(sc);
  g_plan_plant = saved;
  const PostPlan& plan = d.plan;
  d.sequence = plan.sequence, d.shape = plan.shape;
  d.failed = false;
  if (!plan.ok) {  // nothing to do with a chain the plan does not read: always the stream's
    const std::string& r = plan.refusal;
    d.failed = true, d.detail = r;
    d.keep = r.find("not ours yet") != std::string::npos ? PostKeep::Permutation : r.find("slot") != std::string::npos ? PostKeep::Slot : PostKeep::Shape;
    return d;
  }
  const std::vector<ChainDraw>& D = sc.draws;
  if (!plan.chain_end_found && gate.Stop(PostKeep::ChainEnd, "the chain's last draw was not found")) return d;
  if (plan.null_inputs && gate.Stop(PostKeep::NullInput, std::to_string(plan.null_inputs) + " chain fetch slots read image 0")) return d;

  // the claimed range: the last depth-of-field layer's dof through the chain's end
  const bool no_layer = plan.t2_blur || plan.combine_only;   // lane 224: the combine alone has no dof layer either
  const uint32_t first = no_layer ? plan.claim_first : plan.layers.back().dof, last = plan.chain_last;   // (lane 238: claim_first = the t2 draw / the combine; restores in front of it stay)   // lane 140: t2_blur has no layer (the claimed range starts at the t2 draw)
  if (last < first) {
    if (gate.Stop(PostKeep::ChainEnd, "the chain's end lies before its start")) return d;
    d.keep = PostKeep::ChainEnd;
    return d;
  }
  if (last + 1 != D.size() && gate.Stop(PostKeep::ChainEnd, std::to_string(D.size() - 1 - last) + " chain draws follow the claimed end (the film pass is the chain's end, not the combine)")) return d;
  const ChainDraw& dof = D[first];
  const ChainDraw& combine = D[plan.combine];
  const ChainSlot* s0 = no_layer ? combine.Slot(0) : dof.Slot(0);   // lane 140: the t2 blur's colour / depth are the combine's own slots 0 / 3
  const ChainSlot* s1 = no_layer ? combine.Slot(3) : dof.Slot(1);
  // lane 238: the base permutation (yGlareEtc_base, the CAW editor) is one texture, the scene colour: no depth, no fog; nothing reads a depth there
  const bool need_depth = !(plan.combine_only && !plan.fog) && !(plan.t2_blur && !plan.dof && !plan.fog);   // lane 277: a t2 + bloom combine without dof or fog reads no depth either
  if (!s0 || (!s1 && need_depth)) {
    if (gate.Stop(PostKeep::Slot, "the dof draw does not bind its colour and depth")) return d;
    d.keep = PostKeep::Slot;
    return d;
  }
  d.colour = s0->image, d.depth = s1 ? s1->image : 0u, d.out = plan.combine_target;
  // lane 224 review: the pass reads colour and depth while it writes out; one image on both sides is a feedback the stream's own draw would have shared (all shapes)
  if ((d.colour == d.out || d.depth == d.out) && d.colour && gate.Stop(PostKeep::WritesInputs, "the combine writes the image its colour or depth input reads")) return d;
  if (plan.combine_only) {   // lane 224 review: the plan never looked at the combine's viewport / scissor / blend; ours draws the whole target, opaque
    const rdr_adapter::Draw& cd = f.draws[combine.draw];
    const rdr::DynamicState& dy = f.dynamics[cd.dynamic].host;
    const rdr::BlendState& bl = f.fixeds[cd.fixed].state.blend[0];
    const bool full_vp = dy.viewport[0] <= 0.f && dy.viewport[1] <= 0.f && dy.viewport[0] + dy.viewport[2] >= float(combine.target_w) && dy.viewport[1] + dy.viewport[3] >= float(combine.target_h);   // (the guest viewport is the 8192 square; the scissor is what clips)
    const bool full_sc = dy.scissor.x <= 0 && dy.scissor.y <= 0 && dy.scissor.width >= combine.target_w && dy.scissor.height >= combine.target_h;
    if (!(full_vp && full_sc) && gate.Stop(PostKeep::Shape, "the combine alone does not cover its whole target (viewport " + std::to_string(dy.viewport[0]) + "," + std::to_string(dy.viewport[1]) + " " + std::to_string(dy.viewport[2]) + "x" + std::to_string(dy.viewport[3]) + ", scissor " + std::to_string(dy.scissor.x) + "," + std::to_string(dy.scissor.y) + " " + std::to_string(dy.scissor.width) + "x" + std::to_string(dy.scissor.height) + ", target " + std::to_string(combine.target_w) + "x" + std::to_string(combine.target_h) + ")")) return d;
    if (bl.enable && gate.Stop(PostKeep::Shape, "the combine alone is drawn blended")) return d;
  }
  if (plan.t2_blur && !plan.skip_dof_chain) {   // lane 140 (lane 277 review SF1: a dead blur is not recorded, so its source is not read or required: d.blur_src stays 0)
    d.blur_src = plan.blur_source_image;
    if (plan.dof && plan.t2_far < 0 && plan.slot_image[2] != plan.slot_image[6] && gate.Stop(PostKeep::Slot, "the t2 blur: the combine's slots 2 and 6 hold different images (the pass uses one blur for both)")) return d;
    const rdr_adapter::PostImageInfo si = in.info(d.blur_src), ci = in.info(d.colour);
    if (si.known && ci.known && si.scaled != ci.scaled && gate.Stop(PostKeep::Texel, "the t2 blur's source and the scene colour are not both scaled or both unscaled")) return d;
    if (si.known && (si.width != D[plan.t2_index].target_w || si.height != D[plan.t2_index].target_h) && gate.Stop(PostKeep::Texel, "the t2 blur's source is not the size of its target")) return d;
    if (faults.shape_misclassified) d.colour = d.blur_src;   // planted: the t2's source taken for the scene colour
  }
  d.far_blur = plan.far_blur_separate && !faults.t2_far_ignored ? plan.far_blur_image : 0;
  // inputs: the pass reads one colour and one depth image for the dof and the combine
  const ChainSlot* c0 = combine.Slot(0);
  const ChainSlot* c3 = combine.Slot(3);
  if ((!c0 || c0->image != d.colour) && gate.Stop(PostKeep::Slot, "the combine's colour is not the dof's colour")) return d;
  if ((plan.dof || plan.fog) && (!c3 || c3->image != d.depth) && gate.Stop(PostKeep::Slot, "the combine's depth is not the dof's depth")) return d;
  if (faults.null_input) d.null_depth = true, d.depth = 0;   // planted (S100 07:15): the dof's slot 1 image id is zeroed in this copy of the frame's chain; with the checks off the hook binds a black image there
  if (!d.colour || (need_depth && !d.depth) || !d.out) {
    if (gate.Stop(PostKeep::NullInput, d.null_depth ? "the dof's depth reads image 0 (planted)" : "an input image is image 0")) return d;
  }
  const auto have = [&in](uint32_t id) { return in.map(id) != rdr::kNoImage; };
  if (d.blur_src && !have(d.blur_src) && gate.Stop(PostKeep::NotReady, "the t2 blur's source image is not in the renderer")) return d;   // lane 140
  if (!have(d.colour) || (d.depth && !have(d.depth)) || !have(d.out) || (d.far_blur && !have(d.far_blur))) {
    if (gate.Stop(PostKeep::NotReady, "an image the chain reads or writes is not in the renderer")) return d;
  }
  // the dof depth filter against the stream's own sampler
  static const PostLayer kNoLayer;   // lane 140: the t2 blur's frames have no dof depth sampler
  const PostLayer& layer = no_layer ? kNoLayer : plan.layers.back();
  bool linear = layer.depth_linear;
  if (faults.dof_depth_filter) linear = !linear;
  if (faults.r32f_point) linear = false;
  if (limits.no_r32f_linear && layer.depth_linear && !faults.r32f_point && !faults.dof_depth_filter && gate.Stop(PostKeep::Filter, "the depth copy cannot be filtered linearly on this device")) return d;
  // F4 (lane 140 review): a MIXED dof depth sampler (min and mag filter differ) was a guess (the restore rule); nothing follows it, so the frame stays the stream's
  if (!no_layer && layer.stream_filter == 2 && gate.Stop(PostKeep::Filter, "the stream binds the dof depth with a mixed (min != mag) sampler")) return d;
  if (layer.stream_filter != 2 && (linear ? 1 : 0) != layer.stream_filter && gate.Stop(PostKeep::Filter, std::string("the dof depth filter would be ") + (linear ? "linear" : "point") + ", the stream's is " + (layer.stream_filter ? "linear" : "point"))) return d;
  bool merge_linear[4] = {true, true, true, true};   // lane 238: what the pass binds on the m1_m2 merge: the plan's (the stream's own filter per slot); the planted twin binds linear whatever the stream does
  for (int sl = 0; sl < 4; ++sl) merge_linear[sl] = faults.merge_filter_linear || plan.params.glare_merge_linear[sl];
  // lane 238: a draw our pass does not record (dead bloom / glare, the dof chain of a frame that reads none) binds nothing of ours: its slots' filters are not compared (run 7131: 46 m1_m2 binds with slot 3 POINT kept the frame the stream's although the glare was dead)
  std::vector<uint8_t> dead_draw(D.size(), 0);
  if (!faults.dead_filters_checked) {
    for (uint32_t x : plan.skipped_draw_list) if (x < dead_draw.size()) dead_draw[x] = 1;
    if (plan.skip_dof_chain) for (size_t x = 0; x < D.size(); ++x) if (D[x].kind == ChainKind::Dof || D[x].kind == ChainKind::HoleFill || (plan.t2_blur && x == plan.t2_index)) dead_draw[x] = 1;   // (lane 277: the dead t2 blur)
  }
  // every chain slot's filter against what the pass binds there (the dof depth is handled above); address modes are counted, not checked: the pass clamps (POST_OURS.md)
  for (uint32_t k = first; k <= last; ++k)
    for (const ChainSlot& sl : D[k].slots) {
      if (dead_draw[k]) continue;
      if (D[k].kind == ChainKind::Combine && (sl.slot == 2 || sl.slot == 6) && !plan.dof) continue;   // lane 277 review SF2: a combine without a dof permutation does not sample slots 2 / 6
      if (sl.addr_u != 2 || sl.addr_v != 2) ++d.slot_repeat;
      const int want = ExpectedFilter(D[k].kind, sl.slot, merge_linear);
      if (sl.filter == 2 && D[k].kind == ChainKind::M1M2 && sl.slot <= 3 && sl.image && gate.Stop(PostKeep::Filter, "the stream binds a m1_m2 slot " + std::to_string(sl.slot) + " with a mixed (min != mag) sampler")) return d;   // lane 238 (review SF-2)
      if (want < 0 || sl.filter == 2 || !sl.image || (k == first && sl.slot == 1 && !no_layer)) continue;
      if (int(sl.filter) != want && gate.Stop(PostKeep::Filter, std::string("the stream binds a ") + ChainKindName(D[k].kind) + " slot " + std::to_string(sl.slot) + (sl.filter ? " linear" : " point") + "; the pass binds it " + (want ? "linear" : "point"))) return d;
    }
  // the pass's own images follow the render scale; the combine's texel is the picture's own
  const rdr_adapter::PostImageInfo colour_info = in.info(d.colour), out_info = in.info(d.out);
  if (!out_info.known && gate.Stop(PostKeep::NotReady, "the combine's target has no known size")) return d;   // (no silent 1280x720)
  const bool scaled_images = !faults.unscaled_steps && colour_info.scaled;   // the pass's own images follow the scene colour's scaling
  const bool scale_matters = limits.render_scale != 1.0f;
  if (scale_matters && !scaled_images && (colour_info.scaled || out_info.scaled) && gate.Stop(PostKeep::Unscaled, "the pass's quarter / eighth size images keep the game's size at a render scale above 1")) return d;
  const bool colour_scaled = faults.fixed_texel ? false : colour_info.scaled;
  d.fixed_texel_copy = faults.fixed_texel;
  if (scale_matters && colour_info.known && out_info.known && colour_scaled != out_info.scaled && gate.Stop(PostKeep::Texel, "the combine reads a picture of another extent than it writes")) return d;

  // the claim: ops between the chain's first and last draw
  uint32_t cfirst = first;   // lane 140 planted claim_from_restore: the claim starts at the copy_depth in front of the dof (it writes the depth the dof reads: the writes / reads checks must refuse)
  if (faults.claim_from_restore && first > 0 && D[first - 1].kind == ChainKind::CopyDepth) cfirst = first - 1;
  d.restores_before = first > 0 && D[first - 1].kind == ChainKind::CopyDepth;
  const uint32_t first_op = D[cfirst].op, last_op = D[last].op;
  std::vector<uint8_t> claimed(f.draws.count, 0);
  std::unordered_set<uint32_t> targets;
  for (uint32_t k = cfirst; k <= last; ++k) {
    claimed[D[k].draw] = 1;
    targets.insert(D[k].target);
  }
  if ((faults.interleaved || faults.reads_chain) && last - first >= 2) claimed[D[first + 1].draw] = 0;   // planted twins: a chain draw is left to the stream
  rdr_adapter::PostClaim& c = d.claim;
  c.draw = claimed;
  c.drop.assign(f.ops.count, 0);
  c.first_draw = D[cfirst].draw, c.last_draw = D[last].draw;
  c.draws = last - cfirst + 1;
  // what the pass writes: its output; what it reads: the dof's colour and depth and the earlier layer's blur. Nothing KEPT inside the range may write them.
  const std::unordered_set<uint32_t> inputs = {d.colour, d.depth, d.far_blur};
  // which claimed draw reads which image, by op (a copy is dropped only when a claimed draw later in the frame reads its destination)
  std::vector<std::pair<uint32_t, uint32_t>> claimed_reads;
  for (uint32_t k = 0; k < f.ops.count; ++k) {
    const rdr_adapter::Op& op = f.ops[k];
    if (op.kind != rdr_adapter::OpKind::Draw || !claimed[op.index]) continue;
    const rdr_adapter::Draw& dr = f.draws[op.index];
    for (uint32_t s = 0; s < dr.texture_count && dr.texture_first + s < f.texture_slots.count; ++s) claimed_reads.emplace_back(k, f.texture_slots[dr.texture_first + s].image);
  }
  const auto claimed_reads_after = [&](uint32_t k, uint32_t image) {
    for (const auto& r : claimed_reads)
      if (r.first > k && r.second == image) return true;
    return false;
  };
  std::unordered_set<uint32_t> made;   // images the dropped copies write
  std::vector<uint32_t> kept_chain_copies;   // ops of copies from a claimed target that stay
  uint32_t cur = 0;
  uint32_t interleaved = 0, writes = 0;
  bool plant_used = false;
  for (uint32_t k = 0; k < f.ops.count; ++k) {
    const rdr_adapter::Op& op = f.ops[k];
    const bool inside = k > first_op && k < last_op;
    if (op.kind == rdr_adapter::OpKind::SetTargets) {
      const rdr::TargetsOp& t = f.targets[op.index];
      cur = t.color_count ? t.color[0].image : t.depth.image;
      if (inside && t.color_count == 1 && (t.color[0].image != d.out || faults.drop_out_targets) && targets.count(t.color[0].image)) c.drop[k] = 1, ++c.targets;
    } else if (op.kind == rdr_adapter::OpKind::Clear) {
      if (inside && targets.count(cur)) {
        const uint8_t flags = f.clears[op.index].flags;
        const bool depth_bits = (flags & (rdr::kClearDepth | rdr::kClearStencil)) != 0;
        if (faults.drop_depth_clear) {   // planted: the old rule, any clear of a claimed target goes
          c.drop[k] = 1, ++c.clears;
          if (depth_bits) ++c.depth_clears_dropped;
        } else if (!depth_bits) {
          c.drop[k] = 1, ++c.clears;
        } else if (flags & rdr::kClearColor) {
          c.drop[k] = 2, ++c.clears;   // colour stripped, the depth / stencil part stays
        }
      }
      if (inside && !c.drop[k] && inputs.count(cur) && cur) ++writes;
    } else if (op.kind == rdr_adapter::OpKind::Copy) {
      const rdr::CopyOp& cp = f.copies[op.index];
      uint32_t dest = cp.dest.image;
      if (inside && targets.count(cp.source.image)) {
        if (faults.writes_input_inside && !plant_used) {   // planted: this copy stays and writes the pass's colour input
          plant_used = true;
          dest = d.colour;
          kept_chain_copies.push_back(k);
        } else if (claimed_reads_after(k, dest)) {
          c.drop[k] = 1, ++c.copies, made.insert(dest);
          if (d.far_blur && dest == d.far_blur && !faults.far_copy_unchecked) ++writes;   // lane 296b (SF1): a dropped copy into the far blur is a write the pass would miss (it would sample last frame's blur)
        } else {
          ++c.copies_kept;
          kept_chain_copies.push_back(k);
        }
      }
      if (inside && !c.drop[k] && inputs.count(dest) && dest) ++writes;
    } else if (op.kind == rdr_adapter::OpKind::Draw) {
      if (k > first_op && k <= last_op && !claimed[op.index]) ++interleaved;
      if (claimed[op.index]) {
        // (a claimed draw never writes what the pass reads)
      }
    }
  }
  for (uint32_t k = cfirst; k <= last; ++k)
    if (D[k].target != d.out && inputs.count(D[k].target) && D[k].target) ++writes;
  if (c.depth_clears_dropped && gate.Stop(PostKeep::DepthClear, std::to_string(c.depth_clears_dropped) + " depth / stencil clears of a claimed target would be dropped (the HUD's depth clear)")) return d;
  if (interleaved && !faults.reads_chain && gate.Stop(PostKeep::Interleaved, std::to_string(interleaved) + " stream draws sit between the claimed ones")) return d;
  if (writes && gate.Stop(PostKeep::WritesInputs, std::to_string(writes) + " kept ops inside the range write an image the pass reads or its output")) return d;
  // nothing outside the chain may read what the chain (or a dropped copy) made: draws, copies and presents after the chain's start
  std::unordered_set<uint32_t> made_all = made;
  for (uint32_t t : targets)
    if (t != d.out) made_all.insert(t);
  uint32_t reads = 0;
  for (uint32_t k = first_op + 1; k < f.ops.count; ++k) {
    const rdr_adapter::Op& op = f.ops[k];
    if (op.kind == rdr_adapter::OpKind::Draw) {
      if (claimed[op.index]) continue;
      const rdr_adapter::Draw& dr = f.draws[op.index];
      for (uint32_t s = 0; s < dr.texture_count && dr.texture_first + s < f.texture_slots.count; ++s)
        if (made_all.count(f.texture_slots[dr.texture_first + s].image)) ++reads;
    } else if (op.kind == rdr_adapter::OpKind::Copy) {
      if (c.drop[k]) continue;
      const rdr::CopyOp& cp = f.copies[op.index];
      if (made_all.count(cp.source.image) && !(k < last_op && targets.count(cp.source.image))) ++reads;   // (a kept copy OF a claimed target is judged below by who reads its destination)
    } else if (op.kind == rdr_adapter::OpKind::Present) {
      if (made_all.count(f.presents[op.index].source.image)) ++reads;
    }
  }
  // a copy from a claimed target that stays (no claimed draw reads its destination) copies pictures our pass never made: nobody after it may read the destination either
  for (uint32_t k : kept_chain_copies) {
    const rdr::CopyOp& cp = f.copies[f.ops[k].index];
    for (uint32_t m = k + 1; m < f.ops.count; ++m) {
      const rdr_adapter::Op& op = f.ops[m];
      if (op.kind == rdr_adapter::OpKind::Draw && !claimed[op.index]) {
        const rdr_adapter::Draw& dr = f.draws[op.index];
        for (uint32_t s = 0; s < dr.texture_count && dr.texture_first + s < f.texture_slots.count; ++s)
          if (f.texture_slots[dr.texture_first + s].image == cp.dest.image) ++reads;
      } else if (op.kind == rdr_adapter::OpKind::Copy && f.copies[op.index].source.image == cp.dest.image && !c.drop[m]) {
        ++reads;
      } else if (op.kind == rdr_adapter::OpKind::Present && f.presents[op.index].source.image == cp.dest.image) {
        ++reads;
      }
    }
  }
  if (reads && gate.Stop(PostKeep::ReadsChain, std::to_string(reads) + " later stream draws / copies / presents read an image the dropped chain made")) return d;

  // dead work (counted; the pass does not record it)
  d.bloom0 = plan.hdr_drawn && plan.bloom_zero, d.glare0 = plan.hdr_drawn && plan.glare_zero, d.dof0 = plan.skip_dof_chain;
  d.dead_draws = plan.skipped_draws + (plan.skip_dof_chain ? (plan.t2_blur ? 1u : 2u) : 0u);   // lane 277: no dof permutation and no live HDR: the t2 blur is dead alone (it has no hole_fill)
  d.dead_copies = plan.skipped_copies;

  // the effective parameters
  d.params = plan.params;
  if (plan.t2_blur && plan.skip_dof_chain) d.params.t2_blur = false;   // lane 277: nothing reads the blur (no dof permutation, HDR chain dead): not recorded
  for (int sl = 0; sl < 4; ++sl) d.params.glare_merge_linear[sl] = merge_linear[sl];   // lane 238
  if (faults.t2_tables_shared && plan.t2_blur) {   // lane 238 planted: the DoF blur takes the bloom t2's tables
    std::memcpy(d.params.blur_t2_offsets, d.params.bloom_t2_offsets, sizeof d.params.blur_t2_offsets);
    std::memcpy(d.params.blur_t2_weights, d.params.bloom_t2_weights, sizeof d.params.blur_t2_weights);
  }
  d.params.dof_depth_linear = linear;
  d.params.scaled_images = scaled_images;
  d.dof_depth_repeat = !no_layer && s1 && (s1->addr_u != 2 || s1->addr_v != 2);   // lane 140 (clamp is addr 2)
  d.params.dof_depth_repeat = faults.dof_depth_address && !no_layer && s1 && (s1->addr_u == 0 || s1->addr_v == 0);   // clamp is the shipped choice; the experiment mirrors the stream
  if (!in.depth_of_field && plan.dof) {   // DEPTH OF FIELD off: the combine without its dof permutation (the binder's swap); the dof + hole_fill passes stay only when the bloom reads them
    const bool hdr_live = plan.hdr_drawn && !(plan.bloom_zero && plan.glare_zero);
    d.params.dof = false;
    if (plan.t2_blur) {   // lane 238: with a live HDR chain the t2 is still the bloom's source (the stream draws it whatever the combine permutation); only the combine loses its dof permutation
      d.params.t2_blur = hdr_live, d.params.dof_chain = false;
    } else {
      d.params.t2_blur = false;   // lane 140: the combine without its dof permutation reads no blur: the t2 is dead too
      d.params.dof_chain = hdr_live;
    }
    if (!hdr_live) d.dof0 = true, d.dead_draws += plan.t2_blur ? 1u : 2u;
  }
  if (!in.film_grain && plan.film) d.params.film = false;   // FILM GRAIN off: the 30 Years filter pass is not recorded (the stream draws it clean: a copy of the picture)
  if (faults.film_dropped) d.params.film = false;   // lane 238 planted: the film pass is claimed and not recorded
  d.t2_default_tables = faults.t2_default_tables && plan.t2_blur;
  d.no_combine = faults.no_combine;
  d.params.stop_before_combine = faults.no_combine;
  d.inject_fail_late = faults.inject_fail_late;
  c.ok = true;
  d.ok = true;
  return d;
}

}  // namespace rdr_scene
