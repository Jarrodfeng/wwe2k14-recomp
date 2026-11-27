
// ---- caches -----------------------------------------------------------------------------------------------------------------------

VkRenderPass Renderer::Impl::GetRenderPass(const RenderPassKey& key) {
  auto found = render_passes.find(key);
  if (found != render_passes.end()) return found->second;
  VkAttachmentDescription attachments[kAttachmentSlots] = {};
  VkAttachmentReference color_refs[kMaxColorTargets] = {};
  VkAttachmentReference depth_ref{};
  uint32_t count = 0;
  for (uint32_t i = 0; i < key.color_count; ++i) {
    VkAttachmentDescription& a = attachments[count];
    a.format = VkFormat(key.formats[i]);
    a.samples = VkSampleCountFlagBits(key.samples);
    a.loadOp = VkAttachmentLoadOp(key.load[i]);
    a.storeOp = VkAttachmentStoreOp(key.store[i]);
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_refs[i] = {count, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    ++count;
  }
  if (key.has_depth) {
    VkAttachmentDescription& a = attachments[count];
    a.format = VkFormat(key.formats[kMaxColorTargets]);
    a.samples = VkSampleCountFlagBits(key.samples);
    a.loadOp = VkAttachmentLoadOp(key.load[kMaxColorTargets]);
    a.storeOp = VkAttachmentStoreOp(key.store[kMaxColorTargets]);
    a.stencilLoadOp = VkAttachmentLoadOp(key.stencil_load);
    a.stencilStoreOp = VkAttachmentStoreOp(key.stencil_store);
    a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth_ref = {count, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    ++count;
  }
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = key.color_count;
  subpass.pColorAttachments = color_refs;
  subpass.pDepthStencilAttachment = key.has_depth ? &depth_ref : nullptr;
  VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  info.attachmentCount = count;
  info.pAttachments = attachments;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  const uint32_t view_mask = key.views > 1 ? ((1u << key.views) - 1u) & ~(g_layered_plant.load(std::memory_order_relaxed) == 1 ? 1u : 0u) : 0u;   // (plant 1: view 0 is not drawn)   // S102 C1: one pass into several layers (layered_targets.h); the same mask is the correlation mask (the views are one scene)
  VkRenderPassMultiviewCreateInfo multiview{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
  multiview.subpassCount = 1;
  multiview.pViewMasks = &view_mask;
  multiview.correlationMaskCount = 1;
  multiview.pCorrelationMasks = &view_mask;
  if (key.views > 1) info.pNext = &multiview;
  VkRenderPass pass = VK_NULL_HANDLE;
  vkCreateRenderPass(device.device(), &info, nullptr, &pass);
  render_passes.emplace(key, pass);
  return pass;
}

uint32_t Renderer::Impl::GetPassClass(const PassClassKey& key) {
  auto found = pass_classes.find(key);
  if (found != pass_classes.end()) return found->second;
  RenderPassKey compatible{};
  std::memcpy(compatible.formats, key.formats, sizeof(key.formats));
  std::memset(compatible.load, VK_ATTACHMENT_LOAD_OP_LOAD, sizeof(compatible.load));
  std::memset(compatible.store, VK_ATTACHMENT_STORE_OP_STORE, sizeof(compatible.store));
  compatible.color_count = key.color_count;
  compatible.has_depth = key.has_depth;
  compatible.samples = key.samples;
  compatible.views = key.views;   // S102 C1
  const uint32_t id = uint32_t(class_passes.size());
  class_passes.push_back(GetRenderPass(compatible));
  class_keys.push_back(key);
  pass_classes.emplace(key, id);
  return id;
}

VkFramebuffer Renderer::Impl::GetFramebuffer(const FramebufferKey& key) {
  auto found = framebuffers.find(key);
  if (found != framebuffers.end()) return found->second;
  ++g_render_times.new_framebuffers;
  const uint64_t fb_t0 = RenderNow();
  struct FbTimer {
    uint64_t t0;
    ~FbTimer() { g_render_times.fb_ns += RenderNow() - t0; }
  } fb_timer{fb_t0};
  VkImageView views[kAttachmentSlots];
  uint32_t count = 0;
  for (VkImageView view : key.views) {
    if (view) views[count++] = view;
  }
  VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  info.renderPass = key.pass;
  info.attachmentCount = count;
  info.pAttachments = views;
  info.width = key.width;
  info.height = key.height;
  info.layers = 1;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  vkCreateFramebuffer(device.device(), &info, nullptr, &framebuffer);
  framebuffers.emplace(key, framebuffer);
  return framebuffer;
}

VkPipeline Renderer::Impl::BuildPipeline(const DrawState& state, uint32_t pass_class) {
  if (state.program >= programs.size() || state.vertex_layout >= layouts.size()) {  // S102 R21-B (review F5): kNoTableId from a full table (or any id never made) is refused by name
    Problem("pipeline_bad_ids", "program %u of %zu, layout %u of %zu", unsigned(state.program), programs.size(), unsigned(state.vertex_layout), layouts.size());
    return VK_NULL_HANDLE;
  }
  const Program& program = programs[state.program];
  const VertexLayoutDesc& layout = layouts[state.vertex_layout];
  const PassClassKey& pass_key = class_keys[pass_class];

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = program.vertex;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = program.fragment;
  stages[1].pName = "main";
  const bool depth_only_twin = state.variant == kVariantDepthOnly;  // S102 G10: the colour pass's depth-only prepass twin (colour_cuts.h): no fragment stage; the state has every write mask 0
  const uint32_t stage_count = program.fragment && !depth_only_twin ? 2 : 1;

  VkVertexInputBindingDescription bindings[2];
  uint32_t binding_count = 0;
  for (uint32_t i = 0; i < 2; ++i) {
    if (!layout.strides[i]) continue;
    bindings[binding_count++] = {i, layout.strides[i], layout.per_instance[i] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
  }
  VkVertexInputAttributeDescription attributes[kMaxVertexAttributes];
  for (uint32_t i = 0; i < layout.attribute_count; ++i) {
    attributes[i] = {layout.attributes[i].location, layout.attributes[i].binding, VkFormat(layout.attributes[i].format), layout.attributes[i].offset};
    if (!device.VertexFormatReadable(attributes[i].format)) {  // lane 200: a vertex format the device cannot read: said once per pipeline
      if (FirstTime(2, state.program, (uint64_t(layout.attributes[i].location) << 16) ^ uint64_t(attributes[i].format))) {
        ++cap_stats.vertex_unsupported;
        Log("[rdr] format caps: vertex attribute %u of program %s has format %d, which this device cannot read as vertex input", layout.attributes[i].location, program.name ? program.name : "?", int(attributes[i].format));
        CapsChanged();
      }
    }
  }
  VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vertex_input.vertexBindingDescriptionCount = binding_count;
  vertex_input.pVertexBindingDescriptions = bindings;
  vertex_input.vertexAttributeDescriptionCount = layout.attribute_count;
  vertex_input.pVertexAttributeDescriptions = attributes;

  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VkPrimitiveTopology(state.topology);
  const bool strip = assembly.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || assembly.topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || assembly.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
  assembly.primitiveRestartEnable = state.primitive_restart && strip;  // not allowed on lists

  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.depthClampEnable = state.depth_clamp && device.caps().depth_clamp;
  if (state.depth_clamp && !device.caps().depth_clamp) Log("[rdr] this device has no depth clamp: draws that rely on it may lose pixels at the depth limits");
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = state.cull;
  raster.frontFace = VkFrontFace(state.front_face);
  raster.depthBiasEnable = state.depth_bias_enable;
  raster.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = VkSampleCountFlagBits(pass_key.samples);

  const auto stencil_face = [](const StencilFace& face) {
    VkStencilOpState op{};
    op.failOp = VkStencilOp(face.fail);
    op.passOp = VkStencilOp(face.pass);
    op.depthFailOp = VkStencilOp(face.depth_fail);
    op.compareOp = VkCompareOp(face.compare);
    return op;
  };
  VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable = state.depth_test;
  depth.depthWriteEnable = state.depth_write;
  depth.depthCompareOp = VkCompareOp(state.depth_compare);
  depth.stencilTestEnable = state.stencil_test;
  depth.front = stencil_face(state.front);
  depth.back = stencil_face(state.back);

  VkPipelineColorBlendAttachmentState blend[kMaxColorTargets] = {};
  for (uint32_t i = 0; i < pass_key.color_count; ++i) {
    const BlendState& b = state.blend[i];
    blend[i].blendEnable = b.enable;
    // A blend that is a plain write (ONE, ZERO, ADD on colour and alpha) needs no blend feature: it is not touched and not counted.
    const bool plain_write = b.src_color == VK_BLEND_FACTOR_ONE && b.dst_color == VK_BLEND_FACTOR_ZERO && b.color_op == VK_BLEND_OP_ADD && b.src_alpha == VK_BLEND_FACTOR_ONE &&
                             b.dst_alpha == VK_BLEND_FACTOR_ZERO && b.alpha_op == VK_BLEND_OP_ADD;
    if (b.enable && !plain_write && pass_key.formats[i] != VK_FORMAT_UNDEFINED && !device.FormatHas(VkFormat(pass_key.formats[i]), VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) {
      // Lane 200: blending into a format that cannot blend is invalid. The draw writes its colour over the target instead (logged by name).
      if (caps_fallback) {
        blend[i].blendEnable = VK_FALSE;
        if (FirstTime(3, state.program, uint64_t(pass_key.formats[i]))) {
          ++cap_stats.blend_disabled;
          Log("[rdr] format caps: blend off on attachment %u (%s) in program %s", i, CapFormatName(VkFormat(pass_key.formats[i])), program.name ? program.name : "?");
          CapsChanged();
        }
      } else {
        ++cap_stats.illegal_blend;
        CapsChanged();
      }
    }
    blend[i].srcColorBlendFactor = VkBlendFactor(b.src_color);
    blend[i].dstColorBlendFactor = VkBlendFactor(b.dst_color);
    blend[i].colorBlendOp = VkBlendOp(b.color_op);
    blend[i].srcAlphaBlendFactor = VkBlendFactor(b.src_alpha);
    blend[i].dstAlphaBlendFactor = VkBlendFactor(b.dst_alpha);
    blend[i].alphaBlendOp = VkBlendOp(b.alpha_op);
    blend[i].colorWriteMask = b.write_mask;
  }
  VkPipelineColorBlendStateCreateInfo color{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  color.attachmentCount = pass_key.color_count;
  color.pAttachments = blend;

  const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                           VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  // S111 DYNSTATE (dyn_state.h): the draw states this device sets per draw are not baked (the same set for every pipeline of the session: Caps::dyn_mask)
  VkDynamicState dynamic_all[std::size(dynamic_states) + 8];
  uint32_t dynamic_count = 0;
  for (VkDynamicState d : dynamic_states) dynamic_all[dynamic_count++] = d;
  if (device.caps().dyn_mask & dyn::kRaster)
    for (VkDynamicState d : {VK_DYNAMIC_STATE_CULL_MODE_EXT, VK_DYNAMIC_STATE_FRONT_FACE_EXT, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE_EXT, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE_EXT, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP_EXT})
      dynamic_all[dynamic_count++] = d;
  if (device.caps().dyn_mask & dyn::kBlend)
    for (VkDynamicState d : {VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT}) dynamic_all[dynamic_count++] = d;
  dynamic.dynamicStateCount = dynamic_count;
  dynamic.pDynamicStates = dynamic_all;

  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.stageCount = stage_count;
  info.pStages = stages;
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth;
  info.pColorBlendState = &color;
  info.pDynamicState = &dynamic;
  info.layout = pipeline_layout;
  info.renderPass = class_passes[pass_class];
  VkPipeline pipeline = VK_NULL_HANDLE;
  const auto create_start = std::chrono::steady_clock::now();   // lane 271: the line below says what each build cost (a scene start builds ~70 of them in a row)
  if (pipeline_delay_ms) std::this_thread::sleep_for(std::chrono::milliseconds(pipeline_delay_ms));   // test only (the release entrance check's slow-compile simulation), counted in the build time below
  if (vkCreateGraphicsPipelines(device.device(), pipeline_cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
    if (building_prebuilt) Log("[rdr][pipestore] a listed pipeline was refused by the driver: program %s, class %u (dropped from the list)", program.name ? program.name : "?", pass_class);
    else Problem("pipeline_failed", "program %s, layout %u", program.name ? program.name : "?", unsigned(state.vertex_layout));
    return VK_NULL_HANDLE;
  }
  last_build_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - create_start).count());  // lane 276
  const double build_ms = double(last_build_ns) / 1e6;
  if (building_prebuilt) Log("[rdr] pipeline prebuilt from the list: program %s, class %u, %.2f ms", program.name ? program.name : "?", pass_class, build_ms);
  else Log("[rdr] pipeline built on demand: program %s, class %u, %.2f ms", program.name ? program.name : "?", pass_class, build_ms);
  crumb_ring.NoteBuilt(serial, state.program, uint8_t(pass_class));  // S102 D1: the last pipelines built, named in a lost device's dump
  if (crumb) ++crumb->built;
  if (device.debug_names() && vk::vkSetDebugUtilsObjectNameEXT && program.name) {  // validation on: messages name the program
    VkDebugUtilsObjectNameInfoEXT name{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    name.objectType = VK_OBJECT_TYPE_PIPELINE;
    name.objectHandle = uint64_t(pipeline);
    name.pObjectName = program.name;
    vk::vkSetDebugUtilsObjectNameEXT(device.device(), &name);
  }
  return pipeline;
}

VkPipeline Renderer::Impl::GetPipeline(const DrawState& state, uint32_t pass_class) {
  PipelineKey key{};
  const DrawState ks = dyn::KeyState(state, device.caps().dyn_mask);   // S111 DYNSTATE: what this device sets per draw is not in the key (zero), so two draws that differ only there share one pipeline
  std::memcpy(&key.state, &ks, sizeof(DrawState));
  key.pass_class = pass_class;
  auto found = pipelines.find(key);
  if (found != pipelines.end()) {
    if (found->second.unused_record >= 0) {  // lane 276: a prebuilt pipeline drawn for the first time this session
      store_records[size_t(found->second.unused_record)].seen = store_session;
      found->second.unused_record = -1;
      ++store_stats.prebuilt_used;
      ++store_used;
      store_list_dirty = true;
    }
    return found->second.pipeline;
  }
  last_build_ns = 0;
  VkPipeline pipeline = BuildPipeline(ks, pass_class);
  out_pipe_ns += last_build_ns, out_pipe_n += pipeline ? 1 : 0;  // S102 G5
  pipelines.emplace(key, PipelineEntry{pipeline, -1});
  if (store_on && pipeline) StoreNoteBuilt(ks, pass_class, int64_t(last_build_ns));  // (S102 G10 review F9: the prepass twins are listed too: pstore::Sane admits variant 0 / 1)
  return pipeline;
}
