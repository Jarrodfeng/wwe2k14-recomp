  bool plane_on_ = false;
  uint32_t plane_first_op_ = 0, plane_overlap_ops_ = 0;
  std::vector<PlanePassOps> plane_passes_;
  std::vector<uint32_t> plane_draw_ops_;
  std::vector<uint8_t> plane_drop_;
  // S101 lane 41: the in-place replace hooks (registration order), whether any is on this frame, and the scratch of the draw being replaced.
  std::vector<ReplaceSlot> replace_slots_;
  bool replace_active_ = false;
  ReplaceOut replace_out_;
  uint8_t replace_block_[8192];       // the replaced draw's packed vertex block (rows applied), pushed by BindDraw
  uint32_t replace_block_bytes_ = 0;
  std::unique_ptr<uint8_t[]> replace_fs_block_;   // lane 151: the claimed draw's packed pixel block (kMaxFragmentBlockBytes), made lazily on the first hook that asks (DrawReplaceHook::NeedsFragmentBlock)
  uint32_t replace_fs_block_bytes_ = 0;
  uint32_t test_fs_capacity_ = 0xFFFFFFFFu;   // lane 151 (TEST ONLY): a smaller capacity for the pixel block buffer: the pack must refuse, not write
  uint32_t test_override_from_ = 0, test_override_to_ = 0;   // lane 151 (TEST ONLY): capture image ids
  // lane 122: D1 thin draws
  thin::PinPublisher* thin_pub_ = &thin::g_pins;
  thin::ThinStore thin_store_{&thin::g_pins};
  thin::ThinCounters thin_counters_[uint8_t(thin::Cls::Count)];  // lane 129 (KC): per class
  uint64_t thin_hole_c_[4] = {0, 0, 0, 0}, thin_epoch_drop_c_[4] = {0, 0, 0, 0};   // review F2: per class, [3] = a key never learned
  uint32_t thin_epoch_c_[3] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu}, thin_epoch_gen_c_[3] = {0, 0, 0}, thin_rel_seq_[3] = {0, 0, 0};
  // Lane 202 review SF2: these arrays cover the three thin classes (arena, ring, wrestlers) only, and index 3 of the hole arrays above
  // is the "key never learned" bucket. Cls::Casters (= 3) has no thin draws (ClsOn false, ThinHookSlot null). A lane that gives it
  // thin draws must size these by Cls::Count and move the never-learned bucket to its own index first.
  static_assert(uint8_t(thin::Cls::Count) == 4, "thin class arrays: see the lane 202 note above before adding a class");
  std::unordered_map<uint64_t, uint8_t> thin_key_cls_;          // key hash -> the class that learned it
  uint64_t thin_holes_ = 0, thin_epoch_drops_ = 0;               // a lite draw with no template has no known class: counted once, printed on every class's line
  std::unordered_map<uint32_t, thin::ThinTemplate> thin_wrestler_last_;  // PLANT cloth_pinned: the last wrestler template learned per object (borrowed for a draw of it the hook kept)
  std::unordered_map<uint64_t, rdr::ImageId> thin_rope0_;        // PLANT rope_colour_instance0: ordinal 0's stream slot-0 image per (site, object, part)
  thin::Fault thin_fault_ = thin::Fault::None;
  thin::FrameLite thin_lite_;                           // fview's reader emulation
  std::vector<std::shared_ptr<const thin::PinSet>> thin_hist_;
  struct EmuFrame { ThinFrameStamp stamp; std::vector<ThinDrawFacts> facts; };   // lane 149: what the emulation gave ONE loaded frame (its stamp and facts table); keyed by the frame object
  std::unordered_map<const LoadedFrame*, EmuFrame> thin_emu_;
  std::vector<uint64_t> thin_keys_;                     // this frame: key hash per draw (0 = not a candidate)
  d3::D3Core d3_;                                       // lane 138 (D3 skip the object)
  std::unordered_map<uint32_t, thin::ThinKey> d3_cut_all_;   // lane 274 (S2): the draws the offline emulations (arena + ring) cut, for ThinBeginFrame's ordinals
  std::unique_ptr<d3::RingChain> rc_;                   // lane 229 (Ring D3 R3): the ring class's self-contained chain (d3_binder.h RingChain), created by SetD3RingOffline; null until then
  bool d3r_on_ = false;
  std::unique_ptr<d3::CasterChain> cc_;                // lane 217 (E2b): the caster class's self-contained chain (d3_binder.h CasterChain), created by SetD3CasterOffline; null until then
  bool d3c_on_ = false;
  std::unique_ptr<d3::WrestlerChain> wc_;               // lane 227 (H2): the wrestler class's self-contained chain (d3_binder.h WrestlerChain), created by SetD3WrestlersOffline; null until then
  bool wr_on_ = false;
  bool d3_latched_ = false;                             // lane 152 (B2)
  uint64_t d3_skipped_frames_ = 0, d3_evict_seen_ = 0;
  std::string d3_latch_msg_;
  std::string d3c_latch_msg_;      // lane 228
  const LoadedFrame* d3c_real_frame_ = nullptr;   // lane 228: set while a caster marker's mini frame is bound (TryReplace hands it to the hooks)
  bool d3c_latched_ = false;       // lane 228
  std::string d3w_latch_msg_;      // lane 245
  bool d3w_latched_ = false;       // lane 245
  bool d3w_pick_adopted_ = false;  // lane 245 (reviewer SF1): the sphere stamp was settled by the census (once)
  d3::D3Core::Mini d3_mini_;
  d3::PacketTable d3_own_packets_{4096};
  d3::MarkPublisher d3_own_marks_;
  d3::Bank d3_pass_vs_{}, d3_pass_ps_{};
  std::vector<uint8_t> d3_has_vs_, d3_has_ps_;
  d3::D3Counters d3_base_;
  bool d3_on_ = false, d3_pass_ready_ = false, d3_offline_ = false;
  const std::unordered_map<uint64_t, const thin::ThinTemplate*>* d3_overlay_ = nullptr;
  std::vector<thin::ThinKey> thin_full_;
  bool thin_frame_on_ = false;
  uint64_t thin_frame_no_ = 0;
  uint64_t thin_bound_parity_[2] = {0, 0};   // S102 K4
  uint32_t thin_stamp_ = 0, thin_epoch_ = 0xFFFFFFFFu, thin_epoch_gen_ = 0, thin_proof_k_ = thin::kProofK, thin_churn_ = 0, thin_window_ = 0;
  bool thin_read_only_ = false, thin_force_epoch_ = false, thin_offline_ = false;
  bool thin_captured_only_ = false;   // lane 162 (TEST ONLY)
  uint16_t thin_plant_layout_ = 0xFFFFu;   // lane 162 (the wrong_layout plant)
  uint32_t thin_drop_seq_[uint8_t(thin::Cls::Count)] = {0, 0, 0};  // lane 129 (KC): per class
  uint64_t thin_not_learned_ = 0;
  std::vector<std::array<uint32_t, 3>> thin_not_learned_list_;   // lane 214: (object, part, pass site) behind thin_not_learned_ (offline replay only; fview prints and clears them)
  std::unordered_set<uint32_t>* thin_arena_objects_ = nullptr;  // TEST ONLY (fview)
  uint64_t thin_blend_base_ = 0;
  bool blend_keys_wanted_ = true;   // S102 R21-A: a tween / DLSS consumer reads the draws' blend keys (live.cpp sets it every frame; true = BlendRanges as before)
  uint64_t ff_blend_skipped_ = 0, ff_blend_kept_ = 0;   // S102 R21-A: plain counters, folded into rdr::g_ff_* once a frame (the end of Bind)
  uint64_t thin_key_now_ = 0;                           // the key of the draw BindDraw is on (0 = none)
  ReplaceSlot* thin_slot_now_ = nullptr;
  int crowd_state_ = 0;
  bool crowd_injected_ = false;
  uint8_t card_result_ = 0;       // lane 175: InjectCards' report of this frame (CrowdInject::card_result)
  uint8_t member_result_ = 0;     // lane 272: Inject's report of this frame (CrowdInject::member_result)
  int cards_state_ = 0;           // S101 lane CD: the far cards' own injection this frame (0 not asked yet, 1 ours, 2 the stream's stay)
  uint64_t crowd_kept_base_ = 0;  // S101 lane 50
  bool part_drew_ = false;        // S101 lane 50: the parts of one split guest frame so far
  bool part_member_reported_ = false;   // lane 272: a part of this guest frame already told the members' gate something
  uint64_t part_kept_ = 0;
  CrowdCounters crowd_;
  uint32_t crowd_window_ = 0;
  bool after_post_ = false;  // settings parity S3: this frame's post chain's last pass has been bound
  // S101 lane PO (post_hook.h): the hook, this frame's decision (0 none, 1 planned and not yet injected, 2 injected: the claim's drops apply, 3 the injection failed: the stream's chain), the claim, the census.
  PostHook* post_hook_ = nullptr;
  int post_state_ = 0;
  uint64_t alias_elide_on_claimed_ = 0;   // S102 G12 review F1
  // S102 G12 review F1: PLAN-TIME tests of the post claim. post_state_ is 1 from PlanPost until the first claimed draw injects ours (then 2): every planner that runs before the walk (AliasBegin, PlaneBlankBegin,
  // PlanePlan, OwnGroupPlan, TurnsBegin) must use these, never `post_state_ == 2` (tools/g12/g12_check.py lints it). The walk's own tests keep `== 2`.
  bool PostClaimPlanned(uint32_t ops) const {
    const bool state = copy_alias::g_fault == copy_alias::Fault::PostClaimLate ? post_state_ == 2 : (post_state_ == 1 || post_state_ == 2);   // (the fault: the old test)
    return state && post_claim_.drop.size() == ops;
  }
  bool PostClaimsOp(uint32_t ops, uint32_t p) const { return PostClaimPlanned(ops) && post_claim_.drop[p]; }
  PostClaim post_claim_;
  PostCounters post_;
  rdr::TargetsOp post_last_targets_{};   // S101 lane PO
  uint32_t post_window_ = 0;
  // Settings parity S12 (half-rate shadow maps).
  std::unordered_set<uint32_t> sd_blocked_dests_;   // S102 C3 F1: this frame's copy destinations that a half-rate-skipped source feeds
  shadow_roles::Sets shadow_sets_;  // the game's image ids (S101 lane 86: was shadow_targets_ / shadow_excluded_)
  std::vector<uint8_t> image_roles_;  // S101 lane 86: names.h SurfaceRole by the capture's image id (SyncImages)
  uint8_t ImageRole(uint32_t id) const { return id && id < image_roles_.size() ? image_roles_[id] : uint8_t(0); }
  // S102 S0-3: names.h SurfaceRole -> the renderer's GpuClass (api.h). The plane shadows of the game's own targets read as shadow: only OUR plane images (below) are the plane class.
  // S102 G6: the role -> GPU class table, the derived roles and the scene-size pick live in stamp_roles.h (unit-tested; the plants are g_gpu_class_plant).
  static uint8_t GpuClassOfRole(uint8_t role) { return stamp_roles::GpuClassOfRole(role, g_gpu_class_plant); }
  std::vector<uint8_t> gpu_class_set_;  // S102 G6: the GpuClass last given to each capture image id (0xFF = not yet)
  void RefreshGpuClasses(const std::vector<ImageRecord>& records) {
    uint16_t scene_w = 0, scene_h = 0;
    stamp_roles::PickSceneSize(records, scene_w, scene_h);  // S102 G6 review: screen-shaped SceneColor records only (not the 736x1280 roster picture)
    for (const ImageRecord& r : records) {
      if (!r.id || r.id >= image_ids_.size() || image_ids_[r.id] == rdr::kNoImage) continue;
      if (gpu_class_set_.size() <= r.id) gpu_class_set_.resize(r.id + 1, 0xFF);
      const uint8_t cls = (pl_target_ids_.count(r.id) || pc_storage_ids_.count(r.id)) ? uint8_t(rdr::kGpuPrelight) : GpuClassOfRole(stamp_roles::RoleForStamps(r, scene_w, scene_h));   // S102 C1: a light cube drawn into by the layered pass keeps the pre-light row (the strip's role), not 'copies' 
      if (gpu_class_set_[r.id] == cls) continue;
      gpu_class_set_[r.id] = cls;
      renderer_.SetImageClass(image_ids_[r.id], cls);
    }
  }
  uint32_t current_target_ = 0;
  bool shadow_skip_frame_ = false, skipping_target_ = false;
 public:
  uint64_t shadow_skipped_ = 0;  // target changes left out (for the log)
 private:
  // S10b (DLSS).
  uint32_t current_depth_ = 0, current_color_ = 0;  // the game's ids of the bound targets
  uint32_t dlss_depth_ = 0;                          // the scene's depth target (the game's id)
  float dlss_vp_[16] = {}, dlss_prev_vp_[16] = {};
  uint32_t kept_after_dlss_copy_ = 0;  // lane 201: draws bound after this frame's DLSS input copy (the shown picture then differs from dlss_out by them)
  bool dlss_vp_valid_ = false, dlss_prev_vp_valid_ = false, dlss_scene_done_ = false, dlss_copied_ = false, dlss_copy_pending_ = false;
  rdr::ImageId dlss_color_ = rdr::kNoImage;           // our copy of the picture before the HUD
  rdr::ImageDesc dlss_color_desc_{};
  // S10b, the wrestlers' own motion.
  bool velocity_mode_ = false, velocity_drawn_ = false;
  std::vector<std::pair<uint32_t, uint64_t>> keyed_draws_;  // S9b fix: (builder draw index, base key) of this frame's keyed draws
  std::vector<uint32_t> velocity_draws_;  // this frame's wrestler scene draws (indices into frame.draws)
  rdr::ImageId velocity_image_ = rdr::kNoImage, velocity_depth_ = rdr::kNoImage;
  rdr::ImageDesc velocity_desc_{};
  // The wrestler draws again, by their VELOCITY twins, into our motion image (RGBA32F: NDC move, depth, mask) over our own depth buffer, at
  // the scene's size; the renderer's DLSS motion pass takes the wrestlers' motion from it where they are the visible surface.
  template <class Source>
  void VelocityPass(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    velocity_drawn_ = false;
    if (velocity_draws_.empty() || !dlss_depth_) return;
    const ImageRecord* record = nullptr;
    for (const ImageRecord& r : capture.images())
      if (r.id == dlss_depth_) record = &r;
    if (!record) return;
    rdr::ImageDesc desc;
    desc.width = record->width, desc.height = record->height;
    desc.format = rdr::Format::RGBA32F;
    desc.usage = uint8_t(rdr::kUsageSampled | rdr::kUsageTarget | (record->usage & (rdr::kUsageScaled | rdr::kUsageLasting)));  // S101 names: lasting by role
    if (velocity_image_ == rdr::kNoImage || std::memcmp(&desc, &velocity_desc_, sizeof(desc)) != 0) {
      if (velocity_image_ != rdr::kNoImage) renderer_.DestroyImage(velocity_image_);
      if (velocity_depth_ != rdr::kNoImage) renderer_.DestroyImage(velocity_depth_);
      velocity_image_ = renderer_.CreateImage(desc);
      rdr::ImageDesc depth = desc;
      depth.format = rdr::Format::Depth;
      depth.usage = uint8_t(rdr::kUsageTarget | rdr::kUsageTransient | (record->usage & rdr::kUsageScaled));
      velocity_depth_ = renderer_.CreateImage(depth);
      velocity_desc_ = desc;
    }
    if (velocity_image_ == rdr::kNoImage || velocity_depth_ == rdr::kNoImage) return;
    // The far value of the game's depth test (its compare: GREATER / GEQUAL = reversed, far 0; else far 1).
    const DrawFixed& first = frame.fixeds[frame.draws[velocity_draws_[0]].fixed];
    const uint8_t compare = first.state.depth_compare;
    rdr::TargetsOp targets;
    targets.color_count = 1;
    targets.color[0].image = velocity_image_;
    targets.depth.image = velocity_depth_;
    builder.SetTargets(targets);   // allow:d3
    rdr::ClearOp clear;
    clear.flags = rdr::kClearColor | rdr::kClearDepth;
    clear.full = 1;
    clear.depth = (compare == 4 || compare == 6) ? 0.0f : 1.0f;  // VkCompareOp GREATER 4, GREATER_OR_EQUAL 6
    builder.Clear(clear);
    skipping_target_ = false;  // (S12's skip is for the game's shadow targets only)
    velocity_mode_ = true;
    for (const uint32_t index : velocity_draws_) BindDraw(frame, frame.draws[index], capture, builder, bytes, stats);
    velocity_mode_ = false;
    velocity_drawn_ = true;
    static bool said = false;
    if (!said) said = true, rdr::Log("[bind] DLSS velocity pass: %zu wrestler draws", velocity_draws_.size());
  }
  // Copies the bound colour target (the post chain's last pass just drew it) into dlss_color_, made like that target.
  template <class Source>
  void CopyBeforeHud(const Source& capture, rdr::FrameBuilder& builder) {
    dlss_copy_pending_ = false;
    const ImageRecord* record = nullptr;
    for (const ImageRecord& r : capture.images())
      if (r.id == current_color_) record = &r;
    if (!record) return;
    rdr::ImageDesc desc;
    desc.width = record->width, desc.height = record->height, desc.depth = 1, desc.mips = 1, desc.layers = 1;
    desc.format = rdr::Format(record->format);
    desc.kind = rdr::ImageKind::Image2D;
    desc.usage = uint8_t(rdr::kUsageSampled | (record->usage & (rdr::kUsageScaled | rdr::kUsageLasting)));  // S101 names: lasting by role
    if (dlss_color_ == rdr::kNoImage || std::memcmp(&desc, &dlss_color_desc_, sizeof(desc)) != 0) {
      if (dlss_color_ != rdr::kNoImage) renderer_.DestroyImage(dlss_color_);
      dlss_color_ = renderer_.CreateImage(desc);
      dlss_color_desc_ = desc;
    }
    if (dlss_color_ == rdr::kNoImage) return;
    rdr::CopyOp copy{};
    copy.source.image = Map(current_color_);
    copy.dest.image = dlss_color_;
    copy.source_rect = {0, 0, record->width, record->height};
    copy.dest_rect = copy.source_rect;
    builder.Copy(copy);   // allow:d3
    dlss_copied_ = true;
  }
 private:
  // S101 lane 2 (fview): see SetVariantOverride and BindTrace.
  struct VariantOverride {
    std::vector<uint32_t> spirv, alpha;
    bool drop_twin = false;
  };
  std::vector<std::string> override_errors_;
  std::unordered_map<int, VariantOverride> overrides_;
  BindTrace draw_trace_;
  void ApplyOverride(int variant, bool twin, const uint32_t*& spirv, size_t& bytes) const {
    const auto found = overrides_.find(variant);
    if (found == overrides_.end()) return;
    const std::vector<uint32_t>& words = twin ? found->second.alpha : found->second.spirv;
    if (words.empty()) return;
    spirv = words.data();
    bytes = words.size() * sizeof(uint32_t);
  }
  // Whether the pixel variant is drawn with an alpha-test twin when the game asks for the test: the compiled table's answer, unless a reload dropped it.
  bool TwinOf(int ps) const {
    if (!rdr::fx::kVariants[ps].bytes_alpha) return false;
    const auto found = overrides_.find(ps);
    return found == overrides_.end() || !found->second.drop_twin;
  }
  bool ProgramHasAlphaTwin(int ps) const { return TwinOf(ps); }
  std::unordered_map<uint64_t, uint16_t> programs_;
  std::deque<std::string> names_;  // program names; a deque keeps each string where the renderer holds its pointer
  std::unordered_map<uint64_t, int> layouts_;
  std::unordered_map<uint64_t, uint32_t> occurrences_;
  std::unordered_map<uint64_t, uint32_t> missing_;
  uint32_t wanted_[rdr::kMaxTextures] = {};
  uint32_t stamp_ = 0;
  uint32_t listed_target_ = 0;
  std::unordered_set<uint32_t> isolate_cleared_;  // isolated redraws: colour targets already cleared this frame
  std::vector<uint32_t> gallery_;
  rdr::PresentOp last_present_;

  // ---- S101 lane 59: the stage view (stage_view.h) ----
  struct StageImage { uint16_t width = 0, height = 0; bool scene = false; };
  std::vector<StageImage> stage_images_;   // by the capture's image id
  stage_view::SceneTracker stage_scene_;
  uint64_t stage_view_frames_ = 0, stage_view_missed_ = 0;
  uint32_t stage_last_target_ = 0;
  bool stage_frame_on_ = false, stage_replaced_ = false;

  void NoteStageDraw() {
    const uint32_t id = current_color_;
    stage_scene_.NoteDraw(id, id && id < stage_images_.size() && stage_images_[id].scene);
  }

  // At the present, with the stage view active: copy the scene's target over the presented surface (aspect kept). The presented surface is the game's own (a
  // copy destination with sampled usage); the scene target is a render target only, so it cannot be presented directly.
  void StageViewCopy(uint32_t presented_id, rdr::ImageId presented, rdr::FrameBuilder& builder) {
    if (!stage_frame_on_) return;
    uint32_t scene_id = stage_scene_.image;
    if (stage_view::g_pick_fault) {  // fview's planted twins of the pick: 1 = the highest-id declared SceneColor, 2 = the lowest-id one (instead of the one the frame drew into)
      scene_id = 0;
      for (uint32_t i = 0; i < stage_images_.size(); ++i)
        if (stage_images_[i].scene && (stage_view::g_pick_fault == 1 || !scene_id)) scene_id = i;
    }
    const rdr::ImageId scene = Map(scene_id);
    if (!scene_id || scene == rdr::kNoImage || presented == rdr::kNoImage || scene == presented || !presented_id || presented_id >= stage_images_.size() ||
        scene_id >= stage_images_.size()) {
      if (++stage_view_missed_ == 1) rdr::Log("[bind][stage_view] on, but this frame drew into no declared scene target (or it cannot be presented): the game's picture stays");
      return;
    }
    const StageImage& from = stage_images_[scene_id];
    const StageImage& to = stage_images_[presented_id];
    const stage_view::FitRect fit = stage_view::Fit(from.width, from.height, to.width, to.height);
    rdr::CopyOp copy;
    copy.source.image = scene;
    copy.dest.image = presented;
    copy.source_rect = {0, 0, from.width, from.height};
    copy.dest_rect = {fit.x, fit.y, fit.w, fit.h};
    copy.filter = rdr::CopyFilter::Linear;
    builder.Copy(copy);   // allow:d3
    dlss_copied_ = false;  // no DLSS inputs for a picture that is not the game's own post chain
    stage_replaced_ = true;
    stage_last_target_ = scene_id;
    if (++stage_view_frames_ == 1)
      rdr::Log("[bind][stage_view] presenting scene target %u (%ux%u, %llu draws) over surface %u (%ux%u)", scene_id, from.width, from.height,
               static_cast<unsigned long long>(stage_scene_.draws), presented_id, to.width, to.height);
  }
