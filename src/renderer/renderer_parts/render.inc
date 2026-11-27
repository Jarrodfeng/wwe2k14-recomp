

void Renderer::Impl::Render(const Frame& frame, RenderMode mode) {
  frame_dlss = &frame.dlss;  // S10b
  upload_seal_now = frame.upload_seal;  // S102 R21-B: which sealed upload batches this frame applies (0 = all, as before)
  render_thread.store(std::hash<std::thread::id>()(std::this_thread::get_id()), std::memory_order_relaxed);  // S102 U1
  RenderTimes& rt = g_render_times;
  uint64_t t = RenderNow();
  const uint64_t render_t0 = t;  // S102 S0-3
  const RenderTimes rt0 = rt;    // S102 G5: this Render's own share of the cumulative timers (the 600-frame log resets rt at its end)
  out_collect_ns = out_ring_ns = out_end_ns = out_begin_ns = out_pipe_ns = out_pipe_n = 0;
  out_uploads = out_up_images = out_scaled = out_up_barriers = 0, out_up_bytes = 0, out_batched = false;
  out_up_by_producer[0] = out_up_by_producer[1] = out_up_by_producer[2] = 0;   // S103 PVR
  uint64_t fence_wait_ns = 0;
  auto lap = [&](uint64_t& acc) {
    const uint64_t now = RenderNow();
    acc += now - t;
    t = now;
  };
  const uint32_t slot_index = uint32_t(serial % slots.size());
  FrameSlot& slot = slots[slot_index];
  VkDevice dev = device.device();
  // S105 A9: after a real loss the slot's fence is reset every frame (below) but never submitted again, so it is never signalled: a really lost device returns from the wait at once, a
  // healthy one (the +dev test plant --wwe2k14_rdr_test_device_lost) would wait for ever. Once lost, wait for the device to go idle instead (at once on a lost device; on a healthy one it
  // lets the frames already in flight finish, so the retired resources below are not freed under them).
  // (review F1) the +dev test plant (--wwe2k14_rdr_test_device_lost=N) is set HERE, at the top, before the acquire / present steps: from frame serial N the renderer takes the REAL lost-device path on a
  // healthy device, and this frame's present step is skipped too (a present waiting on a semaphore no submit signals could block the queue for good).
  if (const int64_t plant_at = crumbs::g_test_lost_at.load(std::memory_order_relaxed); plant_at >= 0 && int64_t(serial) >= plant_at && !lost_serial)
    OnDeviceLost("test plant", int(VK_ERROR_DEVICE_LOST), false);
  const bool lost_for_good = lost_serial && !lost_simulated;
  if (lost_for_good) {
    if (!idle_after_loss) idle_after_loss = true, vkDeviceWaitIdle(dev);   // (review F3) once: later frames have nothing in flight
  } else if (rdr::g_fence_friendly.load(std::memory_order_relaxed)) {  // S102 H2: poll + sleep slices; a cap hit falls through to the blocking wait below
    VkFence fence = slot.fence;
    rdr::fence_wait::Wait([&] { return vkGetFenceStatus(dev, fence) != VK_NOT_READY; }, rdr::fence_wait::NowNs, rdr::fence_wait::SleepNs);
  }
  if (const VkResult waited = lost_for_good ? VK_SUCCESS : vkWaitForFences(dev, 1, &slot.fence, VK_TRUE, UINT64_MAX); waited != VK_SUCCESS) {
    if (waited == VK_ERROR_DEVICE_LOST) OnDeviceLost("vkWaitForFences", int(waited), false);  // S102 D1 (before the Problem: a RDR_DEV build stops on it, review F4)
    Problem("frame_fence_wait_failed", "VkResult %d (a lost device shows here first)", int(waited));  // S102 U1 (R9)
  }
  readback.OnSlotWaited(slot_index, serial);  // lane 232: this slot's older read-back copies are complete
  fence_wait_ns = RenderNow() - t;  // S102 S0-3
  lap(rt.wait);
  rt.retired_items += slot.retired.size();
  DrainRetired(slot);
  lap(rt.drain);
  ProcessDestroys(slot);
  if (rdr::g_lean_churn_every.load(std::memory_order_relaxed) | rdr::g_lean_audit_every.load(std::memory_order_relaxed)) LeanTool(slot, frame);   // S102 RT1 (tools only)
  lap(rt.destroys);
  vkResetCommandPool(dev, slot.pool, 0);
  vkResetDescriptorPool(dev, slot.descriptors, 0);
  if ((census_flush::Due(g_cf_lean) || serial % 600 == 0) && (rdr::g_render_lean.load(std::memory_order_relaxed) | rdr::g_lean_hooks.load(std::memory_order_relaxed))) LeanCensus();   // S102 RT1
  lap(rt.pools);
  {  // S102 G5: reading the finished frame's stamps is a driver call too
    const uint64_t collect_t0 = RenderNow();
    CollectGpuTime(slot);  // lane 269
    out_collect_ns = RenderNow() - collect_t0;
  }
  // S102 S0-3: the stamp switch is read ONCE here per frame (rdr::g_gpu_stamps is set from the per-frame switch table, rdr_sw::kGpuStamps, by the live sink). The pool is sized from the
  // pass count: a frame that wanted more grows it before this slot's next frame (its last result is read); a slot that has only the two-query pool grows when stamps first turn on.
  slot.ts_detail = false;
  if (slot.timestamps) {
    const bool want_stamps = rdr::g_gpu_stamps.load(std::memory_order_relaxed) || frame.group_min_run != 0;   // S102 R24-F: the group stamps need the per-step stamps (they chain between them)
    uint32_t cap = slot.ts_cap;
    if (want_stamps && cap < 192) cap = 192;
    if (slot.ts_need > cap) cap = (slot.ts_need + slot.ts_need / 4 + 63) & ~63u;
    if (options.gpu_stamp_plant == 9 && want_stamps) cap = 6;   // S103 GT test plant 9: a pool too small for the frame's steps: stamps are dropped, the bracket check must report it
    if (cap != slot.ts_cap) {
      vkDestroyQueryPool(dev, slot.timestamps, nullptr);
      slot.timestamps = VK_NULL_HANDLE;
      MakeStampPool(slot, cap);
      if (cap > 2) Log("[rdr][gpu] stamp pool %u queries (a frame wanted %u)", slot.ts_cap, slot.ts_need);
      slot.ts_need = 0;
    }
    slot.ts_detail = slot.timestamps && want_stamps && slot.ts_cap >= 4;
    if (frame.group_min_run && slot.timestamps && !slot.gs_pool) {   // S102 R24-F: the group pool, made once per slot (this slot's last frame is finished: its fence was waited)
      VkQueryPoolCreateInfo gq{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      gq.queryType = VK_QUERY_TYPE_TIMESTAMP, gq.queryCount = rdr::kGroupPoolCap;
      if (vkCreateQueryPool(dev, &gq, nullptr, &slot.gs_pool) != VK_SUCCESS) slot.gs_pool = VK_NULL_HANDLE;
    }
  }
  // S102 R24-F: only a frame whose every draw carries a group, with per-step stamps and a pool, is cut (else it is a plain stamped frame)
  slot.gs_frame = frame.group_min_run != 0 && frame.draw_groups.count == frame.draws.count && frame.draws.count != 0 && slot.ts_detail && slot.gs_pool != VK_NULL_HANDLE;
  slot.gs_count = 0, slot.gs_overflow = false, slot.gs_segs.clear(), slot.gs_passes.clear();
  if (store_on) {  // lane 276: a slice of the listed pipelines (menus / loading only), then the 30 s save
    const uint64_t store_t0 = RenderNow();
    if (store_slice_us && prebuild_allowed) PrebuildSlice(uint64_t(store_slice_us) * 1000ull, false);
    StoreMaybeSave();
    const uint64_t store_ns = RenderNow() - store_t0;
    rt.pipestore += store_ns;
    if (store_ns > 8000000ull) Log("[rdr][pipestore] slow frame: %.1f ms in the pipeline store this frame (slice or save)", double(store_ns) / 1e6);
    t = RenderNow();  // (not lapped into another field)
  }

  const VkDeviceSize ring_base = VkDeviceSize(slot_index) * kRingSection;
  const VkDeviceSize geometry_offset = AlignUp(frame.constants.count, 16);
  const VkDeviceSize prev_offset = AlignUp(geometry_offset + frame.geometry.count, kConstantAlign);  // S10b: the previous tick's constants
  prev_constants_base = ring_base + prev_offset;
  if (options.counts) counts_cur.Reset(uint32_t(serial));  // S102 M1: before the early return, so a dropped frame never reads as the frame before it
  if (prev_offset + frame.prev_constants.count + kConstantRange + kOverlayRange + kDlssConstants > kRingSection) {
    Problem("frame_ring_full", "constants %u, geometry %u bytes", frame.constants.count, frame.geometry.count);
    if (options.counts) counts_cur.dropped = 1;  // S102 M1
    frames_dropped.fetch_add(1, std::memory_order_relaxed);  // S103 CB1
    return;
  }
  ring_used_cur = uint64_t(frame.constants.count) + frame.geometry.count + frame.prev_constants.count;   // S103 PVR
  if (options.counts) counts_cur.ring_constants = frame.constants.count, counts_cur.ring_geometry = frame.geometry.count, counts_cur.ring_prev = frame.prev_constants.count;  // S102 M1
  const uint64_t ring_t0 = RenderNow();  // S102 G5
  if (frame.constants.count) std::memcpy(ring_memory.mapped + ring_base, frame.constants.data, frame.constants.count);
  if (frame.geometry.count) std::memcpy(ring_memory.mapped + ring_base + geometry_offset, frame.geometry.data, frame.geometry.count);
  if (frame.prev_constants.count) std::memcpy(ring_memory.mapped + prev_constants_base, frame.prev_constants.data, frame.prev_constants.count);
  out_ring_ns = RenderNow() - ring_t0;
  if (host_buffers.load(std::memory_order_relaxed)) {   // S103 PF2: the real reader record of the host-visible vertex copies: this Render reads every one its draws name (HostBufferIdle)
    for (uint32_t d = 0; d < frame.draws.count; ++d)
      for (int b = 0; b < 2; ++b) {
        const BufferId id = frame.draws[d].vertex[b].buffer;
        if (id == kNoBuffer || id == kFrameGeometry) continue;
        BufferEntry& e = buffers[id];
        if (e.host) std::atomic_ref<uint64_t>(e.last_read).store(serial + 1, std::memory_order_relaxed);
      }
  }
  if (frame.direct_record && serial + 1 > frame.direct_record + (kRendersPerRecord - 1)) {   // recorded at s = direct_record - 1, may be rendered at s .. s + kRendersPerRecord - 1
    direct_overruns.fetch_add(1, std::memory_order_relaxed);
    Problem("posed_direct_overrun", "a frame recorded at Render serial %llu is rendered at %llu (more than %u renders per record): the posed vertex copies it reads may be rewritten", static_cast<unsigned long long>(frame.direct_record - 1), static_cast<unsigned long long>(serial), unsigned(kRendersPerRecord));
  }

  frame_sets.assign(frame.texture_sets.count, VK_NULL_HANDLE);
  state_pipelines.assign(frame.states.count, {VK_NULL_HANDLE, 0});

  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  const uint64_t begin_t0 = RenderNow();  // S102 G5 (review F6): the begin + query reset + first stamps block, timed directly
  vkBeginCommandBuffer(slot.cmd, &begin);
  // S102 D1 round 6: the frame-top barrier (breadcrumbs.h). Before anything else in the buffer: every command submitted earlier on the queue (frame N-1, N-2, the
  // uploads) completes and its writes are visible before any command of this frame runs. Read once a frame; off = nothing recorded.
  frame_barrier_mode = crumbs::g_frame_top_barrier.load(std::memory_order_relaxed);
  if (frame_barrier_mode > 0) {
    if (crumbs::g_frame_barrier_plant.load(std::memory_order_relaxed) != 1) RecordFrameBarrier(slot.cmd, "top");
    const uint64_t n = crumbs::g_frame_barrier_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (census_flush::Due(g_cf_barrier) || n % 600 == 0)
      Log("[rdr][frame_top_barrier] mode %d, frames %llu, barriers %llu (one at the top of every frame%s)", frame_barrier_mode, static_cast<unsigned long long>(n),
          static_cast<unsigned long long>(crumbs::g_frame_barrier_count.load(std::memory_order_relaxed)), frame_barrier_mode >= 2 ? ", one before the present" : "");
  }
  // S102 S0-3. Detail (decided above): [0] top of the buffer, [1] at the colour-output stage, then a stamp after each run of same-class steps and a tail. [1] - [0] is the START GAP: how long
  // the GPU took to get from the top of this buffer to its first colour-output work (earlier submits still running, WDDM holding the batch, a semaphore wait). It is NOT a measure of the
  // display wait (see docs\renderer\GPU_STAMPS.md). Without detail only lane 269's two stamps are written.
  slot.ts_count = 0, slot.ts_need = 0, slot.ts_has_pass = false, slot.ts_presented = false;
  slot.arm_tag = rdr::g_arm_tag.load(std::memory_order_relaxed);  // S102 G10
  slot.ts_class.clear(), slot.ts_image.clear(), slot.ts_kind.clear(), slot.ts_src.clear(), slot.ts_pass.clear();
  slot.ts_serial = serial;  // S102 G11
  slot.ts_trace = rdr::g_frame_trace.load(std::memory_order_relaxed) != nullptr;
  acquire_ns_frame = 0;
  if (rdr::g_frame_trace.load(std::memory_order_relaxed)) {  // S102 G11: the occlusion pool of the frame trace (made once per slot, reset here)
    if (!slot.occ) {
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qi.queryType = VK_QUERY_TYPE_OCCLUSION, qi.queryCount = 512;
      vkCreateQueryPool(dev, &qi, nullptr, &slot.occ);
    }
    if (slot.occ) vkCmdResetQueryPool(slot.cmd, slot.occ, 0, 512);
    slot.occ_count = 0, slot.occ_meta.clear(), slot.occ_serial = serial, slot.occ_pending = slot.occ != VK_NULL_HANDLE;
  }
  if (slot.timestamps) {  // S101 lane22
    vkCmdResetQueryPool(slot.cmd, slot.timestamps, 0, slot.ts_detail ? slot.ts_cap : 2);  // (only what the frame writes)
    if (slot.gs_frame) vkCmdResetQueryPool(slot.cmd, slot.gs_pool, 0, rdr::kGroupPoolCap);   // S102 R24-F
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, slot.timestamps, 0);
    slot.ts_count = 1;
    if (slot.ts_detail) {
      vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, slot.timestamps, 1);
      slot.ts_count = 2;
    }
  }
  {  // S102 D1: this frame's crumb (the switches read once a frame) and its first marker words: [0] the frame began, [1] the uploads
    crumb = nullptr, crumb_step = nullptr, marker_next = 0;
    plant5_done = false;  // S102 D1 plant 5: once a frame
    const int marker_mode = crumbs::g_marker.load(std::memory_order_relaxed);
    const bool marking = marker_mode > 0 && EnsureMarker();
    frame_marker = marking ? marker_mode : 0;
    if (crumbs::g_on.load(std::memory_order_relaxed)) {
      crumb = &crumb_ring.Begin(serial, slot_index, uint8_t(mode), uint8_t(marking ? marker_mode : 0));
      crumb->destroys = last_destroys;
    }
    last_destroys = 0;
    marker_next = 1, marker_open = 0;
    if (marking) {  // word 0 (the frame began), then marker 1 (the uploads)
      const uint32_t value = crumbs::MarkValue(serial);
      vkCmdUpdateBuffer(slot.cmd, marker_buffer, VkDeviceSize(slot_index) * crumbs::kMarkerWords * 4, 4, &value);
      MarkBegin(slot.cmd, slot_index);
    }
  }
  ps_count = 0, ps_names.clear(), ps_meta.clear(), ps_afoff.clear();  // S102 G10
  if (rdr::g_draw_census_stats.load(std::memory_order_relaxed) && rdr::g_draw_census.load(std::memory_order_relaxed) && options.instrument_stats) {
    if (!ps_pool) {
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
      qi.queryCount = 4096;
      qi.pipelineStatistics = VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT | VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
      if (vkCreateQueryPool(dev, &qi, nullptr, &ps_pool) == VK_SUCCESS) ps_cap = 4096;
      else ps_pool = VK_NULL_HANDLE, Log("[rdr][census] no pipeline-statistics query pool on this device");
    }
    if (ps_pool) vkCmdResetQueryPool(slot.cmd, ps_pool, 0, ps_cap);
  }
  out_begin_ns = RenderNow() - begin_t0;
  last_slot = slot_index;
  lap(rt.retire);
  // S102 G5 (review F4): with the meter on a stamp goes JUST BEFORE ApplyUploads and one just after, so the uploads' span is the commands between them (not measured from [1], the colour-output stamp,
  // which would also hold anything the GPU does between the top of the buffer and the first upload). Both are class `other`: without the meter the uploads span falls in the FIRST RUN of steps (its class); with it, the span is split out into `other` (the roles then differ by that amount).
  slot.ts_up = 0, slot.ts_up_mid = 0;
  slot.ts_mark = rdr::g_gpu_mark_seq.load(std::memory_order_acquire);
  slot.ts_steps_rec = slot.ts_steps_closed = slot.ts_w_pre = slot.ts_w_closes = slot.ts_w_post = 0, slot.ts_census = false;   // S103 GT
  // S103 GT: the pair of stamps around ApplyUploads (their GPU span: the upload line of [rdr][gpu][uploads]) is also written in the stamp-per-step mode (--wwe2k14_rdr_gpu_pass_stamps), not only with the outside meter (which waits on fences)
  // S103 GT review: written whenever the frame is stamped (ts_detail), not only with the meter / pass stamps: the Surface bench sets gpu_stamps alone, and without the pair the uploads' GPU time was billed to the frame's first role.
  // A deliberate baseline change for every stamped run (both builds). The pair has its own kind (6, "uploads") so the tail row (kind 3, #0) keeps one meaning.
  const bool meter_stamps = slot.ts_detail;
  if (meter_stamps && slot.ts_count + 2 < slot.ts_cap) {
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, slot.ts_count++);
    ++slot.ts_w_pre;
    slot.ts_class.push_back(kGpuOther);
    slot.ts_image.push_back(0), slot.ts_kind.push_back(6), slot.ts_src.push_back(255);  // S102 G11 (item 11): G5's meter stamps keep the per-stamp vectors the same length as ts_class (the census and the trace read them together)
    if (slot.ts_trace) slot.ts_pass.push_back(0xFFFFFFFFu);
    ++slot.ts_need;
  }
  ApplyUploads(slot.cmd, slot);
  if (crumb) crumb->uploads = out_uploads;  // S102 D1
  if (meter_stamps && out_uploads && slot.ts_count + 1 < slot.ts_cap && slot.ts_count > 2) {  // S102 G5: the uploads' own span on the GPU
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, slot.ts_count);
    slot.ts_up = slot.ts_count++;
    ++slot.ts_w_post;
    slot.ts_class.push_back(kGpuOther);
    slot.ts_image.push_back(0), slot.ts_kind.push_back(6), slot.ts_src.push_back(255);  // S102 G11 (item 11): G5's meter stamps keep the per-stamp vectors the same length as ts_class (the census and the trace read them together)
    if (slot.ts_trace) slot.ts_pass.push_back(0xFFFFFFFFu);
    ++slot.ts_need;
  }
  lap(rt.uploads);

  Compile(frame);
  slot.sd_arm = sd_on;   // S102 R24-C review F4
  lap(rt.compile);
  rt.passes += passes.size();
  rt.steps += steps.size();
  if (options.counts) counts_cur.steps = uint32_t(steps.size());  // S102 M1
  // Lane 232: the read-back requests recorded into THIS frame. (The frame_ring_full early return above happens before this: a request queued for such a frame stays pending and lands in a later frame.)
  readback.TakePending(readback_taking);
  readback_recorded.assign(readback_taking.size(), 0);
  out_cap_h_ = frame.out_cap_h, out_bilinear_ = frame.out_bilinear;  // S102 G2: this frame's output levers, decided at record time
  present_fetch_ = frame.present_fetch;  // S102 R24-A
  if (frame.arena_overdraw) {  // S102 C4: the live census line (a Surface "no gain" is told apart from "took 0 frames"): every 600 frames while the switch is on
    const uint64_t n = ++colour_cuts::g_overdraw_frames;
    if (census_flush::Due(g_cf_overdraw) || n % 600 == 0)
      Log("[rdr][arena_overdraw] frames %llu, colour passes taken %llu, declined %llu (depth clear %llu, nothing eligible %llu, mixed direction %llu, stencil %llu), draws pre-drawn %llu, texture-alpha proof admitted %llu refused %llu, texture scans %llu images %.2f MB %.2f ms",
          static_cast<unsigned long long>(n), static_cast<unsigned long long>(colour_cuts::g_frames_taken.load()),
          static_cast<unsigned long long>(colour_cuts::g_declined_clear.load() + colour_cuts::g_declined_none.load() + colour_cuts::g_declined_direction.load() + colour_cuts::g_declined_stencil.load()),
          static_cast<unsigned long long>(colour_cuts::g_declined_clear.load()), static_cast<unsigned long long>(colour_cuts::g_declined_none.load()), static_cast<unsigned long long>(colour_cuts::g_declined_direction.load()),
          static_cast<unsigned long long>(colour_cuts::g_declined_stencil.load()), static_cast<unsigned long long>(colour_cuts::g_draws_prepassed.load()),
          static_cast<unsigned long long>(colour_cuts::g_overdraw_texture_taken.load()), static_cast<unsigned long long>(colour_cuts::g_overdraw_texture_refused.load()),
          static_cast<unsigned long long>(colour_cuts::g_scan_images.load()), double(colour_cuts::g_scan_bytes.load()) / 1048576.0, double(colour_cuts::g_scan_ns.load()) / 1e6);
  }
  arena_fragments_ = frame.arena_fragments;  // S102 R18-B3
  if (arena_fragments_) {  // the live census line (a Surface "no gain" is told apart from "took 0 frames"): every 600 frames while a bit is on
    const uint64_t n = ++arena_fragments::g_frames;
    if (census_flush::Due(g_cf_frag) || n % 600 == 0)
      Log("[rdr][arena_fragments] mask %u, frames %llu, colour passes taken %llu, draws blend-off %llu (%.1f a frame), draws widened %llu, declined %llu states (by reason: not a plain alpha blend %llu, other target blended %llu, float target %llu), exact proofs %llu inexact %llu",
          arena_fragments_, static_cast<unsigned long long>(n), static_cast<unsigned long long>(arena_fragments::g_passes_taken.load()), static_cast<unsigned long long>(arena_fragments::g_blend_off.load()),
          double(arena_fragments::g_blend_off.load()) / double(n),
          static_cast<unsigned long long>(arena_fragments::g_widened.load()), static_cast<unsigned long long>(arena_fragments::g_declined[0].load() + arena_fragments::g_declined[1].load() + arena_fragments::g_declined[2].load()),
          static_cast<unsigned long long>(arena_fragments::g_declined[0].load()), static_cast<unsigned long long>(arena_fragments::g_declined[1].load()), static_cast<unsigned long long>(arena_fragments::g_declined[2].load()),
          static_cast<unsigned long long>(arena_fragments::g_proof_exact.load()), static_cast<unsigned long long>(arena_fragments::g_proof_inexact.load()));
  }
  colour_cuts_ = frame.colour_cuts | (frame.arena_overdraw ? colour_cuts::kPrepass : 0u);  // S102 G10; S102 C4: the arena overdraw switch runs the same prepass planner (its rule adds the texture-alpha proof in the binder)
  bool presenting = false;
  uint32_t image_index = 0;
  // S102 S0-3: one stamp per RUN of steps of the same class (a stamp drains the pipe, so a stamp after every copy would cost real time): a run is closed (stamped) when the
  // next step has another class, after the first step when a plant is on, and after the last step. Its span goes to the run's class.
  bool run_open = false, planted = false;
  // S102 D1: the step loop's breadcrumb state; rplay --crumbs-selftest stalls this frame's GPU at a step (breadcrumbs.h)
  const int crumb_plant = crumbs::g_plant.load(std::memory_order_relaxed);
  const bool selftest_here = int64_t(serial) == crumbs::g_selftest_frame.load(std::memory_order_relaxed) && frame_marker > 0;
  const int64_t selftest_step = crumbs::g_selftest_step.load(std::memory_order_relaxed);
  if (selftest_here && !selftest_event) {
    VkEventCreateInfo ei{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    vkCreateEvent(dev, &ei, nullptr, &selftest_event);
  }
  bool selftest_stalled = false, crumb_prev_copy = false;
  uint8_t crumb_prev_class = 0xFF;
  uint32_t selftest_at_step = 0;
  uint16_t selftest_marker = crumbs::kNoMarker;
  uint8_t run_class = kGpuOther;
  const bool census = slot.ts_detail && (rdr::g_gpu_census.load(std::memory_order_relaxed) || rdr::g_gpu_pass_stamps.load(std::memory_order_relaxed) || slot.gs_frame);  // S102 G6; S102 R24-B: the live per-pass stamps take the census's stamp-per-step mode; S102 R24-F: so do the group stamps
  uint32_t run_image = 0;
  uint8_t run_kind = 3, run_src = 255;
  uint32_t run_pass = 0xFFFFFFFFu;  // S102 G11
  auto close_run = [&](uint8_t cls, bool plant_stamp = false) -> bool {   // S103 GT review: true when the stamp was written (false = the pool overflowed and the stamp was dropped)
    ++slot.ts_need;
    if (slot.ts_count + 1 < slot.ts_cap) {  // (the last query is the tail's)
      vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, slot.ts_count++);
      ++slot.ts_w_closes;
      slot.ts_class.push_back(cls);
      slot.ts_image.push_back(plant_stamp ? 0 : run_image), slot.ts_kind.push_back(plant_stamp ? 4 : run_kind), slot.ts_src.push_back(plant_stamp ? 255 : run_src);
      if (slot.ts_trace) slot.ts_pass.push_back(plant_stamp ? 0xFFFFFFFFu : run_pass);  // S102 G11 (F9): only while the frame trace is open  // S102 G6 (kind 4 = a test plant's stamp)
    } else {
      ++role_window.overflow, ++role_run.overflow;  // (the pool grows before this slot's next frame)
      return false;
    }
    return true;
  };
  // S103 GT: a stamp that closes a STEP (the stamp-per-step mode counts them against the steps recorded: every pass bracketed = equal). Test plant 8 leaves the 2nd closing stamp of each frame out: that step's
  // time falls into the next step's span, the chain still adds up, and only the count (steps closed < steps recorded) shows it.
  slot.ts_census = census;
  uint32_t plant8_closes = 0;
  auto close_step = [&](uint8_t cls) {
    if (options.gpu_stamp_plant == 8 && census && ++plant8_closes == 2) return;
    if (close_run(cls) && census) ++slot.ts_steps_closed;   // counted only when the stamp was really written
  };
  auto class_of = [&](ImageId image) -> uint8_t { return image != kNoImage && image < images.high_water() ? std::atomic_ref<uint8_t>(images[image].gpu_class).load(std::memory_order_relaxed) : uint8_t(kGpuOther); };  // S102 R21-B (hazard 3)
  const bool copy_batch_on = rdr::g_copy_batch.load(std::memory_order_relaxed);  // S102 G12: read every frame
  for (size_t step_i = 0; step_i < steps.size(); ++step_i) {
    const Step& step = steps[step_i];
    if (step.kind != StepKind::Present) {
      if (mode == RenderMode::PersistentOnly && !step.persistent) continue;
      if (mode == RenderMode::DisplayOnly && step.persistent) continue;
      if (mode == RenderMode::NoPasses) continue;  // S102 lane KO (ko_draw_none): no pass, no copy; the present below still runs
    }
    // The class of this step (a pass: its first colour target, else its depth target; a copy: its destination; the present: output) and whether it records at all.
    uint8_t step_class = kGpuOther;
    bool records = true;
    if (step.kind == StepKind::Pass) {
      const PassPlan& plan = passes[step.index];
      step_class = class_of(plan.targets ? (plan.targets->color_count ? plan.targets->color[0].image : plan.targets->depth.image) : kNoImage);
    } else if (step.kind == StepKind::Copy) {
      step_class = class_of(frame.copies[step.index].dest.image);
    } else if (step.kind == StepKind::Dispatch) {   // S102 L1b: the first cube's class (the binder names the light cubes pre-light)
      step_class = class_of(frame.dispatches[step.index].cube[0]);
    } else {
      step_class = kGpuOutput;
      records = (windowed || offscreen_out) && !presenting && mode != RenderMode::PersistentOnly;
      if (lost_serial && !lost_simulated) records = false;  // S102 D1 (review F5): no acquire / present on a lost device (the acquire blocked 0.5-1.9 s on the Surface)
    }
    if (!records) continue;
    if (slot.ts_detail) ++slot.ts_steps_rec;   // S103 GT: the bracket check counts every recorded step
    if (slot.ts_detail) {
      uint32_t step_image = 0;  // S102 G6 (review: only while stamping): what the census names the step by
      uint8_t step_src = 255;
      if (step.kind == StepKind::Pass) {
        const PassPlan& tp = passes[step.index];
        step_image = tp.targets ? (tp.targets->color_count ? tp.targets->color[0].image : tp.targets->depth.image) : 0;
      } else if (step.kind == StepKind::Copy) {
        step_image = frame.copies[step.index].dest.image;
        step_src = class_of(frame.copies[step.index].source.image);
      } else if (step.kind == StepKind::Dispatch) {
        step_image = frame.dispatches[step.index].cube[0];   // S102 L1b
      }
      const uint8_t step_kind = step.kind == StepKind::Pass ? 0 : step.kind == StepKind::Copy ? 1 : step.kind == StepKind::Dispatch ? 5 : 2;   // (5 = a dispatch, S102 L1b)
      if (run_open && (step_class != run_class || census)) close_step(run_class);  // S102 G6: the census closes a run after every step
      run_class = step_class, run_open = true, run_image = step_image, run_kind = step_kind, run_src = step_src;
      run_pass = step.kind == StepKind::Pass ? step.index : 0xFFFFFFFFu;  // S102 G11
    }
    // S102 D1: the step's crumb and its marker word (breadcrumbs.h). Mode 1 marks the first step of each run of one GPU class and the present; mode 2 every
    // pass / dispatch / present and the first copy of a run of copies; mode 3 every step.
    crumbs::Step* cs = crumb && !(crumb_plant == 2 && step_i == 3) ? crumbs::Ring::Add(*crumb) : nullptr;  // (plant 2: the 4th step's crumb is left out)
    uint16_t step_marker = crumbs::kNoMarker;
    if (frame_marker >= 3 || (frame_marker == 2 && !(step.kind == StepKind::Copy && crumb_prev_copy)) ||
        (frame_marker == 1 && (step_class != crumb_prev_class || step.kind == StepKind::Present))) {
      step_marker = MarkBegin(slot.cmd, slot_index);
    }
    crumb_prev_copy = step.kind == StepKind::Copy, crumb_prev_class = step_class;
    if (cs) {
      cs->kind = step.kind == StepKind::Pass ? crumbs::kPass : step.kind == StepKind::Copy ? crumbs::kCopy : step.kind == StepKind::Dispatch ? crumbs::kDispatch : crumbs::kPresent;
      cs->cls = step_class, cs->step = uint32_t(step_i), cs->index = step.index, cs->marker = step_marker;
      if (step.kind == StepKind::Pass) {
        const PassPlan& cp = passes[step.index];
        cs->image = cp.targets ? (cp.targets->color_count ? cp.targets->color[0].image : cp.targets->depth.image) : 0;
      } else if (step.kind == StepKind::Copy) {
        cs->image = frame.copies[step.index].dest.image;
      } else if (step.kind == StepKind::Dispatch) {
        cs->image = frame.dispatches[step.index].cube[0];
      }
    }
    if (selftest_here && !selftest_stalled && int64_t(step_i) >= selftest_step && selftest_event && step_marker != crumbs::kNoMarker) {  // rplay --crumbs-selftest: the GPU stops here until the host sets the event
      // TEST ONLY, never in a player's frame: a host-set event waited on by a pending command buffer is invalid usage under today's spec (VUID-vkSetEvent-event-09543,
      // VUID-vkCmdWaitEvents-srcStageMask-01158), so the selftest arms run without validation. No flush of its own (review F1): it records the production marker stream.
      vkCmdWaitEvents(slot.cmd, 1, &selftest_event, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, nullptr, 0, nullptr, 0, nullptr);
      selftest_stalled = true, selftest_at_step = uint32_t(step_i), selftest_marker = step_marker;
      if (crumb_plant == 1 && cs) cs->marker = uint16_t(step_marker + 1);  // plant 1: the crumb names the wrong marker word (the decode must miss the step)
    }
    if (std::FILE* ra = crumbs::g_role_audit.load(std::memory_order_relaxed)) {  // S102 D1 offline instrument (rplay --role-audit): the step's images and their state before it
      const auto img = [&](ImageId id) {
        if (id == kNoImage || id >= images.high_water()) { std::fprintf(ra, "[0,0,0,0]"); return; }
        const ImageEntry& e = images[id];
        bool undef = !e.states.empty();
        for (const SubresourceState& st : e.states) undef &= st.layout == VK_IMAGE_LAYOUT_UNDEFINED;
        std::fprintf(ra, "[%u,%u,%u,%d]", unsigned(id), e.width, e.height, undef ? 1 : 0);
      };
      std::fprintf(ra, "{\"f\":%llu,\"s\":%u,\"m\":%d,", static_cast<unsigned long long>(serial), unsigned(step_i), int(mode));
      if (step.kind == StepKind::Pass) {
        const PassPlan& ap = passes[step.index];
        std::fprintf(ra, "\"k\":\"pass\",\"i\":%u,\"t\":[", step.index);
        for (uint32_t c = 0; ap.targets && c < ap.targets->color_count; ++c) { if (c) std::fputc(',', ra); img(ap.targets->color[c].image); }
        std::fprintf(ra, "],\"d\":");
        img(ap.targets ? ap.targets->depth.image : kNoImage);
        std::fprintf(ra, ",\"sl\":%d,\"ss\":%d,\"st\":%d,\"sc\":%d", int(ap.stencil_load), int(ap.stencil_store), ap.stencil_tests ? 1 : 0, ap.strip_stencil_clear ? 1 : 0);   // S102 R24-C: stencil load / store ops, a draw tests stencil, the in-pass clears are stripped
        std::fprintf(ra, ",\"load\":%d,\"r\":[", ap.targets && ap.targets->color_count ? int(ap.load[0]) : -1);
        for (uint32_t q = 0; q < ap.sampled_count; ++q) { if (q) std::fputc(',', ra); img(sampled[ap.first_sampled + q].image); }
        std::fprintf(ra, "]}\n");
      } else if (step.kind == StepKind::Copy) {
        const CopyOp& ac = frame.copies[step.index];
        std::fprintf(ra, "\"k\":\"copy\",\"i\":%u,\"src\":", step.index);
        img(ac.source.image);
        std::fprintf(ra, ",\"dst\":");
        img(ac.dest.image);
        std::fprintf(ra, ",\"asp\":%d,\"rect\":[%d,%d,%u,%u]}\n", std::find(depth_only_copies.begin(), depth_only_copies.end(), &ac) != depth_only_copies.end() ? 1 : 3, int(ac.dest_rect.x), int(ac.dest_rect.y), unsigned(ac.dest_rect.width), unsigned(ac.dest_rect.height));   // S102 R24-C: asp 1 = depth only, 3 = depth + stencil (colour copies say 3 too)
      } else if (step.kind == StepKind::Dispatch) {
        std::fprintf(ra, "\"k\":\"dispatch\",\"i\":%u,\"t\":[", step.index);
        img(frame.dispatches[step.index].cube[0]);
        std::fprintf(ra, "]}\n");
      } else {
        std::fprintf(ra, "\"k\":\"present\",\"i\":%u,\"src\":", step.index);
        img(frame.presents[step.index].source.image);
        std::fprintf(ra, "}\n");
      }
    }
    crumb_step = cs && step.kind == StepKind::Pass ? cs : nullptr;
    if (step.kind == StepKind::Pass) {
      RecordPass(slot.cmd, frame, passes[step.index], slot, ring_base);
      crumb_step = nullptr;  // S102 D1
      slot.ts_has_pass = true;
      if (slot.ts_detail) ++role_run.cls_passes[step_class], ++role_window.cls_passes[step_class];
    } else if (step.kind == StepKind::Copy) {
      const CopyOp& copy = frame.copies[step.index];
      // S102 G12 (copy_batch.h): adjacent plain copies of one source into distinct subresources of one destination go as one command with one barrier flush. Not while a read-back is
      // being taken (it reads after each copy), and a step this mode would not record ends the batch.
      uint32_t taken = 1;
      if (copy_batch_on && readback_taking.empty()) {
        CopyGeom geoms[rdr::kMaxBatch];
        rdr::BatchKey keys[rdr::kMaxBatch];
        const CopyOp* ops[rdr::kMaxBatch];
        const auto key_of = [](const CopyOp& c, const CopyGeom& g) {
          rdr::BatchKey k;
          k.source_image = c.source.image, k.source_mip = c.source.mip, k.source_layer = c.source.layer;
          k.dest_image = c.dest.image, k.dest_sub = g.dest_sub;
          k.plain = g.path == 1 && c.source.image != c.dest.image;
          return k;
        };
        rdr::StepView views[rdr::kMaxBatch];
        uint32_t cand = 0;
        for (size_t j = step_i; j < steps.size() && cand < rdr::kMaxBatch; ++j) {   // the candidates: this copy and the steps after it (rdr::BatchSpan decides where the batch ends)
          const Step& cs = steps[j];
          views[cand].is_copy = cs.kind == StepKind::Copy;
          views[cand].records = !((mode == RenderMode::PersistentOnly && !cs.persistent) || (mode == RenderMode::DisplayOnly && cs.persistent));
          if (views[cand].is_copy) {
            const CopyOp& cc = frame.copies[cs.index];
            geoms[cand] = GeometryOf(cc), keys[cand] = key_of(cc, geoms[cand]), ops[cand] = &cc;
            views[cand].key = keys[cand];
          }
          ++cand;
          if (!views[cand - 1].is_copy) break;
        }
        const uint32_t n = rdr::BatchSpan(views, cand);
        if (n >= 2) RecordCopyBatch(slot.cmd, ops, geoms, n), taken = n;
      }
      if (cs && taken >= 2) cs->kind = crumbs::kCopyBatch, cs->draws = taken;  // S102 D1: one crumb for the batch
      if (taken < 2) {
        RecordCopy(slot.cmd, copy);
        if (!readback_taking.empty()) RecordReadbacksAfterCopy(slot.cmd, copy);
      }
      step_i += taken - 1;
    } else if (step.kind == StepKind::Dispatch) {   // S102 L1b
      RecordLightCubes(slot.cmd, frame.dispatches[step.index], slot_index, ring_base);
    } else {
      if (frame_barrier_mode >= 2 && crumbs::g_frame_barrier_plant.load(std::memory_order_relaxed) != 1) RecordFrameBarrier(slot.cmd, "present"), frame_barrier_end_done = true;  // S102 D1 round 6 (mode 2)
      presenting = RecordPresent(slot.cmd, frame.presents[step.index], slot, ring_base, image_index);
      slot.ts_presented = presenting;
      if (presenting && present_capped_) {  // S102 G2 (b), review F5: a stamp before and after the blit, so its ms shows apart (it is part of 'output' too)
        if (slot.ts_detail) close_step(kGpuOutput), run_open = false;
        swapchain.RecordCapBlit(slot.cmd, image_index);
        if (slot.ts_detail) close_run(kBlitTag);
      }
    }
    if (slot.ts_detail && (options.gpu_stamp_plant == 3 || options.gpu_stamp_plant == 4) && !planted) {  // test plants, once a frame after the first step: 3 an empty pass (plane role), 4 a slow one (rim role)
      planted = true;
      close_step(run_class);   // (a real step's closing stamp: counted as one)
      run_open = false;
      if (options.gpu_stamp_plant == 4) RecordStampPlant(slot.cmd);
      close_run(options.gpu_stamp_plant == 4 ? kGpuRim : kGpuPlane, true);  // (labelled as a plant in the census)
    }
  }
  if (slot.ts_detail && run_open) close_step(run_class);
  if (options.gpu_stamp_plant == 10 && slot.ts_detail && slot.ts_count + 1 < slot.ts_cap) {   // S103 GT test plant 10: a stamp written outside every counted site: the stamp-count identity must break
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, slot.ts_count++);
    slot.ts_class.push_back(kGpuOther), slot.ts_image.push_back(0), slot.ts_kind.push_back(4), slot.ts_src.push_back(255);
    if (slot.ts_trace) slot.ts_pass.push_back(0xFFFFFFFFu);
    ++slot.ts_need;
  }
  RecordReadbacks(slot.cmd);  // lane 232: after every step, so the copy sees this frame's result
  if (options.gpu_stamp_plant == 5 && slot.timestamps) RecordStampPlant(slot.cmd);  // S102 S0-3 plant 5: a slow copy loop at the END of the frame: the NEXT frame's start gap must rise with no window
  if (slot.timestamps) {  // S101 lane22, lane 269; S102 S0-3: the tail stamp is the last query ([1] when not detailed); a render with no pass is no sample
    const uint32_t tail = slot.ts_detail ? slot.ts_count : 1;
    vkCmdWriteTimestamp(slot.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, slot.timestamps, tail);
    if (slot.ts_detail) {
      slot.ts_class.push_back(kGpuOther);
      slot.ts_image.push_back(0), slot.ts_kind.push_back(3), slot.ts_src.push_back(255);  // S102 G6
      if (slot.ts_trace) slot.ts_pass.push_back(0xFFFFFFFFu);  // S102 G11 (F9)
      ++slot.ts_need;
    }
    if (slot.ts_detail) {   // S103 GT review: the stamps written must be exactly [0] + [1] + the upload pair + every closing stamp + the tail (a stamp written outside the counted sites breaks it); no GPU involved
      const bool ok = tail == 2 + slot.ts_w_pre + slot.ts_w_closes + slot.ts_w_post;
      for (RoleSums* sums : {&role_window, &role_run}) {
        ++sums->ident_frames, sums->ident_bad += ok ? 0 : 1;
        if (slot.ts_census) sums->steps_rec += slot.ts_steps_rec, sums->steps_closed += slot.ts_steps_closed, ++sums->census_frames;
      }
    }
    slot.ts_count = tail + 1;
    slot.ts_need += 2;  // [0] and [1]
    slot.ts_pending = true;
    slot.ts_submit_ns = RenderNow();
  }
  if (frame_barrier_mode >= 2 && !frame_barrier_end_done && crumbs::g_frame_barrier_plant.load(std::memory_order_relaxed) != 1) RecordFrameBarrier(slot.cmd, "end");  // S102 D1 round 6: no present this frame
  frame_barrier_end_done = false;
  if (frame_marker) {  // S102 D1: the frame's end marker (after the read-backs): the front end got through the whole buffer
    const uint16_t end_marker = MarkBegin(slot.cmd, slot_index);  // (closes the last step's marker)
    MarkClose(slot.cmd, slot_index);
    if (crumb) crumb->end_marker = end_marker;
  }
  const uint64_t end_t0 = RenderNow();  // S102 G5
  if (const VkResult ended = vkEndCommandBuffer(slot.cmd); ended != VK_SUCCESS) Problem("frame_record_failed", "vkEndCommandBuffer VkResult %d", int(ended));  // S102 U1 (R9)
  out_end_ns = RenderNow() - end_t0;
  lap(rt.record);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &slot.cmd;
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | (present_capped_ ? VK_PIPELINE_STAGE_TRANSFER_BIT : 0);  // S102 G2: the cap's blit writes the swapchain image at the transfer stage
  VkSemaphore finished = VK_NULL_HANDLE;
  if (presenting && !swapchain.offscreen()) {  // S102 G2: an offscreen output has no display to wait for or to signal
    finished = swapchain.finished_semaphore(image_index);
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &slot.available;
    submit.pWaitDstStageMask = &wait_stage;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &finished;
  }
  vkResetFences(dev, 1, &slot.fence);
  const bool meter_outside = rdr::g_gpu_outside.load(std::memory_order_relaxed) && slot.timestamps;  // S102 G5: read once a frame
  const uint64_t meter_t0 = meter_outside ? RenderNow() : 0;
  uint64_t meter_wait_ns = 0;  // S102 G5 (review F3): the instrument's own fence wait: idle time, like the slot's fence wait and the acquire
  if (options.gpu_stamp_plant == 6) {  // S102 G5 plant 6: GPU work OUTSIDE the stamped buffer: an extra submit of its own small buffer before the frame's (its copy loop is on the same queue)
    if (!slot.plant_cmd) {
      VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      alloc.commandPool = slot.pool, alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, alloc.commandBufferCount = 1;
      vkAllocateCommandBuffers(dev, &alloc, &slot.plant_cmd);
    }
    VkCommandBufferBeginInfo plant_begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    plant_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(slot.plant_cmd, &plant_begin);
    RecordStampPlant(slot.plant_cmd);
    vkEndCommandBuffer(slot.plant_cmd);
    VkSubmitInfo plant_submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    plant_submit.commandBufferCount = 1, plant_submit.pCommandBuffers = &slot.plant_cmd;
    vkQueueSubmit(device.queue(), 1, &plant_submit, VK_NULL_HANDLE);
  }
  // S102 D1 (review F5): once the device is really lost nothing more is submitted (the fence stays unsignalled: a lost device's waits return at once).
  if (lost_serial && !lost_simulated) {
    if (crumb) crumb->submitted = false;
  } else if (const VkResult submitted = vkQueueSubmit(device.queue(), 1, &submit, slot.fence); submitted != VK_SUCCESS) {
    if (submitted == VK_ERROR_DEVICE_LOST) OnDeviceLost("vkQueueSubmit", int(submitted), false);  // S102 D1 (before the Problem, review F4)
    Problem("frame_submit_failed", "VkResult %d", int(submitted));  // S102 U1 (R9)
    frames_dropped.fetch_add(1, std::memory_order_relaxed);  // S103 CB1
  } else if (crumb) {
    crumb->submitted = true;
  }
  if (selftest_stalled) {  // S102 D1 (rplay --crumbs-selftest): wait until the marker words say the GPU reached the stalled step, run the lost-device handler as if lost, release the GPU
    const uint32_t* words = reinterpret_cast<const uint32_t*>(marker_memory.mapped) + size_t(slot_index) * crumbs::kMarkerWords;
    const uint32_t want = crumbs::MarkValue(serial);
    // render-clock-begin: dev - the stall self-test waits on a marker, never a picture value
    const auto t0 = std::chrono::steady_clock::now();  // the marker before the stalled one has its END written once everything before the stall finished
    while (selftest_marker >= 2 && selftest_marker <= crumbs::kMaxMarker && words[crumbs::EndWord(selftest_marker - 1u)] != want && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3))
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    // render-clock-end
    std::this_thread::sleep_for(std::chrono::milliseconds(30));  // the front end is at the wait: nothing after it may be written
    if (!g_problem_nonfatal.load(std::memory_order_relaxed)) vkSetEvent(dev, selftest_event);  // --crumbs-fatal: the process stops at the Problem: release the GPU first (never leave it waiting)
    OnDeviceLost("selftest", int(VK_ERROR_DEVICE_LOST), true);
    const crumbs::SlotReach reach = crumbs::Reach(words, crumb ? crumb->end_marker : crumbs::kNoMarker);
    const crumbs::Step* named = crumb ? crumbs::StepOfMarker(*crumb, reach.unfinished) : nullptr;
    // The ring against the frame's own plan: every recorded step once, in order, with its kind, index and image.
    uint32_t recorded = 0, mismatched = 0, k = 0;
    for (size_t si = 0; si < steps.size(); ++si) {
      const Step& st = steps[si];
      if (st.kind != StepKind::Present) {
        if (mode == RenderMode::PersistentOnly && !st.persistent) continue;
        if (mode == RenderMode::DisplayOnly && st.persistent) continue;
        if (mode == RenderMode::NoPasses) continue;
      } else if (!((windowed || offscreen_out) && mode != RenderMode::PersistentOnly)) {
        continue;
      }
      ++recorded;
      const crumbs::Step* c = crumb && k < crumb->n ? &crumb->steps[k] : nullptr;
      const uint8_t want_kind = st.kind == StepKind::Pass ? crumbs::kPass : st.kind == StepKind::Copy ? crumbs::kCopy : st.kind == StepKind::Dispatch ? crumbs::kDispatch : crumbs::kPresent;
      if (!c || c->step != si || c->index != st.index || !(c->kind == want_kind || (want_kind == crumbs::kCopy && c->kind == crumbs::kCopyBatch))) { ++mismatched; continue; }
      if (c->kind == crumbs::kCopyBatch) si += c->draws - 1;
      ++k;
    }
    const bool ok = named && named->step == selftest_at_step && reach.serial_lo == want && !reach.done && crumb && mismatched == 0 && k == crumb->n;
    Log("[rdr][crumbs][selftest] frame %llu stalled at step %u (marker %u); decoded: slot %u frame word %08x, marker reached %d, step %d; ring: steps recorded %u, crumbs %u, mismatched %u; %s",
        static_cast<unsigned long long>(serial), selftest_at_step, unsigned(selftest_marker), slot_index, reach.serial_lo, reach.unfinished, named ? int(named->step) : -1, recorded,
        crumb ? crumb->n : 0, mismatched, ok ? "PASS" : "FAIL");
    Log("[rdr][crumbs][selftest] words: finished marker %d, front end reached %d", reach.ended, reach.begun);
    vkSetEvent(dev, selftest_event);
  }
  readback.MarkRecorded(slot_index, serial);  // lane 232: the recorded read-back copies are in flight in this slot
  if (ps_pool && ps_count) {  // S102 G10 (instrument): wait for this frame, read the statistics, fold them into the census rows by program name
    if (!(lost_serial && !lost_simulated)) vkWaitForFences(dev, 1, &slot.fence, VK_TRUE, UINT64_MAX);   // S105 A9 review F2: not on the fence a real loss leaves unsubmitted
    std::vector<uint64_t> values(size_t(ps_count) * 2);
    if (!(lost_serial && !lost_simulated) && vkGetQueryPoolResults(dev, ps_pool, 0, ps_count, values.size() * 8, values.data(), 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
      std::lock_guard<std::mutex> lock(g_draw_census_mutex);
      for (uint32_t q = 0; q < ps_count; ++q) {
        DrawCensusRow& row = g_draw_census_rows[ps_names[q]];
        row.vs_invocations += values[q * 2], row.fs_invocations += values[q * 2 + 1];
        if (q < ps_meta.size() && !ps_meta[q].empty())
          if (std::FILE* log = g_draw_log.load(std::memory_order_relaxed)) std::fprintf(log, "%s vs %llu fs %llu afoff %u\n", ps_meta[q].c_str(), static_cast<unsigned long long>(values[q * 2]), static_cast<unsigned long long>(values[q * 2 + 1]), unsigned(q < ps_afoff.size() ? ps_afoff[q] : 0));  // S102 C4; S102 R18-B3: afoff
      }
    }
    ps_count = 0;
  }
  if (options.gpu_timestamp_plant == 2) CollectGpuTime(slot);  // lane 269 plant: read at once, before the GPU is done; must count as not ready
  lap(rt.submit);
  slot.out_wall_ns = 0;
  if (meter_outside) {  // S102 G5 (the instrument): wait for this frame's fence now, so the submit-to-signal wall time is known; collected with the stamps a frame later (CollectGpuTime)
    const uint64_t wait_t0 = RenderNow();
    if (!(lost_serial && !lost_simulated)) vkWaitForFences(dev, 1, &slot.fence, VK_TRUE, UINT64_MAX);   // S105 A9 review F2: not on the fence a real loss leaves unsubmitted
    meter_wait_ns = RenderNow() - wait_t0;
    slot.out_wall_ns = RenderNow() - meter_t0;
    t = RenderNow();  // (the meter's wait is not lapped into any field: it is the instrument's own cost)
  }
  uint64_t present_call_ns = 0;
  if (presenting) {
    // S100 phone 120 fix: with display timing, each picture asks for the refresh after the last one asked for (or now): the halfway
    // picture and its tick land a refresh apart, as the old presenter paced Android (desired present times).
    // Fix 2 (run s100ph4): the 120 mode spaces them at its own rate (options.present_hz): Jarrod's phone reports a 60 Hz refresh while it
    // runs at 120, so the reported refresh held it at 60. Those ask half a slot early, so each lands on the refresh nearest its slot
    // (a slot right on a refresh would otherwise slip one refresh now and then).
    uint64_t desired = 0;
    const uint64_t spacing = options.present_hz ? 1000000000ull / options.present_hz : swapchain.refresh_ns();
    if (device.caps().display_timing && spacing) {
      const uint64_t now = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
      const uint64_t slot = std::max(now, last_desired_ns + spacing);
      last_desired_ns = slot;
      desired = options.present_hz ? slot - spacing / 2 : slot;
    }
    const uint64_t present_t0 = RenderNow();  // S102 S0-3: the time the Present call itself takes (it can block on the display's queue)
    const bool accepted = swapchain.Present(image_index, finished, desired);
    present_call_ns = RenderNow() - present_t0;
    if (accepted || (crumb_plant == 3 && DeviceLost())) {  // lane 256: only an accepted present counts (S102 D1: none after a lost device; plant 3 = the old count, rplay's twin)
      g_presented.fetch_add(1, std::memory_order_relaxed);  // the overlay's shown rate
    }
  }
  if (const char* at = swapchain.lost_at(); at && !lost_serial) OnDeviceLost(at, int(VK_ERROR_DEVICE_LOST), false);  // S102 D1: the acquire (in the present step) or the present saw it first
  crumb = nullptr, crumb_step = nullptr;  // S102 D1: the frame's record is complete
  lap(rt.present);
  {  // S102 S0-3: what this Render spent in Renderer::Render alone (rdr_submit: all of it but the slot's fence wait and the swapchain acquire), and the acquire and Present calls apart.
    // (The live render THREAD's own CPU time, which also holds the bind and the parse, is counted in live.cpp's loop: g_render_thread_ns_total.)
    const uint64_t total = RenderNow() - render_t0;
    const uint64_t idle = fence_wait_ns + acquire_ns_frame + (options.gpu_stamp_plant == 7 ? 0 : meter_wait_ns);  // S102 G5 (F3): the meter's own wait is idle too (plant 7: it is left in the busy time)
    rdr::g_submit_busy_ns_total.fetch_add(total > idle ? total - idle : 0, std::memory_order_relaxed);
    rdr::g_submit_busy_calls.fetch_add(1, std::memory_order_relaxed);
    rdr::g_acquire_ns_total.fetch_add(acquire_ns_frame, std::memory_order_relaxed);
    rdr::g_present_call_ns_total.fetch_add(present_call_ns, std::memory_order_relaxed);
    win_acquire_ns += acquire_ns_frame, win_present_ns += present_call_ns, ++win_renders;
    run_acquire_ns += acquire_ns_frame, run_present_ns += present_call_ns, ++run_renders;
  }
  {  // S102 G5: this Render's CPU time at every point outside the command buffer (outside.h). The cumulative timers of g_render_times give the deltas; the rest are timed above.
    OutsideSums f;
    f.frames = 1;
    auto sub = [](uint64_t a, uint64_t b) { return a > b ? a - b : uint64_t(0); };
    f.ns[kOutFenceWait] = fence_wait_ns;
    f.ns[kOutDrain] = sub(rt.drain, rt0.drain);
    f.ns[kOutDestroys] = sub(rt.destroys, rt0.destroys);
    f.ns[kOutPools] = sub(rt.pools, rt0.pools);
    f.ns[kOutCollect] = out_collect_ns;
    f.ns[kOutPipeStore] = sub(rt.pipestore, rt0.pipestore);
    f.ns[kOutRingCopy] = out_ring_ns;
    f.ns[kOutBegin] = out_begin_ns;  // (timed directly; the stamp-pool regrow and the frame's set-up in the retire lap are not named)
    f.ns[kOutUploads] = sub(rt.uploads, rt0.uploads);
    f.ns[kOutCompile] = sub(rt.compile, rt0.compile);
    f.ns[kOutTextureSets] = sub(rt.set_ns, rt0.set_ns);
    f.ns[kOutFramebuffers] = sub(rt.fb_ns, rt0.fb_ns);
    f.ns[kOutPipelines] = out_pipe_ns;
    f.ns[kOutRecord] = sub(sub(sub(sub(sub(rt.record, rt0.record), f.ns[kOutTextureSets]), f.ns[kOutFramebuffers]), out_pipe_ns), out_end_ns + acquire_ns_frame);
    f.ns[kOutEnd] = out_end_ns;
    f.ns[kOutSubmit] = sub(rt.submit, rt0.submit);
    f.ns[kOutAcquire] = acquire_ns_frame;
    f.ns[kOutPresent] = present_call_ns;
    slot.ts_up_bytes = out_up_bytes;
    if (out_uploads) ++up_tot_frames, up_tot_images += out_up_images, up_tot_buffers += out_uploads - out_up_images, up_tot_barriers += out_up_barriers, up_tot_bytes += out_up_bytes, up_tot_by_producer[0] += out_up_by_producer[0], up_tot_by_producer[1] += out_up_by_producer[1], up_tot_by_producer[2] += out_up_by_producer[2];   // S103 UPL (+ PVR)
    if (slot.ts_detail) for (UpSums* u : {&up_window, &up_run}) {   // S103 GT: steady vs loading frames (stamped runs only: nothing is counted for a player)
      u->max_bytes = std::max<uint64_t>(u->max_bytes, out_up_bytes);
      u->ring_peak = std::max<uint64_t>(u->ring_peak, ring_used_cur);   // S103 PVR
      u->ring_ns += out_ring_ns, u->ring_bytes += ring_used_cur;   // S103 PF2
      if (out_uploads) {   // S103 UPL: calls and barriers of the frames that uploaded (a frame with no upload records none)
        for (int pk = 0; pk < 3; ++pk) u->prod_copies[pk] += out_up_by_producer[pk];   // S103 PVR
        ++u->up_frames, u->image_uploads += out_up_images, u->buffer_uploads += out_uploads - out_up_images, u->barriers += out_up_barriers, u->batched_frames += out_batched ? 1 : 0;
        u->max_barriers = std::max<uint64_t>(u->max_barriers, out_up_barriers), u->max_images = std::max<uint64_t>(u->max_images, out_up_images);
      }
      if (out_up_bytes > kLoadingBytes) ++u->load_frames, u->load_bytes += out_up_bytes;
      else ++u->steady_frames, u->steady_bytes += out_up_bytes;
    }
    f.uploads = out_uploads, f.scaled_uploads = out_scaled, f.upload_bytes = out_up_bytes, f.upload_barriers = out_up_barriers, f.pipelines_built = out_pipe_n;
    f.upload_batched_frames = out_batched ? 1 : 0;
    AddOutside(f);
  }
  if (options.counts) {  // S102 M1: the game's switch (--wwe2k14_rdr_counts) logs the mean counts a frame every 600 frames
    counts_sum.Add(counts_cur.total);
    counts_sum_extra += counts_cur.ring_constants + counts_cur.ring_geometry + counts_cur.ring_prev;
    counts_sum_passes += counts_cur.passes;
    if (++counts_sum_frames == 600) {
      const double n = 600.0;
      Log("[rdr][counts] 600 frames, a frame: %.1f passes, %.1f draws (%.1f indexed), %.1f pipeline binds, %.1f descriptor binds, %.1f dynamic sets, %.1f clears, %.1f copies, %.1f barriers, "
          "%.0f KB into the ring, %.0f vertices, %.0f indices, %.2f M pixels of area bound",
          counts_sum_passes / n, (counts_sum.draws + counts_sum.indexed_draws) / n, counts_sum.indexed_draws / n, counts_sum.pipeline_binds / n, counts_sum.descriptor_binds / n,
          counts_sum.dynamic_sets / n, counts_sum.clears / n, counts_sum.copies / n, counts_sum.barriers / n, counts_sum_extra / n / 1024.0, counts_sum.vertices / n, counts_sum.indices / n,
          counts_sum.area_px / n / 1e6);
      counts_sum = CallCounts{};
      counts_sum_extra = counts_sum_passes = counts_sum_frames = 0;
    }
  }
  std::atomic_ref<uint64_t>(serial).fetch_add(1, std::memory_order_relaxed);  // S102 R21-B (hazard 5): DestroyImage / DestroyBuffer read it on the bind stage
  if (++rt.frames == 600) {
    const double f = 600.0 * 1e6;
    Log("[rdr][render] 600 frames, ms a frame: wait for the slot %.3f, retire %.3f (drain %.3f, destroys %.3f, pool resets %.3f), uploads %.3f, "
        "compile %.3f, record %.3f (texture sets %.3f, new framebuffers %.3f), submit %.3f, present %.3f; a frame: %.0f passes, %.0f steps, %.0f KB "
        "uploaded, %.0f retired items, %.0f destroys, %.1f new framebuffers (%zu cached), %.0f texture sets",
        rt.wait / f, (rt.retire + rt.drain + rt.destroys + rt.pools) / f, rt.drain / f, rt.destroys / f, rt.pools / f, rt.uploads / f,
        rt.compile / f, rt.record / f, rt.set_ns / f, rt.fb_ns / f, rt.submit / f, rt.present / f, rt.passes / 600.0, rt.steps / 600.0,
        rt.upload_bytes / 600.0 / 1024.0, rt.retired_items / 600.0, rt.destroy_items / 600.0, rt.new_framebuffers / 600.0, framebuffers.size(),
        rt.texture_sets / 600.0);
    Log("[rdr][render] bc decoded %llu images, %.1f MB so far (%llu levels)%s", (unsigned long long)bc_images.load(), double(bc_decoded_bytes.load()) / 1048576.0,
        (unsigned long long)bc_decoded_levels.load(), bc_images.load() ? "" : " (none: the device samples BC, or bc_decode is off)");  // S102 U1
    if (rt.pipestore) Log("[rdr][render] pipeline store: %.3f ms a frame in the listed-pipeline slices and saves", rt.pipestore / f);
    rt = RenderTimes{};
  }
}

void Renderer::RenderFrame(const Frame& frame, RenderMode mode) { impl_->Render(frame, mode); }

// S102 G5: add one Render's points to the window and the run; with the meter on the window prints every 600 frames (the line GPU_OUTSIDE.md reads).
void Renderer::Impl::AddOutside(const OutsideSums& f) {
  for (OutsideSums* sums : {&out_window, &out_run}) {
    sums->frames += f.frames;
    for (uint32_t p = 0; p < kOutPointCount; ++p) sums->ns[p] += f.ns[p];
    sums->uploads += f.uploads, sums->scaled_uploads += f.scaled_uploads, sums->upload_bytes += f.upload_bytes, sums->upload_barriers += f.upload_barriers;
    sums->pipelines_built += f.pipelines_built, sums->upload_batched_frames += f.upload_batched_frames;
  }
  if (out_window.frames >= 600 || (census_flush::Due(g_cf_outside) && out_window.frames)) {
    if (rdr::g_gpu_outside.load(std::memory_order_relaxed)) Log("%s", OutsideLine("[rdr][outside] window:", out_window).c_str());
    out_window = OutsideSums{};
  }
}
const OutsideSums& Renderer::OutsideRun() const { return impl_->out_run; }  // S102 G5
uint64_t Renderer::FramesDropped() const { return impl_->frames_dropped.load(std::memory_order_relaxed); }  // S103 CB1
void Renderer::NoteFrameDropped() { impl_->frames_dropped.fetch_add(1, std::memory_order_relaxed); }
