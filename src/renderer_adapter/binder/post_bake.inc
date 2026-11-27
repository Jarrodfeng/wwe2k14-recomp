
  // S101 lane PO (post_hook.h): the post chain's promotion, three small entry points out of Bind's frame.
  // PlanPost: once per Bind, ask the hook which of the frame's draws / ops are the post chain's (post_state_ 1), or none (0).
  template <class Source>
  [[gnu::noinline]] void PlanPost(const LoadedFrame& frame, const Source& capture, const BindStats& stats) {
    post_state_ = 0;
    if (!g_post_ours || !post_hook_ || velocity_mode_ || stats.isolate) return;
    if (persist_only_bind) {   // S101 lane 224: nothing of this bind is shown; the stream keeps its draws (the persistent steps are whatever lasting copies need), counted apart from the fallbacks
      post_hook_->PersistOnlyFrame();
      return;
    }
    PostPlanIn in;
    in.frame = &frame;
    in.draw_origins = frame.origins.count == frame.draws.count && frame.draws.count != 0;
    const auto& records = capture.images();
    in.info = [&records](uint32_t id) {
      PostImageInfo info;
      if (!id || id > records.size() || records[id - 1].id != id) return info;
      info.width = records[id - 1].width, info.height = records[id - 1].height;
      info.scaled = (records[id - 1].usage & rdr::kUsageScaled) != 0;
      info.known = true;
      return info;
    };
    in.map = [this](uint32_t id) { return Map(id); };
    in.depth_of_field = g_depth_of_field, in.film_grain = g_film_grain;
    post_claim_ = PostClaim{};
    if (post_hook_->PlanFrame(in, post_claim_) && post_claim_.ok && post_claim_.draw.size() == frame.draws.count && post_claim_.drop.size() == frame.ops.count) {
      post_state_ = 1;
    } else if (post_claim_.no_chain) {
      post_hook_->NoPostFrame();
    } else {
      ++post_.fallback_frames;
    }
  }

  // PostClaimedDraw: a draw the hook claimed. The first one runs the injection (our whole chain goes in here, into the targets bound at this point); a refused injection
  // returns false (this draw and every later one is drawn the stream's way). The others are dropped; the last one stands for the stream's combine: what follows is the HUD.
  [[gnu::noinline]] bool PostClaimedDraw(const LoadedFrame& frame, uint32_t draw_index, rdr::FrameBuilder& builder, BindStats& stats) {
    if (post_state_ == 1) {
      const auto t0 = std::chrono::steady_clock::now();
      PostInject in;
      in.frame = &frame;
      in.draw_index = draw_index;
      in.builder = &builder;
      in.images = HookImages();  // S101 lane 190: the aliased destinations read as their sources
      in.image_count = uint32_t(image_ids_.size());
      in.formats = image_formats_.size() >= image_ids_.size() ? image_formats_.data() : nullptr;   // S102 R24-E
      const uint32_t passes = post_hook_->Inject(in);
      post_.ms_ours += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (!passes) {
        post_state_ = 3;
        ++post_.fallback_frames;
        builder.SetTargets(post_last_targets_);   // S101 lane PO: a failure after passes were appended left the builder on our image; the stream's dof draw goes where the stream put it   // allow:post
        return false;
      }
      post_state_ = 2;
      ++post_.ours_frames;
      // S101 lane 224 (review SF): a claim of ONE draw (the combine alone) has the stream's target switch BEFORE the injection, so nothing re-binds it after our pass; the builder would stay on our
      // out-only targets and every later draw without its own switch (menus, overlays, the HUD) would lose the stream's depth / stencil / second colour target.
      if (post_claim_.draws == 1) builder.SetTargets(post_last_targets_);   // allow:post
    }
    ++stats.draws;
    ++post_.draws_dropped;
    draw_trace_.outcome = BindOutcome::LeftOut;
    draw_trace_.fate = kFatePostOurs;
    if (draw_index == post_claim_.last_draw) {  // what BindDraw does at the stream's last chain draw (kPostChainLastVs)
      if (g_dlss && !after_post_ && current_color_) dlss_copy_pending_ = true;  // S10b: the picture before the HUD, after this draw
      after_post_ = true;
    }
    return true;
  }

  // The census line, once per window of frames the switch decided something for.
  void PostFrameEnd() {
    if (!g_post_ours || !post_hook_ || velocity_mode_) return;
    if ((post_.ours_frames + post_.fallback_frames) && (++post_window_ >= 600 || census_flush::Due(g_cf_post))) {
      rdr::Log("%s", post_hook_->Census(post_.draws_dropped, post_.copies_dropped, post_.clears_dropped, post_.targets_dropped).c_str());
      post_window_ = 0;
    }
  }

  // ---- S102 P1: the owned crowd card bake (bake_hook.h; docs\renderer\CARD_BAKE_OURS.md) ----------------------------------------------------------------------------------------------
  // BakePlan: find the frame's bake passes: a SetTargets whose first colour target has the role AtlasTarget (the crowd person pictures), then until the next SetTargets one Clear, ONE draw
  // of the bake program (yAudDefault skinned_lit_lm_vertex) and one Copy. 1-20 of them make the frame's group; anything else (another atlas target, a pass with other draws) is left alone.
  [[gnu::noinline]] void BakePlan(const LoadedFrame& frame, bool ignore_dead) {
    bake_passes_.clear();
    if (!frame.ops.count) return;
    const auto is_bake = [](const Draw& d) {   // (any twin of the program, as G4's: the name starts with the effect and technique; review F6)
      const int vs = FindVariant(d.vs_hash);
      return vs >= 0 && rdr::fx::kVariants[vs].name && std::strncmp(rdr::fx::kVariants[vs].name, "yAudDefault_skinned_lit_lm_vertex", 33) == 0;
    };
    BakePassOps cur;
    bool open = false, ok = false;
    uint32_t draws = 0, copies = 0, clears = 0;
    auto close = [&]() {
      if (!open) return;
      if (ok && draws == 1 && copies == 1 && clears <= 1) {
        if (ignore_dead || !(dead_on_ && dead_plan_.drop[cur.set_op])) bake_passes_.push_back(cur);   // a pass G4 cut is G4's: no op of it is ours
      }
      else if (draws || copies) ++bake_.not_a_bake;
      open = false;
    };
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::SetTargets) {
        close();
        const rdr::TargetsOp& t = frame.targets[op.index];
        open = t.color_count >= 1 && ImageRole(t.color[0].image) == uint8_t(SurfaceRole::AtlasTarget);
        cur = BakePassOps{};
        cur.set_op = i;
        ok = true, draws = copies = clears = 0;
      } else if (!open) {
        continue;
      } else if (op.kind == OpKind::Clear) {
        ++clears, cur.clear_op = i;
      } else if (op.kind == OpKind::Draw) {
        ++draws, cur.draw_op = i;
        ok = ok && is_bake(frame.draws[op.index]);
      } else if (op.kind == OpKind::Copy) {
        ++copies, cur.copy_op = i;
      }
    }
    close();
    if (bake_passes_.empty() || bake_passes_.size() > 20) {
      bake_passes_.clear();
      return;
    }
    bake_.passes_found += bake_passes_.size();
    if (!bake_hook_->FrameReady(frame, uint32_t(bake_passes_.size()))) {
      ++bake_.frames_fallback;
      bake_passes_.clear();
      return;
    }
    bake_drop_.assign(frame.ops.count, 0);
    for (const BakePassOps& p : bake_passes_) {
      bake_drop_[p.set_op] = 1, bake_drop_[p.draw_op] = 1, bake_drop_[p.copy_op] = 1;
      if (p.clear_op != ~0u) bake_drop_[p.clear_op] = 1;
    }
    bake_first_op_ = bake_passes_[0].set_op;
    bake_on_ = true;
  }

  // BakeRun: the group, at the first pass's place. Per pass: the stream's target switch and clear, the bake draw with the hook's pose bank, the copy into the hook's picture. The targets the builder
  // had before are set again afterwards. `g_own_card_bake_none` (test arm) re-issues nothing.
  template <class Source>
  [[gnu::noinline]] void BakeRun(const LoadedFrame& frame, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    ++bake_.frames_ours;
    if (g_own_card_bake_none) return;
    const uint32_t keep_target = current_target_, keep_depth = current_depth_, keep_color = current_color_, keep_listed = listed_target_;
    const bool keep_skipping = skipping_target_;   // review F2: the half-rate shadow skip belongs to the game's shadow targets; BindDraw would return early for our bake draws
    skipping_target_ = g_own_card_bake_skip_leak ? shadow_skip_frame_ : false;   // the planted fault (--bake-skip-leak): the half-rate skip state of the frame stays on for the group (on this capture the stream target before the group is not a skipped one, so the fault forces the state)
    const uint32_t n = uint32_t(bake_passes_.size());
    BakeMini& m = bake_mini_;
    for (uint32_t k = 0; k < n; ++k) {
      const BakePassOps& p = bake_passes_[k];
      rdr::TargetsOp targets = frame.targets[frame.ops[p.set_op].index];
      current_target_ = targets.color_count ? targets.color[0].image : targets.depth.image;
      current_depth_ = targets.depth.image;
      current_color_ = targets.color_count ? targets.color[0].image : 0;
      listed_target_ = targets.color_count ? targets.color[0].image : 0;
      for (uint32_t c = 0; c < targets.color_count; ++c) targets.color[c].image = Map(targets.color[c].image);
      targets.depth.image = Map(targets.depth.image);
      builder.SetTargets(targets);   // allow:bake
      if (p.clear_op != ~0u) builder.Clear(frame.clears[frame.ops[p.clear_op].index]);
      // the draw: a one-draw view whose vertex bank is the hook's pose
      const Draw& sd = frame.draws[frame.ops[p.draw_op].index];
      rdr::CopyOp copy = frame.copies[frame.ops[p.copy_op].index];
      BakePassIn in;
      in.frame = &frame, in.pass = k, in.pass_count = n, in.draw_index = frame.ops[p.draw_op].index, in.copy_dest = copy.dest.image;
      m.banks.assign(2, ConstantBank{});
      m.banks[0] = frame.constant_banks[sd.vs_bank];
      m.banks[1] = frame.constant_banks[sd.ps_bank];
      bake_hook_->Pose(in, frame.constant_banks[sd.vs_bank], m.banks[0]);
      m.draws.assign(1, sd);
      Draw& d = m.draws[0];
      d.fixed = d.dynamic = d.bool_loop = 0, d.vs_bank = 0, d.ps_bank = 1;
      m.fixeds.assign(1, frame.fixeds[sd.fixed]);
      m.dynamics.assign(1, frame.dynamics[sd.dynamic]);
      m.bools.assign(1, frame.bool_loops[sd.bool_loop]);
      m.slots.clear(), m.samplers.clear(), m.layouts.clear(), m.elements.clear();
      for (uint32_t t = 0; t < sd.texture_count; ++t) {
        TextureSlot ts = frame.texture_slots[sd.texture_first + t];
        m.samplers.push_back(frame.samplers[ts.sampler]);
        ts.sampler = uint32_t(m.samplers.size() - 1);
        m.slots.push_back(ts);
      }
      d.texture_first = 0, d.texture_count = uint32_t(m.slots.size());
      if (sd.layout != 0xFFFFFFFFu) {
        VertexLayout vl = frame.layouts[sd.layout];
        for (uint32_t e = 0; e < vl.element_count; ++e) m.elements.push_back(frame.elements[vl.element_first + e]);
        vl.element_first = 0;
        m.layouts.push_back(vl);
        d.layout = 0;
      }
      m.frame.header = frame.header;
      m.frame.draws = {m.draws.data(), 1};
      m.frame.fixeds = {m.fixeds.data(), 1};
      m.frame.dynamics = {m.dynamics.data(), 1};
      m.frame.samplers = {m.samplers.data(), uint32_t(m.samplers.size())};
      m.frame.texture_slots = {m.slots.data(), uint32_t(m.slots.size())};
      m.frame.constant_banks = {m.banks.data(), 2};
      m.frame.bool_loops = {m.bools.data(), 1};
      m.frame.layouts = {m.layouts.data(), uint32_t(m.layouts.size())};
      m.frame.elements = {m.elements.data(), uint32_t(m.elements.size())};
      BindDraw(m.frame, m.frame.draws[0], capture, builder, bytes, stats);
      ++bake_.draws_ours;
      // the copy into the picture
      const uint32_t dest_id = bake_hook_->CopyDest(in);
      copy.source.image = Map(copy.source.image);
      copy.dest.image = Map(dest_id);
      builder.Copy(copy);   // allow:bake
      ++bake_.copies_ours;
      ++bake_.passes_ours;
    }
    current_target_ = keep_target, current_depth_ = keep_depth, current_color_ = keep_color, listed_target_ = keep_listed;
    skipping_target_ = keep_skipping;
    if (bind_have_targets_) builder.SetTargets(bind_last_targets_);   // allow:bake
  }
