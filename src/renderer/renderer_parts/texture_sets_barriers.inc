
VkSampler Renderer::Impl::GetSampler(const SamplerDesc& desc) {
  auto found = samplers.find(desc);
  if (found != samplers.end()) return found->second;
  VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  info.magFilter = VkFilter(desc.mag_filter);
  info.minFilter = VkFilter(desc.min_filter);
  info.mipmapMode = VkSamplerMipmapMode(desc.mip_mode);
  // Mirror-clamp is not in the floor: mirrored repeat is the nearest thing every device has.
  const auto address = [](uint8_t mode) { return mode == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VkSamplerAddressMode(mode); };
  info.addressModeU = address(desc.address_u);
  info.addressModeV = address(desc.address_v);
  info.addressModeW = address(desc.address_w);
  info.mipLodBias = desc.lod_bias;
  info.anisotropyEnable = desc.max_anisotropy > 1 && device.caps().anisotropy;
  info.maxAnisotropy = std::min(float(desc.max_anisotropy), device.caps().max_anisotropy);
  info.minLod = desc.min_lod;
  info.maxLod = desc.max_lod;
  info.borderColor = VkBorderColor(desc.border);
  VkSampler sampler = VK_NULL_HANDLE;
  vkCreateSampler(device.device(), &info, nullptr, &sampler);
  samplers.emplace(desc, sampler);
  return sampler;
}

void Renderer::Impl::FillTextureInfos(const Frame& frame, uint32_t index, VkDescriptorImageInfo* infos) {   // S102 RT1: the infos loop of GetTextureSet, unchanged
  const TextureSet& textures = frame.texture_sets[index];
  for (uint32_t i = 0; i < kMaxTextures; ++i) {
    const TextureSlot& texture = textures.slots[i];
    ImageEntry* entry = texture.image != kNoImage ? &images[texture.image] : nullptr;
    if (entry && !entry->live) {
      Problem("texture_not_live", "slot %u image %u", i, unsigned(texture.image));
      entry = nullptr;
    }
    if (entry) {
      const uint8_t image_kind = entry->desc.kind == ImageKind::Cube ? 1 : entry->desc.kind == ImageKind::Volume ? 2 : 0;
      if (image_kind != texture.kind) {
        Problem("texture_kind_mismatch", "slot %u image %u is kind %u, the shader samples kind %u", i, unsigned(texture.image), unsigned(image_kind), unsigned(texture.kind));
        entry = nullptr;
      }
    }
    if (!entry) {
      // Empty slots read as transparent black through a view of the kind the shader expects.
      infos[i] = {dummy_sampler, images[DummyFor(texture.kind)].sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      continue;
    }
    SamplerDesc sampler = frame.samplers[texture.sampler];
    const Format read_format = texture.view_format != Format::Undefined ? texture.view_format : entry->desc.format;
    if (!filterable[uint32_t(read_format)]) {
      // Linear filtering of a format that cannot filter is undefined (TEXTURES_FRAME T29, T30). Lane 200 (GitHub #185): decided from the
      // device's format features; the values are untouched, only the smoothing between texels is lost.
      const bool wants_linear = sampler.mag_filter == VK_FILTER_LINEAR || sampler.min_filter == VK_FILTER_LINEAR || sampler.mip_mode == VK_SAMPLER_MIPMAP_MODE_LINEAR;
      if (caps_fallback) {
        cap_stats.linear_to_nearest += wants_linear;
        if (wants_linear) CapsChanged();
        sampler.mag_filter = sampler.min_filter = VK_FILTER_NEAREST;
        sampler.mip_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.max_anisotropy = 1;
      } else {
        cap_stats.illegal_linear += wants_linear;
        if (wants_linear) CapsChanged();
      }
    }
    infos[i].sampler = GetSampler(sampler);
    infos[i].imageView = SampledView(*entry, texture);
    infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
}

VkDescriptorSet Renderer::Impl::GetTextureSetLean(const Frame& frame, uint32_t index, FrameSlot& slot) {   // S102 RT1 (bit 1)
  VkDescriptorSet& set = frame_sets[index];
  VkDescriptorImageInfo infos[kMaxTextures];
  FillTextureInfos(frame, index, infos);
  rdr::lean::SetKey<size_t(kMaxTextures)> key;
  for (uint32_t i = 0; i < kMaxTextures; ++i) key.view[i] = reinterpret_cast<uint64_t>(infos[i].imageView), key.sampler[i] = reinterpret_cast<uint64_t>(infos[i].sampler);
  const uint64_t hash = key.Hash();
  bool collide = false;
  uint64_t prev_use = 0;
  if (auto* hit = lean_cache.Find(key, hash, serial, collide, &prev_use)) {
    set = hit->set;
    rdr::LeanAdd(rdr::kLcSetHit);
    if (prev_use != serial) rdr::LeanAdd(rdr::kLcSetHitOld);   // held from an EARLIER frame (the census the brief asks for); the rest were built earlier in this same frame
    return set;
  }
  ++g_render_times.texture_sets;
  if (options.counts) ++counts_cur.texture_sets;
  struct SetTimer {
    uint64_t t0 = RenderNow();
    ~SetTimer() { g_render_times.set_ns += RenderNow() - t0; }
  } set_timer;
  if (collide) rdr::LeanAdd(rdr::kLcSetCollide);
  if (!lean_pool && !lean_pool_made_fail) {
    if (const int c = rdr::g_lean_capacity.load(std::memory_order_relaxed); c > 0) lean_cache.set_capacity(size_t(c));   // tools only (rplay --lean-capacity)
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, uint32_t(lean_cache.capacity()) * kMaxTextures};
    VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = uint32_t(lean_cache.capacity());
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device.device(), &info, nullptr, &lean_pool) != VK_SUCCESS) lean_pool = VK_NULL_HANDLE, ++lean_pool_made_fail;
  }
  if (lean_pool && !collide) {
    if (lean_cache.size() >= lean_cache.capacity()) {  // full: sets idle for 600 frames go (they are retired into THIS slot: every frame that could name them is older than this slot's fence)
      lean_freed.clear();
      lean_cache.Sweep(serial, rdr::g_lean_sweep_age.load(std::memory_order_relaxed) ? uint64_t(rdr::g_lean_sweep_age.load(std::memory_order_relaxed)) : 600, lean_freed);
      for (VkDescriptorSet gone : lean_freed) {
        Retired retired;
        retired.lean_set = gone;
        slot.retired.push_back(retired);
      }
      rdr::LeanAdd(rdr::kLcSetEvict, lean_freed.size());
    }
    if (lean_cache.size() < lean_cache.capacity()) {
      VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      alloc.descriptorPool = lean_pool;
      alloc.descriptorSetCount = 1;
      alloc.pSetLayouts = &textures_layout;
      if (vkAllocateDescriptorSets(device.device(), &alloc, &set) == VK_SUCCESS) {
        WriteTextureSet(set, infos);
        lean_cache.Insert(key, hash, set, serial);
        rdr::LeanAdd(rdr::kLcSetMiss);
        return set;
      }
      set = VK_NULL_HANDLE;
    }
  }
  rdr::LeanAdd(rdr::kLcSetFull);  // no room (or a colliding key): this frame's own pool, as before
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = slot.descriptors;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &textures_layout;
  if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) {
    Problem("descriptor_pool_empty", "more than %u texture sets in one frame", kMaxSetsPerFrame);
    return VK_NULL_HANDLE;
  }
  WriteTextureSet(set, infos);
  return set;
}

void Renderer::Impl::LeanCensus() {   // S102 RT1: every 600 frames while a bit is on
  using namespace rdr;
  Log("[rdr][render_lean] frames %llu, mask %d: sets kept %llu / built %llu / fallback %llu / collide %llu / evicted %llu (cache %zu of %zu); hooks: shadow skip %llu run %llu, wrestler skip %llu run %llu; hand-off: staging pool hit %llu miss %llu, ui record skip %llu run %llu, plane sort skip %llu run %llu, note flat %llu",
      static_cast<unsigned long long>(serial), g_render_lean.load(std::memory_order_relaxed) | g_lean_hooks.load(std::memory_order_relaxed), static_cast<unsigned long long>(LeanGet(kLcSetHit)), static_cast<unsigned long long>(LeanGet(kLcSetMiss)),
      static_cast<unsigned long long>(LeanGet(kLcSetFull)), static_cast<unsigned long long>(LeanGet(kLcSetCollide)), static_cast<unsigned long long>(LeanGet(kLcSetEvict)), lean_cache.size(), lean_cache.capacity(),
      static_cast<unsigned long long>(LeanGet(kLcShadowSkip)), static_cast<unsigned long long>(LeanGet(kLcShadowRun)), static_cast<unsigned long long>(LeanGet(kLcWrestlerSkip)), static_cast<unsigned long long>(LeanGet(kLcWrestlerRun)),
      static_cast<unsigned long long>(LeanGet(kLcStageHit)), static_cast<unsigned long long>(LeanGet(kLcStageMiss)), static_cast<unsigned long long>(LeanGet(kLcSetRecordSkip)), static_cast<unsigned long long>(LeanGet(kLcSetRecordRun)),
      static_cast<unsigned long long>(LeanGet(kLcSortSkip)), static_cast<unsigned long long>(LeanGet(kLcSortRun)), static_cast<unsigned long long>(LeanGet(kLcNoteFlat)));
}

VkDescriptorSet Renderer::Impl::GetTextureSet(const Frame& frame, uint32_t index, FrameSlot& slot) {
  VkDescriptorSet& set = frame_sets[index];
  if (set) return set;
  if (rdr::g_render_lean.load(std::memory_order_relaxed) & rdr::kLeanSets) return GetTextureSetLean(frame, index, slot);   // S102 RT1
  ++g_render_times.texture_sets;
  if (options.counts) ++counts_cur.texture_sets;  // S102 M1
  struct SetTimer {
    uint64_t t0 = RenderNow();
    ~SetTimer() { g_render_times.set_ns += RenderNow() - t0; }
  } set_timer;
  VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  alloc.descriptorPool = slot.descriptors;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &textures_layout;
  if (vkAllocateDescriptorSets(device.device(), &alloc, &set) != VK_SUCCESS) {
    Problem("descriptor_pool_empty", "more than %u texture sets in one frame", kMaxSetsPerFrame);
    return VK_NULL_HANDLE;
  }
  VkDescriptorImageInfo infos[kMaxTextures];
  FillTextureInfos(frame, index, infos);
  WriteTextureSet(set, infos);
  return set;
}

void Renderer::Impl::WriteTextureSet(VkDescriptorSet set, const VkDescriptorImageInfo* infos) {
  VkWriteDescriptorSet writes[kMaxTextures];
  for (uint32_t b = 0; b < uint32_t(kMaxTextures); ++b) {
    writes[b] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[b].dstSet = set;
    writes[b].dstBinding = b;
    writes[b].descriptorCount = 1;
    writes[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[b].pImageInfo = infos + b;
  }
  vkUpdateDescriptorSets(device.device(), kMaxTextures, writes, 0, nullptr);
}

// ---- barriers ---------------------------------------------------------------------------------------------------------------------

const char* Renderer::Impl::LayeredTargetsProblem(const TargetsOp& t) {
  bool any = false;
  for (uint32_t c = 0; c < kMaxColorTargets; ++c) {
    const uint16_t pad = t.color[c].pad;
    if (pad == 0) continue;
    if (c >= t.color_count) return "a layered count on an unused colour slot";
    if (pad == 1 || pad > kMaxViews) return "a view count that is not 0 or 2..6";
    if (c != 0) return "only colour 0 may be layered";
    any = true;
  }
  if (t.depth.pad) return "a layered depth target";
  if (!any) return nullptr;
  if (t.color_count != 1) return "a layered colour beside another colour";
  if (t.depth.image != kNoImage) return "a layered colour beside a depth target";
  const ImageRef& ref = t.color[0];
  if (ref.image == kNoImage || ref.image >= images.high_water() || !images[ref.image].live) return "a layered target on a missing image";
  const ImageEntry& entry = images[ref.image];
  if (uint32_t(ref.layer) + ViewCount(ref) > entry.layers) return "layers past the end of the image";
  if (ref.mip >= entry.desc.mips || entry.desc.samples != 1) return "a layered target of a mip that is not there, or a multisampled one";
  return nullptr;
}

void Renderer::Impl::Transition(ImageEntry& entry, uint32_t first_sub, uint32_t sub_count, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access, bool discard, bool ranged) {
  constexpr VkAccessFlags kWrites = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  if (ranged && sub_count > 1 && (first_sub / entry.layers) == ((first_sub + sub_count - 1) / entry.layers)) {   // S102 C1 review F9: one range barrier for the layers of a layered target when they agree
    bool same = true;
    const SubresourceState first = entry.states[first_sub];
    for (uint32_t sub = first_sub + 1; sub < first_sub + sub_count; ++sub) {
      const SubresourceState& s = entry.states[sub];
      same &= s.layout == first.layout && s.access == first.access && s.stage == first.stage;
    }
    if (same) {
      const bool layout_change = first.layout != layout;
      const bool hazard = (first.access & kWrites) || (access & kWrites);
      if (!layout_change && !hazard) {
        for (uint32_t sub = first_sub; sub < first_sub + sub_count; ++sub) entry.states[sub].stage |= stage;
        return;
      }
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask = first.access;
      barrier.dstAccessMask = access;
      barrier.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : first.layout;
      barrier.newLayout = layout;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = entry.image;
      barrier.subresourceRange = {entry.aspect, first_sub / entry.layers, 1, first_sub % entry.layers, sub_count};
      barriers.push_back(barrier);
      barrier_src |= first.stage;
      barrier_dst |= stage;
      for (uint32_t sub = first_sub; sub < first_sub + sub_count; ++sub) {
        SubresourceState& s = entry.states[sub];
        s.layout = layout, s.stage = stage, s.access = access;
      }
      return;
    }
  }
  for (uint32_t sub = first_sub; sub < first_sub + sub_count; ++sub) {
    SubresourceState& state = entry.states[sub];
    const bool layout_change = state.layout != layout;
    const bool hazard = (state.access & kWrites) || (access & kWrites);
    if (!layout_change && !hazard) {
      state.stage |= stage;  // another reader; a later writer must wait for this stage too
      continue;
    }
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = state.access;
    barrier.dstAccessMask = access;
    barrier.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : state.layout;
    barrier.newLayout = layout;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = entry.image;
    barrier.subresourceRange = {entry.aspect, sub / entry.layers, 1, sub % entry.layers, 1};
    barriers.push_back(barrier);
    barrier_src |= state.stage;
    barrier_dst |= stage;
    state.layout = layout;
    state.stage = stage;
    state.access = access;
  }
}

void Renderer::Impl::FlushBarriers(VkCommandBuffer cmd) {
  if (barriers.empty()) return;
  ++out_flushes;  // S102 G5
  vkCmdPipelineBarrier(cmd, barrier_src, barrier_dst, 0, 0, nullptr, 0, nullptr, uint32_t(barriers.size()), barriers.data());
  if (options.counts) ++counts_cur.total.barriers, counts_cur.total.barrier_images += uint32_t(barriers.size());  // S102 M1
  barriers.clear();
  barrier_src = barrier_dst = 0;
}
