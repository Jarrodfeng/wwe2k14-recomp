  uint64_t stale_contents_ = 0;
  uint64_t stale_subres_ = 0;
  std::unordered_map<uint64_t, GeometryEntry> geometry_;  // by geometry offset (the reader's content-deduplicated key)
  uint64_t frame_number_ = 0;
  // S102 lane G3 (map_turns.h): DRAWN frames (binds that are a guest frame's picture), the half rate's and the turns' clock.
  uint64_t drawn_frame_ = 0;
  map_turns::DrawnClock drawn_clock_;
  map_turns::Planner turns_planner_;
  map_turns::Plan turns_plan_;
  bool turns_on_ = false;
  bool turns_prev_open_ = false;
  bool OldHalfSkips(uint32_t image) const { return shadow_skip_frame_ && shadow_sets_.Skips(image) && !turns_planner_.Owns(image); }   // the old all-at-once half rate, except the images a turn owns
  void TurnsBegin(const LoadedFrame& frame, const BindStats& stats) {
    turns_on_ = false;
    if (!turns_planner_.px_of)   // review F9: the follower rule's size bound
      turns_planner_.px_of = [this](uint32_t id) -> uint64_t {
        const auto found = rec_index_.find(id);
        return found == rec_index_.end() || found->second >= synced_records_.size() ? 0 : uint64_t(synced_records_[found->second].width) * synced_records_[found->second].height;
      };
    const bool ledger_ok = !(stats.isolate || velocity_mode_ || !frame.ops.count);
    const bool ok = !(persist_only_bind || frame_part_open || turns_prev_open_);
    turns_prev_open_ = frame_part_open && !persist_only_bind;
    const uint64_t stale_before = turns_planner_.census.StaleTotal();
    turns_planner_.Build(frame, drawn_frame_, [this](uint32_t id) { return ImageRole(id); }, dead_on_ ? &dead_plan_.drop : nullptr, ok, ledger_ok, turns_plan_, [this](uint32_t image) { return OldHalfSkips(image); });
    if (turns_planner_.census.StaleTotal() != stale_before && turns_planner_.census.StaleTotal() <= 8)   // one line per stale read (the first 8; the report carries the total)
      rdr::Log("[bind][turns] STALE READ: image %u (%s) was read in drawn frame %llu after its last write was left out in an earlier one (stale reads so far %llu)", turns_planner_.last_stale_image,
               map_turns::GroupName(turns_planner_.last_stale_group), static_cast<unsigned long long>(drawn_frame_), static_cast<unsigned long long>(turns_planner_.census.StaleTotal()));
    turns_on_ = turns_plan_.count > 0;
  }
 public:
  std::string TurnsReport() const { return turns_planner_.Report(); }
  const map_turns::Census& turns_census() const { return turns_planner_.census; }
  uint64_t drawn_frames() const { return drawn_frame_; }
 private:
  // S101 lane 35 (the crowd's first promotion): the hook, this frame's decision (0 not asked yet, 1 ours, 2 the stream's stay), and the census.
  CrowdHook* crowd_hook_ = nullptr;
  // S102 P1: the owned crowd card bake. bake_on_: this frame's passes are ours (every op of every bake pass is left out of the walk and re-issued as one group at the first pass's place).
  BakeHook* bake_hook_ = nullptr;
  BakeCounters bake_;
  bool bake_on_ = false;
  uint64_t plane_bake_dropped_ = 0;   // S102 L2: ops left out of the walk (rope draws)
  uint32_t bake_first_op_ = 0;
  struct BakePassOps { uint32_t set_op = 0, clear_op = ~0u, draw_op = 0, copy_op = 0; };
  std::vector<BakePassOps> bake_passes_;
  std::vector<uint8_t> bake_drop_;
  rdr::TargetsOp bind_last_targets_{};   // the last target switch the builder was given in this walk (restored after the bake group)
  bool bind_have_targets_ = false;
  struct BakeMini {   // a one-draw frame view: the bake draw with the hook's pose bank (the same pattern as the D3 mini frames)
    std::vector<Draw> draws;
    std::vector<DrawFixed> fixeds;
    std::vector<DrawDynamic> dynamics;
    std::vector<rdr::SamplerDesc> samplers;
    std::vector<TextureSlot> slots;
    std::vector<BoolLoop> bools;
    std::vector<VertexLayout> layouts;
    std::vector<VertexElement> elements;
    std::vector<ConstantBank> banks;
    LoadedFrame frame;
  } bake_mini_;
  // S102 P3: the plane chain owned as one pass. plane_on_: this frame's four passes are ours (every op of them is left out of the walk and re-issued as one group at the first pass's place).
  PlaneCounters plane_;
  bool in_plane_run_ = false;   // IssueCopy is called from PlaneRun (census only)
  uint32_t forced_elide_op_ = ~0u;   // the planted force_alias fault: this op's copy is treated as elided (SetAlias) although the plan did not elide it
  const rdr::CopyOp* copy_override_ = nullptr;   // the planted wrong_dest fault hands IssueCopy a doctored copy

  // ---- S102 lane G11 (frame_cuts.h; docs\renderer\FRAME_CLASS.md): the full-screen self blit and the colour + depth restore of the frame buffer ---------------------------------------------------
  // Switches wwe2k14_rdr_frame_selfblit / wwe2k14_rdr_frame_restore (PerFrame, default off), read every frame at FrameCutsBegin. The tracker (frame_cuts.h) sees every op of the capture frame (FrameCutsOp),
  // whatever other plans do with it, the writes the post group makes outside the capture's own ops (Foreign), and the copies (FrameCutsNoteCopy: IssueCopy sets last_copy_issued_ at its builder.Copy line).
  frame_cuts::Tracker frame_cuts_;
  frame_roundtrip::RoundTrip rt_;   // S102 R18-B1: the identity restores (frame_roundtrip.h), fed the same ops as frame_cuts_; rt_level_ = wwe2k14_rdr_frame_roundtrip, read every frame
  int rt_level_ = 0;
  bool frame_cuts_on_ = false, frame_cuts_blit_ = false, frame_cuts_restore_ = false, frame_cuts_prev_part_open_ = false, frame_cuts_post_done_ = false;
  bool last_copy_issued_ = false;
  rdr::TargetsOp frame_cuts_targets_{};
  bool frame_cuts_have_targets_ = false;
  uint32_t frame_cuts_window_ = 0;
  std::vector<int32_t> frame_cuts_op_targets_;   // per op: the index of the target switch current at it (-1: none yet)
  uint32_t plan_memo_window_ = 0;   // S102 R23-P: binds since the last census line
  // S102 R23-P: the census (plan_memo.h): this frame's digests and each planner's output, noted once the plans are made (tool only: g_census_on is set by rplay / fview).
  static uint64_t VecDigest(const std::vector<uint8_t>& v) { return v.empty() ? 0x51 : plan_memo::Hash64(v.data(), v.size()) ^ uint64_t(v.size()) << 40; }
  void PlanMemoCensus(const LoadedFrame& frame) {
    using namespace plan_memo;
    static Digests cur;
    Compute(frame, cur);
    uint64_t recs = 0x77;
    for (const ImageRecord& r : synced_records_) recs = Mix(Mix(recs, uint64_t(r.id) << 32 | uint64_t(r.width) << 16 | r.height), uint64_t(r.format) << 24 | uint64_t(r.usage) << 16 | uint64_t(r.role) << 8 | r.flags);
    cur.v[kDgRecords] = recs;
    uint64_t out[kPlCount] = {};
    bool ran[kPlCount] = {}, empty[kPlCount] = {};
    const uint32_t n = frame.ops.count;
    ran[kPlPost] = g_post_ours && post_hook_;
    out[kPlPost] = Mix(Mix(Mix(post_state_, VecDigest(post_claim_.draw)), VecDigest(post_claim_.drop)), uint64_t(post_claim_.first_draw) << 32 | post_claim_.last_draw);
    empty[kPlPost] = post_state_ == 0;
    ran[kPlAlias] = alias_plan_.elide.size() == n && n;
    out[kPlAlias] = Mix(Mix(Mix(VecDigest(alias_plan_.elide), VecDigest(alias_plan_.sd)), Mix(VecDigest(alias_plan_.fd), VecDigest(alias_plan_.direct))), alias_plan_.count);
    empty[kPlAlias] = alias_plan_.count == 0 && alias_plan_.direct_count == 0;
    ran[kPlDead] = dead_plan_.drop.size() == n && n;
    out[kPlDead] = Mix(VecDigest(dead_plan_.drop), dead_plan_.count);
    empty[kPlDead] = dead_plan_.count == 0;
    ran[kPlShadows] = shadows_active_ && shadows_plan_.named;
    {
      uint64_t sum = 0;
      for (const auto& item : shadows_plan_.chain) sum += Mix(item.first, item.second);
      for (const uint32_t d : shadows_plan_.reduce_dests) sum += Mix(d, 99);
      for (const uint32_t d : shadows_plan_.dests_rgba8) sum += Mix(d, 98);
      for (const uint32_t d : shadows_plan_.dests_16f) sum += Mix(d, 97);
      out[kPlShadows] = Mix(Mix(sum, shadows_plan_.casters), shadows_plan_.chain.size());
      empty[kPlShadows] = shadows_plan_.chain.empty();
    }
    ran[kPlPreLightC] = pc_plan_.drop.size() == n && n;
    out[kPlPreLightC] = Mix(Mix(VecDigest(pc_plan_.drop), pc_plan_.first_op), pc_plan_.groups.size());
    empty[kPlPreLightC] = pc_plan_.count == 0;
    ran[kPlRtSpan] = rt_level_ >= 2 && frame_cuts_on_;
    out[kPlRtSpan] = Mix(VecDigest(rt_role_), rt_spans_on_);
    empty[kPlRtSpan] = !rt_spans_on_;
    g_census.Note(cur, out, ran, empty);
  }
  void FrameCutsBegin(const LoadedFrame& frame, const BindStats& stats) {
    frame_cuts_on_ = frame_cuts_blit_ = frame_cuts_restore_ = false;
    const bool prev_open = frame_cuts_prev_part_open_;
    frame_cuts_prev_part_open_ = frame_part_open && !persist_only_bind;
    frame_cuts_blit_ = rdr_sw::BoolSeen(rdr_sw::kFrameSelfBlit, frame_cuts::g_fview_blit.load(std::memory_order_relaxed), frame.header.guest_frame);
    frame_cuts_restore_ = rdr_sw::BoolSeen(rdr_sw::kFrameRestore, frame_cuts::g_fview_restore.load(std::memory_order_relaxed), frame.header.guest_frame);
    rt_level_ = int(rdr_sw::IntSeen(rdr_sw::kFrameRoundtrip, frame_roundtrip::g_fview_level.load(std::memory_order_relaxed), frame.header.guest_frame));   // S102 R18-B1: read every frame
    if (g_dlss || g_velocity_census) rt_level_ = 0;   // (the scene's depth and colour are read at the Present: not judged)
    if (!frame_cuts_blit_ && !frame_cuts_restore_ && rt_level_ <= 0) return;
    frame_cuts_on_ = true;
    frame_cuts_have_targets_ = false;
    frame_cuts_post_done_ = false;
    const bool usable_frame = !(stats.isolate || velocity_mode_ || persist_only_bind || frame_part_open || prev_open || !frame.ops.count);
    frame_cuts_.BeginFrame(usable_frame);
    if (rt_level_ > 0) rt_.BeginFrame(usable_frame);   // S102 R18-B1
    if ((++frame_cuts_window_ >= 600 || census_flush::Due(g_cf_frame_cuts))) {   // the live census: what the cuts took and why they declined (every 600 frames while a switch is on)
      frame_cuts_window_ = 0;
      if (frame_cuts_blit_ || frame_cuts_restore_) rdr::Log("%s", FrameCutsReport().c_str());
      if (rt_level_ > 0) rdr::Log("%s", FrameRoundTripReport().c_str());   // S102 R18-B1
    }
    for (const ContentRef& c : frame.contents) frame_cuts_.MarkDirty(c.image);
    if (rt_level_ > 0) for (const ContentRef& c : frame.contents) rt_.MarkDirty(c.image);   // S102 R18-B1
    frame_cuts_op_targets_.assign(frame.ops.count, -1);
    int32_t cur = -1;
    for (uint32_t p = 0; p < frame.ops.count; ++p) {   // the target switch current at each op; the depth targets some draw tests stencil against
      const Op& op = frame.ops[p];
      if (op.kind == OpKind::SetTargets) cur = int32_t(op.index);
      frame_cuts_op_targets_[p] = cur;
      if (op.kind == OpKind::Draw && cur >= 0 && frame.fixeds[frame.draws[op.index].fixed].state.stencil_test) frame_cuts_.MarkStencil(frame.targets[uint32_t(cur)].depth.image);
    }
  }
  // The top of every op of the walk (before any other plan can drop it).
  void FrameCutsOp(const LoadedFrame& frame, const Op& op, uint32_t op_index) {
    switch (op.kind) {
      case OpKind::SetTargets: {
        const rdr::TargetsOp& t = frame.targets[op.index];
        uint32_t color[4] = {};
        for (uint32_t c = 0; c < t.color_count && c < 4; ++c) color[c] = t.color[c].image;
        frame_cuts_.OpSetTargets(color, t.color_count, t.depth.image);
        if (rt_level_ > 0) rt_.OpSetTargets(color, t.color_count, t.depth.image);   // S102 R18-B1
        frame_cuts_targets_ = t, frame_cuts_have_targets_ = true;
        break;
      }
      case OpKind::Clear: frame_cuts_.OpClear(); if (rt_level_ > 0) rt_.OpClear(); break;   // S102 R18-B1
      case OpKind::Draw:
        // F5: the post group draws its pass at the FIRST claimed draw and writes the targets of EVERY claimed draw there: tell the tracker before the walk gets to the ones that come later
        if ((post_state_ == 1 || post_state_ == 2) && !frame_cuts_post_done_ && op.index < post_claim_.draw.size() && post_claim_.draw[op.index]) {
          frame_cuts_post_done_ = true;
          for (uint32_t p = op_index; p < frame.ops.count; ++p) {
            const Op& later = frame.ops[p];
            if (later.kind != OpKind::Draw || later.index >= post_claim_.draw.size() || !post_claim_.draw[later.index] || frame_cuts_op_targets_[p] < 0) continue;
            const rdr::TargetsOp& t = frame.targets[uint32_t(frame_cuts_op_targets_[p])];
            for (uint32_t c = 0; c < t.color_count; ++c) frame_cuts_.Foreign(t.color[c].image);
            frame_cuts_.Foreign(t.depth.image);
            if (rt_level_ > 0) {   // S102 R18-B1: the same writes, for the pairs
              for (uint32_t c = 0; c < t.color_count; ++c) rt_.Foreign(t.color[c].image);
              rt_.Foreign(t.depth.image);
            }
          }
        }
        frame_cuts_.OpDraw();
        if (rt_level_ > 0) {   // S102 R18-B1: a draw writes the depth target only when its state can (depth test + write, or stencil test)
          const Draw& dr = frame.draws[op.index];
          const rdr::DrawState& st = frame.fixeds[dr.fixed].state;
          // review F3: a marker / thin draw (D3, thin templates) or one with an object origin does not draw from the state the record carries: it counts as a depth writer
          const DrawOrigin* org = frame.OriginOf(op.index);
          const bool unknown = (dr.flags & (kDrawNoGeometry | kDrawThin)) != 0 || (org && org->object != 0);   // (an engine object's part: an owned / replaced draw draws with our pipeline)
          rt_.OpDraw(unknown || (st.depth_test != 0 && st.depth_write != 0) || st.stencil_test != 0);
        }
        break;
      case OpKind::Copy: frame_cuts_.OpCopy(frame.copies[op.index].dest.image); if (rt_level_ > 0) rt_.OpCopy(frame.copies[op.index].dest.image); break;   // S102 R18-B1
      default: frame_cuts_.Flush(); if (rt_level_ > 0) rt_.Flush(); break;   // S102 R18-B1
    }
  }
  // After a copy op was handled: `issued` = IssueCopy really reached builder.Copy (not an alias elide, not a drop). A whole copy of one image into another of the same size, format and scaling is a provenance.
  // S102 R18-B1: G11's whole-copy rule as a function (FrameCutsNoteCopy's lines, unchanged): the copy moves mip 0 of one image onto mip 0 of another of the same size, format and scaling, raw bits.
  bool FrameCutsWholeCopy(const LoadedFrame& frame, const Op& op) const {
    const rdr::CopyOp& c = frame.copies[op.index];
    const auto rec = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    const ImageRecord* rs = rec(c.source.image);
    const ImageRecord* rd = rec(c.dest.image);
    bool whole = rs && rd && rs->width == rd->width && rs->height == rd->height && rs->format == rd->format && rs->mips == 1 && rd->mips == 1 && rs->layers == 1 && rd->layers == 1 &&
                 (rs->usage & rdr::kUsageScaled) == (rd->usage & rdr::kUsageScaled);   // F3: a copy between a scaled and an unscaled image resamples
    if (whole) whole = !(c.source.mip || c.source.layer || c.dest.mip || c.dest.layer) && c.source_rect.x == 0 && c.source_rect.y == 0 && c.source_rect.width >= rs->width && c.source_rect.height >= rs->height &&
                       c.dest_rect.x == 0 && c.dest_rect.y == 0 && c.dest_rect.width >= rd->width && c.dest_rect.height >= rd->height;
    // (the copy's red / blue swap flag (C8) is folded into the reading slots' swizzle and the copy is raw bits: a slot that reads with the identity swizzle reads the source's own pixels, which the rule
    // checks. A 16-bit target's copy is converted.)
    if (whole && op.index < frame.copy_extras.count && frame.copy_extras[op.index].target16) whole = false;
    return whole;
  }
  void FrameCutsNoteCopy(const LoadedFrame& frame, const Op& op, bool issued) {
    const rdr::CopyOp& c = frame.copies[op.index];
    const bool whole = FrameCutsWholeCopy(frame, op);
    frame_cuts_.CopyDone(c.source.image, c.dest.image, issued, whole);
    if (rt_level_ > 0) rt_.CopyDone(RtKey(c.source.image), RtKey(c.dest.image), issued, whole);   // S102 R18-B1: pairs are over the images' game ids after the alias (RtKey), the key the write versions use
  }
  // The walk's Draw case: true = the draw is not recorded (the self blit), or was replaced by two image copies (the restore).
  template <class Source>
  bool FrameCutsDraw(const LoadedFrame& frame, uint32_t op_index, const Source& capture, rdr::FrameBuilder& builder, BindStats& stats) {
    if (skipping_target_ || !frame_cuts_have_targets_) return false;
    const uint32_t draw_index = frame.ops[op_index].index;
    const uint64_t ps = frame.draws[draw_index].ps_hash;
    const bool rt_on = rt_level_ > 0;   // S102 R18-B1: the identity restore rides on G11's static rules, whether or not G11's own switch is on (the blit is G11's alone: review F6)
    if ((ps != frame_cuts::kBlitPs || !frame_cuts_blit_) && (ps != frame_cuts::kRestorePs || !(frame_cuts_restore_ || rt_on))) return false;
    const auto rec = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    const std::function<bool(uint64_t, uint32_t, std::vector<uint8_t>&)> geometry = [&capture](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) { return capture.ReadGeometry(offset, bytes, out); };
    frame_cuts::Facts facts = frame_cuts::FactsOf(frame, draw_index, frame_cuts_targets_, rec, geometry);
    // F8: two ids the walk maps to ONE image (an alias) are never a copy between two images
    if (ps == frame_cuts::kBlitPs) {
      facts.same_colour = Map(facts.tex_image[0]) == Map(facts.color_image);
    } else {
      for (int i = 0; i < 2 && i < int(facts.tex_count); ++i) {
        if (facts.tex_slot[i] == 1) facts.same_colour = Map(facts.tex_image[i]) == Map(facts.color_image);
        if (facts.tex_slot[i] == 0) facts.same_depth = Map(facts.tex_image[i]) == Map(facts.depth_image);
      }
    }
    const bool g11_on = ps == frame_cuts::kBlitPs ? frame_cuts_blit_ : frame_cuts_restore_;
    frame_cuts::Verdict v = frame_cuts_.Decide(facts, g11_on);   // S102 R18-B1: the census of G11's lane counts only while G11's own switch is on
    if (v.kind == frame_cuts::Verdict::Keep) {
      if (rt_on && ps == frame_cuts::kRestorePs) rt_.CountStatic(v.why);   // S102 R18-B1
      return false;
    }
    bool identity = false;   // S102 R18-B1: the draw is the frame's identity (leave it out: no copies either)
    if (rt_on && v.kind == frame_cuts::Verdict::Blit) rt_.CancelDraw();   // G11 leaves the self blit out (its own fate and census): it writes nothing, so the pairs it sits between stay valid
    if (rt_on && v.kind == frame_cuts::Verdict::Restore) {
      frame_roundtrip::Why why = frame_roundtrip::Why::Taken;
      frame_cuts::Verdict mv = v;   // the pairs are over the physical images: an elided copy's destination reads its source (Map)
      mv.colour_src = RtKey(v.colour_src), mv.colour_dst = RtKey(v.colour_dst), mv.depth_src = RtKey(v.depth_src), mv.depth_dst = RtKey(v.depth_dst);
      if (rt_.RestoreIdentity(mv, why)) {   // both sources are exactly what the targets hold: the restore is left out whole
        identity = true;
        rt_.CountIdentityRestore(uint64_t(facts.target_w) * facts.target_h, frame_cuts_restore_);
        rt_.CancelDraw();
      } else {
        rt_.NoteRestore(mv);   // the targets equal the sources from here (the draw or G11's two copies make them so)
        if (!frame_cuts_restore_) {   // G11's restore is off: the draw stays
          if (frame_roundtrip::g_trace) rdr::Log("[rdr][frame_roundtrip] op %u: restore colour %u -> %u depth %u -> %u kept: %s", op_index, v.colour_src, v.colour_dst, v.depth_src, v.depth_dst, frame_roundtrip::WhyName(int(why)));
          return false;
        }
      }
      if (frame_roundtrip::g_trace) rdr::Log("[rdr][frame_roundtrip] op %u: restore colour %u -> %u (mapped %u -> %u) depth %u -> %u (mapped %u -> %u): %s", op_index, v.colour_src, v.colour_dst, mv.colour_src, mv.colour_dst,
                                              v.depth_src, v.depth_dst, mv.depth_src, mv.depth_dst, identity ? "IDENTITY, left out" : frame_roundtrip::WhyName(int(why)));
    }
    if (v.kind == frame_cuts::Verdict::Restore && frame_cuts::g_fault == frame_cuts::Fault::RestoreOtherSource) {   // the LEGAL twin: another colour image of the same size and format (the copy is valid, the pictures are not)
      for (const ImageRecord& r : synced_records_)
        if (r.width == facts.target_w && r.height == facts.target_h && r.format == facts.color_format && r.id != v.colour_src && r.id != v.colour_dst && r.id != v.depth_src && r.id != v.depth_dst &&
            (r.usage & rdr::kUsageScaled) == (facts.scaled_color ? rdr::kUsageScaled : 0)) {
          v.colour_src = r.id;
          break;
        }
    }
    ++stats.draws;
    ++stats.settings_out;
    draw_trace_.outcome = BindOutcome::LeftOut;
    draw_trace_.fate = identity ? kFateFrameIdentity : kFateFrameCut;
    if (identity) return true;   // S102 R18-B1
    if (v.kind == frame_cuts::Verdict::Restore) {   // two whole image copies in the draw's place, through the walk's own copy handling (IssueCopy's synthetic-copy parameter)
      rdr::CopyOp copy;
      copy.filter = rdr::CopyFilter::Nearest;
      copy.source_rect = copy.dest_rect = rdr::Rect{0, 0, facts.target_w, facts.target_h};
      const uint32_t src[2] = {v.colour_src, v.depth_src}, dst[2] = {v.colour_dst, v.depth_dst};
      for (int k = 0; k < (v.copy_depth ? 2 : 1); ++k) {
        copy.source = rdr::ImageRef{}, copy.source.image = rdr::ImageId(src[k]);
        copy.dest = rdr::ImageRef{}, copy.dest.image = rdr::ImageId(dst[k]);
        IssueCopy(frame, op_index, capture, builder, stats, &copy);
      }
    }
    return true;
  }
  // ---- S102 R18-B1 step 2: the scene target's round trip (frame_roundtrip.h Span; docs\renderer\FRAME_ROUNDTRIP.md) -------------------------------------------------------------------------------
  // Pattern (every op by op index): [snapshot copy of the frame buffer's colour -> X] [... depth -> Y] ... [SetTargets scene target] [Clear, full] [Restore draw (ps 9A83): X, Y -> scene target].
  // When the frame buffer F is exactly the scene target S at the first snapshot (a pair made by the first restore, nothing written to either since) and nothing writes S between the snapshot and the clear,
  // clear + restore put S back to what it is: the two snapshots (their only reader is the restore), the clear and the restore draw are all left out. The plan below proves everything that does not depend on
  // the walk's state (the pattern, G11's static rules for the restore, no other reader of X / Y, nobody writes S in the window, nobody else owns the four ops); RtSpanOp proves the pair at the first snapshot.
  bool rt_spans_on_ = false;
  // The key of the pairs and the write versions: an elided copy's destination reads its source for the rest of the frame (the alias, one hop: Map's first line), so its key IS the source's game id.
  uint32_t RtKey(uint32_t id) const { return alias_on_ && id < alias_.size() && alias_[id] ? alias_[id] : id; }
  std::vector<uint8_t> rt_role_;        // by op index: 0 none, 1 first snapshot copy, 2 second snapshot copy, 3 the clear, 4 the restore draw
  std::vector<uint32_t> rt_span_of_;    // by op index: the span (frame_roundtrip::Span) the op belongs to
  std::vector<uint8_t> rt_span_done_;   // by span: the restore draw was left out
  bool RtOpOwned(const LoadedFrame& frame, uint32_t p) const {   // decline on overlap: an op another plan (or the alias plan) handles is never mine
    const auto hit = [p](const std::vector<uint8_t>& v) { return p < v.size() && v[p] != 0; };
    if (dead_on_ && hit(dead_plan_.drop)) return true;
    if (bake_on_ && hit(bake_drop_)) return true;
    if (PostClaimPlanned(frame.ops.count) && hit(post_claim_.drop)) return true;
    if (turns_on_ && hit(turns_plan_.drop)) return true;
    if (plane_blank_on_ && hit(plane_blank_plan_.drop)) return true;
    if (pa_on_ && hit(pa_plan_.drop)) return true;
    if (pl_on_ && hit(pl_plan_.role)) return true;
    if (og_on_ && p < og_plan_.group_of_op.size() && og_plan_.group_of_op[p] != own_group::kNone) return true;
    if (plane_on_ && hit(plane_drop_)) return true;
    if (pb_on_ && hit(pb_drop_)) return true;
    if (dr_on_ && hit(dr_copy_)) return true;
    if (alias_on_ && (hit(alias_plan_.elide) || hit(alias_plan_.sd) || hit(alias_plan_.fd) || hit(alias_plan_.direct))) return true;
    if (pb_on_ && (p == pb_after_op_ || p == pb_first_op_)) return true;   // review F4: L2's composite / bake run at these ops, after the span's hook
    if (PostClaimPlanned(frame.ops.count) && p < frame.ops.count && frame.ops[p].kind == OpKind::Draw && frame.ops[p].index < post_claim_.draw.size() && post_claim_.draw[frame.ops[p].index]) return true;   // (a claimed draw: the post Inject runs at the first)
    return false;
  }
  // review F2: the ledger (C5's dr_stale_d_ / dr_banned_, as sets): the snapshots a taken span stopped writing are "stale" from that frame on; a later frame whose stream READS one (a draw's texture slot, a copy's source, the
  // Present) before the stream writes it (a copy into it, a target) is a violation: the pair is banned for good and the span never takes it again. Scanned over the capture's ops (whatever this lane does with them).
  frame_roundtrip::Ledger rt_ledger_;
  void RtLedgerScan(const LoadedFrame& frame) {
    if (rt_ledger_.Empty()) return;
    const auto read = [this](uint32_t id) { if (rt_ledger_.Read(id)) ++rt_.census.violations; };
    for (uint32_t p = 0; p < frame.ops.count && !rt_ledger_.Empty(); ++p) {
      const Op& o = frame.ops[p];
      if (o.kind == OpKind::Draw) {
        const Draw& dr = frame.draws[o.index];
        for (uint32_t k = 0; k < dr.texture_count; ++k) read(frame.texture_slots[dr.texture_first + k].image);
      } else if (o.kind == OpKind::Copy) {
        read(frame.copies[o.index].source.image);
        rt_ledger_.Written(frame.copies[o.index].dest.image);   // written by the stream before any later read: not stale
      } else if (o.kind == OpKind::Present) {
        read(frame.presents[o.index].source.image);
      }
    }
  }
  template <class Source>
  void RtSpanPlan(const LoadedFrame& frame, const Source& capture, const BindStats& stats) {
    (void)stats;
    rt_.ClearSpans();
    rt_role_.clear(), rt_span_of_.clear(), rt_span_done_.clear();
    const uint32_t n = frame.ops.count;
    RtLedgerScan(frame);   // review F2: before any early return: a later frame that reads a snapshot we stopped writing is seen even on a frame that takes no span
    if (!rt_.usable || n < 4 || frame_cuts_op_targets_.size() < n) return;
    using frame_roundtrip::SpanWhy;
    const auto rec = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    const std::function<bool(uint64_t, uint32_t, std::vector<uint8_t>&)> geometry = [&capture](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) { return capture.ReadGeometry(offset, bytes, out); };
    const auto targets_write = [&](uint32_t p, uint32_t a, uint32_t b) {   // does the draw / clear at op p have image a or b as a target (colour or depth)? An unknown target switch counts
      const int32_t ti = frame_cuts_op_targets_[p];
      if (ti < 0) return true;
      const rdr::TargetsOp& t = frame.targets[uint32_t(ti)];
      for (uint32_t c = 0; c < t.color_count; ++c)
        if (t.color[c].image == a || t.color[c].image == b) return true;
      return t.depth.image == a || t.depth.image == b;
    };
    for (uint32_t d = 2; d < n; ++d) {
      const Op& od = frame.ops[d];
      if (od.kind != OpKind::Draw || frame.draws[od.index].ps_hash != frame_cuts::kRestorePs) continue;
      const Op& oc = frame.ops[d - 1];
      const int32_t ti = frame_cuts_op_targets_[d];
      if (oc.kind != OpKind::Clear || ti < 0 || frame_cuts_op_targets_[d - 1] != ti) continue;
      rt_.CountSpanFound();
      const auto fail = [&](SpanWhy w) { rt_.CountSpanPlanDecline(w); };
      frame_roundtrip::Span s;
      s.clr = d - 1, s.drw = d;
      const rdr::TargetsOp& T = frame.targets[uint32_t(ti)];
      const frame_cuts::Facts f = frame_cuts::FactsOf(frame, od.index, T, rec, geometry);
      const frame_cuts::Verdict v = frame_cuts_.Decide(f, false);   // G11's static rules for this restore (the census stays G11's)
      if (v.kind != frame_cuts::Verdict::Restore) { fail(SpanWhy::Static); continue; }
      s.sc = v.colour_dst, s.sd = v.depth_dst, s.x = v.colour_src, s.y = v.depth_src, s.w = f.target_w, s.h = f.target_h;
      const rdr::ClearOp& clr = frame.clears[oc.index];
      if (!clr.full) { fail(SpanWhy::Static); continue; }
      s.clear_stencil = (clr.flags & rdr::kClearStencil) != 0;
      for (uint32_t p = d - 1; p-- > 0;) {   // the snapshot copies: the nearest copy before the clear into X, into Y
        const Op& o = frame.ops[p];
        if (o.kind != OpKind::Copy) continue;
        const rdr::CopyOp& c = frame.copies[o.index];
        if (c.dest.image == s.x && s.c1 == ~0u) s.c1 = p, s.fc = c.source.image;
        if (c.dest.image == s.y && s.c2 == ~0u) s.c2 = p, s.fd = c.source.image;
        if (s.c1 != ~0u && s.c2 != ~0u) break;
      }
      if (s.c1 == ~0u || s.c2 == ~0u || !s.fc || !s.fd || s.fc == s.sc || s.fd == s.sd || !FrameCutsWholeCopy(frame, frame.ops[s.c1]) || !FrameCutsWholeCopy(frame, frame.ops[s.c2])) { fail(SpanWhy::Pattern); continue; }
      if (s.c1 > s.c2) std::swap(s.c1, s.c2);   // review F1: c1 = the FIRST snapshot (colour or depth), c2 the second; fc / fd / x / y keep their colour / depth meaning
      // review F1: between the two snapshots nothing writes the frame buffer (the pair is judged at the first, the second copies a later state otherwise)
      bool between = false;
      for (uint32_t p = s.c1 + 1; p < s.c2 && !between; ++p) {
        const Op& o = frame.ops[p];
        if (o.kind == OpKind::Draw || o.kind == OpKind::Clear) between = targets_write(p, s.fc, s.fd);
        else if (o.kind == OpKind::Copy) between = frame.copies[o.index].dest.image == s.fc || frame.copies[o.index].dest.image == s.fd;
      }
      if (PostClaimPlanned(n))
        for (uint32_t p = s.c1 + 1; p < s.c2 && !between; ++p) {
          const Op& o = frame.ops[p];
          if (o.kind == OpKind::Draw && o.index < post_claim_.draw.size() && post_claim_.draw[o.index]) between = targets_write(p, s.fc, s.fd);
        }
      for (const ContentRef& c : frame.contents)
        if (c.image == s.fc || c.image == s.fd) between = true;
      if (between) { fail(SpanWhy::Written); continue; }
      // X and Y: written by their snapshot copy only, read by this restore only, not lasting, never read back by the CPU
      bool readers = false;
      for (uint32_t p = 0; p < n && !readers; ++p) {
        const Op& o = frame.ops[p];
        if (o.kind == OpKind::Draw) {
          const Draw& dr = frame.draws[o.index];
          if (p != d)
            for (uint32_t k = 0; k < dr.texture_count; ++k) {
              const uint32_t im = frame.texture_slots[dr.texture_first + k].image;
              if (im == s.x || im == s.y) readers = true;
            }
          if (targets_write(p, s.x, s.y)) readers = true;
          if (p > s.c1 && p < d && (dr.flags & (kDrawNoGeometry | kDrawThin)) != 0) readers = true;   // review F3: a marker / thin / owned draw in the window samples what its template says: unknown readers
        } else if (o.kind == OpKind::Clear) {
          if (targets_write(p, s.x, s.y)) readers = true;
        } else if (o.kind == OpKind::Copy) {
          const rdr::CopyOp& c = frame.copies[o.index];
          if (c.source.image == s.x || c.source.image == s.y) readers = true;
          if ((c.dest.image == s.x && p != s.c1) || (c.dest.image == s.y && p != s.c2)) readers = true;
        }
      }
      for (const ContentRef& c : frame.contents)
        if (c.image == s.x || c.image == s.y || c.image == s.sc || c.image == s.sd || c.image == s.fc || c.image == s.fd) readers = true;
      const ImageRecord* rx = rec(s.x);
      const ImageRecord* ry = rec(s.y);
      if (!rx || !ry || (rx->usage & rdr::kUsageLasting) || (ry->usage & rdr::kUsageLasting) || copy_alias::WasReadBack(s.x) || copy_alias::WasReadBack(s.y) || rt_ledger_.Banned(s.x) || rt_ledger_.Banned(s.y)) readers = true;
      if (readers) { fail(SpanWhy::Readers); continue; }
      // nobody writes S between the first snapshot and the clear (a draw or clear with S as a target, a copy into it); the post claim's claimed draws never target it
      bool written = false;
      for (uint32_t p = std::min(s.c1, s.c2) + 1; p < s.clr && !written; ++p) {
        const Op& o = frame.ops[p];
        if (o.kind == OpKind::Draw || o.kind == OpKind::Clear) written = targets_write(p, s.sc, s.sd);
        else if (o.kind == OpKind::Copy) written = frame.copies[o.index].dest.image == s.sc || frame.copies[o.index].dest.image == s.sd;
      }
      if (PostClaimPlanned(n))
        for (uint32_t p = 0; p < n && !written; ++p) {
          const Op& o = frame.ops[p];
          if (o.kind == OpKind::Draw && o.index < post_claim_.draw.size() && post_claim_.draw[o.index]) written = targets_write(p, s.sc, s.sd);
        }
      if (written) { fail(SpanWhy::Written); continue; }
      // a clear that also clears stencil: the dropped clear would have set the stencil plane; it is the same value an earlier full clear of the frame set, and no draw tests it (G11's stencil rule is in the verdict)
      if (s.clear_stencil) {   // review F5: the LAST op before the first snapshot that can touch S_d's stencil must be a full clear with the same value
        bool same = false;
        for (uint32_t p = s.c1; p-- > 0;) {
          const Op& o = frame.ops[p];
          if (o.kind == OpKind::Copy && frame.copies[o.index].dest.image == s.sd) break;
          if (o.kind != OpKind::Clear || frame_cuts_op_targets_[p] < 0 || frame.targets[uint32_t(frame_cuts_op_targets_[p])].depth.image != s.sd) continue;
          const rdr::ClearOp& e = frame.clears[o.index];
          if (!(e.flags & rdr::kClearStencil)) continue;
          same = e.full && e.stencil == clr.stencil;
          break;
        }
        if (!same) { fail(SpanWhy::Stencil); continue; }
      }
      if (RtOpOwned(frame, s.c1) || RtOpOwned(frame, s.c2) || RtOpOwned(frame, s.clr) || RtOpOwned(frame, s.drw)) { fail(SpanWhy::Overlap); continue; }
      if (rt_role_.empty()) rt_role_.assign(n, 0), rt_span_of_.assign(n, 0);
      const uint32_t idx = uint32_t(rt_.spans.size());
      rt_.spans.push_back(s);
      rt_role_[s.c1] = 1, rt_role_[s.c2] = 2, rt_role_[s.clr] = 3, rt_role_[s.drw] = 4;
      rt_span_of_[s.c1] = rt_span_of_[s.c2] = rt_span_of_[s.clr] = rt_span_of_[s.drw] = idx;
    }
    rt_span_done_.assign(rt_.spans.size(), 0);
    rt_spans_on_ = !rt_.spans.empty();
  }
  // The walk, at an op of a span: true = the op is left out.
  bool RtSpanOp(const LoadedFrame& frame, const Op& op, uint32_t op_index, rdr::FrameBuilder& builder, BindStats& stats) {
    using frame_roundtrip::SpanWhy;
    frame_roundtrip::Span& s = rt_.spans[rt_span_of_[op_index]];
    switch (rt_role_[op_index]) {
      case 1:   // the first snapshot: the one place the span is decided
        if (s.state != 0) return s.state == 1;
        if (RtKey(s.x) == RtKey(s.sc) || RtKey(s.y) == RtKey(s.sd) || RtKey(s.x) == RtKey(s.fc) || RtKey(s.y) == RtKey(s.fd)) {   // the alias plan mapped the snapshots onto the scene target / frame buffer: not a copy between images
          ++rt_.census.span_why[int(SpanWhy::Overlap)];
          s.state = 2;
          return false;
        }
        {
          const SpanWhy why = rt_.AcceptSpan(s, RtKey(s.fc), RtKey(s.sc), RtKey(s.fd), RtKey(s.sd));
          if (frame_roundtrip::g_trace) rdr::Log("[rdr][frame_roundtrip] span ops %u/%u/%u/%u: %s", s.c1, s.c2, s.clr, s.drw, frame_roundtrip::SpanWhyName(int(why)));
          if (why == SpanWhy::Taken) copy_alias::NoteRtDropped(s.x), copy_alias::NoteRtDropped(s.y), rt_ledger_.Taken(s.x, s.y);
        }
        if (s.state == 1) draw_trace_.outcome = BindOutcome::LeftOut, ++rt_.census.copies_dropped;
        return s.state == 1;
      case 2:
        if (s.state == 1) {
          draw_trace_.outcome = BindOutcome::LeftOut, ++rt_.census.copies_dropped;
          if (!rt_.SpanStillEqual(RtKey(s.fc), RtKey(s.sc), RtKey(s.fd), RtKey(s.sd))) ++rt_.census.violations;   // review F1 tripwire: the plan proved nothing writes F between the snapshots
        }
        return s.state == 1;
      case 3: {   // the clear
        if (s.state != 1) return false;
        if (frame_roundtrip::g_fault == frame_roundtrip::Fault::KeepClear) return false;   // planted: the scene target is wiped and nothing puts it back
        draw_trace_.outcome = BindOutcome::LeftOut;
        if (frame_roundtrip::g_fault == frame_roundtrip::Fault::KeepDepthClear) {            // planted: the depth plane is wiped
          rdr::ClearOp kept = frame.clears[op.index];
          kept.flags = uint8_t(kept.flags & rdr::kClearDepth);
          if (kept.flags) builder.Clear(kept);
        }
        rt_.CancelDraw();   // (the clear's writes were queued at the top of the op: it writes nothing)
        return true;
      }
      default: {   // 4: the restore draw
        if (s.state != 1) return false;
        ++stats.draws;
        ++stats.settings_out;
        draw_trace_.outcome = BindOutcome::LeftOut;
        draw_trace_.fate = kFateFrameIdentity;
        rt_.CancelDraw();
        rt_.CountSpanTaken(s, frame_cuts_restore_);
        rt_span_done_[rt_span_of_[op_index]] = 1;
        return true;
      }
    }
  }
  void RtSpanEnd() {
    for (size_t i = 0; i < rt_.spans.size(); ++i)
      if (rt_.spans[i].state == 1 && !rt_span_done_[i]) ++rt_.census.violations;   // taken at the snapshot, but the restore draw never reached its hook: the scene target was never put back
  }
