
// ---- record -----------------------------------------------------------------------------------------------------------------------

void Renderer::Impl::RecordPass(VkCommandBuffer cmd, const Frame& frame, const PassPlan& pass, FrameSlot& slot, VkDeviceSize ring_base) {
  const TargetsOp& targets = *pass.targets;
  // S102 M1: this pass's counts (integer increments; the area bound is computed only when counting is on). Barriers are read as the change in the frame total.
  const bool counting = options.counts;
  CallCounts pc;
  const uint32_t barriers_before = counts_cur.total.barriers, barrier_images_before = counts_cur.total.barrier_images;
  const DrawOp* last_draw = nullptr;  // for the planted extra draw
  const bool plant_here = counts_plant >= 0 && uint32_t(&pass - passes.data()) == uint32_t(counts_plant);
  bool plant_done = false;
  uint64_t dyn_area_now = 0;  // the area bound of the last recorded draw (the planted draw repeats it)
  uint32_t plant_op = ~0u;  // the pass's last draw op: the planted draw follows it at once, in the state it left bound
  if (plant_here)
    for (uint32_t i = pass.end_op; i > pass.first_op; --i)
      if (frame.ops[i - 1].kind == OpKind::Draw) { plant_op = i - 1; break; }
  const auto plant_draw = [&](const DrawOp& d) {  // S102 M1 test plant (rplay --counts-plant N): one extra draw, the repeat of the draw before it. The picture is not meant to be judged.
    plant_done = true;
    if (d.index.buffer != kNoBuffer) vkCmdDrawIndexed(cmd, d.count, d.instances, d.first, d.base_vertex, 0), ++pc.indexed_draws, pc.indices += uint64_t(d.count) * d.instances;
    else vkCmdDraw(cmd, d.count, d.instances, d.first, 0), ++pc.draws, pc.vertices += uint64_t(d.count) * d.instances;
    pc.area_px += dyn_area_now;
  };
  uint32_t pass_w = pass.width, pass_h = pass.height;
  // Layouts first: attachments, then anything this pass samples.
  for (uint32_t c = 0; c < targets.color_count; ++c) {
    ImageEntry& entry = images[targets.color[c].image];
    const uint32_t sub = targets.color[c].mip * entry.layers + targets.color[c].layer;
    Transition(entry, sub, g_layered_plant.load(std::memory_order_relaxed) == 2 ? 1u : ViewCount(targets.color[c]) /* S102 C1: a layered target's layers (plant 2: the first only) */, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
               pass.load[c] != VK_ATTACHMENT_LOAD_OP_LOAD, true /* S102 C1: ranged */);
  }
  if (targets.depth.image != kNoImage) {
    ImageEntry& entry = images[targets.depth.image];
    const uint32_t sub = targets.depth.mip * entry.layers + targets.depth.layer;
    const bool discard = pass.load[kMaxColorTargets] != VK_ATTACHMENT_LOAD_OP_LOAD && pass.stencil_load != VK_ATTACHMENT_LOAD_OP_LOAD;
    Transition(entry, sub, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, discard);
  }
  for (uint32_t k = 0; k < pass.sampled_count; ++k) {
    const SampledImage& item = sampled[pass.first_sampled + k];
    bool is_attachment = item.image == targets.depth.image;
    for (uint32_t c = 0; c < targets.color_count; ++c) is_attachment |= item.image == targets.color[c].image;
    if (is_attachment) {
      Problem("sampled_while_attached", "image %u is both a target and a texture of one pass", unsigned(item.image));
      continue;
    }
    ImageEntry& entry = images[item.image];
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | (item.vertex ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : 0);
    Transition(entry, 0, uint32_t(entry.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, stages, VK_ACCESS_SHADER_READ_BIT, false);
  }
  FlushBarriers(cmd);

  RenderPassKey pass_key{};
  PassClassKey class_key{};
  FramebufferKey framebuffer_key{};
  uint32_t samples = 1;
  for (uint32_t c = 0; c < targets.color_count; ++c) {
    ImageEntry& entry = images[targets.color[c].image];
    pass_key.formats[c] = class_key.formats[c] = uint16_t(entry.format);
    pass_key.load[c] = pass.load[c];
    pass_key.store[c] = pass.store[c];
    framebuffer_key.views[c] = TargetView(entry, targets.color[c].mip, targets.color[c].layer, ViewCount(targets.color[c]));   // S102 C1
    samples = entry.desc.samples;
    pass_key.views = class_key.views = uint8_t(ViewCount(targets.color[c]) > 1 ? ViewCount(targets.color[c]) : 0);
  }
  if (targets.depth.image != kNoImage) {
    ImageEntry& entry = images[targets.depth.image];
    pass_key.formats[kMaxColorTargets] = class_key.formats[kMaxColorTargets] = uint16_t(entry.format);
    pass_key.load[kMaxColorTargets] = pass.load[kMaxColorTargets];
    pass_key.store[kMaxColorTargets] = pass.store[kMaxColorTargets];
    pass_key.stencil_load = pass.stencil_load;
    pass_key.stencil_store = pass.stencil_store;
    framebuffer_key.views[kMaxColorTargets] = TargetView(entry, targets.depth.mip, targets.depth.layer);
    samples = entry.desc.samples;
  }
  pass_key.color_count = class_key.color_count = uint8_t(targets.color_count);
  pass_key.has_depth = class_key.has_depth = targets.depth.image != kNoImage;
  pass_key.samples = class_key.samples = uint8_t(samples);
  const uint32_t pass_class = GetPassClass(class_key);
  // S102 C4 (half_trans.h): the tail of additive no-depth-write draws that ends a scene colour pass is drawn at half size (after this pass, below). Decided here, before the pass begins: the planner needs the
  // images and pipelines to exist (PrepareHalf), else the tail stays in this pass. The first part stores colour and depth (the half passes read the depth, the composite adds onto the colour).
  uint32_t tail_begin = pass.end_op;
  std::vector<uint8_t> half_skip;  // S102 C4: per op of the pass, 1 = a half-size draw (set only when the tail is taken)
  HalfPrep half_prep;
  if (frame.half_transparents && !g_draw_mod_mode.load(std::memory_order_relaxed) && targets.color_count == 1 && targets.depth.image != kNoImage && images[targets.color[0].image].gpu_class == kGpuColor &&
      images[targets.color[0].image].desc.samples == 1 && images[targets.depth.image].desc.samples == 1 && images[targets.depth.image].sampled_view != VK_NULL_HANDLE) {
    const uint64_t half_n = ++half_trans::g_frames;
    const int half_plant = half_trans::g_plant.load(std::memory_order_relaxed);
    std::vector<half_trans::OpIn> hin;
    hin.reserve(pass.end_op - pass.first_op);
    for (uint32_t i = pass.first_op; i < pass.end_op; ++i) {
      half_trans::OpIn in;
      const Op& hop = frame.ops[i];
      if (hop.kind == OpKind::Draw) {
        const DrawOp& hd = frame.draws[hop.index];
        const DrawState& st = frame.states[hd.state];
        half_trans::DrawRule rule;
        rule.color_count = uint8_t(targets.color_count), rule.blend_enable = st.blend[0].enable != 0;
        rule.src_color = st.blend[0].src_color, rule.dst_color = st.blend[0].dst_color, rule.color_op = st.blend[0].color_op;
        rule.src_alpha = st.blend[0].src_alpha, rule.dst_alpha = st.blend[0].dst_alpha, rule.alpha_op = st.blend[0].alpha_op, rule.write_mask = st.blend[0].write_mask;
        rule.depth_test = st.depth_test != 0, rule.depth_write = st.depth_write != 0, rule.depth_compare = st.depth_compare, rule.stencil_test = st.stencil_test != 0;
        rule.shader_ok = st.program < programs.size() && programs[st.program].half_ok;
        const half_trans::Reason why = half_trans::Classify(rule, half_plant);
        ++half_trans::g_reasons[why];
        in.is_draw = true, in.kind = why == half_trans::kTaken ? half_trans::kTake : (half_plant != 2 && half_trans::CanHoist(rule)) ? half_trans::kHoist : half_trans::kOther;
      }
      hin.push_back(in);
    }
    const half_trans::Plan hplan = half_trans::PlanTail(hin);
    if (hplan.draws == 0) ++half_trans::g_declined_short;
    else if (!PrepareHalf(pass, slot, half_prep)) ++half_trans::g_declined_target;
    else {
      tail_begin = pass.first_op + hplan.begin;
      half_skip = hplan.flags;  // per op of the pass: 1 = drawn at half size (the main loop leaves it out)
      ++half_trans::g_passes_taken, half_trans::g_draws_halved += hplan.draws;
      pass_key.store[0] = VK_ATTACHMENT_STORE_OP_STORE;
      pass_key.store[kMaxColorTargets] = VK_ATTACHMENT_STORE_OP_STORE;
    }
    if (census_flush::Due(g_cf_half) || half_n % 600 == 0)  // S102 C4: the live census line: a Surface "no gain" is told apart from "took 0 frames"
      Log("[rdr][half_transparents] colour passes seen %llu, taken %llu, declined %llu (no tail of 4 or more %llu, target %llu), draws halved %llu; draws refused by reason: colour targets %llu, not additive %llu, alpha changes %llu, depth state %llu, stencil %llu, shader %llu",
          static_cast<unsigned long long>(half_n), static_cast<unsigned long long>(half_trans::g_passes_taken.load()), static_cast<unsigned long long>(half_trans::g_declined_short.load() + half_trans::g_declined_target.load()),
          static_cast<unsigned long long>(half_trans::g_declined_short.load()), static_cast<unsigned long long>(half_trans::g_declined_target.load()), static_cast<unsigned long long>(half_trans::g_draws_halved.load()),
          static_cast<unsigned long long>(half_trans::g_reasons[1].load()), static_cast<unsigned long long>(half_trans::g_reasons[2].load()), static_cast<unsigned long long>(half_trans::g_reasons[3].load()),
          static_cast<unsigned long long>(half_trans::g_reasons[4].load()), static_cast<unsigned long long>(half_trans::g_reasons[5].load()), static_cast<unsigned long long>(half_trans::g_reasons[6].load()));
  }
  framebuffer_key.pass = GetRenderPass(pass_key);
  framebuffer_key.width = pass.width;
  framebuffer_key.height = pass.height;

  VkClearValue clear_values[kAttachmentSlots];
  uint32_t clear_count = 0;
  bool any_clear = false;
  uint32_t load_clear_mask = 0;  // S102 M1: bit c = colour attachment c's load op is CLEAR
  bool load_clear_depth = false;
  for (uint32_t c = 0; c < targets.color_count; ++c) {
    any_clear |= pass.load[c] == VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (pass.load[c] == VK_ATTACHMENT_LOAD_OP_CLEAR) load_clear_mask |= 1u << c;
    clear_values[clear_count++] = pass.clear[c];
  }
  if (targets.depth.image != kNoImage) {
    load_clear_depth = pass.load[kMaxColorTargets] == VK_ATTACHMENT_LOAD_OP_CLEAR || pass.stencil_load == VK_ATTACHMENT_LOAD_OP_CLEAR;
    any_clear |= load_clear_depth;
    clear_values[clear_count++] = pass.clear[kMaxColorTargets];
  }
  if (!any_clear) clear_count = 0;

  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = framebuffer_key.pass;
  begin.framebuffer = GetFramebuffer(framebuffer_key);
  begin.renderArea.extent = {pass.width, pass.height};
  begin.clearValueCount = clear_count;
  begin.pClearValues = clear_values;
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  pc.clears += LoadClearCount(load_clear_mask, load_clear_depth);  // S102 M1: one per attachment whose load op is CLEAR
  const bool dyn_on = device.caps().dyn_mask != 0;   // S111 DYNSTATE (dyn_state.h): the draw state the pipelines do not bake is set per draw below
  dyn::Applied dyn_last;                             // what was last set in THIS render pass (nothing yet: the first draw sets all)
  dyn::Counts dyn_n;
  // S102 G11 (offline census, rplay --frame-trace FILE): one line per frame-class pass and per draw / clear in it (numbers only). Nothing is recorded differently.
  std::FILE* trace = nullptr;
  const uint32_t trace_pass = uint32_t(&pass - passes.data());
  if (images[(targets.color_count ? targets.color[0] : targets.depth).image].gpu_class == kGpuFrame) trace = rdr::g_frame_trace.load(std::memory_order_relaxed);
  if (trace)
    std::fprintf(trace, "{\"t\":\"p\",\"f\":%llu,\"p\":%u,\"img\":%u,\"w\":%u,\"h\":%u,\"colors\":%u,\"depth\":%d,\"load\":[%d,%d,%d,%d,%d],\"store\":[%d,%d,%d,%d,%d],\"first_op\":%u,\"end_op\":%u,\"sampled\":%u}\n",
                 static_cast<unsigned long long>(serial), trace_pass, unsigned((targets.color_count ? targets.color[0] : targets.depth).image), pass.width, pass.height, targets.color_count, targets.depth.image != kNoImage ? 1 : 0,
                 int(pass.load[0]), int(pass.load[1]), int(pass.load[2]), int(pass.load[3]), int(pass.load[kMaxColorTargets]), int(pass.store[0]), int(pass.store[1]), int(pass.store[2]), int(pass.store[3]), int(pass.store[kMaxColorTargets]), pass.first_op, pass.end_op, pass.sampled_count);

  const ImageRef& first_target = targets.color_count ? targets.color[0] : targets.depth;
  const bool layered_pass = targets.color_count && ViewCount(targets.color[0]) > 1;   // S102 C1 review F6: a multiview pass makes a query count once per view: no in-pass queries there
  const bool scaled = images[first_target.image].desc.usage & kUsageScaled;
  // S105 A1 review F1: the factor is per axis = the pass image's REAL size / its logical size (exactly `scale` at integer scales; at 0.6667 the image is ceil'd to 854 wide and the viewport must reach it: 853.33 left a column undrawn)
  const float factor_x = scaled ? float(images[first_target.image].width) / float(std::max<uint32_t>(1u, images[first_target.image].desc.width)) : 1.0f;
  const float factor_y = scaled ? float(images[first_target.image].height) / float(std::max<uint32_t>(1u, images[first_target.image].desc.height)) : 1.0f;
  uint64_t dyn_area = 0;  // S102 M1: the current dynamic block's viewport x scissor x pass rectangle, in pixels

  // S102 G10 (colour cut 1, colour_cuts.h): a depth-only prepass of the eligible opaque arena draws, FIRST in the pass; their normal draws then test LESS_OR_EQUAL (below), so the fragment shader
  // runs for the visible surface only. It declines (counted) when a depth clear follows the pass's first draw. Nothing in the pass is reordered.
  const int cut_plant = colour_cuts::g_colour_cuts_plant.load(std::memory_order_relaxed);
  std::vector<uint8_t> cut_eligible;
  bool cut_on = false;
  if ((colour_cuts_ & colour_cuts::kPrepass) && targets.depth.image != kNoImage && images[first_target.image].gpu_class == kGpuColor && !g_draw_mod_mode.load(std::memory_order_relaxed)) {
    cut_eligible.assign(pass.end_op - pass.first_op, 0);
    // Review F8: the inputs for the pure planner (colour_cuts.h PlanPass), one per draw op in order
    std::vector<colour_cuts::DrawIn> plan_in;
    std::vector<uint32_t> plan_op;
    bool depth_clear = false;
    for (uint32_t i = pass.first_op; i < pass.end_op; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::Clear) {
        const ClearOp& clear = frame.clears[op.index];
        if (!clear_absorbed[op.index] && (clear.flags & (kClearDepth | kClearStencil))) depth_clear = true;  // review F7: any depth clear the load op did not absorb
        continue;
      }
      if (op.kind != OpKind::Draw) continue;
      const DrawOp& draw = frame.draws[op.index];
      const DrawState& st = frame.states[draw.state];
      const Program& program = programs[st.program];
      colour_cuts::DrawIn in;
      colour_cuts::DepthRule& rule = in.rule;
      rule.depth_test = st.depth_test != 0, rule.depth_write = st.depth_write != 0, rule.stencil_test = st.stencil_test != 0, rule.depth_compare = st.depth_compare;
      rule.blend_kind = 0;
      for (uint32_t c = 0; c < targets.color_count; ++c) {
        const BlendState& b = st.blend[c];
        rule.blend_kind = std::max<uint8_t>(rule.blend_kind, colour_cuts::BlendKind(b.enable != 0, b.src_color, b.dst_color, b.color_op, b.src_alpha, b.dst_alpha, b.alpha_op, b.write_mask));
        if (b.write_mask) rule.colour_written = true;
      }
      rule.alpha_benign = colour_cuts::AlphaBenign(frame.dynamics[draw.dynamic].system[8], frame.dynamics[draw.dynamic].system[9]);
      in.shader_class = program.prepass_class;
      in.named = colour_cuts::NamedByMask(colour_cuts_, (draw.flags & kDrawOpAlphaOpaque) != 0, cut_plant);  // review F2: the binder's proof, not a name
      if (cut_plant == 7 || cut_plant == 8) {  // diagnosis only (C4 review F4): 7 = only the vertex-lit family (name has "vertex_lm_vertex"), 8 = everything but it
        const bool vlm = program.name && std::strstr(program.name, "vertex_lm_vertex") != nullptr;
        in.named = in.named && (cut_plant == 7 ? vlm : !vlm);
      }
      auto writes = [](const StencilFace& f) { return f.fail != 0 || f.pass != 0 || f.depth_fail != 0; };  // VK_STENCIL_OP_KEEP == 0
      in.stencil_writer = st.stencil_test && (writes(st.front) || writes(st.back));
      plan_in.push_back(in);
      plan_op.push_back(i);
    }
    const colour_cuts::Plan plan = colour_cuts::PlanPass(plan_in, depth_clear, cut_plant);
    for (size_t k = 0; k < plan.flags.size(); ++k) cut_eligible[plan_op[k] - pass.first_op] = plan.flags[k];
    const uint32_t eligible_count = plan.eligible;
    const bool declined = plan.verdict == colour_cuts::kDeclineClear;
    if (plan.verdict == colour_cuts::kDeclineDirection) ++colour_cuts::g_declined_direction;
    else if (plan.verdict == colour_cuts::kDeclineStencil) ++colour_cuts::g_declined_stencil;
    if (declined) ++colour_cuts::g_declined_clear;
    else if (plan.verdict == colour_cuts::kDeclineDirection || plan.verdict == colour_cuts::kDeclineStencil) {}
    else if (!eligible_count) ++colour_cuts::g_declined_none;
    else {
      cut_on = true;
      cut_pre_pipes.assign(frame.states.count, VK_NULL_HANDLE);
      cut_main_pipes.assign(frame.states.count, VK_NULL_HANDLE);
      ++colour_cuts::g_frames_taken;
      VkPipeline pre_pipeline = VK_NULL_HANDLE;
      uint32_t pre_dynamic = ~0u, pre_textures = ~0u;
      BufferRange pre_vertex[2];
      BufferRange pre_index;
      uint8_t pre_index_32 = 0xFF;
      for (uint32_t i = pass.first_op; i < pass.end_op; ++i) {
        if (!cut_eligible[i - pass.first_op]) continue;
        const DrawOp& draw = frame.draws[frame.ops[i].index];
        VkPipeline& twin = cut_pre_pipes[draw.state];
        if (!twin) {
          DrawState ds = frame.states[draw.state];
          if (cut_plant != 6) ds.variant = kVariantDepthOnly;  // plant 6 (S102 C4 review F4, diagnosis only): the twin keeps the NORMAL fragment stage, colour write mask 0: if the stray pixels vanish the cause is the two pipelines' depth variance
          for (uint32_t c = 0; c < kMaxColorTargets; ++c) ds.blend[c].write_mask = 0;
          twin = GetPipeline(ds, pass_class);
        }
        if (!twin) continue;
        if (cut_plant == 3) continue;  // plant 3: the prepass records nothing (the counters below stay as if it had run: the check compares the picture's fragment count)
        if (twin != pre_pipeline) {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, twin);
          pre_pipeline = twin;
          ++pc.pipeline_binds;
        }
        if (dyn_on) {  // S111 DYNSTATE: the twin's own state (every colour write mask 0), as it was built
          DrawState ts = frame.states[draw.state];
          for (uint32_t c = 0; c < kMaxColorTargets; ++c) ts.blend[c].write_mask = 0;
          DynApply(cmd, dyn_last, ts, pass_class, dyn_n);
        }
        if (draw.dynamic != pre_dynamic) {
          pre_dynamic = draw.dynamic;
          const DynamicState& dyn = frame.dynamics[draw.dynamic];
          VkViewport viewport{dyn.viewport[0] * factor_x, dyn.viewport[1] * factor_y, dyn.viewport[2] * factor_x, dyn.viewport[3] * factor_y, dyn.viewport[4], dyn.viewport[5]};
          vkCmdSetViewport(cmd, 0, 1, &viewport);
          VkRect2D scissor{};
          scissor.offset = {std::max(0, int32_t(float(dyn.scissor.x) * factor_x)), std::max(0, int32_t(float(dyn.scissor.y) * factor_y))};
          scissor.extent = {uint32_t(std::ceil(float(dyn.scissor.width) * factor_x)), uint32_t(std::ceil(float(dyn.scissor.height) * factor_y))};
          if (cut_plant == 4) scissor.offset.x = 8, scissor.extent.width = 0x7FFFFFFF;  // plant 4: offset + extent overflows int32 (VUID-vkCmdSetScissor-offset-00596): only the validation layer sees it; the driver clamps to the render area
          vkCmdSetScissor(cmd, 0, 1, &scissor);
          vkCmdSetDepthBias(cmd, dyn.depth_bias_constant, dyn.depth_bias_clamp, dyn.depth_bias_slope);
          vkCmdSetBlendConstants(cmd, dyn.blend_constants);
          vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_compare_mask);
          vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_write_mask);
          vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_reference);
          vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dyn.system), dyn.system);
          ++pc.dynamic_sets, ++pc.push_constants;
        }
        const uint32_t offsets[2] = {uint32_t(ring_base + draw.vs_constants),
                                     uint32_t((draw.flags & kDrawOpPrevConstants) ? prev_constants_base + draw.vs_constants : ring_base + draw.fs_constants)};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, offsets);
        ++pc.descriptor_binds;
        if (draw.textures != pre_textures) {
          VkDescriptorSet set = GetTextureSet(frame, draw.textures, slot);
          if (!set) continue;
          pre_textures = draw.textures;
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
          ++pc.descriptor_binds;
        }
        const VkDeviceSize geometry_base = ring_base + AlignUp(frame.constants.count, 16);
        for (uint32_t b = 0; b < 2; ++b) {
          if (draw.vertex[b].buffer == kNoBuffer) continue;
          if (draw.vertex[b].buffer == pre_vertex[b].buffer && draw.vertex[b].offset == pre_vertex[b].offset) continue;
          pre_vertex[b] = draw.vertex[b];
          const bool frame_owned = draw.vertex[b].buffer == kFrameGeometry;
          VkBuffer buffer = frame_owned ? ring : buffers[draw.vertex[b].buffer].buffer;
          VkDeviceSize offset = draw.vertex[b].offset + (frame_owned ? geometry_base : 0);
          vkCmdBindVertexBuffers(cmd, b, 1, &buffer, &offset);
          ++pc.vertex_binds;
        }
        if (draw.index.buffer != kNoBuffer) {
          if (draw.index.buffer != pre_index.buffer || draw.index.offset != pre_index.offset || draw.index_32bit != pre_index_32) {
            pre_index = draw.index;
            pre_index_32 = draw.index_32bit;
            const bool frame_owned = draw.index.buffer == kFrameGeometry;
            VkBuffer buffer = frame_owned ? ring : buffers[draw.index.buffer].buffer;
            vkCmdBindIndexBuffer(cmd, buffer, draw.index.offset + (frame_owned ? geometry_base : 0), draw.index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
            ++pc.index_binds;
          }
          vkCmdDrawIndexed(cmd, draw.count, draw.instances, draw.first, draw.base_vertex, 0);
          ++pc.indexed_draws, pc.indices += uint64_t(draw.count) * draw.instances;
        } else {
          vkCmdDraw(cmd, draw.count, draw.instances, draw.first, 0);
          ++pc.draws, pc.vertices += uint64_t(draw.count) * draw.instances;
        }
        ++colour_cuts::g_draws_prepassed;
      }
    }
  }

  // S102 R18-B3 (mask 1): blend off for the draws the binder proved EXACTLY opaque. Per pass: a pipeline per state is made on first use. af_plant 4 = "not applied" (reads the mask, changes nothing: the counts stay 0).
  const int af_plant = arena_fragments::g_plant.load(std::memory_order_relaxed);
  const bool af_blend = (arena_fragments_ & arena_fragments::kBlendOff) && af_plant != 4 && images[first_target.image].gpu_class == kGpuColor && !g_draw_mod_mode.load(std::memory_order_relaxed);
  uint64_t af_off_draws = 0;
  uint64_t af_declined[arena_fragments::kReasonCount] = {};
  std::vector<uint8_t> af_all_arena;  // plant 1 only: per program, 1 = a yBG_* program
  if (af_blend) {
    af_pipes.assign(frame.states.count, {VK_NULL_HANDLE, 0});
    if (af_plant >= 1 && af_plant <= 3) {
      af_all_arena.assign(programs.size(), 0);
      for (size_t p = 0; p < programs.size(); ++p)
        if (programs[p].name && std::strstr(programs[p].name, "yBG_"))
          af_all_arena[p] = std::strstr(programs[p].name, "vertex_lm_vertex") ? arena_fragments::kClassVertexLit : std::strstr(programs[p].name, "yBG_Default_base") ? arena_fragments::kClassDefaultBase : arena_fragments::kClassArena;
    }
  }

  // S102 R24-F (wwe2k14_rdr_gpu_group_stamps): in a colour / frame pass of a frame that carries draw groups, a timestamp (BOTTOM_OF_PIPE, inside the render pass) at each change of group, so the pass's GPU time
  // splits by group. Not in a multiview pass (a stamp there writes one query a view). The chain: previous step's stamp -> group stamps -> this pass's own closing stamp (written by the step loop).
  const bool gs_on = slot.gs_frame && !layered_pass && slot.ts_count >= 2 && !g_draw_mod_mode.load(std::memory_order_relaxed) &&
                     (images[first_target.image].gpu_class == kGpuColor || images[first_target.image].gpu_class == kGpuFrame);
  const int gs_plant = rdr::g_gpu_group_plant.load(std::memory_order_relaxed);
  rdr::SegmentPlanner gs_plan;
  FrameSlot::GsPass gs_pass;
  uint32_t gs_prev = 0, gs_drawn = 0;
  if (gs_on) {
    gs_pass.begin_stamp = slot.ts_count - 1, gs_pass.end_stamp = slot.ts_count, gs_pass.first_seg = uint32_t(slot.gs_segs.size());
    gs_prev = gs_pass.begin_stamp;
    gs_plan.min_run = frame.group_min_run;
    gs_plan.budget = rdr::kGroupPoolCap > slot.gs_count + 1 ? rdr::kGroupPoolCap - slot.gs_count - 1 : 0;
    gs_plan.last = rdr::kGrpIdle;
  }
  const uint64_t gs_issued0 = uint64_t(pc.draws) + pc.indexed_draws;   // (the prepass's draws are counted before this point)
  VkPipeline bound_pipeline = VK_NULL_HANDLE;
  uint32_t bound_dynamic = ~0u, bound_textures = ~0u;
  BufferRange bound_vertex[2];
  BufferRange bound_index;
  uint8_t bound_index_32 = 0xFF;
  const bool draw_census_on = g_draw_census.load(std::memory_order_relaxed) && images[first_target.image].gpu_class == kGpuColor;
  std::map<std::string, DrawCensusRow> census_rows;  // (review F12: only ever filled, and the string keys only built, when draw_census_on)
  const bool ps_on = draw_census_on && ps_pool && g_draw_census_stats.load(std::memory_order_relaxed);
  const char* ps_open_name = nullptr;
  bool ps_open = false;
  // S102 G10 (instrument): rplay --draw-mod: leave out / 1x1-scissor the draws whose program name matches (see overlay.h g_draw_mod_mode).
  const int draw_mod = g_draw_mod_mode.load(std::memory_order_relaxed);
  if (draw_mod) {
    const uint32_t version = g_draw_mod_version.load();
    if (draw_mod_version != version || draw_mod_match.size() != programs.size()) {
      std::string names;
      {
        std::lock_guard<std::mutex> lock(g_draw_mod_mutex);
        names = g_draw_mod_names;
      }
      draw_mod_match.assign(programs.size(), 0);
      for (size_t p = 0; p < programs.size(); ++p) {
        const std::string name = programs[p].name ? programs[p].name : "";
        size_t at = 0;
        while (at <= names.size()) {
          size_t end = names.find(',', at);
          if (end == std::string::npos) end = names.size();
          const std::string part = names.substr(at, end - at);
          if (!part.empty() && (part == "*" || name.find(part) != std::string::npos)) draw_mod_match[p] = 1;
          at = end + 1;
        }
      }
      draw_mod_version = version;
    }
  }

  for (uint32_t i = pass.first_op; i < pass.end_op; ++i) {
    if (!half_skip.empty() && half_skip[i - pass.first_op]) continue;  // S102 C4: a draw of the half-size tail (drawn after this pass's first part, RecordHalfTail)
    const Op& op = frame.ops[i];
    if (op.kind == OpKind::Clear) {
      if (clear_absorbed[op.index]) continue;
      const ClearOp& clear = frame.clears[op.index];
      if (trace)
        std::fprintf(trace, "{\"t\":\"c\",\"f\":%llu,\"p\":%u,\"op\":%u,\"flags\":%u,\"full\":%u,\"w\":%u,\"h\":%u}\n", static_cast<unsigned long long>(serial), trace_pass, i, unsigned(clear.flags), unsigned(clear.full), clear.rect.width, clear.rect.height);  // S102 G11
      VkClearAttachment attachments[kAttachmentSlots];
      uint32_t count = 0;
      if (clear.flags & kClearColor) {
        for (uint32_t c = 0; c < targets.color_count; ++c) {
          if (!(clear.color_mask & (1u << c))) continue;
          attachments[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          attachments[count].colorAttachment = c;
          std::memcpy(attachments[count].clearValue.color.float32, clear.color, sizeof(clear.color));
          ++count;
        }
      }
      if ((clear.flags & (kClearDepth | kClearStencil)) && targets.depth.image != kNoImage) {
        VkImageAspectFlags depth_aspects = ((clear.flags & kClearDepth) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((clear.flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        uint32_t clear_stencil = clear.stencil;
        if (pass.strip_stencil_clear && (clear.flags & kClearStencil)) {   // S102 R24-C (b): a stencil clear nothing reads is not issued (plant 3: even when a later pass reads it); decided and counted in Compile
          if (g_stencil_dead_poison.load(std::memory_order_relaxed)) clear_stencil = 0xA5;   // poison: issued, with garbage
          else depth_aspects &= ~VkImageAspectFlags(VK_IMAGE_ASPECT_STENCIL_BIT);
        }
        attachments[count].aspectMask = depth_aspects;
        attachments[count].colorAttachment = 0;
        attachments[count].clearValue.depthStencil = {clear.depth, clear_stencil};
        if (depth_aspects) ++count;
      }
      if (count) {
        VkClearRect rect{};
        rect.layerCount = 1;
        if (clear.full) {
          rect.rect.extent = {pass.width, pass.height};
        } else {
          const int64_t left = std::clamp<int64_t>(int64_t(float(clear.rect.x) * factor_x), 0, pass.width);
          const int64_t top = std::clamp<int64_t>(int64_t(float(clear.rect.y) * factor_y), 0, pass.height);
          const int64_t right = std::clamp<int64_t>(int64_t(std::ceil(float(clear.rect.x + int64_t(clear.rect.width)) * factor_x)), left, pass.width);
          const int64_t bottom = std::clamp<int64_t>(int64_t(std::ceil(float(clear.rect.y + int64_t(clear.rect.height)) * factor_y)), top, pass.height);
          rect.rect.offset = {int32_t(left), int32_t(top)};
          rect.rect.extent = {uint32_t(right - left), uint32_t(bottom - top)};
        }
        if (rect.rect.extent.width && rect.rect.extent.height) vkCmdClearAttachments(cmd, count, attachments, 1, &rect), pc.clears += count;  // S102 M1
      }
      continue;
    }
    if (op.kind != OpKind::Draw) continue;
    const DrawOp& draw = frame.draws[op.index];
    bool mod_scissor = false;  // S102 G10
    if (draw_mod) {
      const uint16_t program = frame.states[draw.state].program;
      if (program < draw_mod_match.size() && draw_mod_match[program]) {
        if (draw_mod == 1) continue;
        mod_scissor = true;
      }
    }

    VkPipeline pipe_now = VK_NULL_HANDLE;
    bool af_this = false;  // S102 R18-B3: this draw runs with its blend switched off
    uint8_t dyn_compare = frame.states[draw.state].depth_compare;  // S111 DYNSTATE: the compare this draw runs with (the cut's plant 1 changes it)
    if (cut_on && cut_plant == 1 && cut_eligible[i - pass.first_op]) {  // S102 G10 plant 1 only (review F5: the real cut changes no op: eligible draws already test GREATER_OR_EQUAL)
      VkPipeline& main_pipe = cut_main_pipes[draw.state];
      if (!main_pipe) {
        DrawState ms = frame.states[draw.state];
        ms.depth_compare = colour_cuts::MainCompare(ms.depth_compare, cut_plant);
        main_pipe = GetPipeline(ms, pass_class);
      }
      pipe_now = main_pipe;
      dyn_compare = colour_cuts::MainCompare(dyn_compare, cut_plant);  // S111 DYNSTATE
    } else if (af_blend && ((draw.flags & kDrawOpBlendOff) || (af_plant >= 1 && af_plant <= 3 && af_all_arena[frame.states[draw.state].program] && (af_plant == 1 || af_all_arena[frame.states[draw.state].program] == (af_plant == 2 ? arena_fragments::kClassDefaultBase : arena_fragments::kClassVertexLit))))) {
      // S102 R18-B3 mask 1: the pipeline of this state without the plain alpha blend (the binder proved the output alpha EXACTLY 1: arena_fragments.h). One pipeline per state a pass, no draw record added.
      auto& slot_af = af_pipes[draw.state];
      if (!slot_af.second) {
        DrawState ds = frame.states[draw.state];
        slot_af.second = 2;
        bool drop = af_plant >= 1 && af_plant <= 3 && (draw.flags & kDrawOpBlendOff) == 0;  // plants 1-3: the planted family, whatever its blend (translucent overlays too)
        if (!drop) {
          arena_fragments::BlendIn in[kMaxColorTargets];
          bool clamps[kMaxColorTargets] = {};
          for (uint32_t c = 0; c < targets.color_count && c < kMaxColorTargets; ++c) {
            const BlendState& b = ds.blend[c];
            in[c] = {b.enable != 0, b.src_color, b.dst_color, b.color_op, b.src_alpha, b.dst_alpha, b.alpha_op, b.write_mask};
            switch (images[targets.color[c].image].desc.format) {  // review F1: fixed-point targets clamp the source alpha to [0,1] before blending; float targets do not
              case Format::RGBA8: case Format::BGRA8: case Format::RGBA8_SNORM: case Format::R8: case Format::RG8: case Format::RGBA4: case Format::R5G6B5: case Format::A1RGB5:
              case Format::RGB10A2: case Format::RGBA16_UNORM: case Format::RG16_UNORM: case Format::R16_UNORM: clamps[c] = true; break;
              default: break;
            }
          }
          int why = 0;
          drop = arena_fragments::CanDropBlend(in, clamps, std::min<uint32_t>(targets.color_count, kMaxColorTargets), &why);
          if (!drop) ++af_declined[why];
        }
        if (drop) {
          for (uint32_t c = 0; c < kMaxColorTargets; ++c) ds.blend[c].enable = 0;
          slot_af.first = GetPipeline(ds, pass_class), slot_af.second = slot_af.first ? 1 : 2;
        }
      }
      if (slot_af.second == 1) {
        pipe_now = slot_af.first;
        ++af_off_draws;
        af_this = true;
      } else {
        auto& cached = state_pipelines[draw.state];
        if (!cached.first || cached.second != pass_class) cached = {GetPipeline(frame.states[draw.state], pass_class), pass_class};
        pipe_now = cached.first;
      }
    } else {
      auto& cached = state_pipelines[draw.state];
      if (!cached.first || cached.second != pass_class) cached = {GetPipeline(frame.states[draw.state], pass_class), pass_class};
      pipe_now = cached.first;
    }
    if (!pipe_now) continue;  // named by BuildPipeline
    if (pipe_now != bound_pipeline) {
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_now);
      bound_pipeline = pipe_now;
      ++pc.pipeline_binds;  // S102 M1
    }
    if (dyn_on) {  // S111 DYNSTATE: for EVERY draw (two draws on one pipeline differ in exactly this state); only a changed value is a command
      if (af_this || dyn_compare != frame.states[draw.state].depth_compare) {   // a twin: the state it was built from, not the draw op's
        DrawState es = frame.states[draw.state];
        es.depth_compare = dyn_compare;
        if (af_this)
          for (uint32_t c = 0; c < kMaxColorTargets; ++c) es.blend[c].enable = 0;
        DynApply(cmd, dyn_last, es, pass_class, dyn_n);
      } else {
        DynApply(cmd, dyn_last, frame.states[draw.state], pass_class, dyn_n);
      }
    }

    if (draw.dynamic != bound_dynamic) {
      bound_dynamic = draw.dynamic;
      const DynamicState& dyn = frame.dynamics[draw.dynamic];
      if (counting) {  // S102 M1: the area bound of this block: viewport (clamped to the pass) intersected with the scissor, integer pixels
        const auto lo = [](float v) { return int64_t(std::floor(v)); };
        const auto hi = [](float v) { return int64_t(std::ceil(v)); };
        const float vx = dyn.viewport[0] * factor_x, vy = dyn.viewport[1] * factor_y, vw = dyn.viewport[2] * factor_x, vh = dyn.viewport[3] * factor_y;
        const int64_t vx0 = std::clamp<int64_t>(lo(vx), 0, pass_w), vy0 = std::clamp<int64_t>(lo(vy), 0, pass_h);
        const int64_t vx1 = std::clamp<int64_t>(hi(vx + vw), vx0, pass_w), vy1 = std::clamp<int64_t>(hi(vy + vh), vy0, pass_h);
        const int64_t sx0 = std::clamp<int64_t>(std::max(0, int32_t(float(dyn.scissor.x) * factor_x)), 0, pass_w), sy0 = std::clamp<int64_t>(std::max(0, int32_t(float(dyn.scissor.y) * factor_y)), 0, pass_h);
        const int64_t sx1 = std::clamp<int64_t>(sx0 + int64_t(std::ceil(float(dyn.scissor.width) * factor_x)), sx0, pass_w), sy1 = std::clamp<int64_t>(sy0 + int64_t(std::ceil(float(dyn.scissor.height) * factor_y)), sy0, pass_h);
        const int64_t ax0 = std::max(vx0, sx0), ay0 = std::max(vy0, sy0), ax1 = std::min(vx1, sx1), ay1 = std::min(vy1, sy1);
        dyn_area = ax1 > ax0 && ay1 > ay0 ? uint64_t(ax1 - ax0) * uint64_t(ay1 - ay0) : 0;
      }
      ++pc.dynamic_sets, ++pc.push_constants;  // S102 M1
      VkViewport viewport{dyn.viewport[0] * factor_x, dyn.viewport[1] * factor_y, dyn.viewport[2] * factor_x, dyn.viewport[3] * factor_y, dyn.viewport[4], dyn.viewport[5]};
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      VkRect2D scissor{};
      scissor.offset = {std::max(0, int32_t(float(dyn.scissor.x) * factor_x)), std::max(0, int32_t(float(dyn.scissor.y) * factor_y))};
      scissor.extent = {uint32_t(std::ceil(float(dyn.scissor.width) * factor_x)), uint32_t(std::ceil(float(dyn.scissor.height) * factor_y))};
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      vkCmdSetDepthBias(cmd, dyn.depth_bias_constant, dyn.depth_bias_clamp, dyn.depth_bias_slope);
      vkCmdSetBlendConstants(cmd, dyn.blend_constants);
      vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_compare_mask);
      vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_write_mask);
      vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_reference);
      vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dyn.system), dyn.system);
    }

    if (ps_on && !layered_pass) {  // S102 G10: a statistics query per run of same-named programs (S102 C1 review F6: not inside a multiview pass)
      const uint16_t program = frame.states[draw.state].program;
      const char* name = program < programs.size() && programs[program].name ? programs[program].name : "?";
      const bool per_draw = g_draw_log.load(std::memory_order_relaxed) != nullptr;  // S102 C4: one query per draw, with a line of its state
      if (name != ps_open_name || per_draw) {
        if (ps_open) vkCmdEndQuery(cmd, ps_pool, ps_count - 1);
        ps_open = ps_count < ps_cap;
        if (ps_open) vkCmdBeginQuery(cmd, ps_pool, ps_count++, 0), ps_names.push_back(name);
        if (ps_open && per_draw) {
          const DrawState& dst = frame.states[draw.state];
          const BlendState& b0 = dst.blend[0];
          char line[512];
          std::snprintf(line, sizeof line, "f %llu p %u i %u %s | blend %u %u %u %u wm %u dt %u dw %u cmp %u st %u alphaop %u tris %llu area %llu tex %u vb %u",
                        static_cast<unsigned long long>(serial), trace_pass, i, name, unsigned(b0.enable), unsigned(b0.src_color), unsigned(b0.dst_color), unsigned(b0.color_op), unsigned(b0.write_mask), unsigned(dst.depth_test),
                        unsigned(dst.depth_write), unsigned(dst.depth_compare), unsigned(dst.stencil_test), unsigned((draw.flags & kDrawOpAlphaOpaque) != 0), static_cast<unsigned long long>(uint64_t(draw.count) * draw.instances / 3),
                        static_cast<unsigned long long>(dyn_area), unsigned(draw.textures), unsigned(draw.vertex[0].buffer == kFrameGeometry));
          ps_meta.resize(ps_count), ps_meta[ps_count - 1] = line;
          ps_afoff.resize(ps_count), ps_afoff[ps_count - 1] = af_this ? 1 : 0;  // S102 R18-B3
        }
        ps_open_name = name;
      }
    }
    if (mod_scissor) {  // S102 G10: 1x1 scissor for this draw; the next draw re-sets its own block
      const VkRect2D tiny{{0, 0}, {1, 1}};
      vkCmdSetScissor(cmd, 0, 1, &tiny);
      bound_dynamic = ~0u;
    }
    const uint32_t offsets[2] = {uint32_t(ring_base + draw.vs_constants),
                                 uint32_t((draw.flags & kDrawOpPrevConstants) ? prev_constants_base + draw.vs_constants : ring_base + draw.fs_constants)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, offsets);
    ++pc.descriptor_binds;  // S102 M1
    if (draw.textures != bound_textures) {
      VkDescriptorSet set = GetTextureSet(frame, draw.textures, slot);
      if (!set) continue;  // named by GetTextureSet
      bound_textures = draw.textures;
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
      ++pc.descriptor_binds;  // S102 M1
    }

    const VkDeviceSize geometry_base = ring_base + AlignUp(frame.constants.count, 16);
    for (uint32_t b = 0; b < 2; ++b) {
      if (draw.vertex[b].buffer == kNoBuffer) continue;
      if (draw.vertex[b].buffer == bound_vertex[b].buffer && draw.vertex[b].offset == bound_vertex[b].offset) continue;
      bound_vertex[b] = draw.vertex[b];
      const bool frame_owned = draw.vertex[b].buffer == kFrameGeometry;
      VkBuffer buffer = frame_owned ? ring : buffers[draw.vertex[b].buffer].buffer;
      VkDeviceSize offset = draw.vertex[b].offset + (frame_owned ? geometry_base : 0);
      vkCmdBindVertexBuffers(cmd, b, 1, &buffer, &offset);
      ++pc.vertex_binds;  // S102 M1
    }
    if (gs_on) {   // S102 R24-F: the draw is issued below: close the segment before it when its group differs (and the segment is long enough, and the pool has room)
      const uint8_t g = frame.draw_groups[op.index];
      if (gs_plan.NeedStamp(g)) {
        const uint32_t q = slot.gs_count++;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.gs_pool, q);
        const uint32_t enc = rdr::kPoolBit | q;
        rdr::GroupSeg seg;
        seg.begin = gs_prev, seg.end = enc, seg.pass_end_stamp = uint16_t(gs_pass.end_stamp);
        gs_plan.Close(seg);
        if (!(gs_plant == 1 && q == 2)) slot.gs_segs.push_back(seg);   // plant 1: the stamp is written, its segment is not recorded: the chain breaks
        gs_prev = enc;
        --gs_plan.budget;
      } else if (gs_plan.draws > 0 && g != gs_plan.last && gs_plan.draws >= std::max<uint32_t>(1, gs_plan.min_run) && gs_plan.budget == 0) {
        slot.gs_overflow = true;   // out of stamps: the rest of the pass is one mixed segment (labelled by its dominant group)
      }
      if (!(gs_plant == 2 && gs_drawn == 4)) gs_plan.Add(g);   // plant 2: one draw is left out of the segment counts (coverage 100 % breaks)
      else gs_plan.last = g;
      ++gs_drawn;
    }
    const bool occ_on = trace && !layered_pass && slot.occ && slot.occ_count < 512;   // (S102 C1 review F6: not inside a multiview pass)  // S102 G11
    if (occ_on) vkCmdBeginQuery(cmd, slot.occ, slot.occ_count, device.caps().occlusion_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0), slot.occ_meta.push_back(trace_pass), slot.occ_meta.push_back(i), ++slot.occ_count;
    if (draw.index.buffer != kNoBuffer) {
      if (draw.index.buffer != bound_index.buffer || draw.index.offset != bound_index.offset || draw.index_32bit != bound_index_32) {
        bound_index = draw.index;
        bound_index_32 = draw.index_32bit;
        const bool frame_owned = draw.index.buffer == kFrameGeometry;
        VkBuffer buffer = frame_owned ? ring : buffers[draw.index.buffer].buffer;
        vkCmdBindIndexBuffer(cmd, buffer, draw.index.offset + (frame_owned ? geometry_base : 0), draw.index_32bit ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        ++pc.index_binds;  // S102 M1
      }
      int32_t draw_base_vertex = draw.base_vertex;
      if (crumbs::g_plant.load(std::memory_order_relaxed) == 5 && !plant5_done) plant5_done = true, draw_base_vertex += 1 << 22;  // S102 D1 TEST ONLY (rplay --crumbs-plant 5): one vertex fetch a frame far past its vertex buffer: GPU-assisted validation must flag it
      vkCmdDrawIndexed(cmd, draw.count, draw.instances, draw.first, draw_base_vertex, 0);
      ++pc.indexed_draws, pc.indices += uint64_t(draw.count) * draw.instances;  // S102 M1
    } else {
      vkCmdDraw(cmd, draw.count, draw.instances, draw.first, 0);
      ++pc.draws, pc.vertices += uint64_t(draw.count) * draw.instances;  // S102 M1
    }
    if (occ_on) vkCmdEndQuery(cmd, slot.occ, slot.occ_count - 1);  // S102 G11
    pc.area_px += dyn_area;  // S102 M1
    if (trace) {  // S102 G11: the draw's own state: blend of target 0, depth, the viewport rectangle, the pixels bound (area)
      const DrawState& ds = frame.states[draw.state];
      const DynamicState& dy = frame.dynamics[draw.dynamic];
      std::fprintf(trace, "{\"t\":\"d\",\"f\":%llu,\"p\":%u,\"op\":%u,\"area\":%llu,\"verts\":%llu,\"blend\":[%u,%u,%u,%u,%u,%u,%u,%u],\"dt\":%u,\"dw\":%u,\"st\":%u,\"vp\":[%.1f,%.1f,%.1f,%.1f],\"sc\":[%d,%d,%u,%u],\"tex\":%u,\"prog\":%u}\n",
                   static_cast<unsigned long long>(serial), trace_pass, i, static_cast<unsigned long long>(dyn_area), static_cast<unsigned long long>(uint64_t(draw.count) * draw.instances),
                   unsigned(ds.blend[0].enable), unsigned(ds.blend[0].src_color), unsigned(ds.blend[0].dst_color), unsigned(ds.blend[0].color_op), unsigned(ds.blend[0].src_alpha), unsigned(ds.blend[0].dst_alpha), unsigned(ds.blend[0].alpha_op), unsigned(ds.blend[0].write_mask),
                   unsigned(ds.depth_test), unsigned(ds.depth_write), unsigned(ds.stencil_test), dy.viewport[0], dy.viewport[1], dy.viewport[2], dy.viewport[3], int(dy.scissor.x), int(dy.scissor.y), unsigned(dy.scissor.width), unsigned(dy.scissor.height),
                   unsigned(draw.textures), unsigned(ds.program));
    }
    if (draw_census_on) {  // S102 G10 (rplay --draw-census): the scene colour passes' draws per program name
      const uint16_t program = frame.states[draw.state].program;
      DrawCensusRow& row = census_rows[program < programs.size() && programs[program].name ? programs[program].name : "?"];
      ++row.draws, row.indices += uint64_t(draw.count) * draw.instances, row.area_px += dyn_area;
    }
    last_draw = &draw;
    dyn_area_now = dyn_area;
    if (plant_here && i == plant_op) plant_draw(draw);
  }
  if (plant_here && !plant_done && last_draw) plant_draw(*last_draw);  // (the pass's last draw op was skipped: the last one that was recorded, state still bound)
  if (gs_on) {   // S102 R24-F: the last segment ends at the pass's own closing stamp (the step loop writes it at index end_stamp, right after this pass)
    rdr::GroupSeg seg;
    seg.begin = gs_prev, seg.end = gs_pass.end_stamp, seg.pass_end_stamp = uint16_t(gs_pass.end_stamp);
    gs_plan.Close(seg);
    slot.gs_segs.push_back(seg);
    gs_pass.segs = uint32_t(slot.gs_segs.size()) - gs_pass.first_seg;
    gs_pass.issued = uint32_t(uint64_t(pc.draws) + pc.indexed_draws - gs_issued0);
    slot.gs_passes.push_back(gs_pass);
  }
  if (ps_open) vkCmdEndQuery(cmd, ps_pool, ps_count - 1);
  if (af_blend) {  // S102 R18-B3: the pass's counts, added once (relaxed, fixed slots: nothing per draw)
    if (af_off_draws) arena_fragments::g_blend_off.fetch_add(af_off_draws, std::memory_order_relaxed), arena_fragments::g_passes_taken.fetch_add(1, std::memory_order_relaxed);
    for (int r = 0; r < arena_fragments::kReasonCount; ++r)
      if (af_declined[r]) arena_fragments::g_declined[r].fetch_add(af_declined[r], std::memory_order_relaxed);
  }
  if (draw_census_on) {
    std::lock_guard<std::mutex> lock(g_draw_census_mutex);
    for (const auto& r : census_rows) {
      DrawCensusRow& to = g_draw_census_rows[r.first];
      to.draws += r.second.draws, to.indices += r.second.indices, to.area_px += r.second.area_px;
    }
  }
  if (sd_on && g_stencil_dead_poison.load(std::memory_order_relaxed) && pass.stencil_store == VK_ATTACHMENT_STORE_OP_DONT_CARE && targets.depth.image != kNoImage && (images[targets.depth.image].aspect & VK_IMAGE_ASPECT_STENCIL_BIT)) {
    VkClearAttachment poison{};   // poison: a pass that does not store its stencil leaves garbage in it
    poison.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT, poison.clearValue.depthStencil = {0.0f, 0xA5};
    VkClearRect prect{};
    prect.layerCount = 1, prect.rect.extent = {pass.width, pass.height};
    vkCmdClearAttachments(cmd, 1, &poison, 1, &prect);
  }
  if (dyn_n.blend_masked || dyn_n.blend_illegal) DynNote(dyn_n, pass_class);   // S111 DYNSTATE: the format-caps counts of a blend set per draw
  vkCmdEndRenderPass(cmd);
  if (tail_begin < pass.end_op) RecordHalfTail(cmd, frame, pass, slot, ring_base, half_skip, pass_class, factor_x, factor_y, half_prep, pc);  // S102 C4
  if (crumb_step) {  // S102 D1: the pass's counts into its crumb (the counters above, no per-draw work), its first draw op's and last recorded draw's programs
    crumb_step->draws = pc.draws + pc.indexed_draws, crumb_step->binds = pc.pipeline_binds;
    crumb_step->prims = uint32_t(std::min<uint64_t>(pc.vertices + pc.indices, 0xFFFFFFFFull));
    crumb_step->last_prog = last_draw ? frame.states[last_draw->state].program : uint16_t(0xFFFF);
    for (uint32_t i = pass.first_op; i < pass.end_op; ++i)
      if (frame.ops[i].kind == OpKind::Draw) { crumb_step->first_prog = frame.states[frame.draws[frame.ops[i].index].state].program; break; }
  }
  if (counting) {  // S102 M1
    pc.barriers = counts_cur.total.barriers - barriers_before;
    pc.barrier_images = counts_cur.total.barrier_images - barrier_images_before;
    PassCounts record;
    record.index = uint32_t(&pass - passes.data()), record.width = pass.width, record.height = pass.height, record.color_count = targets.color_count, record.first_op = pass.first_op;
    record.target_image = first_target.image, record.target_mip = first_target.mip, record.target_layer = first_target.layer;
    record.target_format = uint32_t(images[first_target.image].desc.format);
    record.calls = pc;
    counts_cur.pass.push_back(record);
    pc.barriers = pc.barrier_images = 0;  // (already in the frame total: FlushBarriers added them there)
    counts_cur.total.Add(pc);
    ++counts_cur.passes;
  }
}

Renderer::Impl::CopyGeom Renderer::Impl::GeometryOf(const CopyOp& copy) {
  CopyGeom g;
  ImageEntry& source = images[copy.source.image];
  ImageEntry& dest = images[copy.dest.image];
  g.source = &source, g.dest = &dest;
  g.source_sub = copy.source.mip * source.layers + copy.source.layer;
  g.dest_sub = copy.dest.mip * dest.layers + copy.dest.layer;
  g.whole = copy.dest_rect.x == 0 && copy.dest_rect.y == 0 && copy.dest_rect.width >= dest.desc.width && copy.dest_rect.height >= dest.desc.height;
  const float source_factor = (source.desc.usage & kUsageScaled) ? scale : 1.0f;
  const float dest_factor = (dest.desc.usage & kUsageScaled) ? scale : 1.0f;
  const auto origin = [](int32_t value, float factor) { return int32_t(float(value) * factor); };
  const auto extent = [](uint32_t value, float factor) { return uint32_t(std::ceil(float(value) * factor)); };
  g.source_x = origin(copy.source_rect.x, source_factor), g.source_y = origin(copy.source_rect.y, source_factor);
  g.dest_x = origin(copy.dest_rect.x, dest_factor), g.dest_y = origin(copy.dest_rect.y, dest_factor);
  const uint32_t source_limit_w = std::max(1u, source.width >> copy.source.mip), source_limit_h = std::max(1u, source.height >> copy.source.mip);
  const uint32_t dest_limit_w = std::max(1u, dest.width >> copy.dest.mip), dest_limit_h = std::max(1u, dest.height >> copy.dest.mip);
  g.source_w = std::min(extent(copy.source_rect.width, source_factor), source_limit_w - uint32_t(g.source_x));
  g.source_h = std::min(extent(copy.source_rect.height, source_factor), source_limit_h - uint32_t(g.source_y));
  g.dest_w = std::min(extent(copy.dest_rect.width, dest_factor), dest_limit_w - uint32_t(g.dest_x));
  g.dest_h = std::min(extent(copy.dest_rect.height, dest_factor), dest_limit_h - uint32_t(g.dest_y));
  if (!depth_only_copies.empty()) g.depth_only = std::find(depth_only_copies.begin(), depth_only_copies.end(), &copy) != depth_only_copies.end();   // S102 R24-C
  const bool same_block = source.format == dest.format || (!source.depth && !dest.depth && BlockBytes(source.desc.format) != 0 && BlockBytes(source.desc.format) == BlockBytes(dest.desc.format));
  if (same_block && g.source_w == g.dest_w && g.source_h == g.dest_h) g.path = 1;
  else if (!source.depth && !dest.depth && device.FormatHas(source.format, VK_FORMAT_FEATURE_BLIT_SRC_BIT) && device.FormatHas(dest.format, VK_FORMAT_FEATURE_BLIT_DST_BIT)) g.path = 2;
  return g;
}

VkImageCopy Renderer::Impl::RegionOf(const CopyOp& copy, const CopyGeom& g) {
  VkImageCopy region{};
  const VkImageAspectFlags aspect_s = g.depth_only ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : g.source->aspect, aspect_d = g.depth_only ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : g.dest->aspect;   // S102 R24-C
  region.srcSubresource = {aspect_s, copy.source.mip, copy.source.layer, 1};
  region.srcOffset = {g.source_x, g.source_y, 0};
  region.dstSubresource = {aspect_d, copy.dest.mip, copy.dest.layer, 1};
  region.dstOffset = {g.dest_x, g.dest_y, 0};
  region.extent = {g.source_w, g.source_h, 1};
  return region;
}
