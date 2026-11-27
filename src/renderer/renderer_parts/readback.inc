
static float HalfToFloat(uint16_t half) {
  const int exponent = (half >> 10) & 31, mantissa = half & 1023;
  const float sign = (half >> 15) ? -1.0f : 1.0f;
  if (exponent == 31) return 0.0f;  // infinity or NaN: nothing to show
  if (exponent == 0) return sign * float(mantissa) / (1024.0f * 16384.0f);
  return sign * std::ldexp(1.0f + float(mantissa) / 1024.0f, exponent - 15);
}

// The staging bytes of a read-back as tightly packed RGBA8 (BGRA swapped; R8 as grey; 16F scaled by the peak: display only).
static void ConvertReadback(Format format, uint32_t texel, const uint8_t* mapped, uint32_t width, uint32_t height, std::vector<uint8_t>& rgba) {
  const size_t pixels = size_t(width) * height;
  rgba.resize(pixels * 4);
  if (format == Format::RGBA8 || format == Format::BGRA8) {
    std::memcpy(rgba.data(), mapped, pixels * 4);
    if (format == Format::BGRA8) {
      for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
    }
  } else if (format == Format::Depth) {   // S102 G11: raw depth bits, a hash is all they are read for
    std::memcpy(rgba.data(), mapped, pixels * 4);
  } else if (format == Format::R8) {
    for (size_t i = 0; i < pixels; ++i) {
      rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = mapped[i];
      rgba[i * 4 + 3] = 255;
    }
  } else {
    const uint32_t channels = texel / 2;
    const uint16_t* halves = reinterpret_cast<const uint16_t*>(mapped);
    float peak = 1e-6f;
    for (size_t i = 0; i < pixels * channels; ++i) peak = std::max(peak, std::fabs(HalfToFloat(halves[i])));
    for (size_t i = 0; i < pixels; ++i) {
      for (uint32_t c = 0; c < 3; ++c) rgba[i * 4 + c] = c < channels ? uint8_t(std::min(255.0f, std::fabs(HalfToFloat(halves[i * channels + c])) / peak * 255.0f)) : 0;
      rgba[i * 4 + 3] = 255;
    }
  }
}

// Colour surfaces come back as RGBA8 for looking at. A float surface is scaled so its largest value is white.
bool Renderer::ReadPresent(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) { return impl_->swapchain.ReadLast(rgba, width, height); }

bool Renderer::ImageIs8Bit(ImageId image) const {
  if (image == kNoImage) return false;
  const ImageEntry& entry = impl_->images[image];
  return entry.live && entry.desc.samples == 1 && (entry.desc.format == Format::RGBA8 || entry.desc.format == Format::BGRA8);
}

float Renderer::render_scale() const { return impl_->scale; }   // S105 A1

std::vector<Renderer::LiveImageInfo> Renderer::LiveImageInfos() const {   // S105 A1 (test use)
  std::vector<LiveImageInfo> out;
  for (uint32_t id = 1; id < impl_->images.high_water(); ++id) {
    const ImageEntry& entry = impl_->images[id];
    if (!entry.live) continue;
    LiveImageInfo info;
    info.id = id, info.width = entry.width, info.height = entry.height, info.format = uint32_t(entry.desc.format), info.scaled = (entry.desc.usage & kUsageScaled) != 0;
    out.push_back(info);
  }
  return out;
}

bool Renderer::ImageExtent(ImageId image, uint32_t& width, uint32_t& height, bool& scaled) const {   // S105 A1
  width = height = 0, scaled = false;
  if (image == kNoImage) return false;
  const ImageEntry& entry = impl_->images[image];
  if (!entry.live) return false;
  width = entry.width, height = entry.height, scaled = (entry.desc.usage & kUsageScaled) != 0;
  return true;
}

bool Renderer::ReadImage(ImageRef image, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  Impl& im = *impl_;
  ImageEntry& entry = im.images[image.image];
  if (!entry.live || entry.desc.samples != 1) return false;
  uint32_t texel = 0;
  switch (entry.desc.format) {
    case Format::R8: texel = 1; break;
    case Format::RGBA8: case Format::BGRA8: case Format::RG16F: texel = 4; break;
    case Format::RGBA16F: texel = 8; break;
    case Format::Depth: texel = 4; break;   // S102 G11: the depth aspect, 4 bytes a texel (the sweep hashes the frame buffer's depth)
    default: return false;
  }
  VkDevice dev = im.device.device();
  vkDeviceWaitIdle(dev);
  width = std::max(1u, entry.width >> image.mip);
  height = std::max(1u, entry.height >> image.mip);
  const VkDeviceSize bytes = VkDeviceSize(width) * height * texel;
  VkBuffer staging = VK_NULL_HANDLE;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vkCreateBuffer(dev, &info, nullptr, &staging);
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, staging, &needs);
  Allocation memory;
  if (!im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true, memory)) {
    vkDestroyBuffer(dev, staging, nullptr);
    return false;
  }
  vkBindBufferMemory(dev, staging, memory.memory, memory.offset);

  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = im.device.queue_family();
  vkCreateCommandPool(dev, &pool_info, nullptr, &pool);
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  vkAllocateCommandBuffers(dev, &alloc, &cmd);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  const uint32_t sub = image.mip * entry.layers + image.layer;
  const VkImageLayout layout_before = entry.states[sub].layout;   // S102 P1 (review F6): a sampled image goes back to SHADER_READ_ONLY after the read (else the next frame that reads it without rewriting it trips validation)
  im.Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
  im.FlushBarriers(cmd);
  VkBufferImageCopy region{};
  region.imageSubresource = {entry.desc.format == Format::Depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : entry.aspect, image.mip, image.layer, 1};   // S102 G11: depth only
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
  if (layout_before == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    im.Transition(entry, sub, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    im.FlushBarriers(cmd);
  }
  VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0, nullptr);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(im.device.queue(), 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(im.device.queue());

  ConvertReadback(entry.desc.format, texel, static_cast<const uint8_t*>(memory.mapped), width, height, rgba);
  vkDestroyCommandPool(dev, pool, nullptr);
  vkDestroyBuffer(dev, staging, nullptr);
  im.allocator.Free(memory);
  return true;
}

// ---- lane 232 (3d a2): the no-drain read-back ----
static uint32_t ReadbackTexelBytes(Format f) {
  switch (f) {
    case Format::R8: return 1;
    case Format::RGBA8: case Format::BGRA8: case Format::RG16F: return 4;
    case Format::RGBA16F: return 8;
    default: return 0;
  }
}

// Records ONE request's copy into `cmd` (false: cancelled: the image is gone, unreadable or out of range, or no staging memory).
bool Renderer::Impl::RecordOneReadback(VkCommandBuffer cmd, const rdr::ReadbackTracker::Request& req) {
  VkDevice dev = device.device();
  ImageEntry& entry = images[req.key.image];
  const uint32_t texel = ReadbackTexelBytes(entry.desc.format);
  if (!entry.live || entry.desc.samples != 1 || !texel || req.key.mip >= entry.desc.mips || req.key.layer >= entry.layers) return false;
  const uint32_t width = std::max(1u, entry.width >> req.key.mip), height = std::max(1u, entry.height >> req.key.mip);
  const VkDeviceSize bytes = VkDeviceSize(width) * height * texel;
  ReadbackStaging& st = readback_staging[ReadbackKey(req.key)];
  if (st.buffer == VK_NULL_HANDLE || st.bytes != bytes) {  // a new level, or the picture was recreated at another size (never in flight: Queue refuses a busy key)
    if (st.buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(dev, st.buffer, nullptr);
      allocator.Free(st.memory);
      st = ReadbackStaging{};
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    vkCreateBuffer(dev, &info, nullptr, &buffer);
    VkMemoryRequirements needs{};
    vkGetBufferMemoryRequirements(dev, buffer, &needs);
    Allocation memory;
    if (!allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true, memory)) {
      vkDestroyBuffer(dev, buffer, nullptr);
      readback_staging.erase(ReadbackKey(req.key));
      return false;
    }
    vkBindBufferMemory(dev, buffer, memory.memory, memory.offset);
    st.buffer = buffer, st.memory = memory, st.bytes = bytes;
  }
  st.width = width, st.height = height, st.format = entry.desc.format;
  const uint32_t sub = req.key.mip * entry.layers + req.key.layer;
  Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
  FlushBarriers(cmd);
  VkBufferImageCopy region{};
  region.imageSubresource = {entry.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : entry.aspect, req.key.mip, req.key.layer, 1};
  region.imageExtent = {width, height, 1};
  vkCmdCopyImageToBuffer(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.buffer, 1, &region);
  CountCopy();  // S102 M1: the in-frame read-back (the ReadImage-style calls below own their command buffer and are not part of any frame)
  VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0, nullptr);
  CountBarrier(0);
  return true;
}

// SF-2: a request with a source is recorded right after the copy step that writes its level from THAT source (the frame may hold other copies into the same image, from other targets).
void Renderer::Impl::RecordReadbacksAfterCopy(VkCommandBuffer cmd, const CopyOp& c) {
  for (size_t i = 0; i < readback_taking.size(); ++i) {
    const rdr::ReadbackTracker::Request& req = readback_taking[i];
    if (readback_recorded[i] || !req.source || req.key.image != uint32_t(c.dest.image) || req.key.mip != c.dest.mip || req.key.layer != c.dest.layer || req.source != uint32_t(c.source.image)) continue;
    readback_recorded[i] = 1;
    if (!RecordOneReadback(cmd, req)) readback.Cancel(req.key);
  }
}

// At the end of the steps: requests without a source are recorded now (the frame's final picture); a request whose source copy never ran in this frame is cancelled (never another target's picture).
void Renderer::Impl::RecordReadbacks(VkCommandBuffer cmd) {
  for (size_t i = 0; i < readback_taking.size(); ++i) {
    if (readback_recorded[i]) continue;
    readback_recorded[i] = 1;
    const rdr::ReadbackTracker::Request& req = readback_taking[i];
    if (req.source || !RecordOneReadback(cmd, req)) readback.Cancel(req.key);
  }
}

bool Renderer::QueueReadback(ImageRef image, uint64_t tag, ImageId source) {
  Impl& im = *impl_;
  if (image.image == kNoImage) return false;
  const ImageEntry& entry = im.images[image.image];
  if (!entry.live || entry.desc.samples != 1 || !ReadbackTexelBytes(entry.desc.format) || image.mip >= entry.desc.mips || image.layer >= entry.layers) return false;
  return im.readback.Queue({image.image, image.mip, image.layer}, tag, uint32_t(source));
}

void Renderer::PollReadbacks(std::vector<ReadbackResult>& out) {
  Impl& im = *impl_;
  if (!im.readback.InFlight() && !im.readback.HasCancelled()) return;
  VkDevice dev = im.device.device();
  std::vector<rdr::ReadbackTracker::Done> done;
  im.readback.TakeCancelled(done);  // SF-1: reported with cancelled = true so the caller forgets the tag
  const size_t cancelled = done.size();
  im.readback.Poll([&](uint32_t slot) { return vkGetFenceStatus(dev, im.slots[slot].fence) == VK_SUCCESS; }, done);
  for (size_t di = 0; di < done.size(); ++di) {
    const rdr::ReadbackTracker::Done& d = done[di];
    if (di < cancelled) {
      ReadbackResult r;
      r.tag = d.tag, r.cancelled = true;
      out.push_back(std::move(r));
      continue;
    }
    const auto it = im.readback_staging.find(Impl::ReadbackKey(d.key));
    if (it == im.readback_staging.end()) {
      ReadbackResult gone;
      gone.tag = d.tag, gone.cancelled = true;
      out.push_back(std::move(gone));
      continue;
    }
    const Impl::ReadbackStaging& st = it->second;
    ReadbackResult r;
    r.tag = d.tag;
    r.image = ImageRef{ImageId(d.key.image), uint8_t(d.key.mip), uint8_t(d.key.layer), 0};
    r.width = st.width, r.height = st.height;
    ConvertReadback(st.format, ReadbackTexelBytes(st.format), static_cast<const uint8_t*>(st.memory.mapped), st.width, st.height, r.rgba);
    out.push_back(std::move(r));
  }
}

// S101 fview: one texel of a colour image as the GPU holds it (raw bytes of the image's own format), plus a display RGBA8 of it. Equality of two raw reads is
// exact for every readable format (the 16F display copy of ReadImage is scaled by the image's peak, so it cannot be compared). Test and viewer use only.
bool Renderer::ReadTexelRaw(ImageRef image, int x, int y, uint8_t raw[8], uint32_t& raw_bytes, uint8_t rgba[4]) {
  Impl& im = *impl_;
  ImageEntry& entry = im.images[image.image];
  if (!entry.live || entry.desc.samples != 1) return false;
  uint32_t texel = 0;
  switch (entry.desc.format) {
    case Format::R8: texel = 1; break;
    case Format::RGBA8: case Format::BGRA8: case Format::RG16F: texel = 4; break;
    case Format::RGBA16F: texel = 8; break;
    default: return false;
  }
  const uint32_t width = std::max(1u, entry.width >> image.mip), height = std::max(1u, entry.height >> image.mip);
  if (x < 0 || y < 0 || uint32_t(x) >= width || uint32_t(y) >= height) return false;
  VkDevice dev = im.device.device();
  vkDeviceWaitIdle(dev);
  VkBuffer staging = VK_NULL_HANDLE;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = 16;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vkCreateBuffer(dev, &info, nullptr, &staging);
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, staging, &needs);
  Allocation memory;
  if (!im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true, memory)) {
    vkDestroyBuffer(dev, staging, nullptr);
    return false;
  }
  vkBindBufferMemory(dev, staging, memory.memory, memory.offset);
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = im.device.queue_family();
  vkCreateCommandPool(dev, &pool_info, nullptr, &pool);
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  vkAllocateCommandBuffers(dev, &alloc, &cmd);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  const uint32_t sub = image.mip * entry.layers + image.layer;
  im.Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
  im.FlushBarriers(cmd);
  VkBufferImageCopy region{};
  region.imageSubresource = {entry.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : entry.aspect, image.mip, image.layer, 1};   // S102 R24-C review: one region may not name depth AND stencil (Vulkan: separate regions per aspect); a depth texel is the depth aspect
  region.imageOffset = {x, y, 0};
  region.imageExtent = {1, 1, 1};
  vkCmdCopyImageToBuffer(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
  VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0, nullptr);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(im.device.queue(), 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(im.device.queue());
  const uint8_t* mapped = static_cast<const uint8_t*>(memory.mapped);
  std::memset(raw, 0, 8);
  std::memcpy(raw, mapped, texel);
  raw_bytes = texel;
  if (entry.desc.format == Format::RGBA8 || entry.desc.format == Format::BGRA8) {
    std::memcpy(rgba, mapped, 4);
    if (entry.desc.format == Format::BGRA8) std::swap(rgba[0], rgba[2]);
  } else if (entry.desc.format == Format::R8) {
    rgba[0] = rgba[1] = rgba[2] = mapped[0], rgba[3] = 255;
  } else {  // 16F: the magnitude clamped to 0..1, for display only
    const uint32_t channels = texel / 2;
    const uint16_t* halves = reinterpret_cast<const uint16_t*>(mapped);
    for (uint32_t c = 0; c < 3; ++c) rgba[c] = c < channels ? uint8_t(std::min(1.0f, std::fabs(HalfToFloat(halves[c]))) * 255.0f) : 0;
    rgba[3] = 255;
  }
  vkDestroyCommandPool(dev, pool, nullptr);
  vkDestroyBuffer(dev, staging, nullptr);
  im.allocator.Free(memory);
  return true;
}

// S101 lane 20: ReadTexelRaw for many texels at once: one idle wait, one staging buffer, one command buffer, one submit.
bool Renderer::ReadTexelsRaw(std::vector<TexelRead>& reads) {
  Impl& im = *impl_;
  std::vector<uint32_t> texel(reads.size(), 0);
  size_t wanted = 0;
  for (size_t i = 0; i < reads.size(); ++i) {
    TexelRead& r = reads[i];
    r.ok = false;
    ImageEntry& entry = im.images[r.image.image];
    if (!entry.live || entry.desc.samples != 1) continue;
    switch (entry.desc.format) {
      case Format::R8: texel[i] = 1; break;
      case Format::RGBA8: case Format::BGRA8: case Format::RG16F: texel[i] = 4; break;
      case Format::RGBA16F: texel[i] = 8; break;
      default: continue;
    }
    const uint32_t width = std::max(1u, entry.width >> r.image.mip), height = std::max(1u, entry.height >> r.image.mip);
    if (r.x < 0 || r.y < 0 || uint32_t(r.x) >= width || uint32_t(r.y) >= height) {
      texel[i] = 0;
      continue;
    }
    ++wanted;
  }
  if (!wanted) return true;
  VkDevice dev = im.device.device();
  vkDeviceWaitIdle(dev);
  VkBuffer staging = VK_NULL_HANDLE;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = 16 * reads.size();
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vkCreateBuffer(dev, &info, nullptr, &staging);
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, staging, &needs);
  Allocation memory;
  if (!im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_MEMORY_PROPERTY_HOST_CACHED_BIT, true, memory)) {
    vkDestroyBuffer(dev, staging, nullptr);
    return false;
  }
  vkBindBufferMemory(dev, staging, memory.memory, memory.offset);
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool_info.queueFamilyIndex = im.device.queue_family();
  vkCreateCommandPool(dev, &pool_info, nullptr, &pool);
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  vkAllocateCommandBuffers(dev, &alloc, &cmd);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);
  for (size_t i = 0; i < reads.size(); ++i) {
    if (!texel[i]) continue;
    const TexelRead& r = reads[i];
    ImageEntry& entry = im.images[r.image.image];
    const uint32_t sub = r.image.mip * entry.layers + r.image.layer;
    im.Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT, false);
    im.FlushBarriers(cmd);
    VkBufferImageCopy region{};
    region.bufferOffset = 16 * i;
    region.imageSubresource = {entry.depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : entry.aspect, r.image.mip, r.image.layer, 1};
    region.imageOffset = {r.x, r.y, 0};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyImageToBuffer(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &region);
  }
  VkMemoryBarrier to_host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &to_host, 0, nullptr, 0, nullptr);
  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(im.device.queue(), 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(im.device.queue());
  for (size_t i = 0; i < reads.size(); ++i) {
    if (!texel[i]) continue;
    TexelRead& r = reads[i];
    ImageEntry& entry = im.images[r.image.image];
    const uint8_t* mapped = static_cast<const uint8_t*>(memory.mapped) + 16 * i;
    std::memset(r.raw, 0, 8);
    std::memcpy(r.raw, mapped, texel[i]);
    r.raw_bytes = texel[i];
    if (entry.desc.format == Format::RGBA8 || entry.desc.format == Format::BGRA8) {
      std::memcpy(r.rgba, mapped, 4);
      if (entry.desc.format == Format::BGRA8) std::swap(r.rgba[0], r.rgba[2]);
    } else if (entry.desc.format == Format::R8) {
      r.rgba[0] = r.rgba[1] = r.rgba[2] = mapped[0], r.rgba[3] = 255;
    } else {
      const uint32_t channels = texel[i] / 2;
      const uint16_t* halves = reinterpret_cast<const uint16_t*>(mapped);
      for (uint32_t c = 0; c < 3; ++c) r.rgba[c] = c < channels ? uint8_t(std::min(1.0f, std::fabs(HalfToFloat(halves[c]))) * 255.0f) : 0;
      r.rgba[3] = 255;
    }
    r.ok = true;
  }
  vkDestroyCommandPool(dev, pool, nullptr);
  vkDestroyBuffer(dev, staging, nullptr);
  im.allocator.Free(memory);
  return true;
}

// S101 fview: AddProgram for live reload: checks vkCreateShaderModule and the 16-bit program id; false = nothing was added (a shader the driver refused).
bool Renderer::AddProgramChecked(const ProgramDesc& desc, uint16_t& id) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> tables(im.tables_mutex);  // S102 R21-B (hazards 1, 2)
  if (im.store_on && im.StoreFindProgram(desc, id)) return true;  // lane 276: the same words are the same program
  if (im.programs.size() >= 65535) {
    Problem("program_table_full", "65535 programs: restart the viewer");
    return false;
  }
  Program program;
  program.name = desc.name;
  program.prepass_class = colour_cuts::ClassifyFragment(desc.fragment_spirv, desc.fragment_bytes);  // S102 G10
  program.half_ok = desc.fragment_spirv && !half_trans::ReadsFragCoord(desc.fragment_spirv, desc.fragment_bytes);  // S102 C4
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = desc.vertex_bytes;
  info.pCode = desc.vertex_spirv;
  if (vkCreateShaderModule(im.device.device(), &info, nullptr, &program.vertex) != VK_SUCCESS) return false;
  if (desc.fragment_spirv) {
    info.codeSize = desc.fragment_bytes;
    info.pCode = desc.fragment_spirv;
    if (vkCreateShaderModule(im.device.device(), &info, nullptr, &program.fragment) != VK_SUCCESS) {
      vkDestroyShaderModule(im.device.device(), program.vertex, nullptr);
      return false;
    }
  }
  if (im.programs.size() >= kNoTableId || !im.programs.push_back(program)) {  // S102 R21-B (review F5)
    Problem("program_table_full", "%u programs: restart the viewer", unsigned(kNoTableId));
    vkDestroyShaderModule(im.device.device(), program.vertex, nullptr);
    if (program.fragment) vkDestroyShaderModule(im.device.device(), program.fragment, nullptr);
    return false;
  }
  id = uint16_t(im.programs.size() - 1);
  if (im.store_on) im.StoreProgramAdded(id);
  return true;
}

bool Renderer::validation_active() const { return impl_->device.debug_names(); }  // S101 fview

bool VertexFormatIsInteger(uint16_t vk_format) {
  switch (VkFormat(vk_format)) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_A8B8G8R8_UINT_PACK32: case VK_FORMAT_A8B8G8R8_SINT_PACK32:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2B10G10R10_SINT_PACK32:
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
      return true;
    default:
      return false;
  }
}

