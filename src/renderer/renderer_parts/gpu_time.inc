
void Renderer::Impl::MakeStampPool(FrameSlot& slot, uint32_t cap) {  // S102 S0-3
  VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  query.queryType = VK_QUERY_TYPE_TIMESTAMP;
  query.queryCount = cap;
  slot.timestamps = VK_NULL_HANDLE;
  vkCreateQueryPool(device.device(), &query, nullptr, &slot.timestamps);
  slot.ts_cap = slot.timestamps ? cap : 0;
}

// S102 S0-3 test plant 4: a slow "pass" for the per-role line's twin check: 256 copies of 16 MB inside one scratch buffer (about 8 GB of traffic), each fenced from the
// last. Its stamp is added to the rim class, so it must show there and nowhere else.
void Renderer::Impl::RecordStampPlant(VkCommandBuffer cmd) {
  VkDevice dev = device.device();
  constexpr VkDeviceSize kHalf = 16ull << 20;
  if (!stamp_scratch) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = kHalf * 2;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(dev, &info, nullptr, &stamp_scratch);
    VkMemoryRequirements needs{};
    vkGetBufferMemoryRequirements(dev, stamp_scratch, &needs);
    if (!allocator.Alloc(needs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true, stamp_scratch_memory)) {
      vkDestroyBuffer(dev, stamp_scratch, nullptr);
      stamp_scratch = VK_NULL_HANDLE;
      Log("[rdr][gpu] stamp plant 4: no memory for the scratch buffer");
      return;
    }
    vkBindBufferMemory(dev, stamp_scratch, stamp_scratch_memory.memory, stamp_scratch_memory.offset);
  }
  const VkBufferCopy region{0, kHalf, kHalf};
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  for (int i = 0; i < 256; ++i) {
    vkCmdCopyBuffer(cmd, stamp_scratch, stamp_scratch, 1, &region);
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
  }
}

// Lane 269, S102 S0-3. Called after a slot's fence wait, so its last submit is complete and every result is available without waiting; a not-ready result
// is counted and dropped, never waited for. A render that recorded no pass is not a drawn frame and gives no sample (counted apart). With per-step stamps the
// GPU time splits: [0] -> [1] is the wait for the display, [1] -> the tail is the work, cut at each step's stamp into its class.
void Renderer::Impl::CollectGpuTime(FrameSlot& slot) {
  if (slot.occ_pending && slot.occ) {  // S102 G11: the pixels each frame-class draw wrote (read after the slot's fence wait, so never waits)
    slot.occ_pending = false;
    std::FILE* ft = rdr::g_frame_trace.load(std::memory_order_relaxed);
    uint64_t px[512];
    if (ft && slot.occ_count && vkGetQueryPoolResults(device.device(), slot.occ, 0, slot.occ_count, sizeof(px), px, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
      for (uint32_t q = 0; q < slot.occ_count; ++q)
        std::fprintf(ft, "{\"t\":\"o\",\"f\":%llu,\"p\":%u,\"op\":%u,\"px\":%llu}\n", static_cast<unsigned long long>(slot.occ_serial), slot.occ_meta[2 * q], slot.occ_meta[2 * q + 1], static_cast<unsigned long long>(px[q]));
  }
  if (!slot.ts_pending || !slot.timestamps) return;
  slot.ts_pending = false;
  if (slot.ts_detail && !slot.ts_has_pass) {  // (with stamps on only: the default gpu_ms counts every render as before)
    ++role_window.no_pass, ++role_run.no_pass;
    return;
  }
  const uint32_t count = slot.ts_count;
  uint64_t ticks[2 + 2 * 1024];
  if (count < 2 || count > sizeof(ticks) / sizeof(ticks[0])) {
    ++gpu_bad, ++gpu_bad_total;
    return;
  }
  const VkResult result = vkGetQueryPoolResults(device.device(), slot.timestamps, 0, count, count * sizeof(uint64_t), ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
  if (result != VK_SUCCESS) {
    ++gpu_not_ready, ++gpu_not_ready_total;
    return;
  }
  const uint64_t mask = ts_valid_bits >= 64 ? ~0ull : ((1ull << ts_valid_bits) - 1);
  const uint32_t last = count - 1;
  auto span = [&](uint32_t i, uint32_t j) { return double((ticks[j] - ticks[i]) & mask) * ts_period; };
  const double ns = span(0, last);
  const double wall_ns = double(RenderNow() - slot.ts_submit_ns);
  if (!(ns > 0.0) || ns > wall_ns) {  // sanity: (0, time since the submit]
    ++gpu_bad, ++gpu_bad_total;
    return;
  }
  if (slot.ts_detail && slot.ts_class.size() + 2 == count) {
    double cls_ns[kGpuClassCount] = {};
    double blit_ns = 0;  // S102 G2: the cap's blit span (also counted in cls_ns[kGpuOutput])
    bool sane = true;
    for (uint32_t k = 2; k <= last && sane; ++k) {
      const double step_ns = span(k - 1, k);
      if (step_ns > ns) sane = false;  // a stamp out of order shows as a huge wrapped span
      if (slot.ts_class[k - 2] == kBlitTag) cls_ns[kGpuOutput] += step_ns, blit_ns += step_ns;  // S102 G2
      else cls_ns[std::min<uint32_t>(slot.ts_class[k - 2], kGpuClassCount - 1)] += step_ns;
    }
    if (std::FILE* ft = rdr::g_frame_trace.load(std::memory_order_relaxed); ft && slot.ts_pass.size() == slot.ts_class.size()) {  // S102 G11: the GPU ms of each frame-class pass (needs the census: a stamp after every step)
      for (uint32_t k = 2; k <= last; ++k)
        if (slot.ts_class[k - 2] == kGpuFrame && slot.ts_kind[k - 2] == 0 && slot.ts_pass[k - 2] != 0xFFFFFFFFu)
          std::fprintf(ft, "{\"t\":\"g\",\"f\":%llu,\"p\":%u,\"ms\":%.5f}\n", static_cast<unsigned long long>(slot.ts_serial), slot.ts_pass[k - 2], span(k - 1, k) / 1e6);
    }
    if (slot.ts_image.size() == slot.ts_class.size() && rdr::g_gpu_pass_stamps.load(std::memory_order_relaxed)) {  // S102 R24-B: the per-step table (mean / p90 per step over the window)
      std::map<uint64_t, uint32_t> seen;  // occurrences of (class, kind, image) so far this frame = the step's index among its image's steps
      for (uint32_t k = 2; k <= last; ++k) {
        const uint64_t base = (uint64_t(slot.ts_class[k - 2]) << 48) | (uint64_t(slot.ts_kind[k - 2]) << 40) | slot.ts_image[k - 2];
        const uint32_t occ = std::min<uint32_t>(seen[base]++, 255);
        const bool plant5 = slot.sd_arm && k == 2 && g_stencil_dead_plant.load(std::memory_order_relaxed) == 5;   // plant 5: +1 ms on the first step of every ON frame (the ON column must show it, the OFF column not)
        pass_table[base | (uint64_t(occ) << 32) | (slot.sd_arm ? (1ull << 63) : 0)].push_back(float(span(k - 1, k) / 1e3 + (plant5 ? 1000.0 : 0.0)));
      }
      ++pass_table_frames;
      ++pass_table_frames_arm[slot.sd_arm ? 1 : 0];
    }
    if (slot.gs_frame && slot.ts_image.size() == slot.ts_class.size()) CollectGroups(slot, ticks, count, mask, ns);   // S102 R24-F: the group stamps of the colour / frame passes
    if (slot.ts_image.size() == slot.ts_class.size() && slot.ts_src.size() == slot.ts_class.size() && rdr::g_gpu_census.load(std::memory_order_relaxed)) {  // S102 G6
      for (uint32_t k = 2; k <= last; ++k) {
        CensusRow& row = census_run[(uint64_t(slot.ts_class[k - 2]) << 40) | (uint64_t(slot.ts_kind[k - 2]) << 32) | slot.ts_image[k - 2]];
        row.ms += span(k - 1, k) / 1e6, ++row.steps;
        if (slot.ts_kind[k - 2] == 1 && slot.ts_class[k - 2] == kGpuCopies && slot.ts_src[k - 2] < kGpuClassCount) {  // S102 G6 (item 7): the copies by what they copy FROM
          CensusRow& by_src = census_copy_src[slot.ts_src[k - 2]];
          by_src.ms += span(k - 1, k) / 1e6, ++by_src.steps;
        }
      }
      ++census_frames;
    }
    const double gap_ns = span(0, 1);
    if (!sane || gap_ns > ns) {
      ++gpu_bad, ++gpu_bad_total;
      return;
    }
    const double work_ns = ns - gap_ns;
    static uint64_t plant12_n = 0;
    const bool plant12_skip = options.gpu_stamp_plant == 12 && (++plant12_n & 1);   // S103 GT test plant 12: every other frame is left out of the role sums (the csv's GPU time still counts it): the rig's work-vs-csv check must fail
    if (slot.ts_mark != mark_win.seq) {   // S103 GT round 3: the first frame of a new route window: the finished one is logged
      FlushMarkWindow("");
      mark_win = MarkWin{};
      mark_win.seq = slot.ts_mark;
    }
    if (!plant12_skip) {
      MarkWin& w = mark_win;
      for (int c = 0; c < kGpuClassCount; ++c) w.cls_ns[c] += cls_ns[c];
      w.gap_ns += gap_ns, w.work_ns += work_ns, w.bracket_ns += ns, ++w.frames;
      if (slot.ts_up_bytes > kLoadingBytes) ++w.loading, w.loading_bytes += slot.ts_up_bytes;
      else ++w.steady, w.steady_bytes += slot.ts_up_bytes;
      if (slot.ts_up && slot.ts_up <= last) {
        const double u = span(slot.ts_up - 1 - (slot.ts_up_mid ? 1u : 0u), slot.ts_up);   // S103 PVR
        if (u < ns) w.up_gpu_ns += u, ++w.up_frames;
      }
    }
    if (!plant12_skip) for (RoleSums* sums : {&role_window, &role_run}) {
      for (int c = 0; c < kGpuClassCount; ++c) sums->cls_ms[c] += cls_ns[c] / 1e6;
      sums->blit_ms += blit_ns / 1e6;  // S102 G2
      sums->gap_ms += gap_ns / 1e6, sums->work_ms += work_ns / 1e6, sums->total_ms += ns / 1e6;
      ++sums->frames;
      sums->stamps += count;
      if ((windowed || offscreen_out) && !slot.ts_presented) ++sums->no_present;
    }
    if (rdr::g_arms_on.load(std::memory_order_relaxed)) {  // S102 G10 (review F1): kept per arm tag, only when rplay --arms asked (the game never grows this)
      std::lock_guard<std::mutex> lock(rdr::g_arm_mutex);
      ArmSamples& arm = rdr::g_arm_samples[slot.arm_tag];
      rdr::g_arm_kept.fetch_add(1, std::memory_order_relaxed);
      arm.colour.push_back(float(cls_ns[kGpuColor] / 1e6)), arm.work.push_back(float(work_ns / 1e6)), arm.total.push_back(float(ns / 1e6));
      arm.copies.push_back(float(cls_ns[kGpuCopies] / 1e6)), arm.shadow.push_back(float((cls_ns[kGpuShadow] + cls_ns[kGpuPlane] + cls_ns[kGpuRim]) / 1e6)), arm.other.push_back(float((cls_ns[kGpuPost] + cls_ns[kGpuCards] + cls_ns[kGpuFrame] + cls_ns[kGpuUi] + cls_ns[kGpuOutput] + cls_ns[kGpuPrelight] + cls_ns[kGpuFeedback] + cls_ns[kGpuOther]) / 1e6));
    }
    work_samples.push_back(float(work_ns / 1e6));
    gap_samples.push_back(float(gap_ns / 1e6));
    if (slot.ts_up && slot.ts_up <= last) {  // S102 G5: the uploads' own span (the stamp after ApplyUploads minus the one before it)
      const uint32_t up_pre = slot.ts_up - 1 - (slot.ts_up_mid ? 1u : 0u);   // S103 PVR: the mid stamp sits between the pair
      const double up_ns = span(up_pre, slot.ts_up);
      if (up_ns < ns && slot.ts_up_mid) {
        const double img_ns = span(up_pre, slot.ts_up_mid), buf_ns = span(slot.ts_up_mid, slot.ts_up);
        for (UpSums* u : {&up_window, &up_run}) u->mid_img_ns += img_ns, u->mid_buf_ns += buf_ns, ++u->mid_frames;
      }
      if (up_ns < ns) {
        for (OutsideSums* o : {&out_window, &out_run}) o->upload_gpu_ns += uint64_t(up_ns), ++o->upload_gpu_samples;
        for (UpSums* u : {&up_window, &up_run}) u->gpu_ns += up_ns, ++u->gpu_frames;   // S103 GT
      }
    }
    rdr::g_gpu_work_ns_total.fetch_add(uint64_t(work_ns), std::memory_order_relaxed);
  }
  if (slot.out_wall_ns) {  // S102 G5: the meter: submit-to-signal wall time less the stamped bracket = the GPU's time outside what the stamps cover
    for (OutsideSums* o : {&out_window, &out_run}) {
      o->gpu_wall_ns += slot.out_wall_ns, o->gpu_stamped_ns += uint64_t(ns), ++o->gpu_samples;
      o->gpu_outside_ns += slot.out_wall_ns > uint64_t(ns) ? slot.out_wall_ns - uint64_t(ns) : 0;
    }
    slot.out_wall_ns = 0;
  }
  gpu_samples.push_back(float(ns / 1e6));
  rdr::g_gpu_ns_total.fetch_add(uint64_t(ns), std::memory_order_relaxed);
  rdr::g_gpu_last_us.store(uint32_t(std::min(ns / 1000.0, 4.0e9)), std::memory_order_relaxed);
  rdr::g_gpu_frames.fetch_add(1, std::memory_order_relaxed);
  if (gpu_samples.size() >= 600) LogGpuWindow("600 frames");
}

// S102 R24-F: one frame's group stamps. `ticks` are the main pool's results (count of them), the group pool's are read here (never waited for: a not-ready result drops the frame's groups). Each instrumented pass is a chain of
// segments from its begin stamp to its closing stamp; the chain is checked (continuous, no wrapped span), its spans are added per dominant group, the draws per group come from the exact counts, and the draws the segments hold are
// compared with the draws the pass issued (coverage). Any failed check is counted and the frame's groups are not added to the table.
void Renderer::Impl::CollectGroups(FrameSlot& slot, const uint64_t* ticks, uint32_t count, uint64_t mask, double frame_ns) {
  uint64_t gt[rdr::kGroupPoolCap];
  const uint32_t n = std::min<uint32_t>(slot.gs_count, rdr::kGroupPoolCap);
  if (n && vkGetQueryPoolResults(device.device(), slot.gs_pool, 0, n, n * sizeof(uint64_t), gt, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    ++group_not_ready;
    return;
  }
  const auto raw = [&](uint32_t id) { return (id & rdr::kPoolBit) ? gt[id & ~rdr::kPoolBit] : ticks[id]; };
  const auto span = [&](uint32_t a, uint32_t b) { return double((raw(b) - raw(a)) & mask) * ts_period; };
  std::map<uint32_t, const FrameSlot::GsPass*> by_end;
  for (const FrameSlot::GsPass& p : slot.gs_passes)
    if (p.end_stamp >= 2 && p.end_stamp < count && p.begin_stamp < p.end_stamp && p.end_stamp - 2 < slot.ts_kind.size() && slot.ts_kind[p.end_stamp - 2] == 0) by_end[p.end_stamp] = &p;
  if (by_end.size() != slot.gs_passes.size()) {   // a pass whose closing stamp was not written (the main pool ran out): not a frame to read
    ++group_chain_broken;
    return;
  }
  struct Row { uint64_t key; rdr::PassGroups g; const FrameSlot::GsPass* p; double direct_ns; };   // S103 GSTP: direct_ns = ticks[end] - ticks[begin] of the pass, read straight from the stamps
  std::vector<Row> rows;
  std::map<uint64_t, uint32_t> seen;
  bool ok = true;
  for (uint32_t k = 2; k < count; ++k) {   // the pass table's occurrence rule: the step's index among the steps of its image
    const uint64_t base = (uint64_t(slot.ts_class[k - 2]) << 48) | (uint64_t(slot.ts_kind[k - 2]) << 40) | slot.ts_image[k - 2];
    const uint32_t occ = std::min<uint32_t>(seen[base]++, 255);
    const auto it = by_end.find(k);
    if (it == by_end.end()) continue;
    const FrameSlot::GsPass& p = *it->second;
    const rdr::ChainResult chain = rdr::CheckChain(slot.gs_segs.data() + p.first_seg, p.segs, p.begin_stamp, p.end_stamp, frame_ns, span);
    Row row{base | (uint64_t(occ) << 32) | (slot.sd_arm ? (1ull << 63) : 0), {}, &p, span(p.begin_stamp, p.end_stamp)};
    for (uint32_t s = 0; s < p.segs; ++s) {
      const rdr::GroupSeg& seg = slot.gs_segs[p.first_seg + s];
      rdr::Aggregate(seg, span(seg.begin, seg.end), row.g);
    }
    const uint32_t held = row.g.total_draws;
    if (chain.broken) ++group_chain_broken, ok = false;
    if (held != p.issued) ++group_cover_bad, ok = false;
    {   // S103 GSTP (review fix 2): the groups' sum is ADDED UP from the segments' spans, the pass's own span is read straight from its two end stamps: independent readings of one pass, equal to rounding (a segment the chain lost, a stamp out of order or a wrapped span shows here)
      double sum = 0;
      for (uint8_t g = 0; g < rdr::kGrpCount; ++g) sum += row.g.ns[g];
      if (std::fabs(sum - row.direct_ns) > 2.0 * ts_period) ++group_sum_bad, ok = false;
    }
    rows.push_back(row);
  }
  if (!ok) return;
  for (const Row& row : rows) {
    GroupPassAcc& pa = group_pass[row.key];
    double pass_ns = 0;
    for (uint8_t g = 0; g < rdr::kGrpCount; ++g) pass_ns += row.g.ns[g];
    pa.direct_us += row.direct_ns / 1e3;   // S103 GSTP
    pa.us += pass_ns / 1e3, ++pa.frames, pa.draws += row.g.total_draws, pa.issued += row.p->issued, pa.mixed += row.g.mixed_draws, pa.segs += row.p->segs, pa.stamps += row.p->segs - 1;
    for (uint8_t g = 0; g < rdr::kGrpCount; ++g) {
      if (!row.g.draws[g] && !(row.g.ns[g] > 0)) continue;
      GroupAcc& a = group_table[row.key | (uint64_t(g) << 56)];
      a.us.push_back(float(row.g.ns[g] / 1e3));
      a.draws += row.g.draws[g];
    }
  }
  ++group_frames;
  group_stamps_total += slot.gs_count;
  if (slot.gs_overflow) ++group_overflow_frames;
}

void Renderer::Impl::FlushMarkWindow(const char* suffix) {   // S103 GT round 3
  const MarkWin& w = mark_win;
  if (!w.seq || !w.frames) return;
  std::string name;
  {
    std::lock_guard<std::mutex> lock(rdr::g_gpu_mark_mu);
    if (w.seq <= rdr::g_gpu_mark_names.size()) name = rdr::g_gpu_mark_names[w.seq - 1];
  }
  const double f = double(w.frames) * 1e6;   // ns -> ms a frame
  const double up = w.up_gpu_ns / f;   // ms a frame: the upload span summed over the frames that had one, spread over ALL the window's frames
  double other_net = w.cls_ns[kGpuOther] / f - up;
  if (other_net < 0) other_net = 0;
  const double stamped = w.work_ns + w.gap_ns;
  Log("[rdr][gpu][window] %s%s: GPU frames %llu; by role, mean ms a frame: colour %.3f, shadow %.3f, rim %.3f, plane %.3f, cards %.3f, post %.3f, output %.3f, ui %.3f, frame %.3f, copies %.3f, prelight %.3f, feedback %.3f, "
      "other %.3f, uploads %.3f = work %.3f; start gap [0]-[1] %.3f; bracket %.3f (the csv gpu_ms basis: [0] to the tail stamp of every frame); unstamped %.3f (bracket - work - gap); "
      "uploads steady (<= %llu KB) frames %llu = %.3f MB/frame, loading frames %llu = %.3f MB/frame",
      name.c_str(), suffix, static_cast<unsigned long long>(w.frames), w.cls_ns[kGpuColor] / f, w.cls_ns[kGpuShadow] / f, w.cls_ns[kGpuRim] / f, w.cls_ns[kGpuPlane] / f, w.cls_ns[kGpuCards] / f, w.cls_ns[kGpuPost] / f,
      w.cls_ns[kGpuOutput] / f, w.cls_ns[kGpuUi] / f, w.cls_ns[kGpuFrame] / f, w.cls_ns[kGpuCopies] / f, w.cls_ns[kGpuPrelight] / f, w.cls_ns[kGpuFeedback] / f, other_net, up, w.work_ns / f, w.gap_ns / f, w.bracket_ns / f,
      (w.bracket_ns - stamped) / f, static_cast<unsigned long long>(kLoadingBytes >> 10), static_cast<unsigned long long>(w.steady), w.steady ? double(w.steady_bytes) / 1048576.0 / double(w.steady) : 0.0,
      static_cast<unsigned long long>(w.loading), w.loading ? double(w.loading_bytes) / 1048576.0 / double(w.loading) : 0.0);
}

void Renderer::Impl::LogGpuWindow(const char* why) {
  if (!std::strcmp(why, "last window") || !std::strcmp(why, "summary")) FlushMarkWindow(" (open at exit)");   // S103 GT round 3: the window that was still open
  if (gpu_samples.empty() && !gpu_bad && !gpu_not_ready && !role_window.no_pass) return;
  std::vector<float> v = gpu_samples;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  double sum = 0;
  for (float x : v) sum += x;
  Log("[rdr][gpu] %s: frames %zu, gpu ms median %.3f / mean %.3f / p95 %.3f / max %.3f; dropped %llu implausible (outside 0..wall time), %llu not ready",
      why, n, n ? v[n / 2] : 0.0f, n ? sum / double(n) : 0.0, n ? v[std::min(n - 1, size_t(double(n) * 0.95))] : 0.0f, n ? v[n - 1] : 0.0f,
      static_cast<unsigned long long>(gpu_bad), static_cast<unsigned long long>(gpu_not_ready));
  if (role_window.frames) {  // S102 S0-3: by role, the start gap apart; the means are over the frames with stamps, absent roles count 0, so the roles add up to the work
    auto median = [](std::vector<float>& s) {
      if (s.empty()) return 0.0;
      std::sort(s.begin(), s.end());
      return double(s[s.size() / 2]);
    };
    const double f = double(role_window.frames);
    const RoleSums& r = role_window;
    Log("[rdr][gpu] %s by role, mean ms a frame over %llu frames: colour %.3f, shadow %.3f, rim %.3f, plane %.3f, cards %.3f, post %.3f, output %.3f, ui %.3f, frame %.3f, copies %.3f, prelight %.3f, feedback %.3f, other %.3f = work %.3f (median %.3f)"
        " + start gap [0]-[1] %.3f (median %.3f) = %.3f (the bracket above); a render spends %.3f ms in vkAcquireNextImage and %.3f ms in the Present call; renders with no pass %llu,"
        " with no present (a window is up) %llu, stamp overflows %llu, %.1f stamps a frame; passes a frame by class: colour %.1f, shadow %.1f, rim %.1f, plane %.1f, cards %.1f, post %.1f, output %.1f, ui %.1f, frame %.1f, copies %.1f, prelight %.1f, feedback %.1f, other %.1f; of 'output' the capped blit %.3f ms",  // S102 G2 F5 + G6
        why, static_cast<unsigned long long>(r.frames), r.cls_ms[kGpuColor] / f, r.cls_ms[kGpuShadow] / f, r.cls_ms[kGpuRim] / f, r.cls_ms[kGpuPlane] / f, r.cls_ms[kGpuCards] / f,
        r.cls_ms[kGpuPost] / f, r.cls_ms[kGpuOutput] / f, r.cls_ms[kGpuUi] / f, r.cls_ms[kGpuFrame] / f, r.cls_ms[kGpuCopies] / f, r.cls_ms[kGpuPrelight] / f, r.cls_ms[kGpuFeedback] / f, r.cls_ms[kGpuOther] / f, r.work_ms / f, median(work_samples), r.gap_ms / f, median(gap_samples), r.total_ms / f,
        win_renders ? double(win_acquire_ns) / 1e6 / double(win_renders) : 0.0, win_renders ? double(win_present_ns) / 1e6 / double(win_renders) : 0.0,
        static_cast<unsigned long long>(r.no_pass), static_cast<unsigned long long>(r.no_present), static_cast<unsigned long long>(r.overflow), double(r.stamps) / f,
        double(r.cls_passes[kGpuColor]) / f, double(r.cls_passes[kGpuShadow]) / f, double(r.cls_passes[kGpuRim]) / f, double(r.cls_passes[kGpuPlane]) / f, double(r.cls_passes[kGpuCards]) / f,
        double(r.cls_passes[kGpuPost]) / f, double(r.cls_passes[kGpuOutput]) / f, double(r.cls_passes[kGpuUi]) / f, double(r.cls_passes[kGpuFrame]) / f, double(r.cls_passes[kGpuCopies]) / f, double(r.cls_passes[kGpuPrelight]) / f,
        double(r.cls_passes[kGpuFeedback]) / f, double(r.cls_passes[kGpuOther]) / f, r.blit_ms / f);
  }
  if (rdr::g_gpu_stamps.load(std::memory_order_relaxed)) {   // S103 GT: the upload view (steady vs loading frames), the same window as the lines above; the format is read by tools\surface\bench_line.py --roles (stamped runs only)
    const UpSums& u = up_window;
    const uint64_t n = u.steady_frames + u.load_frames;
    if (n) {
      const double mb = 1.0 / 1048576.0;
      Log("[rdr][gpu][uploads] %s: frames %llu; steady (<= %llu KB uploaded) frames %llu = %.3f MB/frame; loading (> %llu KB) frames %llu = %.3f MB/frame (%.1f %% of the frames, %.1f %% of the bytes); all frames %.3f MB/frame; biggest frame %.2f MB; "
          "GPU ms of the uploads' commands %.3f a frame over %llu frames with the stamps (a copy is its own role: see 'copies' above); "
          "calls (S103 UPL): %llu frames uploaded (%.1f %%), a frame that uploaded: %.2f image copies (biggest %llu), %.2f buffer copies, %.2f barriers (biggest %llu), batched in %.1f %% of them (frames with 2+ image copies %llu, of them left unbatched by a duplicate subresource %llu); a frame (all): %.2f image copies, %.2f barriers; upload batch switch %s; "
          "S103 PVR: buffer copies a frame that uploaded by producer: posed models %.2f, cloth / CPU-skinned %.2f, other %.2f; span split at the mid stamp (stamp-per-step mode only): image part %.3f ms, buffer part %.3f ms over %llu frames; peak frame ring use (constants + geometry + previous constants) %.2f MB of %.2f MB; S103 PF2: the host's ring memcpy %.3f ms a frame (mean %.2f MB a frame)",
          why, static_cast<unsigned long long>(n), static_cast<unsigned long long>(kLoadingBytes >> 10), static_cast<unsigned long long>(u.steady_frames), u.steady_frames ? double(u.steady_bytes) * mb / double(u.steady_frames) : 0.0,
          static_cast<unsigned long long>(kLoadingBytes >> 10), static_cast<unsigned long long>(u.load_frames), u.load_frames ? double(u.load_bytes) * mb / double(u.load_frames) : 0.0,
          100.0 * double(u.load_frames) / double(n), (u.steady_bytes + u.load_bytes) ? 100.0 * double(u.load_bytes) / double(u.steady_bytes + u.load_bytes) : 0.0,
          double(u.steady_bytes + u.load_bytes) * mb / double(n), double(u.max_bytes) * mb, u.gpu_frames ? u.gpu_ns / 1e6 / double(u.gpu_frames) : 0.0, static_cast<unsigned long long>(u.gpu_frames),
          static_cast<unsigned long long>(u.up_frames), 100.0 * double(u.up_frames) / double(n), u.up_frames ? double(u.image_uploads) / double(u.up_frames) : 0.0, static_cast<unsigned long long>(u.max_images),
          u.up_frames ? double(u.buffer_uploads) / double(u.up_frames) : 0.0, u.up_frames ? double(u.barriers) / double(u.up_frames) : 0.0, static_cast<unsigned long long>(u.max_barriers),
          u.up_frames ? 100.0 * double(u.batched_frames) / double(u.up_frames) : 0.0, static_cast<unsigned long long>(rdr::g_upload_multi_frames.load(std::memory_order_relaxed)), static_cast<unsigned long long>(rdr::g_upload_dup_fallbacks.load(std::memory_order_relaxed)), double(u.image_uploads) / double(n), double(u.barriers) / double(n), rdr::g_upload_batch.load(std::memory_order_relaxed) ? "ON" : "off",
          u.up_frames ? double(u.prod_copies[1]) / double(u.up_frames) : 0.0, u.up_frames ? double(u.prod_copies[2]) / double(u.up_frames) : 0.0, u.up_frames ? double(u.prod_copies[0]) / double(u.up_frames) : 0.0,
          u.mid_frames ? u.mid_img_ns / 1e6 / double(u.mid_frames) : 0.0, u.mid_frames ? u.mid_buf_ns / 1e6 / double(u.mid_frames) : 0.0, static_cast<unsigned long long>(u.mid_frames),
          double(u.ring_peak) * mb, double(kRingSection - kOverlayRange - kConstantRange - kDlssConstants) * mb, double(u.ring_ns) / 1e6 / double(n), double(u.ring_bytes) * mb / double(n));
    }
    up_window = UpSums{};
  }
  if (role_window.ident_frames) {   // S103 GT: the bracket line: in the stamp-per-step frames every recorded step is closed by a stamp that was written, and in every stamped frame the stamp count is exactly [0] + [1] + uploads pair + closes + tail
    const RoleSums& r = role_window;
    Log("[rdr][gpu][bracket] %s: %llu stamped frames, stamp count identity broken in %llu; %llu stamp-per-step frames, steps recorded %llu, steps closed by a written stamp %llu (%s), stamp pool overflows %llu",
        why, static_cast<unsigned long long>(r.ident_frames), static_cast<unsigned long long>(r.ident_bad), static_cast<unsigned long long>(r.census_frames), static_cast<unsigned long long>(r.steps_rec),
        static_cast<unsigned long long>(r.steps_closed), r.steps_rec == r.steps_closed && !r.ident_bad ? "all bracketed" : "UNBRACKETED", static_cast<unsigned long long>(r.overflow));
  }
  if (!pass_table.empty() && pass_table_frames) {  // S102 R24-B: one row per step (role, kind, occurrence on that image, image, size, format): mean and p90 GPU us over the frames that had the step
    static const char* const kinds[] = {"pass", "copy", "present", "tail", "plant", "dispatch", "uploads", "uploads_img"};   // S103 GT: 6 = the pair of stamps around ApplyUploads, 7 (S103 PVR) = the mid stamp between its image part and its buffer part
    struct Row { uint64_t key; double mean, p90, sum; size_t n; };
    std::vector<Row> rows;
    double table_sum = 0;
    for (auto& [key, v] : pass_table) {
      std::sort(v.begin(), v.end());
      double sum = 0;
      for (float x : v) sum += x;
      rows.push_back({key, sum / double(v.size()), double(v[std::min(v.size() - 1, size_t(double(v.size()) * 0.9))]), sum, v.size()});
      table_sum += sum;
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.sum > b.sum; });
    const double f = double(pass_table_frames);
    const double fa[2] = {double(std::max<uint64_t>(1, pass_table_frames_arm[0])), double(std::max<uint64_t>(1, pass_table_frames_arm[1]))};   // S102 R24-C review F4: per-arm frame counts
    Log("[rdr][gpu][passes] %s: per-step GPU table over %llu frames (a stamp after EVERY step: it drains the pipe, read as a ranking); %zu steps; the steps add up to %.3f ms a frame (= the work); columns: role kind #occurrence-on-that-image image WxH fmt: mean us / p90 us over the frames that had it, frames that had it, ms a frame (over all frames)",
        why, static_cast<unsigned long long>(pass_table_frames), rows.size(), table_sum / 1e3 / f);
    double rest = 0;
    size_t shown = 0;
    std::set<uint64_t> want_rows;   // S103 R24S: a printed row's other-arm twin (same key, bit 63 flipped) prints too, under the floor or past the cap, so an in-run flip always shows both columns
    {
      size_t n_want = 0;
      for (const Row& r : rows) {
        if (n_want >= 70 || r.sum / 1e3 / f < 0.004) continue;
        ++n_want;
        want_rows.insert(r.key), want_rows.insert(r.key ^ (1ull << 63));
      }
    }
    for (const Row& r : rows) {
      if (!want_rows.count(r.key)) { rest += r.sum; continue; }
      ++shown;
      const uint32_t img = uint32_t(r.key & 0xFFFFFFFFu);
      Log("[rdr][gpu][passes] %s %s #%u image %u %ux%u fmt %u: %.0f us mean / %.0f us p90, %zu of %llu frames, %.4f ms a frame [stencil_dead %s: %llu frames]",
          GpuClassName(uint32_t((r.key >> 48) & 0x7FFF)), kinds[std::min<uint32_t>(uint32_t((r.key >> 40) & 0xFF), 7)], uint32_t((r.key >> 32) & 0xFF), img,
          uint32_t(CensusImageW(img)), uint32_t(CensusImageH(img)), uint32_t(CensusImageFmt(img)), r.mean, r.p90, r.n, static_cast<unsigned long long>(pass_table_frames), r.sum / 1e3 / fa[r.key >> 63], (r.key >> 63) ? "ON" : "OFF", static_cast<unsigned long long>(pass_table_frames_arm[r.key >> 63]));
    }
    if (rest > 0) Log("[rdr][gpu][passes] the %zu smaller steps together: %.4f ms a frame", rows.size() - shown, rest / 1e3 / f);
    pass_table.clear(), pass_table_frames = 0, pass_table_frames_arm[0] = pass_table_frames_arm[1] = 0;
  }
  if (group_frames || group_not_ready || group_chain_broken || group_cover_bad || group_sum_bad) {   // S102 R24-F: the group table: per instrumented pass and group, mean / p90 GPU us over the frames that had the group
    static const char* const kinds[] = {"pass", "copy", "present", "tail", "plant", "dispatch", "uploads", "uploads_img"};
    const double f = double(std::max<uint64_t>(1, group_frames));
    uint64_t pass_draws = 0, held_draws = 0;
    for (const auto& [pk, pa] : group_pass) pass_draws += pa.issued, held_draws += pa.draws;
    Log("[rdr][gpu][groups] %s: group GPU table over %llu frames (a stamp at each change of draw group inside the colour / frame passes, min run %d draws; a stamp inside a pass drains it, read as a ranking, not as the shipped cost); %.1f group stamps a frame;"
        " frames dropped: group results not ready %llu, chain broken %llu, coverage short %llu; frames that ran out of stamps %llu; draws held by the segments %llu of %llu the passes issued (%.2f %%); frames where the groups did not add up to the pass's own stamp span %llu",
        why, static_cast<unsigned long long>(group_frames), std::max(1, rdr::g_gpu_group_stamps.load(std::memory_order_relaxed)), double(group_stamps_total) / f,
        static_cast<unsigned long long>(group_not_ready), static_cast<unsigned long long>(group_chain_broken), static_cast<unsigned long long>(group_cover_bad), static_cast<unsigned long long>(group_overflow_frames),
        static_cast<unsigned long long>(held_draws), static_cast<unsigned long long>(pass_draws), pass_draws ? 100.0 * double(held_draws) / double(pass_draws) : 0.0, static_cast<unsigned long long>(group_sum_bad));
    struct GRow { uint64_t key, pkey; double mean, p90, sum; size_t n; uint64_t draws; };
    std::vector<GRow> grows;
    for (auto& [key, a] : group_table) {
      std::sort(a.us.begin(), a.us.end());
      double sum = 0;
      for (float x : a.us) sum += x;
      grows.push_back({key, key & ~(0x3Full << 56), sum / double(a.us.size()), double(a.us[std::min(a.us.size() - 1, size_t(double(a.us.size()) * 0.9))]), sum, a.us.size(), a.draws});
    }
    std::sort(grows.begin(), grows.end(), [](const GRow& a, const GRow& b) { return a.sum > b.sum; });
    // per pass first: the pass line (its own span, the draws, the purity), then its groups biggest first
    std::vector<std::pair<uint64_t, GroupPassAcc>> passes_sorted(group_pass.begin(), group_pass.end());
    std::sort(passes_sorted.begin(), passes_sorted.end(), [](const auto& a, const auto& b) { return a.second.us > b.second.us; });
    for (const auto& [pk, pa] : passes_sorted) {
      const uint32_t img = uint32_t(pk & 0xFFFFFFFFu);
      const double pf = double(std::max<uint64_t>(1, pa.frames));
      Log("[rdr][gpu][groups] %s %s #%u image %u %ux%u fmt %u [stencil_dead %s]: pass %.4f ms a frame (its own stamp span %.4f ms) over %llu frames, %.1f draws a frame (segments hold %.1f), %.1f segments a frame, %.2f %% of its draws sit in a segment labelled with another group",
          GpuClassName(uint32_t((pk >> 48) & 0x7FFF)), kinds[std::min<uint32_t>(uint32_t((pk >> 40) & 0xFF), 7)], uint32_t((pk >> 32) & 0xFF), img,
          uint32_t(CensusImageW(img)), uint32_t(CensusImageH(img)), uint32_t(CensusImageFmt(img)), (pk >> 63) ? "ON" : "OFF", pa.us / 1e3 / pf, pa.direct_us / 1e3 / pf, static_cast<unsigned long long>(pa.frames),
          double(pa.issued) / pf, double(pa.draws) / pf, double(pa.segs) / pf, pa.draws ? 100.0 * double(pa.mixed) / double(pa.draws) : 0.0);
      for (const GRow& r : grows) {
        if (r.pkey != pk) continue;
        Log("[rdr][gpu][groups]   %s: %.0f us mean / %.0f us p90, %zu of %llu frames, %.1f draws a frame, %.4f ms a frame = %.1f %% of the pass", GroupName(uint32_t((r.key >> 56) & 0x3F)), r.mean, r.p90, r.n,
            static_cast<unsigned long long>(pa.frames), double(r.draws) / pf, r.sum / 1e3 / pf, pa.us > 0 ? 100.0 * r.sum / pa.us : 0.0);
      }
    }
    group_table.clear(), group_pass.clear();
    group_frames = group_not_ready = group_chain_broken = group_cover_bad = group_overflow_frames = group_stamps_total = group_sum_bad = 0;
  }
  if (!std::strcmp(why, "summary") && !census_run.empty() && census_frames) {  // S102 G6: the census, ranked by ms a frame (kind: pass / copy / present / tail)
    std::vector<std::pair<uint64_t, CensusRow>> rows(census_run.begin(), census_run.end());
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    static const char* const kinds[] = {"pass", "copy", "present", "tail", "plant", "dispatch", "uploads", "uploads_img"};
    for (const auto& r : rows) {
      const double f = double(census_frames);
      Log("[rdr][gpu][census] class %s kind %s image %u (%ux%u fmt %u): %.4f ms a frame, %.2f steps a frame",
          GpuClassName(uint32_t(r.first >> 40)), kinds[std::min<uint32_t>(uint32_t((r.first >> 32) & 0xFF), 7)], uint32_t(r.first & 0xFFFFFFFFu),
          uint32_t(CensusImageW(uint32_t(r.first & 0xFFFFFFFFu))), uint32_t(CensusImageH(uint32_t(r.first & 0xFFFFFFFFu))), uint32_t(CensusImageFmt(uint32_t(r.first & 0xFFFFFFFFu))),
          r.second.ms / f, double(r.second.steps) / f);
    }
    for (int c = 0; c < kGpuClassCount; ++c)  // S102 G6 (item 7): what the copies copy from, so lanes can rank them
      if (census_copy_src[c].steps)
        Log("[rdr][gpu][census] copies by source class %s: %.4f ms a frame, %.2f steps a frame", GpuClassName(uint32_t(c)), census_copy_src[c].ms / double(census_frames), double(census_copy_src[c].steps) / double(census_frames));
  }
  win_acquire_ns = win_present_ns = win_renders = 0;
  gpu_samples.clear();
  work_samples.clear(), gap_samples.clear();
  role_window = RoleSums{};
  gpu_bad = gpu_not_ready = 0;
}

void Renderer::LogGpuSummary() { impl_->LogGpuWindow("summary"); }
void Renderer::GpuTotals(uint64_t& implausible, uint64_t& not_ready) const { implausible = impl_->gpu_bad_total, not_ready = impl_->gpu_not_ready_total; }
void Renderer::SetImageClass(ImageId image, uint8_t gpu_class) {  // S102 S0-3
  // S102 R21-B (STAGE_SPLIT.md hazard 3): the class steers Record (the colour prepass, the half-size tail, the arena blend-off), so a change made by the bind
  // stage waits for the frame that sealed it (ApplyUploads), never lands under a frame being recorded. Seal 0 (one thread): applied at the next Render, as before.
  Impl& im = *impl_;
  if (image == kNoImage || image >= im.images.high_water()) return;
  if (im.options.gpu_stamp_plant == 11) return;   // S103 GT test plant 11: no image is named (every pass is `other`): the role checks must fail
  std::lock_guard<std::mutex> lock(im.upload_mutex);
  im.class_changes.emplace_back(image, gpu_class < kGpuClassCount ? gpu_class : uint8_t(kGpuOther));
}
Renderer::GpuRoleTotals Renderer::GpuRoles() const {  // S102 S0-3
  const Impl::RoleSums& r = impl_->role_run;
  GpuRoleTotals out;
  for (int c = 0; c < kGpuClassCount; ++c) out.cls_ms[c] = r.frames ? r.cls_ms[c] / double(r.frames) : 0.0;
  out.gap_ms = r.frames ? r.gap_ms / double(r.frames) : 0.0;
  out.blit_ms = r.frames ? r.blit_ms / double(r.frames) : 0.0;  // S102 G2
  for (int c = 0; c < kGpuClassCount; ++c) out.cls_passes_a_frame[c] = r.frames ? double(r.cls_passes[c]) / double(r.frames) : 0.0;
  out.acquire_ms = impl_->run_renders ? double(impl_->run_acquire_ns) / 1e6 / double(impl_->run_renders) : 0.0;
  out.present_call_ms = impl_->run_renders ? double(impl_->run_present_ns) / 1e6 / double(impl_->run_renders) : 0.0;
  out.work_ms = r.frames ? r.work_ms / double(r.frames) : 0.0;
  out.total_ms = r.frames ? r.total_ms / double(r.frames) : 0.0;
  out.frames = r.frames, out.no_pass = r.no_pass, out.no_present = r.no_present, out.overflow = r.overflow;
  out.stamps_a_frame = r.frames ? double(r.stamps) / double(r.frames) : 0.0;
  out.census_frames = r.census_frames, out.steps_recorded = r.steps_rec, out.steps_closed = r.steps_closed;   // S103 GT
  out.ident_frames = r.ident_frames, out.ident_bad = r.ident_bad;
  return out;
}
uint32_t Renderer::UnnamedColourTargets(std::vector<std::string>* what) const {   // S103 GT round 3
  uint32_t n = 0;
  Impl& im = *impl_;
  for (uint32_t id = 1; id < im.images.high_water(); ++id) {
    const ImageEntry& e = im.images[id];
    if (!e.live || e.depth || !(e.desc.usage & kUsageTarget) || e.gpu_class != kGpuOther) continue;
    ++n;
    if (what) what->push_back(std::to_string(id) + " " + std::to_string(e.width) + "x" + std::to_string(e.height) + " fmt " + std::to_string(unsigned(e.desc.format)));
  }
  return n;
}
Renderer::UploadTotals Renderer::UploadRun() const {   // S103 GT
  const Impl::UpSums& u = impl_->up_run;
  UploadTotals t;
  t.steady_frames = u.steady_frames, t.steady_bytes = u.steady_bytes, t.loading_frames = u.load_frames, t.loading_bytes = u.load_bytes, t.max_bytes = u.max_bytes;
  t.frames_with_span = u.gpu_frames, t.gpu_ms_sum = u.gpu_ns / 1e6;
  return t;
}

const FrameCounts& Renderer::LastFrameCounts() const { return impl_->counts_cur; }  // S102 M1
uint32_t Renderer::RingRoom() const { return uint32_t(kRingSection - kOverlayRange - kConstantRange - kDlssConstants - 1024); }   // S103 PVR (the frame_ring_full test in Render adds the section alignments: 1 KB of slack)
Renderer::UploadStats Renderer::LastUploadStats() const {   // S103 UPL
  const Impl& im = *impl_;
  UploadStats s;
  s.images = im.out_up_images, s.buffers = im.out_uploads - im.out_up_images, s.barriers = im.out_up_barriers, s.bytes = im.out_up_bytes, s.batched = im.out_batched;
  for (int pk = 0; pk < 3; ++pk) s.by_producer[pk] = im.out_up_by_producer[pk], s.total_by_producer[pk] = im.up_tot_by_producer[pk];   // S103 PVR
  s.total_frames = im.up_tot_frames, s.total_images = im.up_tot_images, s.total_buffers = im.up_tot_buffers, s.total_barriers = im.up_tot_barriers, s.total_bytes = im.up_tot_bytes;
  return s;
}
void Renderer::SetCountsPlant(int pass) { impl_->counts_plant = pass; }

uint64_t Renderer::LastGpuNs() {  // S101 lane22
  Impl& im = *impl_;
  if (!im.gpu_timestamps || im.last_slot == ~0u) return 0;
  FrameSlot& slot = im.slots[im.last_slot];
  vkWaitForFences(im.device.device(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
  uint64_t ticks[2] = {0, 0};
  if (slot.ts_count < 2) return 0;
  // S102 S0-3: the bracket is query 0 to the LAST query written (the tail), whichever stamp mode the frame ran in.
  if (vkGetQueryPoolResults(im.device.device(), slot.timestamps, 0, 1, sizeof(uint64_t), &ticks[0], sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) return 0;
  if (vkGetQueryPoolResults(im.device.device(), slot.timestamps, slot.ts_count - 1, 1, sizeof(uint64_t), &ticks[1], sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) return 0;
  const uint64_t mask = im.ts_valid_bits >= 64 ? ~0ull : ((1ull << im.ts_valid_bits) - 1);
  return uint64_t(double((ticks[1] - ticks[0]) & mask) * double(im.device.properties().limits.timestampPeriod));
}

void Renderer::WaitIdle() { vkDeviceWaitIdle(impl_->device.device()); }
