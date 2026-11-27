
  // ---- S102 P3 (review F5): the two ways a pass reaches the builder, shared by the stream's walk and every owned pass (PlaneRun now; the node lanes P4P5 / P7 / P8 use them too) -------------------
  // IssueTargets(frame, op_index, ...): the target switch of op `op_index` (the game's ids): current_* bookkeeping, SH2's redirect, the half-rate shadow skip, the gallery, Map(), the post claim's drop,
  // post_last_targets_ / bind_last_targets_ (what the post and bake groups restore), the isolate grey clear. Returns with nothing issued when the target is skipped or post-claimed.
  // IssueCopy(frame, op_index, ...): the copy of op `op_index`: post claim, half-rate skip, the dlss depth mark, gallery, list, the copy-alias elide (SetAlias), SH2's whole-copy rule, Map(), and EndAliasOf(dest)
  // before a real copy. NO other code in this file may call builder.SetTargets / builder.Copy except the lines marked `helper:` or `allow:` (tools\shadow\own_pass_helpers_lint.py).
  void IssueTargets(const LoadedFrame& frame, uint32_t op_index, rdr::FrameBuilder& builder, BindStats& stats, const rdr::ImageRef* retarget = nullptr) {   // S102 C1: `retarget` (renderer ids; a layered ref) replaces colour target 0 after the Map
    rdr::TargetsOp targets = frame.targets[frame.ops[op_index].index];
    current_target_ = targets.color_count ? targets.color[0].image : targets.depth.image;  // S12 (the game's ids)
    current_depth_ = targets.depth.image;  // S10b
    current_color_ = targets.color_count ? targets.color[0].image : 0;
    if (shadows_active_) ShadowsSetTargets(targets);  // S101 lane 123: the game's ids, before Map()
    skipping_target_ = OldHalfSkips(current_target_);  // S101 lane 86 (S102 G3: not the images the new turns own; ported into P3's IssueTargets at the merge)
    if (skipping_target_) {
      if (++shadow_skipped_ == 1)
        rdr::Log("[bind] shadow maps every other frame: %zu shadow targets learned (%zu excluded); frame %llu keeps the last maps",
                 shadow_sets_.targets.size(), shadow_sets_.excluded.size(), static_cast<unsigned long long>(frame_number_));  // S101 lane 86
      return;
    }
    listed_target_ = targets.color_count ? targets.color[0].image : 0;
    if (stats.gallery) for (uint32_t c = 0; c < targets.color_count; ++c) AddToGallery(targets.color[c].image);
    for (uint32_t c = 0; c < targets.color_count; ++c) targets.color[c].image = Map(targets.color[c].image);
    targets.depth.image = Map(targets.depth.image);
    for (uint32_t c = 0; c < rdr::kMaxColorTargets; ++c) targets.color[c].pad = 0;   // S102 C1 review F8: a game ref never carries a view count (only our retarget does)
    targets.depth.pad = 0;
    if (dr_on_ && !retarget && targets.color_count && op_index < dr_set_dest_.size() && dr_set_dest_[op_index]) targets.color[0].image = Map(dr_set_dest_[op_index]);   // S102 C5 (copy_direct): this pass renders straight into the destination of the copy it no longer makes (depth and the other slots stay the game's)
    if (!retarget && targets.color_count && prelight_layered::Planted(prelight_layered::Fault::BadPad) && !pl_bad_pad_done_) targets.color[0].pad = 6, pl_bad_pad_done_ = true;   // planted (C1 review F8): once per run
    if (retarget && targets.color_count) targets.color[0] = *retarget, targets.depth = rdr::ImageRef{};   // S102 C1: the layered pass has no depth target (the planner proved the draw uses none)
    if (fd_last_.valid && (frame_part_open || persist_only_bind)) FdSaveLastPicture(targets, builder);   // S102 C3 F3
    if (post_state_ == 2 && post_claim_.drop[op_index]) {  // S101 lane PO: the target switch of a claimed pass (its pass is not recorded)
      ++post_.targets_dropped;
      return;
    }
    if (post_state_ != 0) post_last_targets_ = targets;   // S101 lane PO: the last target switch the builder was given (a late injection failure re-issues it)
    if (bake_on_) bind_last_targets_ = targets, bind_have_targets_ = true;   // S102 P1: what the bake group restores (only while the switch plans a group: the default path copies nothing)
    if (skip_2d_rows_) row2d_targets_ = targets, row2d_have_ = true;   // S103 L2a review F1: the stream's current targets (re-issued after our rows are inserted)
    builder.SetTargets(targets);   // helper:IssueTargets
    // Isolated redraws (rplay --isolate, live picture isolates): a colour target is cleared to mid-grey the first time this frame binds
    // it, so the draws left out do not show through from the previous frame (the game does not clear targets it fully covers).
    if (stats.isolate && targets.color_count) {
      bool fresh = false;
      for (uint32_t c = 0; c < targets.color_count; ++c) fresh |= isolate_cleared_.insert(targets.color[c].image).second;
      if (fresh) {
        rdr::ClearOp grey;
        grey.flags = rdr::kClearColor;
        grey.full = 1;
        grey.color[0] = grey.color[1] = grey.color[2] = 0.25f;
        grey.color[3] = 1.0f;
        builder.Clear(grey);
      }
    }
  }
  template <class Source>
  void IssueCopy(const LoadedFrame& frame, uint32_t op_index, const Source& capture, rdr::FrameBuilder& builder, BindStats& stats, const rdr::CopyOp* synthetic = nullptr) {   // S102 G11: `synthetic` = a copy the caller makes up (the restore's), not one of the frame's
    rdr::CopyOp copy = synthetic ? *synthetic : copy_override_ ? *copy_override_ : frame.copies[frame.ops[op_index].index];   // (copy_override_: the planted wrong_dest fault only)
    if (post_state_ == 2 && post_claim_.drop[op_index]) {  // S101 lane PO: a copy a claimed draw feeds
      if (alias_on_ && op_index < alias_plan_.elide.size() && alias_plan_.elide[op_index]) ++alias_elide_on_claimed_;   // S102 G12 review F1: a copy the alias plan elided although the post claim drops it: must stay 0
      ++post_.copies_dropped;
      return;
    }
    if (dr_on_ && !synthetic && op_index < dr_copy_.size() && dr_copy_[op_index]) {   // S102 C5 (copy_direct): the pass already rendered into the destination: no copy, no alias
      ++dr_dropped_;
      return;
    }
    if (OldHalfSkips(copy.source.image)) return;  // S12 (S101 lane 86: shadow_sets_; S102 G3: not the images the new turns own; ported into P3's IssueCopy at the merge)
    if ((g_dlss || g_velocity_census) && dlss_depth_ && copy.source.image == dlss_depth_) dlss_scene_done_ = true;  // S10b: the scene's depth copy
    if (stats.gallery) AddToGallery(copy.dest.image);
    if (stats.list) {
      rdr::Log("[bind] copy %ux%u at %d,%d -> %d,%d", copy.source_rect.width, copy.source_rect.height, copy.source_rect.x, copy.source_rect.y, copy.dest_rect.x, copy.dest_rect.y);
      ListImage("source", copy.source.image, capture);
      ListImage("dest", copy.dest.image, capture);
    }
    if ((alias_on_ && op_index < alias_plan_.elide.size() && alias_plan_.elide[op_index]) || op_index == forced_elide_op_) {  // (forced_elide_op_: the planted force_alias test) S101 lane 190: every reader of the destination samples the source for the rest of the frame (copy_alias.h)
      if (copy_alias::g_fault == copy_alias::Fault::SdNoAlias && op_index < alias_plan_.sd.size() && alias_plan_.sd[op_index]) return;   // S102 C3 plant: dropped, but the readers are not re-pointed
      if (copy_alias::g_fault == copy_alias::Fault::FdNoAlias && op_index < alias_plan_.fd.size() && alias_plan_.fd[op_index] == 1) return;   // S102 C3 plant: dropped, readers not re-pointed
      if (copy_alias::g_fault == copy_alias::Fault::FdPresentStale && op_index < alias_plan_.fd.size() && alias_plan_.fd[op_index] == 2) return;   // S102 C3 plant: the Present shows the destination nobody wrote
      SetAlias(copy.dest.image, copy.source.image);
      return;
    }
    if (shadows_active_ && !ShadowsCopy(copy)) return;  // S101 lane 123: whole copies of our images (a duplicate of the last one is dropped)
    const bool had_alias = alias_on_ && copy.dest.image < alias_.size() && alias_[copy.dest.image];   // (for the group's census)
    if (alias_on_ && g_own_plane_fault != OwnPlaneFault::NoAliasEnd && EndAliasOf(copy.dest.image)) return;
    if (had_alias && in_plane_run_ && g_own_plane_fault != OwnPlaneFault::NoAliasEnd) ++plane_.alias_ended;   // S102 G8 (kept in the shared helper): a kept copy into an aliased destination ends its alias BEFORE Map (the planted no_alias_end skips it)
    copy.source.image = Map(copy.source.image);
    copy.dest.image = Map(copy.dest.image);
    last_copy_issued_ = true; builder.Copy(copy);   // helper:IssueCopy
  }

  // ---- S102 P3: the plane shadow chain owned as one pass (plane_pass.h; docs\renderer\PLANE_OURS_WHOLE.md) ---------------------------------------------------------------------------
  // PlanePlan: find the frame's chain: four passes in a row, each a SetTargets whose first colour target has the role ShadowMap, an optional Clear, its draws and one Copy: (0) the mask: 1+ draws of pass site
  // 0x82BF4D38; (1) the downscale: one draw of call site 0x828159FC; (2) and (3) the blurs: one draw each of call site 0x827EEC84. Anything else (another order, a pass with other draws, a missing copy, another role)
  // is counted and the stream's walk keeps the frame.
  struct PlanePassOps { uint32_t set_op = 0, clear_op = ~0u, copy_op = 0, draw_first = 0, draw_count = 0; uint8_t kind = 0; };   // kind 0 mask, 1 downscale, 2 blur
  [[gnu::noinline]] void PlanePlan(const LoadedFrame& frame) {
    plane_passes_.clear(), plane_draw_ops_.clear();
    const uint32_t n = frame.ops.count;
    if (!n || !frame.origins.count) return;
    uint32_t first_mask = ~0u;
    for (uint32_t i = 0; i < n && first_mask == ~0u; ++i) {
      if (frame.ops[i].kind != OpKind::Draw) continue;
      const DrawOrigin* o = frame.OriginOf(frame.ops[i].index);
      if (o && o->pass_site == kPlaneShadowSite) first_mask = i;
    }
    if (first_mask == ~0u) return;   // no plane chain in this frame (the wm30 arena, a menu): nothing to count
    ++plane_.frames_chain;
    const auto decline = [&](uint64_t& why) {
      ++plane_.frames_declined, ++why;
      plane_passes_.clear(), plane_draw_ops_.clear();
    };
    ReplaceSlot* slot = nullptr;
    for (ReplaceSlot& s : replace_slots_)
      if (s.hook && s.enabled == &g_plane_shadow_ours && *s.enabled) slot = &s;
    // S102 P3 (G4b): a frame whose blank chain G4b skips (branch worktree-agent-ae75afe6e8f89e742, flag `plane_blank_on_`) is G4b's. G4b is NOT merged here, so the flag does not exist yet:
    // the line below is written against that name and the main session replaces `false` by `plane_blank_on_` at the merge (one token).
    const bool g4b_blank_frame = plane_blank_on_;   // S102 main (G4b merged): PlaneBlankBegin runs first (line ~337), so a chain G4b skips is never re-issued here
    if (g4b_blank_frame) return decline(plane_.declined_blank);
    if (!slot && !g_own_plane_none) return decline(plane_.declined_no_hook);   // the chain's draws are the plane hook's: without it there is nothing to re-issue them with
    if (shadows_active_ || shadow_skip_frame_ || d3c_on_ || d3_on_ || d3r_on_ || wr_on_) return decline(plane_.declined_other);   // paths the group does not cover (PLANE_OURS_WHOLE.md section 5)
    uint32_t i = first_mask;
    while (i > 0 && frame.ops[i].kind != OpKind::SetTargets) --i;
    if (frame.ops[i].kind != OpKind::SetTargets) return decline(plane_.declined_irregular);
    static const uint8_t kinds[4] = {0, 1, 2, 2};
    for (uint32_t k = 0; k < 4; ++k) {
      PlanePassOps p;
      p.kind = kinds[k];
      if (i >= n || frame.ops[i].kind != OpKind::SetTargets) return decline(plane_.declined_irregular);
      const rdr::TargetsOp& t = frame.targets[frame.ops[i].index];
      if (!t.color_count || ImageRole(t.color[0].image) != uint8_t(SurfaceRole::ShadowMap)) return decline(plane_.declined_role);
      p.set_op = i++;
      if (i < n && frame.ops[i].kind == OpKind::Clear) p.clear_op = i++;
      p.draw_first = uint32_t(plane_draw_ops_.size());
      while (i < n && frame.ops[i].kind == OpKind::Draw) {
        const DrawOrigin* o = frame.OriginOf(frame.ops[i].index);
        const bool is = o && (p.kind == 0 ? o->pass_site == kPlaneShadowSite : p.kind == 1 ? o->call_site == kPlaneDownCallSite : (o->call_site == kPlaneBlurCallSite && o->pass_site != kPlaneShadowSite));
        if (!is) return decline(plane_.declined_irregular);
        plane_draw_ops_.push_back(i++);
        ++p.draw_count;
      }
      if (!p.draw_count || (p.kind != 0 && p.draw_count != 1) || i >= n || frame.ops[i].kind != OpKind::Copy) return decline(plane_.declined_irregular);
      p.copy_op = i++;
      plane_passes_.push_back(p);
    }
    plane_drop_.assign(n, 0);
    for (const PlanePassOps& p : plane_passes_) {
      plane_drop_[p.set_op] = plane_drop_[p.copy_op] = 1;
      if (p.clear_op != ~0u) plane_drop_[p.clear_op] = 1;
      for (uint32_t d = 0; d < p.draw_count; ++d) plane_drop_[plane_draw_ops_[p.draw_first + d]] = 1;
    }
    // review F10c: every mask / downscale / blur site draw of the frame must be inside the chain found (a second chain or a stray draw would be left to the walk with half a pass issued)
    for (uint32_t op = 0; op < n; ++op) {
      if (plane_drop_[op] || frame.ops[op].kind != OpKind::Draw) continue;
      const DrawOrigin* o = frame.OriginOf(frame.ops[op].index);
      if (o && (o->pass_site == kPlaneShadowSite || o->call_site == kPlaneDownCallSite || o->call_site == kPlaneBlurCallSite)) return decline(plane_.declined_extra);
    }
    // review F4: no chain op may be one another planner has already taken. Every planner that drops ops adds its test here: G4 (dead work), the post claim, the bake group, and
    // G3's map turns (map_turns.h, branch worktree-agent-a9c4c2ef877272b3c: `turns_on_ && turns_plan_.drop[op]`) - NOT written here because G3 is not merged: the main session adds the line at the merge.
    uint32_t overlap = 0;
    for (uint32_t op = 0; op < n; ++op) {
      if (!plane_drop_[op]) continue;
      const bool taken = (dead_on_ && dead_plan_.drop[op]) || PostClaimsOp(frame.ops.count, op) || (bake_on_ && bake_drop_[op]) || (g_own_plane_force_overlap && op == plane_passes_[0].set_op) || (turns_on_ && turns_plan_.drop[op]);   // S102 main: G3's turns wired at the merge
      overlap += taken ? 1 : 0;
    }
    if (overlap && !g_own_plane_ignore_overlap) return decline(plane_.declined_overlap);
    plane_overlap_ops_ = overlap;   // (non-zero only under the planted ignore_overlap fault)
    plane_first_op_ = plane_passes_[0].set_op;
    plane_on_ = true;
  }

  // PlaneRun: the group, at the first pass's place. Per pass: the target switch and clear of the pass table, the draws through the normal bind (the plane hook makes the mask and blur draws from the record), the copy.
  // `g_own_plane_none` (test arm) re-issues nothing.
  template <class Source>
  [[gnu::noinline]] void PlaneRun(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    ++plane_.frames_ours;
    plane_.passes += plane_passes_.size();
    plane_.overlap_issued += g_own_plane_none ? 0 : plane_overlap_ops_;
    if (g_own_plane_none) return;
    in_plane_run_ = true;
    ReplaceSlot* slot = nullptr;
    for (ReplaceSlot& s : replace_slots_)
      if (s.hook && s.enabled == &g_plane_shadow_ours && *s.enabled) slot = &s;
    const uint64_t replaced0 = slot ? slot->counters.replaced : 0;
    uint32_t wanted = 0;   // the mask and blur draws of the group (the downscale draw is not the plane hook's)
    uint32_t order[4] = {0, 1, 2, 3};
    if (g_own_plane_fault == OwnPlaneFault::SwapBlur) std::swap(order[2], order[3]);   // PLANTED FAULT: blur z, then blur x
    const auto mask_dest = frame.copies[frame.ops[plane_passes_[0].copy_op].index].dest.image;   // (the game's id: it is mapped with the copy below)
    skipping_target_ = false;
    for (uint32_t k = 0; k < 4; ++k) {
      const PlanePassOps& p = plane_passes_[order[k]];
      IssueTargets(frame, p.set_op, builder, stats);
      ++plane_.targets;
      if (p.clear_op != ~0u) builder.Clear(frame.clears[frame.ops[p.clear_op].index]), ++plane_.clears;
      for (uint32_t d = 0; d < p.draw_count; ++d) {
        BindDraw(frame, frame.draws[frame.ops[plane_draw_ops_[p.draw_first + d]].index], capture, builder, bytes, stats);
        ++plane_.draws;
        if (p.kind == 0) ++plane_.mask_draws, ++wanted;
        else if (p.kind == 1) ++plane_.down_draws;
        else ++plane_.blur_draws, ++wanted;
      }
      if (g_own_plane_fault == OwnPlaneFault::NoCopy && p.kind == 0) continue;                      // PLANTED FAULT: the mask pass's copy into its picture is forgotten
      if (g_own_plane_fault == OwnPlaneFault::WrongDest && p.kind == 1) {                          // PLANTED FAULT: the downscale copies into the mask's picture
        rdr::CopyOp wrong = frame.copies[frame.ops[p.copy_op].index];
        wrong.dest.image = mask_dest;
        copy_override_ = &wrong;
        IssueCopy(frame, p.copy_op, capture, builder, stats);
        copy_override_ = nullptr;
      } else if (g_own_plane_fault == OwnPlaneFault::ForceAlias && p.kind == 1) {                  // TEST (force_alias): this copy is treated as elided; the blur copies after it must end the alias
        const bool keep_on = alias_on_;
        alias_on_ = true, forced_elide_op_ = p.copy_op;
        IssueCopy(frame, p.copy_op, capture, builder, stats);
        forced_elide_op_ = ~0u, alias_on_ = keep_on || alias_on_;
      } else {
        IssueCopy(frame, p.copy_op, capture, builder, stats);
      }
      ++plane_.copies;
    }
    in_plane_run_ = false;
    const uint64_t made = slot ? slot->counters.replaced - replaced0 : 0;
    plane_.draws_ours += made;
    plane_.draws_stream += wanted > made ? wanted - made : 0;
    ++(made >= wanted ? plane_.frames_complete : plane_.frames_partial);
  }

  // ---- S102 L2: the ring-plane shadow bake (plane_bake.h; docs\renderer\SHADOWS_OURS.md "S102 L2") --------------------------------------------------------------------------------------------------------
  // PlaneBakePlan (once per Bind, after every plan whose drop set it must not overlap): finds the frame's plane chain (the same four passes as PlanePlan), the rope proxy's mask draws (the object the hook learned: 3+ eye
  // points in a frame), and decides. pb_on_: the frame's rope draws are left out of the mask pass (when pb_ok_), the bake is kept / made at the chain's first op (PlaneBakeRun) and the composite is made right after the chain's last
  // copy (PlaneBakeComposite); the mat's reads of the game's map are pointed at the composite (pb_sub_, in BindDraw). A frame whose chain the walk does not run (half-rate skip, G3 turn, blank chain) keeps the last drawn frame's
  // decision for the mat's reads, so the picture the mat shows is never half one and half the other.
  struct PbPass { uint32_t set_op = 0, clear_op = ~0u, copy_op = 0; std::vector<uint32_t> draws; uint8_t kind = 0; };
  plane_bake::Counters pb_;
  bool pb_on_ = false, pb_ok_ = false, pb_sub_ = false, pb_last_took_ = false;
  bool pb_dyn_on_ = false, pb_dyn_fullscreen_ = false;
  uint32_t pb_dyn_size_ = 0;
  rdr::ImageId pb_extra_image_ = rdr::kNoImage;
  uint32_t pb_first_op_ = 0, pb_after_op_ = 0, pb_final_id_ = 0, pb_last_target_id_ = 0, pb_mask_color_id_ = 0, pb_mask_depth_id_ = 0, pb_blur_draw_ = 0, pb_clear_op_ = ~0u;
  std::vector<uint8_t> pb_drop_;
  std::vector<uint32_t> pb_rope_ops_;
  std::vector<PbPass> pb_passes_;
  rdr::ImageId pb_raw_ = rdr::kNoImage, pb_raw_depth_ = rdr::kNoImage, pb_a_ = rdr::kNoImage, pb_soft_ = rdr::kNoImage, pb_c_ = rdr::kNoImage;
  uint32_t pb_images_size_ = 0;
  std::vector<float> pb_key_, pb_key_now_;
  uint32_t pb_key_object_ = 0;
  bool pb_key_valid_ = false, pb_c_valid_ = false;   // the key holds a made bake / the composite image holds a made composite
  uint32_t pb_window_ = 0;
  float pb_m_[16] = {}, pb_plane_y_ = 0;   // this frame's screen fit M = PlaneInv * CameraViewProj and the plane height, from a plane draw's own rows at plan time (F10)
  float pb_c7_[2] = {0, 0}, pb_bake_c_[2] = {0, 0};   // the plane position this frame (c7 x, z) and the one the bake was made around (F2: stored with the bake)
  uint32_t pb_miss_run_ = 0, pb_steady_ = 0, pb_idle_ = 0;   // F8: consecutive key misses, frames the key has been steady, frames since the last re-bake
  std::vector<float> pb_prev_key_;
  bool pb_past_chain_ = false;   // the walk is past the chain's last copy (F6)
  bool pb_composited_ = false;   // F6: the composite of this frame has been made
  bool pb_drop_final_copy_ = false;

  // TEST ONLY: see plane_bake.h g_wobble_*. The banks are the capture reader's own copies (the one a draw's constants are packed from), changed in place once per bank.
  void PlaneBakeWobble(const LoadedFrame& frame) {
    const uint64_t g = frame_number_ - 1;   // the replay's frame index (one bind per frame, from 0)
    const uint32_t rope = plane_bake::g_shared.rope_object;
    if (!rope || g < plane_bake::g_wobble_lo || g >= plane_bake::g_wobble_hi || !frame.origins.count) return;
    const float ramp = std::min(1.0f, float(g - plane_bake::g_wobble_lo + 1) / 10.0f);
    const float amp = plane_bake::g_wobble_amp * ramp;
    std::unordered_set<uint32_t> done;
    for (uint32_t i = 0; i < frame.draws.count; ++i) {
      const DrawOrigin* o = frame.OriginOf(i);
      if (!o || o->pass_site != kPlaneShadowSite || !done.insert(frame.draws[i].vs_bank).second) continue;
      uint32_t* bank = const_cast<ConstantBank&>(frame.constant_banks[frame.draws[i].vs_bank]).dwords;
      const auto bump = [&](uint32_t at, float by) {
        float t;
        std::memcpy(&t, &bank[at], 4);
        t += by;
        std::memcpy(&bank[at], &t, 4);
      };
      bump(7 * 4 + 0, plane_bake::g_wobble_c7 * ramp);   // c7.x: the plane position
      bump(1 * 4 + 0, plane_bake::g_wobble_c1 * ramp);   // c1.x: the fade array
      if (o->object != rope) continue;
      for (uint32_t row = plane_bake::kPaletteFirst; row < plane_bake::kPaletteFirst + plane_bake::kPaletteRows; row += 3) {   // 3 rows a bone: row 0 = the x row, its w = the x translation
        float t;
        std::memcpy(&t, &bank[4 * row + 3], 4);
        t += amp;
        std::memcpy(&bank[4 * row + 3], &t, 4);
      }
    }
  }
  ReplaceSlot* PbPlaneSlot() {
    for (ReplaceSlot& s : replace_slots_)
      if (s.hook && s.enabled == &g_plane_shadow_ours && *s.enabled) return &s;
    return nullptr;
  }
  void PbDestroyImages() {
    for (rdr::ImageId* id : {&pb_raw_, &pb_raw_depth_, &pb_a_, &pb_soft_, &pb_c_})
      if (*id != rdr::kNoImage) renderer_.DestroyImage(*id), *id = rdr::kNoImage;
    pb_images_size_ = 0, pb_key_valid_ = false, pb_c_valid_ = false;
  }
  rdr::ImageId PbMakeLike(uint32_t game_id, uint32_t w, uint32_t h, bool depth, bool lasting) {
    const auto found = rec_index_.find(game_id);
    if (found == rec_index_.end() || found->second >= synced_records_.size()) return rdr::kNoImage;
    const ImageRecord& r = synced_records_[found->second];
    rdr::ImageDesc desc;
    desc.width = uint16_t(w), desc.height = uint16_t(h), desc.depth = 1, desc.mips = 1, desc.layers = 1;
    desc.format = rdr::Format(r.format);
    desc.kind = rdr::ImageKind::Image2D;
    desc.usage = depth ? uint8_t(rdr::kUsageTarget) : uint8_t(rdr::kUsageSampled | rdr::kUsageTarget | (lasting ? rdr::kUsageLasting : 0));
    const rdr::ImageId image = renderer_.CreateImage(desc);
    if (image != rdr::kNoImage) renderer_.SetImageClass(image, rdr::kGpuPlane);
    return image;
  }
  // F8: the bake's two TRANSIENT maps (raw + its depth, the half-softened one: about 12 MB at 1024) exist only around a re-bake; the kept ones (softened bake, composite) are made with the size.
  bool PbEnsureTransients() {
    if (pb_raw_ && pb_raw_depth_ && pb_a_) return true;
    const uint32_t size = pb_images_size_;
    if (!pb_raw_) pb_raw_ = PbMakeLike(pb_mask_color_id_, size, size, false, false);
    if (!pb_raw_depth_) pb_raw_depth_ = PbMakeLike(pb_mask_depth_id_, size, size, true, false);
    if (!pb_a_) pb_a_ = PbMakeLike(pb_mask_color_id_, size, size, false, false);
    return pb_raw_ && pb_raw_depth_ && pb_a_;
  }
  void PbDestroyTransients() {
    for (rdr::ImageId* id : {&pb_raw_, &pb_raw_depth_, &pb_a_})
      if (*id != rdr::kNoImage) renderer_.DestroyImage(*id), *id = rdr::kNoImage;
  }
  bool PbEnsureImages(uint32_t size) {
    if (pb_images_size_ == size && pb_soft_ && pb_c_) return true;
    PbDestroyImages();
    const uint32_t c = size > 256 ? size / 2 : size;
    pb_soft_ = PbMakeLike(pb_mask_color_id_, size, size, false, true);
    pb_c_ = PbMakeLike(pb_last_target_id_, c, c, false, true);
    if (!(pb_soft_ && pb_c_)) {
      PbDestroyImages();
      return false;
    }
    pb_images_size_ = size;
    return true;
  }
  [[gnu::noinline]] void PlaneBakePlan(const LoadedFrame& frame, const BindStats& stats) {
    pb_on_ = pb_ok_ = pb_sub_ = false;
    pb_dyn_on_ = false, pb_extra_image_ = rdr::kNoImage;
    plane_bake::g_shared.proj_valid = false;
    const uint64_t guest_frame = frame.header.guest_frame;   // (tools/lint/lint_seen.py: the switch table is acknowledged with the GUEST frame)
    pb_composited_ = false, pb_past_chain_ = false, pb_drop_final_copy_ = false;
    if (!rdr_sw::BoolSeen(rdr_sw::kPlaneBake, plane_bake::g_fview.load(std::memory_order_relaxed), guest_frame)) {
      pb_last_took_ = false;
      if (pb_images_size_) PbDestroyImages();   // F8: everything freed on switch-off
      return;
    }
    const uint32_t n = frame.ops.count;
    if (!n || !frame.origins.count) return;
    uint32_t first_mask = ~0u;
    for (uint32_t i = 0; i < n && first_mask == ~0u; ++i) {
      if (frame.ops[i].kind != OpKind::Draw) continue;
      const DrawOrigin* o = frame.OriginOf(frame.ops[i].index);
      if (o && o->pass_site == kPlaneShadowSite) first_mask = i;
    }
    if (first_mask == ~0u) {   // no plane chain in this frame (a menu, the wm30 arena): nothing to count; the composite no longer stands for any map (F5)
      pb_last_took_ = false, pb_c_valid_ = false;
      return;
    }
    ++pb_.frames;
    if ((++pb_window_ >= 600 || census_flush::Due(g_cf_pb))) {
      pb_window_ = 0;
      rdr::Log("%s", plane_bake::CensusLine(pb_, pb_images_size_).c_str());
    }
    // `kept`: the walk does not run the chain this frame (the mat reads the map the last drawn frame left): the decision of that frame stands for the mat's reads.
    const auto decline = [&](uint64_t& why, bool kept) {
      ++pb_.declined, ++why;
      pb_on_ = false;
      if (kept) pb_sub_ = pb_last_took_ && pb_c_valid_, pb_.half_skip_kept += pb_sub_ ? 1 : 0;
      else pb_last_took_ = false;
    };
    if (velocity_mode_ || stats.isolate || persist_only_bind || frame_part_open) return decline(pb_.d_other, true);
    if (g_own_plane) return decline(pb_.d_other, false);   // P3's group takes the chain whole
    if (plane_blank_on_) return decline(pb_.d_blank, false);   // F5: a skipped blank chain leaves the game's map black: the mat reads it
    if (shadow_skip_frame_) return decline(pb_.d_skip, true);
    if (d3c_on_ || d3_on_ || d3r_on_ || wr_on_) return decline(pb_.d_other, false);
    ReplaceSlot* slot = PbPlaneSlot();
    if (!slot || !plane_bake::g_shared.bake_program_ok) return decline(pb_.d_hook, false);
    if (slot->state == 0) {   // the hook's frame state, as TryReplace / PlaneJoinSlot ask it (the first claimed draw does it otherwise)
      slot->state = slot->hook->FrameReady(d3c_real_frame_ ? *d3c_real_frame_ : frame) ? 1 : 2;
      ++(persist_only_bind ? slot->counters.persist_only : slot->state == 1 ? slot->counters.ours_frames : slot->counters.fallback_frames);
    }
    if (slot->state != 1) return decline(pb_.d_hook, false);
    // ---- the chain: four passes in a row (a mask pass of rope + wrestler proxy draws, the downscale, two blurs), each a SetTargets of a ShadowMap-role target, an optional Clear, its draws and ONE Copy.
    pb_passes_.clear();
    pb_rope_ops_.clear();
    uint32_t i = first_mask;
    while (i > 0 && frame.ops[i].kind != OpKind::SetTargets) --i;
    if (frame.ops[i].kind != OpKind::SetTargets) return decline(pb_.d_chain, false);
    static const uint8_t kinds[4] = {0, 1, 2, 2};
    const uint32_t rope = plane_bake::g_shared.rope_object;
    for (uint32_t k = 0; k < 4; ++k) {
      PbPass p;
      p.kind = kinds[k];
      if (i >= n || frame.ops[i].kind != OpKind::SetTargets) return decline(pb_.d_chain, false);
      const rdr::TargetsOp& t = frame.targets[frame.ops[i].index];
      if (!t.color_count || ImageRole(t.color[0].image) != uint8_t(SurfaceRole::ShadowMap)) return decline(pb_.d_chain, false);
      p.set_op = i++;
      if (i < n && frame.ops[i].kind == OpKind::Clear) p.clear_op = i++;
      while (i < n && frame.ops[i].kind == OpKind::Draw) {
        const DrawOrigin* o = frame.OriginOf(frame.ops[i].index);
        const bool is = o && (p.kind == 0 ? o->pass_site == kPlaneShadowSite : p.kind == 1 ? o->call_site == kPlaneDownCallSite : (o->call_site == kPlaneBlurCallSite && o->pass_site != kPlaneShadowSite));
        if (!is) return decline(pb_.d_chain, false);
        if (p.kind == 0 && rope && o->object == rope) pb_rope_ops_.push_back(i);
        p.draws.push_back(i++);
      }
      if (p.draws.empty() || (p.kind != 0 && p.draws.size() != 1) || i >= n || frame.ops[i].kind != OpKind::Copy) return decline(pb_.d_chain, false);
      p.copy_op = i++;
      pb_passes_.push_back(std::move(p));
    }
    pb_after_op_ = i;   // the op after the chain's last copy: must be a target switch (the composite is a pass of its own and the walk's current targets are not restored)
    if (pb_after_op_ >= n || frame.ops[pb_after_op_].kind != OpKind::SetTargets) return decline(pb_.d_chain, false);
    pb_drop_.assign(n, 0);
    for (uint32_t op = 0; op < n; ++op) {   // every mask / downscale / blur site draw of the frame must be inside the chain found (a second chain or a stray draw: declined)
      if (frame.ops[op].kind != OpKind::Draw) continue;
      const DrawOrigin* o = frame.OriginOf(frame.ops[op].index);
      if (!o || !(o->pass_site == kPlaneShadowSite || o->call_site == kPlaneDownCallSite || o->call_site == kPlaneBlurCallSite)) continue;
      bool inside = false;
      for (const PbPass& p : pb_passes_)
        if (op > p.set_op && op < p.copy_op) inside = true;
      if (!inside) return decline(pb_.d_extra, false);
    }
    {   // a BLANK chain (G4b's rule: every mask draw has a real fade upload, c0.x > 0, and c6.w, the plane module's weight, exactly 0: Bryan's entrance) draws nothing the bake could improve: left to G4b / the stream
      bool all_blank = !pb_passes_[0].draws.empty();
      for (uint32_t d : pb_passes_[0].draws) {
        const uint32_t* bank = frame.constant_banks[frame.draws[frame.ops[d].index].vs_bank].dwords;
        float fade0, weight;
        std::memcpy(&fade0, &bank[0], 4), std::memcpy(&weight, &bank[6 * 4 + 3], 4);
        all_blank = all_blank && fade0 > 0.0f && weight == 0.0f;
      }
      if (all_blank) return decline(pb_.d_blank, false);
    }
    {   // F10 / F2: the frame's screen fit and plane position from a plane mask draw's OWN rows (c9-c12 PlaneInv, c13-c16 camera, c7 the plane's position), not from the hook
      const uint32_t* bank = frame.constant_banks[frame.draws[frame.ops[pb_passes_[0].draws[0]].index].vs_bank].dwords;
      float pinv[16], cam[16], c7[4];
      std::memcpy(pinv, bank + 9 * 4, 64), std::memcpy(cam, bank + 13 * 4, 64), std::memcpy(c7, bank + 7 * 4, 16);
      for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c) {
          double acc = 0;
          for (uint32_t k = 0; k < 4; ++k) acc += double(pinv[4 * r + k]) * double(cam[4 * k + c]);
          pb_m_[4 * r + c] = float(acc);
        }
      pb_plane_y_ = c7[1], pb_c7_[0] = c7[0], pb_c7_[1] = c7[2];
    }
    if (!rope || pb_rope_ops_.empty()) return decline(pb_.d_no_rope, false);
    if (pb_passes_[0].clear_op == ~0u) return decline(pb_.d_chain, false);
    // another planner's drop set holds an op of the chain (G4 dead work, the post claim, the bake group, G3's turns): the chain is theirs; the walk runs (or skips) it as it has it
    bool others = false;
    for (const PbPass& p : pb_passes_) {
      const auto taken = [&](uint32_t op) { return (dead_on_ && dead_plan_.drop[op]) || PostClaimsOp(frame.ops.count, op) || (bake_on_ && bake_drop_[op]) || (turns_on_ && turns_plan_.drop[op]) || (plane_blank_on_ && plane_blank_plan_.drop.size() >= frame.ops.count && plane_blank_plan_.drop[op]); };  // S102 main (L2 review F12): + G4b / G4c's blank-chain drop set (planned just before)
      others = others || taken(p.set_op) || taken(p.copy_op) || (p.clear_op != ~0u && taken(p.clear_op));
      for (uint32_t d : p.draws) others = others || taken(d);
    }
    if (others) return decline(pb_.d_overlap, true);
    pb_first_op_ = pb_passes_[0].set_op;
    pb_clear_op_ = pb_passes_[0].clear_op;
    pb_mask_color_id_ = frame.targets[frame.ops[pb_passes_[0].set_op].index].color[0].image;
    pb_mask_depth_id_ = frame.targets[frame.ops[pb_passes_[0].set_op].index].depth.image;
    pb_last_target_id_ = frame.targets[frame.ops[pb_passes_[3].set_op].index].color[0].image;
    pb_final_id_ = frame.copies[frame.ops[pb_passes_[3].copy_op].index].dest.image;
    pb_blur_draw_ = frame.ops[pb_passes_[3].draws[0]].index;
    if (!pb_mask_depth_id_) return decline(pb_.d_chain, false);
    const int64_t want_size = rdr_sw::IntSeen(rdr_sw::kPlaneBakeSize, int64_t(plane_bake::g_size.load(std::memory_order_relaxed)), guest_frame);   // (fview's --plane-bake=SIZE is the fallback value)
    if (!PbEnsureImages(uint32_t(std::min<int64_t>(2048, std::max<int64_t>(256, want_size))))) return decline(pb_.d_images, false);
    for (uint32_t op : pb_rope_ops_) pb_drop_[op] = 1;
    pb_drop_[pb_passes_[3].copy_op] = 2;   // the chain's last copy (252 -> the game's map) feeds only the mat, which now reads the composite: left out on a taken frame (a removal)
    pb_on_ = true;
  }

  // The state a pass of ours must leave as it found it (the walk's next op is a target switch, so only the walk's own bookkeeping is kept): see BakeRun.
  struct PbKeep { uint32_t target, depth, color, listed; bool skipping; };
  PbKeep PbBegin() {
    PbKeep k = {current_target_, current_depth_, current_color_, listed_target_, skipping_target_};
    current_target_ = pb_mask_color_id_, current_color_ = pb_mask_color_id_, current_depth_ = 0, listed_target_ = pb_mask_color_id_;   // (a shadow-role target while our pass runs)
    skipping_target_ = false;
    return k;
  }
  void PbEnd(const PbKeep& k) {
    current_target_ = k.target, current_depth_ = k.depth, current_color_ = k.color, listed_target_ = k.listed, skipping_target_ = k.skipping;
    pb_dyn_on_ = false, pb_extra_image_ = rdr::kNoImage, plane_bake::g_call = plane_bake::Call{};
  }
  void PbSetTargets(rdr::ImageId color, rdr::ImageId depth, uint32_t size, bool fullscreen_own, rdr::FrameBuilder& builder) {
    rdr::TargetsOp t{};
    t.color[0].image = color, t.color_count = 1, t.depth.image = depth;
    builder.SetTargets(t);   // allow:bake (S102 L2: the plane bake's own passes; the walk's next op is a target switch)
    pb_dyn_on_ = true, pb_dyn_fullscreen_ = fullscreen_own, pb_dyn_size_ = size;
  }
  // The bake's key against the frame's rope draws; true = the bake on hand is this shape.
  bool PbKeyHits(const LoadedFrame& frame) {
    pb_key_now_.assign(pb_rope_ops_.size() * plane_bake::kKeyFloats, 0.0f);
    for (size_t k = 0; k < pb_rope_ops_.size(); ++k) plane_bake::PackKey(frame.constant_banks[frame.draws[frame.ops[pb_rope_ops_[k]].index].vs_bank].dwords, pb_key_now_.data() + k * plane_bake::kKeyFloats);
    if (!pb_key_valid_ || pb_key_object_ != plane_bake::g_shared.rope_object || pb_key_.size() != pb_key_now_.size()) return false;
    if (plane_bake::g_fault == plane_bake::Fault::StaleKey) return true;   // PLANT: the key never invalidates
    return plane_bake::KeyRatio(pb_key_.data(), pb_key_now_.data()) < 1.0f;
  }

  template <class Source>
  [[gnu::noinline]] void PlaneBakeRun(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    if (plane_bake::g_fault == plane_bake::Fault::BakeLate) PlaneBakeComposite(frame, capture, builder, bytes, stats, true);   // PLANT: the composite before the bake
    if (PbKeyHits(frame)) {
      pb_ok_ = true;
      ++pb_.hits;
      pb_miss_run_ = 0;
      if (++pb_idle_ > 120) PbDestroyTransients();   // F8: the two transient maps (raw + depth, the half-softened) are freed after 120 frames without a re-bake
      pb_prev_key_ = pb_key_now_;
      return;
    }
    // F8: a rope that keeps moving (a bounce) would re-bake every frame: after 2 misses in a row the game's chain draws the ropes until the shape has been steady for 8 frames
    const bool steady_now = pb_prev_key_.size() == pb_key_now_.size() && plane_bake::KeyRatio(pb_prev_key_.data(), pb_key_now_.data()) < 1.0f;
    pb_steady_ = steady_now ? pb_steady_ + 1 : 0;
    pb_prev_key_ = pb_key_now_;
    if (++pb_miss_run_ > 2 && pb_steady_ < 8) {
      pb_ok_ = false, pb_last_took_ = false;
      ++pb_.declined, ++pb_.unsteady;
      return;
    }
    if (!PbEnsureTransients()) {
      pb_ok_ = false;
      ++pb_.declined, ++pb_.d_images;
      return;
    }
    pb_idle_ = 0;
    ReplaceSlot* slot = PbPlaneSlot();
    const uint32_t size = pb_images_size_;
    plane_bake::Call& call = plane_bake::g_call;
    const PbKeep keep = PbBegin();
    const uint64_t replaced0 = slot ? slot->counters.replaced : 0;
    // pass 1: the rope proxy's mask draws, through the plane hook's own mask program with an ortho matrix, into the raw bake (colour + depth, cleared like the game's mask pass)
    PbSetTargets(pb_raw_, pb_raw_depth_, size, false, builder);
    rdr::ClearOp clear = frame.clears[frame.ops[pb_clear_op_].index];
    clear.full = 1;
    if (!(clear.flags & rdr::kClearDepth)) clear.depth = 1.0f;
    clear.flags = uint8_t(clear.flags | rdr::kClearColor | rdr::kClearDepth);
    builder.Clear(clear);
    call = plane_bake::Call{};
    call.variant = plane_bake::kVariantBake, call.bake_size = size, call.target_w = size;
    call.centre[0] = pb_c7_[0], call.centre[1] = pb_c7_[1], call.extent = plane_bake::g_extent;
    uint64_t made = 0;
    for (uint32_t op : pb_rope_ops_) {
      const uint64_t before = slot ? slot->counters.replaced : 0;
      BindDraw(frame, frame.draws[frame.ops[op].index], capture, builder, bytes, stats);
      made += (slot ? slot->counters.replaced : 0) - before;
      ++pb_.bake_draws;
    }
    const bool all_ours = slot && made == pb_rope_ops_.size();
    // passes 2 and 3: soften along x, then along z (the stream's last blur draw is the template: its place, fixed state and sampler; the program, count and blocks are ours)
    const Draw& tmpl = frame.draws[pb_blur_draw_];
    const auto pass = [&](plane_bake::Variant v, rdr::ImageId target, rdr::ImageId source) {
      PbSetTargets(target, rdr::kNoImage, size, true, builder);
      call = plane_bake::Call{};
      call.variant = v, call.slot0 = source, call.target_w = size, call.bake_size = size;
      BindDraw(frame, tmpl, capture, builder, bytes, stats);
    };
    pass(plane_bake::kVariantSoftenX, pb_a_, pb_raw_);
    pass(plane_bake::kVariantSoftenZ, pb_soft_, pb_a_);
    pb_.passes_added += 3;
    PbEnd(keep);
    (void)replaced0;
    if (!all_ours) {   // a rope draw stayed the stream's (the hook refused it): nothing drawn here is trusted; the rope draws stay in the game's mask pass this frame
      pb_key_valid_ = false;
      pb_c_valid_ = false;
      pb_ok_ = false;
      ++pb_.declined, ++pb_.d_hook;
      return;
    }
    pb_key_ = pb_key_now_, pb_key_object_ = plane_bake::g_shared.rope_object, pb_key_valid_ = true;
    pb_bake_c_[0] = pb_c7_[0], pb_bake_c_[1] = pb_c7_[1];   // F2: the composite reads the bake around the centre it was MADE around, not this frame's
    pb_ok_ = true;
    ++pb_.rebakes;
  }

  template <class Source>
  [[gnu::noinline]] void PlaneBakeComposite(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats, bool early = false) {
    if (early != (plane_bake::g_fault == plane_bake::Fault::BakeLate)) return;   // (the planted bake_late fault moves the composite before the bake: it is made once, there)
    if (!pb_ok_ && !early) {   // the bake was not made (a refused rope draw): the frame is the stream's
      pb_last_took_ = false;
      return;
    }
    if (early && !pb_c_valid_) return;   // (planted bake_late, first frame: nothing to composite yet)
    const uint32_t size = pb_images_size_, c = size > 256 ? size / 2 : size;
    const PbKeep keep = PbBegin();
    PbSetTargets(pb_c_, rdr::kNoImage, c, true, builder);
    plane_bake::Call& call = plane_bake::g_call;
    call = plane_bake::Call{};
    call.variant = plane_bake::kVariantComposite;
    call.slot0 = Map(pb_last_target_id_), call.slot1 = pb_soft_, call.target_w = c, call.bake_size = size;
    call.centre[0] = plane_bake::g_fault == plane_bake::Fault::CentreNow ? pb_c7_[0] + 30.0f : pb_bake_c_[0], call.centre[1] = plane_bake::g_fault == plane_bake::Fault::CentreNow ? pb_c7_[1] : pb_bake_c_[1], call.extent = plane_bake::g_extent;
    std::memcpy(call.m, pb_m_, sizeof(call.m)), call.plane_y = pb_plane_y_;
    pb_extra_image_ = pb_soft_;
    BindDraw(frame, frame.draws[pb_blur_draw_], capture, builder, bytes, stats);
    ++pb_.passes_added;
    PbEnd(keep);
    pb_composited_ = true;
    pb_c_valid_ = true;
    pb_sub_ = true;
    pb_last_took_ = true;
    ++pb_.taken;
    pb_.rope_draws_left_out += pb_rope_ops_.size();
  }
