
  // ---- S102 P4P5: the shadow module's and the rim map's draw nodes as groups (own_group.h; docs\renderer\OWN_SHADOW_RIM.md) ------------------------------------------------------------
  // OwnGroupPlan: lay the frame's passes out (a pass = a SetTargets up to the next), say whose each is from the draws' origins and pixel programs, and, for a group whose switch is on and
  // which is ONE run of the frame's ops, mark it a unit. A unit is re-issued at its first pass's place: the order is the stream's, so a unit that is one run needs no move; the ops go
  // through the walk as they are (their draws still pass the replace hooks: the casters / rim map ours). The `none` arm (test) leaves the unit's ops out. A group with a foreign pass
  // inside is counted and left as the stream has it (the node's walk was not a clean unit: the contract's fallback).
  [[gnu::noinline]] void OwnGroupPlan(const LoadedFrame& frame, const BindStats& stats) {
    og_on_ = false;
    const bool want[own_group::kGroupCount] = {false, own_group::g_on[own_group::kModule].load(std::memory_order_relaxed), own_group::g_on[own_group::kRim].load(std::memory_order_relaxed)};
    if (!(want[own_group::kModule] || want[own_group::kRim]) || velocity_mode_ || stats.isolate || !frame.ops.count) return;
    og_builder_.Reset();
    const auto ps_is = [&](const Draw& d, const char* name) {
      const int ps = FindVariant(d.ps_hash);
      return ps >= 0 && rdr::fx::kVariants[ps].name && std::strcmp(rdr::fx::kVariants[ps].name, name) == 0;
    };
    const auto draw_facts = [&](const Draw& d, const DrawOrigin* o) {
      own_group::DrawFacts f;
      if (!o) return f;
      f.has_origin = true, f.op = o->op, f.pass_site = o->pass_site, f.call_site = o->call_site, f.part = o->part;
      f.ps_flag_any = ps_is(d, "yBlur_flag_any_fragment");
      f.ps_t2 = ps_is(d, "yBlur_t2_fragment");
      f.ps_min = ps_is(d, "yBlur_min_pair_fragment") || ps_is(d, "yBlur_min_rgba_fragment") || ps_is(d, "yBlur_min_r_fragment") || ps_is(d, "yBlur_min_rg_fragment");
      return f;
    };
    std::vector<own_group::DrawFacts> og_draw_facts;   // per op (only draws filled): the oracle below reads them again
    og_draw_facts.resize(frame.ops.count);
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::SetTargets) og_builder_.SetTargets(i);
      else if (op.kind == OpKind::Clear) og_builder_.Clear();
      else if (op.kind == OpKind::Copy) og_builder_.Copy();
      else if (op.kind == OpKind::Present) og_builder_.Present(i);   // a pass never swallows the present
      else if (op.kind == OpKind::Draw) {
        og_draw_facts[i] = draw_facts(frame.draws[op.index], frame.OriginOf(op.index));
        og_builder_.Draw(og_draw_facts[i]);
      }
    }
    own_group::Layout(og_builder_.Finish(frame.ops.count), frame.ops.count, og_plan_);
    for (uint8_t g = own_group::kModule; g < own_group::kGroupCount; ++g) {
      const own_group::GroupFacts& gf = og_plan_.g[g];
      if (!gf.Present()) continue;
      ++og_.frames[g];
      og_.passes[g] += gf.passes, og_.ops[g] += gf.ops, og_.draws[g] += gf.draws, og_.copies[g] += gf.copies;
      if (!want[g]) continue;
      if (!gf.OneRun()) {
        ++og_.frames_not_one_run[g];
        continue;
      }
      // Exclusivity (review item 6): an op of the group that another planner of this frame already owns (G4's dead work, P1's bake) is not ours to cut or re-issue: the group is declined.
      bool overlap = false;
      for (uint32_t o = gf.first_op; o < gf.last_op && !overlap; ++o) {
        if (og_plan_.group_of_op[o] != g) continue;
        overlap = (dead_on_ && dead_plan_.drop[o]) || (bake_on_ && bake_drop_[o]) || (turns_on_ && turns_plan_.drop[o]) || PostClaimsOp(frame.ops.count, o) ||   /* S102 main: G3 turns + the post claim added at the merge (G4b plans AFTER this, and its chain images are not these groups') */ (own_group::g_fault.load(std::memory_order_relaxed) == own_group::kFaultOverlap && o == gf.first_op);   // (the fault: planted overlap)   // (the particle runs are planned per draw later in the walk and hold no group draw: their origins are the particle sites)
      }
      if (overlap) {
        ++og_.declined_overlap[g];
        continue;
      }
      og_plan_.taken[g] = true;
      ++og_.frames_taken[g];
      og_on_ = true;
    }
    // The rule-independent oracle (the `none` arm): a draw kept in the walk whose ORIGIN is a taken group's, whatever pass the classifier put it in.
    for (uint8_t g = own_group::kModule; g < own_group::kGroupCount; ++g) {
      if (!own_group::g_none[g].load(std::memory_order_relaxed) || !want[g]) continue;   // (whether or not the group was taken: a group the planner could not take leaves every one of its draws in the walk)
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        if (frame.ops[i].kind != OpKind::Draw || (og_plan_.taken[g] && og_plan_.group_of_op[i] == g)) continue;
        if (g == own_group::kModule ? own_group::OriginIsModule(og_draw_facts[i]) : own_group::OriginIsRim(og_draw_facts[i])) ++og_.oracle_left[g];
      }
    }
  }
  // The walk asks per op of a group: true = left out (the `none` arm), false = it goes through as the stream has it (counted as re-issued with its unit).
  bool OwnGroupOp(uint32_t op_index, bool is_draw, bool is_copy) {
    const uint8_t g = og_plan_.group_of_op[op_index];
    if (!og_plan_.taken[g]) return false;
    if (own_group::g_none[g].load(std::memory_order_relaxed)) {
      ++og_.ops_left_out[g];
      og_.draws_left_out[g] += is_draw ? 1 : 0;
      og_.copies_left_out[g] += is_copy ? 1 : 0;
      return true;
    }
    ++og_.reissued[g];
    return false;
  }
 public:
  std::string OwnGroupCensus() const { return own_group::CensusLine(og_); }
  const own_group::Counters& own_group_counters() const { return og_; }
 private:
  bool og_on_ = false;
  own_group::Plan og_plan_;
  own_group::Counters og_;
  own_group::FactsBuilder og_builder_;

  // S101 lane 35: the crowd hook call, out of BindDraw's frame (its two 8 KB constant blocks would give every draw a 16 KB stack frame, switch off or on).
  template <class Source>
  [[gnu::noinline]] uint32_t InjectCrowd(const LoadedFrame& frame, const Draw& draw, rdr::FrameBuilder& builder, int vs, int ps, const rdr::DrawState& state, const rdr::DynamicState& dynamic,
                                         const rdr::TextureSet& textures, const BoolLoop& bools, const Source& capture, bool cards = false) {
    uint8_t vs_block[8192], fs_block[8192];
    CrowdInject in;
    in.frame = &frame;
    in.draw = &draw;
    in.draw_index = uint32_t(&draw - &frame.draws[0]);
    in.builder = &builder;
    in.vs = vs, in.ps = ps;
    in.state = state;
    in.dynamic = dynamic;
    in.textures = textures;
    in.images = HookImages();  // S101 lane 190
    in.image_count = uint32_t(image_ids_.size());
    in.vs_block_bytes = PackBlock(rdr::fx::kVariants[vs], frame.constant_banks.Ref(draw.vs_bank), bools, vs_block);
    in.vs_block = vs_block;
    static const rdr::fx::Variant no_fragment_block{};
    in.fs_block_bytes = PackBlock(ps >= 0 ? rdr::fx::kVariants[ps] : no_fragment_block, frame.constant_banks.Ref(draw.ps_bank), bools, fs_block);
    in.fs_block = fs_block;
    in.density = g_crowd_density;
    in.rank = &CrowdRank;
    in.read_geometry = [&capture](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) { return capture.ReadGeometry(offset, bytes, out); };
    in.alpha_test = frame.fixeds[draw.fixed].alpha_test != 0;   // S102 T6: the members' instanced twin builds its program (vertex twin + the dropped draw's pixel stage) the way the cards do
    in.program = [this](int pvs, int pps, bool alpha) { return Program(pvs, pps, alpha); };
    if (cards) {  // S101 lane CD: the far cards' own injection draws with its own vertex program and the dropped draw's pixel stage
      in.alpha_test = frame.fixeds[draw.fixed].alpha_test != 0;
      in.program = [this](int pvs, int pps, bool alpha) { return Program(pvs, pps, alpha); };
      const uint32_t cards_drawn = crowd_hook_->InjectCards(in);
      card_result_ = in.card_result;   // lane 175
      return cards_drawn;
    }
    const uint32_t members_drawn = crowd_hook_->Inject(in);
    member_result_ = in.member_result;   // lane 272
    return members_drawn;
  }
