
  // ---- lane 122: D1 thin draws (thin_draw.h, thin_store.h, docs\renderer\THIN_DRAWS.md) --------------------------------------------------------------------------------------
  static bool* ThinOursFlag(thin::Cls c) { return c == thin::Cls::Arena ? &g_arena_ours : c == thin::Cls::Ring ? &g_ring_ours : &g_wrestlers_ours; }  // lane 129 (KC)
  static thin::Cls ThinClsOf(const bool* enabled, bool& ok) {  // the thin class of a hook slot's switch (ok false: a class without thin draws)
    ok = enabled == &g_arena_ours || enabled == &g_ring_ours || enabled == &g_wrestlers_ours;
    return enabled == &g_ring_ours ? thin::Cls::Ring : enabled == &g_wrestlers_ours ? thin::Cls::Wrestlers : thin::Cls::Arena;
  }
  bool ThinClassActive(thin::Cls c) const { return thin::ClsOn(c) && *ThinOursFlag(c); }
  bool ThinAnyActive() const { return ThinClassActive(thin::Cls::Arena) || ThinClassActive(thin::Cls::Ring) || ThinClassActive(thin::Cls::Wrestlers); }
  ReplaceSlot* ThinHookSlot(thin::Cls c = thin::Cls::Arena) {  // the class's replace slot
    if (c == thin::Cls::Casters) return nullptr;   // lane 202: the Casters class has no thin draws and no slot (ThinOursFlag would answer the wrestlers' slot for it)
    for (ReplaceSlot& s : replace_slots_)
      if (s.hook && s.enabled == ThinOursFlag(c) && *s.enabled) return &s;
    return nullptr;
  }
  std::string ThinCensusLine(thin::Cls c) const {  // lane 129 (KC): the class's counters with the shared hole / epoch counts and this class's pinned count
    thin::ThinCounters k = thin_counters_[uint8_t(c)];
    k.holes = thin_hole_c_[uint8_t(c)] + thin_hole_c_[3], k.epoch_drops = thin_epoch_drop_c_[uint8_t(c)] + thin_epoch_drop_c_[3], k.not_learned = thin_not_learned_;   // (review F2: per class; [3] = a key never learned)
    k.pinned = thin_store_.PinnedCount(c);
    char k4[256];   // S102 K4 (review live check b): pinned keys drawn lately but not proved for 4 periods, and which parity the bound guest frames had (a lock to one parity shows here). Census only.
    std::snprintf(k4, sizeof k4, "\n[rdr][thin] %s proofs: keys not proved within %u frames while drawn %llu, bound guest frames even %llu / odd %llu", thin::ClsName(c), unsigned(thin::kProofOverdue),
                  (unsigned long long)thin_store_.OverdueKeys(c, thin_frame_no_, thin::kProofOverdue), (unsigned long long)thin_bound_parity_[0], (unsigned long long)thin_bound_parity_[1]);
    return thin::CensusLine(c, k) + k4;
  }
  // Once per bound frame, class on: the frame's keys, the hook's frame state, the asset epoch (an arena change voids every template).
  // The facts of draw i: the reader's table when the frame has one, else derived from the draw itself (a capture without the table; fview's emulation fills the table first).
  ThinDrawFacts ThinFactsOf(const LoadedFrame& frame, uint32_t i) const {
    if (frame.thin_facts.count == frame.draws.count) return frame.thin_facts[i];
    const Draw& d = frame.draws[i];
    ThinDrawFacts f;
    f.index_addr = d.index_offset, f.index_count = d.index_count;
    f.vb_addr = uint32_t(d.vertex_offset ^ (d.vertex_offset >> 32)), f.vb_dw1 = d.vertex_bytes;   // review F4: the vertex identity (a content key in a capture: it moves when the bytes do)
    for (uint32_t t = 0; t < d.texture_count && d.texture_first + t < frame.texture_slots.count; ++t) {
      const TextureSlot& s = frame.texture_slots[d.texture_first + t];
      if (s.slot == 0) f.img0 = s.image;
      else if (s.slot == 4) f.img4 = s.image;
      else if (s.slot == 5) f.img5 = s.image;
    }
    return f;
  }
  uint64_t ThinProgramOf(const LoadedFrame& frame, uint32_t i) const {
    if (thin_fault_ == thin::Fault::NoProgramKey) return 0;  // planted: the first run's bug (a key without the program)
    return thin::ProgramKey(frame.draws[i].vs_hash, frame.draws[i].ps_hash, ThinFactsOf(frame, i));
  }
  // `read_only` (an isolate bind): keys and Find only; no hook call, no learn, no unpin, no publish.
  [[gnu::noinline]] void ThinBeginFrame(const LoadedFrame& frame, bool read_only = false) {
    thin_frame_on_ = true;
    thin_read_only_ = read_only;
    if (!read_only)
      for (uint8_t c = 0; c < uint8_t(thin::Cls::Count); ++c) thin_counters_[c].frames += ThinClassActive(thin::Cls(c));  // lane 129 (KC): per class
    thin_frame_no_ = frame.header.guest_frame;
    if (!read_only) ++thin_bound_parity_[frame.header.guest_frame & 1];   // S102 K4: the parity histogram of the bound guest frames (a session that binds one parity only is visible in the census)
    thin_stamp_ = frame.thin.count ? frame.thin[0].generation : 0;
    thin_store_.BeginFrame(thin_frame_no_, thin_stamp_);
    const std::unordered_map<uint32_t, thin::ThinKey>* d3_cut = nullptr;   // lane 258: the draws the (offline) D3 emulation cut still count in the ordinals of their triple (built only when there are any: the default path allocates nothing)
    const std::unordered_map<uint32_t, const std::vector<uint64_t>*>* d3_fill = nullptr;   // lane 289: the LIVE form: the part triples of each ring marker's skipped call, counted at the marker's draw
    const bool ring_skip = rc_ && d3::g_ring_d3 && rc_->d3r_.ring() && !rc_->d3r_.verify;   // the skip arm: the cut draws are absent from the stream (the verify arm drops nothing)
    const bool ring_cut = rc_ && d3::g_ring_d3 && !rc_->d3r_.reserved.empty() && rc_->d3r_.ordinal_fill_mode != 0;   // lane 289: in the LIVE form (mode 0) the ring's cut draws are filled from the marker instead (an oracle list exists only with D3_FILL_CHECK, used below)
    const bool arena_cut = d3::g_arena_d3 && !d3_.reserved.empty();   // lane 274 (S2): the arena's emulated skips reserve their cut draws' ordinals too (the ring models split in its scan)
    if (ring_cut || arena_cut) {
      d3_cut_all_.clear();
      d3_cut = &d3_cut_all_;
      const auto add = [&](const std::vector<std::pair<uint32_t, DrawOrigin>>& list) {
        for (const auto& rv : list) {
          const DrawOrigin& o = rv.second;
          if (o.klass == DrawClass::ModelPart && o.object && (o.pass_site == thin::kSiteColour || o.pass_site == thin::kSiteMirror)) d3_cut_all_[rv.first] = thin::ThinKey{o.pass_site, o.object, o.part, 0, 0};
        }
      };
      if (ring_cut) add(rc_->d3r_.reserved);
      if (arena_cut) add(d3_.reserved);
    }
    if (ring_skip && rc_->d3r_.ordinal_fill_mode == 0) {   // lane 289: SKIP arm. The marker carries the generation of the mark set the hook read (index_offset); the set holds the sequence's part triples (D3Core::Publish), the reader counts the same list (reader.h TryD3Marker)
      rc_->d3r_fill_.clear();
      rc_->d3r_fill_sets_.clear();
      for (uint32_t i = 0; i < frame.draws.count; ++i) {
        const DrawOrigin* o = frame.OriginOf(i);
        if (!o || !d3::IsRingMarkerCall(o->call_site)) continue;
        const std::shared_ptr<const d3::MarkSet> ms = rc_->d3r_.marks->Find(uint32_t(frame.draws[i].index_offset));
        const std::vector<uint64_t>* parts = ms ? ms->PartsOf(d3::MarkKey(o->object, o->call_site & 0xFFu)) : nullptr;
        if (parts) {
          rc_->d3r_fill_[i] = parts;
          rc_->d3r_fill_sets_.push_back(ms);
          if (!read_only) ++rc_->d3r_.fills;
        } else if (!read_only) {
          ++rc_->d3r_.fill_missing, ++rc_->d3r_.fill_missing_frame;
        }
      }
      if (!rc_->d3r_fill_.empty()) d3_fill = &rc_->d3r_fill_;
    }
    thin::ComputeKeys(frame.draws.count, [&](uint32_t i, uint32_t& site, uint32_t& object, uint32_t& part, uint64_t& program) {
      const DrawOrigin* o = frame.OriginOf(i);
      if (!o || o->klass != DrawClass::ModelPart || !o->object || (o->pass_site != thin::kSiteColour && o->pass_site != thin::kSiteMirror)) return false;
      site = o->pass_site, object = o->object, part = o->part, program = ThinProgramOf(frame, i);
      return true;
    }, thin_keys_, &thin_full_, thin_fault_ == thin::Fault::NoOrdinal, d3_cut, d3_fill);
    if (ring_skip && rc_->d3r_.fill_check && rc_->d3r_.ordinal_fill_mode == 0 && !rc_->d3r_.reserved.empty() && !read_only) {   // lane 289 (offline check): the oracle (the cut draws counted in place) must give the same keys as the live-form fill
      std::vector<uint64_t> oracle;
      std::unordered_map<uint32_t, thin::ThinKey> oracle_cut;   // the ring's cut draws counted in place (+ the arena's, as the main call has them)
      for (const auto* list : {&rc_->d3r_.reserved, &d3_.reserved})
        for (const auto& rv : *list) {
          const DrawOrigin& o = rv.second;
          if (o.klass == DrawClass::ModelPart && o.object && (o.pass_site == thin::kSiteColour || o.pass_site == thin::kSiteMirror)) oracle_cut[rv.first] = thin::ThinKey{o.pass_site, o.object, o.part, 0, 0};
        }
      thin::ComputeKeys(frame.draws.count, [&](uint32_t i, uint32_t& site, uint32_t& object, uint32_t& part, uint64_t& program) {
        const DrawOrigin* o = frame.OriginOf(i);
        if (!o || o->klass != DrawClass::ModelPart || !o->object || (o->pass_site != thin::kSiteColour && o->pass_site != thin::kSiteMirror)) return false;
        site = o->pass_site, object = o->object, part = o->part, program = ThinProgramOf(frame, i);
        return true;
      }, oracle, nullptr, false, &oracle_cut, nullptr);
      ++rc_->d3r_.fill_checked;
      if (oracle != thin_keys_) ++rc_->d3r_.fill_differ;
    }
    if (read_only) return;
    bool any_hook = false;
    for (uint8_t c = 0; c < uint8_t(thin::Cls::Count); ++c) {  // lane 129 (KC): every class that is on (the arena's body was this, once)
      if (!ThinClassActive(thin::Cls(c))) continue;
      ReplaceSlot* hs = ThinHookSlot(thin::Cls(c));
      if (!hs) continue;
      any_hook = true;
      if (hs->state == 0) {  // the hook's once-per-frame step must run although every draw may be lite (the lite draws never reach TryReplace)
        bool claims = false;
        for (uint32_t i = 0; i < thin_keys_.size() && !claims; ++i) {
          const DrawOrigin* o = thin_keys_[i] ? frame.OriginOf(i) : nullptr;
          claims = o && hs->hook->Claims(*o);
          if (!claims && c == uint8_t(thin::Cls::Wrestlers) && d3::g_wrestlers_d3 && i < frame.draws.count && frame.OriginOf(i) && d3::IsWrestlerMarkerCall(frame.OriginOf(i)->call_site)) claims = true;   // lane 227: a frame whose wrestler draws are all markers still gets its once-per-frame step (skeletons, idle count, sweeps)
        }
        if (claims) {
          hs->state = hs->hook->FrameReady(frame) ? 1 : 2;
          ++(persist_only_bind ? hs->counters.persist_only : hs->state == 1 ? hs->counters.ours_frames : hs->counters.fallback_frames);
        }
      }
      if (!hs->state && c == uint8_t(thin::Cls::Wrestlers) && d3::g_wrestlers_d3) {   // lane 227: the frame's wrestler markers (thin_keys_ is 0 for a marker draw)
        bool marker = false;
        for (uint32_t i = 0; i < frame.draws.count && !marker; ++i) marker = frame.OriginOf(i) && d3::IsWrestlerMarkerCall(frame.OriginOf(i)->call_site);
        if (marker) {
          hs->state = hs->hook->FrameReady(frame) ? 1 : 2;
          ++(hs->state == 1 ? hs->counters.ours_frames : hs->counters.fallback_frames);
        }
      }
      hs->hook->WantJoinDrops(true);
      // review F2: ONE epoch per class. A class whose hook destroyed its assets loses its own templates; the other classes keep theirs.
      const uint32_t epoch = hs->hook->AssetEpoch();
      if (epoch != thin_epoch_c_[c] || thin_force_epoch_) {
        thin_epoch_c_[c] = epoch;
        if (thin_store_.ClearClass(thin::Cls(c))) thin_epoch_gen_c_[c] = thin_pub_->generation() + 1;  // review 4: frames in flight (this one included) that still flag lite draws predate the clear: epoch drops, not holes
      }
      // review F2: single buffers the hook destroyed (a wrestler part swept after 240 idle frames): only the templates naming them go.
      const uint32_t rel = hs->hook->BufferReleaseSeq();
      if (rel != thin_rel_seq_[c]) {
        thin_rel_seq_[c] = rel;
        std::vector<uint64_t> ids;
        hs->hook->TakeReleasedBuffers(ids);
        uint32_t n = 0;
        for (uint64_t id : ids) n += thin_store_.EraseByBuffer(id);
        if (n) thin_epoch_gen_c_[c] = thin_pub_->generation() + 1, thin_counters_[c].unpinned += n, thin_counters_[c].buffer_unpins += n;
      }
    }
    thin_force_epoch_ = false;
    (void)any_hook;
  }
  // A draw flagged kDrawThin: its template, or null (a hole, counted; or an epoch drop: the frame predates the clear).
  const thin::ThinTemplate* ThinFind(const LoadedFrame& frame, const Draw& draw) {
    const uint32_t i = uint32_t(&draw - &frame.draws[0]);
    const uint64_t key = thin_frame_on_ && i < thin_keys_.size() ? thin_keys_[i] : 0;
    const thin::ThinTemplate* t = key ? thin_store_.Find(key) : nullptr;
    if (d3_overlay_) {   // lane 138: a marker's synthesized part draws use the sequence's own copy of the template (a marker frame does not depend on the D1 store keeping it)
      const auto ov = d3_overlay_->find(key);
      if (ov != d3_overlay_->end()) t = ov->second;
    }
    if (!t) {   // review F2: attributed to the class the key was learned in ([3] = never learned)
      const auto kc = thin_key_cls_.find(key);
      const uint8_t c = kc != thin_key_cls_.end() ? kc->second : uint8_t(3);
      const uint32_t gen = c < 3 ? thin_epoch_gen_c_[c] : std::max(thin_epoch_gen_c_[0], std::max(thin_epoch_gen_c_[1], thin_epoch_gen_c_[2]));
      if (thin_offline_ && (frame.header.flags & kFrameHasThin)) {
        ++thin_not_learned_;
        if (const DrawOrigin* no = frame.OriginOf(i)) thin_not_learned_list_.push_back({no->object, no->part, no->pass_site});   // lane 214 (offline only): WHICH draws the census could not draw
      } else ++(thin_stamp_ < gen ? thin_epoch_drop_c_[c] : thin_hole_c_[c]);
    }
    return t;
  }
  // Fills replace_out_ and the packed block from the template (the stream draw's own bank), as TryReplace would; null on a slot the draw lacks.
  [[gnu::noinline]] ReplaceSlot* ThinApply(const LoadedFrame& frame, const Draw& draw, const thin::ThinTemplate& tpl, int vs, const rdr::TextureSet& textures, const BoolLoop& bools) {
    const auto t0 = std::chrono::steady_clock::now();
    thin::ThinCounters& tc = thin_counters_[uint8_t(tpl.cls)];  // lane 129 (KC)
    replace_block_bytes_ = PackBlock(rdr::fx::kVariants[vs], frame.constant_banks.Ref(draw.vs_bank), bools, replace_block_);
    ReplaceOut& out = replace_out_;
    out = ReplaceOut{};
    out.vertex = tpl.vertex, out.index = tpl.index, out.index_count = tpl.index_count, out.index_32bit = tpl.index_32bit, out.vertex_layout = tpl.layout, out.topology = tpl.topology;
    for (int s = 0; s < rdr::kMaxTextures; ++s) {
      if (!tpl.slots[s].set) continue;
      if (wanted_[s] != stamp_) continue;  // a slot this program does not read
      if (textures.slots[s].image == rdr::kNoImage) {
        ++tc.slot_missing;  // the stream binds nothing there: the draw goes on without our image (counted, required 0)
        continue;
      }
      out.slots[s].set = true, out.slots[s].image = tpl.slots[s].image, out.slots[s].view_format = tpl.slots[s].view_format;
      std::memcpy(out.slots[s].swizzle, tpl.slots[s].swizzle, sizeof(out.slots[s].swizzle));
    }
    // lane 129 (KC): the wrestlers' palette rows are the STREAM's own bank (the hook overrode them only with rows it had checked bit-equal): nothing is applied. The plant keeps the learned rows.
    if (tpl.plant_rows && thin_fault_ == thin::Fault::TemplateRows)
      ApplyRowOverride(rdr::fx::kVariants[vs].constants, rdr::fx::kVariants[vs].constant_count, replace_block_, replace_block_bytes_, tpl.plant_first_register, tpl.plant_rows, tpl.plant_row_data.data());
    ++tc.thin;
    tc.ms_thin += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return ThinHookSlot(tpl.cls);
  }
  // After a P2a replacement or refusal of a keyed draw (BindDraw, once BlendRanges has run): learn the key, or prove it, or unpin it.
  [[gnu::noinline]] void ThinAfter(const LoadedFrame& frame, const Draw& draw, ReplaceSlot* replaced_by, int vs, int ps, const rdr::TextureSet& textures, const rdr::DrawState& state) {
    const uint32_t i = uint32_t(&draw - &frame.draws[0]);
    const uint64_t key = i < thin_keys_.size() ? thin_keys_[i] : 0;
    if (!key || !thin_frame_on_ || thin_read_only_) return;
    ThinPollDrops();
    if (!replaced_by) {  // claimed and refused (or not claimed): a pinned key whose full-path check failed leaves the set
      if (draw_trace_.keep_why == uint8_t(KeepWhy::NotReady) + 1) return;  // review 8: a hook that was not ready is no evidence about the key
      if (thin_fault_ == thin::Fault::ClothPinned && ThinClassActive(thin::Cls::Wrestlers) && !thin_store_.Known(key)) {
        // lane 129 PLANT cloth_pinned: a draw of a wrestler object the hook KEPT (cloth, a refused palette) is pinned with a template borrowed from a part of the same object
        const auto last = thin_wrestler_last_.find(thin_full_[i].object);
        if (last != thin_wrestler_last_.end() && thin_store_.Learn(thin_full_[i], last->second)) ++thin_counters_[uint8_t(thin::Cls::Wrestlers)].learned;
        return;
      }
      const thin::ThinStore::Entry* en = thin_store_.Pinned(key);
      const uint8_t ec = en ? uint8_t(en->t.cls) : uint8_t(thin::Cls::Arena);
      if (thin_store_.Unpin(key)) ++thin_counters_[ec].unpinned;
      return;
    }
    bool known_cls = false;
    const thin::Cls cls = ThinClsOf(replaced_by->enabled, known_cls);  // lane 129 (KC): the hook that replaced the draw says the class; a class with thin off learns nothing
    if (!known_cls || !ThinClassActive(cls)) {  // review 1: another hook (or a class with thin off) replaced the draw of a key a class pinned (a reused address): the template is stale
      const thin::ThinStore::Entry* en = thin_store_.Pinned(key);
      if (thin_fault_ != thin::Fault::ForeignNoUnpin && en && thin_store_.Unpin(key)) ++thin_counters_[uint8_t(en->t.cls)].unpinned;
      return;
    }
    thin::ThinCounters& tc = thin_counters_[uint8_t(cls)];
    const auto t0 = std::chrono::steady_clock::now();
    thin::ThinTemplate now;
    now.cls = cls;
    now.vs = vs, now.ps = ps;
    now.layout = replace_out_.vertex_layout, now.topology = replace_out_.topology, now.index_32bit = replace_out_.index_32bit;
    now.vertex = replace_out_.vertex, now.index = replace_out_.index, now.index_count = replace_out_.index_count;
    for (int s = 0; s < rdr::kMaxTextures; ++s) {
      if (!replace_out_.slots[s].set || wanted_[s] != stamp_) continue;
      thin::ThinSlot& t = now.slots[s];
      t.set = true, t.image = replace_out_.slots[s].image, t.view_format = replace_out_.slots[s].view_format;
      std::memcpy(t.swizzle, replace_out_.slots[s].swizzle, sizeof(t.swizzle));
      for (uint32_t k = 0; k < draw.texture_count; ++k) {  // the STREAM slot's level window (a stream fact the proof compares)
        const TextureSlot& ts = frame.texture_slots[draw.texture_first + k];
        if (ts.slot == s) t.min_level = ts.min_level, t.max_level = ts.max_level;
      }
    }
    now.stream_vertex_count = draw.vertex_count, now.stream_index_count = draw.index_count;
    now.blend_base = thin_blend_base_;
    (void)state;
    // lane 129 (KC): what is STORED. The honest template is `now`; the plants change the stored copy only (the proof compares the stored one with the honest bind, and a re-template stores the plant again).
    thin::ThinTemplate stored = now;
    if (thin_fault_ == thin::Fault::TemplateRows && cls == thin::Cls::Wrestlers && replace_out_.rows) {  // R: Brock's displaced arm class (WRESTLER map 204): rows from the template, not the bank
      stored.plant_first_register = replace_out_.first_register, stored.plant_rows = replace_out_.rows;
      stored.plant_row_data.assign(replace_out_.row_data, replace_out_.row_data + size_t(replace_out_.rows) * 4);
    }
    if (thin_fault_ == thin::Fault::RopeColourVisible && cls == thin::Cls::Ring && !stored.slots[0].set && wanted_[0] == stamp_ && textures.slots[0].image != rdr::kNoImage) {
      // R (in spirit, VISIBLE): the rope's slot 0 takes another ring part's stream texture (the file's `_red` rope on a blue ring): the picture shows it
      const uint64_t triple = thin_full_[i].Triple();
      thin_rope0_.emplace(triple, textures.slots[0].image);
      for (const auto& kv : thin_rope0_)
        if (kv.second != textures.slots[0].image) {
          thin::ThinSlot& t = stored.slots[0];
          t.set = true, t.image = kv.second, t.view_format = textures.slots[0].view_format;
          std::memcpy(t.swizzle, textures.slots[0].swizzle, sizeof(t.swizzle));
          break;
        }
    }
    if (thin_fault_ == thin::Fault::RopeColourInstance0 && cls == thin::Cls::Ring && !stored.slots[0].set && wanted_[0] == stamp_ && textures.slots[0].image != rdr::kNoImage) {
      // R: ring.md B2.3, the rope colour is chosen per instance at run time (the hook leaves slot 0 the stream's): every instance of a part takes instance 0's image
      const uint64_t triple = thin_full_[i].Triple();
      auto it = thin_rope0_.find(triple);
      if (it == thin_rope0_.end()) it = thin_rope0_.emplace(triple, textures.slots[0].image).first;
      thin::ThinSlot& t = stored.slots[0];
      t.set = true, t.image = it->second, t.view_format = textures.slots[0].view_format;
      std::memcpy(t.swizzle, textures.slots[0].swizzle, sizeof(t.swizzle));
    }
    if (thin_fault_ == thin::Fault::WrongLayout) {   // lane 162 PLANT: the stored template names another template's vertex layout (the proof compare must see it; a wrong layout is never what a draw is meant to use)
      if (thin_plant_layout_ == 0xFFFFu) thin_plant_layout_ = now.layout;
      else if (now.layout != thin_plant_layout_) stored.layout = thin_plant_layout_;
    }
    if (const thin::ThinStore::Entry* pinned = thin_store_.Pinned(key)) {
      ++tc.proof;
      thin_store_.Proved(key);   // S102 K4
      uint32_t mask = thin::Compare(pinned->t, now);
      if (pinned->t.cls != cls) mask |= 1u << uint32_t(thin::Differ::Program);  // the key's draw is now another class's
      const uint32_t hard = mask & ~(1u << uint32_t(thin::Differ::Blend));
      if (hard) {
        if (tc.proof_mismatch < 12)  // the first few, by name: what a stale template would have drawn
          rdr::Log("[rdr][thin] %s proof mismatch frame %llu key site %08X object %08X part %u ordinal %u: program %s/%s -> %s/%s, layout %u -> %u, mask %x; geometry (lane 220) vertex buffer %llu+%llu -> %llu+%llu, index buffer %llu+%llu -> %llu+%llu, index count %u -> %u",
                   thin::ClsName(cls), (unsigned long long)thin_frame_no_, thin_full_[i].pass_site, thin_full_[i].object, thin_full_[i].part, thin_full_[i].ordinal,
                   pinned->t.vs >= 0 ? rdr::fx::kVariants[pinned->t.vs].name : "-", pinned->t.ps >= 0 ? rdr::fx::kVariants[pinned->t.ps].name : "-",
                   vs >= 0 ? rdr::fx::kVariants[vs].name : "-", ps >= 0 ? rdr::fx::kVariants[ps].name : "-", pinned->t.layout, now.layout, hard,
                   (unsigned long long)pinned->t.vertex.buffer, (unsigned long long)pinned->t.vertex.offset, (unsigned long long)now.vertex.buffer, (unsigned long long)now.vertex.offset,
                   (unsigned long long)pinned->t.index.buffer, (unsigned long long)pinned->t.index.offset, (unsigned long long)now.index.buffer, (unsigned long long)now.index.offset, pinned->t.index_count, now.index_count);
        ++tc.proof_mismatch;
        for (uint32_t d = 0; d < uint32_t(thin::Differ::Count); ++d)
          if (hard & (1u << d)) ++tc.differ[d];
      } else {
        ++tc.proof_ok;
      }
      if (mask & (1u << uint32_t(thin::Differ::Blend))) ++tc.blend_moved;
      if (mask) thin_store_.Retemplate(thin_full_[i], stored);
    } else if (thin_store_.Learn(thin_full_[i], stored)) {
      ++tc.learned;
      thin_key_cls_[key] = uint8_t(cls);
    } else {
      ++tc.full_other;
    }
    if (cls == thin::Cls::Wrestlers && thin_fault_ == thin::Fault::ClothPinned) thin_wrestler_last_[thin_full_[i].object] = now;
    tc.ms_proof += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  // Review 2: objects whose join the arena dropped lose all their pinned keys, in the same frame.
  void ThinPollDrops() {
    for (uint8_t c = 0; c < uint8_t(thin::Cls::Count); ++c) {  // lane 129 (KC): per class (the arena's, the ring's and the wrestlers' hooks each keep a drop sequence)
      ReplaceSlot* hs = ThinHookSlot(thin::Cls(c));
      if (!hs) continue;
      const uint32_t seq = hs->hook->JoinDropSeq();
      if (seq == thin_drop_seq_[c]) continue;
      thin_drop_seq_[c] = seq;
      std::vector<uint32_t> objects;
      hs->hook->TakeJoinDrops(objects);
      for (uint32_t o : objects) {
        const uint32_t n = thin_store_.UnpinObject(o);
        thin_counters_[c].unpinned += n, thin_counters_[c].object_unpins += n;
      }
    }
  }
