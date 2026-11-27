
  // ---- S101 lane 67: particle runs as one draw (src\renderer_adapter\particle_runs.h) -----------------------------------------------------------------------
  struct ParticleRun {
    bool active = false, consumed = false;
    const Draw* first = nullptr;
    std::vector<uint32_t> members;  // op indices, first included
    particles::Packed packed;
    uint16_t layout = 0;
    int vs = -1;
    particles::EndWhy end = particles::EndWhy::FrameEnd;  // lane 132: what ended this run (FindRun's census.last_end)
    bool soft = false;  // lane 132: a run of ySoftParticle draws (ySoftParticleRuns)
  };
  // At a draw op: is it the start of a run of two or more? If so, packs the run and marks its other ops to be skipped; BindDraw then binds the first draw as the run's.
  template <class Source>
  [[gnu::noinline]] void ParticleBegin(const LoadedFrame& frame, const Source& capture, uint32_t op_index, BindStats& stats) {
    (void)stats;
    particle_run_.active = false;
    if (int64_t(op_index) <= particle_resume_op_) return;  // inside a run already scanned and kept the stream's (each member is counted once, the run is scanned once)
    const uint32_t draw_index = frame.ops[op_index].index;
    const particles::Kind kind = particles::Classify(frame, draw_index);
    if (kind == particles::Kind::NotParticle) return;
    particles::Census& c = particle_census_;
    ++c.draws;
    if (!particles::IsRunKind(kind)) {  // lane 132: SoftRun joins the runs
      ++(kind == particles::Kind::Soft ? c.soft : kind == particles::Kind::Third ? c.third : c.other);
      return;
    }
    particle_run_.soft = kind == particles::Kind::SoftRun;  // lane 132
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint32_t>& members = particle_run_.members;
    particles::FindRun(frame, op_index, particle_fault_, c, members);
    ++c.scans;
    const uint64_t n = members.size();
    c.draws += n - 1;
    ++c.runs;
    particle_run_.end = c.last_end;  // lane 132 (N5): a run of one is drawn by the merged path too (a part of `merged`), no longer `lone`; counted in ParticleEnd when it was drawn
    if (skipping_target_ || after_post_) {
      c.other += n;  // (a draw BindDraw would leave out: the run stays the stream's, each draw on its own)
      particle_resume_op_ = int64_t(members.back());
    } else if (!BuildParticleRun(frame, capture)) {
      c.other += n;
      particle_resume_op_ = int64_t(members.back());
    } else {
      for (size_t m = 1; m < members.size(); ++m) particle_skip_[members[m]] = 1;
      particle_run_.active = true, particle_run_.consumed = false;
      particle_run_.first = &frame.draws[draw_index];
    }
    c.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  template <class Source>
  bool BuildParticleRun(const LoadedFrame& frame, const Source& capture) {
    static const int hard_runs_vs = VariantByName("yParticleRuns_runs_vertex");
    static const int soft_runs_vs = VariantByName("ySoftParticleRuns_runs_vertex");  // lane 132
    const bool soft = particle_run_.soft;
    const int runs_vs = soft ? soft_runs_vs : hard_runs_vs;
    if (runs_vs < 0) return false;
    const uint32_t vertex_bytes = soft ? particles::kSoftVertexBytes : particles::kVertexBytes;  // lane 132
    const uint32_t element_count = soft ? 3 : 4;
    const Draw& first = frame.draws[frame.ops[particle_run_.members[0]].index];
    const bool swap_pairs = !(frame.header.flags & kFrameSixteenPairsHighFirst);
    // (Layout()'s swap01 fix-up for captures before kFrameFetchOrderOriginal is not repeated: it concerns rigid props and shadows, never the particle layout, whose four
    // fetches Classify pins at offsets 0 / 12 / 16 / 24.)
    const auto read = [&](const Draw& d, std::vector<uint8_t>& out) {
      if (!capture.ReadGeometry(d.vertex_offset, d.vertex_bytes, out)) return false;
      if (swap_pairs) SwapSixteenPairs(frame, frame.layouts[d.layout], out, d.vertex_bytes);
      return true;
    };
    if (!particles::Pack(frame, particle_run_.members, particle_fault_, read, particle_run_.packed, particle_scratch_, soft)) return false;
    // The merged layout: the variant's attributes 0-3 read the stream's own elements (formats and offsets as the stream's layout has them), 4-7 the LocalWorld rows.
    const rdr::fx::Variant& variant = rdr::fx::kVariants[runs_vs];
    const VertexLayout& layout = frame.layouts[first.layout];
    rdr::VertexLayoutDesc desc{};
    desc.strides[0] = soft ? particles::kSoftMergedStride : particles::kMergedStride;  // lane 132
    uint64_t key = soft ? 0x534F4654ull : 0x50415254ull;
    const auto mix = [&](uint64_t value) { key = (key ^ value) * 0x100000001b3ull; };
    for (uint32_t a = 0; a < variant.attribute_count; ++a) {
      const rdr::fx::Attribute& attribute = variant.attributes[a];
      if (attribute.fetch < element_count) {
        const VertexElement& element = frame.elements[layout.element_first + attribute.fetch];
        desc.attributes[a] = {attribute.location, 0, element.vk_format, element.host_offset};
      } else {  // (lane 132: the rows follow the game's vertex: LocalWorld, then for the soft variant LocalWorldView)
        desc.attributes[a] = {attribute.location, 0, particles::kLocalWorldFormat, uint32_t(vertex_bytes + 16 * (attribute.fetch - element_count))};
      }
      mix(uint64_t(desc.attributes[a].location) << 48 | uint64_t(desc.attributes[a].format) << 32 | desc.attributes[a].offset);
    }
    desc.attribute_count = variant.attribute_count;
    auto found = layouts_.find(key);
    if (found == layouts_.end()) found = layouts_.emplace(key, int(renderer_.AddVertexLayout(desc))).first;
    particle_run_.layout = uint16_t(found->second);
    particle_run_.vs = runs_vs;
    return true;
  }
  // After the run's first draw was bound: counts it; if BindDraw did not build the merged draw (it should always), the folded ops are un-skipped and draw as the stream's.
  void ParticleEnd() {
    particles::Census& c = particle_census_;
    const uint64_t n = particle_run_.members.size();
    if (particle_run_.consumed) {
      ++c.merged;
      c.merged_draws += n;
      c.particles_drawn += particle_run_.packed.particles;
      if (n == 1) ++c.single, ++c.single_end[uint8_t(particle_run_.end)];  // lane 132 (review 7: the "ended by" numbers sum to `single`)
      if (particle_run_.soft) c.soft_drawn += n;      // lane 132
    } else {
      for (size_t m = 1; m < n; ++m) particle_skip_[particle_run_.members[m]] = 0;
      c.other += n;
      particle_resume_op_ = int64_t(particle_run_.members.back());
    }
    particle_run_.active = false;
  }
  ParticleRun particle_run_;
  particles::Census particle_census_;
  particles::Fault particle_fault_ = particles::Fault::None;
  bool particles_on_ = false, particle_have_texture_ = false;
  int64_t particle_resume_op_ = -1;  // F1: ops up to here belong to a run already scanned and not merged
  uint32_t particle_idle_frames_ = 0;
  uint32_t stream_particle_draws_ = 0;  // S103 PF
  bool particle_idle_said_ = false;
  uint64_t particle_draws_base_ = 0;
  uint32_t particle_window_ = 0;
  std::vector<uint8_t> particle_skip_, particle_scratch_;
  rdr::TextureSet particle_first_textures_{};

  // ---- S101 lane 190 (copy_alias.h): resolve copies dropped, their destinations aliased to the source for one frame ---------------------------------------------
  copy_alias::Planner copy_alias_;
  // S102 lane G4 (dead_work.h): per frame, the ops of passes nothing reads (the switches are read here, every frame).
  dead_work::Planner dead_planner_;
  dead_work::Plan dead_plan_;
  bool dead_on_ = false;
  bool dead_prev_part_open_ = false;  // the previous bind was a part with more of its guest frame to come: this one is the last part, its readers may be in the earlier ones
  void DeadWorkBegin(const LoadedFrame& frame, const BindStats& stats) {
    dead_on_ = false;
    dead_work::Mode mode = DeadMode();
    const bool prev_open = dead_prev_part_open_;
    dead_prev_part_open_ = frame_part_open && !persist_only_bind;   // (review F9: a lasting-only bind sets frame_part_open too, but it is no part of a split frame)
    if (!mode.card_bake && !mode.prelight) return;
    const bool ledger_ok = !(stats.isolate || velocity_mode_ || !frame.ops.count);
    const bool ok = !(persist_only_bind || frame_part_open || prev_open);
    const uint64_t stale_before = dead_planner_.census.stale_read;
    dead_planner_.Build(frame, mode, ok, ledger_ok, dead_plan_);
    if (dead_planner_.census.stale_read != stale_before)   // one line per stale read (review F2)
      rdr::Log("[bind][dead_work] stale read: image %u was read in frame %llu after its last bake was cut: never cut again (stale reads so far %llu)", dead_planner_.last_stale_image,
               static_cast<unsigned long long>(frame_number_), static_cast<unsigned long long>(dead_planner_.census.stale_read));
    dead_on_ = !dead_work::g_census_only.load(std::memory_order_relaxed) && dead_plan_.count > 0;
  }
  // S102 lane G4b (plane_blank.h): the blank plane shadow chain. Its switch is the per-frame table's wwe2k14_rdr_skip_blank_plane (fview: --skip-blank-plane), read here every frame.
  plane_blank::Planner plane_blank_planner_;
  dead_work::Plan plane_blank_plan_;
  bool plane_blank_on_ = false;        // this frame's chain ops are left out (the op loop reads it)
  bool plane_blank_enabled_ = false;   // the switch is on this frame (copy_alias excludes the chain's images)
  bool plane_blank_prev_part_open_ = false;
  // Interface for another lane's own pass (P3's own plane): set true by the code that takes the chain this frame, BEFORE PlaneBlankBegin runs (the planner then declines and clears its ledger).
  // Interface for another lane's drop set (G3's turns): push its per-op drop vector into PlaneBlankOthers() the same way (see the marked lines in PlaneBlankBegin).
  bool plane_blank_chain_taken_ = false;
  void PlaneBlankBegin(const LoadedFrame& frame, const BindStats& stats) {
    plane_blank_on_ = false;
    plane_blank_enabled_ = false;
    const bool prev_open = plane_blank_prev_part_open_;
    plane_blank_prev_part_open_ = frame_part_open && !persist_only_bind;
    if (!rdr_sw::BoolSeen(rdr_sw::kSkipBlankPlane, plane_blank::g_fview.load(std::memory_order_relaxed), frame.header.guest_frame)) {   // (F2: the GUEST frame, as every other reader of the table)
      plane_blank_planner_.Forget();   // F5: switched off: the picture is unknown from here on
      return;
    }
    plane_blank_enabled_ = true;
    plane_blank::Ctx ctx;
    ctx.guest_frame = frame.header.guest_frame;
    ctx.ledger_ok = !(stats.isolate || velocity_mode_ || !frame.ops.count);
    ctx.persist_only = persist_only_bind;
    ctx.plan_ok = !(persist_only_bind || frame_part_open || prev_open);
    ctx.chain_taken = plane_blank_chain_taken_;
    ctx.keep_mask = (g_shadow_half && !shadow_sets_.Empty()) || turns_on_;   // S102 G4c (review F1): while a later frame can leave the mask out, the mask pass is never dropped (A would go stale)
    if (shadow_skip_frame_) ctx.half_skips = [this](uint32_t image) { return OldHalfSkips(image); };   // S102 G4c: a half-rate frame does not draw the chain at all
    // The other plans' drop sets (F4): dead_plan_ (G4), bake_drop_ (P1), post_claim_ (PO, while it claims), and G3's turns plan at merge (push it here).
    if (dead_on_ && dead_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(dead_plan_.drop.data());   // (review F4: size-guarded like the others)
    if (bake_on_ && bake_drop_.size() >= frame.ops.count) ctx.others.push_back(bake_drop_.data());
    if (PostClaimPlanned(frame.ops.count)) ctx.others.push_back(post_claim_.drop.data());   // S102 G12 review F1: at plan time the claim is state 1, not 2
    if (turns_on_ && turns_plan_.drop.size() >= frame.ops.count) ctx.turns = turns_plan_.drop.data();   // S102 G3 (review F3): a pass G3's turn leaves out is handled like the half rate's (planner `hs`), not declined as an overlap
    plane_blank_planner_.Build(frame, ctx, plane_blank_plan_);
    plane_blank_on_ = plane_blank_plan_.count > 0;
  }
  // The planner's view of the binder: image records (the bake's 64x64 checks) and the bake's vertex program (review F5).
  dead_work::Mode DeadMode() {
    dead_work::Mode mode;
    mode.card_bake = dead_work::g_skip_card_bake.load(std::memory_order_relaxed);
    mode.prelight = dead_work::g_skip_prelight.load(std::memory_order_relaxed);
    mode.record = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    mode.is_bake_draw = [](const Draw& d) {
      const int vs = FindVariant(d.vs_hash);   // (any twin of the program: the name starts with the effect and technique)
      return vs >= 0 && rdr::fx::kVariants[vs].name && std::strncmp(rdr::fx::kVariants[vs].name, "yAudDefault_skinned_lit_lm_vertex", 33) == 0;
    };
    return mode;
  }
 public:
  std::string DeadWorkReport() const {
    dead_work::Mode mode;
    mode.card_bake = dead_work::g_skip_card_bake.load(std::memory_order_relaxed);
    mode.prelight = dead_work::g_skip_prelight.load(std::memory_order_relaxed);
    return dead_planner_.Report(mode);
  }
  const dead_work::Census& dead_work_census() const { return dead_planner_.census; }
  std::string PlaneBlankReport() const { return plane_blank_planner_.Report(); }   // S102 lane G4b
  std::string PreLightAnalyticReport() const { return pa_planner_.Report(); }   // S102 lane L1
  const prelight_analytic::Census& prelight_analytic_census() const { return pa_planner_.census; }
  std::string PreLightReport() const { return pl_planner_.Report(); }   // S102 lane C1
  std::string PreLightComputeReport() const { return pc_planner_.Report(); }   // S102 lane L1b
  const prelight_layered::Census& prelight_census() const { return pl_planner_.census; }
  std::string FrameCutsReport() const { return frame_cuts_.Report(frame_cuts_blit_, frame_cuts_restore_); }   // S102 lane G11
  std::string FrameRoundTripReport() const {   // S102 R18-B1
    char b[96];
    std::snprintf(b, sizeof b, " read-backs of left-out snapshots %llu", static_cast<unsigned long long>(copy_alias::RtReadbackHits()));
    return rt_.Report(rt_level_) + b;
  }
  const frame_roundtrip::Census& frame_roundtrip_census() const { return rt_.census; }
  const frame_cuts::Census& frame_cuts_census() const { return frame_cuts_.census; }
  const plane_blank::Census& plane_blank_census() const { return plane_blank_planner_.census; }
