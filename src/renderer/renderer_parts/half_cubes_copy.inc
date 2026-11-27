
// S102 C4 (half_trans.h): the half-size tail's images, pass keys and the two internal pipelines. false = decline (the tail then stays in the pass, drawn at full size).
VkDescriptorSet Renderer::Impl::MakeHalfSet(FrameSlot& slot, ImageEntry* a, ImageEntry* b, ImageEntry* c) {
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = slot.descriptors;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &textures_layout;
  if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
  VkDescriptorImageInfo infos[kMaxTextures];
  ImageEntry& dummy = images[dummy_image];
  for (uint32_t i = 0; i < kMaxTextures; ++i) infos[i] = {dummy_sampler, dummy.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (a) infos[0] = {dummy_sampler, a->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (b) infos[1] = {dummy_sampler, b->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (c) infos[2] = {dummy_sampler, c->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  WriteTextureSet(set, infos);
  return set;
}

bool Renderer::Impl::PrepareHalf(const PassPlan& pass, FrameSlot& slot, HalfPrep& prep) {
  const TargetsOp& targets = *pass.targets;
  ImageEntry& color = images[targets.color[0].image];
  ImageEntry& depth = images[targets.depth.image];
  prep.hw = std::max<uint32_t>(1, (pass.width + 1) / 2), prep.hh = std::max<uint32_t>(1, (pass.height + 1) / 2);
  const bool made = half_color != kNoImage && half_depth_img != kNoImage && half_w == prep.hw && half_h == prep.hh && images[half_color].desc.format == color.desc.format && images[half_depth_img].desc.format == depth.desc.format;
  if (!made) {
    if (half_color != kNoImage) self->DestroyImage(half_color), half_color = kNoImage;
    if (half_depth_img != kNoImage) self->DestroyImage(half_depth_img), half_depth_img = kNoImage;
    ImageDesc desc;
    desc.width = uint16_t(prep.hw), desc.height = uint16_t(prep.hh);
    desc.format = color.desc.format;
    desc.usage = kUsageSampled | kUsageTarget;
    half_color = self->CreateImage(desc);
    desc.format = depth.desc.format;
    half_depth_img = self->CreateImage(desc);
    half_w = prep.hw, half_h = prep.hh;
  }
  if (half_color == kNoImage || half_depth_img == kNoImage) return false;
  ImageEntry& hc = images[half_color];
  ImageEntry& hd = images[half_depth_img];
  if (!hc.sampled_view || !hd.sampled_view || !hd.depth) return false;
  const uint8_t dont_care = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  // (1) the half-size depth: depth only, written by one triangle that reads the full-size depth
  RenderPassKey& dk = prep.depth_key;
  dk = RenderPassKey{};
  dk.formats[kMaxColorTargets] = uint16_t(hd.format), dk.load[kMaxColorTargets] = VK_ATTACHMENT_LOAD_OP_DONT_CARE, dk.store[kMaxColorTargets] = VK_ATTACHMENT_STORE_OP_STORE;
  dk.stencil_load = dont_care, dk.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE, dk.color_count = 0, dk.has_depth = 1, dk.samples = 1;
  prep.depth_class = PassClassKey{};
  prep.depth_class.formats[kMaxColorTargets] = uint16_t(hd.format), prep.depth_class.has_depth = 1, prep.depth_class.samples = 1;
  // (2) the half-size draws: cleared colour, the half-size depth loaded
  RenderPassKey& hk = prep.half_key;
  hk = RenderPassKey{};
  hk.formats[0] = uint16_t(hc.format), hk.load[0] = VK_ATTACHMENT_LOAD_OP_CLEAR, hk.store[0] = VK_ATTACHMENT_STORE_OP_STORE;
  hk.formats[kMaxColorTargets] = uint16_t(hd.format), hk.load[kMaxColorTargets] = VK_ATTACHMENT_LOAD_OP_LOAD, hk.store[kMaxColorTargets] = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  hk.stencil_load = dont_care, hk.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE, hk.color_count = 1, hk.has_depth = 1, hk.samples = 1;
  // (3) the add-back: the full-size colour loaded, no depth attachment (the full-size depth is read as a texture)
  RenderPassKey& ck = prep.comp_key;
  ck = RenderPassKey{};
  ck.formats[0] = uint16_t(color.format), ck.load[0] = VK_ATTACHMENT_LOAD_OP_LOAD, ck.store[0] = pass.store[0];
  ck.stencil_load = dont_care, ck.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE, ck.color_count = 1, ck.has_depth = 0, ck.samples = 1;
  prep.comp_class = PassClassKey{};
  prep.comp_class.formats[0] = uint16_t(color.format), prep.comp_class.color_count = 1, prep.comp_class.samples = 1;
  DrawState depth_state{};
  depth_state.program = half_depth_program;
  depth_state.vertex_layout = present_layout;
  depth_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  depth_state.depth_test = 1, depth_state.depth_write = 1, depth_state.depth_compare = VK_COMPARE_OP_ALWAYS;
  prep.depth_pipe = GetPipeline(depth_state, GetPassClass(prep.depth_class));
  DrawState comp_state{};
  comp_state.program = half_composite_program;
  comp_state.vertex_layout = present_layout;
  comp_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  comp_state.depth_compare = VK_COMPARE_OP_ALWAYS;
  comp_state.blend[0] = {1, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_ADD, 0xF};  // adds rgba: the tail's draws only ever added to the target's alpha, or left it alone (half_trans.h)
  prep.comp_pipe = GetPipeline(comp_state, GetPassClass(prep.comp_class));
  prep.depth_state = depth_state, prep.comp_state = comp_state;   // S111 DYNSTATE: RecordHalfTail sets what the two pipelines do not bake
  if (prep.depth_pipe == VK_NULL_HANDLE || prep.comp_pipe == VK_NULL_HANDLE) return false;
  prep.depth_set = MakeHalfSet(slot, &depth, nullptr, nullptr);
  prep.comp_set = MakeHalfSet(slot, &hc, &hd, &depth);
  if (!prep.depth_set || !prep.comp_set) {
    Problem("descriptor_pool_empty", "no room for the half-size sets: the tail stays in the pass");
    return false;
  }
  return true;
}

// S102 C4: after the pass's first part (its render pass has ended): (1) the half-size depth from the full-size depth, (2) the tail's draws into a cleared half-size colour against it, (3) the half-size colour added
// onto the full-size colour with a depth-aware upsample. Three render passes, a handful of barriers; the fragment work is a quarter of the tail's (plus one half-size triangle and one full-size one).
void Renderer::Impl::RecordHalfTail(VkCommandBuffer cmd, const Frame& frame, const PassPlan& pass, FrameSlot& slot, VkDeviceSize ring_base, const std::vector<uint8_t>& half_skip, uint32_t pass_class, float factor_x, float factor_y, const HalfPrep& prep, CallCounts& pc) {
  const TargetsOp& targets = *pass.targets;
  ImageEntry& color = images[targets.color[0].image];
  ImageEntry& depth = images[targets.depth.image];
  ImageEntry& hc = images[half_color];
  ImageEntry& hd = images[half_depth_img];
  const bool ps_on = g_draw_census.load(std::memory_order_relaxed) && ps_pool && g_draw_census_stats.load(std::memory_order_relaxed);
  bool ps_open = false;
  const auto query_end = [&]() { if (ps_open) vkCmdEndQuery(cmd, ps_pool, ps_count - 1), ps_open = false; };
  const auto query_begin = [&](const std::string& name) {
    if (!ps_on) return;
    query_end();
    ps_open = ps_count < ps_cap;
    if (ps_open) vkCmdBeginQuery(cmd, ps_pool, ps_count++, 0), ps_names.push_back(name);
  };
  const uint32_t const_offsets[2] = {uint32_t(ring_base), uint32_t(ring_base)};
  const int plant = half_trans::g_plant.load(std::memory_order_relaxed);
  float push[16] = {};
  push[0] = plant == 1 ? 1.0f : 0.0f;  // plant 1: the composite's plain bilinear upsample

  // (1) the half-size depth
  Transition(depth, 0, uint32_t(depth.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
  Transition(hd, 0, uint32_t(hd.states.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, true);
  FlushBarriers(cmd);
  {
    VkDescriptorSet set = prep.depth_set;
    FramebufferKey fk{};
    fk.pass = GetRenderPass(prep.depth_key);
    fk.views[kMaxColorTargets] = TargetView(hd, 0, 0);
    fk.width = prep.hw, fk.height = prep.hh;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = fk.pass;
    begin.framebuffer = GetFramebuffer(fk);
    begin.renderArea.extent = {prep.hw, prep.hh};
    if (plant != 3) {  // plant 3: the half-size depth pass is left out (the depth the half draws test against is whatever the image held)
      vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
      query_begin("half|depth");
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prep.depth_pipe);
      { dyn::Applied dl; dyn::Counts dn; DynApply(cmd, dl, prep.depth_state, GetPassClass(prep.depth_class), dn); DynNote(dn, GetPassClass(prep.depth_class)); }   // S111 DYNSTATE
      VkViewport viewport{0, 0, float(prep.hw), float(prep.hh), 0, 1};
      VkRect2D scissor{{0, 0}, {prep.hw, prep.hh}};
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      vkCmdSetScissor(cmd, 0, 1, &scissor);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, const_offsets);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
      vkCmdDraw(cmd, 3, 1, 0, 0);
      query_end();
      vkCmdEndRenderPass(cmd);
      pc.pipeline_binds += 1, pc.descriptor_binds += 2, pc.dynamic_sets += 1, ++pc.draws, pc.vertices += 3;
    }
  }

  // (2) the tail's draws, half size
  Transition(hc, 0, uint32_t(hc.states.size()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, true);
  // (the half pass writes no depth, but its storeOp counts as a write for the barriers after it: the access is READ | WRITE, as the first pass's)
  Transition(hd, 0, uint32_t(hd.states.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, false);
  FlushBarriers(cmd);
  {
    FramebufferKey fk{};
    fk.pass = GetRenderPass(prep.half_key);
    fk.views[0] = TargetView(hc, 0, 0);
    fk.views[kMaxColorTargets] = TargetView(hd, 0, 0);
    fk.width = prep.hw, fk.height = prep.hh;
    VkClearValue clears[kAttachmentSlots] = {};
    clears[0].color = {{0, 0, 0, 0}};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = fk.pass;
    begin.framebuffer = GetFramebuffer(fk);
    begin.renderArea.extent = {prep.hw, prep.hh};
    begin.clearValueCount = 2;
    begin.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    const float half_factor_x = factor_x * 0.5f, half_factor_y = factor_y * 0.5f;
    dyn::Applied half_dyn;   // S111 DYNSTATE: what was last set in this render pass
    dyn::Counts half_dn;
    VkPipeline bound_pipeline = VK_NULL_HANDLE;
    uint32_t bound_dynamic = ~0u, bound_textures = ~0u;
    BufferRange bound_vertex[2];
    BufferRange bound_index;
    uint8_t bound_index_32 = 0xFF;
    const char* open_name = nullptr;
    for (uint32_t i = pass.first_op; i < pass.end_op; ++i) {
      if (!half_skip[i - pass.first_op]) continue;
      const DrawOp& draw = frame.draws[frame.ops[i].index];
      const DrawState& st = frame.states[draw.state];
      VkPipeline pipe = GetPipeline(st, pass_class);
      if (!pipe) continue;
      if (ps_on) {
        const char* name = st.program < programs.size() && programs[st.program].name ? programs[st.program].name : "?";
        if (name != open_name) query_begin(std::string("half|") + name), open_name = name;
      }
      if (pipe != bound_pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        bound_pipeline = pipe;
        ++pc.pipeline_binds;
      }
      DynApply(cmd, half_dyn, st, pass_class, half_dn);   // S111 DYNSTATE: every draw (the tail's draws share few pipelines)
      if (draw.dynamic != bound_dynamic) {
        bound_dynamic = draw.dynamic;
        const DynamicState& dyn = frame.dynamics[draw.dynamic];
        VkViewport viewport{dyn.viewport[0] * half_factor_x, dyn.viewport[1] * half_factor_y, dyn.viewport[2] * half_factor_x, dyn.viewport[3] * half_factor_y, dyn.viewport[4], dyn.viewport[5]};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        VkRect2D scissor{};
        scissor.offset = {std::max(0, int32_t(float(dyn.scissor.x) * half_factor_x)), std::max(0, int32_t(float(dyn.scissor.y) * half_factor_y))};
        scissor.extent = {uint32_t(std::ceil(float(dyn.scissor.width) * half_factor_x)), uint32_t(std::ceil(float(dyn.scissor.height) * half_factor_y))};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdSetDepthBias(cmd, dyn.depth_bias_constant, dyn.depth_bias_clamp, dyn.depth_bias_slope);
        vkCmdSetBlendConstants(cmd, dyn.blend_constants);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_compare_mask);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_write_mask);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, dyn.stencil_reference);
        vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(dyn.system), dyn.system);
        ++pc.dynamic_sets, ++pc.push_constants;
      }
      const uint32_t offsets[2] = {uint32_t(ring_base + draw.vs_constants), uint32_t((draw.flags & kDrawOpPrevConstants) ? prev_constants_base + draw.vs_constants : ring_base + draw.fs_constants)};
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, offsets);
      ++pc.descriptor_binds;
      if (draw.textures != bound_textures) {
        VkDescriptorSet tex = GetTextureSet(frame, draw.textures, slot);
        if (!tex) continue;
        bound_textures = draw.textures;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &tex, 0, nullptr);
        ++pc.descriptor_binds;
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
        ++pc.vertex_binds;
      }
      if (draw.index.buffer != kNoBuffer) {
        if (draw.index.buffer != bound_index.buffer || draw.index.offset != bound_index.offset || draw.index_32bit != bound_index_32) {
          bound_index = draw.index;
          bound_index_32 = draw.index_32bit;
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
    }
    query_end();
    DynNote(half_dn, pass_class);   // S111 DYNSTATE review: the format-caps counts of the tail's blends set per draw (BuildPipeline counted the baked ones)
    vkCmdEndRenderPass(cmd);
  }

  // (3) the add-back onto the full-size colour
  Transition(hc, 0, uint32_t(hc.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
  Transition(hd, 0, uint32_t(hd.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
  Transition(color, targets.color[0].mip * color.layers + targets.color[0].layer, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
             VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, false);
  FlushBarriers(cmd);
  {
    VkDescriptorSet set = prep.comp_set;
    FramebufferKey fk{};
    fk.pass = GetRenderPass(prep.comp_key);
    fk.views[0] = TargetView(color, targets.color[0].mip, targets.color[0].layer);
    fk.width = pass.width, fk.height = pass.height;
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass = fk.pass;
    begin.framebuffer = GetFramebuffer(fk);
    begin.renderArea.extent = {pass.width, pass.height};
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    query_begin("half|composite");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, prep.comp_pipe);
    { dyn::Applied dl; dyn::Counts dn; DynApply(cmd, dl, prep.comp_state, GetPassClass(prep.comp_class), dn); DynNote(dn, GetPassClass(prep.comp_class)); }   // S111 DYNSTATE
    VkViewport viewport{0, 0, float(pass.width), float(pass.height), 0, 1};
    VkRect2D scissor{{0, 0}, {pass.width, pass.height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, const_offsets);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    query_end();
    vkCmdEndRenderPass(cmd);
    pc.pipeline_binds += 1, pc.descriptor_binds += 2, pc.dynamic_sets += 1, ++pc.push_constants, ++pc.draws, pc.vertices += 3;
  }
  if (options.counts) counts_cur.passes += 3;
}

// ---- S102 lane L1b (light_cubes.h, docs\renderer\PRELIGHT_DIRECT.md section 9): the light cubes written by one compute dispatch ----------------------------------------------------------------------
// Made at startup (Renderer::Create, only when RendererOptions::light_cube_compute asks: the start-time value of the switch), never at the first dispatch: a refusal then is known to the binder BEFORE it drops the
// slots' ops (review F2: compute_supported() is false until the pipelines exist), and the first entrance does not pay the compile. lc_state 0 untried, 1 ready, 2 failed.
bool Renderer::Impl::LightCubesInit() {
  if (lc_state != 0) return lc_state == 1;
  lc_state = 2;
  if (g_light_cube_plant.load(std::memory_order_relaxed) == 3) return false;   // planted (review F2): the init fails: every group must be declined, the cubes equal the stream's
  const auto t0 = std::chrono::steady_clock::now();
  VkDevice dev = device.device();
  if (!device.FormatHas(VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) return false;
  VkDescriptorSetLayoutBinding bindings[7] = {};
  bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  for (uint32_t b = 1; b <= uint32_t(kMaxLightCubes) + 1; ++b) bindings[b] = {b, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};   // 1..5 the cubes, 6 the direction table
  VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = 7;
  layout_info.pBindings = bindings;
  if (vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &lc_set_layout) != VK_SUCCESS) return false;
  VkPipelineLayoutCreateInfo pl_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl_info.setLayoutCount = 1;
  pl_info.pSetLayouts = &lc_set_layout;
  if (vkCreatePipelineLayout(dev, &pl_info, nullptr, &lc_layout) != VK_SUCCESS) return false;
  const auto make = [&](const uint32_t* words, size_t bytes, VkShaderModule& module, VkPipeline& pipeline) {
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = bytes, mi.pCode = words;
    if (vkCreateShaderModule(dev, &mi, nullptr, &module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    ci.layout = lc_layout;
    return vkCreateComputePipelines(dev, pipeline_cache, 1, &ci, nullptr, &pipeline) == VK_SUCCESS;
  };
  VkShaderModule unused_swap_module = VK_NULL_HANDLE;
  if (!make(kSpv_rdr_light_cubes_comp, sizeof(kSpv_rdr_light_cubes_comp), lc_module, lc_pipeline)) return false;
  if (!make(kSpv_rdr_light_dirs_comp, sizeof(kSpv_rdr_light_dirs_comp), lc_dirs_module, lc_dirs_pipeline)) return false;
  if (!make(kSpv_rdr_light_dirs_comp_swap, sizeof(kSpv_rdr_light_dirs_comp_swap), unused_swap_module, lc_dirs_pipeline_swap)) return false;
  vkDestroyShaderModule(dev, unused_swap_module, nullptr);   // (a pipeline keeps what it needs)
  const uint32_t n = uint32_t(slots.size());
  VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, n}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, n * (uint32_t(kMaxLightCubes) + 1)}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = n, pool_info.poolSizeCount = 2, pool_info.pPoolSizes = sizes;
  if (vkCreateDescriptorPool(dev, &pool_info, nullptr, &lc_pool) != VK_SUCCESS) return false;
  std::vector<VkDescriptorSetLayout> layouts_(n, lc_set_layout);
  lc_sets.assign(n, VK_NULL_HANDLE);
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = lc_pool, alloc.descriptorSetCount = n, alloc.pSetLayouts = layouts_.data();
  if (vkAllocateDescriptorSets(dev, &alloc, lc_sets.data()) != VK_SUCCESS) return false;
  for (uint32_t s = 0; s < n; ++s) {   // the constants binding never changes: the ring, a dynamic offset per frame (five blocks of 18 vec4)
    VkDescriptorBufferInfo info{ring, 0, uint64_t(kMaxLightCubes) * kLightCubeVec4 * 16};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = lc_sets[s], write.dstBinding = 0, write.descriptorCount = 1, write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, write.pBufferInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
  }
  // The direction table (review F7): 64x64 x 6 RGBA32F, written by rdr_light_dirs.comp when the texel -> grid mapping changes, read by every dispatch. A cube-compatible image so its sampled view is legal.
  ImageDesc table;
  table.width = table.height = uint16_t(kLightCubeSize), table.format = Format::RGBA32F, table.kind = ImageKind::Cube, table.usage = uint8_t(kUsageStorage | kUsageSampled);
  lc_table = self->CreateImage(table);
  if (lc_table == kNoImage) return false;
  lc_state = 1;
  Log("[rdr][light_cubes] compute pipelines made at startup in %.2f ms", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  return true;
}

void Renderer::Impl::RecordLightCubes(VkCommandBuffer cmd, const DispatchOp& op, uint32_t slot_index, VkDeviceSize ring_base) {
  const uint32_t n = std::min<uint32_t>(op.cube_count, uint32_t(kMaxLightCubes));
  if (!n || lc_state != 1 || slot_index >= lc_sets.size()) {
    if (n) Problem("light_cubes_unavailable", "the compute pipeline is not made (%u cubes asked)", n);
    return;
  }
  ImageEntry* cube[kMaxLightCubes] = {};
  for (uint32_t k = 0; k < n; ++k) {
    const ImageId id = op.cube[k];
    if (id == kNoImage || id >= images.high_water() || !images[id].live) { Problem("light_cube_bad", "cube %u is not an image", k); return; }
    ImageEntry& e = images[id];
    if (e.desc.format != Format::RGBA8 || e.width != kLightCubeSize || e.height != kLightCubeSize || e.layers != 6 || e.desc.mips != 1 || !(e.vk_usage & VK_IMAGE_USAGE_STORAGE_BIT)) {
      Problem("light_cube_bad", "cube %u: format %d, %ux%u, %u layers, %u mips, storage %d (a light cube is 64x64 RGBA8 x 6 with storage usage)", k, int(e.desc.format), e.width, e.height, e.layers, unsigned(e.desc.mips), int(!!(e.vk_usage & VK_IMAGE_USAGE_STORAGE_BIT)));
      return;
    }
    cube[k] = &e;
  }
  const int plant = g_light_cube_plant.load(std::memory_order_relaxed);
  ImageEntry& table = images[lc_table];
  // the descriptors of this frame slot: the five cubes (bindings past the cube count repeat the last cube: every statically used binding must be valid) and the table
  VkDescriptorImageInfo infos[kMaxLightCubes + 1];
  VkWriteDescriptorSet writes[kMaxLightCubes + 1];
  for (uint32_t b = 0; b <= uint32_t(kMaxLightCubes); ++b) {
    ImageEntry& target = b < uint32_t(kMaxLightCubes) ? *cube[std::min(b, n - 1)] : table;
    infos[b] = {VK_NULL_HANDLE, TargetView(target, 0, 0, 6), VK_IMAGE_LAYOUT_GENERAL};
    writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[b].dstSet = lc_sets[slot_index], writes[b].dstBinding = 1 + b, writes[b].descriptorCount = 1, writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, writes[b].pImageInfo = &infos[b];
  }
  vkUpdateDescriptorSets(device.device(), uint32_t(kMaxLightCubes) + 1, writes, 0, nullptr);
  const uint32_t offset = uint32_t(ring_base + op.constants);
  // The direction table is made when the texel -> grid mapping (cube 0's K[15], read from this frame's constants) differs from the one it was made with (or the planted face axis changed), not every frame.
  float k15[4];
  std::memcpy(k15, ring_memory.mapped + ring_base + op.constants + 15 * 16, sizeof(k15));
  const bool swap = plant == 2;
  const bool regen = !lc_table_valid || std::memcmp(k15, lc_table_k, sizeof(k15)) != 0 || swap != lc_table_swap;
  if (regen) {
    Transition(table, 0, 6, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, true, true);
    FlushBarriers(cmd);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, swap ? lc_dirs_pipeline_swap : lc_dirs_pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lc_layout, 0, 1, &lc_sets[slot_index], 1, &offset);
    vkCmdDispatch(cmd, kLightCubeSize / kLightCubeGroups, kLightCubeSize / kLightCubeGroups, 6);
    if (options.counts) ++counts_cur.total.dispatches, ++counts_cur.total.pipeline_binds, ++counts_cur.total.descriptor_binds;
    std::memcpy(lc_table_k, k15, sizeof(k15));
    lc_table_valid = true, lc_table_swap = swap;
    ++lc_table_builds;
  }
  // ONE barrier flush for the cubes (every layer of every cube to GENERAL for the shader's writes; their old contents are never read: the dispatch writes every texel) and the table's write -> read
  Transition(table, 0, 6, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false, true);
  for (uint32_t k = 0; k < n; ++k) Transition(*cube[k], 0, 6, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, true, true);
  FlushBarriers(cmd);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lc_pipeline);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, lc_layout, 0, 1, &lc_sets[slot_index], 1, &offset);
  vkCmdDispatch(cmd, kLightCubeSize / kLightCubeGroups, kLightCubeSize / kLightCubeGroups, n * 6);
  if (options.counts) ++counts_cur.total.dispatches, ++counts_cur.total.pipeline_binds, ++counts_cur.total.descriptor_binds;
  if (plant == 1)   // planted: the barrier between the dispatch and the readers is left out (the state says the cubes are readable already): only validation sees it
    for (uint32_t k = 0; k < n; ++k)
      for (SubresourceState& s : cube[k]->states) s.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, s.stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, s.access = VK_ACCESS_SHADER_READ_BIT;
}

void Renderer::Impl::RecordCopy(VkCommandBuffer cmd, const CopyOp& copy) {
  const CopyGeom g = GeometryOf(copy);
  ImageEntry& source = *g.source;
  ImageEntry& dest = *g.dest;

  Transition(source, g.source_sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
  Transition(dest, g.dest_sub, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, g.whole && dest.desc.mips == 1 && dest.layers == 1);
  FlushBarriers(cmd);

  if (g.depth_only && g_stencil_dead_poison.load(std::memory_order_relaxed)) {   // S102 R24-C poison: the destination's stencil is garbage after a depth-only copy
    const VkClearDepthStencilValue garbage{0.0f, 0xA5};
    const VkImageSubresourceRange srange{VK_IMAGE_ASPECT_STENCIL_BIT, copy.dest.mip, 1, copy.dest.layer, 1};
    vkCmdClearDepthStencilImage(cmd, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &garbage, 1, &srange);
  }
  if (g.path == 1) {
    // Every copy the game makes (GAME_RULES_TEXTURES_FRAME C4): raw bits, same size. A depth copy stays a depth image and is read as depth.
    const VkImageCopy region = RegionOf(copy, g);
    vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    CountCopy();  // S102 M1
  } else if (g.path == 2) {
    VkImageBlit blit{};
    blit.srcSubresource = {source.aspect, copy.source.mip, copy.source.layer, 1};
    blit.srcOffsets[0] = {g.source_x, g.source_y, 0};
    blit.srcOffsets[1] = {g.source_x + int32_t(g.source_w), g.source_y + int32_t(g.source_h), 1};
    blit.dstSubresource = {dest.aspect, copy.dest.mip, copy.dest.layer, 1};
    blit.dstOffsets[0] = {g.dest_x, g.dest_y, 0};
    blit.dstOffsets[1] = {g.dest_x + int32_t(g.dest_w), g.dest_y + int32_t(g.dest_h), 1};
    vkCmdBlitImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, BlitFilterFor(source.desc.format, copy.filter == CopyFilter::Linear));
    CountCopy();  // S102 M1
  } else {
    Problem("copy_shape_unsupported", "format %d to %d, %ux%u to %ux%u", int(source.desc.format), int(dest.desc.format), g.source_w, g.source_h, g.dest_w, g.dest_h);
  }
}

// S102 G12: `count` (>= 2) adjacent plain copies of one source into distinct subresources of one destination (copy_batch.h Compatible): every layout change in ONE barrier, ONE
// vkCmdCopyImage with one region each. The regions are the ones RecordCopy would have recorded one by one (RegionOf), in the same order.
void Renderer::Impl::RecordCopyBatch(VkCommandBuffer cmd, const CopyOp* const* copies, const CopyGeom* geoms, uint32_t count) {
  ImageEntry& source = *geoms[0].source;
  ImageEntry& dest = *geoms[0].dest;
  VkImageCopy regions[rdr::kMaxBatch];
  for (uint32_t i = 0; i < count; ++i) {
    Transition(source, geoms[i].source_sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
    Transition(dest, geoms[i].dest_sub, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, geoms[i].whole && dest.desc.mips == 1 && dest.layers == 1);
    regions[i] = RegionOf(*copies[i], geoms[i]);
  }
  const int plant = rdr::g_copy_batch_plant.load(std::memory_order_relaxed);
  if (plant != 2) {
    FlushBarriers(cmd);
  } else {  // plant 2: the barrier is left out (state kept as if it had run): only the validation layers see the layout error
    barriers.clear();
    barrier_src = barrier_dst = 0;
  }
  vkCmdCopyImage(cmd, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dest.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, plant == 1 ? count - 1 : count, regions);   // (plant 1: the last region is not copied)
  CountCopy();  // S102 M1: one command, as counted
  rdr::NoteBatch(count);
}
