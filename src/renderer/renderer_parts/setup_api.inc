
// ---- setup ------------------------------------------------------------------------------------------------------------------------

bool Renderer::Impl::Init(const RendererOptions& opts, std::string& error) {
  options = opts;
  scale = opts.render_scale;
  DeviceOptions device_options;
  device_options.validation = opts.validation;
  windowed = opts.window.window != nullptr;
  device_options.windowed = windowed;
  device_options.gpu_index = opts.gpu_index;
  device_options.gpu_name = opts.gpu_name;  // S102 U1
  if (opts.dlss_mode >= 0) device_options.streamline_dir = opts.streamline_dir;  // S10b
  device_options.format_caps_mask = opts.format_caps_mask;
  device_options.pipeline_statistics = opts.instrument_stats;  // S102 G10 (review F11)
  device_options.exclusive_fullscreen = opts.exclusive_fullscreen && windowed;  // S102 G2b
  device_options.robust_buffers = opts.robust_buffers;  // S102 D1
  device_options.occlusion_precise = opts.occlusion_precise;  // S102 G11 (F1)
  device_options.multiview = opts.multiview;  // S102 C1 review F2
  device_options.dynamic_state = opts.dynamic_state;  // S111 DYNSTATE
  if (!device.Create(device_options, error)) return false;
  allocator.Init(&device);
  VkDevice dev = device.device();
  caps_fallback = !(opts.format_caps_mask & kCapFallbacksOff);
  bc_mode = opts.bc_decode;
  pipeline_delay_ms = opts.pipeline_delay_ms;
#ifdef RDR_DEV
  if (opts.staging_cap_mb) max_staged = uint64_t(opts.staging_cap_mb) << 20;  // S102 U1 (dev tests)
  staging_wait_plant = opts.staging_wait_plant;
#endif
  {
    // S102 U1: said once, because the missing BC formats are the cause of the Mali-G615 black title card (docs\renderer\TITLE_CARD_MALI.md).
    const bool native = device.FormatHas(VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    Log("[rdr] block-compressed textures (BC1-BC5): %s; bc_decode %u -> %s", native ? "the device samples them" : "the device CANNOT sample them", unsigned(bc_mode),
        bc_mode == 2 ? "decoded on the CPU always (diagnosis)" : (bc_mode == 1 && !native) ? "decoded on the CPU at upload" : (!native ? "REFUSED (off: the v0.3.2 behaviour)" : "sampled as they are"));
  }
  for (uint32_t f = 1; f < uint32_t(Format::Count); ++f) {
    const VkFormat format = ToVk(Format(f), device.caps());
    filterable[f] = format != VK_FORMAT_UNDEFINED && device.FormatHas(format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
    // One line per format that lacks a feature the renderer would use (docs\renderer\FORMAT_CAPS.md). A full-feature device prints none.
    // Integer (R32_UINT) never filters or blends by design, and a format the device cannot sample at all (ASTC / EAC on a PC) is refused by CreateImage by name.
    if (format == VK_FORMAT_UNDEFINED || format == device.caps().depth_format || format == VK_FORMAT_R32_UINT || !device.FormatHas(format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) continue;
    const bool no_linear = !filterable[f];
    const bool no_blend = !device.FormatHas(format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) && device.FormatHas(format, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    if (no_linear || no_blend)
      Log("[rdr] format caps: %s (format %u):%s%s%s", CapFormatName(format), f, no_linear ? " no linear filtering -> sampled and blitted nearest (values unchanged)" : "",
          no_blend ? " no blending as a colour attachment -> blend off on draws into it" : "", caps_fallback ? "" : " [FALLBACKS OFF: asking the driver anyway]");
  }
  if (opts.format_caps_mask) Log("[rdr] format caps: test mask %08x (pretends features are missing)", unsigned(opts.format_caps_mask));

  // The ring: one section per frame in flight, for constants and dynamic geometry.
  VkBufferCreateInfo ring_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  ring_info.size = kRingSection * opts.frames_in_flight;
  ring_info.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
  vkCreateBuffer(dev, &ring_info, nullptr, &ring);
  VkMemoryRequirements ring_needs{};
  vkGetBufferMemoryRequirements(dev, ring, &ring_needs);
  if (!allocator.Alloc(ring_needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, ring_memory)) {
    error = "Not enough graphics memory for the frame ring.";
    return false;
  }
  vkBindBufferMemory(dev, ring, ring_memory.memory, ring_memory.offset);

  // One universal layout: set 0 the two constant blocks, set 1 the textures.
  VkDescriptorSetLayoutBinding constant_bindings[2] = {};
  constant_bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
  constant_bindings[1] = {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};  // S10b: velocity draws read the previous tick's vertex constants here
  VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount = 2;
  layout_info.pBindings = constant_bindings;
  vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &constants_layout);
  // Every texture slot is its own binding (binding = slot), so a shader declares each one as the kind it samples and nothing aliases.
  VkDescriptorSetLayoutBinding texture_bindings[kMaxTextures];
  for (uint32_t b = 0; b < uint32_t(kMaxTextures); ++b)
    texture_bindings[b] = {b, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VkShaderStageFlags(b < uint32_t(kFragmentTextures) ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT), nullptr};
  layout_info.bindingCount = kMaxTextures;
  layout_info.pBindings = texture_bindings;
  vkCreateDescriptorSetLayout(dev, &layout_info, nullptr, &textures_layout);
  VkDescriptorSetLayout set_layouts[2] = {constants_layout, textures_layout};
  VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount = 2;
  pipeline_layout_info.pSetLayouts = set_layouts;
  VkPushConstantRange system_constants{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 16};
  pipeline_layout_info.pushConstantRangeCount = 1;
  pipeline_layout_info.pPushConstantRanges = &system_constants;
  vkCreatePipelineLayout(dev, &pipeline_layout_info, nullptr, &pipeline_layout);

  VkDescriptorPoolSize constant_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &constant_size;
  vkCreateDescriptorPool(dev, &pool_info, nullptr, &constants_pool);
  VkDescriptorSetAllocateInfo set_alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_alloc.descriptorPool = constants_pool;
  set_alloc.descriptorSetCount = 1;
  set_alloc.pSetLayouts = &constants_layout;
  vkAllocateDescriptorSets(dev, &set_alloc, &constants_set);
  VkDescriptorBufferInfo buffer_infos[2] = {{ring, 0, kConstantRange}, {ring, 0, kConstantRange}};
  VkWriteDescriptorSet writes[2] = {};
  for (uint32_t i = 0; i < 2; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = constants_set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[i].pBufferInfo = &buffer_infos[i];
  }
  vkUpdateDescriptorSets(dev, 2, writes, 0, nullptr);

  std::vector<uint8_t> cache_blob;  // lane 276: the driver's cache from the last session (header-checked), else empty
  StoreInit(opts, cache_blob);
  VkPipelineCacheCreateInfo cache_info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  if (!cache_blob.empty()) {
    cache_info.initialDataSize = cache_blob.size();
    cache_info.pInitialData = cache_blob.data();
  }
  if (vkCreatePipelineCache(dev, &cache_info, nullptr, &pipeline_cache) != VK_SUCCESS && !cache_blob.empty()) {
    Log("[rdr][pipestore] cache refused by the driver: starting empty");
    cache_info.initialDataSize = 0;
    cache_info.pInitialData = nullptr;
    store_stats.cache_loaded_bytes = 0;
    vkCreatePipelineCache(dev, &cache_info, nullptr, &pipeline_cache);
  }

  slots.resize(opts.frames_in_flight);
  if (opts.gpu_timestamps) {  // S101 lane22
    uint32_t family_count = 0;
    vk::vkGetPhysicalDeviceQueueFamilyProperties(device.physical(), &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vk::vkGetPhysicalDeviceQueueFamilyProperties(device.physical(), &family_count, families.data());
    gpu_timestamps = device.queue_family() < family_count && families[device.queue_family()].timestampValidBits >= 36;
    ts_valid_bits = device.queue_family() < family_count ? families[device.queue_family()].timestampValidBits : 0;
    ts_period = double(device.properties().limits.timestampPeriod);
    if (!gpu_timestamps) Log("[rdr][gpu] no GPU timestamps: queue family %u has %u valid timestamp bits (need 36)", device.queue_family(), ts_valid_bits);
    else if (opts.gpu_timestamp_plant == 1) ts_period *= 1000.0;  // lane 269 plant: the wrong period, the sanity check must catch it
    if (gpu_timestamps) Log("[rdr][gpu] GPU timestamps on: %u valid bits, period %.3f ns a tick, read %u frames late, no stall", ts_valid_bits, ts_period, opts.frames_in_flight);
  }
  for (FrameSlot& slot : slots) {
    VkCommandPoolCreateInfo command_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    command_pool.queueFamilyIndex = device.queue_family();
    vkCreateCommandPool(dev, &command_pool, nullptr, &slot.pool);
    VkCommandBufferAllocateInfo command_alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_alloc.commandPool = slot.pool;
    command_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_alloc.commandBufferCount = 1;
    vkAllocateCommandBuffers(dev, &command_alloc, &slot.cmd);
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(dev, &fence, nullptr, &slot.fence);
    VkDescriptorPoolSize sampler_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSetsPerFrame * kMaxTextures};
    VkDescriptorPoolCreateInfo texture_pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    texture_pool.maxSets = kMaxSetsPerFrame;
    texture_pool.poolSizeCount = 1;
    texture_pool.pPoolSizes = &sampler_size;
    vkCreateDescriptorPool(dev, &texture_pool, nullptr, &slot.descriptors);
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(dev, &semaphore, nullptr, &slot.available);
    if (gpu_timestamps) MakeStampPool(slot, rdr::g_gpu_stamps.load(std::memory_order_relaxed) ? 192 : 2);  // S101 lane22; S102 S0-3: two queries until stamps first turn on, then sized for a frame's steps (~130 seen), grown when a frame wanted more
  }
  swapchain.ConfigureExclusive(opts.exclusive_fullscreen && windowed);  // S102 G2b: before the first swapchain
  if (windowed && !swapchain.Create(&device, opts.window, opts.vsync, error)) return false;
  if (!windowed && opts.present_offscreen_w && opts.present_offscreen_h) {  // S102 G2
    if (!swapchain.CreateOffscreen(&device, opts.present_offscreen_w, opts.present_offscreen_h, error)) return false;
    offscreen_out = true;
  }
  return true;
}

void Renderer::Impl::Shutdown() {
  VkDevice dev = device.device();
  if (!dev) return;
  vkDeviceWaitIdle(dev);
  for (FrameSlot& s : slots) CollectGpuTime(s);  // lane 269: the last frames
  LogGpuWindow("last window");
  Log("[rdr][render] bc decoded %llu images, %.1f MB in all (%llu levels) at shutdown", (unsigned long long)bc_images.load(), double(bc_decoded_bytes.load()) / 1048576.0, (unsigned long long)bc_decoded_levels.load());  // S102 U1
  StoreSave(true);  // lane 276 (joins the save worker first)
  StoreJoin();
  StoreSummary("at shutdown");
  swapchain.Destroy();
  if (present_sampler) vkDestroySampler(dev, present_sampler, nullptr);
  if (present_sampler_nearest) vkDestroySampler(dev, present_sampler_nearest, nullptr);
  if (device.format_caps_mask() || caps_summary_dirty || cap_stats.linear_to_nearest || cap_stats.blits_to_nearest || cap_stats.blend_disabled || cap_stats.vertex_unsupported) CapsSummary();
  for (FrameSlot& slot : slots) {
    DrainRetired(slot);
    vkDestroySemaphore(dev, slot.available, nullptr);
    vkDestroyDescriptorPool(dev, slot.descriptors, nullptr);
    if (&slot == &slots.back() && lean_pool) vkDestroyDescriptorPool(dev, lean_pool, nullptr), lean_pool = VK_NULL_HANDLE;   // S102 RT1 (after the last slot drained its retired sets)
    vkDestroyFence(dev, slot.fence, nullptr);
    vkDestroyCommandPool(dev, slot.pool, nullptr);
    if (slot.timestamps) vkDestroyQueryPool(dev, slot.timestamps, nullptr);  // S101 lane22
    if (slot.gs_pool) vkDestroyQueryPool(dev, slot.gs_pool, nullptr);  // S102 R24-F
    if (slot.occ) vkDestroyQueryPool(dev, slot.occ, nullptr);  // S102 G11
  }
  if (stamp_scratch) {  // S102 S0-3 (test plant 4)
    vkDestroyBuffer(dev, stamp_scratch, nullptr);
    allocator.Free(stamp_scratch_memory);
  }
  if (marker_buffer) {  // S102 D1
    vkDestroyBuffer(dev, marker_buffer, nullptr);
    allocator.Free(marker_memory);
    marker_buffer = VK_NULL_HANDLE;
  }
  if (selftest_event) vkDestroyEvent(dev, selftest_event, nullptr), selftest_event = VK_NULL_HANDLE;  // S102 D1
  for (auto& item : readback_staging) {  // lane 232
    vkDestroyBuffer(dev, item.second.buffer, nullptr);
    allocator.Free(item.second.memory);
  }
  readback_staging.clear();
  for (auto& item : pipelines) vkDestroyPipeline(dev, item.second.pipeline, nullptr);
  for (auto& item : framebuffers) vkDestroyFramebuffer(dev, item.second, nullptr);
  for (auto& item : render_passes) vkDestroyRenderPass(dev, item.second, nullptr);
  for (auto& item : samplers) vkDestroySampler(dev, item.second, nullptr);
  if (dummy_sampler) vkDestroySampler(dev, dummy_sampler, nullptr);
  for (Program& program : programs) {
    vkDestroyShaderModule(dev, program.vertex, nullptr);
    if (program.fragment) vkDestroyShaderModule(dev, program.fragment, nullptr);
  }
  for (uint32_t id = 1; id < images.high_water(); ++id) {
    ImageEntry& entry = images[id];
    if (!entry.live) continue;
    for (auto& view : entry.target_views) vkDestroyImageView(dev, view.second, nullptr);
    for (auto& view : entry.reinterpreted_views) vkDestroyImageView(dev, view.second, nullptr);
    if (entry.sampled_view) vkDestroyImageView(dev, entry.sampled_view, nullptr);
    vkDestroyImage(dev, entry.image, nullptr);
  }
  for (uint32_t id = 1; id < buffers.high_water(); ++id) {
    BufferEntry& entry = buffers[id];
    if (entry.live) vkDestroyBuffer(dev, entry.buffer, nullptr);
  }
  staging_pool.Drain([&](const PooledStaging& item) { vkDestroyBuffer(dev, item.buffer, nullptr); });   // S102 RT1
  for (const ImageUpload& upload : image_uploads) vkDestroyBuffer(dev, upload.staging, nullptr);
  for (const BufferUpload& upload : buffer_uploads) vkDestroyBuffer(dev, upload.staging, nullptr);
  for (const UploadBatch& batch : sealed_batches) {  // S102 R21-B: a batch sealed for a frame that was never rendered
    for (const ImageUpload& upload : batch.images) vkDestroyBuffer(dev, upload.staging, nullptr);
    for (const BufferUpload& upload : batch.buffers) vkDestroyBuffer(dev, upload.staging, nullptr);
  }
  if (lc_pipeline) vkDestroyPipeline(dev, lc_pipeline, nullptr);   // S102 L1b
  if (lc_state == 1) Log("[rdr][light_cubes] the direction table was made %llu time(s) in this run", static_cast<unsigned long long>(lc_table_builds));   // S102 L1b review F7
  if (lc_dirs_pipeline) vkDestroyPipeline(dev, lc_dirs_pipeline, nullptr);
  if (lc_dirs_pipeline_swap) vkDestroyPipeline(dev, lc_dirs_pipeline_swap, nullptr);
  if (lc_module) vkDestroyShaderModule(dev, lc_module, nullptr);
  if (lc_dirs_module) vkDestroyShaderModule(dev, lc_dirs_module, nullptr);
  if (lc_pool) vkDestroyDescriptorPool(dev, lc_pool, nullptr);
  if (lc_layout) vkDestroyPipelineLayout(dev, lc_layout, nullptr);
  if (lc_set_layout) vkDestroyDescriptorSetLayout(dev, lc_set_layout, nullptr);
  vkDestroyBuffer(dev, ring, nullptr);
  vkDestroyPipelineCache(dev, pipeline_cache, nullptr);
  vkDestroyDescriptorPool(dev, constants_pool, nullptr);
  vkDestroyPipelineLayout(dev, pipeline_layout, nullptr);
  vkDestroyDescriptorSetLayout(dev, textures_layout, nullptr);
  vkDestroyDescriptorSetLayout(dev, constants_layout, nullptr);
  allocator.Destroy();
  device.Destroy();
}

Renderer::Renderer() : impl_(new Impl) {}

Renderer::~Renderer() { impl_->Shutdown(); }

std::unique_ptr<Renderer> Renderer::Create(const RendererOptions& options, std::string& error) {
  std::unique_ptr<Renderer> renderer(new Renderer);
  if (!renderer->impl_->Init(options, error)) return nullptr;
  // Unused texture slots point at this.
  ImageDesc dummy;
  dummy.width = dummy.height = 1;
  dummy.format = Format::RGBA8;
  renderer->impl_->dummy_image = renderer->CreateImage(dummy);
  const uint32_t black = 0;
  renderer->UpdateImage(renderer->impl_->dummy_image, 0, 0, &black, 4);
  // Empty slots must match what the shader samples them as (a cube sampler needs a cube view).
  ImageDesc dummy_cube = dummy;
  dummy_cube.kind = ImageKind::Cube;
  renderer->impl_->dummy_cube = renderer->CreateImage(dummy_cube);
  for (uint32_t face = 0; face < 6; ++face) renderer->UpdateImage(renderer->impl_->dummy_cube, 0, face, &black, 4);
  ImageDesc dummy_volume = dummy;
  dummy_volume.kind = ImageKind::Volume;
  renderer->impl_->dummy_volume = renderer->CreateImage(dummy_volume);
  renderer->UpdateImage(renderer->impl_->dummy_volume, 0, 0, &black, 4);
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  vkCreateSampler(renderer->impl_->device.device(), &sampler, nullptr, &renderer->impl_->dummy_sampler);
  sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(renderer->impl_->device.device(), &sampler, nullptr, &renderer->impl_->present_sampler);
  sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;  // lane 200: for a present source that cannot filter
  vkCreateSampler(renderer->impl_->device.device(), &sampler, nullptr, &renderer->impl_->present_sampler_nearest);

  // The display curve starts as the identity.
  ImageDesc curve;
  curve.width = 256;
  curve.height = 1;
  curve.format = Format::RGBA8;
  renderer->impl_->display_curve = renderer->CreateImage(curve);
  uint8_t identity[256 * 4];
  for (int i = 0; i < 256; ++i) identity[i * 4] = identity[i * 4 + 1] = identity[i * 4 + 2] = identity[i * 4 + 3] = uint8_t(i);
  renderer->SetDisplayCurve(identity);

  ProgramDesc present;
  present.name = "rdr_present";
  present.vertex_spirv = kSpv_rdr_present_vert;
  present.vertex_bytes = sizeof(kSpv_rdr_present_vert);
  present.fragment_spirv = kSpv_rdr_present_frag;
  present.fragment_bytes = sizeof(kSpv_rdr_present_frag);
  renderer->impl_->present_program = renderer->AddProgram(present);
  present.name = "rdr_present_sgsr";  // settings parity S10a: the same vertex stage, SGSR 1 then the display curve
  present.fragment_spirv = kSpv_rdr_present_sgsr_frag;
  present.fragment_bytes = sizeof(kSpv_rdr_present_sgsr_frag);
  renderer->impl_->present_sgsr_program = renderer->AddProgram(present);
  present.name = "rdr_present_fetch";  // S102 R24-A: the same vertex stage, one texelFetch then the display curve (exactly 1:1 only)
  present.fragment_spirv = kSpv_rdr_present_fetch_frag;
  present.fragment_bytes = sizeof(kSpv_rdr_present_fetch_frag);
  renderer->impl_->present_fetch_program = renderer->AddProgram(present);
  ProgramDesc overlay;  // settings parity S11
  overlay.name = "rdr_overlay";
  overlay.vertex_spirv = kSpv_rdr_overlay_vert;
  overlay.vertex_bytes = sizeof(kSpv_rdr_overlay_vert);
  overlay.fragment_spirv = kSpv_rdr_overlay_frag;
  overlay.fragment_bytes = sizeof(kSpv_rdr_overlay_frag);
  renderer->impl_->overlay_program = renderer->AddProgram(overlay);
  renderer->impl_->self = renderer.get();  // S10b
  if (options.light_cube_compute && renderer->impl_->device.caps().compute) renderer->impl_->LightCubesInit();   // S102 L1b review F2: at startup, not at the first dispatch
  ProgramDesc dlss_motion;  // S10b: DLSS's motion vectors
  dlss_motion.name = "rdr_dlss_motion";
  dlss_motion.vertex_spirv = kSpv_rdr_fullscreen_vert;
  dlss_motion.vertex_bytes = sizeof(kSpv_rdr_fullscreen_vert);
  dlss_motion.fragment_spirv = kSpv_rdr_dlss_motion_frag;
  dlss_motion.fragment_bytes = sizeof(kSpv_rdr_dlss_motion_frag);
  renderer->impl_->dlss_motion_program = renderer->AddProgram(dlss_motion);
  ProgramDesc half_depth;  // S102 C4: half-size transparents: the half-size depth (nearest of four) and the depth-aware add-back
  half_depth.name = "rdr_half_depth";
  half_depth.vertex_spirv = kSpv_rdr_fullscreen_vert, half_depth.vertex_bytes = sizeof(kSpv_rdr_fullscreen_vert);
  half_depth.fragment_spirv = kSpv_rdr_half_depth_frag, half_depth.fragment_bytes = sizeof(kSpv_rdr_half_depth_frag);
  renderer->impl_->half_depth_program = renderer->AddProgram(half_depth);
  ProgramDesc half_composite = half_depth;
  half_composite.name = "rdr_half_composite";
  half_composite.fragment_spirv = kSpv_rdr_half_composite_frag, half_composite.fragment_bytes = sizeof(kSpv_rdr_half_composite_frag);
  renderer->impl_->half_composite_program = renderer->AddProgram(half_composite);
  present.name = "rdr_present_dlss";  // S10b: DLSS's picture + the HUD, then the display curve
  present.fragment_spirv = kSpv_rdr_present_dlss_frag;
  present.fragment_bytes = sizeof(kSpv_rdr_present_dlss_frag);
  renderer->impl_->present_dlss_program = renderer->AddProgram(present);
  VertexLayoutDesc overlay_vertices{};
  overlay_vertices.attributes[0] = {0, 0, uint16_t(VK_FORMAT_R32G32_SFLOAT), 0};
  overlay_vertices.attributes[1] = {1, 0, uint16_t(VK_FORMAT_R32G32_SFLOAT), 8};
  overlay_vertices.attributes[2] = {2, 0, uint16_t(VK_FORMAT_R8G8B8A8_UNORM), 16};
  overlay_vertices.attribute_count = 3;
  overlay_vertices.strides[0] = sizeof(OverlayVertex);
  renderer->impl_->overlay_layout = renderer->AddVertexLayout(overlay_vertices);
  const uint32_t white = 0xFFFFFFFFu;
  renderer->SetOverlayTexture(0, 1, 1, reinterpret_cast<const uint8_t*>(&white));
  renderer->impl_->present_layout = renderer->AddVertexLayout(VertexLayoutDesc{});
  return renderer;
}

void Renderer::SetDisplayCurve(const uint8_t* rgba256) { UpdateImage(impl_->display_curve, 0, 0, rgba256, 256 * 4); }

void Renderer::SetOverlayTexture(uint32_t id, uint32_t width, uint32_t height, const uint8_t* rgba) { SetOverlayImage(id, MakeOverlayTexture(width, height, rgba)); }
ImageId Renderer::MakeOverlayTexture(uint32_t width, uint32_t height, const uint8_t* rgba) {  // S102 R21-B (review F1): any thread; the upload joins the pending batch
  ImageDesc desc;
  desc.width = uint16_t(width);
  desc.height = uint16_t(height);
  desc.format = Format::RGBA8;
  const ImageId image = CreateImage(desc);
  if (image != kNoImage) UpdateImage(image, 0, 0, rgba, size_t(width) * height * 4);
  return image;
}
void Renderer::SetOverlayImage(uint32_t id, ImageId image) {  // S102 R21-B (review F1): the thread that renders (the map is RecordOverlay's)
  Impl& im = *impl_;
  if (const auto it = im.overlay_textures.find(id); it != im.overlay_textures.end()) DestroyImage(it->second);
  im.overlay_textures[id] = image;
}

void Renderer::RemoveOverlayTexture(uint32_t id) {   // S103 AIM: id 0 (the plain white) is never freed
  Impl& im = *impl_;
  if (!id) return;
  if (const auto it = im.overlay_textures.find(id); it != im.overlay_textures.end()) DestroyImage(it->second), im.overlay_textures.erase(it);
}

void Renderer::SetOverlay(std::shared_ptr<const OverlayList> list) { impl_->overlay = std::move(list); }
ImageId Renderer::dlss_output() const { return impl_->dlss_count ? impl_->dlss_out : kNoImage; }
bool Renderer::dlss_in_last_present() const { return impl_->dlss_count && impl_->dlss_had_history; }
bool Renderer::ColourTargetable(Format format) const { return impl_->device.FormatHas(ToVk(format, impl_->device.caps()), VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT); }   // S102 C1
bool Renderer::compute_supported() const { return impl_->device.caps().compute && impl_->lc_state == 1 && !g_force_no_compute.load(std::memory_order_relaxed); }   // review F2: true only once the pipelines exist   // S102 L1b
Renderer::RawVk Renderer::RawVulkan() const {   // S103 PS1 (tools only): see renderer.h
  const Device& d = impl_->device;
  return RawVk{d.instance(), d.physical(), d.device(), d.queue(), d.queue_family()};
}
bool Renderer::StorageTargetable(Format format) const {   // S102 L1b: the format a compute shader can write AND the readers sample: only 8-bit RGBA (storage support is core for it; BGRA is optional and declined)
  return format == Format::RGBA8 && impl_->device.FormatHas(ToVk(format, impl_->device.caps()), VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
}
bool Renderer::multiview_supported() const { return impl_->device.caps().multiview && !g_force_no_multiview.load(std::memory_order_relaxed); }   // S102 C1

uint64_t Renderer::validation_errors() const { return impl_->device.validation_errors(); }
const char* Renderer::device_name() const { return impl_->device.properties().deviceName; }

// ---- resources --------------------------------------------------------------------------------------------------------------------

ImageId Renderer::CreateImage(const ImageDesc& desc) {
  Impl& im = *impl_;
  const uint32_t id = im.images.Add();
  if (!id) {
    Problem("image_table_full", "more than %u images", kChunkSize * kMaxChunks);
    return kNoImage;
  }
  ImageEntry& entry = im.images[id];
  entry.desc = desc;
  std::atomic_ref<uint8_t>(entry.gpu_class).store(0, std::memory_order_relaxed);  // S102 S0-3 (a recycled id keeps no class); R21-B: atomic like its readers
  // S102 U1: a block-compressed texture the device cannot sample (Mali, Xclipse, Adreno 6xx: no BC) is made in a plain format and decoded on upload
  // (bc_decode.h). Before this every such texture was refused below and the pictures made of them (the title card, the menus) were missing.
  entry.decode_from = Format::Undefined;
  if (bc::IsBc(desc.format) && desc.kind != ImageKind::Volume && im.bc_mode != 0) {
    const VkFormat native = ToVk(desc.format, im.device.caps());
    if (im.bc_mode == 2 || !im.device.FormatHas(native, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
      entry.decode_from = desc.format;
      entry.desc.format = bc::Substitute(desc.format);
      im.bc_images.fetch_add(1, std::memory_order_relaxed);
    }
  }
  entry.format = ToVk(entry.desc.format, im.device.caps());
  entry.depth = entry.desc.format == Format::Depth;
  entry.aspect = entry.depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
  entry.width = im.ScaledExtent(desc.width, desc);
  entry.height = im.ScaledExtent(desc.height, desc);
  entry.layers = desc.kind == ImageKind::Cube ? 6u : desc.layers;

  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.flags = desc.kind == ImageKind::Cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  if (desc.usage & kUsageMutable) info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
  info.imageType = desc.kind == ImageKind::Volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
  info.format = entry.format;
  info.extent = {entry.width, entry.height, desc.kind == ImageKind::Volume ? uint32_t(desc.depth) : 1u};
  info.mipLevels = desc.mips;
  info.arrayLayers = entry.layers;
  info.samples = VkSampleCountFlagBits(desc.samples);
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  const bool target = desc.usage & kUsageTarget;
  const bool transient = desc.usage & kUsageTransient;
  // Refuse by name what the device cannot do, instead of asking the driver for something invalid.
  VkFormatFeatureFlags needs_features = (transient && im.device.caps().lazy_memory) ? 0 : VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  if (target) needs_features |= entry.depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  if (desc.usage & kUsageStorage) needs_features |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
  if (entry.format == VK_FORMAT_UNDEFINED || !im.device.FormatHas(entry.format, needs_features)) {
    Problem("image_format_unsupported", "format %d, usage %d", int(desc.format), int(desc.usage));
    im.images.Remove(id);
    return kNoImage;
  }
  if (target) info.usage |= entry.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (desc.usage & kUsageStorage) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;  // S10b (DLSS writes its output from a compute pass)
  if (transient && im.device.caps().lazy_memory) {
    info.usage |= VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  } else {
    info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }
  VkDevice dev = im.device.device();
  if (vkCreateImage(dev, &info, nullptr, &entry.image) != VK_SUCCESS) {
    Problem("image_create_failed", "%ux%u format %d", entry.width, entry.height, int(desc.format));
    im.images.Remove(id);
    return kNoImage;
  }
  VkMemoryRequirements needs{};
  vkGetImageMemoryRequirements(dev, entry.image, &needs);
  const bool lazy = transient && im.device.caps().lazy_memory;
  if (!(lazy && im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, 0, false, entry.memory)) && !im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, entry.memory)) {
    Problem("image_memory_failed", "%llu bytes", static_cast<unsigned long long>(needs.size));
    vkDestroyImage(dev, entry.image, nullptr);
    entry.image = VK_NULL_HANDLE;
    im.images.Remove(id);
    return kNoImage;
  }
  vkBindImageMemory(dev, entry.image, entry.memory.memory, entry.memory.offset);
  entry.vk_usage = info.usage;
  if (info.usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = entry.image;
    view.viewType = desc.kind == ImageKind::Cube ? VK_IMAGE_VIEW_TYPE_CUBE : desc.kind == ImageKind::Volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    view.format = entry.format;
    view.subresourceRange = {VkImageAspectFlags(entry.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, desc.mips, 0, entry.layers};
    vkCreateImageView(dev, &view, nullptr, &entry.sampled_view);
  }
  entry.states.assign(size_t(desc.mips) * entry.layers, SubresourceState{});
  entry.copy_dest = false;
  entry.carries_state = false;
  entry.ever_read = false;
  entry.live = true;
  {
    std::lock_guard<std::mutex> lock(im.upload_mutex);
    im.new_images.push_back(ImageId(id));
  }
  return ImageId(id);
}

void Renderer::UpdateImage(ImageId image, uint32_t mip, uint32_t layer, const void* data, size_t bytes) {
  Impl& im = *impl_;
  // S102 U1: a decoded BC image takes its level as the game's blocks and stages the plain texels, decoded straight into the staging memory (bc_decode.h).
  Format decode_from = Format::Undefined;
  uint32_t dec_w = 0, dec_h = 0;
  size_t src_bytes = bytes;
  if (image != kNoImage && im.images[image].decode_from != Format::Undefined) {
    const ImageEntry& e = im.images[image];
    decode_from = e.decode_from;
    dec_w = std::max(1u, uint32_t(e.desc.width) >> mip), dec_h = std::max(1u, uint32_t(e.desc.height) >> mip);
    bytes = bc::DecodedBytes(decode_from, dec_w, dec_h);
  }
  im.WaitForStagingRoom();
  VkDevice dev = im.device.device();
  ImageUpload upload{image, mip, layer, VK_NULL_HANDLE, {}, VkDeviceSize(bytes)};  // S102 M1: bytes asked for
  VkMemoryRequirements needs{};
  uint32_t pool_class = 0;   // S102 RT1 (bit 4): a kept staging buffer of this size class, when one is free
  if (rdr::g_render_lean.load(std::memory_order_relaxed) & rdr::kLeanHandoff) {
    if ((pool_class = rdr::lean::StagingClass(bytes)) != 0) {
      Impl::PooledStaging kept;
      if (im.staging_pool.Take(pool_class, kept)) {
        upload.staging = kept.buffer, upload.memory = kept.memory, needs.size = kept.memory.size;
        rdr::LeanAdd(rdr::kLcStageHit);
      }
    }
  }
  if (upload.staging == VK_NULL_HANDLE) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = pool_class ? rdr::lean::StagingClassBytes(pool_class) : bytes;   // S102 RT1: a pooled buffer is made at its class size so any upload of the class fits
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  vkCreateBuffer(dev, &info, nullptr, &upload.staging);
  vkGetBufferMemoryRequirements(dev, upload.staging, &needs);
  if (!im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, true, upload.memory)) {
    Problem("staging_memory_failed", "%zu bytes", bytes);
    vkDestroyBuffer(dev, upload.staging, nullptr);
    return;
  }
  vkBindBufferMemory(dev, upload.staging, upload.memory.memory, upload.memory.offset);
  if (pool_class) rdr::LeanAdd(rdr::kLcStageMiss);
  }
  upload.pool_class = uint8_t(pool_class);
  if (decode_from != Format::Undefined) {
    if (!bc::Decode(decode_from, static_cast<const uint8_t*>(data), src_bytes, dec_w, dec_h, static_cast<uint8_t*>(upload.memory.mapped)))
      Problem("bc_decode_short", "format %d level %u: %zu bytes of %zu", int(decode_from), mip, src_bytes, bc::EncodedBytes(decode_from, dec_w, dec_h));
    if (im.bc_decoded_levels.fetch_add(1, std::memory_order_relaxed) == 0) Log("[rdr] bc: the first block-compressed level decoded on the CPU (%ux%u, format %d)", dec_w, dec_h, int(decode_from));
    im.bc_decoded_bytes.fetch_add(bytes, std::memory_order_relaxed);
  } else {
    std::memcpy(upload.memory.mapped, data, bytes);
  }
  im.staged_bytes.fetch_add(needs.size, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(im.upload_mutex);
  im.image_uploads.push_back(upload);
}

// S100 (diagnosis of the entrance's slow frames, SESSION100_SUMMARY 04:4x): where a frame's time goes in Render, logged every 600 frames.
struct RenderTimes {
  uint64_t frames = 0, wait = 0, retire = 0, uploads = 0, compile = 0, record = 0, submit = 0, present = 0;
  uint64_t upload_bytes = 0, passes = 0, steps = 0;
  uint64_t drain = 0, destroys = 0, pools = 0;               // retire, split (ns)
  uint64_t pipestore = 0;                                    // lane 276: the listed-pipeline slice + the save's blob copy (printed only when non-zero)
  uint64_t retired_items = 0, destroy_items = 0, new_framebuffers = 0, texture_sets = 0, set_ns = 0, fb_ns = 0;
};
