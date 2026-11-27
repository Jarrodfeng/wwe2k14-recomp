
  // ---- S101 lane 41: the in-place replace seam ------------------------------------------------------------------------------------------------------------------
  struct ReplaceSlot {
    DrawReplaceHook* hook = nullptr;
    const bool* enabled = nullptr;
    ReplaceCounters counters;
    int state = 0;            // this frame: 0 not asked yet, 1 ready, 2 not ready (every claimed draw stays the stream's)
    uint32_t window = 0;      // frames since the last census line
    uint64_t slots_ignored = 0;  // set texture slots (of replaced draws) that the draw's program does not read: not applied
  };
  // lane 255: the plane hook's slot for a plane-site draw the casters decided (both switches on), ready for this frame; null when it is not there, does not claim the draw, or is not ready.
  ReplaceSlot* PlaneJoinSlot(const DrawOrigin& origin, const LoadedFrame& frame) {
    for (ReplaceSlot& s : replace_slots_) {
      if (!s.hook || s.enabled != &g_plane_shadow_ours || !*s.enabled || !s.hook->Claims(origin)) continue;
      if (s.state == 0) {
        s.state = s.hook->FrameReady(d3c_real_frame_ ? *d3c_real_frame_ : frame) ? 1 : 2;
        ++(persist_only_bind ? s.counters.persist_only : s.state == 1 ? s.counters.ours_frames : s.counters.fallback_frames);   // as TryReplace (lane 239)
      }
      return s.state == 1 ? &s : nullptr;
    }
    return nullptr;
  }
  uint32_t plane_join_serial_ = 0;
  static uint64_t Kept(const ReplaceCounters& c) {
    uint64_t n = 0;
    for (uint64_t k : c.kept) n += k;
    return n;
  }

  void LogReplaceWindow(const ReplaceSlot& slot) {
    const std::string line = ReplaceCensus(slot.hook->Name());
    rdr::Log("%s", line.c_str());
    const std::string detail = slot.hook->WindowDetail();   // S101 lane 100
    if (!detail.empty()) rdr::Log("[rdr][%s] %s", slot.hook->Name(), detail.c_str());
    // S102 U1: the one line a report reads first (docs\renderer\TITLE_CARD_MALI.md): the title card's two hooks, in words, with the renderer's refusals next to them.
    if (std::strcmp(slot.hook->Name(), "movie") == 0 || std::strcmp(slot.hook->Name(), "ui") == 0) {
      // Per window (this 600-frame window's counts, not the whole run's): the ones the title card is judged by. "not ready" (kept[0]) is a normal state while a picture
      // loads and is left out; a renderer refusal of a texture with nothing kept is its own word, never "partly kept".
      rdr::ProblemCount counts[rdr::kMaxProblemNames];
      const int n = std::min(rdr::ProblemCounts(counts, rdr::kMaxProblemNames), rdr::kMaxProblemNames);
      uint64_t refused = 0, other = 0;
      for (int i = 0; i < n; ++i) (std::strncmp(counts[i].name, "image_", 6) == 0 || std::strncmp(counts[i].name, "texture_", 8) == 0 ? refused : other) += counts[i].count;
      const ReplaceCounters& c = slot.counters;
      VerdictPrev& prev = verdict_prev_[std::strcmp(slot.hook->Name(), "movie") == 0 ? 0 : 1];
      const uint64_t kept_all = Kept(c) - c.kept[0], d_ours = c.ours_frames - prev.ours, d_replaced = c.replaced - prev.replaced, d_kept = kept_all - prev.kept;
      const uint64_t d_refused = refused - prev.refused, d_other = other - prev.other;
      prev = {c.ours_frames, c.replaced, kept_all, refused, other};
      const char* word = !d_ours ? "not drawn by us" : d_kept ? "partly kept" : d_refused ? "drawn, but textures were refused" : "ok";
      rdr::Log("[rdr][verdict] %s %s: this window: ours frames %llu, replaced %llu, kept %llu (not ready left out); textures refused %llu, other renderer problems %llu", slot.hook->Name(), word,
               static_cast<unsigned long long>(d_ours), static_cast<unsigned long long>(d_replaced), static_cast<unsigned long long>(d_kept), static_cast<unsigned long long>(d_refused),
               static_cast<unsigned long long>(d_other));
    }
  }
  struct VerdictPrev {
    uint64_t ours = 0, replaced = 0, kept = 0, refused = 0, other = 0;
  };
  VerdictPrev verdict_prev_[2];  // S102 U1: movie, ui: the counts at the last window
  void LogParticleCensus() {
    rdr::Log("%s", particles::CensusLine(particle_census_).c_str());
    rdr::Log("%s", particles::BreakdownLine(particle_census_).c_str());
    rdr::Log("%s", particles::ReasonsLine(particle_census_).c_str());  // lane 132
  }
  void LogCrowdCensus() {
      if (g_crowd_density < 100 || g_crowd_rank.Fast()) rdr::Log("%s", CrowdFastLine().c_str());  // S102 R21-C
      rdr::Log("[rdr][crowd] ours frames %llu, fallback %llu, stream draws dropped %llu, members drawn %llu, ms ours %.2f, persist-only %llu", static_cast<unsigned long long>(crowd_.ours_frames),
               static_cast<unsigned long long>(crowd_.fallback_frames), static_cast<unsigned long long>(crowd_.draws_dropped), static_cast<unsigned long long>(crowd_.members_drawn),
               crowd_.ms_ours, static_cast<unsigned long long>(crowd_.persist_only));
      if (crowd_hook_->DrawsFloor()) {  // lane 156: the floor counters reach the window line (a case zip is cut before the stop-time report)
        const std::string floor = crowd_hook_->FloorDetail();
        if (!floor.empty()) rdr::Log("[rdr][crowd] floor: ours frames %llu%s", static_cast<unsigned long long>(crowd_.ours_frames), floor.c_str());
      }
      if (crowd_.draws_kept) rdr::Log("[rdr][crowd] stream draws kept for members ours could not draw: %llu", static_cast<unsigned long long>(crowd_.draws_kept));
      if (crowd_hook_->InjectsCards())  // S101 lane CD
        rdr::Log("[rdr][crowd] cards: frames ours %llu, fallback %llu, card draws dropped %llu, cards drawn %llu, kept %llu, ms ours %.2f, persist-only %llu", static_cast<unsigned long long>(crowd_.card_frames_ours),
                 static_cast<unsigned long long>(crowd_.card_frames_fallback), static_cast<unsigned long long>(crowd_.card_draws_dropped), static_cast<unsigned long long>(crowd_.cards_drawn),
                 static_cast<unsigned long long>(crowd_.card_draws_kept), crowd_.card_ms_ours, static_cast<unsigned long long>(crowd_.card_persist_only));
  }

 public:
  // S101 lane 216: the window census lines NOW, for the end of a run (a route quit, a title termination, the black box's exit zip): the process hard-exits, so a
  // scene shorter than one 600-frame window never printed its line. Same text as a window line (the board reads it the same way); only classes that are on.
  // Render thread only (the counters are plain). Does not touch the window counters (the 600-frame cadence is unchanged).
  void FlushCensus() {
    if (replace_active_)
      for (const ReplaceSlot& slot : replace_slots_)
        if (slot.hook && *slot.enabled) LogReplaceWindow(slot);
    if (shadows_active_) rdr::Log("%s", ShadowsOursReport().c_str());
    if (particles_on_ && particle_census_.draws != 0) LogParticleCensus();
    if (g_crowd_ours && crowd_hook_ && (crowd_.ours_frames + crowd_.fallback_frames + crowd_.card_frames_ours + crowd_.card_frames_fallback)) LogCrowdCensus();
    if (ThinAnyActive() && replace_active_) LogThinCensus();  // (not thin_frame_on_: ThinEndFrame clears it between frames)
  }
 private:

  // Once per bound frame, hooks on: a hook that was never asked (no draw it claims) hears NoClaimFrame; the census line per 600 frames of activity.
  // `asked`: false under isolate / velocity (no replace step ran, so "no claimed draw" would be false news: NoClaimFrame is not called).
  [[gnu::noinline]] void ReplaceFrameEnd(bool asked) {
    for (ReplaceSlot& slot : replace_slots_) {
      if (!slot.hook || !*slot.enabled) continue;
      if (slot.state == 0) {
        if (asked) slot.hook->NoClaimFrame();
      }
      else if (++slot.window >= 600) {
        LogReplaceWindow(slot);
        slot.window = 0;
      }
    }
  }

  // The first enabled hook that claims this draw gets Replace (its decision is counted: replaced or kept by reason). Returns the slot whose `out` the caller applies, else null.
  // Out of BindDraw's frame like InjectCrowd: it packs the stream's vertex block (8 KB) for the hook's per-draw checks only when a hook claimed the draw.
  template <class Source>
  [[gnu::noinline]] ReplaceSlot* TryReplace(const LoadedFrame& frame, const Draw& draw, const Source& capture, int vs, int ps, const rdr::DrawState& state, const rdr::TextureSet& textures,
                                            const BoolLoop& bools, ReplaceOut& out) {
    const uint32_t index = uint32_t(&draw - &frame.draws[0]);
    const DrawOrigin* origin = frame.OriginOf(index);
    if (!origin) return nullptr;
    for (ReplaceSlot& slot : replace_slots_) {
      if (!slot.hook || !*slot.enabled || !slot.hook->Claims(*origin)) continue;
      ReplaceCounters& c = slot.counters;
      const auto t0 = std::chrono::steady_clock::now();
      if (slot.state == 0) {
        slot.state = slot.hook->FrameReady(d3c_real_frame_ ? *d3c_real_frame_ : frame) ? 1 : 2;   // lane 228: a caster marker's mini frame is never the frame a hook builds its object tables from (the shadow passes draw FIRST: the first claimed draw may be a marker's)
        ++(persist_only_bind ? c.persist_only : slot.state == 1 ? c.ours_frames : c.fallback_frames);
      }
      if (slot.state == 2) {
        ++c.kept[uint8_t(KeepWhy::NotReady)];
        draw_trace_.keep_why = uint8_t(KeepWhy::NotReady) + 1;  // S101 lane 84 (review F5): a later hook that is not ready says so in the trace too
        continue;  // (S101: a hook that is not ready lets the next hook that claims the draw try: the arena and the wrestlers share the model-part sites)
      }
      // The stream's packed vertex block, once, in a member buffer: the hook reads it for its per-draw checks, then the rows (if any) are applied to it in place
      // and BindDraw pushes it (a COPY of the constant bank's data: the bank is never written).
      ReplaceIn in;
      in.frame = &frame;
      in.join_frame = d3c_real_frame_ ? d3c_real_frame_ : &frame;   // lane 228
      in.draw = &draw;
      in.draw_index = index;
      in.origin = origin;
      in.vs = vs, in.ps = ps;
      in.vertex_count = draw.vertex_count, in.index_count = draw.index_count;
      in.textures = &textures;
      replace_block_bytes_ = PackBlock(rdr::fx::kVariants[vs], frame.constant_banks.Ref(draw.vs_bank), bools, replace_block_);
      in.vs_block_bytes = replace_block_bytes_;
      in.vs_block = replace_block_;
      if (slot.hook->NeedsFragmentBlock()) {  // lane 151: the stream's packed pixel constants (read only) and the image mapper, only for a hook that asks (REPLACE_SEAM.md)
        if (!replace_fs_block_) replace_fs_block_.reset(new uint8_t[rdr::fx::kMaxFragmentBlockBytes]);   // made on the first hook that asks; no cost or memory for any other hook
        replace_fs_block_bytes_ = ps >= 0 ? PackBlock(rdr::fx::kVariants[ps], frame.constant_banks.Ref(draw.ps_bank), bools, replace_fs_block_.get(), std::min(rdr::fx::kMaxFragmentBlockBytes, test_fs_capacity_)) : 0;
        if (replace_fs_block_bytes_) in.fs_block = replace_fs_block_.get(), in.fs_block_bytes = replace_fs_block_bytes_;   // (ps < 0 or a block that does not fit: null / 0 and the hook refuses the draw)
        in.map_image = [this](uint32_t id) { return Mapped(id); };
      }
      in.stream_layout = state.vertex_layout, in.stream_topology = state.topology, in.stream_index_32bit = draw.index_32bit;
      in.image_size = [&capture](uint32_t id, uint32_t& w, uint32_t& h) {   // S103 P4F: an image's description by capture id (the movie planes' set choice)
        const auto& records = capture.images();
        if (!id || id > records.size() || records[id - 1].id != id) return false;
        w = records[id - 1].width, h = records[id - 1].height;
        return true;
      };
      in.read_geometry = [&capture](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& dest) { return capture.ReadGeometry(offset, bytes, dest); };
      in.vertex_offset = draw.vertex_offset, in.index_offset = draw.index_offset;
      in.vertex_stride = draw.layout != 0xFFFFFFFFu ? frame.layouts[draw.layout].guest_stride : 0;
      // The geometry description starts UNSET (draw_replace_hook.h sentinels): a hook copies ReplaceIn::stream_* to keep the stream's, or sets its own.
      out = ReplaceOut{};
      KeepWhy why = KeepWhy::NotOurs;
      bool yes = slot.hook->Replace(in, out, why);
      if (yes && out.own.hook_geometry) yes = false, why = KeepWhy::Mismatch;   // lane 255: hook_geometry is the binder's join bit; a hook that sets it is refused
      ReplaceSlot* result_slot = &slot;   // lane 255: the slot whose fate and counters this draw takes (the plane slot after a casters + plane join)
      if (slot.enabled == &g_shadow_casters_ours && g_plane_shadow_ours && origin->pass_site == kPlaneShadowSite && (yes || why != KeepWhy::NotOurs)) {
        if (ReplaceSlot* ps_slot = PlaneJoinSlot(*origin, frame)) {
          // Casters and plane together (SHADOW_PLANE.md "Lane 255"): the casters decided (our mesh, or a final refusal); the plane hook decides ONCE for the same draw.
          // Its own result is the draw when it says yes: with the casters' mesh joined in (their yes, no rows) or on the stream's mesh (their final refusal). Its no leaves the casters' result.
          ReplaceIn plane_in = in;
          if (ps_slot->hook->NeedsFragmentBlock()) {   // N3: the plane hook's own input contract (the stream's pixel block), packed as for any hook that asks
            if (!replace_fs_block_) replace_fs_block_.reset(new uint8_t[rdr::fx::kMaxFragmentBlockBytes]);
            replace_fs_block_bytes_ = ps >= 0 ? PackBlock(rdr::fx::kVariants[ps], frame.constant_banks.Ref(draw.ps_bank), bools, replace_fs_block_.get(), std::min(rdr::fx::kMaxFragmentBlockBytes, test_fs_capacity_)) : 0;
            if (replace_fs_block_bytes_) plane_in.fs_block = replace_fs_block_.get(), plane_in.fs_block_bytes = replace_fs_block_bytes_;
            plane_in.map_image = [this](uint32_t id) { return Mapped(id); };
          }
          ReplaceOut plane_out{};
          KeepWhy plane_why = KeepWhy::NotOurs;
          const uint32_t fault = ps_slot->hook->JoinFault();
          bool plane_yes = false;
          const bool join_mesh = yes && !out.own.set && !out.rows && !out.keep_stream_geometry;
          if (fault == 2 && join_mesh && (++plane_join_serial_ % 7) == 0) {
            // PLANT join_drop: the plane hook is never asked (neither claimed nor kept); the draw is the casters'
          } else {
            plane_yes = ps_slot->hook->Replace(plane_in, plane_out, plane_why);
            if (plane_yes && plane_out.own.hook_geometry) plane_yes = false, plane_why = KeepWhy::Mismatch;   // (the join bit is the binder's)
            if (fault == 1 && join_mesh) {   // PLANT join_double: the same draw is offered again (the hook's ordinals run ahead)
              ReplaceOut again{};
              KeepWhy again_why = KeepWhy::NotOurs;
              ps_slot->hook->Replace(plane_in, again, again_why);
            }
            if (!plane_yes) ++ps_slot->counters.kept[uint8_t(plane_why) < uint8_t(KeepWhy::Count) ? uint8_t(plane_why) : uint8_t(KeepWhy::NotOurs)];
          }
          if (plane_yes && plane_out.own.set) {
            if (join_mesh && plane_out.own.stream_geometry) {   // both ours: our mesh AND our program
              plane_out.own.stream_geometry = false, plane_out.own.hook_geometry = true;
              plane_out.vertex = out.vertex, plane_out.index = out.index, plane_out.index_count = out.index_count;
              plane_out.vertex_layout = out.vertex_layout, plane_out.topology = out.topology, plane_out.index_32bit = out.index_32bit;
              ++ps_slot->counters.joined;
            }
            out = plane_out, yes = true, why = KeepWhy::NotOurs, result_slot = ps_slot;
          }
        }
      }
      if (yes && out.own.set &&  // S101 lane 131 (review F2): an own draw is whole or refused; nothing of the geometry / rows / inline-vertex / keep-geometry contracts may be mixed into it
          (!out.own.program || (!out.own.count && !out.own.stream_geometry && !out.own.hook_geometry) /* lane 159: a stream-geometry own draw has the stream's count */ || (out.own.stream_geometry && out.own.hook_geometry) || !out.own.vs_constants || !out.own.vs_bytes || !out.own.fs_constants || !out.own.fs_bytes || out.rows || out.keep_stream_geometry ||
           out.inline_vertices || (!out.own.hook_geometry && (out.vertex.buffer != rdr::kNoBuffer || out.index.buffer != rdr::kNoBuffer)) ||
           (out.own.hook_geometry && (out.vertex.buffer == rdr::kNoBuffer || out.vertex_layout == kReplaceUnsetLayout || out.topology == kReplaceUnset8 || out.index_32bit == kReplaceUnset8))))  // lane 255: a hook_geometry own draw carries the casters' mesh
        yes = false, why = KeepWhy::Mismatch;
      if (yes && out.own.set) {  // lane 159 (S2): an own draw's blend ranges lie inside its own block, in whole floats
        if (out.own.blend_count > 4) yes = false, why = KeepWhy::Mismatch;
        for (uint32_t i = 0; yes && i < out.own.blend_count; ++i) {
          const rdr::BlendRange& b = out.own.blend[i];
          if ((b.offset | b.bytes) & 3u || !b.bytes || uint64_t(b.offset) + b.bytes > out.own.vs_bytes) yes = false, why = KeepWhy::Mismatch;
        }
      }
      if (yes && !out.keep_stream_geometry && !out.own.set &&  // (S101 UI contract: a keep-geometry yes owns only its slots; S101 lane 131: an own draw owns program and count, no buffers)
          (out.vertex.buffer == rdr::kNoBuffer || out.vertex_layout == kReplaceUnsetLayout || out.topology == kReplaceUnset8 || out.index_32bit == kReplaceUnset8))
        yes = false, why = KeepWhy::Mismatch;  // said yes with no vertices or an unset layout / topology / index width: the stream's draw stays
      uint32_t ignored = 0;
      if (yes) {
        for (int s = 0; s < rdr::kMaxTextures && yes; ++s) {
          if (!out.slots[s].set) continue;
          if (wanted_[s] != stamp_) ++ignored;  // a slot this program does not read: counted, not applied
          else if (textures.slots[s].image == rdr::kNoImage) yes = false, why = KeepWhy::Texture;  // the stream binds nothing there: no sampler to use
        }
      }
      if (yes && out.rows) ApplyRowOverride(rdr::fx::kVariants[vs].constants, rdr::fx::kVariants[vs].constant_count, replace_block_, replace_block_bytes_, out.first_register, out.rows, out.row_data);
      slot.slots_ignored += yes ? ignored : 0;
      c.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (!yes) {
        draw_trace_.keep_why = uint8_t(why) < uint8_t(KeepWhy::Count) ? uint8_t(why) + 1 : uint8_t(KeepWhy::NotOurs) + 1;  // S101 lane 84 (lane 79 extra 1): why the last claiming hook kept this draw (KeepWhy + 1; 0 = no hook claimed it)
        ++c.kept[uint8_t(why) < uint8_t(KeepWhy::Count) ? uint8_t(why) : uint8_t(KeepWhy::NotOurs)];
        // S101 (lane 40's note): "not ours" (the hook claimed the site but the object is another class's: the arena and the wrestlers share
        // the model-part sites 0x82C0A950 / 0x82C0B94C) offers the draw to the next hook. Any other refusal is final: the stream's draw.
        if (why == KeepWhy::NotOurs) continue;
        return nullptr;
      }
      ReplaceCounters& rc = result_slot->counters;   // lane 255
      if (result_slot->enabled == &g_plane_shadow_ours && origin->pass_site == kPlaneShadowSite) result_slot->hook->NoteMask(out.own.hook_geometry);   // counted here, from the binder's own result: our mesh + program, or our program on the stream's mesh
      if (result_slot != &slot) ++rc.replaced; else ++c.replaced;
      if (thin_arena_objects_ && slot.enabled == &g_arena_ours) thin_arena_objects_->insert(origin->object);  // lane 122 (TEST ONLY, fview --thin-swap-slot): which objects the arena hook really replaced
      if (wwe2k14::cost::On())  // S101 lane 83: publish the object's class for the record / reader counters (the hook classes only; one load when off)
        wwe2k14::cost::NoteObject(origin->object, slot.enabled == &g_arena_ours ? wwe2k14::cost::kArena : (slot.enabled == &g_wrestlers_ours || slot.enabled == &g_caw_ours) ? wwe2k14::cost::kWrestlers
                                                  : slot.enabled == &g_ring_ours ? wwe2k14::cost::kRing : slot.enabled == &g_flash_ours ? wwe2k14::cost::kFlash : wwe2k14::cost::kOther);
      draw_trace_.keep_why = 0;  // S101 lane 84
      draw_trace_.fate = result_slot->enabled == &g_arena_ours ? kFateArenaOurs : slot.enabled == &g_wrestlers_ours ? kFateWrestlersOurs : slot.enabled == &g_caw_ours ? kFateCawOurs : slot.enabled == &g_ring_ours ? kFateRingOurs : slot.enabled == &g_ui_ours ? (out.own.set ? kFateUiQuadsOurs /* lane 148 */ : kFateUiOurs) : slot.enabled == &g_ui_text_ours ? kFateUiTextOurs : slot.enabled == &g_flash_ours ? kFateFlashOurs
                         : slot.enabled == &g_props_ours ? kFatePropsOurs : result_slot->enabled == &g_plane_shadow_ours ? kFatePlaneShadowOurs : slot.enabled == &g_shadow_casters_ours ? kFateShadowCastersOurs : slot.enabled == &g_screens_ours ? kFateScreensOurs : slot.enabled == &g_rim_ours ? kFateRimOurs : slot.enabled == &g_crowd_props_ours ? kFateCrowdOurs : slot.enabled == &g_movie_ours ? kFateMovieOurs : slot.enabled == &g_circles_ours ? kFateCirclesOurs : slot.enabled == &g_small_passes_ours ? kFateSmallPassesOurs /* lane 135 */ : slot.enabled == &g_plane_shadow_ours ? kFatePlaneShadowOurs /* lane 139 */ : slot.enabled == &g_rim_chain_ours ? kFateRimChainOurs /* lane 151 */ : kFateReplaced;  /* lane 136 */  /* lane 133: the crowd props keep the crowd's fate */  /* lane 124: rim_map */  // S101 lane 115: screens has fate 13
                         //  // S101 lane 58 / ring / UI contracts; lane 84: props; lane 89: ui_text has its own fate (12)
      return result_slot;
    }
    return nullptr;
  }
