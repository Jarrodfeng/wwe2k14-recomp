
bool Renderer::Impl::RecordPresent(VkCommandBuffer cmd, const PresentOp& present, FrameSlot& slot, VkDeviceSize ring_base, uint32_t& image_index) {
  ImageEntry& source = images[present.source.image];
  // Settings parity S10b: DLSS from the picture before the HUD, to the size the picture is shown at (the fit below, or the window with
  // present_fill); the present then adds the HUD back.
  bool dlss = false;
  if (options.dlss_mode >= 0 && device.dlss() && frame_dlss && frame_dlss->valid) {
    const bool dlss_turned = (swapchain.quarter_turns() & 1) != 0;
    const float shown_w0 = float(dlss_turned ? swapchain.extent().height : swapchain.extent().width);
    const float shown_h0 = float(dlss_turned ? swapchain.extent().width : swapchain.extent().height);
    const float src_w0 = float(present.source_rect.width ? present.source_rect.width : source.desc.width);
    const float src_h0 = float(present.source_rect.height ? present.source_rect.height : source.desc.height);
    const float fit0 = std::min(shown_w0 / src_w0, shown_h0 / src_h0);
    const uint32_t out_w = uint32_t(options.present_fill ? shown_w0 : std::floor(src_w0 * fit0 + 0.5f));
    const uint32_t out_h = uint32_t(options.present_fill ? shown_h0 : std::floor(src_h0 * fit0 + 0.5f));
    dlss = RecordDlss(cmd, slot, ring_base, out_w, out_h);
  }
  dlss_had_history = dlss;
  Transition(source, 0, uint32_t(source.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
  if (dlss) {  // DLSS's output and the pre-HUD picture read by the present
    ImageEntry& out = images[dlss_out];
    Transition(out, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    ImageEntry& before = images[frame_dlss->color];
    Transition(before, 0, uint32_t(before.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT, false);
  }
  if (overlay) {  // settings parity S11: the overlay's textures readable before the pass begins
    for (const auto& [id, image] : overlay_textures) {
      ImageEntry& texture = images[image];
      Transition(texture, 0, uint32_t(texture.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_SHADER_READ_BIT, false);
    }
  }
  FlushBarriers(cmd);

  // Our own small constant block, at the top of this frame's ring section.
  const VkDeviceSize constants_offset = ring_base + kRingSection - kConstantRange;
  static constexpr float kTurns[4][4] = {{1, 0, 0, 1}, {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0}};
  float constants[16] = {};
  std::memcpy(constants, kTurns[swapchain.quarter_turns() & 3], sizeof(float) * 4);
  constants[4] = float(present.source_rect.x) / float(source.desc.width);
  constants[5] = float(present.source_rect.y) / float(source.desc.height);
  constants[6] = float(present.source_rect.width) / float(source.desc.width);
  constants[7] = float(present.source_rect.height) / float(source.desc.height);
  // Settings parity S10a: SGSR's source facts (the image's real texels: scaled images are bigger than their desc) and its sharpness.
  constants[8] = 1.0f / float(source.width), constants[9] = 1.0f / float(source.height);
  constants[10] = float(source.width), constants[11] = float(source.height);
  constants[12] = options.sgsr_sharpness;
  if (overlay && overlay->width > 0 && overlay->height > 0) constants[13] = 2.0f / overlay->width, constants[14] = 2.0f / overlay->height;  // S11
  std::memcpy(ring_memory.mapped + constants_offset, constants, sizeof(constants));

  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = slot.descriptors;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &textures_layout;
  if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) {
    Problem("descriptor_pool_empty", "no room for the present set");
    return false;
  }
  VkDescriptorImageInfo infos[kMaxTextures];
  ImageEntry& dummy = images[dummy_image];
  for (uint32_t i = 0; i < kMaxTextures; ++i) infos[i] = {dummy_sampler, dummy.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  infos[0] = {PresentSamplerFor(source), source.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  infos[1] = {dummy_sampler, images[display_curve].sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (dlss) {  // S10b: rdr_present_dlss.frag's slots
    infos[0] = {PresentSamplerFor(images[dlss_out]), images[dlss_out].sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    infos[2] = {PresentSamplerFor(source), source.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    infos[3] = {PresentSamplerFor(images[frame_dlss->color]), images[frame_dlss->color].sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
  WriteTextureSet(set, infos);

  // Last, so a failure above never leaves an acquired image unpresented.
  const uint32_t out_cap_h = out_cap_h_;  // S102 G2 (b): PerFrame, carried by the frame
  swapchain.NoteOutputCap(out_cap_h);
  const uint64_t acquire_t0 = RenderNow();  // S102 S0-3: a CPU wait on the display, not render work
  const bool acquired = swapchain.Acquire(slot.available, image_index);
  acquire_ns_frame += RenderNow() - acquire_t0;
  if (!acquired) return false;
  const bool out_capped = swapchain.ApplyOutputCap(out_cap_h, rdr::g_out_cap_plant.load(std::memory_order_relaxed));  // S102 G2 (b): extent() / framebuffer() / render_pass() are the capped target's from here
  present_capped_ = out_capped;
  rdr::g_out_frames.fetch_add(1, std::memory_order_relaxed);
  if (out_capped) rdr::g_out_cap_frames.fetch_add(1, std::memory_order_relaxed);

  PassClassKey class_key{};
  class_key.formats[0] = uint16_t(swapchain.format());
  class_key.color_count = 1;
  class_key.samples = 1;
  class_key.pad = out_capped ? 2 : 1;  // S102 G2 review F1: the capped pass (transfer-src final layout, 2 dependencies) is a different pass from the window's: its own class
  // its own class: pipelines here are built against the swapchain's render pass, whose external dependency makes it a different one
  const uint32_t present_class = GetPassClass(class_key);
  class_passes[present_class] = swapchain.render_pass();
  DrawState state{};
  // Settings parity S10a: SGSR only where the picture is made bigger on its way to the screen.
  const bool sgsr_turned = (swapchain.quarter_turns() & 1) != 0;
  const float sgsr_out_w = float(sgsr_turned ? swapchain.extent().height : swapchain.extent().width);
  const float sgsr_out_h = float(sgsr_turned ? swapchain.extent().width : swapchain.extent().height);
  const bool sgsr = options.present_sgsr && !out_bilinear_ /* S102 G2 (c): PerFrame, carried by the frame */ && (sgsr_out_w > constants[6] * float(source.width) * 1.01f ||
                                             sgsr_out_h > constants[7] * float(source.height) * 1.01f);
  {  // S102 S0-1: the swapchain's size, once (the boot's [gfx][applied] line cannot know it): the bench's profile line and a Surface log read it here
    static uint32_t said_w = 0, said_h = 0, said_dw = 0, said_dh = 0;
    if (said_w != swapchain.panel_extent().width || said_h != swapchain.panel_extent().height || said_dw != swapchain.extent().width || said_dh != swapchain.extent().height) {
      said_w = swapchain.panel_extent().width, said_h = swapchain.panel_extent().height, said_dw = swapchain.extent().width, said_dh = swapchain.extent().height;
      Log("[gfx][present] swapchain=%ux%u drawn=%ux%u picture=%ux%u sgsr=%d", said_w, said_h, said_dw, said_dh, uint32_t(constants[6] * float(source.width)),  // S102 G2 F6: the panel, and what the pass draws at (smaller while capped)
          uint32_t(constants[7] * float(source.height)), sgsr ? 1 : 0);
    }
  }
  state.program = dlss ? present_dlss_program : sgsr ? present_sgsr_program : present_program;  // S10b: DLSS replaces SGSR
  if (present_fetch_) {  // S102 R24-A (wwe2k14_rdr_present_fetch): exactly 1:1 reads its pixel with one texelFetch (rdr_present_fetch.frag); everything else keeps the program above
    using namespace rdr;
    const auto count = [](PresentFetchSlot s) { g_present_fetch_count[s].fetch_add(1, std::memory_order_relaxed); };
    const uint64_t frames = g_present_fetch_count[kPcFrames].fetch_add(1, std::memory_order_relaxed) + 1;
    if (census_flush::Due(g_cf_fetch) || frames % 600 == 0) {  // the census line, every 600 frames while the switch is on (strings are made here only)
      const auto get = [](PresentFetchSlot s) { return static_cast<unsigned long long>(g_present_fetch_count[s].load(std::memory_order_relaxed)); };
      Log("[rdr][present_fetch] frames %llu, taken %llu, declined %llu (by reason: fit %llu, sgsr/dlss %llu, cap %llu)", static_cast<unsigned long long>(frames), get(kPcTaken), get(kPcDeclFit) + get(kPcDeclSgsrDlss) + get(kPcDeclCap),
          get(kPcDeclFit), get(kPcDeclSgsrDlss), get(kPcDeclCap));
    }
    // The 1:1 test is made HERE, on the swapchain as this acquire left it (extent() is the capped target's while capped, so a cap declines on its own line first). The fragment at (x, y) reads texel (x, y), so:
    // the picture is the panel's size, the picture is shown whole at the origin (the fit fills the panel, no turn), and nothing replaces the program (SGSR / DLSS). The 'fit' arithmetic is RecordPresent's below.
    const int plant = g_present_fetch_plant.load(std::memory_order_relaxed);  // TEST ONLY (0 in the game): 2 = no fit test
    if (dlss || sgsr) {
      count(kPcDeclSgsrDlss);
    } else if (out_capped) {
      count(kPcDeclCap);
    } else {
      const uint32_t panel_w = swapchain.extent().width, panel_h = swapchain.extent().height;
      const float s_w = float(present.source_rect.width ? present.source_rect.width : source.desc.width);
      const float s_h = float(present.source_rect.height ? present.source_rect.height : source.desc.height);
      const float s_fit = std::min(float(panel_w) / s_w, float(panel_h) / s_h);
      const float v_w = options.present_fill ? float(panel_w) : std::floor(s_w * s_fit + 0.5f), v_h = options.present_fill ? float(panel_h) : std::floor(s_h * s_fit + 0.5f);
      const bool whole = present.source_rect.x == 0 && present.source_rect.y == 0 && (present.source_rect.width == 0 || present.source_rect.width == int32_t(source.desc.width)) &&
                         (present.source_rect.height == 0 || present.source_rect.height == int32_t(source.desc.height));
      const bool plain = present.source.mip == 0 && present.source.layer == 0 && source.layers == 1 && source.desc.samples == 1 && source.desc.kind == ImageKind::Image2D;
      const bool one_to_one = whole && plain && swapchain.quarter_turns() == 0 && source.width == panel_w && source.height == panel_h && v_w == float(panel_w) && v_h == float(panel_h);
      if (one_to_one || plant == 2) state.program = present_fetch_program, count(kPcTaken);
      else count(kPcDeclFit);
    }
  }
  if (sgsr) rdr::g_out_sgsr_frames.fetch_add(1, std::memory_order_relaxed);
  if (sgsr) {
    static bool said = false;
    if (!said) said = true, Log("[rdr] present through SGSR: %ux%u texels shown on %.0fx%.0f", uint32_t(constants[6] * float(source.width)),
                                uint32_t(constants[7] * float(source.height)), sgsr_out_w, sgsr_out_h);
  }
  state.vertex_layout = present_layout;
  state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  state.depth_compare = VK_COMPARE_OP_ALWAYS;
  state.blend[0].write_mask = 0xF;
  VkPipeline pipeline = GetPipeline(state, present_class);

  // Session 100 (Jarrod's phone test: "doesn't seem to be respecting a 16:9 aspect ratio, instead stretching the image to fill the
  // phone"): the frame keeps its own aspect (the source rectangle's: 16:9, or what the game's aspect setting renders), centred, black
  // bars from the pass's clear. The swapchain may be turned (quarter_turns: the phone's native portrait surface): the fit is made in
  // the displayed orientation and the viewport turned back with it.
  const float fb_w = float(swapchain.extent().width), fb_h = float(swapchain.extent().height);
  const bool turned = (swapchain.quarter_turns() & 1) != 0;
  const float shown_w = turned ? fb_h : fb_w, shown_h = turned ? fb_w : fb_h;
  const float src_w = float(present.source_rect.width ? present.source_rect.width : source.desc.width);
  const float src_h = float(present.source_rect.height ? present.source_rect.height : source.desc.height);
  const float fit = std::min(shown_w / src_w, shown_h / src_h);
  // Settings parity S5: with the screen-shape setting the reader drew the scene anamorphic (squeezed by the window's factor): stretched back here.
  const float view_shown_w = options.present_fill ? shown_w : std::floor(src_w * fit + 0.5f);
  const float view_shown_h = options.present_fill ? shown_h : std::floor(src_h * fit + 0.5f);
  const float vw = turned ? view_shown_h : view_shown_w, vh = turned ? view_shown_w : view_shown_h;
  const float vx = std::floor((fb_w - vw) * 0.5f), vy = std::floor((fb_h - vh) * 0.5f);
  VkViewport viewport{vx, vy, vw, vh, 0, 1};
  // S102 G1 lever 2 (present_noclear): the pass is begun here, after the viewport is known. The default clears the whole panel; the lever's pass has
  // no clear and the bars around the viewport (none when it covers the panel) are cleared by rectangles (present_clear.h).
  VkClearValue clear{};
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  const bool noclear = options.present_noclear && swapchain.render_pass_noclear() != VK_NULL_HANDLE;  // (the pass is made on this first ask, never for a player without the lever)
  begin.renderPass = noclear ? swapchain.render_pass_noclear() : swapchain.render_pass();
  begin.framebuffer = swapchain.framebuffer(image_index);
  begin.renderArea.extent = swapchain.extent();
  begin.clearValueCount = 1;
  begin.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  if (noclear) {
    PresentBar bars[4];
    // No present pipeline: nothing will be drawn into the viewport, so it is cleared too (a viewport of nothing leaves the whole panel to clear).
    const int bar_count = pipeline ? PresentBars(int32_t(fb_w), int32_t(fb_h), int32_t(vx), int32_t(vy), int32_t(vw), int32_t(vh), bars)
                                   : PresentBars(int32_t(fb_w), int32_t(fb_h), 0, 0, 0, 0, bars);
    VkClearAttachment attachment{VK_IMAGE_ASPECT_COLOR_BIT, 0, clear};
    for (int b = 0; b < bar_count; ++b) {
      VkClearRect rect{{{bars[b].x, bars[b].y}, {bars[b].w, bars[b].h}}, 0, 1};
      vkCmdClearAttachments(cmd, 1, &attachment, 1, &rect);
    }
    static bool said = false;
    if (!said) said = true, Log("[rdr] present without the full-panel clear: %d bar rectangle(s) cleared", bar_count);
  }
  if (pipeline) vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  if (pipeline) { dyn::Applied dl; dyn::Counts dn; DynApply(cmd, dl, state, present_class, dn); DynNote(dn, present_class); }   // S111 DYNSTATE
  VkRect2D scissor{{0, 0}, swapchain.extent()};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdSetDepthBias(cmd, 0, 0, 0);
  const float blend[4] = {0, 0, 0, 0};
  vkCmdSetBlendConstants(cmd, blend);
  vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  const uint32_t offsets[2] = {uint32_t(constants_offset), uint32_t(constants_offset)};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, offsets);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
  if (pipeline) vkCmdDraw(cmd, 3, 1, 0, 0);
  if (overlay && !overlay->draws.empty()) RecordOverlay(cmd, slot, ring_base, present_class);  // settings parity S11
  vkCmdEndRenderPass(cmd);  // (S102 G2: the capped picture's blit is recorded by the step loop, after a stamp of its own)
  return true;
}

// Settings parity S10b: 4x4 helpers (row-major m[r * 4 + c], column vectors: y = M x).
static bool Invert4x4(const float* m, float* out) {
  double inv[16], det;
  inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
  inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
  inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
  inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
  inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
  inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
  inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
  inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
  inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
  inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
  inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
  inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
  inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
  inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
  inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
  inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
  det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
  if (std::abs(det) < 1e-20) return false;
  for (int i = 0; i < 16; ++i) out[i] = float(inv[i] / det);
  return true;
}
static void Mul4x4(const float* a, const float* b, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r * 4] * b[c] + a[r * 4 + 1] * b[4 + c] + a[r * 4 + 2] * b[8 + c] + a[r * 4 + 3] * b[12 + c];
}
static void Transpose4x4(const float* m, float* out) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[c * 4 + r] = m[r * 4 + c];
}

// Settings parity S10b: DLSS for this present, outside any render pass: the camera's motion vectors (rdr_dlss_motion.frag, a pass into
// an RG16F image of the render size), then Streamline's evaluation into dlss_out (the shown size). False: not done (the present then
// shows the picture as without DLSS).
bool Renderer::Impl::RecordDlss(VkCommandBuffer cmd, FrameSlot& slot, VkDeviceSize ring_base, uint32_t out_w, uint32_t out_h) {
  const DlssInputs& in = *frame_dlss;
  ImageEntry& color = images[in.color];
  ImageEntry& depth = images[in.depth];
  if (!color.live || !depth.live || !color.image || !depth.image || !depth.depth) return false;
  const uint32_t w = color.width, h = color.height;
  if (depth.width != w || depth.height != h || !w || !h) {
    Problem("dlss_size_mismatch", "colour %ux%u, depth %ux%u", w, h, depth.width, depth.height);
    return false;
  }
  int mode = options.dlss_mode;
  if (out_w <= w || out_h <= h) mode = 0, out_w = w, out_h = h;  // not made bigger: DLAA at the render size
  // Our images (render size: the motion vectors; shown size: the output), made again when a size changes.
  if (dlss_motion == kNoImage || dlss_motion_w != w || dlss_motion_h != h) {
    if (dlss_motion != kNoImage) self->DestroyImage(dlss_motion);
    ImageDesc desc;
    desc.width = uint16_t(w), desc.height = uint16_t(h);
    desc.format = Format::RG16F;
    desc.usage = kUsageSampled | kUsageTarget;
    dlss_motion = self->CreateImage(desc);
    dlss_motion_w = w, dlss_motion_h = h;
  }
  if (dlss_out == kNoImage || dlss_out_w != out_w || dlss_out_h != out_h) {
    if (dlss_out != kNoImage) self->DestroyImage(dlss_out);
    ImageDesc desc;
    desc.width = uint16_t(out_w), desc.height = uint16_t(out_h);
    desc.format = Format::RGBA8;
    desc.usage = kUsageSampled | kUsageStorage;
    dlss_out = self->CreateImage(desc);
    dlss_out_w = out_w, dlss_out_h = out_h;
  }
  if (dlss_motion == kNoImage || dlss_out == kNoImage) return false;
  ImageEntry& motion = images[dlss_motion];
  ImageEntry& out = images[dlss_out];
  // Matrices: clip_to_prev = ViewProj(previous) * inverse(ViewProj(this)) (column vectors), for the shader as rows; Streamline wants row
  // vectors (v' = v * M): the transposes.
  float inv_vp[16], clip_to_prev[16], inv_prev[16], prev_to_clip[16];
  if (!Invert4x4(in.view_proj, inv_vp) || !Invert4x4(in.prev_view_proj, inv_prev)) return false;
  Mul4x4(in.prev_view_proj, inv_vp, clip_to_prev);
  Mul4x4(in.view_proj, inv_prev, prev_to_clip);
  // The motion pass's constants (their own block, before the overlay region).
  const VkDeviceSize constants_offset = ring_base + kRingSection - kConstantRange - kOverlayRange - kDlssConstants;
  float constants[24];
  std::memcpy(constants, clip_to_prev, sizeof(clip_to_prev));
  constants[16] = float(w), constants[17] = float(h), constants[18] = 1.0f / float(w), constants[19] = 1.0f / float(h);
  constants[20] = in.jitter[0], constants[21] = in.jitter[1], constants[22] = constants[23] = 0;
  std::memcpy(ring_memory.mapped + constants_offset, constants, sizeof(constants));
  Transition(motion, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, true);
  Transition(depth, 0, uint32_t(depth.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
             VK_ACCESS_SHADER_READ_BIT, false);
  ImageEntry* object_motion = nullptr;  // S10b: the wrestlers' own motion (the binder's velocity pass), when this frame has it
  if (in.object_motion != kNoImage && images[in.object_motion].live && images[in.object_motion].width == w && images[in.object_motion].height == h) {
    object_motion = &images[in.object_motion];
    Transition(*object_motion, 0, uint32_t(object_motion->states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
               VK_ACCESS_SHADER_READ_BIT, false);
  }
  FlushBarriers(cmd);
  RenderPassKey pass_key{};
  pass_key.formats[0] = uint16_t(motion.format);
  pass_key.load[0] = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  pass_key.store[0] = VK_ATTACHMENT_STORE_OP_STORE;
  pass_key.stencil_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  pass_key.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  pass_key.color_count = 1;
  pass_key.samples = 1;
  PassClassKey class_key{};
  class_key.formats[0] = uint16_t(motion.format);
  class_key.color_count = 1;
  class_key.samples = 1;
  FramebufferKey framebuffer_key{};
  framebuffer_key.pass = GetRenderPass(pass_key);
  framebuffer_key.views[0] = TargetView(motion, 0, 0);
  framebuffer_key.width = w, framebuffer_key.height = h;
  DrawState state{};
  state.program = dlss_motion_program;
  state.vertex_layout = present_layout;
  state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  state.depth_compare = VK_COMPARE_OP_ALWAYS;
  state.blend[0].write_mask = 0xF;
  VkPipeline pipeline = GetPipeline(state, GetPassClass(class_key));
  if (!pipeline) return false;
  VkDescriptorSet set = VK_NULL_HANDLE;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = slot.descriptors;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &textures_layout;
  if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) return false;
  VkDescriptorImageInfo infos[kMaxTextures];
  ImageEntry& dummy = images[dummy_image];
  for (uint32_t i = 0; i < kMaxTextures; ++i) infos[i] = {dummy_sampler, dummy.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  infos[0] = {dummy_sampler, depth.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  if (object_motion) infos[1] = {dummy_sampler, object_motion->sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};  // the wrestlers'
  constants[22] = object_motion ? 1.0f : 0.0f;
  std::memcpy(ring_memory.mapped + constants_offset, constants, sizeof(constants));
  WriteTextureSet(set, infos);
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = framebuffer_key.pass;
  begin.framebuffer = GetFramebuffer(framebuffer_key);
  begin.renderArea.extent = {w, h};
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  { dyn::Applied dl; dyn::Counts dn; DynApply(cmd, dl, state, GetPassClass(class_key), dn); DynNote(dn, GetPassClass(class_key)); }   // S111 DYNSTATE
  VkViewport viewport{0, 0, float(w), float(h), 0, 1};
  VkRect2D scissor{{0, 0}, {w, h}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  const uint32_t offsets[2] = {uint32_t(constants_offset), uint32_t(constants_offset)};
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &constants_set, 2, offsets);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(cmd);
  // DLSS reads the three inputs and writes the output from compute shaders.
  Transition(motion, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
  Transition(color, 0, uint32_t(color.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
             VK_ACCESS_SHADER_READ_BIT, false);
  Transition(depth, 0, uint32_t(depth.states.size()), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
             VK_ACCESS_SHADER_READ_BIT, false);
  Transition(out, 0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, true);
  FlushBarriers(cmd);
  const auto describe = [](ImageEntry& e, VkImageLayout layout, VkImageView view) {
    streamline::Image i;
    i.image = e.image, i.view = view, i.memory = e.memory.memory, i.format = e.format, i.layout = layout, i.usage = e.vk_usage;
    return i;
  };
  streamline::Frame f;
  f.frame_index = uint32_t(dlss_count);
  f.width = w, f.height = h, f.out_width = out_w, f.out_height = out_h;
  f.mode = mode;
  f.color_in = describe(color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, color.sampled_view);
  f.depth = describe(depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depth.sampled_view);
  f.motion = describe(motion, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, motion.sampled_view);
  f.color_out = describe(out, VK_IMAGE_LAYOUT_GENERAL, out.sampled_view);
  Transpose4x4(in.view_proj, f.view_to_clip);
  Transpose4x4(inv_vp, f.clip_to_view);
  Transpose4x4(clip_to_prev, f.clip_to_prev_clip);
  Transpose4x4(prev_to_clip, f.prev_clip_to_clip);
  f.jitter[0] = in.jitter[0], f.jitter[1] = in.jitter[1];
  f.mvec_scale[0] = 1.0f / float(w), f.mvec_scale[1] = 1.0f / float(h);
  f.reset = in.reset || !dlss_had_history;
  std::string reason;
  if (!streamline::Evaluate(cmd, f, &reason)) {
    Problem("dlss_failed", "%s", reason.c_str());
    return false;
  }
  if (dlss_count++ == 0)
    Log("[rdr][dlss] first evaluation: %ux%u -> %ux%u, mode %d (0 DLAA), jitter %.3f %.3f", w, h, out_w, out_h, mode, in.jitter[0], in.jitter[1]);
  return true;
}

// Settings parity S11: the app's overlay over the whole window, in the present pass after the picture. Its vertices and indices go in this
// slot's kOverlayRange; the constant block (written by RecordPresent) carries the turn and 2 / width, 2 / height in sgsr.yz.
void Renderer::Impl::RecordOverlay(VkCommandBuffer cmd, FrameSlot& slot, VkDeviceSize ring_base, uint32_t present_class) {
  const OverlayList& list = *overlay;
  const VkDeviceSize vertex_bytes = list.vertices.size() * sizeof(OverlayVertex);
  const VkDeviceSize index_offset = AlignUp(vertex_bytes, 16);
  const VkDeviceSize index_bytes = list.indices.size() * sizeof(uint16_t);
  if (list.width <= 0 || list.height <= 0 || index_offset + index_bytes > kOverlayRange) {
    Problem("overlay_too_big", "%zu vertices, %zu indices", list.vertices.size(), list.indices.size());
    return;
  }
  const VkDeviceSize base = ring_base + kRingSection - kConstantRange - kOverlayRange;
  std::memcpy(ring_memory.mapped + base, list.vertices.data(), size_t(vertex_bytes));
  std::memcpy(ring_memory.mapped + base + index_offset, list.indices.data(), size_t(index_bytes));
  DrawState state{};
  state.program = overlay_program;
  state.vertex_layout = overlay_layout;
  state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  state.depth_compare = VK_COMPARE_OP_ALWAYS;
  state.blend[0] = {1, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE,
                    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, 0xF};
  VkPipeline pipeline = GetPipeline(state, present_class);
  if (!pipeline) return;
  static bool said = false;
  if (!said) said = true, Log("[rdr] overlay drawn: %zu draws, %zu vertices over %.0fx%.0f", list.draws.size(), list.vertices.size(), list.width, list.height);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  { dyn::Applied dl; dyn::Counts dn; DynApply(cmd, dl, state, present_class, dn); DynNote(dn, present_class); }   // S111 DYNSTATE
  const VkDeviceSize vertex_at = base;
  vkCmdBindVertexBuffers(cmd, 0, 1, &ring, &vertex_at);
  vkCmdBindIndexBuffer(cmd, ring, base + index_offset, VK_INDEX_TYPE_UINT16);
  const float fb_w = float(swapchain.extent().width), fb_h = float(swapchain.extent().height);
  VkViewport viewport{0, 0, fb_w, fb_h, 0, 1};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  // A clip rectangle in the overlay's space to framebuffer pixels: through clip space and the same turn as the vertices.
  static constexpr float kTurns[4][4] = {{1, 0, 0, 1}, {0, -1, 1, 0}, {-1, 0, 0, -1}, {0, 1, -1, 0}};
  const float* turn = kTurns[swapchain.quarter_turns() & 3];
  const auto to_fb = [&](float x, float y, float& out_x, float& out_y) {
    const float cx = x * 2.0f / list.width - 1.0f, cy = y * 2.0f / list.height - 1.0f;
    out_x = (cx * turn[0] + cy * turn[1] + 1.0f) * 0.5f * fb_w;
    out_y = (cx * turn[2] + cy * turn[3] + 1.0f) * 0.5f * fb_h;
  };
  uint32_t bound_texture = ~0u;
  for (const OverlayDraw& draw : list.draws) {
    if (!draw.count) continue;
    VkRect2D scissor{{0, 0}, swapchain.extent()};
    if (draw.clipped) {
      float x0, y0, x1, y1;
      to_fb(draw.clip[0], draw.clip[1], x0, y0);
      to_fb(draw.clip[2], draw.clip[3], x1, y1);
      const int32_t left = int32_t(std::max(0.0f, std::min(x0, x1))), top = int32_t(std::max(0.0f, std::min(y0, y1)));
      const int32_t right = int32_t(std::min(fb_w, std::max(x0, x1))), bottom = int32_t(std::min(fb_h, std::max(y0, y1)));
      if (right <= left || bottom <= top) continue;
      scissor = {{left, top}, {uint32_t(right - left), uint32_t(bottom - top)}};
    }
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    if (draw.texture != bound_texture) {
      const auto it = overlay_textures.find(draw.texture);
      const ImageId image = it != overlay_textures.end() ? it->second : overlay_textures[0];
      VkDescriptorSet set = VK_NULL_HANDLE;
      VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      alloc.descriptorPool = slot.descriptors;
      alloc.descriptorSetCount = 1;
      alloc.pSetLayouts = &textures_layout;
      if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) {
        Problem("descriptor_pool_empty", "no room for an overlay set");
        return;
      }
      ImageEntry& texture = images[image];  // (made readable before the pass: RecordPresent)
      VkDescriptorImageInfo infos[kMaxTextures];
      ImageEntry& dummy = images[dummy_image];
      for (uint32_t i = 0; i < kMaxTextures; ++i) infos[i] = {dummy_sampler, dummy.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      infos[0] = {PresentSamplerFor(texture), texture.sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      WriteTextureSet(set, infos);
      vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 1, 1, &set, 0, nullptr);
      bound_texture = draw.texture;
    }
    vkCmdDrawIndexed(cmd, draw.count, 1, draw.first_index, draw.base_vertex, 0);
  }
}
