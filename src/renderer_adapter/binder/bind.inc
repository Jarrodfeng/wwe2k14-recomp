
  template <class Source>
  void Bind(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, BindStats& stats) {
    scene_frame_id_ = scene_frame_image_ != rdr::kNoImage ? FindFrameImage(frame) : 0u;   // S103 lane S
    scene_bind_state_ = scene_frame_image_ == rdr::kNoImage ? 0 : scene_frame_id_ ? 1 : 2;   // S103 CLK2 (review M1)
    fc_remnant_noted_ = false;   // S103 L2b: the first remnant draw of this Bind is the one noted
    if (scene_frame_image_ != rdr::kNoImage && !scene_frame_id_) ++scene_frame_unfound_, fallback_census::NoteRender(frame.header.guest_frame, "frame_image_unfound");   // S103 L2b (review F3): SetSceneFrame was set, the frame image was not found
    alias_images_dirty_ = alias_images_dirty_ || scene_frame_image_ != rdr::kNoImage || scene_frame_was_on_;
    scene_frame_was_on_ = scene_frame_image_ != rdr::kNoImage;
    struct SceneFrameEnd { Binder* b; ~SceneFrameEnd() { b->scene_frame_image_ = rdr::kNoImage; b->scene_frame_depth_ = rdr::kNoImage; b->scene_frame_id_ = 0; b->skip_2d_rows_ = false; b->row2d_insert_ = nullptr; b->row2d_inserted_ = false; b->row2d_have_ = false; } } scene_frame_end{this};   // (S103 L2a: + the 2D row skip)
    if (scene_frame_id_) {   // S103 lane S (REMNANT_SEAM.md H1, SCENE_SKIPS.md 8): with the scene walks skipped, nothing in the stream points the remnant at the frame image (the post
      // effects issued that switch); the binder does, before the first op: our frame + our depth (the remnant never tests depth; its depth-only clear clears ours, harmlessly)
      rdr::TargetsOp t{};
      t.color[0].image = scene_frame_image_;
      t.color_count = 1;
      t.depth.image = scene_frame_depth_;
      builder.SetTargets(t);   // helper:IssueTargets (the synthetic switch of an OURS frame)
      if (skip_2d_rows_) row2d_targets_ = t, row2d_have_ = true;   // S103 L2a review F1: what the insert restores
      current_target_ = current_color_ = scene_frame_id_;
      current_depth_ = 0;
    }
    std::vector<uint8_t> bytes;
    occurrences_.clear();
    keyed_draws_.clear();
    isolate_cleared_.clear();
    ++frame_number_;
    plan_memo::g_mask.store(uint32_t(rdr_sw::IntSeen(rdr_sw::kPlanMemo, int64_t(plan_memo::g_fallback.load(std::memory_order_relaxed)), frame.header.guest_frame)) & plan_memo::kAll, std::memory_order_relaxed);   // S102 R23-P: read every bind
    if (plan_memo::g_mask.load(std::memory_order_relaxed) && (++plan_memo_window_ >= 600 || census_flush::Due(g_cf_plan_memo))) plan_memo_window_ = 0, rdr::Log("%s", plan_memo::Line().c_str());   // S102 R23-P: the live census line
    g_crowd_rank.SetFast(g_crowd_fast && (g_crowd_fast_steps & 2) != 0), g_crowd_fast_fault_id = g_crowd_fast_fault == "early_keep" ? 1 : g_crowd_fast_fault == "early_first" ? 2 : g_crowd_fast_fault == "rank_radius_off" ? 3 : 0, g_crowd_rank.radius = g_crowd_fast_fault_id == 3 ? 12.0f : CrowdRankTracker::kTrackRadius;  // S102 R21-C: the flat rank table (wwe2k14_rdr_crowd_fast; a flip moves the memory across)
    g_crowd_rank.NextFrame(frame.header.guest_frame);  // S101 lane 234
    if (plane_bake::g_wobble_amp != 0.0f || plane_bake::g_wobble_c7 != 0.0f || plane_bake::g_wobble_c1 != 0.0f) PlaneBakeWobble(frame);   // S102 L2 (TEST ONLY, fview --plane-bake-wobble)
    if (g_const_override.active) g_const_override.BeginBind();  // lane 164 (fview only)
    after_post_ = false;  // S3
    // S102 lane G3 step 1: the half rate's parity counts DRAWN frames, not binds. A lasting-only bind (a skipped frame's), an isolate and the second part of a frame split at a readback are binds
    // with no new picture: counting them made the alternation irregular on a machine that drops frames (the Surface; R7 hazard H1). (map_turns.h DrawnClock)
    drawn_frame_ = drawn_clock_.Bind(persist_only_bind, stats.isolate, frame_part_open, frame.header.guest_frame);
    // The half rate's clock: wwe2k14_rdr_half_rate_drawn_clock (PerFrame, default OFF) = drawn frames; OFF = the old per-bind count, exactly as before (frame_number_, every bind).
    const bool half_drawn = map_turns::g_half_rate_drawn_clock.load(std::memory_order_relaxed);
    const uint64_t half_n = (half_drawn ? drawn_frame_ : frame_number_) + map_turns::g_half_clock_offset;
    shadow_skip_frame_ = g_shadow_half && (!half_drawn || !persist_only_bind) && (half_n & 1) && half_n > 8 && !shadow_sets_.Empty();  // S12 (S101 lane 86: shadow_sets_); S102 G3 step 1 (a lasting-only bind keeps its copies under the drawn clock)
    skipping_target_ = false;
    dlss_vp_valid_ = dlss_scene_done_ = dlss_copied_ = dlss_copy_pending_ = false;  // S10b
    kept_after_dlss_copy_ = 0;  // lane 201
    stage_scene_.Reset();  // S101 lane 59
    stage_replaced_ = false;
    stage_frame_on_ = stage_view::FrameOn(frame.header.flags);  // the frame's own stamp (kFrameStagePlaying), not the director's state now
    dlss_depth_ = 0;
    velocity_draws_.clear();  // S10b
    crowd_state_ = 0, crowd_injected_ = false;  // S101 lane 35
    bake_on_ = false, bind_have_targets_ = false;   // S102 P1
    plane_on_ = false;   // S102 P3
    if (g_own_card_bake_before_dead && g_own_card_bake && bake_hook_ && !velocity_mode_ && !stats.isolate) BakePlan(frame, true);   // PLANTED FAULT p1_before_g4: the old order (P1 plans before G4 and takes every bake op)
    {
      const auto t0 = std::chrono::steady_clock::now();
      plan_memo::Timer tm_(plan_memo::kTmPost);
      PlanPost(frame, capture, stats);  // S101 lane PO: ask the post hook which of this frame's draws / ops are its (post_state_ 0: none)
      if (g_post_ours && post_hook_) post_.ms_ours += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    cards_state_ = 0;                           // S101 lane CD
    crowd_kept_base_ = crowd_.draws_kept;       // S101 lane 50 (the frame's kept count for crowd_skip_core.h)
    replace_active_ = false;  // S101 lane 41: any replace hook whose switch is on
    for (ReplaceSlot& slot : replace_slots_) {
      slot.state = 0;
      replace_active_ = replace_active_ || (slot.hook && *slot.enabled);
    }
    thin_frame_on_ = false;  // lane 122 (D1 thin draws)
    if (ThinAnyActive() && replace_active_ && !velocity_mode_ && !d3_skip_observe) { plan_memo::Timer tm_(plan_memo::kTmThinBegin); ThinBeginFrame(frame, stats.isolate != nullptr); }  // lane 129 (KC): any class; (review 3: an isolate bind is read-only)
    d3_on_ = d3_chain_frame && d3::g_arena_d3 && thin_frame_on_ && !thin_read_only_ && ThinClassActive(thin::Cls::Arena);  // lane 138 (D3): the arena's markers are met and its sequences learned only while the thin frame is on
    if (d3_on_) { d3t::Scope t(d3t::kArenaBegin); D3BeginFrame(); }  // lane 138
    d3c_on_ = d3_chain_frame && d3::g_caster_d3 && !d3_skip_observe && cc_ && cc_->d3c_.caster() && !thin_read_only_;   // (review S1: only a core that is the caster class's, with its own packets / marks / gate, runs)   // lane 217 (E2b): the caster class's own chain; independent of the thin store (a caster part has no D1 form)
    if (d3c_on_) { d3t::Scope t(d3t::kCasterBegin); D3CBeginFrame(); }
    d3r_on_ = d3_chain_frame && d3::g_ring_d3 && rc_ && rc_->d3r_.ring() && thin_frame_on_ && !thin_read_only_ && ThinClassActive(thin::Cls::Ring);   // lane 229 (Ring D3 R3): the ring's markers are met and its sequences learned only while the thin frame is on and the ring class is thin
    if (d3r_on_) { d3t::Scope t(d3t::kRingBegin); D3RBeginFrame(); }
    wr_on_ = d3::g_wrestlers_d3 && wc_ && wc_->w_.wrestlers() && thin_frame_on_ && !thin_read_only_ && ThinClassActive(thin::Cls::Wrestlers);   // lane 227 (H2): the wrestlers' markers are met and their sequences learned only while the thin frame is on (their templates are the D1 store's)
    if (wr_on_) D3WBeginFrame();
    EvictGeometry();
    { plan_memo::Timer tm_(plan_memo::kTmDead); DeadWorkBegin(frame, stats); }  // S102 lane G4: before the alias plan (a dropped copy is not its to judge)
    if (!g_own_card_bake_before_dead && g_own_card_bake && bake_hook_ && !velocity_mode_ && !stats.isolate) BakePlan(frame, false);   // S102 P1 (review F1): after G4: a pass G4 cuts is not ours; only the live ones are grouped
    { plan_memo::Timer tm_(plan_memo::kTmTurns); TurnsBegin(frame, stats); }  // S102 lane G3: the maps' turns (after G4's plan, before the shadow classification and the alias plan)
    { plan_memo::Timer tm_(plan_memo::kTmOwnGroup); OwnGroupPlan(frame, stats); }   // S102 P4P5: the shadow module's and the rim map's passes as groups (a no-op while both switches are off)
    { plan_memo::Timer tm_(plan_memo::kTmPlaneBlank); PlaneBlankBegin(frame, stats); }  // S102 lane G4b: the blank plane shadow chain (plane_blank.h): after every plan whose drop set it must not overlap (dead_plan_, bake_drop_, post_claim_), before the alias plan
    { plan_memo::Timer tm_(plan_memo::kTmPreLightA); PreLightAnalyticBegin(frame, stats); }  // S102 lane L1 (prelight_analytic.h): after every plan whose drop set it must not overlap, before C1's layered pass (which then sees this drop set) and the alias plan
    { plan_memo::Timer tm_(plan_memo::kTmPreLightC); PreLightComputeBegin(frame, capture, stats); }  // S102 lane L1b (prelight_compute.h): after L1's drops (it declines on overlap), before C1's layered pass (which then sees this drop set) and the alias plan
    { plan_memo::Timer tm_(plan_memo::kTmPreLightL); PreLightBegin(frame, stats); }  // S102 lane C1 (prelight_layered.h): after every plan whose drop set it must not overlap, before the alias plan (the group's copies are an op another hook drops)
    { plan_memo::Timer tm_(plan_memo::kTmShadows); ShadowsOursBegin(frame, stats); }  // S101 lane 123 (SH2): classify this frame's shadow images, make ours
    { plan_memo::Timer tm_(plan_memo::kTmPlaneBake); PlaneBakePlan(frame, stats); }  // S102 L2: the ring-plane shadow bake (after every plan whose drop set it must not overlap: G4, G4b, the post claim, P1, G3)
    { plan_memo::Timer tm_(plan_memo::kTmAlias); AliasBegin(frame, stats); }  // S101 lane 190: the frame's copy-elide set (after the shadow classification and the post claim: both change which copies are ours to judge)
    { plan_memo::Timer tm_(plan_memo::kTmFrameCutsBegin); FrameCutsBegin(frame, stats); }  // S102 G11: the frame buffer cuts (frame_cuts.h): the switches are read here, every frame
    if (g_own_plane && !velocity_mode_ && !stats.isolate && !persist_only_bind) PlanePlan(frame);   // S102 P3: the plane chain as one group (after the alias plan: a copy it elides is aliased, not recorded)
    rt_spans_on_ = false;
    if (rt_level_ >= 2 && frame_cuts_on_) {   // S102 R18-B1: after every plan whose drop set it must not overlap (plane_drop_ included)
      const auto t0 = std::chrono::steady_clock::now();
      plan_memo::Timer tm_(plan_memo::kTmRtSpan);
      RtSpanPlan(frame, capture, stats);
      rt_.census.plan_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), ++rt_.census.plan_frames;
    }
    if (plan_memo::g_census_on.load(std::memory_order_relaxed) && !velocity_mode_ && !stats.isolate && !persist_only_bind && !frame_part_open) PlanMemoCensus(frame);   // S102 R23-P: tool only (rplay / fview --plan-memo-census)
    stream_particle_draws_ = 0;  // S103 PF: counted below at every draw whose origin is a particle recorder site (any switch state)
    // S101 lane 67: particle runs. On: the look-ahead at each particle draw (ParticleBegin) folds the run into the first draw's bind; the folded ops are skipped below.
    static const int particle_runs_vs = VariantByName("yParticleRuns_runs_vertex");
    particles_on_ = g_particles_ours && !stats.isolate && !velocity_mode_ && particle_runs_vs >= 0;
    particle_resume_op_ = -1;
    particle_run_.active = false;
    if (particles_on_) {
      particle_skip_.assign(frame.ops.count, 0);
      particle_draws_base_ = particle_census_.draws;
      particle_have_texture_ = false;
    }
    for (const Op& op : frame.ops) {
      draw_trace_ = BindTrace{};  // S101 lane 2 (fview)
      const uint32_t op_index = uint32_t(&op - frame.ops.data);  // S101 lane 67
      if (builder.GroupsOn()) builder.SetGroup(op.kind == OpKind::Draw ? GroupOfOrigin(frame.OriginOf(op.index)) : uint8_t(rdr::kGrpOther));   // S102 R24-F: the group the draws made while this op is handled are tagged with
      if (frame_cuts_on_) { plan_memo::Timer tm_(plan_memo::kTmFrameCutsOp); FrameCutsOp(frame, op, op_index); }   // S102 G11: the write versions (every op, whatever another plan does with it)
      if (rt_spans_on_ && op_index < rt_role_.size() && rt_role_[op_index] && RtSpanOp(frame, op, op_index, builder, stats)) {   // S102 R18-B1: an op of the scene target's round trip, left out
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      if (pb_on_) {   // S102 L2: the ring-plane shadow bake: the composite right after the chain's last copy, the bake before its first op, the rope draws left out of the mask pass
        if (op_index == pb_after_op_) {
          pb_past_chain_ = true;
          PlaneBakeComposite(frame, capture, builder, bytes, stats);
        }
        if (op_index == pb_first_op_) PlaneBakeRun(frame, capture, builder, bytes, stats);
        if (pb_ok_ && pb_drop_[op_index] == 2 && op.kind == OpKind::Copy) {   // the chain's last copy (see PlaneBakePlan): left out on a taken frame
          draw_trace_.outcome = BindOutcome::LeftOut;
          if (stats.trace) {
            draw_trace_.frame_ops = builder.Finish().ops.count;
            stats.trace->push_back(draw_trace_);
          }
          continue;
        }
        if (pb_ok_ && pb_drop_[op_index] == 1 && plane_bake::g_fault != plane_bake::Fault::RopesKept) {
          ++stats.draws;
          ++stats.settings_out;
          ++plane_bake_dropped_;
          draw_trace_.outcome = BindOutcome::LeftOut;
          draw_trace_.fate = kFateDeadWork;
          if (stats.trace) {
            draw_trace_.frame_ops = builder.Finish().ops.count;
            stats.trace->push_back(draw_trace_);
          }
          continue;
        }
      }
      if (plane_on_ && plane_drop_[op_index]) {  // S102 P3: an op of the plane chain: the stream's walk leaves it out; the first one of the frame re-issues the whole group as ours
        ++plane_.ops_taken;
        if (op_index == plane_first_op_) PlaneRun(frame, capture, builder, bytes, stats);
        draw_trace_.outcome = BindOutcome::LeftOut;
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      if (bake_on_ && bake_drop_[op_index]) {  // S102 P1: an op of a bake pass: the stream's walk leaves it out; the first one of the frame re-issues the whole group as ours
        ++bake_.ops_taken;
        if (op_index == bake_first_op_) BakeRun(frame, capture, builder, bytes, stats);
        draw_trace_.outcome = BindOutcome::LeftOut;
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      // S102 P4P5 (IssueCopy / IssueTargets, P3's shared helpers): this lane issues no copy or target switch itself: a unit's ops fall through to the walk's own SetTargets / Copy cases below, so there is no site to move to the helpers.
      if (og_on_ && og_plan_.group_of_op[op_index] != own_group::kNone && OwnGroupOp(op_index, op.kind == OpKind::Draw, op.kind == OpKind::Copy)) {  // S102 P4P5: an op of a group the `none` arm leaves out
        if (op.kind == OpKind::Draw) {
          ++stats.draws;
          ++stats.settings_out;
        }
        draw_trace_.outcome = BindOutcome::LeftOut;
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      if (particles_on_ && particle_skip_[op_index]) {  // S101 lane 67: a draw folded into the run's merged draw
        ++stats.draws;
        draw_trace_.outcome = BindOutcome::LeftOut;
        draw_trace_.fate = kFateParticleRun;
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      if (dead_on_ && dead_plan_.drop[op_index]) {  // S102 lane G4: a pass nothing reads (dead_work.h): none of its ops is recorded
        if (op.kind == OpKind::Draw) {
          ++stats.draws;
          ++stats.settings_out;
          draw_trace_.outcome = BindOutcome::LeftOut;
          draw_trace_.fate = kFateDeadWork;
          if (stats.trace) {
            draw_trace_.frame_ops = builder.Finish().ops.count;
            stats.trace->push_back(draw_trace_);
          }
        }
        continue;
      }
      if ((turns_on_ && turns_plan_.drop[op_index]) || (plane_blank_on_ && plane_blank_plan_.drop[op_index])) {  // S102 lane G3 (map_turns.h): a map whose turn is off this drawn frame; S102 lane G4b (plane_blank.h): the blank plane shadow chain: none of its ops is recorded
        if (op.kind == OpKind::Draw) {
          ++stats.draws;
          ++stats.settings_out;
          draw_trace_.outcome = BindOutcome::LeftOut;
          draw_trace_.fate = kFateDeadWork;
          if (stats.trace) {
            draw_trace_.frame_ops = builder.Finish().ops.count;
            stats.trace->push_back(draw_trace_);
          }
        }
        continue;
      }
      if (pa_on_ && pa_plan_.drop[op_index]) {  // S102 L1: an op of a pre-light group whose cube is not baked this frame (its readers evaluate the lights themselves): none of its ops is recorded
        if (op.kind == OpKind::Draw) {
          ++stats.draws;
          ++stats.settings_out;
          ++pa_planner_.census.bake_draws_dropped;
          draw_trace_.outcome = BindOutcome::LeftOut;
          draw_trace_.fate = kFateDeadWork;
        } else if (op.kind == OpKind::Copy) {
          ++pa_planner_.census.copies_dropped;
        }
        ++pa_planner_.census.ops_dropped;
        if (op.kind == OpKind::Draw && stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      if (pc_on_ && op_index == pc_plan_.first_op) PreLightComputeDispatch(frame, builder);   // S102 L1b: where the first taken slot began, ONE dispatch writes every taken cube (the slots' own ops are left out just below)
      if (pc_on_ && pc_plan_.drop[op_index]) {   // S102 L1b: an op of a pre-light slot whose cube the dispatch writes: none of its ops is recorded
        if (op.kind == OpKind::Draw) {
          ++stats.draws;
          ++stats.settings_out;
          draw_trace_.outcome = BindOutcome::LeftOut;
          draw_trace_.fate = kFateDeadWork;
          if (stats.trace) {
            draw_trace_.frame_ops = builder.Finish().ops.count;
            stats.trace->push_back(draw_trace_);
          }
        }
        ++pc_planner_.census.ops_dropped;
        continue;
      }
      if (pl_on_ && pl_plan_.role[op_index]) {  // S102 C1: an op of a pre-light group taken this frame: its strip is not made, its draw is one layered pass into the cube, its copies are left out
        PreLightOp(frame, capture, op_index, builder, bytes, stats);
        if (stats.trace) {
          draw_trace_.frame_ops = builder.Finish().ops.count;
          stats.trace->push_back(draw_trace_);
        }
        continue;
      }
      switch (op.kind) {
        case OpKind::SetTargets:
          IssueTargets(frame, op_index, builder, stats);   // S102 P3 (F5): the one place a target switch reaches the builder (shared with the owned passes)
          break;
        case OpKind::Clear:
          if (skipping_target_) break;  // S12
          if (post_state_ == 2 && post_claim_.drop[op_index]) {  // S101 lane PO: a clear of a claimed pass's target; 2 = only its colour part goes (the depth part is the HUD's)
            ++post_.clears_dropped;
            if (post_claim_.drop[op_index] == 2) {
              rdr::ClearOp kept = frame.clears[op.index];
              kept.flags = uint8_t(kept.flags & ~rdr::kClearColor);
              if (kept.flags) builder.Clear(kept);
            }
            break;
          }
          if (stats.list) {  // S100: the far crowd cards (capture s100cards) - which clears touch the scene's depth
            const rdr::ClearOp& c = frame.clears[op.index];
            rdr::Log("[bind] clear flags %u depth %g stencil %u full %u; colour target %u depth target %u", uint32_t(c.flags), double(c.depth),
                     uint32_t(c.stencil), uint32_t(c.full), current_color_, current_depth_);
          }
          if (shadows_active_ && cur_ours_) {  // S101 lane 123: a clear of one of our images: the rectangle grows with it
            rdr::ClearOp scaled = frame.clears[op.index];
            ShadowsClear(scaled);
            builder.Clear(scaled);
            break;
          }
          builder.Clear(frame.clears[op.index]);
          break;
        case OpKind::Copy:
          last_copy_issued_ = false;
          IssueCopy(frame, op_index, capture, builder, stats);   // S102 P3 (F5): the one place a copy reaches the builder (shared with the owned passes)
          if (frame_cuts_on_) FrameCutsNoteCopy(frame, op, last_copy_issued_);   // S102 G11
          break;
        case OpKind::Present: {
          rdr::PresentOp present = frame.presents[op.index];
          if (stats.list) ListImage("present", present.source.image, capture);
          const uint32_t presented_id = present.source.image;
          const bool present_aliased = alias_on_ && presented_id < alias_.size() && alias_[presented_id] != 0;   // S102 C3 F3: the picture is the source image itself (its copy into the Present image was not made)
          const rdr::ImageId present_dest = presented_id < image_ids_.size() ? image_ids_[presented_id] : rdr::kNoImage;
          present.source.image = Map(present.source.image);
          if ((!persist_only_bind && !frame_part_open) || copy_alias::g_fault == copy_alias::Fault::NoPresentGuard) {
            last_present_ = present;
            fd_last_ = {};
            if (present_aliased && present_dest != rdr::kNoImage && presented_id < stage_images_.size()) fd_last_ = {true, present.source.image, present_dest, stage_images_[presented_id].width, stage_images_[presented_id].height};
          }  // S102 G8 F1: a persist-only / part bind shows nothing: it must not replace the last picture (with the alias on, its Present maps a destination nobody wrote)
          if (dlss_copy_pending_) CopyBeforeHud(capture, builder);  // S10b: a frame without a HUD: the finished picture
          StageViewCopy(presented_id, present.source.image, builder);  // S101 lane 59: a frame stamped "playing": the scene's target replaces the picture (after CopyBeforeHud, which sets dlss_copied_)
          if (g_dlss) VelocityPass(frame, capture, builder, bytes, stats);  // S10b: the wrestlers' own motion
          if (g_dlss) {  // S10b
            rdr::DlssInputs in;
            in.valid = dlss_vp_valid_ && dlss_depth_ && dlss_color_ != rdr::kNoImage && dlss_copied_ && !stage_replaced_;  // S101 lane 59: not for a replaced picture
            if (in.valid) {
              in.color = dlss_color_;
              in.depth = Map(dlss_depth_);
              in.object_motion = velocity_drawn_ ? velocity_image_ : rdr::kNoImage;
              std::memcpy(in.view_proj, dlss_vp_, sizeof(dlss_vp_));
              std::memcpy(in.prev_view_proj, dlss_prev_vp_valid_ ? dlss_prev_vp_ : dlss_vp_, sizeof(dlss_vp_));
              in.reset = !dlss_prev_vp_valid_;
              // The phase the reader moved this frame's scene draws by (its header, kFrameDlssJitter); none: no jitter.
              if (frame.header.flags & kFrameDlssJitter) rdr::DlssJitter(frame.header.flags >> 24, g_dlss_phases, in.jitter);
              builder.SetDlss(in);
              std::memcpy(dlss_prev_vp_, dlss_vp_, sizeof(dlss_vp_));
            }
            dlss_prev_vp_valid_ = in.valid;
          }
          builder.Present(present);
          break;
        }
        case OpKind::Draw:
          if (!d3_chain_frame && (frame.draws[op.index].flags & kDrawNoGeometry) && frame.OriginOf(op.index) && d3::IsAnyMarkerCall(frame.OriginOf(op.index)->call_site)) d3t::g_marker_off_chain.fetch_add(1, std::memory_order_relaxed);   // S102 K3 review F2: a marker on a frame the chains did not run for would be a silent hole (must stay 0)
          if (d3c_on_ && frame.OriginOf(op.index) && d3::IsCasterMarkerCall(frame.OriginOf(op.index)->call_site)) {  // lane 217 (E2b): a caster marker stands for one pass entry of a model the game did not build
            D3CMarker(frame, op.index, capture, builder, bytes, stats);
            break;
          }
          if (d3r_on_ && frame.OriginOf(op.index) && d3::IsRingMarkerCall(frame.OriginOf(op.index)->call_site)) {  // lane 229 (Ring D3 R3): a ring marker stands for a ring model call the game did not build
            D3RMarker(frame, op.index, capture, builder, bytes, stats);
            break;
          }
          if (wr_on_ && frame.OriginOf(op.index) && d3::IsWrestlerMarkerCall(frame.OriginOf(op.index)->call_site)) {  // lane 227 (H2): a wrestler marker stands for a wrestler call the game did not build
            D3WMarker(frame, op.index, capture, builder, bytes, stats);
            break;
          }
          if (d3_on_ && frame.OriginOf(op.index) && d3::IsMarkerCall(frame.OriginOf(op.index)->call_site)) {  // lane 138 (D3): a marker stands for an object the game did not build
            D3Marker(frame, op.index, capture, builder, bytes, stats);
            break;
          }
          if ((post_state_ == 1 || post_state_ == 2) && post_claim_.draw[op.index] && PostClaimedDraw(frame, op.index, builder, stats)) break;  // S101 lane PO: dropped (ours drew the chain)
          // S10b: the picture before the HUD is copied right before the first draw after the post chain's last pass that is not one of the
          // 30 Years filters' screen-copy passes (their quad, patched 1456901233B6564C), so DLSS gets the finished picture (the colour wash
          // included) and only the HUD is added back at render size.
          if (dlss_copy_pending_ && frame.draws[op.index].vs_patched_hash != 0x1456901233B6564Cull) CopyBeforeHud(capture, builder);
          if (frame_cuts_on_ && FrameCutsDraw(frame, op_index, capture, builder, stats)) break;   // S102 G11 (F7): after the picture-before-the-HUD copy, which a left-out draw must not skip: the self blit / the restore of the frame buffer (frame_cuts.h)
          if (stage_frame_on_ && !skipping_target_) NoteStageDraw();  // S101 lane 59
          if (const DrawOrigin* po = frame.OriginOf(op.index); po && (po->call_site == particles::kSpriteSite || po->call_site == particles::kQuadSite || po->call_site == particles::kThirdSite)) ++stream_particle_draws_;  // S103 PF
          if (particles_on_) ParticleBegin(frame, capture, op_index, stats);  // S101 lane 67
          BindDraw(frame, frame.draws[op.index], capture, builder, bytes, stats);
          if (particles_on_ && particle_run_.active) ParticleEnd();  // S101 lane 67
          break;
      }
      if (stats.trace) {  // S101 lane 2 (fview)
        draw_trace_.frame_ops = builder.Finish().ops.count;
        stats.trace->push_back(draw_trace_);
      }
    }
    // S9b fix (the phone's smoke, Jarrod 11:1x): a draw is found again in the next tick by its shaders, geometry and how many such draws came
    // before it. Draws that repeat in a frame (particles of one quad, crowd members of one mesh) are re-ordered by the game from frame to
    // frame, so that count does not name the same thing twice: those are never interpolated (they show the newest tick), only draws
    // that are unique in the frame.
    for (const auto& [index, base] : keyed_draws_)
      if (occurrences_[base] > 1) builder.ClearBlendKey(index);
    if (rt_spans_on_) RtSpanEnd();   // S102 R18-B1: every span taken reached all four of its ops (violations must stay 0)
    if (ff_blend_skipped_ | ff_blend_kept_) {   // S102 R21-A: the census slots, once a frame
      rdr::g_ff_blend_skipped.fetch_add(ff_blend_skipped_, std::memory_order_relaxed), rdr::g_ff_blend_kept.fetch_add(ff_blend_kept_, std::memory_order_relaxed);
      ff_blend_skipped_ = ff_blend_kept_ = 0;
    }
    if (replace_active_) ReplaceFrameEnd(!stats.isolate && !velocity_mode_);  // S101 lane 41
    if (shadows_active_ && (++shadows_window_ >= 600 || census_flush::Due(g_cf_shadows))) {  // (A3: also on frames without names, where the line says unnamed N)  // S101 lane 123: the census line, once per window of frames
      rdr::Log("%s", ShadowsOursReport().c_str());
      shadows_window_ = 0;
    }
    {   // S102 K2: the chains' scans run over the whole frame's ops when the caller gave them (a lasting bind without the group rule); the ops of this bind are restored after
      LoadedFrame& mutable_frame = const_cast<LoadedFrame&>(frame);   // (the caller owns a non-const frame: live.cpp / fview / rplay swap frame.ops around Bind the same way)
      const Table<Op> bound_ops = mutable_frame.ops;
      const bool view = ord_full_ops && ord_plant != 1 && (d3_on_ || d3r_on_ || d3c_on_ || wr_on_);
      std::vector<Op> plant_ops;
      std::vector<uint8_t> plant_keep;
      const std::vector<uint8_t>* keep = nullptr;
      if (view) {
        const Table<Op>& full = *ord_full_ops;
        mutable_frame.ops = full;
        keep = ord_keep;
        if (ord_plant == 4) keep = nullptr;   // MODE observeall (diagnosis, review F5): the chains learn and prove EVERY run of the full view, bound or not
        if (ord_plant == 3) {   // PLANT noobserve (review F5): every run counts as left out: nothing is learned or proved (the guards still see every run)
          plant_keep.assign(full.count, 0);
          keep = &plant_keep;
        }
        if (ord_plant == 2 && keep && keep->size() == full.count) {   // PLANT: the view lacks one whole pass that holds an object draw the bind left out
            std::vector<uint32_t> cands;   // the dropped keyed object draws (colour / mirror / caster sites: what the chains scan); one of them, rotating with the frame number, loses its whole pass
          for (uint32_t k = 0; k < full.count; ++k) {
            const DrawOrigin* o = full[k].kind == OpKind::Draw ? frame.OriginOf(full[k].index) : nullptr;
            if (o && o->object && !(*keep)[k] && (o->pass_site == thin::kSiteColour || o->pass_site == thin::kSiteMirror || d3::CasterPassOfSite(o->pass_site))) cands.push_back(k);
          }
          // render-clock: dev - the planted candidate pick of the proof rig, never a picture value
          const uint32_t hit = cands.empty() ? full.count : cands[size_t(frame.header.guest_frame % cands.size())];
          if (hit < full.count) {
            uint32_t b = hit, e = hit + 1;
            while (b > 0 && full[b].kind != OpKind::SetTargets) --b;
            while (e < full.count && full[e].kind != OpKind::SetTargets) ++e;
            for (uint32_t k = 0; k < full.count; ++k)
              if (k < b || k >= e) plant_ops.push_back(full[k]), plant_keep.push_back((*keep)[k]);
            mutable_frame.ops = Table<Op>{plant_ops.data(), uint32_t(plant_ops.size())};
            keep = &plant_keep;
          }
        }
        ++ord_view_frames;
        d3_.observe_keep = keep;
        if (rc_) rc_->d3r_.observe_keep = keep;
        if (cc_) cc_->d3c_.observe_keep = keep;
        if (wc_) wc_->w_.observe_keep = keep;
      }
      const uint64_t skipped_before = d3_.c.observe_skipped_runs + (rc_ ? rc_->d3r_.c.observe_skipped_runs : 0) + (cc_ ? cc_->d3c_.c.observe_skipped_runs : 0) + (wc_ ? wc_->w_.c.observe_skipped_runs : 0);
      if (d3_on_) { d3t::Scope t(d3t::kArenaEnd); D3EndFrame(frame); }  // lane 138 (before ThinEndFrame: the pins are still this frame's)
      if (d3r_on_) { d3t::Scope t(d3t::kRingEnd); D3REndFrame(frame, capture); }  // lane 229 (Ring D3 R3)
      if (d3c_on_) { d3t::Scope t(d3t::kCasterEnd); D3CEndFrame(frame, capture); }  // lane 217 (E2b)
      if (wr_on_) D3WEndFrame(frame, capture);  // lane 227 (H2)
      if (view) {
        mutable_frame.ops = bound_ops;
        d3_.observe_keep = nullptr;
        if (rc_) rc_->d3r_.observe_keep = nullptr;
        if (cc_) cc_->d3c_.observe_keep = nullptr;
        if (wc_) wc_->w_.observe_keep = nullptr;
        ord_view_dropped_runs += d3_.c.observe_skipped_runs + (rc_ ? rc_->d3r_.c.observe_skipped_runs : 0) + (cc_ ? cc_->d3c_.c.observe_skipped_runs : 0) + (wc_ ? wc_->w_.c.observe_skipped_runs : 0) - skipped_before;
      }
    }
    if (thin_frame_on_) ThinEndFrame();  // lane 122
    if (particles_on_ && particle_census_.draws == 0 && !particle_idle_said_ && ++particle_idle_frames_ >= 1800) {  // switch on, no particle-site draw seen: the draws carry no origin (the reader's names off)?
      particle_idle_said_ = true;
      rdr::Log("[rdr][particles] switch on but no particle draw seen in %u frames (draw origins missing, or no particles on screen)", particle_idle_frames_);
    }
    if (particles_on_ && particle_census_.draws != particle_draws_base_) {  // S101 lane 67: the census line, once per window of frames with particles
      ++particle_census_.frames;
      if ((++particle_window_ >= 600 || census_flush::Due(g_cf_particle))) {
        LogParticleCensus();
        particle_window_ = 0;
      }
    }
    rdr_scene::live::presence::Particles().NoteStreamFrame(stream_particle_draws_);  // S103 PF: the census column of the [scene][particles] read line
    PostFrameEnd();  // S101 lane PO: the post census window
    if (g_crowd_ours && crowd_hook_ && crowd_state_ == 0) crowd_hook_->NoCrowdFrame();  // S101 lane 35: no crowd draw in this frame (a load screen, a menu)
    // lane 272: a guest frame with NO crowd draw in any part whose member tally says the game dropped member draws: no kept run anchor reached the stream, so nothing triggered ours and the members are gone: a hole.
    if (g_crowd_ours && crowd_hook_ && crowd_state_ == 0 && !velocity_mode_ && !stats.isolate && !persist_only_bind && !frame_part_open && !part_drew_ && !part_member_reported_ &&
        crowd_hook_->MemberNotReadyResult(frame) == 4)
      wwe2k14::member_thin::g_gate.Report(wwe2k14::card_thin::Kind::ThinHole, wwe2k14::member_thin::NowMs());
    // S101 lane 50: tell the game side (P2b / P2c, scene_live/crowd_skip_core.h) whether ours drew this frame and kept nothing of the stream's; it drops the game's
    // crowd work only after a few such frames in a row and goes back to doing it all the moment this says otherwise. A velocity / isolate pass is not a frame.
    // One report per guest frame: a frame split at a readback binds twice (frame_part_open is true for the first part, set by live.cpp); the parts are ORed.
    if (g_crowd_ours && crowd_hook_ && !velocity_mode_ && !stats.isolate) {
      part_drew_ = part_drew_ || (crowd_state_ == 1 && crowd_injected_);
      part_kept_ += crowd_.draws_kept - crowd_kept_base_;
      if (!frame_part_open) {
        wwe2k14::crowd_skip::g_gate.Report(part_drew_, part_kept_, crowd_hook_->DrawsCards(), wwe2k14::crowd_skip::NowMs());
        part_drew_ = false, part_kept_ = 0, part_member_reported_ = false;
      }
    }
    if (g_crowd_ours && crowd_hook_ && (crowd_.ours_frames + crowd_.fallback_frames + crowd_.card_frames_ours + crowd_.card_frames_fallback) && (++crowd_window_ >= 600 || census_flush::Due(g_cf_crowd))) {  // the census line, once per window of frames
      LogCrowdCensus();
      crowd_window_ = 0;
    }
    AliasEnd();  // S101 lane 190: the alias never outlives the frame
  }
