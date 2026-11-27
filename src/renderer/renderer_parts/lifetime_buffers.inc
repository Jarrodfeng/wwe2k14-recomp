static RenderTimes g_render_times;
static uint64_t RenderNow() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void Renderer::Impl::DrainRetired(FrameSlot& slot) {
  VkDevice dev = device.device();
  uint64_t released = 0;
  for (const Retired& item : slot.retired) {
    if (item.lean_set) vkFreeDescriptorSets(dev, lean_pool, 1, &item.lean_set);   // S102 RT1
    if (item.framebuffer) vkDestroyFramebuffer(dev, item.framebuffer, nullptr);
    if (item.view) vkDestroyImageView(dev, item.view, nullptr);
    if (item.image) vkDestroyImage(dev, item.image, nullptr);
    const bool kept = item.pool_class && item.buffer && staging_pool.Give(item.pool_class, PooledStaging{item.buffer, item.memory});   // S102 RT1: the slot's fence has signalled: its copy is done
    if (item.buffer && !kept) vkDestroyBuffer(dev, item.buffer, nullptr);
    if (item.memory.block >= 0 && !kept) allocator.Free(item.memory);
    if (item.free_image) images.Remove(item.free_image);
    if (item.free_buffer) buffers.Remove(item.free_buffer);
    released += item.staged_bytes;
  }
  slot.retired.clear();
  if (released) {
    staged_bytes.fetch_sub(released, std::memory_order_relaxed);
    staging_drained.notify_all();
  }
}

// Loading a lot of textures at once must not run the host out of memory. Bounded: a game that has stopped presenting must not hang here.
void Renderer::Impl::WaitForStagingRoom() {
  if (staged_bytes.load(std::memory_order_relaxed) < max_staged) return;
  // S102 U1 (review): staging is freed only by DrainRetired, which runs in Render on the render thread: a wait on that thread can only time out (2 s a call).
  // Before the first Render (render_thread 0) nothing has run that could free staging either: no wait then.
  if (!staging_wait_plant) {
    const size_t render = render_thread.load(std::memory_order_relaxed);
    const size_t me = std::hash<std::thread::id>()(std::this_thread::get_id());
    if (render == 0 || render == me) return;
    if (me == producer_thread.load(std::memory_order_relaxed)) return;  // S102 R21-B: the bind stage hands Render its frames; waiting here would wait on itself
  }
  std::unique_lock<std::mutex> lock(upload_mutex);
  staging_drained.wait_for(lock, std::chrono::seconds(2), [&] { return staged_bytes.load(std::memory_order_relaxed) < max_staged; });
}

// Runs on the render thread only, so nothing here races recording.
void Renderer::Impl::ProcessDestroys(FrameSlot& slot) {
  std::vector<PendingDestroy> ready;
  {
    std::lock_guard<std::mutex> lock(destroy_mutex);
    auto split = std::stable_partition(destroy_queue.begin(), destroy_queue.end(), [&](const PendingDestroy& item) { return item.ready_serial > serial; });
    ready.assign(split, destroy_queue.end());
    destroy_queue.erase(split, destroy_queue.end());
  }
  g_render_times.destroy_items += ready.size();
  last_destroys = uint32_t(ready.size());  // S102 D1: the frame's crumb
  for (const PendingDestroy& item : ready) {
    if (item.image) {
      ImageEntry& entry = images[item.image];
      if (!entry.live) continue;
      for (auto it = framebuffers.begin(); it != framebuffers.end();) {
        bool uses = false;
        for (const auto& view : entry.target_views) {
          for (VkImageView key_view : it->first.views) uses |= key_view == view.second;
        }
        if (uses) {
          Retired retired;
          retired.framebuffer = it->second;
          slot.retired.push_back(retired);
          it = framebuffers.erase(it);
        } else {
          ++it;
        }
      }
      const auto retire_views = [&](auto& views) {
        for (auto& view : views) {
          Retired retired;
          retired.view = view.second;
          slot.retired.push_back(retired);
        }
        views.clear();
      };
      if (lean_cache.size()) {  // S102 RT1: the views a kept set may name (a sampled view; the reinterpreted ones)
        lean_dying.push_back(reinterpret_cast<uint64_t>(entry.sampled_view));
        for (auto& view : entry.reinterpreted_views) lean_dying.push_back(reinterpret_cast<uint64_t>(view.second));
      }
      retire_views(entry.target_views);
      retire_views(entry.reinterpreted_views);
      Retired retired;
      retired.image = entry.image;
      retired.view = entry.sampled_view;
      retired.memory = entry.memory;
      retired.free_image = item.image;
      slot.retired.push_back(retired);
      entry.sampled_view = VK_NULL_HANDLE;
      entry.image = VK_NULL_HANDLE;
      entry.memory = Allocation{};
      entry.live = false;
    } else if (item.buffer) {
      BufferEntry& entry = buffers[item.buffer];
      if (!entry.live) continue;
      Retired retired;
      retired.buffer = entry.buffer;
      retired.memory = entry.memory;
      retired.free_buffer = item.buffer;
      slot.retired.push_back(retired);
      entry.buffer = VK_NULL_HANDLE;
      entry.memory = Allocation{};
      entry.live = false;
    }
  }
  LeanEvictDying(slot);   // S102 RT1: every kept set that names a view of a destroyed image goes (into THIS slot's retired list: the destroy was gated until no frame in flight names the image)
}

void Renderer::Impl::LeanEvictDying(FrameSlot& slot) {   // S102 RT1
  if (lean_dying.empty()) return;
  lean_freed.clear();
  const int plant = rdr::g_lean_plant.load(std::memory_order_relaxed);
  const size_t gone = lean_cache.EvictViews(lean_dying, lean_freed, plant == 1);
  for (VkDescriptorSet freed : lean_freed) {
    if (plant == 7 && lean_pool) {   // PLANT 7 (tools only): freed at once, while frames in flight may still use the set
      vkFreeDescriptorSets(device.device(), lean_pool, 1, &freed);
      continue;
    }
    Retired retired;
    retired.lean_set = freed;
    slot.retired.push_back(retired);
  }
  rdr::LeanAdd(rdr::kLcSetEvict, gone);
  lean_dying.clear();
}

// S102 RT1 (tools only, rplay --lean-churn N / --lean-audit N): the proof that eviction runs. Every N frames one live image's sampled view is REPLACED (a view change: a new view of the same image,
// the old one destroyed after the destroy delay, exactly the views a destroyed image would retire); the same happens in the arm with the lever off, so the pictures stay comparable.
// The audit counts kept sets that name a view no live image owns (must be 0).
void Renderer::Impl::LeanTool(FrameSlot& slot, const Frame& frame) {
  using namespace rdr;
  for (auto it = lean_churned.begin(); it != lean_churned.end();) {
    if (it->second <= serial) {
      Retired retired;
      retired.view = it->first;
      slot.retired.push_back(retired);
      it = lean_churned.erase(it);
    } else {
      ++it;
    }
  }
  const int every = g_lean_churn_every.load(std::memory_order_relaxed);
  if (every > 0 && serial % uint64_t(every) == 0) {
    std::vector<uint32_t> sampled;   // the images this frame samples (first seen order): the churn hits views the kept sets name; the same choice in both arms
    for (uint32_t t = 0; t < frame.texture_sets.count; ++t)
      for (const TextureSlot& texture : frame.texture_sets[t].slots)
        if (texture.image != kNoImage && texture.image < images.high_water() && images[texture.image].live && images[texture.image].sampled_view != VK_NULL_HANDLE && images[texture.image].desc.samples == 1 &&
            std::find(sampled.begin(), sampled.end(), uint32_t(texture.image)) == sampled.end())
          sampled.push_back(uint32_t(texture.image));
    if (!sampled.empty()) {
      do {
        const uint32_t id = sampled[uint32_t((serial / uint64_t(every)) * 7) % uint32_t(sampled.size())];
        ImageEntry& entry = images[id];
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = entry.image;
        view.viewType = entry.desc.kind == ImageKind::Cube ? VK_IMAGE_VIEW_TYPE_CUBE : entry.desc.kind == ImageKind::Volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        view.format = entry.format;
        view.subresourceRange = {VkImageAspectFlags(entry.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, entry.desc.mips, 0, entry.layers};
        VkImageView fresh = VK_NULL_HANDLE;
        if (vkCreateImageView(device.device(), &view, nullptr, &fresh) != VK_SUCCESS) break;
        const VkImageView old = entry.sampled_view;
        entry.sampled_view = fresh;
        lean_dying.push_back(reinterpret_cast<uint64_t>(old));
        lean_churned.push_back({old, serial + kDestroyDelayFrames});
        for (auto& other : entry.reinterpreted_views) {   // the other views of the image are replaced the same way (made again on first use by SampledView)
          lean_dying.push_back(reinterpret_cast<uint64_t>(other.second));
          lean_churned.push_back({other.second, serial + kDestroyDelayFrames});
        }
        entry.reinterpreted_views.clear();
        LeanEvictDying(slot);
        LeanAdd(kLcChurn);
      } while (false);
    }
  }
  const int audit = g_lean_audit_every.load(std::memory_order_relaxed);
  if (audit > 0 && serial % uint64_t(audit) == 0 && lean_cache.size()) {
    std::unordered_set<uint64_t> live_views;
    for (uint32_t id = 1; id < images.high_water(); ++id) {
      const ImageEntry& entry = images[id];
      if (!entry.live) continue;
      live_views.insert(reinterpret_cast<uint64_t>(entry.sampled_view));
      for (const auto& view : entry.reinterpreted_views) live_views.insert(reinterpret_cast<uint64_t>(view.second));
    }
    uint64_t bad = 0;
    lean_cache.ForEach([&](const auto& e) {
      for (size_t i = 0; i < size_t(kMaxTextures); ++i) bad += e.key.view[i] != 0 && !live_views.count(e.key.view[i]);
    });
    LeanAdd(kLcAudit);
    LeanAdd(kLcAuditBad, bad);
  }
}

// S102 D1 (breadcrumbs.h): the marker words, kMarkerWords a frame slot, host-visible so they can be read after the device is lost. Made on first use;
// a failure turns the markers off for the run (one line), never the frame.
bool Renderer::Impl::EnsureMarker() {
  if (marker_buffer) return true;
  if (marker_failed) return false;
  VkDevice dev = device.device();
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = VkDeviceSize(slots.size()) * crumbs::kMarkerWords * 4;
  info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (vkCreateBuffer(dev, &info, nullptr, &marker_buffer) != VK_SUCCESS) {
    marker_buffer = VK_NULL_HANDLE, marker_failed = true;
    Log("[rdr][crumbs] no GPU marker buffer (vkCreateBuffer failed): the breadcrumbs keep the CPU ring only");
    return false;
  }
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, marker_buffer, &needs);
  if (!allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, true, marker_memory) || !marker_memory.mapped) {
    vkDestroyBuffer(dev, marker_buffer, nullptr);
    marker_buffer = VK_NULL_HANDLE, marker_failed = true;
    Log("[rdr][crumbs] no GPU marker buffer (no host-visible memory): the breadcrumbs keep the CPU ring only");
    return false;
  }
  vkBindBufferMemory(dev, marker_buffer, marker_memory.memory, marker_memory.offset);
  std::memset(marker_memory.mapped, 0, size_t(info.size));
  return true;
}

// The marker words (breadcrumbs.h). Outside a render pass only. MarkBegin closes the open marker, then writes the next one's BEGIN (no barrier: the
// front end's reach); MarkClose writes the open marker's END after all earlier commands finished and makes it visible to the host before anything after it.
uint16_t Renderer::Impl::MarkBegin(VkCommandBuffer cmd, uint32_t slot_index) {
  if (!marker_buffer) return crumbs::kNoMarker;
  MarkClose(cmd, slot_index);
  if (marker_next > crumbs::kMaxMarker) return crumbs::kNoMarker;
  const uint32_t k = marker_next++;
  const uint32_t value = crumbs::MarkValue(serial);
  vkCmdUpdateBuffer(cmd, marker_buffer, (VkDeviceSize(slot_index) * crumbs::kMarkerWords + crumbs::BeginWord(k)) * 4, 4, &value);
  marker_open = k;
  return uint16_t(k);
}

void Renderer::Impl::MarkClose(VkCommandBuffer cmd, uint32_t slot_index) {
  if (!marker_buffer || !marker_open) return;
  if (frame_marker == 4) {  // mode 4: BEGIN words only, no barriers (the GPU is not serialised; a race the barriers would hide stays possible)
    marker_open = 0;
    return;
  }
  const bool bare = crumbs::g_plant.load(std::memory_order_relaxed) == 4;  // plant 4: the first version's stream (no barriers)
  if (!bare) vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 0, nullptr);
  const uint32_t value = crumbs::MarkValue(serial);
  vkCmdUpdateBuffer(cmd, marker_buffer, (VkDeviceSize(slot_index) * crumbs::kMarkerWords + crumbs::EndWord(marker_open)) * 4, 4, &value);
  if (!bare) {
    VkMemoryBarrier flush{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    flush.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, flush.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &flush, 0, nullptr, 0, nullptr);
  }
  marker_open = 0;
}

// The first sign of a lost device (a fence wait, the submit, the acquire or the present returned VK_ERROR_DEVICE_LOST): one loud line, the accounting
// switched to "nothing presented" (rdr::g_device_lost: the swapchain accepts no present, frames_*.csv device_lost = 1), and the breadcrumbs. Once a run.
// `simulated`: rplay's selftest (the device is fine; the GPU is held at a planted wait).
void Renderer::Impl::OnDeviceLost(const char* where, int result, bool simulated) {
  frames_dropped.fetch_add(1, std::memory_order_relaxed);  // S103 CB1: whatever this frame recorded is gone
  if (lost_serial) return;
  lost_serial = serial + 1;
  g_device_lost.store(serial + 1, std::memory_order_relaxed);
  crumbs::g_presented_at_loss.store(g_presented.load(std::memory_order_relaxed), std::memory_order_relaxed);
  lost_simulated = simulated;
  Log("[rdr][DEVICE_LOST] ===== the Vulkan device is LOST (%s, VkResult %d, frame serial %llu, %s). The picture is frozen from here; presents no longer count. =====", where, result,
      static_cast<unsigned long long>(serial), device.properties().deviceName);
  DumpCrumbs(simulated);
  // Last: a RDR_DEV build (rplay / fview) stops on a Problem, and the dump must be out first (review F4).
  Problem("device_lost", "%s returned VkResult %d at frame serial %llu: the GPU stopped and nothing will be drawn again this run (docs\\renderer\\DEVICE_LOST.md)%s", where, result,
          static_cast<unsigned long long>(serial), simulated ? " [selftest: simulated]" : "");
}

// The dump: per frame slot what the marker words say the GPU reached, then the kept frames (newest last) with their steps around that point, then the last
// pipelines built. Every line starts [rdr][crumbs].
void Renderer::Impl::DumpCrumbs(bool simulated) {
  const auto prog_name = [&](uint16_t p) -> const char* { return p < programs.size() && programs[p].name ? programs[p].name : (p == 0xFFFF ? "-" : "?"); };
  const auto step_line = [&](const crumbs::Frame& f, const crumbs::Step& s, const char* tag) {
    Log("[rdr][crumbs] %s frame %llu step %u: %s %u, class %s, image %u, marker %d, draws %u, binds %u, verts+indices %u, programs %s .. %s", tag,
        static_cast<unsigned long long>(f.serial), s.step, crumbs::StepKindName(s.kind), s.index, GpuClassName(s.cls), s.image, s.marker == crumbs::kNoMarker ? -1 : int(s.marker),
        s.draws, s.binds, s.prims, prog_name(s.first_prog), prog_name(s.last_prog));
  };
  if (!crumbs::g_on.load(std::memory_order_relaxed)) {
    Log("[rdr][crumbs] off (--wwe2k14_rdr_breadcrumbs=false): nothing kept");
    return;
  }
  // 1. The marker words of every slot.
  const uint32_t* words_all = marker_buffer && marker_memory.mapped ? reinterpret_cast<const uint32_t*>(marker_memory.mapped) : nullptr;
  if (!words_all) Log("[rdr][crumbs] no GPU marker words (--wwe2k14_rdr_gpu_marker=0, or no buffer): only the CPU ring below");
  const crumbs::Frame* suspect = nullptr;
  int32_t suspect_marker = -1;
  for (uint32_t s = 0; words_all && s < slots.size(); ++s) {
    const uint32_t* words = words_all + size_t(s) * crumbs::kMarkerWords;
    const crumbs::Frame* f = words[0] ? crumb_ring.FindLow(words[0]) : nullptr;
    crumbs::SlotReach r = crumbs::Reach(words, f ? f->end_marker : crumbs::kNoMarker);
    if (f && f->marker_mode == 4) {  // mode 4 has no END words: the front end's reach names the step (the fault is at or after it)
      r.unfinished = r.begun < 0 ? 1 : r.begun;
      r.done = f->end_marker != crumbs::kNoMarker && r.begun == int32_t(f->end_marker);
    }
    if (!r.any) {
      Log("[rdr][crumbs] GPU slot %u: no marker written yet", s);
      continue;
    }
    const crumbs::Step* at = f ? crumbs::StepOfMarker(*f, r.unfinished) : nullptr;
    const char* where = r.done ? "its END (all its work finished)" : r.unfinished == int32_t(crumbs::kMarkUploads) ? "the uploads" : at ? "a step" :
                        f && r.unfinished == int32_t(f->end_marker) ? "the frame's tail (the read-backs after the last step)" : "a marker the ring does not hold";
    Log("[rdr][crumbs] GPU slot %u: frame %llu (word %08x): finished marker %d, first unfinished %d (%s), front end reached %d, end marker %d%s", s,
        f ? static_cast<unsigned long long>(f->serial) : 0ull, r.serial_lo, r.ended, r.done ? -1 : r.unfinished, where, r.begun, f && f->end_marker != crumbs::kNoMarker ? int(f->end_marker) : -1,
        f ? (f->submitted ? "" : " (its submit FAILED: never ran)") : " (not in the ring)");
    if (at && !r.done) step_line(*f, *at, "  unfinished:");
    if (f && f->submitted && !r.done && (!suspect || f->serial < suspect->serial)) suspect = f, suspect_marker = r.unfinished;  // the oldest submitted frame that did not finish
  }
  if (suspect) {
    const crumbs::Step* at = crumbs::StepOfMarker(*suspect, suspect_marker);
    Log("[rdr][crumbs] VERDICT: the GPU stopped in frame %llu in %s or the steps after it up to the next marked one %s",
        static_cast<unsigned long long>(suspect->serial), at ? "the step below" : suspect_marker == int32_t(crumbs::kMarkUploads) ? "the uploads" : "the frame's tail (read-backs)",
        suspect->marker_mode == 4 ? " (mode 4, BEGIN words only: the front end reached it; it may have run ahead of the shaders, and the newest word can read one short)" : " (every marked work before it FINISHED: its END word was written after a full barrier)");
    if (at) step_line(*suspect, *at, "  VERDICT");
  } else if (words_all) {
    Log("[rdr][crumbs] VERDICT: every submitted frame in the ring finished its end marker: the loss is not inside a frame's commands the markers cover (a present, the driver, or a frame not kept)");
  }
  // 2. The kept frames, oldest first: one summary line each; the suspect frame's steps around the marker (all of them for the newest submitted frame when there is no marker).
  const crumbs::Frame* newest_submitted = nullptr;
  for (const crumbs::Frame& f : crumb_ring.frames)
    if (f.serial != ~0ull && f.submitted && (!newest_submitted || f.serial > newest_submitted->serial)) newest_submitted = &f;
  for (uint64_t back = crumbs::kFrames; back-- > 0;) {
    if (serial < back) continue;
    const crumbs::Frame* f = crumb_ring.Find(serial - back);
    if (!f) continue;
    uint32_t passes_n = 0, copies_n = 0, other_n = 0;
    uint64_t draws = 0, prims = 0;
    for (uint32_t i = 0; i < f->n; ++i) {
      const crumbs::Step& s = f->steps[i];
      (s.kind == crumbs::kPass ? passes_n : s.kind == crumbs::kCopy || s.kind == crumbs::kCopyBatch ? copies_n : other_n) += 1;
      if (s.kind == crumbs::kPass) draws += s.draws, prims += s.prims;
    }
    Log("[rdr][crumbs] frame %llu (slot %u, mode %u, %s): steps %u (kept %u: passes %u, copies %u, other %u), draws %llu, verts+indices %llu, uploads %u, destroys %u, pipelines built %u, markers %s",
        static_cast<unsigned long long>(f->serial), f->slot, unsigned(f->mode), f->submitted ? "submitted" : "NOT submitted", f->steps_total, f->n, passes_n, copies_n, other_n,
        static_cast<unsigned long long>(draws), static_cast<unsigned long long>(prims), f->uploads, f->destroys, f->built, f->marker_mode == 4 ? "every step, BEGIN only" : f->marker_mode >= 3 ? "every step" : f->marker_mode == 2 ? "passes + copy runs" : f->marker_mode == 1 ? "class runs" : "off");
    // without a marker verdict, the steps of the newest TWO submitted frames (frames in flight are 2: either can hold the fault)
    if (f == suspect || (!suspect && (f == newest_submitted || (newest_submitted && f->serial + 1 == newest_submitted->serial && f->submitted)))) {
      const crumbs::Step* at = suspect ? crumbs::StepOfMarker(*f, suspect_marker) : nullptr;
      uint32_t from = 0, to = f->n;
      if (at) {
        const uint32_t i = uint32_t(at - f->steps);
        from = i > 6 ? i - 6 : 0, to = std::min(f->n, i + 10);
      }
      for (uint32_t i = from; i < to; ++i) step_line(*f, f->steps[i], &f->steps[i] == at ? "  >>" : "    ");
    }
  }
  // 3. The last pipelines built (they are made on the render thread between submits: a first-use pipeline was the newest thing in 456's loss frame).
  const uint64_t nb = std::min<uint64_t>(crumb_ring.built_count, crumbs::kBuilt);
  for (uint64_t i = crumb_ring.built_count - nb; i < crumb_ring.built_count; ++i) {
    const crumbs::Built& b = crumb_ring.built[i % crumbs::kBuilt];
    Log("[rdr][crumbs] pipeline built at frame %llu: %s, class %u", static_cast<unsigned long long>(b.serial), prog_name(b.program), unsigned(b.cls));
  }
  Log("[rdr][crumbs] end of the dump%s", simulated ? " (selftest)" : "");
}

// S102 D1 round 6: one full memory barrier (wwe2k14_rdr_frame_top_barrier). Outside a render pass only. The role audit names it (a planted twin that drops it is seen).
void Renderer::Impl::RecordFrameBarrier(VkCommandBuffer cmd, const char* where) {
  VkMemoryBarrier all{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  all.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  all.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &all, 0, nullptr, 0, nullptr);
  crumbs::g_frame_barrier_count.fetch_add(1, std::memory_order_relaxed);
  if (options.counts) ++counts_cur.total.barriers;
  if (std::FILE* ra = crumbs::g_role_audit.load(std::memory_order_relaxed)) std::fprintf(ra, "{\"f\":%llu,\"s\":-1,\"m\":0,\"k\":\"frame_barrier\",\"w\":\"%s\"}\n", static_cast<unsigned long long>(serial), where);
}

void Renderer::DestroyImage(ImageId image) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lock(im.destroy_mutex);
  PendingDestroy item;
  item.image = image;
  item.ready_serial = std::atomic_ref<uint64_t>(im.serial).load(std::memory_order_relaxed) + (g_plant_destroy_now.load(std::memory_order_relaxed) ? 0 : kDestroyDelayFrames);  // S102 R21-B: read on the bind stage (plant: no delay)
  im.destroy_queue.push_back(item);
}

BufferId Renderer::CreateBuffer(size_t bytes, const void* initial) {
  Impl& im = *impl_;
  const uint32_t id = im.buffers.Add();
  if (!id) {
    Problem("buffer_table_full", "more than %u buffers", kChunkSize * kMaxChunks);
    return kNoBuffer;
  }
  BufferEntry& entry = im.buffers[id];
  VkDevice dev = im.device.device();
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  vkCreateBuffer(dev, &info, nullptr, &entry.buffer);
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, entry.buffer, &needs);
  const bool direct = im.device.caps().unified_memory;
  const VkMemoryPropertyFlags required = direct ? (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  if (!im.allocator.Alloc(needs, required, 0, true, entry.memory)) {
    Problem("buffer_memory_failed", "%zu bytes", bytes);
    vkDestroyBuffer(dev, entry.buffer, nullptr);
    entry.buffer = VK_NULL_HANDLE;
    im.buffers.Remove(id);
    return kNoBuffer;
  }
  vkBindBufferMemory(dev, entry.buffer, entry.memory.memory, entry.memory.offset);
  entry.bytes = bytes;
  entry.live = true;
  if (initial && entry.memory.mapped) std::memcpy(entry.memory.mapped, initial, bytes);  // a new buffer: the GPU cannot be using it yet
  else if (initial) UpdateBuffer(BufferId(id), 0, initial, bytes);
  return BufferId(id);
}

void Renderer::UpdateBuffer(BufferId buffer, size_t offset, const void* data, size_t bytes, uint8_t producer) {
  Impl& im = *impl_;
  im.WaitForStagingRoom();
  BufferEntry& entry = im.buffers[buffer];
  VkDevice dev = im.device.device();
  BufferUpload upload{VK_NULL_HANDLE, {}, entry.buffer, offset, bytes, producer};
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  vkCreateBuffer(dev, &info, nullptr, &upload.staging);
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, upload.staging, &needs);
  if (!im.allocator.Alloc(needs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0, true, upload.memory)) {
    Problem("staging_memory_failed", "%zu bytes", bytes);
    vkDestroyBuffer(dev, upload.staging, nullptr);
    return;
  }
  vkBindBufferMemory(dev, upload.staging, upload.memory.memory, upload.memory.offset);
  std::memcpy(upload.memory.mapped, data, bytes);
  im.staged_bytes.fetch_add(needs.size, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(im.upload_mutex);
  im.buffer_uploads.push_back(upload);
}

BufferId Renderer::CreateHostBuffer(size_t bytes, uint8_t** mapped) {   // S103 PF2
  Impl& im = *impl_;
  if (mapped) *mapped = nullptr;
  const uint32_t id = im.buffers.Add();
  if (!id) {
    Problem("buffer_table_full", "more than %u buffers", kChunkSize * kMaxChunks);
    return kNoBuffer;
  }
  BufferEntry& entry = im.buffers[id];
  VkDevice dev = im.device.device();
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = bytes;
  info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (vkCreateBuffer(dev, &info, nullptr, &entry.buffer) != VK_SUCCESS) {   // S103 PF2 (review): a failed create is kNoBuffer (the caller's pcopy_failed route), never a bind of a null buffer
    Problem("buffer_create_failed", "%zu bytes (host-visible)", bytes);
    entry.buffer = VK_NULL_HANDLE;
    im.buffers.Remove(id);
    return kNoBuffer;
  }
  VkMemoryRequirements needs{};
  vkGetBufferMemoryRequirements(dev, entry.buffer, &needs);
  const bool unified = im.device.caps().unified_memory;
  const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  const bool got = im.allocator.Alloc(needs, unified ? (visible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) : visible, unified ? 0 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, entry.memory);
  if (!got || !entry.memory.mapped) {
    if (got) im.allocator.Free(entry.memory);
    Problem("buffer_memory_failed", "%zu bytes (host-visible)", bytes);
    vkDestroyBuffer(dev, entry.buffer, nullptr);
    entry.buffer = VK_NULL_HANDLE;
    im.buffers.Remove(id);
    return kNoBuffer;
  }
  vkBindBufferMemory(dev, entry.buffer, entry.memory.memory, entry.memory.offset);
  entry.bytes = bytes;
  entry.live = true;
  entry.host = true, entry.last_read = 0;
  im.host_buffers.fetch_add(1, std::memory_order_relaxed);
  if (static std::atomic<bool> logged{false}; !logged.exchange(true)) {   // once: the memory type the posed copies got (the next Surface log shows whether it is cached or write-combined)
    const VkMemoryPropertyFlags f = im.device.memory().memoryTypes[entry.memory.type].propertyFlags;
    Log("[rdr] host-visible vertex buffers (posed copies): memory type %d flags 0x%x:%s%s%s%s%s (unified device %d)", entry.memory.type, unsigned(f), (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? " DEVICE_LOCAL" : "", (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " HOST_VISIBLE" : "",
        (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? " HOST_COHERENT" : "", (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? " HOST_CACHED" : " (write-combined: not cached)", "", int(unified));
  }
  if (mapped) *mapped = static_cast<uint8_t*>(entry.memory.mapped);
  return BufferId(id);
}

bool Renderer::HostBufferIdle(BufferId buffer) const {
  const BufferEntry& e = impl_->buffers[buffer];
  if (!e.host) return true;
  const uint64_t lr = std::atomic_ref<uint64_t>(const_cast<uint64_t&>(e.last_read)).load(std::memory_order_relaxed);
  return lr == 0 || lr - 1 + impl_->slots.size() < RenderSerial();   // a Render s waits for frame s - slots before it records: frames with index + slots < serial are finished
}
uint64_t Renderer::DirectOverruns() const { return impl_->direct_overruns.load(std::memory_order_relaxed); }

bool Renderer::UnifiedMemory() const { return impl_->device.caps().unified_memory; }
uint32_t Renderer::FramesInFlight() const { return uint32_t(impl_->slots.size()); }
uint64_t Renderer::RenderSerial() const { return std::atomic_ref<uint64_t>(impl_->serial).load(std::memory_order_relaxed); }

void Renderer::DestroyBuffer(BufferId buffer) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lock(im.destroy_mutex);
  PendingDestroy item;
  item.buffer = buffer;
  item.ready_serial = std::atomic_ref<uint64_t>(im.serial).load(std::memory_order_relaxed) + (g_plant_destroy_now.load(std::memory_order_relaxed) ? 0 : kDestroyDelayFrames);  // S102 R21-B: read on the bind stage (plant: no delay)
  im.destroy_queue.push_back(item);
}

uint16_t Renderer::AddProgram(const ProgramDesc& desc) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> tables(im.tables_mutex);  // S102 R21-B (hazards 1, 2)
  uint16_t same = 0;
  if (im.store_on && im.StoreFindProgram(desc, same)) return same;  // lane 276: the same words are the same program (the prebuilt one)
  Program program;
  program.name = desc.name;
  program.prepass_class = colour_cuts::ClassifyFragment(desc.fragment_spirv, desc.fragment_bytes);  // S102 G10
  program.half_ok = desc.fragment_spirv && !half_trans::ReadsFragCoord(desc.fragment_spirv, desc.fragment_bytes);  // S102 C4
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = desc.vertex_bytes;
  info.pCode = desc.vertex_spirv;
  vkCreateShaderModule(im.device.device(), &info, nullptr, &program.vertex);
  if (desc.fragment_spirv) {  // none: a depth-only program
    info.codeSize = desc.fragment_bytes;
    info.pCode = desc.fragment_spirv;
    vkCreateShaderModule(im.device.device(), &info, nullptr, &program.fragment);
  }
  if (im.programs.size() >= kNoTableId || !im.programs.push_back(program)) {  // S102 R21-B (review F5): full: named, and an id no draw can use (never program 0, a real one)
    Problem("program_table_full", "%u programs", unsigned(kNoTableId));
    vkDestroyShaderModule(im.device.device(), program.vertex, nullptr);
    if (program.fragment) vkDestroyShaderModule(im.device.device(), program.fragment, nullptr);
    return kNoTableId;
  }
  if (im.store_on) im.StoreProgramAdded(uint16_t(im.programs.size() - 1));
  return uint16_t(im.programs.size() - 1);
}

uint16_t Renderer::AddVertexLayout(const VertexLayoutDesc& desc) {
  std::lock_guard<std::mutex> tables(impl_->tables_mutex);  // S102 R21-B (hazards 1, 2)
  // S102 R21-B (review F5): a layout appended, or false (named) when the table is full; the caller returns kNoTableId, never the previous layout's id.
  const auto PushLayout = [](Impl& im, const VertexLayoutDesc& d) {
    if (im.layouts.size() >= kNoTableId || !im.layouts.push_back(d)) {
      Problem("layout_table_full", "%u vertex layouts", unsigned(kNoTableId));
      return false;
    }
    return true;
  };
  if (impl_->store_on) {  // lane 276: the same content is the same layout (the prebuilt one), kept canonical
    const uint64_t hash = pstore::HashLayout(desc);
    const VertexLayoutDesc canon = pstore::Canonical(desc);
    auto found = impl_->layout_by_hash.find(hash);
    if (found != impl_->layout_by_hash.end()) {
      const VertexLayoutDesc& have = impl_->layouts[found->second];
      if (have.attribute_count == canon.attribute_count && std::memcmp(have.attributes, canon.attributes, sizeof(VertexAttribute) * canon.attribute_count) == 0 &&
          std::memcmp(have.strides, canon.strides, sizeof(canon.strides)) == 0 && std::memcmp(have.per_instance, canon.per_instance, sizeof(canon.per_instance)) == 0)
        return found->second;
      if (!PushLayout(*impl_, canon)) return kNoTableId;  // a 64-bit hash collision (never seen): its own id, unindexed
      return uint16_t(impl_->layouts.size() - 1);
    }
    if (!PushLayout(*impl_, canon)) return kNoTableId;
    impl_->layout_by_hash.emplace(hash, uint16_t(impl_->layouts.size() - 1));
    return uint16_t(impl_->layouts.size() - 1);
  }
  if (!PushLayout(*impl_, desc)) return kNoTableId;
  return uint16_t(impl_->layouts.size() - 1);
}

VkImageView Renderer::Impl::TargetView(ImageEntry& entry, uint32_t mip, uint32_t layer, uint32_t views) {
  const uint32_t key = views << 16 | mip << 8 | layer;
  for (const auto& view : entry.target_views) {
    if (view.first == key) return view.second;
  }
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = entry.image;
  info.viewType = views > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  info.format = entry.format;
  info.subresourceRange = {entry.aspect, mip, 1, layer, views};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(device.device(), &info, nullptr, &view);
  entry.target_views.push_back({key, view});
  return view;
}

VkImageView Renderer::Impl::SampledView(ImageEntry& entry, const TextureSlot& slot) {
  const bool plain = slot.view_format == Format::Undefined && !slot.swizzle[0] && !slot.swizzle[1] && !slot.swizzle[2] && !slot.swizzle[3];
  if (plain) return entry.sampled_view;
  const uint64_t key = uint64_t(slot.view_format) << 32 | uint64_t(slot.swizzle[0]) << 24 | uint64_t(slot.swizzle[1]) << 16 | uint64_t(slot.swizzle[2]) << 8 | slot.swizzle[3];
  for (const auto& view : entry.reinterpreted_views) {
    if (view.first == key) return view.second;
  }
  VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  info.image = entry.image;
  info.viewType = entry.desc.kind == ImageKind::Cube ? VK_IMAGE_VIEW_TYPE_CUBE : entry.desc.kind == ImageKind::Volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
  info.format = slot.view_format != Format::Undefined ? ToVk(slot.view_format, device.caps()) : entry.format;
  info.components = {VkComponentSwizzle(slot.swizzle[0]), VkComponentSwizzle(slot.swizzle[1]), VkComponentSwizzle(slot.swizzle[2]), VkComponentSwizzle(slot.swizzle[3])};
  info.subresourceRange = {VkImageAspectFlags(entry.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, entry.desc.mips, 0, entry.layers};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(device.device(), &info, nullptr, &view);
  entry.reinterpreted_views.push_back({key, view});
  return view;
}
