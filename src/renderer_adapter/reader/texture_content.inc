
// ---- reader workers (S100 speed: heavy reads off the game's render thread) --------------------------------------------------------
// EnsureContent decides on the game thread (cheap: copy destinations, once a frame, the write watch, the pages protected before any read)
// and hands the read itself (DecodeTexture: untile, endian, the 4444 rotate; and the hash) to a worker. JoinReaderWork, called when a frame
// or frame part is serialized (FrameBytes), waits for this frame's jobs and does what the game-thread path did after the read: compare
// the hash, hand the blob to the sink, append the ContentRef. A CPU write while a worker reads is caught by the watch next frame.
struct ReaderJob {
  virtual ~ReaderJob() = default;
  uint64_t seq = 0;  // S102 R25-A: the join ticket number of a guest-reading job, 0 for the others (set by Pool::Push, under its mutex)
  virtual void Run() = 0;  // on a worker: reads guest memory only through the job's own inputs; no reader tables, no SDK calls
  std::vector<NoteCount> notes;
};
struct TextureJob : ReaderJob {
  uint32_t id = 0;
  uint32_t gen = 0;
  uint32_t w[6] = {};
  std::vector<uint8_t> blob;
  uint64_t hash = 0;
  bool ok = false;
  void Run() override {
    ok = DecodeTexture(id, w, blob);
    if (ok) hash = cap::Xxh3(blob.data(), blob.size());
  }
};
struct ReaderWorkers : wwe2k14::join_ticket::Pool<ReaderJob> {  // S102 R25-A: the queue, WaitAll and the tickets are join_ticket.h's (the offline test runs the same code)
  int started = 0;  // detached: they wait on work_cv until the process ends (a joinable global std::thread would abort at exit)
  std::vector<std::unique_ptr<TextureJob>> frame_jobs;  // game thread: this frame's, in submission order

  bool Ensure() {
    const int want = std::clamp(REXCVAR_GET(wwe2k14_rdr_reader_workers), 0, 8);
    if (!want) return false;
    if (!started) {
      for (int i = 0; i < want; ++i) std::thread([this, i] { wwe2k14::NameThread(L"rdr_reader"); Loop(i); }).detach();
      started = want;
      REXLOG_INFO("[rdr][workers] {} reader worker thread(s) started (texture contents off the game's render thread)", want);
    }
    return true;
  }
  void Loop(int slot) {
    for (;;) {
      ReaderJob* job = Pop(slot);
      if (!job) return;
      t_note_sink = &job->notes;
      job->Run();
      t_note_sink = nullptr;
      Finish(slot);
    }
  }
  void Submit(uint32_t id, const uint32_t w[6]) {
    auto job = std::make_unique<TextureJob>();
    job->id = id;
    job->gen = ById(id).read_gen;  // lane 195: a record refresh bumps it; JoinTextureJobs drops a job of an older generation
    std::memcpy(job->w, w, sizeof(job->w));
    TextureJob* raw = job.get();
    frame_jobs.push_back(std::move(job));
    Queue(raw, true);  // a texture read: reads guest memory (S102 R25-A: only these take a fence's ticket)
  }
  void Queue(ReaderJob* job, bool guest_reads) {  // the caller keeps the job alive until WaitAll
    job->notes.clear();
    Push(job, guest_reads);
  }
};
inline ReaderWorkers g_workers;

// ---- strip-360 E1: where content changes come from (census, diagnosis only) ------------------------------------------------------
// For every texture read and every rebuild of a watched geometry entry: was it new, unchanged (the watch's 64 KB granule or an unchanged
// rewrite), changed after one of the game's own calls named it (an unlock flush, a movie plane flush), or changed with only the page watch
// to tell? The last kind is what the watches are still needed for: those resources are listed by size and address every 600 frames.
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_source_census);
struct SourceCensus {
  uint64_t tex_first = 0, tex_same = 0, tex_named = 0, tex_unnamed = 0;
  uint64_t geo_first = 0, geo_same = 0, geo_changed = 0, geo_rewritten = 0, geo_named = 0, geo_small_changed = 0, geo_small_same = 0;
  std::unordered_map<uint32_t, uint32_t> tex_unnamed_by_image;  // image -> changes with only the watch
  std::unordered_map<uint32_t, uint32_t> tex_addr;              // image -> base physical address (last read)
  struct GeoChange {
    uint32_t n = 0, bytes = 0;
    uint64_t vs = 0;
  };
  std::unordered_map<uint32_t, GeoChange> geo_changed_by_addr;  // vertex stream address -> changes, bytes, the drawing vertex shader
};
inline SourceCensus g_source;
inline bool SourceCensusOn() { return REXCVAR_GET(wwe2k14_rdr_debug_source_census) || REXCVAR_GET(wwe2k14_rdr_debug_notify_verify); }
inline void CensusTextureRead(uint32_t id, bool had_content, bool named, bool same) {
  SourceCensus& s = g_source;
  if (!had_content) ++s.tex_first;
  else if (same) ++s.tex_same;
  else if (named) ++s.tex_named;
  else {
    ++s.tex_unnamed;
    ++s.tex_unnamed_by_image[id];
  }
}
inline void CensusGeometryRebuild(uint64_t old_key, uint64_t new_key, uint32_t addr, uint32_t bytes, uint64_t vs, bool named) {
  SourceCensus& s = g_source;
  if (old_key == new_key) {
    ++s.geo_same;
    return;
  }
  if (named) {
    ++s.geo_named;
    return;
  }
  ++s.geo_changed;
  auto& e = s.geo_changed_by_addr[addr];
  ++e.n;
  e.bytes = bytes;
  e.vs = vs;
}
inline void CensusSourceLog(uint64_t from, uint64_t to);  // (below EnsureContent: needs the image registry)

// ---- strip-360 D8: the movie planes straight from the decoder's flush ---------------------------------------------------------------
// The game decodes Bink on the CPU into a decode set of 8-bit plane textures; each frame its movie present (0x827E0018) copies the newest
// frame into a display set and flushes that set's planes (0x827FB540, dcbst over each plane: docs\systems\video_tron.md "How the pieces
// connect" 5); renderers sample the display set. So a display plane changes exactly when that flush names it. A plane the flush has named
// becomes flush-driven: read after each flush, never by page watch (the watch's 64 KB granules made the planes re-read whenever the decoder
// wrote the neighbouring decode set). `--wwe2k14_rdr_debug_plane_verify` reads a flush-driven plane at its binds anyway and counts any
// change that arrived without a flush.
REXCVAR_DECLARE(bool, wwe2k14_rdr_planes_by_flush);
REXCVAR_DECLARE(bool, wwe2k14_rdr_debug_plane_verify);
REXCVAR_DECLARE(bool, wwe2k14_rdr_plane_repoint_legacy);
inline std::map<uint32_t, uint32_t> g_plane_by_base;  // physical base of a linear 8-bit texture -> image id (EnsureContent)

inline void VerifyPlane(uint32_t id, const uint32_t w[6]) {
  static thread_local std::vector<uint8_t> blob;
  blob.clear();
  if (!DecodeTexture(id, w, blob)) return;
  ++g_planes.verified;
  Image& img = ById(id);
  if (cap::Xxh3(blob.data(), blob.size()) == img.content_hash) return;
  const uint64_t n = ++g_planes.missed;
  if (n <= 8) REXLOG_WARN("[rdr][planes] miss #{}: image {} changed without a flush (guest frame {})", n, id, wwe2k14::g_guest_frame.load(std::memory_order_relaxed));
}

// 0x827FB540 (r3 = the display set), before the original.
inline void OnPlaneFlush(PPCContext& ctx, uint8_t* base) {
  std::lock_guard lock(g_mutex);
  Note("movie_plane_flush");
  if (!REXCVAR_GET(wwe2k14_rdr_planes_by_flush)) return;
  const uint32_t set = ctx.r3.u32;
  const uint32_t count = db::Ld32(base, set + 128);
  if (!count || count > 8) return;
  ++g_planes.flushes;
  for (uint32_t i = 0; i < count; ++i) {
    for (uint32_t k = 0; k < 4; ++k) {
      const uint32_t ptr = db::Ld32(base, set + 156 + 12 * k + 48 * i), size = db::Ld32(base, set + 32 + 4 * k + 64 * i);
      if (!ptr || !size) continue;
      // physical: the 0xE0000000 view is 4 KB higher (RememberHeaderWords); a page of slack below for views that do not apply it
      const uint32_t phys = wwe2k14::guest_view::ToPhysical(ptr);
      const bool named = cap::plane::NameByFlush(g_plane_by_base, phys, size, [](uint32_t id) {  // plane_repoint.h (unit-tested)
        Image& img = ById(id);
        if (!img.live) return false;
        img.flushed = true;
        img.flush_driven = true;
        return true;
      });
      if (named) ++g_planes.named;
      else ++g_planes.unmatched;
    }
  }
}

// S100 (L26 both ways; the select screen's renders, the title logo and the loading bar: garbage after a match, Jarrod, APKs 14 / 15): a copy
// destination's pages are watched from the copy on. The file loader writes textures without a flush (L8), so when the game loads pictures into
// memory a copy wrote earlier (the match's targets), only the watch sees it. A CPU write or flush after the copy = the CPU's bytes are newer:
// the image is a texture read from memory again until the next copy into it.
inline void WatchCopyDest(Image& img, const uint32_t w[6]) {
  uint32_t range[2][2];
  if (!REXCVAR_GET(wwe2k14_rdr_watch_texture_pages) || !TextureRanges(w, range)) {
    img.dest_watched = false;
    return;
  }
  uint32_t pages[2][2];
  for (uint32_t r = 0; r < 2; ++r) {
    pages[r][0] = range[r][0] >> 12;
    pages[r][1] = range[r][1] ? uint32_t((uint64_t(range[r][0]) + range[r][1] - 1) >> 12) : 0;
    if (!range[r][1]) pages[r][0] = 1;  // empty: first > last
  }
  const bool protect = PageWatchOn();
  if (protect && !g_write_watch) g_write_watch = REX_KERNEL_MEMORY()->RegisterPhysicalMemoryInvalidationCallback(&OnPhysicalWrite, nullptr);
  for (uint32_t r = 0; r < 2; ++r) {
    if (pages[r][0] > pages[r][1]) continue;
    ClearWritten(pages[r][0], pages[r][1]);
    if (protect) REX_KERNEL_MEMORY()->EnablePhysicalMemoryAccessCallbacks(range[r][0], range[r][1], true, false);
  }
  std::memcpy(img.dest_pages, pages, sizeof(pages));
  img.dest_watched = true;
  if (range[0][1]) g_copy_dest_by_page[range[0][0] >> 12] = uint32_t(&img - g_reg.images.data()) + 1;  // the coherence census (ById's id)
}
inline bool CopyDestCpuWritten(const Image& img) {
  if (!img.dest_watched) return false;
  for (uint32_t r = 0; r < 2; ++r) {
    if (img.dest_pages[r][0] <= img.dest_pages[r][1] && AnyWritten(img.dest_pages[r][0], img.dest_pages[r][1])) return true;
  }
  return false;
}

inline void EnsureContent(uint32_t id, const uint32_t w[6]) {
  if (!Recording()) return;
  ScopedNs timer(g_times.content);
  if (REXCVAR_GET(wwe2k14_rdr_debug_no_content)) return;  // diagnosis: no texture memory read at all
  Image& img = ById(id);
  if (img.copy_dest) {
    if (!img.flushed && !CopyDestCpuWritten(img)) {
      if (!img.written_in_capture) Note("copy_dest_sampled_before_written_in_capture");
      return;
    }
    // The CPU wrote it after the copy (L26): a texture from memory again.
    Note("copy_destination_rewritten_by_cpu");
    img.copy_dest = false;
    img.gpu_newer = false;
    img.dest_watched = false;
    img.content_written = false;
    img.content_hash = 0;
    img.watched = false;
    FillTextureRecord(img, w);
  }
  const uint64_t frame = g_record_frame;
  if (img.checked_frame == frame) return;  // read once per frame at most
  img.checked_frame = frame;
  const bool debug_full = REXCVAR_GET(wwe2k14_rdr_debug_full_texture_reads);
  // S100 speed: write watches when they can be set (the default); else the sample with a full read every kFullReadEvery frames.
  uint32_t range[2][2];
  const bool watch = REXCVAR_GET(wwe2k14_rdr_watch_texture_pages) && TextureRanges(w, range);
  // L8, L9 ("final at first bind") holds for the match loads it was measured on, not for the menus' picture caches: CSuperStarReader
  // (roster_images.md) keeps 20 rows over 426 slots and loads new renders into the same texture memory - bound once on stale bytes (memory
  // left from the match), then never read again: the select screen's garbage renders (Jarrod, APK 14, S100). With write watches the
  // linear statics are watched like every other texture (an unchanged one costs a page check); without them the old shortcut stays.
  if (!watch && img.linear_static && img.content_written && !img.flushed) return;
  // Strip-360 D8: remember where each linear 8-bit texture lives, so the movie decoder's flush can name it (OnPlaneFlush).
  if (watch && img.is_texture && BaseFormat(img.game_format) == 2 && range[0][1]) g_plane_by_base[range[0][0]] = id;
  if (SourceCensusOn()) g_source.tex_addr[id] = w[1] & 0xFFFFF000u;
  // lane 195 (GATE_FAULTS row 1, the title-logo glitch): flush_driven used to be sticky, so a plane object re-pointed at other memory / another
  // size (the next movie's planes, a reused object) served the old bytes until a flush named the new base. A moved binding is a new picture:
  // it drops its content and the flag, is read once and is then covered by the page watch until a flush names the new base (plane_repoint.h).
  // Census: flush_driven_plane_repointed (any move), flush_driven_plane_repointed_stale (lane 172's condition: the old code served stale bytes).
  if (img.flush_driven && REXCVAR_GET(wwe2k14_rdr_planes_by_flush)) {
    cap::plane::State ps{{img.plane_key[0], img.plane_key[1]}, img.content_written, img.flushed, img.flush_driven};
    const cap::plane::Result rr = cap::plane::Rebind(ps, w, REXCVAR_GET(wwe2k14_rdr_plane_repoint_legacy));
    if (rr.moved) Note("flush_driven_plane_repointed");
    if (rr.stale) Note("flush_driven_plane_repointed_stale");
    img.plane_key[0] = ps.key[0], img.plane_key[1] = ps.key[1], img.content_written = ps.content_written, img.flushed = ps.flushed;
    img.flush_driven = ps.flush_driven;
  }
  if (img.flush_driven && REXCVAR_GET(wwe2k14_rdr_planes_by_flush)) {
    // D8: a movie display plane changes only where the decoder's present copies a frame and flushes it; nothing else is read.
    if (img.content_written && !img.flushed && !debug_full) {
      if (REXCVAR_GET(wwe2k14_rdr_debug_plane_verify)) VerifyPlane(id, w);
      return;
    }
    img.watched = false;
  } else if (watch) {
    const bool protect = PageWatchOn();
    if (protect && !g_write_watch) g_write_watch = REX_KERNEL_MEMORY()->RegisterPhysicalMemoryInvalidationCallback(&OnPhysicalWrite, nullptr);
    uint32_t pages[2][2];
    for (uint32_t r = 0; r < 2; ++r) {
      pages[r][0] = range[r][0] >> 12;
      pages[r][1] = range[r][1] ? uint32_t((uint64_t(range[r][0]) + range[r][1] - 1) >> 12) : 0;
      if (!range[r][1]) pages[r][0] = 1;  // empty: first > last
    }
    const bool same_pages = img.watched && std::memcmp(pages, img.watch_pages, sizeof(pages)) == 0;
    bool written = false;
    for (uint32_t r = 0; r < 2; ++r) {
      if (pages[r][0] <= pages[r][1]) written |= AnyWritten(pages[r][0], pages[r][1]);
    }
    if (img.content_written && !img.flushed && same_pages && !written && !debug_full) return;  // no CPU write since the last read
    // E2 census: a host writer reported these bytes (as against the page watch alone).
    img.read_notified = false;
    for (uint32_t r = 0; r < 2; ++r) {
      if (pages[r][0] <= pages[r][1]) img.read_notified |= AnyWritten(pages[r][0], pages[r][1], g_notified_pages);
    }
    // Clear and protect before reading: a write during the read is caught next time.
    FineNs fine(g_fine.tex_watch);
    g_fine.tex_watch_calls += g_fine_on;
    for (uint32_t r = 0; r < 2; ++r) {
      if (pages[r][0] > pages[r][1]) continue;
      ClearWritten(pages[r][0], pages[r][1]);
      ClearWritten(pages[r][0], pages[r][1], g_notified_pages);
      if (protect) REX_KERNEL_MEMORY()->EnablePhysicalMemoryAccessCallbacks(range[r][0], range[r][1], true, false);
    }
    std::memcpy(img.watch_pages, pages, sizeof(pages));
    img.watched = true;
  } else {
    img.watched = false;
    const uint64_t sample = SampleTexture(w);
    if (img.content_written && !img.flushed && sample == img.content_sample && frame - img.content_read_frame < kFullReadEvery && !debug_full) return;
    img.content_sample = sample;
    img.content_read_frame = frame;
  }
  if (g_workers.Ensure()) {  // S100 speed: the read on a worker, joined when the frame is serialized (JoinReaderWork)
    g_workers.Submit(id, w);
    return;
  }
  static thread_local std::vector<uint8_t> blob;
  blob.clear();
  if (!DecodeTexture(id, w, blob)) return;
  const bool named = img.flushed;
  img.flushed = false;
  // render-clock: lookup - provenance stamp of when an image was first read, a join key
  if (!img.first_read_seq) img.first_read_seq = wwe2k14::prov::Map().Stamp(uint32_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)));  // S101 lane17 review
  const uint64_t hash = cap::Xxh3(blob.data(), blob.size());
  const bool same = img.content_written && hash == img.content_hash;
  if (SourceCensusOn()) CensusTextureRead(id, img.content_written, named || img.read_notified, same);
  if (REXCVAR_GET(wwe2k14_rdr_debug_content_census)) {
    // S100 speed diagnosis: which textures are read again and again (and how often the bytes came back the same), per 600 guest frames.
    struct Entry {
      uint64_t reads = 0, bytes = 0, same = 0;
      uint32_t width = 0, height = 0, format = 0, address = 0;
    };
    static std::unordered_map<uint32_t, Entry> sent;
    static uint64_t window_start = 0;
    Entry& e = sent[id];
    ++e.reads;
    e.same += same ? 1 : 0;
    e.bytes += blob.size();
    e.width = img.rec.width;
    e.height = img.rec.height;
    e.format = BaseFormat(img.game_format);
    e.address = w[1] & 0xFFFFF000u;
    const uint64_t guest = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
    if (guest >= window_start + 600) {
      std::vector<std::pair<uint64_t, uint32_t>> order;
      uint64_t total = 0, reads = 0;
      for (const auto& [k, v] : sent) {
        order.push_back({v.bytes, k});
        total += v.bytes;
        reads += v.reads;
      }
      std::sort(order.rbegin(), order.rend());
      std::string top;
      for (size_t i = 0; i < order.size() && i < 10; ++i) {
        const Entry& v = sent[order[i].second];
        top += fmt::format("{}image {} {}x{} fmt {} @{:08X}: {} reads ({} unchanged) {:.1f} MB", i ? "; " : "", order[i].second, v.width, v.height, v.format,
                           v.address, v.reads, v.same, v.bytes / 1048576.0);
      }
      REXLOG_INFO("[rdr][content] frames {}-{}: {} textures read {} times, {:.1f} MB: {}", window_start, guest, sent.size(), reads, total / 1048576.0, top);
      sent.clear();
      window_start = guest;
    }
  }
  if (same) return;
  img.content_hash = hash;
  img.content_written = true;
  std::vector<uint8_t> copy;
  copy.swap(blob);  // S100 speed: handed over, not copied (the next read grows a new buffer)
  const uint64_t size = copy.size();
  const uint64_t at = SinkAppend(cap::Stream::Textures, std::move(copy));
  if (at == UINT64_MAX) return;
  cap::ContentRef ref;
  ref.image = id;
  ref.offset = at;
  ref.size = size;
  g_frame.contents.push_back(ref);
}

inline void CensusSourceLog(uint64_t from, uint64_t to) {
  if (!SourceCensusOn()) return;
  SourceCensus& s = g_source;
  std::vector<std::pair<uint32_t, uint32_t>> tex(s.tex_unnamed_by_image.begin(), s.tex_unnamed_by_image.end());
  std::sort(tex.begin(), tex.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::string tex_list;
  for (size_t i = 0; i < tex.size() && i < 10; ++i) {
    const uint32_t id = tex[i].first;
    const Image& img = ById(id);
    const auto at = s.tex_addr.find(id);
    tex_list += fmt::format("{}image {} {}x{} fmt {}{} @{:08X} x{}", i ? "; " : "", id, img.rec.width, img.rec.height, BaseFormat(img.game_format),
                            img.linear_static ? " linear" : "", at != s.tex_addr.end() ? at->second : 0u, tex[i].second);
  }
  std::vector<std::pair<uint32_t, SourceCensus::GeoChange>> geo(s.geo_changed_by_addr.begin(), s.geo_changed_by_addr.end());
  std::sort(geo.begin(), geo.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
  std::unordered_map<uint64_t, uint32_t> by_vs;
  for (const auto& g : geo) by_vs[g.second.vs] += g.second.n;
  std::string vs_list;
  for (const auto& [h, n] : by_vs) vs_list += fmt::format("{}vs {:016X} x{}", vs_list.empty() ? "" : ", ", h, n);
  std::string geo_list;
  for (size_t i = 0; i < geo.size() && i < 10; ++i) {
    geo_list += fmt::format("{}@{:08X} {} B x{}", i ? "; " : "", geo[i].first, geo[i].second.bytes, geo[i].second.n);
  }
  REXLOG_INFO("[rdr][source] frames {}-{}: textures read {} new, {} unchanged, {} changed after a flush or a host write report, {} changed "
              "with only the page watch ({} images{}{}); geometry {} new entries, {} watched rebuilds unchanged, {} changed after a host write "
              "report, {} changed with only the page watch ({} streams{}{}), {} rebuilt at every use (rewritten each frame); small (hashed) {} "
              "unchanged, {} changed; changed streams by vertex shader: {}",
              from, to, s.tex_first, s.tex_same, s.tex_named, s.tex_unnamed, tex.size(), tex.empty() ? "" : ": ", tex_list, s.geo_first, s.geo_same,
              s.geo_named, s.geo_changed, geo.size(), geo.empty() ? "" : ": ", geo_list, s.geo_rewritten, s.geo_small_same, s.geo_small_changed, vs_list.empty() ? "none" : vs_list);
  std::unordered_map<uint32_t, uint32_t> keep_addr;
  keep_addr.swap(s.tex_addr);
  s = SourceCensus{};
  s.tex_addr.swap(keep_addr);
}

// The game thread, at FrameBytes (under the reader's lock), after WaitAll: this frame's texture reads, in submission order, into the frame.
inline void JoinTextureJobs() {
  for (std::unique_ptr<TextureJob>& job : g_workers.frame_jobs) {
    for (const NoteCount& n : job->notes) NoteCounted(n.name.c_str(), n.n);
    if (!job->ok) continue;
    Image& img = ById(job->id);
    if (job->gen != img.read_gen) {  // lane 195: read for the shape before a record refresh: never ship it, never mark the image read
      Note("texture_job_dropped_after_record_refresh");
      continue;
    }
    const bool named = img.flushed;
    img.flushed = false;
    // render-clock: lookup - provenance stamp of when an image was first read, a join key
    if (!img.first_read_seq) img.first_read_seq = wwe2k14::prov::Map().Stamp(uint32_t(wwe2k14::g_guest_frame.load(std::memory_order_relaxed)));  // S101 lane17 review
    if (SourceCensusOn()) CensusTextureRead(job->id, img.content_written, named || img.read_notified, img.content_written && job->hash == img.content_hash);
    if (img.content_written && job->hash == img.content_hash) continue;
    img.content_hash = job->hash;
    img.content_written = true;
    const uint64_t size = job->blob.size();
    const uint64_t at = SinkAppend(cap::Stream::Textures, std::move(job->blob));
    if (at == UINT64_MAX) continue;
    cap::ContentRef ref;
    ref.image = job->id;
    ref.offset = at;
    ref.size = size;
    g_frame.contents.push_back(ref);
  }
  g_workers.frame_jobs.clear();
}

// ---- texture slots ------------------------------------------------------------------------------------------------------------------
inline void ComposeSwizzle(uint32_t fetch_swizzle, const uint8_t host[4], bool swap, uint8_t out[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t sel = (fetch_swizzle >> (3 * i)) & 7;
    uint8_t comp;
    if (sel <= 3) comp = host[sel];
    else if (sel == 4) comp = 4;
    else if (sel == 5) comp = 5;
    else {
      comp = 4;
      Note("fetch_swizzle_reserved");
    }
    if (swap) {
      if (comp == 0) comp = 2;
      else if (comp == 2) comp = 0;
    }
    out[i] = comp <= 3 ? uint8_t(kSwzR + comp) : (comp == 4 ? kSwzZero : kSwzOne);
  }
}

inline uint8_t AddressMode(uint32_t clamp) {
  // S4: VkSamplerAddressMode numbers
  switch (clamp) {
    case 0: return 0;
    case 1: return 1;
    case 2: case 4: return 2;
    case 3: case 5: case 7: return 4;
    case 6: return 3;
    default: return 2;
  }
}

inline uint32_t BuildSampler(const uint32_t w[6], const TFetch& t) {
  rdr::SamplerDesc d{};
  // S1: the shader's fetch instruction wins when it names one.
  const uint32_t mag = t.mag != 3 ? t.mag : (w[3] >> 19) & 3;
  const uint32_t min = t.min != 3 ? t.min : (w[3] >> 21) & 3;
  uint32_t mip = t.mip != 3 ? t.mip : (w[3] >> 23) & 3;
  const uint32_t aniso = t.aniso != 7 ? t.aniso : (w[3] >> 25) & 7;
  d.mag_filter = mag == 1;
  d.min_filter = min == 1;
  d.mip_mode = mip == 1;
  d.max_anisotropy = 1;
  if (aniso >= 2 && aniso <= 5) {  // S2
    d.max_anisotropy = uint8_t(1u << (aniso - 1));
    d.mag_filter = d.min_filter = 1;
    d.mip_mode = 1;
  }
  // Settings parity S2: the player's texture filtering, on the samplers the old path gave it to (linear min and mag, mipmapped: not base map);
  // anisotropic sampling takes linear mips with it, as there.
  if (g_aniso_override >= 0 && g_aniso_override <= 5 && d.mag_filter && d.min_filter && mip != 2) {
    d.max_anisotropy = g_aniso_override == 0 ? uint8_t(1) : uint8_t(1u << (g_aniso_override - 1));
    if (d.max_anisotropy > 1) d.mip_mode = 1;
  }
  d.address_u = AddressMode((w[0] >> 10) & 7);
  d.address_v = AddressMode((w[0] >> 13) & 7);
  d.address_w = AddressMode((w[0] >> 16) & 7);
  const uint32_t border = w[5] & 3;
  d.border = border == 1 ? 4 : 0;  // VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : TRANSPARENT_BLACK (S6)
  if (border >= 2) Note("border_colour_ycbcr");
  d.lod_bias = 0;  // S7: the bias lives in the shader
  if (g_jitter.lod_bias != 0.0f && d.mip_mode == 1 && d.min_filter) d.lod_bias = g_jitter.lod_bias;  // S10b: textures for the output size
  d.min_lod = float((w[4] >> 2) & 0xF);
  d.max_lod = mip == 2 ? d.min_lod + 0.25f : 1000.0f;  // S12
  if (g_debug_lod0) d.max_lod = d.min_lod;  // S100 diagnosis
  if (g_debug_no_aniso) d.max_anisotropy = 1;  // S100 diagnosis
  return Dedupe(g_frame.samplers, g_frame.sampler_map, d, reinterpret_cast<const uint8_t*>(&d), sizeof(d));
}

struct SlotCache {
  uint8_t kind = 0;  // 0 none, 1 a texture, 2 no texture (T14)
  rdr_adapter::slot_geo_memo::SlotKey key;  // S102 T15: generation, epoch, the shader's fetch facts (g_vs / g_ps entries: never changed once made), object, words, header bytes
  uint32_t id = 0;
  bool packed = false, copy_dest = false, swap = false, target16 = false;
  uint8_t pack_host[4] = {};
  cap::TextureSlot out;
};
inline rdr_adapter::slot_geo_memo::SlotWayTable<SlotCache, 32> g_slot_cache;  // S102 T15: way 0 is the old one-entry-per-slot record
inline rdr_adapter::slot_geo_memo::SamplerMemo g_sampler_memo;                // S102 T15 (wwe2k14_rdr_sampler_memo)

// S102 T15 (wwe2k14_rdr_slot_content_skip): EnsureContent(id, w), minus the call when its own first lines would return at once.
inline void EnsureContentChecked(uint32_t id, const uint32_t w[6]) {
  namespace dm = rdr_adapter::slot_geo_memo;
  if (dm::g_content_skip_on.load(std::memory_order_relaxed)) {
    const Image& img = ById(id);
    if (dm::ContentDone(img.copy_dest, img.checked_frame, g_record_frame)) {
      ++dm::g_census.content.taken;
      return;
    }
    ++dm::g_census.content.declined;
    ++dm::g_census.content.by[img.copy_dest ? 1 : 0];
  }
  EnsureContent(id, w);
}
