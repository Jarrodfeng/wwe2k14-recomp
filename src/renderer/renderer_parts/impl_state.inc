struct Renderer::Impl {
  RendererOptions options;
  Device device;
  Allocator allocator;
  Table<ImageEntry> images;
  Table<BufferEntry> buffers;

  std::vector<FrameSlot> slots;
  bool gpu_timestamps = false;    // S101 lane22: on, and the queue family has valid timestamp bits
  uint32_t last_slot = ~0u;       // S101 lane22: the slot of the last frame recorded
  FrameCounts counts_cur;         // S102 M1: this frame's counts (options.counts); counts_plant >= 0: the planted extra draw's pass
  int counts_plant = -1;
  void CountBarrier(uint32_t images) { if (options.counts) ++counts_cur.total.barriers, counts_cur.total.barrier_images += images; }  // a vkCmdPipelineBarrier outside FlushBarriers
  void CountCopy() { if (options.counts) ++counts_cur.total.copies; }  // one vkCmdCopy* / vkCmdBlit* actually recorded
  CallCounts counts_sum;          // the game's 600-frame log
  uint64_t counts_sum_extra = 0, counts_sum_passes = 0, counts_sum_frames = 0;
  // Lane 269: the per-frame GPU time without a stall. Each slot's two timestamps are read right after the slot's fence wait (which Render does anyway),
  // frames_in_flight frames after they were written, with no WAIT flag. Results land in rdr::g_gpu_* (overlay.h) and a 600-frame log line.
  uint32_t ts_valid_bits = 0;
  double ts_period = 1.0;         // ns a tick (limits.timestampPeriod)
  std::vector<float> gpu_samples; // ms, this log window
  uint64_t gpu_bad = 0, gpu_not_ready = 0, gpu_bad_total = 0, gpu_not_ready_total = 0;  // window / whole run
  void CollectGpuTime(FrameSlot& slot);
  void LogGpuWindow(const char* why);
  // S102 S0-3: per-role sums, a window (the 600-frame line) and the whole run.
  struct RoleSums {
    double cls_ms[kGpuClassCount] = {};
    double gap_ms = 0, work_ms = 0, total_ms = 0;  // gap = the start gap [0] -> [1] (NOT the display wait: see docs\renderer\GPU_STAMPS.md)
    double blit_ms = 0;                            // S102 G2: the cap's blit (inside the 'output' sum)
    uint64_t frames = 0, no_pass = 0, no_present = 0, overflow = 0, stamps = 0;
    uint64_t cls_passes[kGpuClassCount] = {};      // pass steps recorded by class (the role mapping's own count)
    uint64_t steps_rec = 0, steps_closed = 0, census_frames = 0;   // S103 GT: the bracket check (stamp-per-step frames only)
    uint64_t ident_frames = 0, ident_bad = 0;      // S103 GT review: stamped frames / frames whose stamp count broke ts_count == 2 + pre + closes + post + tail (independent of the GPU)
  };
  RoleSums role_window, role_run;
  // S103 GT (the stream-free build's per-frame upload view): a frame that uploaded more than kLoadingBytes is a LOADING frame (a burst: new pictures / geometry arriving), the rest are STEADY.
  // The GPU span of ApplyUploads' commands (the stamp pair around it, needs the stamps on) is kept with them. Window = since the last [rdr][gpu] line, run = the whole run.
  static constexpr uint64_t kLoadingBytes = 1ull << 20;
  struct UpSums {
    uint64_t steady_frames = 0, steady_bytes = 0, load_frames = 0, load_bytes = 0, max_bytes = 0;
    uint64_t image_uploads = 0, buffer_uploads = 0, barriers = 0, up_frames = 0, batched_frames = 0, max_barriers = 0, max_images = 0;   // S103 UPL: the census of WHAT the bytes and the fixed cost are (image copies, buffer copies, barriers recorded by ApplyUploads, frames that uploaded anything)
    double gpu_ns = 0;
    uint64_t gpu_frames = 0;
    uint64_t prod_copies[3] = {0, 0, 0};   // S103 PVR: buffer copies by producer (other / posed model vertices / cloth + CPU-skinned), over the frames that uploaded
    double mid_img_ns = 0, mid_buf_ns = 0;   // S103 PVR: the span split at the mid stamp (image part, buffer part) over mid_frames (stamp-per-step mode only)
    uint64_t mid_frames = 0;
    uint64_t ring_peak = 0;   // S103 PVR: the biggest frame ring use (constants + geometry + previous constants, bytes) of the window
    uint64_t ring_ns = 0, ring_bytes = 0;   // S103 PF2: the host's memcpy of the frame's constants + geometry + previous constants into the mapped ring (out_ring_ns, ring_used_cur), summed over the window's frames
  };
  UpSums up_window, up_run;
  // S103 GT round 3: the route window. One per mark (rdr::GpuWindowMark): the frames RECORDED between two marks, summed as they are collected, logged as `[rdr][gpu][window] NAME:` when the first frame of the next window arrives.
  struct MarkWin {
    uint32_t seq = 0;
    double cls_ns[kGpuClassCount] = {};
    double gap_ns = 0, work_ns = 0, bracket_ns = 0, up_gpu_ns = 0;   // bracket = what the csv gpu_ms sums: [0] .. tail of every frame
    uint64_t frames = 0, steady = 0, loading = 0, steady_bytes = 0, loading_bytes = 0, up_frames = 0;
  };
  MarkWin mark_win;
  void FlushMarkWindow(const char* suffix);
  struct CensusRow { double ms = 0; uint64_t steps = 0; };  // S102 G6: the census's rows, keyed (class << 40 | kind << 32 | image), the whole run
  std::map<uint64_t, CensusRow> census_run;
  CensusRow census_copy_src[kGpuClassCount];  // S102 G6: copies (class copies) by the GPU class of the copy's source image
  uint64_t census_frames = 0;
  std::map<uint64_t, std::vector<float>> pass_table;  // S102 R24-B: --wwe2k14_rdr_gpu_pass_stamps: per (class, kind, occurrence of that image in the frame, image) the step's GPU us samples this window
  uint64_t pass_table_frames = 0;
  // S102 R24-F: the group table. Key: the pass step (class, kind, occurrence on its image, image: as pass_table) + group << 56 + the stencil_dead arm in bit 63.
  struct GroupAcc { std::vector<float> us; uint64_t draws = 0; };
  struct GroupPassAcc { double us = 0, direct_us = 0; uint64_t frames = 0, draws = 0, issued = 0, mixed = 0, segs = 0, stamps = 0; };
  std::map<uint64_t, GroupAcc> group_table;
  std::map<uint64_t, GroupPassAcc> group_pass;   // the same key without the group
  uint64_t group_frames = 0, group_not_ready = 0, group_chain_broken = 0, group_cover_bad = 0, group_overflow_frames = 0, group_stamps_total = 0, group_sum_bad = 0;   // S103 GSTP: group_sum_bad = frames whose groups did not add up to the pass's own span
  void CollectGroups(FrameSlot& slot, const uint64_t* ticks, uint32_t count, uint64_t mask, double frame_ns);
  uint64_t pass_table_frames_arm[2] = {0, 0};   // S102 R24-C review F4: frames per arm (stencil_dead off / on): the table key has the arm in bit 63, so an in-run flip is two columns, not an average
  uint32_t CensusImageW(uint32_t id) { return id && id < images.high_water() ? images[id].width : 0; }  // S102 G6
  uint32_t CensusImageH(uint32_t id) { return id && id < images.high_water() ? images[id].height : 0; }
  uint32_t CensusImageFmt(uint32_t id) { return id && id < images.high_water() ? uint32_t(images[id].desc.format) : 0; }
  std::vector<float> work_samples, gap_samples;   // ms, this window
  uint64_t win_acquire_ns = 0, win_present_ns = 0, win_renders = 0, run_acquire_ns = 0, run_present_ns = 0, run_renders = 0;  // per Render: the swapchain acquire and the Present call
  uint32_t stamp_scratch_cap = 0;                 // test plant 4: the scratch buffer's copies (see RecordStampPlant)
  VkBuffer stamp_scratch = VK_NULL_HANDLE;
  Allocation stamp_scratch_memory;
  uint64_t acquire_ns_frame = 0;                  // the swapchain acquire's time inside this Render (CPU wait, not render-thread work)
  void MakeStampPool(FrameSlot& slot, uint32_t cap);
  void RecordStampPlant(VkCommandBuffer cmd);
  // Lane 232 (3d a2): the no-drain read-back. Persistent host-visible staging per picture level; the copy is recorded into the frame's own command buffer (RecordReadbacks) and handed out when
  // that frame's fence has signalled (PollReadbacks, never waits). The bookkeeping is rdr::ReadbackTracker (host-tested).
  rdr::ReadbackTracker readback;
  std::vector<rdr::ReadbackTracker::Request> readback_taking;
  struct ReadbackStaging {
    VkBuffer buffer = VK_NULL_HANDLE;
    Allocation memory;
    VkDeviceSize bytes = 0;
    uint32_t width = 0, height = 0;
    Format format = Format::RGBA8;
  };
  std::unordered_map<uint64_t, ReadbackStaging> readback_staging;
  static uint64_t ReadbackKey(const rdr::ReadbackTracker::Key& k) { return (uint64_t(k.image) << 16) | (uint64_t(k.mip & 0xFF) << 8) | (k.layer & 0xFF); }
  std::vector<char> readback_recorded;  // parallel to readback_taking
  bool RecordOneReadback(VkCommandBuffer cmd, const rdr::ReadbackTracker::Request& req);
  void RecordReadbacksAfterCopy(VkCommandBuffer cmd, const CopyOp& c);
  void RecordReadbacks(VkCommandBuffer cmd);
  uint64_t serial = 0;

  VkBuffer ring = VK_NULL_HANDLE;
  Allocation ring_memory;

  VkDescriptorSetLayout constants_layout = VK_NULL_HANDLE;
  VkDescriptorSetLayout textures_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorPool constants_pool = VK_NULL_HANDLE;
  VkDescriptorSet constants_set = VK_NULL_HANDLE;
  VkPipelineCache pipeline_cache = VK_NULL_HANDLE;

  // ---- S102 lane L1b (light_cubes.h): the light cubes' compute pipeline, made on the first dispatch (lc_state 0 untried, 1 ready, 2 failed). One descriptor set per frame slot. Render thread only. ----
  int lc_state = 0;
  VkDescriptorSetLayout lc_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout lc_layout = VK_NULL_HANDLE;
  VkShaderModule lc_module = VK_NULL_HANDLE, lc_dirs_module = VK_NULL_HANDLE;
  VkPipeline lc_pipeline = VK_NULL_HANDLE, lc_dirs_pipeline = VK_NULL_HANDLE, lc_dirs_pipeline_swap = VK_NULL_HANDLE;
  ImageId lc_table = kNoImage;      // review F7: the direction table (64x64x6 RGBA32F), made again only when the texel -> grid mapping changes
  float lc_table_k[4] = {};
  bool lc_table_valid = false, lc_table_swap = false;
  uint64_t lc_table_builds = 0;
  VkDescriptorPool lc_pool = VK_NULL_HANDLE;
  std::vector<VkDescriptorSet> lc_sets;
  bool LightCubesInit();
  void RecordLightCubes(VkCommandBuffer cmd, const DispatchOp& op, uint32_t slot_index, VkDeviceSize ring_base);
  ImageId dummy_image = kNoImage;
  ImageId dummy_cube = kNoImage;
  ImageId dummy_volume = kNoImage;
  VkSampler dummy_sampler = VK_NULL_HANDLE;
  bool filterable[uint32_t(Format::Count)] = {};
  uint32_t pipeline_delay_ms = 0;  // RendererOptions::pipeline_delay_ms (test only)
  uint32_t bc_mode = 1;       // S102 U1: RendererOptions::bc_decode (0 off, 1 auto: only where the device cannot sample BC, 2 force)
  std::atomic<uint64_t> bc_decoded_levels{0}, bc_images{0}, bc_decoded_bytes{0};
  std::atomic<uint64_t> frames_dropped{0};  // S103 CB1: Renderer::FramesDropped
  std::atomic<size_t> render_thread{0};  // S102 U1: hash of the thread that calls Render (WaitForStagingRoom must never wait on it: only its own DrainRetired frees staging)
  uint64_t max_staged = kMaxStagedBytes;  // dev tests may shrink it (RendererOptions::staging_cap_mb)
  bool staging_wait_plant = false;        // dev only: the pre-fix behaviour (the render thread waits too), the planted fault of the stall test  // S102 U1: counters for the verdict / log
  bool caps_fallback = true;  // format_caps.h: false only under kCapFallbacksOff (the re-find)
  CapStats cap_stats;
  VkSampler present_sampler_nearest = VK_NULL_HANDLE;  // lane 200: the present of a source that cannot filter
  // Linear filtering of a renderer Format, from the table made once at Init (no per-call driver query).
  bool CanFilter(Format format) const { return filterable[uint32_t(format)]; }
  std::unordered_set<uint64_t> caps_logged;  // (kind, program, format) already said: each is logged once, however often pipelines are rebuilt
  std::chrono::steady_clock::time_point caps_summary_at{};
  bool caps_summary_dirty = false;
  void CapsSummary() {
    caps_summary_dirty = false;
    // render-clock: perf - log throttle stamp for the format caps summary line
    caps_summary_at = std::chrono::steady_clock::now();
    Log("[rdr] format caps summary: linear->nearest slots %llu, blits->nearest %llu, blend off %llu, vertex unsupported %llu; illegal (fallbacks off) linear %llu, blend %llu",
        static_cast<unsigned long long>(cap_stats.linear_to_nearest), static_cast<unsigned long long>(cap_stats.blits_to_nearest), static_cast<unsigned long long>(cap_stats.blend_disabled),
        static_cast<unsigned long long>(cap_stats.vertex_unsupported), static_cast<unsigned long long>(cap_stats.illegal_linear), static_cast<unsigned long long>(cap_stats.illegal_blend));
  }
  // Review SF1: a counter changed. The summary is written the first time and then at most every 10 s (a phone or a bug-button zip rarely reaches Shutdown).
  void CapsChanged() {
    caps_summary_dirty = true;
    // render-clock: perf - log throttle: the caps summary prints at most every 10 s
    if (caps_summary_at == std::chrono::steady_clock::time_point{} || std::chrono::steady_clock::now() - caps_summary_at >= std::chrono::seconds(10)) CapsSummary();
  }
  bool FirstTime(uint64_t kind, uint64_t a, uint64_t b) { return caps_logged.insert((kind << 56) ^ (a << 24) ^ b).second; }
  VkFilter BlitFilterFor(Format source, bool want_linear) {
    if (!want_linear) return VK_FILTER_NEAREST;
    if (CanFilter(source)) return VK_FILTER_LINEAR;
    if (!caps_fallback) {
      ++cap_stats.illegal_linear;
      CapsChanged();
      return VK_FILTER_LINEAR;
    }
    ++cap_stats.blits_to_nearest;
    if (FirstTime(1, uint64_t(source), 0)) Log("[rdr] format caps: a linear blit from format %u (%s) is made nearest", unsigned(source), CapFormatName(ToVk(source, device.caps())));
    CapsChanged();
    return VK_FILTER_NEAREST;
  }
  VkSampler PresentSamplerFor(const ImageEntry& source) const { return CanFilter(source.desc.format) || !caps_fallback ? present_sampler : present_sampler_nearest; }

  Swapchain swapchain;
  bool windowed = false;
  bool offscreen_out = false;
  uint32_t colour_cuts_ = 0;    // S102 G10: this frame's colour-pass cuts, from the Frame (record time)
  uint32_t arena_fragments_ = 0;  // S102 R18-B3: this frame's arena_fragments.h mask, from the Frame (record time)
  std::vector<std::pair<VkPipeline, uint8_t>> af_pipes;  // S102 R18-B3: per state index, valid for the pass being recorded: (blend-off pipeline, 0 unset / 1 taken / 2 cannot drop)
  std::vector<VkPipeline> cut_pre_pipes, cut_main_pipes;  // S102 G10: per state index, valid for the pass being recorded
  uint32_t out_cap_h_ = 0;      // S102 G2: this frame's output levers, from the Frame (record time); the render thread reads these, never the live switch
  bool out_bilinear_ = false;
  bool present_capped_ = false;  // S102 G2: this frame's present drew at the capped size (set in RecordPresent, read by the submit)  // S102 G2: no window, the present draws into offscreen images (rplay --present-size)
  ImageId display_curve = kNoImage;
  VkSampler present_sampler = VK_NULL_HANDLE;
  uint16_t present_program = 0;
  uint16_t present_sgsr_program = 0;  // settings parity S10a
  uint16_t present_fetch_program = 0;  // S102 R24-A: rdr_present_fetch (the 1:1 present: one texelFetch, no filter)
  bool present_fetch_ = false;        // S102 R24-A: this frame's wwe2k14_rdr_present_fetch, from the Frame (record time)
  // Settings parity S11: the app's overlay.
  uint16_t overlay_program = 0, overlay_layout = 0;
  std::unordered_map<uint32_t, ImageId> overlay_textures;
  std::shared_ptr<const OverlayList> overlay;
  void RecordOverlay(VkCommandBuffer cmd, FrameSlot& slot, VkDeviceSize ring_base, uint32_t present_class);
  // Settings parity S10b: DLSS.
  Renderer* self = nullptr;
  const DlssInputs* frame_dlss = nullptr;  // the frame being recorded's
  uint16_t dlss_motion_program = 0, present_dlss_program = 0;
  uint16_t half_depth_program = 0, half_composite_program = 0;  // S102 C4: the half-size transparents' two internal programs
  ImageId half_color = kNoImage, half_depth_img = kNoImage;      // S102 C4: the half-size target and depth (made again when the size or a format changes)
  uint32_t half_w = 0, half_h = 0;
  ImageId dlss_motion = kNoImage, dlss_out = kNoImage;
  uint32_t dlss_motion_w = 0, dlss_motion_h = 0, dlss_out_w = 0, dlss_out_h = 0;
  uint64_t dlss_count = 0;
  uint64_t last_desired_ns = 0;  // S100: the last desired present time (display timing)
  VkDeviceSize prev_constants_base = 0;  // S10b: where this frame's prev_constants went in the ring
  bool dlss_had_history = false;
  bool RecordDlss(VkCommandBuffer cmd, FrameSlot& slot, VkDeviceSize ring_base, uint32_t out_w, uint32_t out_h);
  uint16_t present_layout = 0;

  StableTable<Program> programs;  // S102 R21-B (STAGE_SPLIT.md hazard 1): append-only, readers by id need no lock; appends under tables_mutex
  VkQueryPool ps_pool = VK_NULL_HANDLE;  // S102 G10: pipeline-statistics queries of the census (vertex + fragment invocations per run of same-program draws)
  uint32_t ps_cap = 0, ps_count = 0;
  std::vector<std::string> ps_names;
  std::vector<uint8_t> ps_afoff;  // S102 R18-B3: parallel to ps_meta: 1 = the draw ran with its blend switched off (the per-draw log's trailing "afoff" field)
  std::vector<std::string> ps_meta;  // S102 C4: the per-draw log's line start for each query (rplay --draw-log); empty otherwise
  std::vector<uint8_t> draw_mod_match;  // S102 G10: per program, 1 = rplay --draw-mod names it
  uint32_t draw_mod_version = ~0u;
  StableTable<VertexLayoutDesc> layouts;  // S102 R21-B: same
  // S102 R21-B (hazards 1, 2): one writer at a time for programs / layouts and the pipeline store's program / layout maps (program_by_hash, program_info,
  // layout_by_hash, hashing_program, store_retry, store_catalog). Held for one add (a shader module) or one map lookup, never across a draw or a pipeline build.
  std::mutex tables_mutex;

  std::unordered_map<RenderPassKey, VkRenderPass, BytesHash, BytesEqual> render_passes;
  std::unordered_map<PassClassKey, uint32_t, BytesHash, BytesEqual> pass_classes;
  std::vector<VkRenderPass> class_passes;
  std::vector<PassClassKey> class_keys;
  std::unordered_map<FramebufferKey, VkFramebuffer, BytesHash, BytesEqual> framebuffers;
  struct PipelineEntry {
    VkPipeline pipeline = VK_NULL_HANDLE;
    int32_t unused_record = -1;  // lane 276: >= 0 = made from the list and not drawn yet this session (the record to stamp at its first draw)
  };
  std::unordered_map<PipelineKey, PipelineEntry, BytesHash, BytesEqual> pipelines;
  std::unordered_map<SamplerDesc, VkSampler, BytesHash, BytesEqual> samplers;

  // ---- lane 276: the pipeline store (pipeline_store.h; SKIP_GAME_WORK_MAP 19.10). All of it is inert while store_on is false. Render thread only. ----
  struct ShaderBlob {
    const uint32_t* words = nullptr;
    size_t bytes = 0;
    const char* name = nullptr;
  };
  struct ProgramInfo {
    uint64_t vhash = 0, fhash = 0;
  };
  struct StoreStats {
    uint64_t bad_recorded = 0, bad_dropped = 0, on_demand = 0, on_demand_ns = 0, prebuilt = 0, prebuilt_ns = 0, prebuilt_used = 0, prebuilt_failed = 0, unresolved_peak = 0;
    uint64_t list_loaded = 0, list_saved = 0, cache_loaded_bytes = 0, cache_saved_bytes = 0, slices = 0, slice_ns = 0, slice_max_ns = 0;
  };
  bool store_on = false;
  std::filesystem::path store_dir;
  pstore::DeviceId store_device;
  std::vector<pstore::Record> store_records;
  uint32_t store_seed_added = 0;                                  // S106 PSEEDF2 N9: seed records merged at boot (pending): StoreNoteBuilt's cap leaves room for them
  std::unordered_map<uint64_t, uint32_t> store_index;             // RecordKey -> record
  std::vector<uint32_t> store_pending;                            // record indices waiting for their prebuild, in the order they were first needed
  size_t store_cursor = 0;
  std::vector<uint32_t> store_unresolved;                         // waiting for a program (the catalog or an AddProgram) that does not exist yet
  bool store_retry = false;                                       // a program or the catalog was added since: try the unresolved again
  std::unordered_map<uint64_t, ShaderBlob> store_catalog;         // SPIR-V hash -> the blob (static arrays owned by the caller)
  std::vector<ProgramInfo> program_info;                          // parallel to programs (filled while store_on)
  std::unordered_map<uint64_t, uint16_t> program_by_hash;         // (vhash, fhash) -> program id
  std::unordered_map<uint64_t, uint16_t> layout_by_hash;          // pstore::HashLayout -> layout id
  ProgramInfo hashing_program;                                    // the hashes StoreFindProgram just made, for StoreProgramAdded
  uint32_t store_session = 1;                                     // the session number this process stamps (the list's + 1)
  bool store_list_dirty = false, store_cache_dirty = false, building_prebuilt = false;
  std::chrono::steady_clock::time_point store_saved_at{};
  uint64_t store_used = 0;                                        // pipelines built or first-used this session (a "substantial" session has >= 20)
  uint32_t store_slice_us = 2000;
  uint32_t store_caps_mask = 0;                                   // RendererOptions::format_caps_mask, written in the list header
  uint32_t store_save_interval_ms = 30000;
  bool prebuild_allowed = true;                                   // false while a scene is live (SetPipelinePrebuildAllowed): no slices then
  std::thread store_worker;                                       // the save's serialise + hash + write, one in flight
  std::atomic<bool> store_worker_busy{false}, store_redo_list{false}, store_redo_cache{false};
  std::atomic<uint64_t> store_saved_list{0}, store_saved_cache{0};
  void StoreJoin() { if (store_worker.joinable()) store_worker.join(); }
  StoreStats store_stats;
  uint64_t last_build_ns = 0;                                     // the vkCreateGraphicsPipelines time of the last BuildPipeline
  std::string store_cache_refusal;
  std::deque<std::string> store_names;                            // stable names of the programs the store made itself
  void StoreInit(const RendererOptions& opts, std::vector<uint8_t>& cache_blob);
  void StoreNoteBuilt(const DrawState& state, uint32_t pass_class, int64_t build_ns);
  bool StoreFindProgram(const ProgramDesc& desc, uint16_t& id);   // store on: the program made from these same SPIR-V words, if there is one (else remembers the hashes for StoreProgramAdded)
  void StoreProgramAdded(uint16_t id);
  void PrebuildSlice(uint64_t budget_ns, bool all);
  // S105 B3: what the store has left, for a thread that is not the render thread (the host's precompile rule): written only by StorePublish on the render thread.
  std::atomic<uint64_t> store_pub_left{0}, store_pub_unresolved{0}, store_pub_total{0}, store_pub_made{0};
  std::atomic<bool> store_pub_valid{false};
  bool store_pub_dirty = true;   // render thread only: a count moved since the last StorePublish (F5)
  void StorePublish();
  bool PrebuildOne(uint32_t index);
  void StoreSave(bool final);
  void StoreMaybeSave();
  void StoreSummary(const char* when);

  std::mutex upload_mutex;
  // S102 R21-B (hazard 4): sealed upload batches. The bind stage seals what is pending when it hands a frame over (SealUploads); that frame's Render
  // takes only the batches sealed up to its Frame::upload_seal, so frame N never applies frame N+1's re-uploads. Seal 0 (every frame off the split)
  // takes every sealed batch and everything pending, as before. Guarded by upload_mutex.
  struct UploadBatch {
    uint64_t seal = 0;
    std::vector<ImageId> fresh;
    std::vector<ImageUpload> images;
    std::vector<BufferUpload> buffers;
    std::vector<std::pair<ImageId, uint8_t>> classes;  // S102 R21-B (hazard 3): SetImageClass changes, applied with the frame that sealed them
  };
  std::deque<UploadBatch> sealed_batches;
  uint64_t next_seal = 0;
  uint64_t upload_seal_now = 0;  // Render: this frame's seal (record thread)
  std::atomic<size_t> producer_thread{0};  // S102 R21-B (hazard 6): the bind stage's thread hash: it never waits for staging room (only Render frees it)
  std::vector<ImageId> new_images;
  std::vector<std::pair<ImageId, uint8_t>> class_changes;  // S102 R21-B (hazard 3): pending SetImageClass (upload_mutex); the class steers what Record does (prepass, half tail, blend-off), so it changes at a frame boundary
  std::vector<ImageUpload> image_uploads;
  std::vector<BufferUpload> buffer_uploads;
  std::atomic<uint64_t> staged_bytes{0};
  std::condition_variable staging_drained;
  std::mutex destroy_mutex;
  std::vector<PendingDestroy> destroy_queue;

  // S102 D1 (breadcrumbs.h, docs\renderer\DEVICE_LOST.md): the ring of the last frames' steps, the marker words, the lost-device handler.
  crumbs::Ring crumb_ring;
  crumbs::Frame* crumb = nullptr;       // this Render's record (null: breadcrumbs off this frame)
  crumbs::Step* crumb_step = nullptr;   // the step being recorded (RecordPass fills its counts)
  VkBuffer marker_buffer = VK_NULL_HANDLE;
  Allocation marker_memory;
  bool marker_failed = false;           // the buffer could not be made: markers stay off for the run
  uint32_t marker_next = 1;             // the next marker of this frame (1 = the uploads)
  uint32_t marker_open = 0;             // the marker whose END is not written yet (0: none)
  int frame_marker = 0;                 // this frame's marker mode (0: no marker words)
  int frame_barrier_mode = 0;           // S102 D1 round 6: this frame's wwe2k14_rdr_frame_top_barrier
  bool frame_barrier_end_done = false;  // mode 2: the barrier before the present was recorded
  void RecordFrameBarrier(VkCommandBuffer cmd, const char* where);
  bool plant5_done = false;             // rplay --crumbs-plant 5 (the GPU-AV vertex-fetch twin)
  uint32_t last_destroys = 0;           // ProcessDestroys: items made ready this Render
  uint64_t lost_serial = 0;             // serial + 1 of the first sign of a lost device (0: fine)
  VkEvent selftest_event = VK_NULL_HANDLE;  // rplay --crumbs-selftest: the stall
  bool EnsureMarker();
  uint16_t MarkBegin(VkCommandBuffer cmd, uint32_t slot_index);   // closes the open marker (its END), writes the next one's BEGIN
  void MarkClose(VkCommandBuffer cmd, uint32_t slot_index);         // the open marker's END: barrier, word, barrier
  bool idle_after_loss = false;         // S105 A9 review F3: the one vkDeviceWaitIdle after a real loss has been done
  bool lost_simulated = false;         // the loss is rplay's selftest (the device is fine: keep submitting)
  void OnDeviceLost(const char* where, int result, bool simulated);
  void DumpCrumbs(bool simulated);

  // Per-frame working storage, reused so a frame allocates nothing once warm.
  std::vector<PassPlan> passes;
  std::vector<Step> steps;
  std::vector<SampledImage> sampled;
  std::vector<uint8_t> next_use;
  std::vector<uint8_t> stencil_needed;
  bool sd_on = false;                                // S102 R24-C: wwe2k14_rdr_stencil_dead, read once per frame at Compile
  std::vector<const CopyOp*> depth_only_copies;      // S102 R24-C: depth copies of this frame that take the depth aspect only (the stencil is dead at their destination)
  std::deque<TargetsOp> trimmed_targets;  // a pass's targets without the depth it never uses (Compile; stable addresses)
  std::vector<uint8_t> state_needed;
  std::vector<uint8_t> clear_absorbed;
  std::vector<uint32_t> image_stamp;
  std::vector<uint32_t> sampled_index;
  std::vector<uint32_t> written_stamp;
  uint32_t stamp = 0;
  uint32_t write_stamp = 0;
  uint32_t last_persistent_count = ~0u;
  struct PooledStaging { VkBuffer buffer = VK_NULL_HANDLE; Allocation memory; };
  rdr::lean::StagingPool<PooledStaging> staging_pool;   // S102 RT1 (bit 4)
  std::vector<VkDescriptorSet> frame_sets;
  // S102 RT1 (render_lean bit 1, Q5): texture sets kept across frames, keyed by the views and samplers they name. They live in their own pool (never reset), are freed only
  // when an image they name is destroyed (ProcessDestroys: the destroy is gated until no frame in flight can name it) or when the cache is full and they were idle.
  VkDescriptorPool lean_pool = VK_NULL_HANDLE;
  rdr::lean::SetCache<size_t(kMaxTextures), VkDescriptorSet> lean_cache{2048};
  std::vector<uint64_t> lean_dying;
  std::vector<VkDescriptorSet> lean_freed;
  uint64_t lean_pool_made_fail = 0;
  void FillTextureInfos(const Frame& frame, uint32_t index, VkDescriptorImageInfo* infos);
  VkDescriptorSet GetTextureSetLean(const Frame& frame, uint32_t index, FrameSlot& slot);
  void LeanCensus();
  void LeanEvictDying(FrameSlot& slot);   // the kept sets that name the views in lean_dying go (retired into this slot)
  void LeanTool(FrameSlot& slot, const Frame& frame);   // tools only (rplay --lean-churn / audit): a view replaced under the cache; the audit of the cache against the live views
  std::vector<std::pair<VkImageView, uint64_t>> lean_churned;   // views replaced by the tool, destroyed once no frame in flight can name them
  std::vector<VkImageMemoryBarrier> barriers;
  VkPipelineStageFlags barrier_src = 0, barrier_dst = 0;
  std::vector<std::pair<VkPipeline, uint32_t>> state_pipelines;  // last pipeline per state index, with its pass class

  float scale = 1.0f;

  // ---- setup ----
  bool Init(const RendererOptions& opts, std::string& error);
  void Shutdown();

  // ---- resources ----
  VkImageView TargetView(ImageEntry& entry, uint32_t mip, uint32_t layer, uint32_t views = 1);   // S102 C1: views > 1 = a 2D-array view of that many layers
  VkImageView SampledView(ImageEntry& entry, const TextureSlot& slot);
  void DrainRetired(FrameSlot& slot);
  void ProcessDestroys(FrameSlot& slot);
  void WaitForStagingRoom();
  ImageId DummyFor(uint8_t kind) const { return kind == 1 ? dummy_cube : kind == 2 ? dummy_volume : dummy_image; }
  void ApplyUploads(VkCommandBuffer cmd, FrameSlot& slot);
  // S102 G5 (docs\renderer\GPU_OUTSIDE.md): the points outside our command buffers. out_window prints every 600 frames (meter on), out_run is the whole run (rplay's line).
  OutsideSums out_window, out_run;
  std::atomic<uint32_t> host_buffers{0};      // S103 PF2: host buffers made (Render scans the draws for them only when there are any)
  std::atomic<uint64_t> direct_overruns{0};   // S103 PF2: frames rendered later than their Record promised
  uint64_t out_collect_ns = 0, out_ring_ns = 0, out_end_ns = 0, out_begin_ns = 0, out_pipe_ns = 0, out_pipe_n = 0;  // this Render's own timers (the rest come from g_render_times' deltas)
  uint64_t out_flushes = 0;                                                                      // FlushBarriers calls that recorded a barrier, whole run
  uint32_t out_uploads = 0, out_up_images = 0, out_scaled = 0, out_up_barriers = 0, out_up_explicit = 0;                                   // this Render's ApplyUploads: uploads, scaled ones (temp image + blit), barriers recorded
  uint64_t out_up_bytes = 0;
  uint64_t ring_used_cur = 0;   // S103 PVR: this Render's ring use (constants + geometry + previous constants)
  uint32_t out_up_by_producer[3] = {0, 0, 0};   // S103 PVR: this Render's buffer copies by producer (0 other, 1 posed model vertices, 2 cloth / CPU-skinned pieces)
  uint64_t up_tot_frames = 0, up_tot_images = 0, up_tot_buffers = 0, up_tot_barriers = 0, up_tot_bytes = 0, up_tot_by_producer[3] = {0, 0, 0};   // S103 UPL (+ PVR by producer): cumulative (LastUploadStats; always counted: a few adds a frame)
  bool out_batched = false;
  void AddOutside(const OutsideSums& frame);

  // ---- frame ----
  void Render(const Frame& frame, RenderMode mode);
  void Compile(const Frame& frame);
  void Transition(ImageEntry& entry, uint32_t first_sub, uint32_t sub_count, VkImageLayout layout, VkPipelineStageFlags stage, VkAccessFlags access, bool discard, bool ranged = false);   // S102 C1: ranged = one barrier for the contiguous layers when their states agree
  const char* LayeredTargetsProblem(const TargetsOp& t);   // S102 C1 review F8: null when the targets are well formed
  void FlushBarriers(VkCommandBuffer cmd);
  void RecordPass(VkCommandBuffer cmd, const Frame& frame, const PassPlan& pass, FrameSlot& slot, VkDeviceSize ring_base);
  struct HalfPrep {  // S102 C4: what the half-size tail needs, made (and checked) before the pass's first render pass begins, so a failure declines the tail instead of dropping it
    VkPipeline depth_pipe = VK_NULL_HANDLE, comp_pipe = VK_NULL_HANDLE;
    DrawState depth_state{}, comp_state{};   // S111 DYNSTATE: the states the two pipelines were asked with (RecordHalfTail sets what they no longer bake)
    RenderPassKey depth_key{}, half_key{}, comp_key{};
    PassClassKey depth_class{}, comp_class{};
    uint32_t hw = 0, hh = 0;
    VkDescriptorSet depth_set = VK_NULL_HANDLE, comp_set = VK_NULL_HANDLE;  // review H1: both sets are allocated (and written) in PrepareHalf: a failure declines the tail, it never drops it
  };
  // S111 DYNSTATE (dyn_state.h): the draw state a pipeline no longer bakes is set per draw. Record thread only.
  std::vector<uint8_t> class_blend_ok;   // per pass class: 0x80 (asked) | bit c = colour attachment c's format can blend
  uint32_t BlendOkOfClass(uint32_t pass_class) {
    if (pass_class >= class_blend_ok.size()) class_blend_ok.resize(size_t(pass_class) + 1, 0);
    if (!class_blend_ok[pass_class]) {
      const PassClassKey& ck = class_keys[pass_class];
      uint8_t ok = 0x80;
      for (uint32_t c = 0; c < ck.color_count && c < uint32_t(kMaxColorTargets); ++c)
        if (ck.formats[c] == VK_FORMAT_UNDEFINED || device.FormatHas(VkFormat(ck.formats[c]), VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) ok = uint8_t(ok | (1u << c));
      class_blend_ok[pass_class] = ok;
    }
    return class_blend_ok[pass_class] & 0xFu;
  }
  // After every graphics pipeline bind, and for every draw of a pass: `state` is the state this draw really runs with (a twin's own, not the draw op's).
  void DynApply(VkCommandBuffer cmd, dyn::Applied& last, const DrawState& state, uint32_t pass_class, dyn::Counts& n) {
    const uint32_t mask = device.caps().dyn_mask;
    if (!mask) return;
    dyn::VkSink sink{cmd};
    dyn::Apply(sink, last, state, mask, class_keys[pass_class].color_count, (mask & dyn::kBlend) ? BlendOkOfClass(pass_class) : 0xFu, caps_fallback, n);
  }
  // The format-caps counts of a pass whose blend was set per draw: once per pass class (the baked path counts once per pipeline attachment, in BuildPipeline).
  void DynNote(const dyn::Counts& n, uint32_t pass_class) {
    if (n.blend_masked && FirstTime(4, pass_class, 0)) {
      ++cap_stats.blend_disabled;
      Log("[rdr] format caps: blend off (set per draw) on a colour attachment of pass class %u that cannot blend", pass_class);
      CapsChanged();
    }
    if (n.blend_illegal && FirstTime(5, pass_class, 0)) {
      ++cap_stats.illegal_blend;
      CapsChanged();
    }
  }
  bool PrepareHalf(const PassPlan& pass, FrameSlot& slot, HalfPrep& prep);
  VkDescriptorSet MakeHalfSet(FrameSlot& slot, ImageEntry* a, ImageEntry* b, ImageEntry* c);
  void RecordHalfTail(VkCommandBuffer cmd, const Frame& frame, const PassPlan& pass, FrameSlot& slot, VkDeviceSize ring_base, const std::vector<uint8_t>& half_skip, uint32_t pass_class, float factor_x, float factor_y, const HalfPrep& prep, CallCounts& pc);
  void RecordCopy(VkCommandBuffer cmd, const CopyOp& copy);
  // S102 G12: where a copy reads and writes, computed once for RecordCopy and for the batch rule (path 0 = unsupported, 1 = vkCmdCopyImage, 2 = vkCmdBlitImage)
  struct CopyGeom {
    ImageEntry* source = nullptr;
    ImageEntry* dest = nullptr;
    uint32_t source_sub = 0, dest_sub = 0, source_w = 0, source_h = 0, dest_w = 0, dest_h = 0;
    int32_t source_x = 0, source_y = 0, dest_x = 0, dest_y = 0;
    bool whole = false;
    bool depth_only = false;   // S102 R24-C: the copy names the depth aspect only (stencil_dead)
    int path = 0;
  };
  CopyGeom GeometryOf(const CopyOp& copy);
  static VkImageCopy RegionOf(const CopyOp& copy, const CopyGeom& g);
  void RecordCopyBatch(VkCommandBuffer cmd, const CopyOp* const* copies, const CopyGeom* geoms, uint32_t count);
  bool RecordPresent(VkCommandBuffer cmd, const PresentOp& present, FrameSlot& slot, VkDeviceSize ring_base, uint32_t& image_index);

  VkRenderPass GetRenderPass(const RenderPassKey& key);
  uint32_t GetPassClass(const PassClassKey& key);
  VkFramebuffer GetFramebuffer(const FramebufferKey& key);
  VkPipeline GetPipeline(const DrawState& state, uint32_t pass_class);
  VkPipeline BuildPipeline(const DrawState& state, uint32_t pass_class);
  VkSampler GetSampler(const SamplerDesc& desc);
  VkDescriptorSet GetTextureSet(const Frame& frame, uint32_t index, FrameSlot& slot);
  void WriteTextureSet(VkDescriptorSet set, const VkDescriptorImageInfo* infos);  // infos: kMaxTextures, pixel slots then vertex slots

  uint32_t ScaledExtent(uint32_t value, const ImageDesc& desc) const {
    if (!(desc.usage & kUsageScaled)) return value;
    return uint32_t(std::ceil(float(value) * scale));
  }
};
