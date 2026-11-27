 private:
  copy_alias::Plan alias_plan_;
  bool alias_on_ = false;               // this frame has an elide set and mode 2: Map() consults alias_
  std::vector<uint32_t> alias_;         // capture image id -> the capture image id it reads this frame (0 none)
  std::vector<uint32_t> alias_set_;     // the ids set this frame (cleared at its end)
  uint32_t alias_window_ = 0;
  uint32_t plane_blank_window_ = 0;   // S102 main: binds since the last live plane_blank census line
  // ---- S102 lane L1 (prelight_analytic.h, docs/renderer/PRELIGHT_DIRECT.md section 8): the pre-light cube bake is not drawn; the character shaders evaluate its four lights per pixel -------------------------
  // Switch: wwe2k14_rdr_prelight_analytic (PerFrame; fview / rplay --prelight-analytic), read in PreLightAnalyticBegin with the guest frame. A taken group's 8 ops are not recorded (G4's mechanism); every draw that
  // sampled its cube at slot 13 is bound with its analytic twin (`<name>_pl`, compile_shaders.py) and the group's own constants.
  prelight_analytic::Planner pa_planner_;
  prelight_analytic::Plan pa_plan_;
  bool pa_on_ = false;        // this frame has a taken group (the op loop and BindDraw read it)
  bool pa_enabled_ = false;   // the switch is on this frame
  uint32_t pa_window_ = 0;
  int64_t pa_first_guest_ = -1;   // (the first guest frame this binder saw: --prelight-analytic-off-from counts from it)
  std::vector<int> pa_twin_;  // registry variant -> its analytic twin (-1: none), made once
  int PreLightTwin(int ps) {
    if (pa_twin_.empty()) {
      pa_twin_.assign(std::size(rdr::fx::kVariants), -1);
      std::unordered_map<std::string, int> by_name;   // (only the character effects' names: the twins are all yCh_*; the whole registry is ~15,000 names)
      for (size_t i = 0; i < std::size(rdr::fx::kVariants); ++i)
        if (rdr::fx::kVariants[i].name && std::strncmp(rdr::fx::kVariants[i].name, "yCh_", 4) == 0) by_name.emplace(rdr::fx::kVariants[i].name, int(i));
      static const char tail[] = "_fragment";
      for (size_t i = 0; i < std::size(rdr::fx::kVariants); ++i) {
        const char* name = rdr::fx::kVariants[i].name;
        const size_t n = name ? std::strlen(name) : 0;
        if (n <= sizeof(tail) - 1 || std::strncmp(name, "yCh_", 4) != 0 || std::strcmp(name + n - (sizeof(tail) - 1), tail) != 0) continue;
        const auto found = by_name.find(std::string(name, n - (sizeof(tail) - 1)) + "_pl" + tail);
        if (found != by_name.end()) pa_twin_[i] = found->second;
      }
    }
    return ps >= 0 && size_t(ps) < pa_twin_.size() ? pa_twin_[size_t(ps)] : -1;
  }
  // The twin's pixel block: packed as any block, then the PreLightK range (the register prelight_analytic::kRegisterBase) overwritten with the bake's constants.
  static uint32_t PackAnalytic(const rdr::fx::Variant& variant, const BankRef& bank, const BoolLoop& bools, rdr::FrameBuilder& builder, const float* k) {
    uint8_t block[8192];
    const uint32_t bytes = PackBlock(variant, bank, bools, block, sizeof(block));
    for (uint32_t r = 0; r < variant.constant_count; ++r)
      if (variant.constants[r].reg == prelight_analytic::kRegisterBase && variant.constants[r].count == prelight_analytic::kVec4) std::memcpy(block + variant.constants[r].offset, k, prelight_analytic::kVec4 * 16);
    return builder.PushConstants(block, bytes);
  }
  void PreLightAnalyticBegin(const LoadedFrame& frame, const BindStats& stats) {
    pa_on_ = false;
    pa_enabled_ = rdr_sw::BoolSeen(rdr_sw::kPrelightAnalytic, prelight_analytic::g_fview.load(std::memory_order_relaxed), frame.header.guest_frame);
    // render-clock: dev - fview test only: the switch-off plant counts frames from its first
    if (pa_first_guest_ < 0) pa_first_guest_ = int64_t(frame.header.guest_frame);
    // render-clock: dev - fview test only: the switch-off plant counts frames from its first
    if (prelight_analytic::g_off_from >= 0 && int64_t(frame.header.guest_frame) - pa_first_guest_ >= prelight_analytic::g_off_from) pa_enabled_ = false;   // fview test only (the switch-off plant): N frames after the first bound one
    if (!pa_enabled_ && !pa_planner_.HasMemory()) return;   // off, and no cube is left whose last bake was dropped (an off switch with a remembered cube keeps twinning its readers until the stream re-bakes it)
    prelight_analytic::Ctx ctx;
    ctx.real_frame = !(stats.isolate || velocity_mode_ || !frame.ops.count);   // F3: a lasting-only bind and either part of a split frame are planned (twinning drops no op)
    ctx.enabled = pa_enabled_;
    ctx.guest_frame = frame.header.guest_frame;
    ctx.record = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    if (dead_on_ && dead_plan_.drop.size() >= frame.ops.count) ctx.dead = dead_plan_.drop.data();
    if (bake_on_ && bake_drop_.size() >= frame.ops.count) ctx.others.push_back(bake_drop_.data());
    if (PostClaimPlanned(frame.ops.count)) ctx.others.push_back(post_claim_.drop.data());
    if (turns_on_ && turns_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(turns_plan_.drop.data());
    if (plane_blank_on_ && plane_blank_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(plane_blank_plan_.drop.data());
    if (og_on_ && og_plan_.group_of_op.size() >= frame.ops.count) {   // the own groups taken this frame (shadow module / rim map)
      pa_og_scratch_.assign(frame.ops.count, 0);
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        const uint8_t g = og_plan_.group_of_op[i];
        if (g != own_group::kNone && g < own_group::kGroupCount && og_plan_.taken[g]) pa_og_scratch_[i] = 1;
      }
      ctx.others.push_back(pa_og_scratch_.data());
    }
    ctx.is_prelight_sss = [](const Draw& d) {
      static const int vs = VariantByName("yPreLight_base_vertex"), ps = VariantByName("yPreLight_sss_fragment");
      return vs >= 0 && ps >= 0 && d.vs_hash && d.ps_hash && FindVariant(d.vs_hash) == vs && FindVariant(d.ps_hash) == ps;
    };
    ctx.fetch_k = [&frame](const Draw& d, float* k) {
      if (d.ps_bank >= frame.constant_banks.count) return false;
      const BankRef bank = frame.constant_banks.Ref(d.ps_bank);
      uint8_t* out = reinterpret_cast<uint8_t*>(k);
      std::memset(out, 0, prelight_analytic::kVec4 * 16);
      bank.CopyBytes(8 * 16, 4 * 16, out + 0 * 16);       // DifLghVec
      bank.CopyBytes(0 * 16, 4 * 16, out + 4 * 16);       // DifLghCol
      bank.CopyBytes(198 * 16, 16, out + 8 * 16);         // DifLghMaxD
      for (uint32_t j = 0; j < 4; ++j) bank.CopyBytes((16 + j) * 16, 4, out + 9 * 16 + j * 4);   // DifLghPrm[j].x
      bank.CopyBytes(199 * 16, 16, out + 10 * 16);        // DifLghMaxA
      bank.CopyBytes(204 * 16, 3 * 16, out + 11 * 16);    // SssLobeA
      bank.CopyBytes(207 * 16, 3 * 16, out + 14 * 16);    // SssLobeB
      return true;
    };
    ctx.reader_kind = [this, &frame](uint32_t di, uint32_t slot) -> int {
      const Draw& d = frame.draws[di];
      if (!d.ps_hash) return 0;
      const int ps = FindVariant(d.ps_hash);
      if (ps < 0) return 0;
      const rdr::fx::Variant& v = rdr::fx::kVariants[ps];
      bool samples = false;
      for (uint32_t s = 0; s < v.sampler_count; ++s)
        if (v.samplers[s].slot == slot && v.samplers[s].kind == 1) samples = true;
      if (!samples) return 0;
      return slot == prelight_analytic::kReaderSlot && pa_twin_available(ps) ? 1 : 2;
    };
    const auto pa_t0 = std::chrono::steady_clock::now();
    pa_planner_.Build(frame, ctx, pa_plan_);
    pa_planner_.census.plan_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pa_t0).count());
    ++pa_planner_.census.plan_runs;
    pa_on_ = pa_plan_.on();
  }
  bool pa_twin_available(int ps) { return PreLightTwin(ps) >= 0; }
  std::vector<uint8_t> pa_og_scratch_;
  // ---- S102 lane L1b (prelight_compute.h, docs/renderer/PRELIGHT_DIRECT.md section 9): the pre-light cubes written by ONE compute dispatch ------------------------------------------------------------------
  // Switch: wwe2k14_rdr_prelight_compute (PerFrame; fview / rplay --prelight-compute), read in PreLightComputeBegin with the guest frame. A taken slot's 8 ops are not recorded (G4's mechanism); one DispatchOp, issued
  // where the first taken slot began, writes all the taken cubes from the slots' own draw constants. The readers are untouched: they sample the cubes as they always did.
  prelight_compute::Planner pc_planner_;
  prelight_compute::Plan pc_plan_;
  bool pc_on_ = false;        // this frame has a taken slot (the op loop reads it)
  bool pc_enabled_ = false;   // the switch is on this frame
  uint32_t pc_window_ = 0;
  bool pc_prev_part_open_ = false;
  std::unordered_set<uint32_t> pc_storage_ids_;     // capture image ids of light cubes made with storage usage
  std::unordered_set<uint64_t> pc_mesh_ok_;         // geometry keys of meshes the shader's model was checked against (checked once per key)
  std::vector<uint8_t> pc_vb_, pc_ib_, pc_og_scratch_;
  template <class Source>
  void PreLightComputeBegin(const LoadedFrame& frame, const Source& capture, const BindStats& stats) {
    pc_on_ = false;
    const bool prev_open = pc_prev_part_open_;
    pc_prev_part_open_ = frame_part_open && !persist_only_bind;
    pc_enabled_ = rdr_sw::BoolSeen(rdr_sw::kPrelightCompute, prelight_compute::g_fview.load(std::memory_order_relaxed), frame.header.guest_frame);
    if (!pc_enabled_) return;
    prelight_compute::Ctx ctx;
    ctx.enabled = true;
    ctx.plan_ok = !(stats.isolate || velocity_mode_ || persist_only_bind || frame_part_open || prev_open || !frame.ops.count);
    ctx.compute = renderer_.compute_supported();
    ctx.guest_frame = frame.header.guest_frame;
    ctx.record = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    ctx.cube_has_storage = [this](uint32_t id) { return pc_storage_ids_.count(id) != 0 && id < image_ids_.size() && image_ids_[id] != rdr::kNoImage; };
    if (dead_on_ && dead_plan_.drop.size() >= frame.ops.count) ctx.dead = dead_plan_.drop.data();
    if (bake_on_ && bake_drop_.size() >= frame.ops.count) ctx.others.push_back(bake_drop_.data());
    if (PostClaimPlanned(frame.ops.count)) ctx.others.push_back(post_claim_.drop.data());
    if (turns_on_ && turns_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(turns_plan_.drop.data());
    if (plane_blank_on_ && plane_blank_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(plane_blank_plan_.drop.data());
    if (pa_on_ && pa_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(pa_plan_.drop.data());   // L1's analytic drops: it took the slot (this lever declines on overlap)
    if (og_on_ && og_plan_.group_of_op.size() >= frame.ops.count) {   // the own groups taken this frame (shadow module / rim map)
      pc_og_scratch_.assign(frame.ops.count, 0);
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        const uint8_t g = og_plan_.group_of_op[i];
        if (g != own_group::kNone && g < own_group::kGroupCount && og_plan_.taken[g]) pc_og_scratch_[i] = 1;
      }
      ctx.others.push_back(pc_og_scratch_.data());
    }
    ctx.is_prelight_sss = [](const Draw& d) {
      static const int vs = VariantByName("yPreLight_base_vertex"), ps = VariantByName("yPreLight_sss_fragment");
      return vs >= 0 && ps >= 0 && d.vs_hash && d.ps_hash && FindVariant(d.vs_hash) == vs && FindVariant(d.ps_hash) == ps;
    };
    ctx.fetch_k = [&frame](const Draw& d, float* k) {   // the bake's raw registers (the layout prelight_analytic::Planner::Precompute reads)
      if (d.ps_bank >= frame.constant_banks.count) return false;
      const BankRef bank = frame.constant_banks.Ref(d.ps_bank);
      uint8_t* out = reinterpret_cast<uint8_t*>(k);
      std::memset(out, 0, prelight_compute::kVec4 * 16);
      bank.CopyBytes(8 * 16, 4 * 16, out + 0 * 16);       // DifLghVec
      bank.CopyBytes(0 * 16, 4 * 16, out + 4 * 16);       // DifLghCol
      bank.CopyBytes(198 * 16, 16, out + 8 * 16);         // DifLghMaxD
      for (uint32_t j = 0; j < 4; ++j) bank.CopyBytes((16 + j) * 16, 4, out + 9 * 16 + j * 4);   // DifLghPrm[j].x
      bank.CopyBytes(199 * 16, 16, out + 10 * 16);        // DifLghMaxA
      bank.CopyBytes(204 * 16, 3 * 16, out + 11 * 16);    // SssLobeA
      bank.CopyBytes(207 * 16, 3 * 16, out + 14 * 16);    // SssLobeB
      return true;
    };
    ctx.fetch_screen = [&frame](const Draw& d, float* screen) {   // yPreLight's vertex constant ScreenScale (register 201)
      if (d.vs_bank >= frame.constant_banks.count) return false;
      frame.constant_banks.Ref(d.vs_bank).CopyBytes(201 * 16, 16, reinterpret_cast<uint8_t*>(screen));
      return true;
    };
    ctx.mesh_ok = [this, &capture](const Draw& d) {
      if (d.vertex_count != prelight_compute::kVertices || d.index_count != prelight_compute::kIndices || d.vertex_bytes != prelight_compute::kVertexBytes || d.index_bytes != prelight_compute::kIndexBytes) return false;
      // The geometry keys are content keys (a draw's vertex_offset / index_offset move when the bytes do: reader.h content_key; capture: geometry.bin offsets of equal content are not shared, so each is checked once):
      // the mesh is read and checked against the model once per key, never per frame.
      const uint64_t key = (d.vertex_offset * 0x100000001b3ull) ^ (d.index_offset * 0x9E3779B97F4A7C15ull) ^ (uint64_t(d.vertex_bytes) << 40);
      const bool planted = prelight_compute::Planted(prelight_compute::Fault::BadMesh);
      if (!planted && pc_mesh_ok_.count(key)) return true;
      bool ok = false;
      if (capture.ReadGeometry(d.vertex_offset, d.vertex_bytes, pc_vb_) && capture.ReadGeometry(d.index_offset, d.index_bytes, pc_ib_) && pc_vb_.size() >= d.vertex_bytes && pc_ib_.size() >= d.index_bytes) {
        ++pc_planner_.census.meshes_checked;
        ok = prelight_compute::Planner::CheckMesh(pc_vb_.data(), d.vertex_bytes, d.vertex_count, pc_ib_.data(), d.index_bytes, d.index_count, planted);
        if (ok) pc_mesh_ok_.insert(key);
      }
      return ok;
    };
    const auto t0 = std::chrono::steady_clock::now();
    pc_planner_.Build(frame, ctx, pc_plan_);
    pc_planner_.census.plan_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    ++pc_planner_.census.plan_runs;
    pc_on_ = pc_plan_.count > 0;
  }
  // The dispatch: the taken cubes (the renderer's images) and their constants, five blocks of PreLightK (kLightCubeVec4 vec4 each; unused blocks zero), pushed into the frame's constants (256-aligned).
  void PreLightComputeDispatch(const LoadedFrame& frame, rdr::FrameBuilder& builder) {
    (void)frame;
    rdr::DispatchOp d{};
    float blocks[rdr::kMaxLightCubes * rdr::kLightCubeVec4 * 4] = {};
    for (size_t i = 0; i < pc_plan_.groups.size() && i < size_t(rdr::kMaxLightCubes); ++i) {
      d.cube[i] = Map(pc_plan_.groups[i].cube);
      std::memcpy(blocks + i * rdr::kLightCubeVec4 * 4, pc_plan_.groups[i].k, sizeof(pc_plan_.groups[i].k));
      ++d.cube_count;
    }
    d.constants = builder.PushConstants(blocks, sizeof(blocks));
    builder.Dispatch(d);
    ++pc_planner_.census.dispatches;
    pc_planner_.census.cubes_written += d.cube_count;
  }
  // ---- S102 lane C1 (prelight_layered.h, docs/renderer/PRELIGHT_DIRECT.md section 7): the pre-light strips' six copies gone ------------------------------------------------------------------------
  // Switch: wwe2k14_rdr_prelight_layered (PerFrame; fview / rplay --prelight-layered), read in PreLightBegin with the guest frame. A taken group = SetTargets(strip), one pre-light draw, six copies (strip tile j ->
  // cube layer j): the SetTargets is not issued (no strip is made), the draw is issued ONCE into a layered target (the cube's layers 0..5, one multiview pass) with our `yPreLight_base_layered` vertex program (view v
  // draws tile v onto the 64x64 face) and the face-sized viewport, and the six copies are left out. If the draw was not drawn by small_passes_ours the group falls back at once (the stream's SetTargets / draw / copies).
  prelight_layered::Planner pl_planner_;
  prelight_layered::Plan pl_plan_;
  bool pl_on_ = false;        // this frame has a taken group (the op loop reads it)
  bool pl_enabled_ = false;   // the switch is on this frame
  uint32_t pl_window_ = 0;
  bool pl_prev_part_open_ = false;   // the previous bind was a part with more of its guest frame to come
  bool pl_layered_ = false;   // true while the layered draw is being bound (BindDraw reads it after the hook's decision)
  bool pl_bad_pad_done_ = false;
  bool pl_refused_ = false;   // BindDraw: the owned hook did not draw it (the draw was not issued)
  int pl_vs_ = -2, pl_vs_swap_ = -2;   // registry indices of the layered vertex programs (-1: not in the registry)
  std::unordered_set<uint32_t> pl_target_ids_;   // capture image ids of light cubes made with target usage
  std::vector<uint8_t> pl_og_scratch_;
  // SHAPE ONLY (S102 C1 review F9): a 64x64, six-layer, one-mip, sampled copy destination: a candidate light cube. The planner proves the rest (the group, the strip, the face map) per frame; this decides only whether the image is MADE with target usage.
  static bool PreLightCubeShape(const ImageRecord& r) { return r.width == 64 && r.height == 64 && r.layers == 6 && r.mips == 1 && (r.flags & kImageCopyDest) && (r.usage & rdr::kUsageSampled); }
  void PreLightBegin(const LoadedFrame& frame, const BindStats& stats) {
    pl_on_ = false;
    const bool prev_open = pl_prev_part_open_;   // (review of G14: a frame split at a read-back: the strip's readers may be in an earlier part)
    pl_prev_part_open_ = frame_part_open && !persist_only_bind;
    pl_enabled_ = rdr_sw::BoolSeen(rdr_sw::kPrelightLayered, prelight_layered::g_fview.load(std::memory_order_relaxed), frame.header.guest_frame);
    if (!pl_enabled_) return;
    if (pl_vs_ == -2) pl_vs_ = VariantByName(prelight_layered::kLayeredVertex), pl_vs_swap_ = VariantByName(prelight_layered::kLayeredVertexSwap);
    prelight_layered::Ctx ctx;
    ctx.plan_ok = !(stats.isolate || velocity_mode_ || persist_only_bind || frame_part_open || prev_open || !frame.ops.count);   // a lasting-only bind and either part of a split frame keep the stream's strip and copies
    ctx.guest_frame = frame.header.guest_frame;
    ctx.multiview = renderer_.multiview_supported() && pl_vs_ >= 0 && pl_vs_swap_ >= 0;
    for (const ReplaceSlot& slot : replace_slots_)
      if (slot.hook && slot.enabled == &g_small_passes_ours && *slot.enabled) ctx.hook_on = true;
    ctx.record = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    ctx.cube_has_target = [this](uint32_t id) { return pl_target_ids_.count(id) != 0; };
    if (dead_on_ && dead_plan_.drop.size() >= frame.ops.count) ctx.dead = dead_plan_.drop.data();
    if (bake_on_ && bake_drop_.size() >= frame.ops.count) ctx.others.push_back(bake_drop_.data());
    if (PostClaimPlanned(frame.ops.count)) ctx.others.push_back(post_claim_.drop.data());
    if (turns_on_ && turns_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(turns_plan_.drop.data());
    if (plane_blank_on_ && plane_blank_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(plane_blank_plan_.drop.data());
    if (pa_on_ && pa_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(pa_plan_.drop.data());   // S102 L1: the analytic lever took the group (C1 declines on overlap)
    if (pc_on_ && pc_plan_.drop.size() >= frame.ops.count) ctx.others.push_back(pc_plan_.drop.data());   // S102 L1b: the compute dispatch took the group (C1 declines on overlap)
    if (og_on_ && og_plan_.group_of_op.size() >= frame.ops.count) {   // the own groups taken this frame (shadow module / rim map)
      pl_og_scratch_.assign(frame.ops.count, 0);
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        const uint8_t g = og_plan_.group_of_op[i];
        if (g != own_group::kNone && g < own_group::kGroupCount && og_plan_.taken[g]) pl_og_scratch_[i] = 1;
      }
      if (pl_og_scratch_.size() >= frame.ops.count) ctx.others.push_back(pl_og_scratch_.data());   // S102 G4c F4: size-guarded (just assigned ops.count)
    }
    pl_planner_.Build(frame, ctx, pl_plan_);
    pl_on_ = pl_plan_.count > 0;
  }
  // BindDraw, after the owned hook's decision: our layered vertex program for the stream's pixel stage, the viewport and scissor of ONE 64x64 face (each view draws its tile over the whole face), x of the clip
  // transform identity (the layered program already puts the tile on the face; the stream's own x scale / offset belong to the strip's viewport).
  void PreLightLayeredDraw(rdr::DrawState& state, rdr::DynamicState& d, int ps, bool alpha_twin) {
    const int vs = prelight_layered::Planted(prelight_layered::Fault::SwapFaces) ? pl_vs_swap_ : pl_vs_;
    state.program = Program(vs, ps, alpha_twin);
    d.viewport[0] = 0.0f;
    d.viewport[2] = float(prelight_layered::kFace);
    const int32_t x0 = std::max<int32_t>(0, d.scissor.x), y0 = std::max<int32_t>(0, d.scissor.y);
    const int32_t edge = prelight_layered::Planted(prelight_layered::Fault::ShrinkScissor) ? int32_t(prelight_layered::kFace) - 1 : int32_t(prelight_layered::kFace);
    const int32_t x1 = std::min<int32_t>(edge, d.scissor.x + int32_t(d.scissor.width)), y1 = std::min<int32_t>(edge, d.scissor.y + int32_t(d.scissor.height));
    d.scissor = {x0, y0, uint32_t(x1 > x0 ? x1 - x0 : 0), uint32_t(y1 > y0 ? y1 - y0 : 0)};
    // The stream's clip transform (ndc scale / offset, the game's viewport and half-pixel rule for the 384x64 strip) is applied AFTER our vertex program: the program already put the tile on the 64x64 face
    // (x -> 6 x + 5 - 2 v), so the strip's x offset must grow by the same factor 6 (a strip offset of o is 6 o in face clip units; the scale is the strip's own and stays).
    if (!prelight_layered::Planted(prelight_layered::Fault::NdcOffsetUnscaled)) d.system[4] *= 6.0f;
  }
  // One op of a taken group. Role 1 (the strip's SetTargets): nothing (no strip is made). Role 2 (the draw): ONE layered pass into the cube. Role 3 (a copy): left out (issued when the group fell back).
  // Role 4 (the planted fallback_no_strip twin only): the strip's ops are left out although the group was declined.
  template <class Source>
  void PreLightOp(const LoadedFrame& frame, const Source& capture, uint32_t op_index, rdr::FrameBuilder& builder, std::vector<uint8_t>& bytes, BindStats& stats) {
    prelight_layered::Group& g = pl_plan_.groups[pl_plan_.group_of[op_index]];
    const uint8_t role = pl_plan_.role[op_index];
    draw_trace_.outcome = BindOutcome::LeftOut;
    if (role == 4) {   // the planted twin: the fallback declined the group, and the strip is not made all the same
      if (frame.ops[op_index].kind == OpKind::Draw) ++pl_planner_.census.strips_unmade;
      return;
    }
    if (role == 1) return;
    if (role == 3) {
      if (g.fallback || prelight_layered::Planted(prelight_layered::Fault::KeepCopies)) IssueCopy(frame, op_index, capture, builder, stats);
      else ++pl_planner_.census.copies_dropped;
      return;
    }
    const Draw& draw = frame.draws[frame.ops[op_index].index];
    rdr::ImageRef cube{};
    cube.image = Map(g.cube);
    const uint32_t views = prelight_layered::Planted(prelight_layered::Fault::MissingLayer) ? prelight_layered::kFaces - 1 : prelight_layered::kFaces;
    const rdr::ImageRef layered = rdr::Layered(cube, views);
    IssueTargets(frame, g.set_op, builder, stats, &layered);
    const BindStats stats_before = stats;   // (a refused layered attempt must not count twice when the stream's draw follows)
    pl_layered_ = true, pl_refused_ = true;   // C1 review F4: refused until builder.Draw(op) is reached (BindDraw clears it there)
    draw_trace_ = BindTrace{};
    BindDraw(frame, draw, capture, builder, bytes, stats);
    pl_layered_ = false;
    if (pl_refused_) {   // the owned hook did not draw it (or the draw never reached the builder): the stream's own ops (the layered pass above stays empty)
      pl_refused_ = false;
      stats = stats_before;
      ++pl_planner_.census.fallback_not_ours;
      g.fallback = true;
      IssueTargets(frame, g.set_op, builder, stats);
      draw_trace_ = BindTrace{};
      BindDraw(frame, draw, capture, builder, bytes, stats);
      return;
    }
    ++pl_planner_.census.passes_layered;
    ++pl_planner_.census.strips_unmade;
  }
  void AliasBegin(const LoadedFrame& frame, const BindStats& stats) {
    alias_on_ = copy_alias::g_fault == copy_alias::Fault::Outlive && !alias_set_.empty();   // (the planted Outlive fault keeps last frame's alias)
    dr_on_ = false, dr_dropped_ = 0;   // S102 C5: the direct plan is made per frame below
    if (copy_alias::g_mode <= 0 && copy_alias::g_fullsize <= 0 && copy_alias::g_small <= 0 && !copy_alias::g_shadow_direct && !copy_alias::g_frame_direct && copy_alias::g_direct <= 0) return;   // S102 G8 / G12: the full-size and small-class switches also run the planner (each takes only its own copies)
    //   // S102 G8: the full-size switch alone also runs the planner (it takes only full-size copies)
    ++copy_alias_.census.frames;
    if (copy_alias::g_plant_readback_frame >= 0 && int64_t(copy_alias_.census.frames) == copy_alias::g_plant_readback_frame)   // dev (fview --copy-alias-readback): a planted CPU read-back of these destinations
      for (const uint32_t id : copy_alias::g_plant_readback_ids) copy_alias::NoteReadback(id);
    if (stats.isolate || velocity_mode_ || !frame.ops.count) {  // an isolate / velocity bind (not a guest frame) keeps every copy and is not observed
      ++copy_alias_.census.frames_skipped;
      return;
    }
    copy_alias::Facts facts;
    facts.record = [this](uint32_t id) -> const ImageRecord* {
      const auto found = rec_index_.find(id);
      return found == rec_index_.end() || found->second >= synced_records_.size() ? nullptr : &synced_records_[found->second];
    };
    // S102 G3 (review F7): a shadow map the half rate or a turn leaves out on alternate frames is no copy to elide: an elided copy leaves its destination unwritten, and the frame that leaves the next copy out would then read it stale.
    facts.excluded = [this](uint32_t id) { return ShadowOursImage(id) != nullptr || (shadows_active_ && shadows_plan_.Of(id) != shadows_ours::kNone) || (g_shadow_half && shadow_sets_.targets.count(id)) || turns_planner_.Tracks(id) || (plane_blank_enabled_ && plane_blank_planner_.IsChainImage(id)); };   // (S102 G4b F7: the chain images while its switch is on)
    facts.op_excluded = [this, &frame](uint32_t p) {
      const Op& op = frame.ops[p];
      if (op.kind != OpKind::Copy) return false;
      if (PostClaimsOp(frame.ops.count, p)) return true;                                                               // post_ours drops it (S102 G12 review F1: state 1 at plan time)
      if (bake_on_ && bake_drop_[p]) return true;                                                                        // S102 P1: a bake pass's copy is re-issued by ours, not recorded here
      if (dead_on_ && dead_plan_.drop[p]) return true;                                                                   // S102 lane G4: a dead pass's copy is not recorded
      if (turns_on_ && turns_plan_.drop[p]) return true;                                                                 // S102 lane G3: a map whose turn is off
      if (og_on_ && og_plan_.group_of_op[p] != own_group::kNone && og_plan_.taken[og_plan_.group_of_op[p]] && own_group::g_none[og_plan_.group_of_op[p]].load(std::memory_order_relaxed)) return true;   // S102 P4P5: the `none` arm leaves the group's copies out; the alias plan does not judge them (review item 6; the plan above runs BEFORE ShadowsOursBegin / AliasBegin)
      if (pa_on_ && pa_plan_.drop[p]) return true;                                                                       // S102 L1: a pre-light face copy whose bake is not drawn is not recorded
      if (pc_on_ && pc_plan_.drop[p]) return true;                                                                       // S102 L1b: a pre-light face copy the compute dispatch replaces is not recorded
      if (pl_on_ && pl_plan_.role[p] == 3 && !prelight_layered::Planted(prelight_layered::Fault::KeepCopies)) return true;   // S102 C1: a pre-light face copy the layered pass replaces is not recorded
      if (plane_blank_on_ && plane_blank_plan_.drop[p]) return true;                                                                 // S102 lane G4b: a blank plane chain's copy is not recorded
      return OldHalfSkips(frame.copies[op.index].source.image);                                                          // the half-rate shadow maps drop it
    };
    facts.role = [this](uint32_t id) { return ImageRole(id); };
    facts.frame_direct = copy_alias::g_frame_direct && !(g_dlss || g_velocity_census);   // S102 C3: the frame buffer's copies: the full-size rule at level 3 (segments, depth, the Present), its own switch and census
    facts.fullsize = (g_dlss || g_velocity_census) ? 0 : copy_alias::ClampLevel(copy_alias::g_fullsize);
    if (facts.frame_direct && facts.fullsize < 3) facts.fullsize = 3;   // S102 C3: the frame-direct switch is the full-size rule at level 3 (segments, depth, the Present), counted in the fd_ fields   // S102 G8: not with DLSS / a velocity pass (they read the scene's depth and colour at the Present: not judged)
    facts.small_level = copy_alias::ClampSmall(copy_alias::g_small);   // S102 G12: the class-mask override
    // S102 C3 F7: the D3 markers' reads of images (the arena / caster / ring / wrestler sequences) are invisible to the planner's events: neither direct switch runs on such a frame
    const bool d3_blind = d3_on_ || d3r_on_ || wr_on_ || d3c_on_;
    if (d3_blind && (copy_alias::g_shadow_direct || copy_alias::g_frame_direct)) ++copy_alias_.census.d3_blind;
    if (d3_blind) facts.frame_direct = false, facts.fullsize = (g_dlss || g_velocity_census) ? 0 : copy_alias::ClampLevel(copy_alias::g_fullsize);
    facts.base_fullsize = (g_dlss || g_velocity_census) ? 0 : copy_alias::ClampLevel(copy_alias::g_fullsize);   // S102 C3 F5
    // S102 C3 F1: the half rate (every tier's default since 3c166f4d) draws the learned shadow maps on alternate frames and leaves their copies out on the others: a texture whose copy a skipped frame does not make
    // must hold the last real copy, so it is never aliased. A destination is blocked when ANY copy into it this frame has a source the half rate skips; everything is blocked while nothing is learned yet.
    sd_blocked_dests_.clear();
    if (g_shadow_half) {
      for (uint32_t p = 0; p < frame.ops.count; ++p) {
        if (frame.ops[p].kind != OpKind::Copy) continue;
        const rdr::CopyOp& c = frame.copies[frame.ops[p].index];
        if (shadow_sets_.Skips(c.source.image)) sd_blocked_dests_.insert(c.dest.image);
      }
    }
    facts.shadow_direct = copy_alias::g_shadow_direct && shadows_active_ && !d3_blind;   // S102 C3: shadow_direct needs shadows_ours (its images are the ones aliased)
    facts.shadow_dims = [this](uint32_t id) -> uint32_t { const ShadowOurs* o = ShadowOursImage(id); return o ? (o->w << 16 | o->h) : 0u; };   // S102 C3 F10
    facts.shadow_blocked = [this](uint32_t id) { return (g_shadow_half && (shadow_sets_.Empty() || shadow_sets_.Skips(id) || sd_blocked_dests_.count(id))) || turns_planner_.Tracks(id) || (plane_blank_enabled_ && plane_blank_planner_.IsChainImage(id)); };   // (turn / plane-blank terms as in facts.excluded; the shadows_ours term is what shadow_direct lifts)
    facts.small_mask = copy_alias::g_small_bake_off.load(std::memory_order_relaxed) ? (copy_alias::kClassAll & ~copy_alias::kClassBake) : copy_alias::kClassAll;   // S102 G12 review F2
    facts.direct_level = (g_dlss || g_velocity_census || velocity_mode_) ? 0 : copy_alias::ClampDirect(copy_alias::g_direct);   // S102 C5
    if (facts.direct_level > 0) {
      dr_set_dest_.assign(frame.ops.count, 0), dr_copy_.assign(frame.ops.count, 0);
      facts.direct_check = [this, &frame](uint32_t p, const rdr::CopyOp& copy) { return DirectCheck(frame, p, copy); };
    }
    facts.present_ok = !stage_frame_on_ && !g_dlss && !g_velocity_census;            // S102 G8: the Present path maps its source (StageViewCopy writes into the picture only for a stage-view frame)
    facts.extend_to_present = g_dlss || g_velocity_census;  // S4: the velocity pass redraws wrestler draws at the Present, after the last stream reader
    if (frame_part_open) {  // a part of a frame split at a readback keeps every copy (the readback reads the real destination); its reads are still checked (Observe)
      ++copy_alias_.census.frames_skipped;
      copy_alias_.Observe(frame, facts, true);
      return;
    }
    if (facts.direct_level > 0) DirectScan(frame);   // S102 C5 (after the part-frame return: a part of a split frame would show reads before their pass)
    { plan_memo::Timer tm_(plan_memo::kTmAliasBuild); copy_alias_.Build(frame, facts, copy_alias::g_mode, alias_plan_); }
    if (facts.direct_level > 0 && alias_plan_.direct_count) {   // S102 C5
      if (copy_alias::g_fault != copy_alias::Fault::DirectStaleSrc || dr_frames_ < 3)   // PLANTED (DirectStaleSrc): from the third frame the copies stay although their passes draw into the destinations
        for (uint32_t p = 0; p < frame.ops.count; ++p)
          if (alias_plan_.direct[p]) dr_copy_[p] = 1;
      if (copy_alias::g_fault == copy_alias::Fault::DirectNoRetarget) std::fill(dr_set_dest_.begin(), dr_set_dest_.end(), 0u);   // PLANTED: the copy goes, the pass keeps drawing into its own target
      if (copy_alias::g_fault == copy_alias::Fault::DirectWrongDest) {   // PLANTED: each direct pass renders into the next direct pass's destination (a bake written to the wrong atlas cell)
        std::vector<uint32_t> ops;
        for (uint32_t p = 0; p < frame.ops.count; ++p)
          if (dr_set_dest_[p]) ops.push_back(p);
        for (size_t i = 0; i + 1 < ops.size(); ++i) std::swap(dr_set_dest_[ops[i]], dr_set_dest_[ops[i + 1]]);
      }
      dr_on_ = true;
    }
    alias_on_ = alias_on_ || alias_plan_.count > 0;   // (the plan is only filled when something is taken: mode 2, or the full-size switch)
    if (alias_on_ && alias_.size() < image_ids_.size()) alias_.resize(image_ids_.size(), 0);
  }
  // The image table the hooks read (a capture image id -> the renderer image): the binder's own, or, while aliases are set, a copy in which every aliased destination names its source.
  // S102 C3 F3: with the Present aliased to its source (frame_direct), last_present_ names the scene target itself. A part of a split frame (a read-back) or a persist bind that draws into that image would
  // overwrite the last picture (the run-exit / case-ring picture, photo mode): the picture is copied into the Present image the game owns first, and last_present_ names that copy.
  struct FdLast { bool valid = false; rdr::ImageId src = rdr::kNoImage, dest = rdr::kNoImage; uint32_t w = 0, h = 0; };
  FdLast fd_last_;
  uint64_t fd_pictures_saved_ = 0;
  void FdSaveLastPicture(const rdr::TargetsOp& targets, rdr::FrameBuilder& builder) {
    if (copy_alias::g_fault == copy_alias::Fault::FdNoLastPicGuard) return;   // planted
    bool hit = targets.depth.image == fd_last_.src;
    for (uint32_t c = 0; c < targets.color_count; ++c) hit |= targets.color[c].image == fd_last_.src;
    if (!hit) return;
    rdr::CopyOp copy;
    copy.source.image = fd_last_.src;
    copy.dest.image = fd_last_.dest;
    copy.source_rect = copy.dest_rect = rdr::Rect{0, 0, fd_last_.w, fd_last_.h};
    copy.filter = rdr::CopyFilter::Nearest;
    builder.Copy(copy);   // allow:last_picture
    last_present_.source.image = fd_last_.dest;
    last_present_.source.mip = 0, last_present_.source.layer = 0;
    fd_last_ = {};
    ++fd_pictures_saved_;
  }
  std::vector<rdr::ImageId> alias_images_;
  bool alias_images_dirty_ = true;
  const rdr::ImageId* HookImages() {
    if (scene_frame_id_ && scene_frame_id_ < image_ids_.size()) {   // S103 lane S: the hooks see our frame for the frame image too
      alias_images_ = image_ids_;
      for (const uint32_t d : alias_set_)
        if (alias_on_ && d < alias_images_.size()) alias_images_[d] = Map(d);
      alias_images_[scene_frame_id_] = scene_frame_image_;
      alias_images_dirty_ = true;
      return alias_images_.data();
    }
    if (!alias_on_ || alias_set_.empty()) return image_ids_.data();
    if (alias_images_dirty_ || alias_images_.size() != image_ids_.size()) {
      alias_images_ = image_ids_;
      for (const uint32_t d : alias_set_)
        if (d < alias_images_.size()) alias_images_[d] = Map(d);
      alias_images_dirty_ = false;
    }
    return alias_images_.data();
  }
  // ---- S102 C5: copy_direct (copy_alias.h g_direct, copy_direct.h the rule; docs\renderer\COPIES_LEFT.md section 6) -------------------------------------------------------------------------
  // A copy the strict alias rule admits (crowd bake, small `other`) is removed by drawing its pass straight into the copy's destination. DirectScan reads the frame's ops once (a flat table by game image id, a
  // frame stamp instead of clearing it: no allocation after the table has grown to the image count); DirectCheck gathers the facts of one candidate copy and asks copy_direct::Decide, which a unit test
  // (tools\c5\copy_direct_test.cpp) plants every rule away from. The ledger (dr_stale_d_ / dr_banned_) is the one stale-read check: the removal leaves the source unwritten, so a LATER frame that reads a
  // taken source (a copy from it, the Present) before the frame has drawn it bans that source for good (review F1).
  struct DrScan { uint32_t stamp = 0, set_op = 0; uint8_t sets = 0, copies = 0, copy_dests = 0, presents = 0; };
  static bool DirectShape(const ImageRecord& r) {   // (F4: a picture that follows the render scale is taken too, when both ends do: DirectCheck)
    return r.width && r.height && r.width <= copy_alias::kDirectMaxEdge && r.height <= copy_alias::kDirectMaxEdge && r.mips == 1 && r.layers == 1 && r.depth == 1 && r.kind == uint8_t(rdr::ImageKind::Image2D) &&
           (r.flags & kImageCopyDest) && (r.usage & rdr::kUsageSampled) && !(r.usage & (rdr::kUsageTarget | rdr::kUsageTransient));
  }
  DrScan* DrAt(uint32_t id) {
    if (id >= dr_scan_.size()) return nullptr;
    DrScan& e = dr_scan_[id];
    if (e.stamp != dr_stamp_) e = DrScan{dr_stamp_, 0, 0, 0, 0, 0};
    return &e;
  }
  void DirectScan(const LoadedFrame& frame) {
    if (dr_scan_.size() < image_ids_.size()) dr_scan_.resize(image_ids_.size());
    if (dr_stale_d_.size() < image_ids_.size()) dr_stale_d_.resize(image_ids_.size(), 0), dr_banned_.resize(image_ids_.size(), 0);
    ++dr_stamp_;
    const bool plant_read = copy_alias::g_fault == copy_alias::Fault::DirectReadSrc && dr_frames_ >= 2;   // PLANTED: the ledger is shown a read of every taken source before its pass
    if (plant_read)
      for (uint32_t s = 0; s < dr_stale_d_.size(); ++s)
        if (dr_stale_d_[s] && !dr_banned_[s]) dr_banned_[s] = 1, ++copy_alias_.census.dr_violations;
    ++dr_frames_;
    const auto read_before_draw = [this](uint32_t id, const DrScan& e) {   // a taken source read before this frame drew it
      if (id < dr_stale_d_.size() && dr_stale_d_[id] && !dr_banned_[id] && e.sets == 0) dr_banned_[id] = 1, ++copy_alias_.census.dr_violations;
    };
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::SetTargets) {
        const rdr::TargetsOp& t = frame.targets[op.index];
        for (uint32_t c = 0; c < t.color_count; ++c)
          if (DrScan* e = DrAt(t.color[c].image)) {
            if (e->sets < 255) ++e->sets;
            e->set_op = i;
          }
      } else if (op.kind == OpKind::Copy) {
        const rdr::CopyOp& cp = frame.copies[op.index];
        if (DrScan* e = DrAt(cp.source.image)) {
          read_before_draw(cp.source.image, *e);
          if (e->copies < 255) ++e->copies;
        }
        if (DrScan* e = DrAt(cp.dest.image)) {
          if (e->copy_dests < 255) ++e->copy_dests;
        }
      } else if (op.kind == OpKind::Present) {
        const uint32_t id = frame.presents[op.index].source.image;
        if (DrScan* e = DrAt(id)) {
          read_before_draw(id, *e);
          if (e->presents < 255) ++e->presents;
        }
      }
    }
  }
  int DirectCheck(const LoadedFrame& frame, uint32_t p, const rdr::CopyOp& copy) {
    const uint32_t S = copy.source.image, D = copy.dest.image;
    const auto rs = rec_index_.find(S), rd = rec_index_.find(D);
    if (rs == rec_index_.end() || rd == rec_index_.end() || rs->second >= synced_records_.size() || rd->second >= synced_records_.size()) return copy_direct::kDirectNoPass;
    const ImageRecord& src = synced_records_[rs->second];
    const ImageRecord& dst = synced_records_[rd->second];
    const DrScan* e = DrAt(S);
    copy_direct::Facts f;
    if (e) f.sets = e->sets, f.copies = e->copies, f.copy_dests = e->copy_dests, f.presents = e->presents;
    f.source_sampled = (src.usage & rdr::kUsageSampled) != 0;
    f.dest_read_back = copy_alias::WasReadBack(D);
    f.dest_target_capable = dr_target_ids_.count(D) != 0;
    f.banned = S < dr_banned_.size() && dr_banned_[S];
    f.scale_mismatch = ((src.usage ^ dst.usage) & rdr::kUsageScaled) != 0;
    // F2: a whole 1:1 copy, proven here and not only by the alias rule (a planted alias fault bypasses that one)
    f.whole_copy = !copy.source.mip && !copy.source.layer && !copy.dest.mip && !copy.dest.layer && copy.source_rect.x == 0 && copy.source_rect.y == 0 && copy.dest_rect.x == 0 && copy.dest_rect.y == 0 &&
                   copy.source_rect.width == copy.dest_rect.width && copy.source_rect.height == copy.dest_rect.height && copy.source_rect.width >= src.width && copy.source_rect.height >= src.height &&
                   src.width == dst.width && src.height == dst.height && src.format == dst.format && src.kind == dst.kind && src.mips == 1 && dst.mips == 1 && src.layers == 1 && dst.layers == 1;
    const uint32_t set_op = e ? e->set_op : 0;
    f.pass_before_copy = e && e->sets == 1 && set_op < p && frame.ops[set_op].kind == OpKind::SetTargets;
    if (f.pass_before_copy) {
      const rdr::TargetsOp& t = frame.targets[frame.ops[set_op].index];
      f.pass_colour_targets = uint8_t(t.color_count);
      if (!t.color_count || t.color[0].image != S) f.pass_before_copy = false;
      f.only_clears_draws = true;
      for (uint32_t i = set_op + 1; i < p; ++i)
        if (frame.ops[i].kind != OpKind::Clear && frame.ops[i].kind != OpKind::Draw) f.only_clears_draws = false;
      // F1 (1): the pass starts with a full colour clear, so the pixels its draw leaves alone are the clear colour in D as they were in S (never the leftovers of an earlier use of the picture)
      if (set_op + 1 < p && frame.ops[set_op + 1].kind == OpKind::Clear) {
        const rdr::ClearOp& c = frame.clears[frame.ops[set_op + 1].index];
        f.first_op_full_clear = c.full && (c.flags & rdr::kClearColor) && (c.color_mask & 1u);
      }
    }
    const int why = copy_direct::Decide(f);
    if (why == copy_direct::kDirectOk) {
      dr_set_dest_[set_op] = D;
      if (S < dr_stale_d_.size()) dr_stale_d_[S] = D;   // the ledger: S is no longer written by the stream's copy path from here on
    }
    return why;
  }
  bool dr_on_ = false;
  uint32_t dr_dropped_ = 0, dr_stamp_ = 0, dr_frames_ = 0;
  std::vector<uint32_t> dr_set_dest_;   // by op index: the game's id of the destination the pass whose SetTargets this is renders into (0 = none)
  std::vector<uint8_t> dr_copy_;        // by op index: 1 = the copy is removed
  std::vector<DrScan> dr_scan_;         // by game image id (flat, stamped per frame)
  std::vector<uint32_t> dr_stale_d_;    // the ledger: taken source -> its destination (0 = none)
  std::vector<uint8_t> dr_banned_;      // sources a later frame read stale: never taken again
  std::unordered_set<uint32_t> dr_target_ids_;   // images made with target usage for the direct switch
  void SetAlias(uint32_t dest, uint32_t source) {
    alias_images_dirty_ = true;
    if (dest >= alias_.size()) alias_.resize(size_t(dest) + 1, 0);
    alias_[dest] = source;
    alias_set_.push_back(dest);
  }
  // S102 G8: the destination is written for real from here on (a kept copy into it): its alias for this frame ends. Returns true only for the planted fault FsNoClear: the copy is swallowed and the
  // alias stays (a kept copy treated as elided: the next segment's readers read the OLD source).
  bool EndAliasOf(uint32_t dest) {
    if (dest >= alias_.size() || !alias_[dest]) return false;
    if (copy_alias::g_fault == copy_alias::Fault::FsNoClear) return true;
    alias_[dest] = 0;
    alias_images_dirty_ = true;
    return false;
  }
  void AliasEnd() {
    if ((copy_alias::g_mode > 0 || copy_alias::g_fullsize > 0 || copy_alias::g_small > 0 || copy_alias::g_shadow_direct || copy_alias::g_frame_direct || copy_alias::g_direct > 0) && (++alias_window_ >= 600 || census_flush::Due(g_cf_alias))) {
      rdr::Log("%s", copy_alias_.Report().c_str());
      alias_window_ = 0;
    }
    if (pa_enabled_ && (++pa_window_ >= 600 || census_flush::Due(g_cf_pa))) {   // S102 L1 (common.md "every lever logs a live census line")
      rdr::Log("%s", pa_planner_.Report().c_str());
      pa_window_ = 0;
    }
    if (pc_enabled_ && (++pc_window_ >= 600 || census_flush::Due(g_cf_pc))) {   // S102 L1b (common.md "every lever logs a live census line"; relaxed counters, text built here only)
      rdr::Log("%s", pc_planner_.Report().c_str());
      pc_window_ = 0;
    }
    if (pl_enabled_ && (++pl_window_ >= 600 || census_flush::Due(g_cf_pl))) {   // S102 C1 (common.md "every lever logs a live census line")
      rdr::Log("%s", pl_planner_.Report().c_str());
      pl_window_ = 0;
    }
    if (plane_blank_enabled_ && (++plane_blank_window_ >= 600 || census_flush::Due(g_cf_plane_blank))) {   // S102 main (06:20, common.md "every lever logs a live census line"): the live game showed no G4b line
      rdr::Log("%s", plane_blank_planner_.Report().c_str());
      plane_blank_window_ = 0;
    }
    if (copy_alias::g_fault == copy_alias::Fault::Outlive) return;   // planted: the alias stays into the next frame (alias_on_ too)
    for (const uint32_t id : alias_set_) alias_[id] = 0;
    alias_set_.clear();
    alias_on_ = false;
  }
 public:
  std::string CopyAliasReport() const {
    char b[200];
    std::snprintf(b, sizeof b, "\n[rdr][copy_alias]   elided on claimed ops (alias_elide_on_claimed) %llu (must be 0: the post claim drops these copies itself)", static_cast<unsigned long long>(alias_elide_on_claimed_));
    return copy_alias_.Report() + b;
  }
  const copy_alias::Census& copy_alias_census() const { return copy_alias_.census; }
