

void Renderer::Impl::ApplyUploads(VkCommandBuffer cmd, FrameSlot& slot) {
  std::vector<ImageId> fresh;
  std::vector<ImageUpload> image_work;
  std::vector<BufferUpload> buffer_work;
  std::vector<std::pair<ImageId, uint8_t>> class_work;  // S102 R21-B
  {
    std::lock_guard<std::mutex> lock(upload_mutex);
    // S102 R21-B: the sealed batches first, in order (all of them for seal 0, else those up to this frame's seal), then (seal 0 only) what is pending.
    while (!sealed_batches.empty() && (upload_seal_now == 0 || sealed_batches.front().seal <= upload_seal_now)) {
      UploadBatch& b = sealed_batches.front();
      fresh.insert(fresh.end(), b.fresh.begin(), b.fresh.end());
      image_work.insert(image_work.end(), b.images.begin(), b.images.end());
      buffer_work.insert(buffer_work.end(), b.buffers.begin(), b.buffers.end());
      class_work.insert(class_work.end(), b.classes.begin(), b.classes.end());
      sealed_batches.pop_front();
    }
    if (upload_seal_now == 0) {
      fresh.insert(fresh.end(), new_images.begin(), new_images.end()), new_images.clear();
      image_work.insert(image_work.end(), image_uploads.begin(), image_uploads.end()), image_uploads.clear();
      buffer_work.insert(buffer_work.end(), buffer_uploads.begin(), buffer_uploads.end()), buffer_uploads.clear();
      class_work.insert(class_work.end(), class_changes.begin(), class_changes.end()), class_changes.clear();
    }
  }
  for (const auto& [image, cls] : class_work)  // S102 R21-B (hazard 3): the record thread is the only writer of a live image's class
    if (image != kNoImage && image < images.high_water()) images[image].gpu_class = cls;
  // Nothing is ever undefined: new images start cleared to transparent black (TEXTURES_FRAME C22, C32), unless this frame uploads every one of
  // their subresources. Block-compressed formats cannot be cleared; they always come with their upload (a file texture).
  std::unordered_map<ImageId, uint32_t> uploads_per_image;
  for (const ImageUpload& upload : image_work) ++uploads_per_image[upload.image];
  // S102 G5: the barrier count of this call starts here (the clears' barriers count too). The plant (rdr::g_upload_batch_plant, tests only): 1 = the batched groups' last flush is dropped,
  // 2 = a duplicate upload is injected and the duplicate fallback is off, 3 = the same duplicate with the fallback ON (the control: must stay clean).
  const uint64_t flushes_before = out_flushes;
  out_up_explicit = 0;
  const bool want_batch = rdr::g_upload_batch.load(std::memory_order_relaxed);
  const int batch_plant = rdr::g_upload_batch_plant.load(std::memory_order_relaxed);
  if (batch_plant >= 2) {
    for (const ImageUpload& upload : image_work) {
      if (!images[upload.image].live) continue;
      ImageUpload twin = upload;
      twin.injected = true;
      image_work.push_back(twin);
      break;
    }
  }
  // The images to clear (fresh and not fully uploaded). The default: each one a barrier, a clear and a barrier. With the batch switch: every change to copy-destination in ONE
  // barrier, then the clears (the changes to shader-read go out with the flush below).
  std::vector<ImageId> clear_list;
  for (ImageId id : fresh) {
    ImageEntry& entry = images[id];
    if (!entry.live || (entry.desc.usage & kUsageTransient)) continue;
    const bool compressed = entry.desc.format >= Format::BC1 && entry.desc.format <= Format::EAC_RG11;
    const bool fully_uploaded = uploads_per_image[id] >= uint32_t(entry.states.size());
    if ((entry.desc.usage & kUsageTarget) || (!compressed && !fully_uploaded)) clear_list.push_back(id);
  }
  bool clear_batch = want_batch && clear_list.size() > 1;
  if (clear_batch) {
    std::unordered_set<ImageId> listed;
    for (ImageId id : clear_list) clear_batch = listed.insert(id).second && clear_batch;
  }
  if (clear_batch) {
    for (ImageId id : clear_list) {
      ImageEntry& entry = images[id];
      Transition(entry, 0, uint32_t(entry.states.size()), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
    }
    FlushBarriers(cmd);
  }
  for (ImageId id : clear_list) {
    ImageEntry& entry = images[id];
    const uint32_t all = uint32_t(entry.states.size());
    if (!clear_batch) {
      Transition(entry, 0, all, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
      FlushBarriers(cmd);
    }
    VkImageSubresourceRange range{entry.aspect, 0, entry.desc.mips, 0, entry.layers};
    if (entry.depth) {
      const VkClearDepthStencilValue zero{0.0f, 0};
      vkCmdClearDepthStencilImage(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    } else {
      const VkClearColorValue zero{};
      vkCmdClearColorImage(cmd, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    }
    if (options.counts) ++counts_cur.total.clears;  // S102 M1
    // A sampled-only image is never transitioned at draw time: it waits in the layout shaders read (an upload below moves what it writes).
    if (!(entry.desc.usage & kUsageTarget))
      Transition(entry, 0, all, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    // Compressed, or fully uploaded this frame: left alone (not in the list). Each upload starts its subresource from undefined itself. A compressed image sampled
    // before its contents arrive is then a validation error by layout, which is the right signal (an adapter bug, never a picture to keep).
  }
  FlushBarriers(cmd);
  // S102 G5 (--wwe2k14_rdr_upload_batch): the uploads in three groups: every layout change to copy-destination in ONE barrier, the copies, every change back in ONE barrier (the
  // default is two barriers per upload). Never when one frame uploads the same subresource twice (the second copy must wait for the first), nor for a single upload.
  out_uploads = uint32_t(image_work.size() + buffer_work.size());
  out_up_images = uint32_t(image_work.size());   // S103 UPL
  // S103 UPL (review item 7): the batch is cut at the first upload of a subresource that an earlier upload of the same segment already wrote (the second copy must wait for the first), instead of dropping the batch for
  // the whole frame: each segment is batched on its own (3 groups: changes to copy-destination, copies, changes back). Plant 2 (tests only) keeps one segment through the duplicate.
  std::vector<std::pair<size_t, size_t>> segs;
  {
    size_t begin = 0;
    bool split = false;
    std::unordered_set<uint64_t> seen;
    if (want_batch && image_work.size() > 1) {
      for (size_t i = 0; i < image_work.size(); ++i) {
        const ImageEntry& entry = images[image_work[i].image];
        if (!entry.live) continue;
        const uint64_t key = (uint64_t(image_work[i].image) << 32) | (image_work[i].mip * entry.layers + image_work[i].layer);
        if (seen.insert(key).second || batch_plant == 2) continue;
        segs.push_back({begin, i});
        begin = i, seen.clear(), seen.insert(key);
        if (!split) {
          split = true;
          rdr::g_upload_dup_fallbacks.fetch_add(1, std::memory_order_relaxed);
          static int logged = 0;
          if (logged++ < 8) Log("[rdr][uploads] batch cut at a duplicate subresource: image %u (%ux%u, format %d) mip %u layer %u, upload %zu of %zu in this frame; each segment is batched on its own", unsigned(image_work[i].image),
                                unsigned(entry.desc.width), unsigned(entry.desc.height), int(entry.desc.format), unsigned(image_work[i].mip), unsigned(image_work[i].layer), i, image_work.size());
        }
      }
    }
    segs.push_back({begin, image_work.size()});
  }
  out_batched = false;
  for (const auto& sg : segs) out_batched = out_batched || (want_batch && sg.second - sg.first > 1);
  if (want_batch && image_work.size() > 1) rdr::g_upload_multi_frames.fetch_add(1, std::memory_order_relaxed);
  if (out_batched) rdr::g_upload_batched_frames.fetch_add(1, std::memory_order_relaxed);
  for (const auto& sg : segs) {
  const size_t seg_begin = sg.first, seg_end = sg.second;
  const bool batch = want_batch && seg_end - seg_begin > 1;
  if (batch) {
    for (size_t ui = seg_begin; ui < seg_end; ++ui) {
      const ImageUpload& upload = image_work[ui];
      ImageEntry& entry = images[upload.image];
      if (!entry.live) continue;
      Transition(entry, upload.mip * entry.layers + upload.layer, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
    }
    FlushBarriers(cmd);
  }
  for (size_t ui = seg_begin; ui < seg_end; ++ui) {
    const ImageUpload& upload = image_work[ui];
    ImageEntry& entry = images[upload.image];
    if (!upload.injected) {
      out_up_bytes += upload.bytes;
      Retired retired;
      retired.buffer = upload.staging;
      retired.memory = upload.memory;
      retired.staged_bytes = upload.memory.size;
      retired.pool_class = upload.pool_class;   // S102 RT1: DrainRetired keeps it for the next upload (after this slot's fence)
      if (upload.pool_class && rdr::g_lean_plant.load(std::memory_order_relaxed) == 3) {   // PLANT 3 (tools only): handed back while its copy is still to run
        staging_pool.Give(upload.pool_class, PooledStaging{upload.staging, upload.memory});
        retired.buffer = VK_NULL_HANDLE, retired.memory = Allocation{}, retired.pool_class = 0;
      }
      slot.retired.push_back(retired);
      g_render_times.upload_bytes += upload.memory.size;
      if (options.counts) counts_cur.upload_bytes += upload.bytes;  // S102 M1 (copies are counted where they are recorded)
    }
    if (!entry.live) continue;
    const uint32_t sub = upload.mip * entry.layers + upload.layer;
    if (!batch) {
      Transition(entry, sub, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, true);
      FlushBarriers(cmd);
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VkImageAspectFlags(entry.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), upload.mip, upload.layer, 1};
    // A scaled image (the render scale) gets the game's contents at the game's own size (S100, the 1.5x check run 7022: a 1280x720 upload
    // copied as 1920x1080 read past its buffer): copied into a temporary image of that size, then blitted up (linear) onto the scaled one.
    const uint32_t guest_w = std::max(1u, uint32_t(entry.desc.width) >> upload.mip), guest_h = std::max(1u, uint32_t(entry.desc.height) >> upload.mip);
    const uint32_t scaled_w = std::max(1u, entry.width >> upload.mip), scaled_h = std::max(1u, entry.height >> upload.mip);
    if (!entry.depth && (guest_w != scaled_w || guest_h != scaled_h)) {
      ++out_scaled;  // S102 G5: a temporary image, its memory and a blit for this upload (render scale other than 1)
      VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      info.imageType = VK_IMAGE_TYPE_2D;
      info.format = entry.format;
      info.extent = {guest_w, guest_h, 1};
      info.mipLevels = 1;
      info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT;
      info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      Retired temp;
      if (vkCreateImage(device.device(), &info, nullptr, &temp.image) == VK_SUCCESS) {
        VkMemoryRequirements needs{};
        vkGetImageMemoryRequirements(device.device(), temp.image, &needs);
        if (allocator.Alloc(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, temp.memory)) {
          vkBindImageMemory(device.device(), temp.image, temp.memory.memory, temp.memory.offset);
          const VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
          to_dst.srcAccessMask = 0, to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          to_dst.srcQueueFamilyIndex = to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          to_dst.image = temp.image, to_dst.subresourceRange = whole;
          vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);
          CountBarrier(1), ++out_up_explicit;  // S102 M1 (+ G5: counted where recorded)
          VkBufferImageCopy guest_region{};
          guest_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          guest_region.imageExtent = {guest_w, guest_h, 1};
          vkCmdCopyBufferToImage(cmd, upload.staging, temp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &guest_region);
          CountCopy();  // S102 M1
          VkImageMemoryBarrier to_src = to_dst;
          to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_src);
          CountBarrier(1), ++out_up_explicit;  // S102 M1 (+ G5: counted where recorded)
          VkImageBlit blit{};
          blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          blit.srcOffsets[1] = {int32_t(guest_w), int32_t(guest_h), 1};
          blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, upload.mip, upload.layer, 1};
          blit.dstOffsets[1] = {int32_t(scaled_w), int32_t(scaled_h), 1};
          vkCmdBlitImage(cmd, temp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, BlitFilterFor(entry.desc.format, true));
          CountCopy();  // S102 M1
        } else {
          Problem("scaled_upload_memory", "%ux%u", guest_w, guest_h);
        }
        slot.retired.push_back(temp);  // destroyed (with its memory) once this slot's GPU work is done
      }
    } else {
      region.imageExtent = {scaled_w, scaled_h, 1};
      vkCmdCopyBufferToImage(cmd, upload.staging, entry.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      CountCopy();  // S102 M1
    }
    if (!batch) {
      Transition(entry, sub, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
      FlushBarriers(cmd);
    }
  }
  if (batch) {
    for (size_t ui = seg_begin; ui < seg_end; ++ui) {
      const ImageUpload& upload = image_work[ui];
      ImageEntry& entry = images[upload.image];
      if (!entry.live) continue;
      Transition(entry, upload.mip * entry.layers + upload.layer, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT, false);
    }
    if (batch_plant == 1) barriers.clear(), barrier_src = barrier_dst = 0;  // plant 1: the last flush is dropped (the recorded state says shader-read, the image stays in copy-destination)
    else FlushBarriers(cmd);
  }
  }
  if (!buffer_work.empty()) {
    // S103 PVR (docs/research/s103/UPF.md section 5): a stamp between the image part and the buffer part, so the [rdr][gpu][uploads] line reads them apart. Only in the stamp-per-step mode
    // (g_gpu_pass_stamps); counted as a post stamp (ts_w_post) so the identity of the stamps written still holds; the post stamp that follows ApplyUploads closes the buffer part.
    if (slot.ts_detail && rdr::g_gpu_pass_stamps.load(std::memory_order_relaxed) && !image_work.empty() && slot.ts_w_pre > 0 && slot.ts_count + 3 < slot.ts_cap) {
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, slot.ts_count);
      slot.ts_up_mid = slot.ts_count++;
      ++slot.ts_w_post;
      slot.ts_class.push_back(kGpuOther);
      slot.ts_image.push_back(0), slot.ts_kind.push_back(7), slot.ts_src.push_back(255);
      if (slot.ts_trace) slot.ts_pass.push_back(0xFFFFFFFFu);
      ++slot.ts_need;
    }
    VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    before.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
    CountBarrier(0), ++out_up_explicit;  // S102 M1 (+ G5)
    for (const BufferUpload& upload : buffer_work) {
      VkBufferCopy region{0, upload.offset, upload.bytes};
      vkCmdCopyBuffer(cmd, upload.staging, upload.dest, 1, &region);
      CountCopy();  // S102 M1
      ++out_up_by_producer[std::min<uint8_t>(upload.producer, 2)];   // S103 PVR
      if (options.counts) counts_cur.upload_bytes += upload.bytes;
      Retired retired;
      retired.buffer = upload.staging;
      retired.memory = upload.memory;
      retired.staged_bytes = upload.memory.size;
      slot.retired.push_back(retired);
    }
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
    CountBarrier(0), ++out_up_explicit;  // S102 M1 (+ G5)
  }
  // S102 G5: the barriers this call recorded, counted where they are recorded: the flushes (FlushBarriers) and the explicit ones (a scaled upload's temp image, the buffer copies).
  out_up_barriers = uint32_t(out_flushes - flushes_before) + out_up_explicit;
}
