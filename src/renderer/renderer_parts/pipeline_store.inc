
// ---- lane 276: the pipeline store (pipeline_store.h; SKIP_GAME_WORK_MAP 19.10) ----

void Renderer::Impl::StoreInit(const RendererOptions& opts, std::vector<uint8_t>& cache_blob) {
  if (opts.pipeline_store_dir.empty()) return;
  store_dir = pstore::PathOf(opts.pipeline_store_dir);
  std::error_code ec;
  std::filesystem::create_directories(store_dir, ec);
  if (ec) {
    Log("[rdr][pipestore] off: cannot make the folder %s", opts.pipeline_store_dir.c_str());
    return;
  }
  const VkPhysicalDeviceProperties& props = device.properties();
  store_device.vendor = props.vendorID, store_device.device = props.deviceID, store_device.driver = props.driverVersion;
  std::memcpy(store_device.uuid, props.pipelineCacheUUID, 16);
  store_on = true;
  store_slice_us = opts.pipeline_prebuild_slice_us;
  store_caps_mask = pstore::LimitsWord(opts.format_caps_mask, device.caps().dyn_mask);   // S111 DYNSTATE: the list's records are keys of this dynamic-state set
  store_save_interval_ms = opts.pipeline_save_interval_ms;
  // render-clock: ctl - save-interval stamp for the shader store file, no picture value
  store_saved_at = std::chrono::steady_clock::now();
  uint32_t list_session = 0;
  std::vector<uint8_t> file;
  if (pstore::ReadFile(store_dir / "pipelines.bin", file, 64ull << 20)) {
    uint32_t dropped = 0;
    bool reshaped = false;   // S111 DYNSTATE review: a list of a smaller dynamic-state set (an older build's full states) is read as this device's keys and written again
    const std::string why = pstore::ParseList(file, store_records, list_session, store_device, store_caps_mask, dropped, opts.pipeline_store_plant == 1, &reshaped);
    if (!why.empty()) {
      Log("[rdr][pipestore] list refused: %s (the pipelines are built on demand; a new list is written)", why.c_str());
      store_records.clear();
      list_session = 0;
      store_list_dirty = true;
    } else {
      for (uint32_t i = 0; i < store_records.size(); ++i) {
        if (!store_index.emplace(pstore::RecordKey(store_records[i]), i).second) store_records[i].dead = true;  // a duplicate: dropped
        else store_pending.push_back(i);
      }
      store_stats.list_loaded = store_pending.size();
      store_stats.bad_dropped = dropped;
      if (dropped || reshaped) store_list_dirty = true;
      if (reshaped) Log("[rdr][pipestore] list made under a smaller dynamic-state set: its records are now this device's keys (records that became one key are kept once); it is written again");
      Log("[rdr][pipestore] list loaded: %zu pipelines, session %u%s", store_pending.size(), list_session + 1, dropped ? " (some records failed the range checks and were dropped)" : "");
    }
  } else {
    Log("[rdr][pipestore] no list yet (first session on this store)");
  }
  if (!opts.pipeline_seed_path.empty()) {   // S106 PSEED: the shipped seed (R lines of shader_list.txt): records the player's list lacks join the pending ones
    std::vector<uint8_t> seed_file;
    if (pstore::ReadFile(pstore::PathOf(opts.pipeline_seed_path), seed_file, 64ull << 20)) {
      std::vector<pstore::Record> seed;
      // S106 PSEED fix B1: the seed's depth slot is a token, swapped here for THIS device's depth format; a record whose colour format the device cannot render to is dropped
      const pstore::SeedParse sp = pstore::ParseSeedLines(std::string(seed_file.begin(), seed_file.end()), seed, device.caps().multiview, uint16_t(device.caps().depth_format),
                                                          [this](uint16_t f) { return f != 0 && device.FormatHas(VkFormat(f), VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT); });
      for (pstore::Record& sr : seed) sr.state = dyn::KeyState(sr.state, device.caps().dyn_mask);   // S111 DYNSTATE: the seed holds full states (it serves every device); here they become this device's keys, and records that differ only in per-draw state merge into one
      const pstore::SeedMerge sm = pstore::MergeSeed(store_records, store_index, store_pending, seed);
      store_seed_added = sm.added;
      Log("[rdr][pipestore] seed: %u records, %u new, %u already in the list, %u refused%s%s, %u multiview skipped, %u colour format unsupported", sp.lines, sm.added, sm.known, sp.refused, sp.refused ? " (" : "", sp.refused ? (sp.first_refusal + ")").c_str() : "", sp.multiview, sp.unsupported);
    } else {
      Log("[rdr][pipestore] seed: %s not readable (no seed)", opts.pipeline_seed_path.c_str());
    }
  }
  store_session = list_session + 1;
  if (pstore::ReadFile(store_dir / "pipeline_cache.bin", file, pstore::kMaxCacheBytes + 64)) {
    const uint8_t* blob = nullptr;
    size_t bytes = 0;
    const std::string why = pstore::ParseCache(file, store_device, blob, bytes, opts.pipeline_store_plant == 2);
    if (!why.empty()) {
      store_cache_refusal = why;
      Log("[rdr][pipestore] cache refused: %s (starting empty)", why.c_str());
      store_cache_dirty = true;
    } else {
      cache_blob = std::move(file);  // (the file is not held twice: the wrapper header is cut off in place)
      cache_blob.erase(cache_blob.begin(), cache_blob.begin() + (cache_blob.size() - bytes));
      store_stats.cache_loaded_bytes = bytes;
      Log("[rdr][pipestore] cache loaded: %zu bytes (device %04X:%04X driver %08X)", bytes, store_device.vendor, store_device.device, store_device.driver);
    }
  } else {
    Log("[rdr][pipestore] no cache yet");
  }
  StorePublish();  // S105 B3: the list's records are counted from the start (a precompile never reads "0 of 0" with records waiting)
}

bool Renderer::Impl::StoreFindProgram(const ProgramDesc& desc, uint16_t& id) {
  hashing_program.vhash = pstore::HashWords(desc.vertex_spirv, desc.vertex_bytes);
  hashing_program.fhash = desc.fragment_spirv ? pstore::HashWords(desc.fragment_spirv, desc.fragment_bytes) : 0;
  auto found = program_by_hash.find(pstore::HashBytes(&hashing_program, sizeof(hashing_program)));
  if (found == program_by_hash.end()) return false;
  if (found->second >= program_info.size() || program_info[found->second].vhash != hashing_program.vhash || program_info[found->second].fhash != hashing_program.fhash) return false;  // a collision of the pair's hash: a new program
  id = found->second;
  return true;
}

void Renderer::Impl::StoreProgramAdded(uint16_t id) {
  if (program_info.size() <= id) program_info.resize(size_t(id) + 1);
  program_info[id] = hashing_program;
  program_by_hash.emplace(pstore::HashBytes(&hashing_program, sizeof(hashing_program)), id);
  store_retry = true;
}

void Renderer::Impl::StoreNoteBuilt(const DrawState& state, uint32_t pass_class, int64_t build_ns) {
  std::lock_guard<std::mutex> tables(tables_mutex);  // S102 R21-B (hazard 2): program_info is written by AddProgram on the bind stage
  ++store_stats.on_demand;
  store_stats.on_demand_ns += uint64_t(build_ns);
  ++store_used;
  const PassClassKey& ck = class_keys[pass_class];
  if (ck.pad != 0 || state.program >= program_info.size() || state.vertex_layout >= layouts.size()) return;  // the swapchain's own class is not kept
  pstore::Record r;
  r.state = state;
  r.state.program = 0;
  r.state.vertex_layout = 0;
  r.vhash = program_info[state.program].vhash;
  r.fhash = program_info[state.program].fhash;
  r.layout = pstore::Canonical(layouts[state.vertex_layout]);
  std::memcpy(r.pass.formats, ck.formats, sizeof(r.pass.formats));
  r.pass.color_count = ck.color_count, r.pass.has_depth = ck.has_depth, r.pass.samples = ck.samples, r.pass.pad = ck.views > 1 ? ck.views : 0;   // S102 C1: a multiview class keeps its view count in `pad` (pipeline_store.h Sane)
  r.seen = store_session;
  r.built = true;
  store_pub_dirty = true;   // S105 B3 F5: the counts changed (published at the next slice call)
  const char* name = programs[state.program].name;
  std::strncpy(r.name, name ? name : "", pstore::kNameBytes - 1);
  if (!pstore::Sane(r)) {  // would be dropped when loaded: not recorded, counted (the pipeline itself was built and works)
    if (++store_stats.bad_recorded == 1) Log("[rdr][pipestore] a pipeline failed the list's range checks and is not recorded (program %s)", r.name);
    return;
  }
  store_cache_dirty = true;
  const uint64_t key = pstore::RecordKey(r);
  auto found = store_index.find(key);
  if (found != store_index.end()) {  // the list already had it (its turn had not come): built now
    pstore::Record& old = store_records[found->second];
    old.built = true, old.seen = store_session;
    store_list_dirty = true;
    return;
  }
  if (store_records.size() >= pstore::kMaxRecords * 2 + store_seed_added) return;  // far past the cap: the save drops the oldest anyway
  store_index.emplace(key, uint32_t(store_records.size()));
  store_records.push_back(r);
  store_list_dirty = true;
}

// true: nothing more to do for this record (built, skipped or dropped). false: its program does not exist yet.
bool Renderer::Impl::PrebuildOne(uint32_t index) {
  pstore::Record& r = store_records[index];
  if (r.built || r.dead) return true;
  if (r.pass.pad > 1 && !device.caps().multiview) {   // S102 C1 review F9: a list from a device that had multiview: nothing to prebuild here (checked before any module is made)
    r.dead = true;
    return true;
  }
  uint16_t program = 0;
  ProgramInfo want{r.vhash, r.fhash};
  // S102 R21-B (hazard 2): the maps are read under tables_mutex, released before AddProgramChecked / AddVertexLayout (they take it) and the build.
  bool have_program = false;
  ShaderBlob vblob, fblob;
  {
    std::lock_guard<std::mutex> tables(tables_mutex);
    auto known = program_by_hash.find(pstore::HashBytes(&want, sizeof(want)));
    if (known != program_by_hash.end()) {
      program = known->second, have_program = true;
    } else {
      auto v = store_catalog.find(r.vhash);
      if (v == store_catalog.end()) return false;
      auto f = r.fhash ? store_catalog.find(r.fhash) : store_catalog.end();
      if (r.fhash && f == store_catalog.end()) return false;
      vblob = v->second;
      if (r.fhash) fblob = f->second;
    }
  }
  if (!have_program) {
    store_names.emplace_back(r.name);
    ProgramDesc desc;
    desc.name = store_names.back().c_str();
    desc.vertex_spirv = vblob.words, desc.vertex_bytes = vblob.bytes;
    if (r.fhash) desc.fragment_spirv = fblob.words, desc.fragment_bytes = fblob.bytes;
    if (!self->AddProgramChecked(desc, program)) {
      r.dead = true;
      ++store_stats.prebuilt_failed;
      return true;
    }
  }
  const uint16_t layout = self->AddVertexLayout(r.layout);
  PassClassKey ck{};
  std::memcpy(ck.formats, r.pass.formats, sizeof(ck.formats));
  ck.color_count = r.pass.color_count, ck.has_depth = r.pass.has_depth, ck.samples = r.pass.samples;
  ck.views = r.pass.pad;   // S102 C1
  const uint32_t pass_class = GetPassClass(ck);
  DrawState state = dyn::KeyState(r.state, device.caps().dyn_mask);   // S111 DYNSTATE: a record is already a key (StoreInit); made again here so every key of `pipelines` comes from one function
  state.program = program;
  state.vertex_layout = layout;
  PipelineKey key{};
  std::memcpy(&key.state, &state, sizeof(DrawState));
  key.pass_class = pass_class;
  if (pipelines.count(key)) {  // asked for on demand before its turn
    r.built = true;
    return true;
  }
  building_prebuilt = true;
  last_build_ns = 0;
  VkPipeline pipeline = BuildPipeline(state, pass_class);
  building_prebuilt = false;
  if (!pipeline) {
    r.dead = true;
    ++store_stats.prebuilt_failed;
    return true;
  }
  pipelines.emplace(key, PipelineEntry{pipeline, int32_t(index)});
  r.built = true;
  ++store_stats.prebuilt;
  store_stats.prebuilt_ns += last_build_ns;
  store_cache_dirty = true;
  return true;
}

void Renderer::Impl::PrebuildSlice(uint64_t budget_ns, bool all) {
  if (!store_on) return;
  bool retry = false;
  {  // S102 R21-B (hazard 2): store_retry is set by AddProgram / RegisterShaderCatalog on the bind stage
    std::lock_guard<std::mutex> tables(tables_mutex);
    retry = store_retry;
    store_retry = false;
  }
  if (retry && !store_unresolved.empty()) {  // a program or the catalog arrived since: the waiting records get another try
    store_pending.insert(store_pending.end(), store_unresolved.begin(), store_unresolved.end());
    store_unresolved.clear();
  }
  if (store_cursor >= store_pending.size()) {
    if (store_pub_dirty || !store_pub_valid.load(std::memory_order_relaxed)) StorePublish();   // S105 B3 F5: nothing pending: publish only when a count moved (no walk of every record per menu frame)
    return;
  }
  const uint64_t t0 = RenderNow();
  const uint64_t built_before = store_stats.prebuilt;
  while (store_cursor < store_pending.size()) {
    const uint32_t index = store_pending[store_cursor++];
    if (!PrebuildOne(index)) store_unresolved.push_back(index);
    if (!all && store_stats.prebuilt > built_before && RenderNow() - t0 >= budget_ns) break;
  }
  if (store_cursor >= store_pending.size()) {
    store_pending.clear();
    store_cursor = 0;
  }
  store_stats.unresolved_peak = std::max<uint64_t>(store_stats.unresolved_peak, store_unresolved.size());
  const uint64_t spent = RenderNow() - t0;
  ++store_stats.slices;
  store_stats.slice_ns += spent;
  store_stats.slice_max_ns = std::max(store_stats.slice_max_ns, spent);
  StorePublish();
}

// S105 B3: the counts the precompile reads (made / total / left that can be tried / unresolved), computed on the render thread, read by atomics anywhere.
void Renderer::Impl::StorePublish() {
  uint64_t total = 0, made = 0;
  for (const pstore::Record& r : store_records) {
    if (r.dead) continue;
    ++total;
    if (r.built) ++made;
  }
  store_pub_total.store(total, std::memory_order_relaxed);
  store_pub_made.store(made, std::memory_order_relaxed);
  store_pub_left.store(store_pending.size() - store_cursor, std::memory_order_relaxed);
  store_pub_unresolved.store(store_unresolved.size(), std::memory_order_relaxed);
  store_pub_dirty = false;
  store_pub_valid.store(true, std::memory_order_release);
}

// The render thread builds the records to keep and copies the driver blob; the serialise + checksum + write run on ONE worker (at most one in flight; a save asked
// while it runs waits for the next check). `final` (shutdown, the quit flush) joins the worker and writes on this thread.
void Renderer::Impl::StoreSave(bool final) {
  if (!store_on) return;
  if (final) StoreJoin();
  else if (store_worker_busy.load(std::memory_order_acquire)) return;
  if (store_redo_list.exchange(false)) store_list_dirty = true;   // a write that failed is tried again
  if (store_redo_cache.exchange(false)) store_cache_dirty = true;
  if (!store_list_dirty && !store_cache_dirty) return;
  const uint64_t t0 = RenderNow();
  store_saved_at = std::chrono::steady_clock::now();
  std::vector<pstore::Record> keep;
  uint32_t written_session = 0;
  const bool do_list = store_list_dirty;
  if (do_list) {
    const bool substantial = store_used >= 20;  // a launch that only reached the menus ages nothing
    written_session = substantial ? store_session : store_session - 1;
    keep.reserve(store_records.size());
    for (pstore::Record r : store_records) {
      if (!pstore::SaveKeeps(r)) continue;   // S106 PSEED: dead, or a seed record nobody drew
      if (r.seen > written_session) r.seen = written_session;
      if (written_session - r.seen > pstore::kKeepSessions) continue;
      keep.push_back(r);
    }
    if (keep.size() > pstore::kMaxRecords) {  // the oldest go first, the order of the rest is kept
      std::vector<uint32_t> seen;
      for (const pstore::Record& r : keep) seen.push_back(r.seen);
      std::sort(seen.begin(), seen.end());
      const uint32_t cut = seen[keep.size() - pstore::kMaxRecords];
      std::vector<pstore::Record> trimmed;
      for (const pstore::Record& r : keep)
        if (r.seen >= cut && trimmed.size() < pstore::kMaxRecords) trimmed.push_back(r);
      keep.swap(trimmed);
    }
    store_list_dirty = false;
  }
  std::vector<uint8_t> blob;
  bool do_cache = false;
  if (store_cache_dirty && pipeline_cache) {
    VkDevice dev = device.device();
    size_t size = 0;
    if (vkGetPipelineCacheData(dev, pipeline_cache, &size, nullptr) == VK_SUCCESS && size && size <= pstore::kMaxCacheBytes) {
      blob.resize(size);
      if (vkGetPipelineCacheData(dev, pipeline_cache, &size, blob.data()) == VK_SUCCESS && size <= blob.size()) {
        blob.resize(size);
        do_cache = true;
        store_cache_dirty = false;
      } else {
        blob.clear();
      }
    }
  }
  const double copy_ms = double(RenderNow() - t0) / 1e6;
  auto job = [this, keep = std::move(keep), blob = std::move(blob), do_list, do_cache, written_session, final, copy_ms]() {
    const uint64_t w0 = RenderNow();
    if (do_list) {
      std::string why;   // S105 B3: which step failed and the OS error
      if (pstore::WriteFileAtomicWhy(store_dir / "pipelines.bin", pstore::SerialiseList(keep, written_session, store_device, store_caps_mask), why)) {
        store_saved_list.store(keep.size(), std::memory_order_relaxed);
      } else {
        store_redo_list.store(true);
        Log("[rdr][pipestore] could not write the list in %s: %s", store_dir.string().c_str(), why.c_str());
      }
    }
    if (do_cache) {
      std::string why;
      if (pstore::WriteFileAtomicWhy(store_dir / "pipeline_cache.bin", pstore::SerialiseCache(store_device, blob.data(), blob.size()), why)) {
        store_saved_cache.store(blob.size(), std::memory_order_relaxed);
      } else {
        store_redo_cache.store(true);
        Log("[rdr][pipestore] could not write the cache in %s: %s", store_dir.string().c_str(), why.c_str());
      }
    }
    wwe2k14::perf_sys::NoteFileWrite(double(RenderNow() - w0) / 1e6);  // lane 302: the [perf][sys] line's file_write_ms
    Log("[rdr][pipestore] saved%s: list %llu pipelines (session %u), cache %llu bytes; render thread %.2f ms (the blob copy), %s %.1f ms", final ? " at the end" : "",
        static_cast<unsigned long long>(store_saved_list.load()), store_session, static_cast<unsigned long long>(store_saved_cache.load()), copy_ms,
        final ? "serialise + write" : "worker", double(RenderNow() - w0) / 1e6);
  };
  if (final) {
    job();
  } else {
    store_worker_busy.store(true, std::memory_order_release);
    store_worker = std::thread([this, job = std::move(job)]() { wwe2k14::NameThread(L"rdr_store");
      job();
      store_worker_busy.store(false, std::memory_order_release);
    });
  }
}

void Renderer::Impl::StoreMaybeSave() {
  if (!store_on) return;
  if (!store_worker_busy.load(std::memory_order_acquire)) StoreJoin();  // (a finished worker is joined here, never waited for)
  if (!store_list_dirty && !store_cache_dirty && !store_redo_list.load() && !store_redo_cache.load()) return;
  // render-clock: ctl - save-interval gate for the shader store file, no picture value
  if (std::chrono::steady_clock::now() - store_saved_at < std::chrono::milliseconds(store_save_interval_ms)) return;
  if (store_stats.prebuilt + store_stats.on_demand == 0 && !store_list_dirty) return;
  StoreSave(false);
}

void Renderer::Impl::StoreSummary(const char* when) {
  if (!store_on) return;
  const StoreStats& s = store_stats;
  Log("[rdr][pipestore] %s: on-demand builds %llu (%.1f ms), prebuilt from the list %llu (%.1f ms, %llu drawn this session, %llu refused), waiting %zu (%zu unresolved), list loaded %llu, cache loaded %llu bytes%s%s, slices %llu (max %.2f ms)",
      when, static_cast<unsigned long long>(s.on_demand), double(s.on_demand_ns) / 1e6, static_cast<unsigned long long>(s.prebuilt), double(s.prebuilt_ns) / 1e6,
      static_cast<unsigned long long>(s.prebuilt_used), static_cast<unsigned long long>(s.prebuilt_failed), store_pending.size() - store_cursor + store_unresolved.size(), store_unresolved.size(),
      static_cast<unsigned long long>(s.list_loaded), static_cast<unsigned long long>(s.cache_loaded_bytes), store_cache_refusal.empty() ? "" : ", cache refused: ", store_cache_refusal.c_str(),
      static_cast<unsigned long long>(s.slices), double(s.slice_max_ns) / 1e6);
}

void Renderer::RegisterShaderCatalog(const ShaderBlobDesc* blobs, size_t count) {
  Impl& im = *impl_;
  if (!im.store_on) return;
  std::lock_guard<std::mutex> tables(im.tables_mutex);  // S102 R21-B (hazard 2): PrebuildOne reads the catalog on the record stage
  const uint64_t hash_t0 = RenderNow();   // S105 B3 F6: the hash of every blob runs on the boot thread
  for (size_t i = 0; i < count; ++i) {
    if (!blobs[i].words || !blobs[i].bytes) continue;
    Impl::ShaderBlob blob;
    blob.words = blobs[i].words, blob.bytes = blobs[i].bytes, blob.name = blobs[i].name;
    const auto made = im.store_catalog.emplace(pstore::HashWords(blobs[i].words, blobs[i].bytes), blob);
    if (!made.second && (made.first->second.bytes != blob.bytes || std::memcmp(made.first->second.words, blob.words, blob.bytes) != 0))
      Log("[rdr][pipestore] two different shaders share a hash (%s): the second is not in the catalog", blob.name ? blob.name : "?");  // never seen
  }
  im.store_retry = true;
  Log("[rdr][pipestore] shader catalog: %zu shaders (hashed in %.1f ms)", im.store_catalog.size(), double(RenderNow() - hash_t0) / 1e6);
}

bool Renderer::pipeline_store_active() const { return impl_->store_on; }

void Renderer::SetPipelinePrebuildAllowed(bool allowed) { impl_->prebuild_allowed = allowed; }

// S102 R21-B (STAGE_SPLIT.md hazard 4): close what is pending into a batch; the frame handed over with this id applies exactly the batches up to it.
uint64_t Renderer::SealUploads() {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lock(im.upload_mutex);
  const uint64_t seal = ++im.next_seal;
  if (!im.new_images.empty() || !im.image_uploads.empty() || !im.buffer_uploads.empty() || !im.class_changes.empty()) {
    Impl::UploadBatch batch;
    batch.seal = seal;
    batch.fresh.swap(im.new_images);
    batch.images.swap(im.image_uploads);
    batch.buffers.swap(im.buffer_uploads);
    batch.classes.swap(im.class_changes);
    im.sealed_batches.push_back(std::move(batch));
  }
  return seal;
}
void Renderer::SetRenderThread() { impl_->render_thread.store(std::hash<std::thread::id>()(std::this_thread::get_id()), std::memory_order_relaxed); }  // S102 R21-B (review F1)
void Renderer::SetProducerThread() { if (!g_plant_no_producer.load(std::memory_order_relaxed)) impl_->producer_thread.store(std::hash<std::thread::id>()(std::this_thread::get_id()), std::memory_order_relaxed); }  // S102 R21-B (hazard 6)

// The live game hard-exits (~Renderer never runs): the quit flush saves what is dirty, waits for a save in flight and prints the summary. Render thread; repeatable.
void Renderer::FlushPipelineStore() {
  Impl& im = *impl_;
  if (!im.store_on) return;
  im.StoreSave(true);
  im.StoreJoin();
  im.StoreSummary("at quit");
}

size_t Renderer::PrebuildPipelines(uint32_t max_ms) {
  Impl& im = *impl_;
  if (!im.store_on) return 0;
  const uint64_t t0 = RenderNow();
  im.PrebuildSlice(uint64_t(max_ms) * 1000000ull, max_ms == 0);
  Log("[rdr][pipestore] prebuild pass: %.1f ms", double(RenderNow() - t0) / 1e6);
  return im.store_pending.size() - im.store_cursor + im.store_unresolved.size();
}

void Renderer::SavePipelineStore() {
  impl_->store_list_dirty = impl_->store_cache_dirty = true;
  impl_->StoreSave(false);
}

Renderer::PipelineStoreCounts Renderer::pipeline_store_counts() const {
  const Impl::StoreStats& s = impl_->store_stats;
  PipelineStoreCounts c;
  c.on_demand = s.on_demand, c.prebuilt = s.prebuilt, c.prebuilt_used = s.prebuilt_used, c.on_demand_ns = s.on_demand_ns, c.prebuilt_ns = s.prebuilt_ns;
  c.list_records = impl_->store_records.size(), c.list_loaded = s.list_loaded;
  return c;
}

// S105 B3: only the atomics StorePublish wrote: safe from any thread (the host's precompile rule); the other counts read render-thread tables.
Renderer::PipelineStoreCounts Renderer::published_store_counts() const {
  PipelineStoreCounts c;
  c.valid = impl_->store_pub_valid.load(std::memory_order_acquire);
  c.total = impl_->store_pub_total.load(std::memory_order_relaxed), c.made = impl_->store_pub_made.load(std::memory_order_relaxed);
  c.left_resolvable = impl_->store_pub_left.load(std::memory_order_relaxed), c.unresolved = impl_->store_pub_unresolved.load(std::memory_order_relaxed);
  return c;
}
