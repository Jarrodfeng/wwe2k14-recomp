  // S102 K3 review F1 / F7: called by the live loop for EVERY handled frame (chain or not). The first chain frame after a frame without the chains is the RESUME frame: every chain core refuses to publish a mark whose
  // last proof compare is older than it (the chains did not watch the frames between: a template could have changed unseen). D3ChainsProven: every running chain's gate has proven (the warm-up's end).
  void D3NoteChain(bool chain, uint64_t fno) {
    if (!chain) { d3_chain_gap_ = true; return; }
    if (!d3_chain_gap_) return;
    d3_chain_gap_ = false;
    d3_.resume_frame = fno;
    if (cc_) cc_->d3c_.resume_frame = fno;
    if (rc_) rc_->d3r_.resume_frame = fno;
  }
  bool D3ChainsProven() const {
    const int64_t now = d3::NowMs();
    bool ok = true;
    if (d3::g_arena_d3 && d3_.gate) ok = ok && d3_.gate->Proven(now);
    if (d3::g_caster_d3 && cc_ && cc_->d3c_.gate) ok = ok && cc_->d3c_.gate->Proven(now);
    if (d3::g_ring_d3 && rc_ && rc_->d3r_.gate) ok = ok && rc_->d3r_.gate->Proven(now);
    return ok;
  }
  bool d3_chain_gap_ = false;
  bool d3_chain_frame = true;   // S102 K3 rework: the frame is bound with the D3 chains running (message.d3_chain, decided at submit); false = the armed-off frame: no Begin / End / marker handling for the arena, caster and ring chains. Offline tools leave it true
  // Lane 307: a skipped frame's lasting bind that kept NONE of the frame's object passes (lasting_prune.h Stats::d3_blind): the thin / D3 per-frame begin and end do not run (exactly a skipped frame with no bind: an empty object list would look like "every object left" to the guards and book clean frames on no evidence). Its D3 packets were discarded (D3FrameSkipped).
  bool d3_skip_observe = false;
  // S102 K2 (R16 piece C, wwe2k14_rdr_lasting_nogroup): the D3 chains count their call ordinals (marker checks, keyed runs, the call-count guards) over the WHOLE frame's ops, not the ops this bind carries. A lasting bind that left
  // object passes out (no model-part group rule) would otherwise number the kept runs from the first kept one: the markers' ordinals, the hook's calls = runs and the per-object guards all disagree. Set by the caller around
  // Bind (live.cpp, fview, rplay): `ord_full_ops` = the frame's ops before pruning, `ord_keep` = per op of those, 1 = the bind carries it. Null = off (the ops of this bind are the ordinal view, the shipped behaviour).
  // ord_plant (TEST ONLY, fview D3_NOGROUP_PLANT): 1 = the view is the bound ops (an ordinal taken from a bound pass), 2 = the view lacks the pass of one dropped object draw, 3 = Observe skips every run.
  const Table<Op>* ord_full_ops = nullptr;
  const std::vector<uint8_t>* ord_keep = nullptr;
  int ord_plant = 0;
  uint64_t ord_view_frames = 0, ord_view_dropped_runs = 0;   // census: lasting binds whose scans ran over the full view, and the keyed runs they did not learn

  template <class Source>
  void CreateImages(const Source& capture) { SyncImages(capture); }

  // Creates the renderer images for records the source added since the last call (ids are never reused; a live game keeps adding them).
  template <class Source>
  void SyncImages(const Source& capture) {
    const auto& records = capture.images();
    for (const ImageRecord& record : records) {  // S101 lane 59: the stage view's per-image facts (role, game size), read by id at the draws and the present
      if (!record.id || record.id > records.size()) continue;  // bounded like image_ids_ below (an id past the table is skipped there too)
      if (stage_images_.size() <= record.id) stage_images_.resize(record.id + 1);
      stage_images_[record.id] = {record.width, record.height, stage_view::IsSceneTarget(record.role, record.role_flags)};
      if (image_roles_.size() <= record.id) image_roles_.resize(record.id + 1, 0);  // S101 lane 86
      image_roles_[record.id] = record.role;
    }
    if (image_ids_.size() < records.size() + 1) {
      image_ids_.resize(records.size() + 1, rdr::kNoImage);
      image_formats_.resize(records.size() + 1, 0);
    }
    for (size_t r = 0; r < records.size(); ++r) {
      const ImageRecord& record = records[r];
      if (!record.id || record.id >= image_ids_.size()) continue;
      if (r < synced_records_.size() && std::memcmp(&synced_records_[r], &record, sizeof(ImageRecord)) == 0) continue;  // unchanged
      DropShadowsOurs(record.id);  // lane 123: a changed record: our shadow image is made again when next needed
      tex_alpha_.Erase(record.id);  // S102 C4 (review F2 / F12): a changed record (format, size, usage, flags) invalidates the scan
      if (image_ids_[record.id] != rdr::kNoImage) renderer_.DestroyImage(image_ids_[record.id]);  // changed (usage gained later): made again
      rdr::ImageDesc desc;
      desc.width = record.width;
      desc.height = record.height;
      desc.depth = record.depth;
      desc.mips = record.mips;
      desc.layers = record.layers;
      desc.format = rdr::Format(record.format);
      desc.kind = rdr::ImageKind(record.kind);
      desc.usage = record.usage;
      pl_target_ids_.erase(record.id);
      if (PreLightCubeShape(record) && rdr_sw::BoolOr(rdr_sw::kPrelightLayered, prelight_layered::g_fview.load(std::memory_order_relaxed)) && renderer_.multiview_supported() && renderer_.ColourTargetable(desc.format)) {   // S102 C1: a light cube the pre-light faces are drawn into needs target usage (made so only while the switch is on at the time the image is made)
        desc.usage = uint8_t(desc.usage | rdr::kUsageTarget);
        pl_target_ids_.insert(record.id);
      }
      pc_storage_ids_.erase(record.id);
      if (PreLightCubeShape(record) && (rdr_sw::BoolOr(rdr_sw::kPrelightCompute, prelight_compute::g_fview.load(std::memory_order_relaxed)) || prelight_compute::g_storage_always.load(std::memory_order_relaxed)) && renderer_.compute_supported() && renderer_.StorageTargetable(desc.format)) {   // S102 L1b: a light cube the compute dispatch writes needs storage usage (made so only while the switch is on at the time the image is made; the planner counts the rest)
        desc.usage = uint8_t(desc.usage | rdr::kUsageStorage);
        pc_storage_ids_.insert(record.id);
      }
      dr_target_ids_.erase(record.id);
      if (copy_alias::g_direct > 0 && DirectShape(record) && renderer_.ColourTargetable(desc.format)) {   // S102 C5: a small copy destination a direct pass renders into needs target usage (made so only while the switch is on at the time the image is made)
        desc.usage = uint8_t(desc.usage | rdr::kUsageTarget);
        dr_target_ids_.insert(record.id);
      }
      image_ids_[record.id] = renderer_.CreateImage(desc);
      if (gpu_class_set_.size() <= record.id) gpu_class_set_.resize(record.id + 1, 0xFF);
      gpu_class_set_[record.id] = 0xFF;  // S102 G6: a made-again image has class other until the refresh below names it
      image_formats_[record.id] = uint32_t(record.format);
    }
    RefreshGpuClasses(records);  // S102 G6 (was S0-3's per-image SetImageClass in the loop above)
    synced_records_.assign(records.begin(), records.end());
    rec_index_.clear();  // lane 123: id -> index into synced_records_
    for (size_t r = 0; r < synced_records_.size(); ++r) rec_index_[synced_records_[r].id] = uint32_t(r);
  }

  // The game released the object behind this id: its renderer image goes (the renderer defers the destroy past frames in flight).
  void ReleaseImage(uint32_t id) {
    DropShadowsOurs(id);  // lane 123
    tex_alpha_.Erase(id);  // S102 C4 (review F2): a released id's scan is gone (the id may name another image later)
    if (!id || id >= image_ids_.size() || image_ids_[id] == rdr::kNoImage) return;
    // S101 lane 1 (review of lane 19's last-picture path): the last present may be read many frames later now; never keep a released id
    // (renderer ids are reused after kDestroyDelayFrames, so a stale one could name another image).
    if (last_present_.source.image == image_ids_[id]) last_present_.source.image = rdr::kNoImage;
    renderer_.DestroyImage(image_ids_[id]);
    image_ids_[id] = rdr::kNoImage;
  }

  template <class Source>
  void ApplyContents(const LoadedFrame& frame, const Source& capture) {
    LoadedTexture texture;
    for (const ContentRef& ref : frame.contents) {
      if (ShadowOursImage(ref.image)) continue;  // lane 123: the shadow images are the GPU's (copies), never CPU contents; ours is another size
      if (!capture.ReadTexture(ref, texture)) continue;
      // S101 (case 7041): a content taken for an older record of an image recreated in another format would overrun the upload; skip it,
      // counted (the image's newer content follows). Also guards live play.
      if (ContentIsStale(image_formats_, ref.image, texture.header.format)) {  // S101 lane17: the rule lives in content_fit.h
        ++stale_contents_;
        continue;
      }
      // lane 195: every subresource must be one of the synced record's (mip, layer, extent), else the whole content is skipped and counted (guards
      // every path: an older-shape content shipped with a newer image table would copy past its staging buffer).
      if (const auto found = rec_index_.find(ref.image); found != rec_index_.end() && found->second < synced_records_.size()) {
        const ImageRecord& rec = synced_records_[found->second];
        bool fits = true;
        for (uint32_t s = 0; s < texture.header.subresource_count && fits; ++s) {
          const Subresource& sub = texture.subresources[s];
          fits = SubresourceFits(rec.width, rec.height, rec.mips, rec.layers, sub.mip, sub.layer, sub.width, sub.height);
        }
        if (!fits) {
          ++stale_contents_;
          ++stale_subres_;
          continue;
        }
      }
      for (uint32_t s = 0; s < texture.header.subresource_count; ++s) {
        const Subresource& sub = texture.subresources[s];
        renderer_.UpdateImage(Map(ref.image), sub.mip, sub.layer, texture.data + sub.offset, size_t(sub.row_bytes) * sub.rows);
      }
      ScanImageAlpha(ref.image, texture);  // S102 C4: scans while the switch is on, otherwise erases the entry (review F1)
    }
  }

  // S102 C4 (texture_alpha.h): the smallest alpha of an image's content, kept per game image id in tex_alpha_. Only while the arena overdraw switch is on (live.cpp sets g_arena_overdraw_scan before ApplyContents);
  // with it off a content ERASES the image's entry (review F1). Only candidates are read (review F6): file textures (kSourceTexture, a 2D image, no render target / copy destination) in a format the scan reads;
  // the scan stops at the first alpha under 255 and is timed (the census line). Takes effect from the next load of a texture (review F7): an image whose content arrived before the switch was turned on stays unknown
  // (its draws are refused, counted) until its content arrives again.
  void ScanImageAlpha(uint32_t image, const LoadedTexture& texture) {
    namespace ta = rdr::texture_alpha;
    const bool scan_on = rdr::colour_cuts::g_arena_overdraw_scan.load(std::memory_order_relaxed);
    const auto found = rec_index_.find(image);
    const bool have_rec = found != rec_index_.end() && found->second < synced_records_.size();
    const ImageRecord* rec = have_rec ? &synced_records_[found->second] : nullptr;
    const rdr::Format format = rdr::Format(texture.header.format);
    const bool candidate = rec && rec->source == kSourceTexture && rec->kind == uint8_t(rdr::ImageKind::Image2D) && ta::RecordMayBeProven(rec->usage, rec->flags) && ta::FormatScanned(format);
    if (!scan_on || !candidate) {
      tex_alpha_.OnContent(image, false, false, format, 0, 0, nullptr, 0);
      return;
    }
    std::vector<ta::SubIn> subs(texture.header.subresource_count);
    for (uint32_t s = 0; s < texture.header.subresource_count; ++s) {
      const Subresource& sub = texture.subresources[s];
      subs[s] = {sub.mip, sub.layer, texture.data + sub.offset, size_t(sub.row_bytes) * sub.rows};
    }
    ta::ScanStats stats;
    const auto t0 = std::chrono::steady_clock::now();
    tex_alpha_.OnContent(image, true, true, format, rec->mips, rec->layers, subs.data(), subs.size(), &stats);
    rdr::colour_cuts::g_scan_ns.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()), std::memory_order_relaxed);
    rdr::colour_cuts::g_scan_bytes.fetch_add(stats.bytes, std::memory_order_relaxed);
    rdr::colour_cuts::g_scan_images.fetch_add(stats.images, std::memory_order_relaxed);
    ++tex_alpha_scans_;
  }
  rdr::texture_alpha::AlphaTable tex_alpha_;  // S102 C4: game image id -> the scan (texture_alpha.h; erased on a record change, a release and a content that arrives with the scan off: review F1 / F2)
  uint64_t tex_alpha_scans_ = 0;

  // S103 lane S (FRAME_CONTRACT.md 6, REMNANT_SEAM.md 4): under OURS the next Bind's frame image (the source of the copy the present reads:
  // image 8 in brock_losswin) IS our frame for the whole frame: every use (targets, copies, readers' slots) maps to `ours`. kNoImage = off.
  // Found per frame from the ops (never a fixed id). One Bind only: Bind clears it at its end.
  void SetSceneFrame(rdr::ImageId ours, rdr::ImageId ours_depth = rdr::kNoImage) { scene_frame_image_ = ours; scene_frame_depth_ = ours_depth; }
  // S103 L2a (LANDING2_PLAN.md P1, dev seam behind --wwe2k14_rdr_2d_ours): the next Bind leaves out the stream's op-19 draws from the ROW sites (quads2d_core.h kOp19Sites, class Row): the caller drew this
  // frame's 2D from our rows. One Bind only (cleared with the scene frame). Every other remnant draw (clears, copies, non-row 2D) stays.
  // S103 L2a review F1: `insert` (when given) is called ONCE, at the first skipped row-site draw of the Bind, with the builder: it appends our rows there, so every stream draw before it lies under our rows and every
  // one after it over them (the stream's own order; a frame with a kept draw BETWEEN row-site draws is refused before this, q2d::ScanStream). The targets the stream had are re-issued after it.
  void SetSkip2DRows(bool on, std::function<void(rdr::FrameBuilder&)> insert = {}) { skip_2d_rows_ = on; row2d_insert_ = on ? std::move(insert) : nullptr; row2d_inserted_ = false; }
  // S103 L2a review F2 / pairing: the frame's ops as the pure scan reads them (quads2d_core.h StreamOp): draws (op-19 site, the colour target at the draw, the blend class of the draw's fixed state) and copies, in order.
  static void ListStreamOps(const LoadedFrame& frame, std::vector<rdr_scene::q2d::StreamOp>& out) {
    out.clear();
    uint32_t target = 0;
    for (uint32_t i = 0; i < frame.ops.count; ++i) {
      const Op& op = frame.ops[i];
      if (op.kind == OpKind::SetTargets) {
        const rdr::TargetsOp& t = frame.targets[op.index];
        target = t.color_count ? t.color[0].image : 0;
      } else if (op.kind == OpKind::Draw) {
        rdr_scene::q2d::StreamOp o;
        o.kind = 0;
        if (const DrawOrigin* d = frame.OriginOf(op.index); d && d->op == 19) o.op19 = true, o.site = d->call_site;
        o.target = target;
        {   // S103 SC review: the stream draw's window scissor (the dev cross-check's truth)
          const rdr::Rect& sc = frame.dynamics[frame.draws[op.index].dynamic].host.scissor;
          o.scissor[0] = sc.x, o.scissor[1] = sc.y, o.scissor[2] = int32_t(sc.width), o.scissor[3] = int32_t(sc.height);
        }
        const auto& b = frame.fixeds[frame.draws[op.index].fixed].state.blend[0];
        o.blend = rdr_scene::q2d::BlendClass(b.enable, b.src_color, b.dst_color, b.color_op);
        out.push_back(o);
      } else if (op.kind == OpKind::Copy) {
        rdr_scene::q2d::StreamOp o;
        o.kind = 1;
        out.push_back(o);
      }
    }
  }
  uint64_t skip_2d_draws() const { return skip_2d_draws_; }
  static bool HasOrigins(const LoadedFrame& frame) { return frame.origins.count != 0 && frame.origins.count >= frame.draws.count; }
  uint32_t scene_frame_id() const { return scene_frame_id_; }
  uint64_t scene_frame_unfound() const { return scene_frame_unfound_; }
  uint8_t scene_bind_state() const { return scene_bind_state_; }   // S103 CLK2 (review M1): this Bind's scene frame: 0 = none set, 1 = placed (our picture is the frame), 2 = set but the frame image was not found
  static uint32_t FindFrameImage(const LoadedFrame& frame) {   // the source of the last copy whose destination the frame's present reads
    uint32_t presented = 0;
    for (uint32_t i = frame.ops.count; i-- > 0;)
      if (frame.ops[i].kind == OpKind::Present) { presented = frame.presents[frame.ops[i].index].source.image; break; }
    if (!presented) return 0;
    for (uint32_t i = frame.ops.count; i-- > 0;)
      if (frame.ops[i].kind == OpKind::Copy && frame.copies[frame.ops[i].index].dest.image == presented) return frame.copies[frame.ops[i].index].source.image;
    return presented;   // no copy: the present reads the frame image itself
  }
