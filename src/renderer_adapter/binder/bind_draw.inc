
  template <class Source>
  void BindDraw(const LoadedFrame& frame, const Draw& draw, const Source& capture, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    ++stats.draws;
    if (!velocity_mode_) draw_trace_.outcome = BindOutcome::LeftOut;  // S101 lane 2 (fview)
    if (skipping_target_) {  // S12: a shadow map's draw on a frame its maps are kept
      ++stats.settings_out;
      return;
    }
    // S101 lane 35 (the crowd's first promotion): a draw the game's crowd recorder submitted. Once our crowd is in the frame, the rest are dropped.
    bool crowd_origin = false, crowd_keep = false, cards_origin = false;
    if (g_crowd_ours && crowd_hook_ && !velocity_mode_ && !stats.isolate) {
      const DrawOrigin* origin = frame.OriginOf(uint32_t(&draw - &frame.draws[0]));
      cards_origin = origin && origin->call_site == kCrowdCardsSite && crowd_hook_->InjectsCards();  // S101 lane CD: the far cards have their own injection (below)
      crowd_origin = origin && (origin->call_site == kCrowdBlocksSite || (origin->call_site == kCrowdCardsSite && crowd_hook_->DrawsCards() && !cards_origin) ||
                                (origin->call_site == kCrowdFloorSite && crowd_hook_->DrawsFloor()));  // S101 lane 82: the seated floor people, behind their own switch
      if (cards_origin && cards_state_ == 1) {  // S101 lane CD: our cards are in the frame; this stream card draw is dropped unless it stays the game's
        if (crowd_hook_->KeepCardDraw(uint32_t(&draw - &frame.draws[0]))) {
          crowd_keep = true;
          ++crowd_.card_draws_kept;
        } else {
          ++crowd_.card_draws_dropped;
          ++stats.settings_out;
          draw_trace_.fate = kFateCrowdOurs;
          return;
        }
      }
      if (g_crowd_fast_fault_id == 2 && crowd_origin && crowd_state_ == 0) {  // S102 R21-C TEST ONLY (twin crowd_early_first): the draw that would trigger the injection takes the early path: ours never injects
        ++crowd_.draws_dropped, ++stats.settings_out;
        draw_trace_.fate = kFateCrowdOurs;
        return;
      }
      if (crowd_origin && crowd_state_ == 1 && crowd_injected_) {
        if (g_crowd_fast_fault_id != 1 && crowd_hook_->KeepDraw(uint32_t(&draw - &frame.draws[0]))) {  // (twin crowd_early_keep, TEST ONLY: id 1 drops a draw the hook keeps)
          crowd_keep = true;  // a member ours could not draw: this stream draw stays
          ++crowd_.draws_kept;
        } else {
          ++crowd_.draws_dropped;
          ++stats.settings_out;
          draw_trace_.fate = kFateCrowdOurs;  // S101 lane 58
          return;
        }
      }
    }
    // S101 lane 86 (SH0): the shadow casters by origin + the target's surface role (shadow_roles.h), not by hash; one question for both switches below
    const DrawOrigin* shadow_origin = (g_shadow_half || !g_shadows) ? frame.OriginOf(uint32_t(&draw - &frame.draws[0])) : nullptr;
    const uint8_t shadow_role = (g_shadow_half || !g_shadows) ? ImageRole(current_target_) : 0;
    if (g_shadow_half && current_target_ && !pb_dyn_on_)  // S12: which targets are the shadow chain's (S102 L2: not while a pass of the plane bake runs: its target is not the game's)
      shadow_sets_.Observe(shadow_origin, draw.ps_hash, draw.vs_patched_hash, current_target_, shadow_role);
    if (draw.flags & kDrawNoGeometry) {
      ++stats.no_geometry;
      if (!velocity_mode_) draw_trace_.outcome = BindOutcome::NoGeometry;  // S101 lane 2 (fview)
      return;
    }
    // lane 122 (D1 thin draws): a draw the reader built only the anchor of. Its program pair, layout and geometry come from the key's template; nothing of the blanked fields is read.
    // (ReadStream, the planted positive control, does not intercept: the blank hashes then find no shader and the draw is lost.)
    const thin::ThinTemplate* thin_tpl = nullptr;
    thin_key_now_ = 0, thin_slot_now_ = nullptr;
    if ((draw.flags & kDrawThin) && thin_fault_ != thin::Fault::ReadStream) {
      thin_tpl = !velocity_mode_ ? ThinFind(frame, draw) : nullptr;
      if (!thin_tpl) {
        ++stats.settings_out;  // (a hole: counted in thin_counters_; the draw is lost)
        return;
      }
    }
    if (stats.count_pipelines && !thin_tpl) CountPipeline(frame, draw, stats);
    int vs = FindVariant(draw.vs_hash);  // (lane 122: a thin draw keeps its shader hashes: the reader still looks the programs up, and the program is part of the key)
    int ps = draw.ps_hash ? FindVariant(draw.ps_hash) : -1;
    // S10b: a scene draw of a wrestler is remembered for the velocity pass (at the present); in that pass it is drawn by its VELOCITY twin.
    if (velocity_mode_) {
      static const int velocity_fragment = VariantByName("yVelocity_motion_fragment");
      vs = VelocityTwin(vs);
      ps = velocity_fragment;
      if (vs < 0 || ps < 0) return;
    } else if ((g_dlss || g_velocity_census) && (g_velocity_legacy ? !dlss_scene_done_ : !after_post_) && VelocityTwin(vs) >= 0 && frame.dynamics[draw.dynamic].host.viewport[2] >= 1200.0f) {  // lane 185: until the post chain's last pass
      velocity_draws_.push_back(uint32_t(&draw - &frame.draws[0]));
    }
    // S3: FILM GRAIN (30 YEARS) off: the 30 Years filters the game draws after its post chain's last pass (the old-film yRetroFilm, the
    // colour-wash yMonotone: capture s100_wm30_300 frame 0, draw 1070 right after the combine 1069) drawn by their CLEAN variants.
    if (!g_film_grain && after_post_ && ps >= 0) {
      static const int retro = VariantByName("yRetroFilm_retro_fragment"), retro_clean = VariantByName("yRetroFilm_clean_fragment");
      static const int mono = VariantByName("yMonotone_monotone_fragment"), mono_clean = VariantByName("yMonotone_clean_fragment");
      const int clean = ps == retro ? retro_clean : ps == mono ? mono_clean : -1;
      if (clean >= 0) {
        ps = clean;
        static bool said = false;
        if (!said) said = true, rdr::Log("[bind] film grain off: the 30 Years filter (%s) drawn clean", rdr::fx::kVariants[clean].name);
      }
    }
    // S8 DEPTH OF FIELD off: the combine's permutation without it (the game's own: same texture slots and registers, e.g. dof_fog [0 2 3 6]
    // -> fog [0 3]).
    if (!g_depth_of_field && ps >= 0) {
      static const std::vector<int> no_dof = [] {
        std::vector<int> map(std::size(rdr::fx::kVariants), -1);
        for (size_t i = 0; i < map.size(); ++i) {
          const char* name = rdr::fx::kVariants[i].name;
          if (!name || std::strncmp(name, "yGlareEtc_dof", 13) != 0) continue;
          std::string rest = name + 13;  // "_fog_fragment", or "_fragment" for dof alone
          map[i] = VariantByName((rest == "_fragment" ? std::string("yGlareEtc_base_fragment") : "yGlareEtc" + rest).c_str());
        }
        return map;
      }();
      if (no_dof[size_t(ps)] >= 0) {
        ps = no_dof[size_t(ps)];
        static bool said = false;
        if (!said) said = true, rdr::Log("[bind] depth of field off: the combine drawn as %s", rdr::fx::kVariants[ps].name);
      }
    }
    // S7 SHADOWS off: the casters are not drawn; the passes' clears and copies stay (empty maps: lit).
    if (!g_shadows && !velocity_mode_ && shadow_roles::IsCaster(shadow_origin, draw.ps_hash, draw.vs_patched_hash, shadow_role)) {  // S101 lane 86
      ++stats.settings_out;
      static bool said = false;
      if (!said) said = true, rdr::Log("[bind] shadows off: the dynamic shadow casters are not drawn");
      return;
    }
    // S6 CROWD DENSITY: a crowd member (the flat or animated figures: yAudience / yAudDefault vertex programs) whose rank is not below the
    // percentage is not drawn.
    if (g_crowd_density < 100 && vs >= 0) {
      // S102 R21-C (step 1): whether a program is a crowd one is the same every frame: answered from a table made once instead of two string compares on every draw of the frame (1,300 a frame)
      const bool crowd_vs = (g_crowd_fast && (g_crowd_fast_steps & 1)) ? CrowdVariantTable()[size_t(vs)] != 0 : [&] {
        const char* vname = rdr::fx::kVariants[vs].name;
        return std::strncmp(vname, "yAudience_", 10) == 0 || std::strncmp(vname, "yAudDefault_", 12) == 0;
      }();
      if (crowd_vs) {
        const uint32_t* c = frame.constant_banks[draw.vs_bank].dwords;
        const int dens_where = !crowd_origin ? 0 : crowd_keep ? 2 : crowd_state_ == 0 ? 1 : 3;   // S102 R21-C: the census of where the density test runs
        const bool dens_out = CrowdRank(c[76 * 4 + 3], c[77 * 4 + 3], c[78 * 4 + 3]) >= g_crowd_density;
        ++crowd_.dens_tests[dens_where];
        if (dens_out) {
          ++crowd_.dens_cull[dens_where];
          ++stats.settings_out;
          static bool said = false;
          if (!said) said = true, rdr::Log("[bind] crowd density %d%%: members left out by rank", g_crowd_density);
          return;
        }
      }
    }
    // The post chain's last pass (the glare combine's quad: the SDK's gpu_dlss_hud_after_vs default, db\shaders\shader_list.txt row
    // D6B8DEB0775CC47F = the patched yGlareEtc corner_uv): what follows in this frame is the 30 Years filter and the HUD.
    // Photo mode: the HUD left out. The 30 Years filters' screen-copy passes (their quad, patched 1456901233B6564C: the SDK's gpu_wide_post_vs;
    // the HUD never uses it, capture s100_wm30_300 draws 1069-1128) stay, so a photo keeps the match's look (FILM GRAIN still applies).
    if (after_post_ && !velocity_mode_ && g_hide_hud.load(std::memory_order_relaxed) && draw.vs_patched_hash != 0x1456901233B6564Cull) {
      ++stats.settings_out;
      return;
    }
    if (skip_2d_rows_ && !velocity_mode_) {   // S103 L2a (LANDING2_PLAN.md P1): a 2D-whole OURS frame: the 2D was drawn from OUR rows over our frame; the stream's op-19 draws from the row sites are left out (no double UI)
      const DrawOrigin* o2d = frame.OriginOf(uint32_t(&draw - &frame.draws[0]));
      if (o2d && o2d->op == 19 && rdr_scene::q2d::ClassOfSite(o2d->call_site) == rdr_scene::q2d::SiteClass::Row) {
        if (!row2d_inserted_ && row2d_insert_) {   // S103 L2a review F1: our rows go in HERE, at the stream's first row-site draw (not before the whole bind: every kept remnant draw would then land on top of them)
          row2d_inserted_ = true;
          row2d_insert_(builder);
          if (row2d_have_) builder.SetTargets(row2d_targets_);   // allow:row2d (the stream's targets, as they were)
        }
        ++skip_2d_draws_;
        ++stats.settings_out;
        return;
      }
    }
    if (dlss_copied_ && after_post_ && !velocity_mode_) ++kept_after_dlss_copy_;  // lane 201: drawn after DLSS's input was copied (photo mode: a 30 Years filter pass)
    if (scene_frame_id_) { const DrawOrigin* fo = frame.OriginOf(uint32_t(&draw - &frame.draws[0])); fallback_census::NoteRemnantOnce(fc_remnant_noted_, scene_frame_id_, velocity_mode_, frame.header.guest_frame, fo ? fo->call_site : 0u); }   // S103 L2b (review F1): a stream draw that reaches the draw on an OURS frame is a remnant draw (under OURS the post chain is skipped: after_post_ is never set, so it is not tested); only the first per Bind is noted
    if (draw.vs_patched_hash == kPostChainLastVs) {
      if (g_dlss && !after_post_ && current_color_) dlss_copy_pending_ = true;  // S10b: the picture before the HUD, after this draw
      after_post_ = true;
    }
    if (!velocity_mode_) draw_trace_.vs = vs, draw_trace_.ps = ps;  // S101 lane 2 (fview): the final choice (film grain / depth of field swaps applied)
    if (vs < 0) ++missing_[draw.vs_hash];
    if (draw.ps_hash && ps < 0) ++missing_[draw.ps_hash];
    if (vs < 0 || (draw.ps_hash && ps < 0)) {
      ++stats.no_shader;
      if (!velocity_mode_) draw_trace_.outcome = BindOutcome::NoShader;  // S101 lane 2 (fview)
      if (stats.list) {
        rdr::Log("[bind] unbound draw VS %016llX%s PS %016llX%s, %u textures", static_cast<unsigned long long>(draw.vs_hash), vs < 0 ? " (missing)" : "",
                 static_cast<unsigned long long>(draw.ps_hash), draw.ps_hash && ps < 0 ? " (missing)" : "", draw.texture_count);
        ListImage("target", listed_target_, capture);
        for (uint32_t t = 0; t < draw.texture_count; ++t) ListImage("texture", frame.texture_slots[draw.texture_first + t].image, capture);
      }
      return;
    }
    const bool particle_merged = particle_run_.active && &draw == particle_run_.first && !velocity_mode_;  // S101 lane 67: this draw carries its whole run
    if (particle_merged && particle_fault_ == particles::Fault::SingleDropped && particle_run_.members.size() == 1) return;  // lane 132 plant: runs of one are dropped
    if (particle_merged) {
      if (particle_run_.soft && particle_fault_ == particles::Fault::SoftAsHard) ps = VariantByName("yParticle_base_fragment");  // lane 132 plant: the soft run takes the hard pixel stage (no depth fade)
      vs = particle_run_.vs;
      if (!velocity_mode_) draw_trace_.vs = vs;
    }
    const rdr::fx::Variant& vertex = rdr::fx::kVariants[vs];
    const_override::Ctx ovx;  // lane 164 (fview only): who this draw is for the constant override; a drop rule leaves the draw out
    const bool ov = g_const_override.active && !velocity_mode_;
    if (ov) {
      ovx = g_const_override.Eval(frame, uint32_t(&draw - &frame.draws[0]), draw, vs, ps);
      if (g_const_override.Drops(ovx)) {
        ++stats.settings_out;
        return;
      }
    }
    // S10b: the scene's camera (ViewProj, c56-c59) and depth target, from its wide draws before the scene's depth copy.
    if ((g_dlss || g_velocity_census) && !dlss_scene_done_ && current_depth_ && frame.dynamics[draw.dynamic].host.viewport[2] >= 1200.0f) {  // lane 185: (census: the same capture offline)
      for (uint32_t r = 0; r < vertex.constant_count; ++r) {
        if (vertex.constants[r].reg != 56 || vertex.constants[r].count != 4) continue;
        std::memcpy(dlss_vp_, &frame.constant_banks[draw.vs_bank].dwords[56 * 4], sizeof(dlss_vp_));
        dlss_vp_valid_ = true;
        dlss_depth_ = current_depth_;
        break;
      }
    }
    if (stats.isolate) {  // "NAME": only matching draws; "!NAME": every draw but the matching ones
      const bool invert = stats.isolate[0] == '!';
      // "A|B|C": a draw matches when its vertex or fragment program name contains any of the parts.
      bool match = false;
      const char* part = stats.isolate + (invert ? 1 : 0);
      while (*part && !match) {
        const char* bar = std::strchr(part, '|');
        const std::string name = bar ? std::string(part, bar) : std::string(part);
        match = !name.empty() && (std::strstr(vertex.name, name.c_str()) || (ps >= 0 && std::strstr(rdr::fx::kVariants[ps].name, name.c_str())));
        part = bar ? bar + 1 : part + std::strlen(part);
      }
      if (match == invert) {
        ++stats.isolated_out;
        return;
      }
    }
    const DrawFixed& fixed = frame.fixeds[draw.fixed];
    const int layout = particle_merged ? int(particle_run_.layout) : thin_tpl ? int(thin_tpl->layout) : Layout(frame, draw, vs, stats);  // S101 lane 67; lane 122: a thin draw's layout is ours (the template's)
    if (layout < 0) {
      ++stats.bad_layout;
      if (!velocity_mode_) draw_trace_.outcome = BindOutcome::BadLayout;  // S101 lane 2 (fview)
      return;
    }

    // S102 G10 (review F2 / F13): the proof of an opaque output alpha, and with it the draw's normal program without the alpha test when the test is benign (it can never fire: alpha is 1 against a
    // reference under 0.01), so the cut's early-Z is guaranteed (a kill in the shader would disable it).
    const bool cuts_on = (builder.ColourCuts() & rdr::colour_cuts::kPrepass) != 0;
    const bool overdraw_on = builder.ArenaOverdraw();  // S102 C4: the rule plus the texture-alpha proof
    // S102 R18-B3: the arena fragments switch (mask 1 blend off, 2 widened prepass) runs the same rule and texture proof; the prepass flag below is set only for the cuts that use it, the blend-off flag only when the alpha is EXACTLY 1
    const uint32_t af_mask = builder.ArenaFragments();
    const bool af_proof = (af_mask & rdr::arena_fragments::kBlendOff) != 0;
    bool exact_one = false, texture_used = false;
    const uint8_t alpha_proven = (!velocity_mode_ && !particle_merged && !thin_tpl && ps >= 0) ? AlphaOpaque(frame, draw, vs, ps, capture, cuts_on, overdraw_on || af_proof, &exact_one, &texture_used) : 0;
    const bool blend_off = alpha_proven && exact_one && (af_mask & rdr::arena_fragments::kBlendOff) != 0;
    // review F2: a draw admitted only by the texture-alpha proof is pre-drawn (the prepass flag) only when C4's own switch is on, never because mask 1 asked for the proof; G10's rule alone takes what it always took
    const uint8_t alpha_opaque = alpha_proven && (overdraw_on || (cuts_on && !texture_used)) ? 1 : 0;
    const bool drop_alpha_test = (alpha_opaque || blend_off) && rdr::colour_cuts::AlphaBenign(fixed.alpha_ref, float(fixed.alpha_func));
    rdr::DrawState state = fixed.state;
    state.program = Program(vs, ps, fixed.alpha_test != 0 && !velocity_mode_ && !drop_alpha_test);
    if (!velocity_mode_) {  // S101 lane 2 (fview)
      draw_trace_.program = state.program;
      draw_trace_.alpha_requested = fixed.alpha_test != 0;
      draw_trace_.alpha_twin = fixed.alpha_test != 0 && ps >= 0 && ProgramHasAlphaTwin(ps);
    }
    state.vertex_layout = uint16_t(layout);
    if (particle_merged) state.topology = 3, state.primitive_restart = 0;  // S101 lane 67: the merged index list is a triangle list
    if (velocity_mode_) {  // S10b: plain writes of the motion, nearest surface wins (its own depth buffer)
      for (auto& b : state.blend) b = rdr::BlendState{};
      state.blend[0].write_mask = 0xF;
      state.depth_test = 1;
      state.depth_write = 1;
      state.stencil_test = 0;
    }

    DrawDynamic shadow_dynamic;  // S101 lane 123
    const DrawDynamic& source = ShadowsDynamic(frame, draw, frame.dynamics[draw.dynamic], shadow_dynamic);
    rdr::DynamicState dynamic = source.host;
    std::memset(dynamic.system, 0, sizeof(dynamic.system));
    std::memcpy(dynamic.system + 0, source.ndc_scale, sizeof(source.ndc_scale));
    std::memcpy(dynamic.system + 4, source.ndc_offset, sizeof(source.ndc_offset));
    dynamic.system[8] = fixed.alpha_ref;
    dynamic.system[9] = float(fixed.alpha_test && !velocity_mode_ ? fixed.alpha_func : 7);

    rdr::TextureSet textures{};
    const auto declare = [&](const rdr::fx::Variant& variant, bool in_vertex) {
      for (uint32_t s = 0; s < variant.sampler_count; ++s) {
        const rdr::fx::Sampler& sampler = variant.samplers[s];
        if (sampler.slot >= rdr::kMaxTextures) {
          ++stats.high_slot;
          continue;
        }
        textures.slots[sampler.slot].kind = sampler.kind;
        if (in_vertex) textures.slots[sampler.slot].vertex = 1;
        wanted_[sampler.slot] = stamp_;
      }
    };
    ++stamp_;
    declare(vertex, true);
    if (ps >= 0) declare(rdr::fx::kVariants[ps], false);
    for (uint32_t t = 0; t < draw.texture_count; ++t) {
      const TextureSlot& slot = frame.texture_slots[draw.texture_first + t];
      if (slot.slot >= rdr::kMaxTextures || wanted_[slot.slot] != stamp_) continue;  // not read by this program
      rdr::TextureSlot& out = textures.slots[slot.slot];
      out.image = Map(slot.image);
      if (pb_on_ && pb_ok_ && pb_past_chain_ && !pb_composited_ && !pb_dyn_on_ && slot.image == pb_final_id_) ++pb_.early_reads;   // F6: a read of the game's map before this frame's composite: must stay 0
      if (pb_sub_ && !pb_dyn_on_ && slot.image == pb_final_id_ && pb_c_ != rdr::kNoImage) out.image = pb_c_, ++pb_.reads_pointed;   // S102 L2: the ring mat reads the composite, not the game's map
      out.sampler = builder.Sampler(frame.samplers[slot.sampler]);
      out.view_format = rdr::Format(slot.view_format);
      std::memcpy(out.swizzle, slot.swizzle, sizeof(out.swizzle));
      if (shadows_active_) ShadowsReader(slot, out);  // S101 lane 123: a slot bound to one of our images (counted; the plants)
    }
    if (shadows_active_ && cur_ours_ && shadows_ours::g_fault == shadows_ours::Fault::BlurInputNull) ShadowsBlurPlant(frame, draw, textures);  // S101 lane 123 (plant)
    if (pb_extra_image_ != rdr::kNoImage) {   // S102 L2: the composite's second map (the bake) in slot 1, with slot 0's sampler (the stream's blur draw binds one texture only)
      textures.slots[1] = textures.slots[0];
      textures.slots[1].image = pb_extra_image_;
    }
    const auto count_empty = [&](int variant) {
      const rdr::fx::Variant& v = rdr::fx::kVariants[variant];
      for (uint32_t s = 0; s < v.sampler_count; ++s) {
        const uint32_t slot = v.samplers[s].slot;
        if (slot < uint32_t(rdr::kMaxTextures) && textures.slots[slot].image == rdr::kNoImage) ++stats.empty_slots[uint64_t(uint32_t(variant)) << 8 | slot];
      }
    };
    count_empty(vs);
    if (ps >= 0) count_empty(ps);
    if (particle_merged && particle_fault_ == particles::Fault::FirstTexture) {  // S101 lane 67 plant: every run takes the frame's first run's textures
      if (!particle_have_texture_) particle_first_textures_ = textures, particle_have_texture_ = true;
      else textures = particle_first_textures_;
    }

    const BoolLoop& bools = frame.bool_loops[draw.bool_loop];
    bool injected_here = false;  // S101 lane 58
    if (crowd_origin && !crowd_keep) {  // S101 lane 35: the first crowd draw that can be drawn: ours goes in here, into the targets bound at this point
      if (crowd_state_ == 0) {
        crowd_state_ = crowd_hook_->FrameReady() ? 1 : 2;
        if (crowd_state_ == 2) {
          ++(persist_only_bind ? crowd_.persist_only : crowd_.fallback_frames);
          if (!persist_only_bind && crowd_hook_->MemberNotReadyResult(frame) == 4) part_member_reported_ = true, wwe2k14::member_thin::g_gate.Report(wwe2k14::card_thin::Kind::ThinHole, wwe2k14::member_thin::NowMs());   // lane 272: a thin frame with no record is a hole
        }
      }
      if (crowd_state_ == 1 && !crowd_injected_) {
        const auto t0 = std::chrono::steady_clock::now();
        member_result_ = 0;
        const uint32_t drawn = InjectCrowd(frame, draw, builder, vs, ps, state, dynamic, textures, bools, capture);
        if (member_result_ && !persist_only_bind) {   // lane 272: tell the members' cut gate what this frame was (full clean / refused, thin drawn / a hole); the hook reads it on the game's threads (member_thin_core.h)
          part_member_reported_ = true;
          using K = wwe2k14::card_thin::Kind;
          wwe2k14::member_thin::g_gate.Report(member_result_ == 1 ? K::FullOk : member_result_ == 2 ? K::FullBad : member_result_ == 3 ? K::ThinOk : K::ThinHole, wwe2k14::member_thin::NowMs());
        }
        crowd_.ms_ours += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (drawn) {
          crowd_injected_ = true;
          injected_here = true;  // S101 lane 58
          ++(persist_only_bind ? crowd_.persist_only : crowd_.ours_frames);
          crowd_.members_drawn += drawn;
        } else {
          crowd_state_ = 2;
          ++(persist_only_bind ? crowd_.persist_only : crowd_.fallback_frames);
        }
      }
      if (crowd_state_ == 1 && crowd_injected_) {  // (the draw that just triggered the hook takes the same test as every later one: a member ours could not draw keeps its draw)
        if (crowd_hook_->KeepDraw(uint32_t(&draw - &frame.draws[0]))) {
          ++crowd_.draws_kept;  // falls through: drawn the stream's way
        } else {
          ++crowd_.draws_dropped;
          ++stats.settings_out;
          draw_trace_.fate = injected_here ? kFateCrowdInjected : kFateCrowdOurs;  // S101 lane 58
          return;
        }
      }
    }
    if (cards_origin && cards_state_ == 0) {  // S101 lane CD: the first far card draw of the frame: our cards go in here (all the card draws follow the last block, grouped by type), into the targets bound now
      if (crowd_state_ == 0) crowd_state_ = crowd_hook_->FrameReady() ? 1 : 2;  // (a frame with cards but no block draw: the hook still needs this frame's record; the blocks' census is not touched)
      const auto t0 = std::chrono::steady_clock::now();
      card_result_ = 0;
      const uint32_t drawn = InjectCrowd(frame, draw, builder, vs, ps, state, dynamic, textures, bools, capture, true);
      if (card_result_) {   // lane 175: tell the cut's gate what this frame was (full / thin, drawn / refused); the hook reads it on the game's threads (card_thin_core.h)
        using K = wwe2k14::card_thin::Kind;
        wwe2k14::card_thin::g_gate.Report(card_result_ == 1 ? K::FullOk : card_result_ == 2 ? K::FullBad : card_result_ == 3 ? K::ThinOk : K::ThinHole, wwe2k14::card_thin::NowMs());
      }
      crowd_.card_ms_ours += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (drawn) {
        cards_state_ = 1;
        ++(persist_only_bind ? crowd_.card_persist_only : crowd_.card_frames_ours);
        crowd_.cards_drawn += drawn;
        if (crowd_hook_->KeepCardDraw(uint32_t(&draw - &frame.draws[0]))) {
          ++crowd_.card_draws_kept;  // falls through: drawn the stream's way
          draw_trace_.fate = kFateCrowdInjected;  // review F5: our instanced draws sit before this op, so the op that ran InjectCards is ours even when its own card stays (its few pixels are credited to ours)
        } else {
          ++crowd_.card_draws_dropped;
          ++stats.settings_out;
          draw_trace_.fate = kFateCrowdInjected;
          return;
        }
      } else {
        cards_state_ = 2;
        ++(persist_only_bind ? crowd_.card_persist_only : crowd_.card_frames_fallback);
      }
    }
    // S101 lane 41: the in-place replace seam. A hook that claimed this draw and said yes supplies the vertex / index streams, the layout, the textures it names and
    // (the wrestlers) rows of the vertex constants; everything else stays the stream draw's. Switches off: this is one false test.
    const ReplaceOut* replaced = nullptr;
    ReplaceSlot* replaced_slot = nullptr;  // lane 122: which hook replaced this draw (null: none)
    if (thin_tpl) {
      replaced_slot = ThinApply(frame, draw, *thin_tpl, vs, textures, bools);  // lane 122: the template instead of the hook (no Replace call, no per-draw checks)
      if (!replaced_slot) {  // review 8: a template but no hook to draw it with: the draw is dropped (never the stream path with blanked geometry)
        ++thin_hole_c_[uint8_t(thin_tpl->cls)], ++stats.settings_out;
        return;
      }
      draw_trace_.thin = 1;
      draw_trace_.keep_why = 0, draw_trace_.fate = thin_tpl->cls == thin::Cls::Ring ? kFateRingOurs : thin_tpl->cls == thin::Cls::Wrestlers ? kFateWrestlersOurs : kFateArenaOurs;  // lane 129 (KC)
    } else if (replace_active_ && !velocity_mode_ && !stats.isolate && !crowd_origin && !particle_merged) {
      replaced_slot = TryReplace(frame, draw, capture, vs, ps, state, textures, bools, replace_out_);
    }
    if (replaced_slot) {
      replaced = &replace_out_;
      if (replaced->own.set) {  // S101 lane 131: our program, layout and topology (state is the stream draw's otherwise)
        state.program = replaced->own.program;
        if (replaced->own.hook_geometry) {  // lane 255: our program on the casters' mesh: the mesh's layout and topology, the stream's primitive restart (the casters' strip is the stream's strip)
          state.vertex_layout = replaced->vertex_layout;
          state.topology = replaced->topology;
        } else if (!replaced->own.stream_geometry) {  // lane 159: a stream-geometry own draw keeps the stream's layout, topology and primitive restart
          state.vertex_layout = replaced->own.vertex_layout;
          state.topology = replaced->own.topology;
          state.primitive_restart = 0;
        }
        if (!velocity_mode_) draw_trace_.program = state.program;  // S101 lane 131 (review F4): fview --ops names the program that is drawn
      } else if (!replaced->keep_stream_geometry) {  // S101 UI contract: keep-geometry keeps the stream's layout / topology
        state.vertex_layout = replaced->vertex_layout;
        state.topology = replaced->topology;
      }
      for (int s = 0; s < rdr::kMaxTextures; ++s) {
        if (!replaced->slots[s].set || wanted_[s] != stamp_) continue;  // (only a slot this program reads, which TryReplace checked the stream binds; its sampler stays the stream's)
        rdr::TextureSlot& t = textures.slots[s];
        t.image = replaced->slots[s].image;
        t.view_format = replaced->slots[s].view_format;  // the hook image's own, not the stream slot's (those describe the game's texture)
        std::memcpy(t.swizzle, replaced->slots[s].swizzle, sizeof(t.swizzle));
      }
    }
    // S102 L1 (prelight_analytic.h): a character draw that sampled a light cube whose bake is not drawn this frame: its program is the analytic twin, its constants carry the bake's own (PreLightK), no cube is bound.
    // After the replace seam, so every hook saw the stream's draw as it was; a draw a hook gave its own program is counted as missed (the audit requires 0).
    const float* pa_k = nullptr;
    int pa_ps = -1;
    if (pa_on_ && !velocity_mode_ && ps >= 0 && !particle_merged) {
      const uint32_t di = uint32_t(&draw - &frame.draws[0]);
      const uint8_t pa_group = di < pa_plan_.reader_group.size() ? pa_plan_.reader_group[di] : prelight_analytic::kNoGroup;
      if (pa_group != prelight_analytic::kNoGroup) {
        const int twin = prelight_analytic::Planted(prelight_analytic::Fault::NoTwin) ? -1 : PreLightTwin(ps);
        if (twin >= 0 && !(replaced && replaced->own.set)) {
          pa_ps = twin, pa_k = pa_plan_.groups[pa_group].k;
          state.program = Program(vs, twin, fixed.alpha_test != 0 && !drop_alpha_test);
          textures.slots[prelight_analytic::kReaderSlot] = rdr::TextureSlot{};
          if (!velocity_mode_) draw_trace_.program = state.program;
          ++pa_planner_.census.readers_twinned;
        } else {
          ++pa_planner_.census.readers_missed;
        }
      }
    }
    if (pl_layered_ && !velocity_mode_) {   // S102 C1: the layered pre-light draw: only the owned hook's draw is made here (never the stream's), with our layered vertex program and the face-sized viewport
      if (!replaced_slot || draw_trace_.fate != kFateSmallPassesOurs) return;   // (pl_refused_ stays true: PreLightOp falls back)
      PreLightLayeredDraw(state, dynamic, ps, fixed.alpha_test != 0 && !drop_alpha_test);
    }
    if (test_override_from_ && ps >= 0 && !velocity_mode_ && std::strcmp(rdr::fx::kVariants[ps].name, "yBlur_flag_any_fragment") == 0 && textures.slots[0].image == Mapped(test_override_from_))
      textures.slots[0].image = Mapped(test_override_to_);  // lane 151 (TEST ONLY, fview --rim-chain-override): a flag_any draw that reads image FROM reads TO instead, in whichever session this binder belongs to
    if (replaced && replaced->own.set && replaced->own.scissor_set) dynamic.scissor = rdr::Rect{replaced->own.scissor[0], replaced->own.scissor[1], uint32_t(std::max(replaced->own.scissor[2], 0)), uint32_t(std::max(replaced->own.scissor[3], 0))};   // S103 SC: the Quad2D row's scissor (the offline row rig)
    rdr::DrawOp op{};
    op.state = builder.State(state);
    op.dynamic = builder.Dynamic(dynamic);
    op.textures = builder.Textures(textures);
    const bool own_draw = replaced && replaced->own.set;  // S101 lane 131: the hook's program and constants, no stream constants, no tween
    if (ov && replaced && !own_draw) g_const_override.Apply(ovx, false, vertex, replace_block_);  // lane 164 (fview only): the override on the block TryReplace packed
    op.vs_constants = own_draw ? builder.PushConstants(replaced->own.vs_constants, replaced->own.vs_bytes)
                      : replaced ? builder.PushConstants(replace_block_, replace_block_bytes_)  // S101 lane 41: the block TryReplace packed once (the hook's rows applied)
                               : ov ? PackConstantsOverridden(vertex, frame.constant_banks[draw.vs_bank], bools, builder, ovx, false)  // lane 164
                               : PackConstants(vertex, frame.constant_banks.Ref(draw.vs_bank), bools, builder);
    if (own_draw && replaced->own.blend_count && !velocity_mode_) BlendRanges(vertex, draw, builder, op, nullptr, replaced->own.blend, replaced->own.blend_count);  // lane 159 (S2): the own block's ranges, salted key
    if (!own_draw && (!particle_merged || particle_fault_ == particles::Fault::TweenRuns)) BlendRanges(vertex, draw, builder, op, thin_tpl ? &thin_tpl->blend_base : nullptr);  // S101 lane 67: a merged draw is never interpolated (the plant gives it a key); lane 122: a thin draw takes the template's key
    if (thin_frame_on_ && !thin_tpl && !velocity_mode_ && !stats.isolate) ThinAfter(frame, draw, replaced_slot, vs, ps, textures, state);  // lane 122: learn / prove / unpin the keyed draw (full path only)
    static const rdr::fx::Variant no_fragment{};
    op.fs_constants = own_draw ? builder.PushConstants(replaced->own.fs_constants, replaced->own.fs_bytes)
                               : pa_k ? PackAnalytic(rdr::fx::kVariants[pa_ps], frame.constant_banks.Ref(draw.ps_bank), bools, builder, pa_k)   // S102 L1: the twin's block with the bake's constants in PreLightK
                               : ov ? PackConstantsOverridden(ps >= 0 ? rdr::fx::kVariants[ps] : no_fragment, frame.constant_banks[draw.ps_bank], bools, builder, ovx, true)  // lane 164
                               : PackConstants(ps >= 0 ? rdr::fx::kVariants[ps] : no_fragment, frame.constant_banks.Ref(draw.ps_bank), bools, builder);
    const bool per_frame = (draw.flags & kDrawInline) != 0;
    // Captures from before the reader put the high half of a 16-bit pair first: the halves are swapped here on upload.
    const VertexLayout* legacy_pairs = (!(frame.header.flags & kFrameSixteenPairsHighFirst) && draw.layout != 0xFFFFFFFFu) ? &frame.layouts[draw.layout] : nullptr;
    const bool own_nobuf = own_draw && !replaced->own.stream_geometry && !replaced->own.hook_geometry;  // lane 159: an own draw with no buffers of its own (movie) vs one on the stream's geometry (plane shadow)
    const bool our_geometry = replaced && !replaced->keep_stream_geometry && (!own_draw || replaced->own.hook_geometry);  // lane 255: hook_geometry = our program on our mesh  // S101 UI contract: a keep-geometry replace binds the stream's streams below
    draw_trace_.geom_ours = (our_geometry || (replaced && replaced->inline_vertex_bytes && replaced->inline_vertices)) ? 1 : own_nobuf ? 2 : 0;   // lane 148: 2 = an own draw (our quad and program: no stream byte is read)   // lane 159: an own draw on the stream's geometry (own.stream_geometry) reads the stream's bytes, so it stays 0   // lane 126 (review F4): the fview ops json says whose bytes these are
    if (own_nobuf) {  // S101 lane 131: no buffers; the program makes its own corners   // lane 159: own_nobuf = own_draw without stream_geometry
      op.count = replaced->own.count;
    } else if (our_geometry) {  // S101 lane 41: our streams (TryReplace checked the vertex buffer is set); no index buffer = a non-indexed draw of index_count vertices
      op.vertex[0] = replaced->vertex;
      op.index = replaced->index;
      op.index_32bit = replaced->index_32bit;
      op.count = replaced->index_count;
    } else if (replaced && replaced->inline_vertex_bytes && replaced->inline_vertices && replaced->inline_vertex_bytes != draw.vertex_bytes) {
      // S101 lane 89 (review F1): a hook that supplied vertex bytes of the wrong size: the stream's vertices are drawn although the hook counted this draw as replaced
      rdr::Problem("replace_inline_vertex_size", "a replace hook supplied %u vertex bytes for a draw of %u (the stream's vertices are drawn)", replaced->inline_vertex_bytes, draw.vertex_bytes);
      if (draw.vertex_bytes && !particle_merged) op.vertex[0] = Geometry(draw.vertex_offset, draw.vertex_bytes, per_frame, capture, builder, bytes, legacy_pairs ? &frame : nullptr, legacy_pairs);
    } else if (replaced && replaced->inline_vertex_bytes && replaced->inline_vertices && !particle_merged) {
      // S101 lane 89 (UT): a keep-geometry replace that supplies this draw's vertex bytes (our glyph quads); same layout, topology and count as the stream's
      op.vertex[0] = {rdr::kFrameGeometry, builder.PushGeometry(replaced->inline_vertices, replaced->inline_vertex_bytes, 16)};
    } else if (draw.vertex_bytes && !particle_merged) {  // S101 lane 67
      op.vertex[0] = Geometry(draw.vertex_offset, draw.vertex_bytes, per_frame, capture, builder, bytes, legacy_pairs ? &frame : nullptr, legacy_pairs);
    }
    if (our_geometry || own_nobuf) {
      // (op.index / op.count are the hook's, set above)
    } else if (draw.index_bytes) {
      op.index = Geometry(draw.index_offset, draw.index_bytes, per_frame, capture, builder, bytes);
      op.index_32bit = draw.index_32bit;
      op.count = draw.index_count;
    } else {
      op.count = draw.vertex_count;
    }
    if (particle_merged) {  // S101 lane 67: the run's merged vertices (stride 96) and its triangle-list indices
      const particles::Packed& packed = particle_run_.packed;
      op.vertex[0] = {rdr::kFrameGeometry, builder.PushGeometry(packed.vertices.data(), uint32_t(packed.vertices.size()), 16)};
      op.index = {rdr::kFrameGeometry, builder.PushGeometry(packed.indices.data(), uint32_t(packed.indices.size() * 2), 4)};
      op.index_32bit = 0;
      op.count = uint32_t(packed.indices.size());
      if (op.blend_key_low | op.blend_key_high) ++particle_census_.blend_keyed;
      particle_run_.consumed = true;
    }
    op.instances = 1;
    if (alpha_opaque) op.flags |= rdr::kDrawOpAlphaOpaque;  // S102 G10 (review F2)
    if (blend_off) op.flags |= rdr::kDrawOpBlendOff;  // S102 R18-B3
    if (velocity_mode_) {  // S10b: binding 1 = the previous tick's vertex constants (Frame::prev_constants at the same offset)
      op.fs_constants = op.vs_constants;
      op.flags |= rdr::kDrawOpPrevConstants;
    }
    if (pl_layered_) {   // S102 C1 review F4: the layered draw reaches the builder only here
      if (prelight_layered::Planted(prelight_layered::Fault::NoGeometry)) return;   // planted: the draw is not issued, pl_refused_ stays true
      pl_refused_ = false;
    }
    if (builder.GroupsOn()) builder.SetGroup(GroupOfFate(draw_trace_.fate, GroupOfOrigin(frame.OriginOf(uint32_t(&draw - &frame.draws[0])))));   // S102 R24-F: what our hooks drew it as, else its origin's group
    builder.Draw(op);
    ++stats.bound;
    if (!velocity_mode_) draw_trace_.outcome = BindOutcome::Bound;  // S101 lane 2 (fview)
    if (stats.list) {
      rdr::Log("[bind] draw %s + %s, %u elements", vertex.name, ps >= 0 ? rdr::fx::kVariants[ps].name : "(no fragment)", op.count);
      const ConstantBank& bank = frame.constant_banks[draw.vs_bank];
      const float* c = reinterpret_cast<const float*>(bank.dwords);
      rdr::Log("[bind]   bools %08X; c56 %.3g %.3g %.3g %.3g; c76 %.3g %.3g %.3g %.3g; c77 %.3g %.3g %.3g %.3g", bools.words[0], c[224], c[225], c[226], c[227],
               c[304], c[305], c[306], c[307], c[308], c[309], c[310], c[311]);
      if (draw.layout != 0xFFFFFFFFu) {
        const VertexLayout& vl = frame.layouts[draw.layout];
        char line[256];
        int at = std::snprintf(line, sizeof(line), "[bind]   layout stride %u (guest %u):", vl.host_stride, vl.guest_stride);
        for (uint32_t e = 0; e < vl.element_count && at < 220; ++e) {
          const VertexElement& el = frame.elements[vl.element_first + e];
          at += std::snprintf(line + at, sizeof(line) - at, " %u:vk%u/g%u@%u", e, uint32_t(el.vk_format), uint32_t(el.source_format), uint32_t(el.host_offset));
        }
        rdr::Log("%s", line);
      }
      ListImage("target", listed_target_, capture);
      if (current_depth_) ListImage("depth", current_depth_, capture);  // S100: the far crowd cards' depth (capture s100cards)
      for (uint32_t t = 0; t < draw.texture_count; ++t) {
        const TextureSlot& slot = frame.texture_slots[draw.texture_first + t];
        if (slot.slot < rdr::kMaxTextures && wanted_[slot.slot] == stamp_) ListImage("texture", slot.image, capture);
      }
    }
  }
