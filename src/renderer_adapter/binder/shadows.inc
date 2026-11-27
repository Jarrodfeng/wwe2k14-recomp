 private:

  rdr::Renderer& renderer_;
  std::vector<rdr::ImageId> image_ids_;
  std::vector<uint32_t> image_formats_;
  // ---- S101 lane 123 (SH2, docs\renderer\SHADOWS_OURS.md): our shadow passes ----------------------------------------------------------------------------
  struct ShadowOurs {
    rdr::ImageId image = rdr::kNoImage;
    shadows_ours::Chain chain = shadows_ours::kNone;
    shadows_ours::Factor f;
    uint32_t w = 0, h = 0;
  };
  std::unordered_map<uint32_t, ShadowOurs> ours_;      // game image id -> our image (sticky: made when first classified, kept until the game releases the id)
  std::unordered_map<uint32_t, uint32_t> rec_index_;   // game image id -> index into synced_records_
  shadows_ours::Plan shadows_plan_;
  shadows_ours::Census shadows_census_;
  shadows_ours::Factor cur_factor_;
  bool shadows_active_ = false, cur_ours_ = false;
  uint32_t cur_ours_w_ = 0, cur_ours_h_ = 0, shadows_window_ = 0;
  struct CleanCopy {
    uint32_t src, dst;
    uint8_t smip, slayer, dmip, dlayer;
  };
  std::vector<CleanCopy> copy_clean_;                 // whole copies made since the source was last drawn / cleared / overwritten (A9: with mip and layer)
  shadows_ours::Chain cur_chain_ = shadows_ours::kNone;
  bool unnamed_said_ = false;

  const ShadowOurs* ShadowOursImage(uint32_t id) const {
    if (ours_.empty()) return nullptr;
    const auto found = ours_.find(id);
    return found == ours_.end() ? nullptr : &found->second;
  }
  void DropShadowsOurs(uint32_t id) {
    const auto found = ours_.find(id);
    if (found == ours_.end()) return;
    renderer_.DestroyImage(found->second.image);
    ours_.erase(found);
  }
  shadows_ours::ImageInfo ShadowInfo(uint32_t id) const {
    const auto found = rec_index_.find(id);
    if (found == rec_index_.end() || found->second >= synced_records_.size()) return {};
    const ImageRecord& r = synced_records_[found->second];
    return {r.width, r.height, r.format, r.role};
  }
  void EnsureShadowOurs(uint32_t id, shadows_ours::Chain chain) {
    if (ours_.count(id)) return;
    const auto found = rec_index_.find(id);
    if (found == rec_index_.end() || found->second >= synced_records_.size() || id >= image_ids_.size() || image_ids_[id] == rdr::kNoImage) {
      ++shadows_census_.refused;
      return;
    }
    const ImageRecord& record = synced_records_[found->second];
    ShadowOurs o;
    o.chain = chain;
    o.f = shadows_ours::FactorOf(chain);
    o.w = shadows_ours::Scaled(record.width, o.f.x);
    o.h = shadows_ours::Scaled(record.height, o.f.y);
    rdr::ImageDesc desc;
    desc.width = uint16_t(o.w), desc.height = uint16_t(o.h), desc.depth = record.depth, desc.mips = record.mips, desc.layers = record.layers;
    desc.format = rdr::Format(record.format);
    desc.kind = rdr::ImageKind(record.kind);
    desc.usage = uint8_t(record.usage & ~rdr::kUsageScaled);  // a shadow image is sized by the setting, never by the render scale
    o.image = renderer_.CreateImage(desc);
    if (o.image == rdr::kNoImage) {
      ++shadows_census_.refused;
      return;
    }
    renderer_.SetImageClass(o.image, chain == shadows_ours::kMap ? rdr::kGpuShadow : rdr::kGpuPlane);  // S102 S0-3
    ours_[id] = o;
    ++shadows_census_.images_made;
    ++(chain == shadows_ours::kMap ? shadows_census_.targets_map : shadows_census_.targets_plane);
  }
  void ShadowsOursBegin(const LoadedFrame& frame, const BindStats& stats) {
    shadows_active_ = g_shadows_ours && !velocity_mode_;  // (A2: isolate frames draw into ours too, scaled: Map() remaps the same ids)
    if (!g_shadows_ours && !ours_.empty()) {  // the switch went off: the game's own images again
      for (auto& item : ours_) renderer_.DestroyImage(item.second.image);
      ours_.clear();
    }
    cur_factor_ = {};
    cur_ours_ = false;
    copy_clean_.clear();
    if (!shadows_active_) return;
    const auto t0 = std::chrono::steady_clock::now();
    { plan_memo::Timer tm_(plan_memo::kTmShadowScan); shadows_plan_.Scan(frame, [this](uint32_t id) { return ShadowInfo(id); }); }
    if (!shadows_plan_.named) {
      ++shadows_census_.frames_unnamed;
      if (!unnamed_said_ && shadows_census_.frames_unnamed >= 600) {
        unnamed_said_ = true;
        rdr::Log("[rdr][shadows_ours] switch on but the frames carry no draw origins (names off): nothing is classified, the stream's shadow maps are drawn");
      }
    } else {
      for (const auto& item : shadows_plan_.chain) EnsureShadowOurs(item.first, shadows_ours::Chain(item.second));
      if (!shadows_plan_.chain.empty()) ++shadows_census_.frames;
    }
    shadows_census_.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  void ShadowsSetTargets(const rdr::TargetsOp& targets) {
    const ShadowOurs* o = ShadowOursImage(targets.color_count ? targets.color[0].image : targets.depth.image);
    cur_ours_ = o != nullptr;
    cur_chain_ = o ? o->chain : shadows_ours::kNone;
    cur_factor_ = o ? o->f : shadows_ours::Factor{};
    cur_ours_w_ = o ? o->w : 0, cur_ours_h_ = o ? o->h : 0;
  }
  void ShadowsNoteWrite() {  // a draw or a clear into the bound colour target: a whole copy made from it is stale now
    if (copy_clean_.empty() || shadows_ours::g_fault == shadows_ours::Fault::DupNoInvalidate) return;
    for (size_t i = 0; i < copy_clean_.size();) {
      if (copy_clean_[i].src == current_color_) copy_clean_[i] = copy_clean_.back(), copy_clean_.pop_back();
      else ++i;
    }
  }
  void ShadowsClear(rdr::ClearOp& clear) {
    ShadowsNoteWrite();
    ++shadows_census_.clears;
    if (clear.full) return;
    clear.rect = shadows_ours::ScaleRect(clear.rect, cur_factor_);
    if (clear.rect.x <= 0 && clear.rect.y <= 0 && clear.rect.width >= cur_ours_w_ && clear.rect.height >= cur_ours_h_) clear.full = 1;
  }
  // false: the copy is dropped. Copies between two of our images are whole (no partial rectangles); a second one from the same unchanged source to the same destination is a duplicate.
  bool ShadowsCopy(rdr::CopyOp& copy) {
    const ShadowOurs* s = ShadowOursImage(copy.source.image);
    const ShadowOurs* d = ShadowOursImage(copy.dest.image);
    if (!s && !d) return true;
    if (s && d) {
      const CleanCopy key = {copy.source.image, copy.dest.image, copy.source.mip, copy.source.layer, copy.dest.mip, copy.dest.layer};
      for (const CleanCopy& k : copy_clean_)
        if (k.src == key.src && k.dst == key.dst && k.smip == key.smip && k.slayer == key.slayer && k.dmip == key.dmip && k.dlayer == key.dlayer) {
          ++shadows_census_.copies_dropped;
          return false;
        }
      const ShadowsInfoCopy whole = {ShadowInfo(copy.source.image), ShadowInfo(copy.dest.image)};
      if (whole.src.width != whole.dst.width || whole.src.height != whole.dst.height) {  // A9: a whole copy only between records of one size (else the rectangles are scaled, below)
        copy.source_rect = shadows_ours::ScaleRect(copy.source_rect, s->f);
        copy.dest_rect = shadows_ours::ScaleRect(copy.dest_rect, d->f);
        return true;
      }
      if (copy.source_rect.x != 0 || copy.source_rect.y != 0 || copy.source_rect.width < whole.src.width || copy.source_rect.height < whole.src.height) ++shadows_census_.copies_partial_made_whole;
      copy.source_rect = {0, 0, s->w, s->h};
      copy.dest_rect = {0, 0, d->w, d->h};
      for (size_t i = 0; i < copy_clean_.size();) {  // A9: whole copies FROM the destination are stale now
        if (copy_clean_[i].src == key.dst) copy_clean_[i] = copy_clean_.back(), copy_clean_.pop_back();
        else ++i;
      }
      copy_clean_.push_back(key);
      ++shadows_census_.copies_whole;
      return true;
    }
    if (s) copy.source_rect = shadows_ours::ScaleRect(copy.source_rect, s->f);
    if (d) copy.dest_rect = shadows_ours::ScaleRect(copy.dest_rect, d->f);
    return true;
  }
  struct ShadowsInfoCopy { shadows_ours::ImageInfo src, dst; };
  const DrawDynamic& ShadowsDynamic(const LoadedFrame& frame, const Draw& draw, const DrawDynamic& in, DrawDynamic& scratch) {
    if (pb_dyn_on_) {   // S102 L2: a pass of the plane bake: its own target's size, the game's clip space to the target with the y of the stream's plane passes (ndc +1 up = the top row)
      scratch = in;
      scratch.host.viewport[0] = 0, scratch.host.viewport[1] = 0, scratch.host.viewport[2] = float(pb_dyn_size_), scratch.host.viewport[3] = float(pb_dyn_size_);
      scratch.host.scissor = {0, 0, pb_dyn_size_, pb_dyn_size_};
      scratch.ndc_scale[0] = 1.0f, scratch.ndc_scale[1] = -1.0f, scratch.ndc_offset[0] = 0.0f, scratch.ndc_offset[1] = 0.0f;
      if (pb_dyn_fullscreen_) scratch.ndc_scale[2] = 1.0f, scratch.ndc_offset[2] = 0.0f, scratch.host.viewport[4] = 0.0f, scratch.host.viewport[5] = 1.0f;
      return scratch;
    }
    if (!shadows_active_ || velocity_mode_ || !cur_ours_) return in;
    ++shadows_census_.draws_retargeted;
    ShadowsNoteWrite();
    using shadows_ours::Fault;
    const Fault fault = shadows_ours::g_fault;
    if (fault == Fault::NoViewportScale || (fault == Fault::PlaneUnscaledChain && cur_chain_ == shadows_ours::kPlane)) return in;  // plants
    if (cur_factor_.unit() && fault != Fault::HalfTexelRescaled) return in;
    scratch = in;
    shadows_ours::ScaleDynamic(scratch, cur_factor_);
    if (fault == Fault::HalfTexelRescaled && cur_chain_ == shadows_ours::kMap && cur_ours_w_ > shadows_ours::kStreamMapSize) {  // plant: the map slides by the half texel the patch would add
      const uint32_t index = uint32_t(&draw - &frame.draws[0]);
      const DrawOrigin* o = frame.OriginOf(index);
      if (o && (o->pass_site == shadow_roles::kMapPassSite || o->call_site == shadow_roles::kCircleCallSite))
        scratch.host.viewport[0] += 0.5f * float(cur_ours_w_) / float(shadows_ours::kStreamMapSize) - 0.5f;
    }
    return scratch;
  }
  void ShadowsReader(const TextureSlot& slot, rdr::TextureSlot& out) {
    if (!ShadowOursImage(slot.image)) return;
    ++shadows_census_.readers;
    if (shadows_ours::g_fault == shadows_ours::Fault::CopySwap) std::memset(out.swizzle, 0, sizeof(out.swizzle));  // plant: the copy's red / blue swap is forgotten
    if (shadows_ours::g_fault == shadows_ours::Fault::FloorReadsWrestlerMap && slot.slot == 12 && !shadows_plan_.dests_16f.empty() &&
        std::find(shadows_plan_.dests_rgba8.begin(), shadows_plan_.dests_rgba8.end(), slot.image) != shadows_plan_.dests_rgba8.end())
      out.image = Map(shadows_plan_.dests_16f.front());  // plant: the floor reads a wrestler map
  }
  void ShadowsBlurPlant(const LoadedFrame& frame, const Draw& draw, rdr::TextureSet& textures) {  // plant: the chain draws (not the casters) lose their first texture
    const uint32_t index = uint32_t(&draw - &frame.draws[0]);
    const DrawOrigin* o = frame.OriginOf(index);
    if (shadow_roles::IsCaster(o, draw.ps_hash, draw.vs_patched_hash, ImageRole(current_target_))) return;
    textures.slots[0].image = rdr::kNoImage;
  }

  std::vector<ImageRecord> synced_records_;  // the records the renderer images were made from (a changed one is made again)
