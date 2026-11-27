
enum class GeoResult { kNone, kBuilt, kPending };

// S102 T15: BuildGeometry's hash of the whole vertex stream, for RunGeometryBuild's GeoKey (wwe2k14_rdr_geo_hash_once).
inline thread_local rdr_adapter::slot_geo_memo::StreamHint t_stream_hint;
struct HintGuard {
  ~HintGuard() { t_stream_hint = rdr_adapter::slot_geo_memo::StreamHint{}; }
};

// One build: gathered here, then converted here (small) or on a worker (big: kPending, `pending` the job). Built: `out` and `content_key`.
inline GeoResult RunGeometryBuild(const uint8_t* base, uint32_t dev, const DrawArgs& a, const VertexShaderInfo& vs, GeoRef& out, uint64_t& content_key,
                                  GeoJob*& pending) {
  static thread_local GeoJob here;
  here.Clear();
  const uint64_t tb0 = ReaderTicks();
  const bool gathered = GeoGather(base, dev, a, vs, here);
  const uint64_t tb1 = ReaderTicks();
  g_times.geo_gather += tb1 - tb0;
  if (!gathered) return GeoResult::kNone;
  struct WorstOnExit {  // lane 273
    uint64_t t0;
    const GeoJob& g;
    bool worker = false;
    ~WorstOnExit() {
      const uint64_t t = ReaderTicks() - t0;
      if (t > g_worst_build.ticks) g_worst_build = {t, g.hash_len + (g.is_inline ? 0 : g.count * (g.idx32 ? 4u : 2u)), g.count, g.is_inline, worker};
    }
  } worst{tb0, here};
  const int32_t threshold = REXCVAR_GET(wwe2k14_rdr_geo_worker_bytes);
  const uint64_t bytes = uint64_t(here.hash_len) + (here.is_inline ? 0 : uint64_t(here.count) * (here.idx32 ? 4 : 2));
  if (threshold >= 0 && bytes >= uint64_t(threshold) && g_workers.Ensure()) {
    GeoJob* j = g_geo_jobs.Take();
    // the gathered inputs to the pooled job (its buffers kept between frames), then the bytes copied into it
    j->vs = here.vs;
    j->prim = here.prim;
    j->is_inline = here.is_inline;
    j->idx32 = here.idx32;
    j->restart_on = here.restart_on;
    j->endian = here.endian;
    j->ib_endian = here.ib_endian;
    j->reset_value = here.reset_value;
    j->base_vertex = here.base_vertex;
    j->vmin = here.vmin;
    j->vmax = here.vmax;
    j->guest_stride_bytes = here.guest_stride_bytes;
    j->count = here.count;
    j->lo = here.lo;
    j->v_count = here.v_count;
    j->hash_len = here.hash_len;
    j->avail = here.avail;
    j->vsrc = here.vsrc;
    j->isrc = here.isrc;
    const uint64_t th0 = ReaderTicks();
    GeoCopyInputs(*j);
    ++g_times.geo_async;
    g_workers.Queue(j, false);  // S102 R25-A: a geometry build reads copies made at the draw, not guest memory
    g_times.geo_hand += ReaderTicks() - th0;
    worst.worker = true;
    pending = j;
    return GeoResult::kPending;
  }
  static thread_local std::vector<uint32_t> indices;
  const uint64_t tk0 = ReaderTicks();
  namespace dm = rdr_adapter::slot_geo_memo;
  // S102 T15 (wwe2k14_rdr_geo_hash_once): BuildGeometry hashed the whole stream for its memo check; this is the same bytes when the range is all of it.
  if (!here.is_inline && t_stream_hint.valid && dm::g_geo_hash_once_on.load(std::memory_order_relaxed)) {
    if (dm::HintMatches(t_stream_hint, here.vsrc, here.hash_len)) {
      here.pre_vhash_ok = true, here.pre_vhash = t_stream_hint.hash;
      ++dm::g_census.hash.taken;
    } else {
      ++dm::g_census.hash.declined, ++dm::g_census.hash.by[0];
    }
  }
  GeoKey(here, indices);
  content_key = here.content_key;
  const uint64_t tk1 = ReaderTicks();
  g_times.geo_key += tk1 - tk0;
  const bool found = GeoFindStored(here, out);
  const uint64_t tk2 = ReaderTicks();
  g_times.geo_find += tk2 - tk1;
  if (found) return GeoResult::kBuilt;
  const bool converted = GeoConvert(here, indices);
  const uint64_t tk3 = ReaderTicks();
  g_times.geo_conv += tk3 - tk2;
  if (!converted) return GeoResult::kNone;
  const bool stored = GeoStore(here, out);
  g_times.geo_store += ReaderTicks() - tk3;
  return stored ? GeoResult::kBuilt : GeoResult::kNone;
}

// A draw whose inputs are all the same as an earlier draw's gets that draw's result without reading or hashing the guest bytes again (S100
// speed: re-reading and hashing every draw's vertex range cost ~1.6 us a draw). Within a frame the game does not rewrite a buffer between
// two draws: reused as is (a build still pending on a worker included). From an earlier frame: reused while no page of its vertex stream
// or index range was written (the write watch) or, where it cannot be watched, while a 48-point sample of them is unchanged (at most
// kFullReadEvery frames after the last full read), and while its converted bytes are still held. Inline draws carry new bytes: never memoised.
struct GeoMemoKey {
  uint32_t fetch[2], layout_lo, layout_hi, guest_stride, prim, base_vertex, start_index, index_count, ib_flags, ib_addr, ib_size, vmin, vmax, sc,
      reset;
};
struct GeoMemoEntry {
  GeoRef ref;
  uint64_t content_key = 0, sample = 0, used_frame = 0, read_frame = 0;
  bool watched = false;              // S100 speed: valid while no page of its vertex stream or index range was written (g_written_pages_geo)
  uint32_t pages[2][2] = {{1, 0}, {1, 0}};
  GeoJob* pending = nullptr;         // S100 speed: built on a worker, not joined yet (this frame part only: JoinGeometryJobs clears it)
  uint64_t clear_epoch = 0;          // lane 266: g_geo_clear_epoch when stored_epoch was set
  uint64_t stored_epoch = 0;         // S100 speed: g_geo_forget_epoch when it was last found (or made) stored
  uint64_t marked_frame = 0;         // S100 speed: the frame its g_geo_used mark was last written
  bool rewritten = false;            // S100 speed: found written at its last two watched reads: not watched, rebuilt at every use
  uint8_t written_streak = 0;        // watched reads in a row that found it written
  uint8_t probe = 0;                 // rebuilds since it was last watched (watched again at kGeoProbeEvery)
  bool small_geo = false;            // strip-360 E3: its bytes fit kSmallGeoBytes: checked by a full hash at every use, never protected
  uint64_t full_hash = 0;            // that hash (the whole vertex stream and the index range)
};
inline std::unordered_map<uint64_t, GeoMemoEntry> g_geo_memo;
inline void ResetGeoMemoForNewStore() { rdr_adapter::geostore::BeginNewStore(g_record_frame, g_geo_memo); }  // S103 IXF (geo_store_reset.h)
// Strip-360 E3: geometry whose vertex stream and index range together fit this many bytes is checked by a full hash of those bytes at every
// use (exact whoever writes it: the crowd's ~3 KB skinned streams, ~0.15 us each); larger geometry by the host writers' reports and the
// game's vertex-buffer unlock (src\vb_unlock_report.h; verify run 6990: every change announced). Page protection only on request.
inline constexpr uint32_t kSmallGeoBytes = 8192;
REXCVAR_DECLARE(bool, wwe2k14_rdr_geo_protect_large);

// S100 speed: memo entries unused for kLiveGeometryKeepFrames are dropped every 600 recorded frames (half-way between LiveForgetGeometry's
// sweeps). Before, entries went only when the map reached 262,144 and was cleared whole: dynamic buffers make new keys every frame, so the
// map grew to that size (every lookup a trip to main memory) and its clear rebuilt every draw of one frame. A map left much smaller than
// its buckets is rebuilt (the standard map never gives buckets back).
inline void ForgetStaleMemo() {
  if (g_record_frame % 600 != 300) return;
  const auto memo_t0 = std::chrono::steady_clock::now();  // lane 266
  for (auto it = g_geo_memo.begin(); it != g_geo_memo.end();) {
    if (!it->second.pending && g_record_frame - it->second.used_frame > kLiveGeometryKeepFrames) it = g_geo_memo.erase(it);
    else ++it;
  }
  if (g_geo_memo.bucket_count() > 8 * g_geo_memo.size() + 4096) {
    std::unordered_map<uint64_t, GeoMemoEntry> fresh(g_geo_memo.begin(), g_geo_memo.end());
    g_geo_memo.swap(fresh);
  }
  g_periodic.memo.Add(PeriodicMs(memo_t0));
}

inline GeoResult BuildGeometry(const uint8_t* base, uint32_t dev, const DrawArgs& a, const VertexShaderInfo& vs, GeoRef& out, GeoJob*& pending) {
  uint64_t content_key = 0;
  pending = nullptr;
  if (a.inline_vertices) {
    ++g_times.geo_inline;
    ScopedNs build_timer(g_times.geo_build);
    return RunGeometryBuild(base, dev, a, vs, out, content_key, pending);
  }
  GeoMemoKey k{};
  const uint32_t c = vs.fetch_constant;
  const uint32_t at = dev + kDevFetch + 24 * (c / 3) + 8 * (c % 3);
  k.fetch[0] = db::Ld32(base, at);
  k.fetch[1] = db::Ld32(base, at + 4);
  k.layout_lo = uint32_t(vs.layout_hash);
  k.layout_hi = uint32_t(vs.layout_hash >> 32);
  k.guest_stride = vs.guest_stride;
  k.prim = uint32_t(a.prim);
  k.base_vertex = uint32_t(a.base_vertex);
  k.start_index = a.start_index;
  k.index_count = a.index_count;
  const uint32_t ib = db::Ld32(base, dev + kDevIb);
  k.ib_flags = db::Ld32(base, ib);
  k.ib_addr = db::Ld32(base, ib + 24);
  k.ib_size = db::Ld32(base, ib + 28);
  k.vmin = db::Ld32(base, dev + kDevVgtMin);
  k.vmax = db::Ld32(base, dev + kDevVgtMax);
  k.sc = db::Ld32(base, dev + kDevScMode);
  k.reset = db::Ld32(base, dev + kDevReset);
  const uint64_t memo_key = db::Hash64(reinterpret_cast<const uint8_t*>(&k), sizeof(k));
  const uint64_t frame = g_record_frame;
  auto sample_of = [&]() {
    uint64_t s[48] = {};
    const uint32_t stream_bytes = ((k.fetch[1] >> 2) & 0xFFFFFF) * 4;
    if ((k.fetch[0] & ~3u) && stream_bytes >= 8) {
      const uint8_t* p = Phys(k.fetch[0] & ~3u);
      for (uint32_t i = 0; i < 32; ++i) std::memcpy(&s[i], p + (uint64_t(stream_bytes - 8) * i / 31 & ~3ull), 8);
    }
    const uint32_t unit = (k.ib_flags >> 31) ? 4 : 2;
    const uint64_t ib_bytes = uint64_t(k.index_count) * unit;
    if (k.ib_addr && ib_bytes >= 8) {
      const uint8_t* p = db::Host(base, k.ib_addr + k.start_index * unit);
      for (uint32_t i = 0; i < 16; ++i) std::memcpy(&s[32 + i], p + ((ib_bytes - 8) * i / 15 & ~1ull), 8);
    }
    return db::Hash64(reinterpret_cast<const uint8_t*>(s), sizeof(s));
  };
  // S100 speed: the bytes this draw reads, watched (the vertex stream as the fetch constant gives it, the index range).
  const bool watch = REXCVAR_GET(wwe2k14_rdr_watch_texture_pages);
  const uint32_t stream_bytes = ((k.fetch[1] >> 2) & 0xFFFFFF) * 4;
  const uint32_t index_unit = (k.ib_flags >> 31) ? 4 : 2;
  // The index buffer's address is a CPU virtual one (read through db::Host): only the physical views (0xA0000000 up) can be watched, and
  // the 0xE0000000 view's physical address is 4 KB higher (RememberHeaderWords). Anything else keeps the sample.
  const uint32_t index_virt = k.ib_addr ? k.ib_addr + k.start_index * index_unit : 0, index_bytes = k.ib_addr ? k.index_count * index_unit : 0;
  const bool index_watchable = !index_bytes || index_virt >= 0xA0000000u;
  const uint32_t index_phys = wwe2k14::guest_view::ToPhysical(index_virt);
  // Strip-360 E3: small geometry (stream + index range within kSmallGeoBytes) is checked by a full hash at every use, never protected.
  const bool small_geo = !PageWatchOn() && (k.fetch[0] & ~3u) && stream_bytes && uint64_t(stream_bytes) + index_bytes <= kSmallGeoBytes;
  // S102 T15 (wwe2k14_rdr_geo_hash_once): the stream's hash and the combined one are kept for this draw: the memo check, the memo store and (the stream
  // part) GeoKey need the same bytes' hash, and the old code made it at each of them. Off: made again at every call, as before.
  namespace dm = rdr_adapter::slot_geo_memo;
  const bool hash_once = dm::g_geo_hash_once_on.load(std::memory_order_relaxed);
  bool hashed = false;
  uint64_t hashed_full = 0, hashed_stream = 0;
  auto full_hash_of = [&]() -> uint64_t {
    if (hash_once && hashed) {
      ++dm::g_census.hash.taken;
      return hashed_full;
    }
    const uint64_t sh = cap::Xxh3(Phys(k.fetch[0] & ~3u), stream_bytes);
    const uint64_t h = dm::CombineSmall(sh, index_bytes != 0, index_bytes ? cap::Xxh3(db::Host(base, index_virt), index_bytes) : 0);
    hashed = true, hashed_stream = sh, hashed_full = h;
    return h;
  };
  auto it = g_geo_memo.find(memo_key);
  uint8_t carry_streak = 0, carry_probe = 0;  // S100 speed: the entry's history for the rebuild below (rewritten buffers: not watched)
  const bool census = SourceCensusOn();
  bool census_rewatch = false;  // E1: a watched entry rebuilt because its pages were written
  bool census_named = false;    // E3: and a host writer reported them
  uint64_t census_old = 0;
  if (census && it == g_geo_memo.end()) ++g_source.geo_first;
  if (it != g_geo_memo.end()) {
    GeoMemoEntry& e = it->second;
    carry_streak = e.written_streak;
    carry_probe = e.probe;
    if (e.pending) {  // the same draw inputs earlier in this frame part, its build still on a worker: patched at the join with it
      e.used_frame = frame;
      pending = e.pending;
      ++g_times.geo_attached;
      return GeoResult::kPending;
    }
    bool ok = rdr_adapter::geostore::UsedThisFrame(e.used_frame, frame);  // already used this frame: checked and marked by its first hit
    // Still stored? Nothing forgotten since it was last found stored (g_geo_forget_epoch): yes, without a lookup. Otherwise the answer is a
    // lookup in g_cap.geo, and the entry takes the offsets found THERE (lane 293: a content key forgotten and stored again under a new sink
    // key by another memo entry, the sign boards' double-buffered ring, must not keep handing out the dropped key). In live mode the used
    // mark is taken too (LiveForgetGeometry drops g_cap.geo and g_geo_used together).
    uint64_t* used = nullptr;
    bool stored = false;
    if (!ok) {
      if (GeoStillStored(e.clear_epoch, e.stored_epoch, e.marked_frame, frame, g_live.active())) {
        stored = true;
      } else {
        stored = rdr_adapter::geoforget::ResolveStored(false, g_cap.geo, e.content_key, e.ref);
        if (stored && g_live.active()) {
          if (auto u = g_geo_used.find(e.content_key); u != g_geo_used.end()) used = &u->second;
        }
      }
      if (stored) e.stored_epoch = g_geo_forget_epoch, e.clear_epoch = g_geo_clear_epoch;
    }
    if (!ok && stored) {
      if (e.small_geo && small_geo) {
        ok = full_hash_of() == e.full_hash;  // E3: exact, whoever wrote it
        if (census) ++(ok ? g_source.geo_small_same : g_source.geo_small_changed);
      } else if (e.watched) {
        ok = true;
        for (uint32_t r = 0; r < 2; ++r) {
          if (e.pages[r][0] <= e.pages[r][1] && AnyWritten(e.pages[r][0], e.pages[r][1], g_written_pages_geo)) ok = false;
        }
        if (!ok) {  // written since its last read
          carry_streak = uint8_t(std::min<uint32_t>(e.written_streak + 1u, 255u));
          carry_probe = 0;
          census_rewatch = census;
          census_old = e.content_key;
          for (uint32_t r2 = 0; r2 < 2 && census; ++r2) {
            if (e.pages[r2][0] <= e.pages[r2][1]) census_named |= AnyWritten(e.pages[r2][0], e.pages[r2][1], g_notified_pages_geo);
          }
        }
      } else if (e.rewritten) {
        // Rewritten at its last watched reads: not watched, so rebuilt at every use (exact); counted towards its next watched probe.
        carry_probe = uint8_t(std::min<uint32_t>(e.probe + 1u, 255u));
        if (census) ++g_source.geo_rewritten;
      } else if (frame - e.read_frame < kFullReadEvery) {
        ok = sample_of() == e.sample;
      }
    }
    if (ok) {
      e.used_frame = frame;
      if (e.watched) e.written_streak = 0;
      out = e.ref;
      bool refreshed = true;
      if (used) {
        *used = frame;
        e.marked_frame = frame;
      } else if (g_live.active() && frame - e.marked_frame >= kGeometryMarkEvery) {
        auto u = g_geo_used.find(e.content_key);
        refreshed = rdr_adapter::geoforget::MarkRefresh(u != g_geo_used.end(), e.marked_frame, frame);  // lane 293: no key, no mark: rebuild
        if (refreshed) u->second = frame;
      }
      if (refreshed) {
        ++g_times.geo_hits;
        return GeoResult::kBuilt;
      }
    }
  }
  // Watch before reading: a write during the build is caught next time (a build on a worker reads the copy made at the draw). S100 speed:
  // not for a buffer found rewritten at its last two watched reads (run 6932: 60-90 such ranges a frame in WM30's match, each a protection
  // system call here and a write fault on the game's thread every frame, for a rebuild that came anyway); every kGeoProbeEvery-th of its
  // rebuilds is watched again, so one that stopped changing goes back to memo hits.
  constexpr uint8_t kGeoProbeEvery = 32;
  const bool rewritten = carry_streak >= 2 && carry_probe < kGeoProbeEvery;
  uint32_t pages[2][2] = {{1, 0}, {1, 0}};
  bool watched;
  const int geo_protect = (PageWatchOn() || (!small_geo && REXCVAR_GET(wwe2k14_rdr_geo_protect_large))) ? 1 : 0;
  const uint64_t small_hash = small_geo ? full_hash_of() : 0;  // (the bytes the build below reads)
  {
    FineNs fine(g_fine.geo_watch);
    watched = !rewritten && watch && index_watchable && (k.fetch[0] & ~3u) && stream_bytes &&
              WatchRange(k.fetch[0] & ~3u, stream_bytes, g_written_pages_geo, pages[0], g_watched_pages_geo, geo_protect) &&
              (!index_bytes || WatchRange(index_phys, index_bytes, g_written_pages_geo, pages[1], g_watched_pages_geo, geo_protect));
    if (g_fine_on && watch && index_watchable && (k.fetch[0] & ~3u) && stream_bytes) g_fine.geo_watch_calls += index_bytes ? 2 : 1;
  }
  if (!index_bytes) pages[1][0] = 1, pages[1][1] = 0;
  ++g_times.geo_builds;
  GeoResult r;
  {
    ScopedNs build_timer(g_times.geo_build);
    HintGuard hint_guard;  // S102 T15: the hint never outlives this build, on any path out
    t_stream_hint = (hash_once && small_geo && hashed) ? dm::StreamHint{Phys(k.fetch[0] & ~3u), stream_bytes, hashed_stream, true} : dm::StreamHint{};
    if (hash_once && !(small_geo && hashed)) ++dm::g_census.hash.declined, ++dm::g_census.hash.by[1];
    r = RunGeometryBuild(base, dev, a, vs, out, content_key, pending);
  }
  if (r == GeoResult::kNone) return r;
  if (census_rewatch) {
    if (r == GeoResult::kBuilt) {
      CensusGeometryRebuild(census_old, content_key, k.fetch[0] & ~3u, stream_bytes, vs.original_hash, census_named);
    } else if (pending) {
      pending->census_rewatch = true;
      pending->census_old_key = census_old;
      pending->census_addr = k.fetch[0] & ~3u;
      pending->census_bytes = stream_bytes;
      pending->census_named = census_named;
    }
  }
  if (g_geo_memo.size() > 262144) g_geo_memo.clear();
  GeoMemoEntry& e = g_geo_memo[memo_key];
  e.ref = r == GeoResult::kBuilt ? out : GeoRef{};
  e.content_key = r == GeoResult::kBuilt ? content_key : 0;
  e.pending = pending;
  e.watched = watched;
  e.small_geo = small_geo;
  e.full_hash = small_hash;
  e.rewritten = rewritten;
  e.written_streak = carry_streak;
  e.probe = rewritten ? carry_probe : 0;
  std::memcpy(e.pages, pages, sizeof(pages));
  if (!watched && !rewritten) e.sample = sample_of();
  e.used_frame = e.read_frame = e.marked_frame = frame;
  e.clear_epoch = g_geo_clear_epoch;
  e.stored_epoch = r == GeoResult::kBuilt ? g_geo_forget_epoch : 0;  // a pending build: set at the join
  if (pending) {
    pending->memo = true;
    pending->memo_key = memo_key;
  }
  return r;
}

inline void ApplyGeometry(cap::Draw& d, const GeoRef& g) {
  d.vertex_offset = g.v_off;
  d.vertex_bytes = g.v_bytes;
  d.vertex_count = g.v_count;
  d.index_offset = g.i_off;
  d.index_bytes = g.i_bytes;
  d.index_count = g.i_count;
  d.index_32bit = g.idx32;
  d.flags |= g.flags;
}

// The game thread, after WaitAll: this frame's geometry builds committed (dedupe, the sink), their draws patched, their memo entries filled.
inline void JoinGeometryJobs() {
  FrameTables& t = g_frame;
  for (size_t k = 0; k < g_geo_jobs.used; ++k) {
    GeoJob& j = *g_geo_jobs.jobs[k];
    for (const NoteCount& n : j.notes) NoteCounted(n.name.c_str(), n.n);
    GeoRef g;
    const bool ok = j.ok && (GeoFindStored(j, g) || GeoStore(j, g));
    if (j.census_rewatch && j.ok) CensusGeometryRebuild(j.census_old_key, j.content_key, j.census_addr, j.census_bytes, j.vs ? j.vs->original_hash : 0, j.census_named);
    for (uint32_t d : j.draws) {
      if (d >= t.draws.size()) continue;
      if (ok) ApplyGeometry(t.draws[d], g);
      else t.draws[d].flags |= cap::kDrawNoGeometry;
    }
    if (j.memo) {
      if (auto it = g_geo_memo.find(j.memo_key); it != g_geo_memo.end() && it->second.pending == &j) {
        if (ok) {
          it->second.ref = g;
          it->second.content_key = j.content_key;
          it->second.pending = nullptr;
          it->second.clear_epoch = g_geo_clear_epoch;
          it->second.stored_epoch = g_geo_forget_epoch;  // GeoFindStored / GeoStore just marked it used
          it->second.marked_frame = g_record_frame;
        } else {
          g_geo_memo.erase(it);
        }
      }
    }
    j.draws.clear();
    // keep the pool's buffers, but not a load's huge ones
    if (j.v_copy.capacity() > (4u << 20)) std::vector<uint8_t>().swap(j.v_copy);
    if (j.i_copy.capacity() > (4u << 20)) std::vector<uint8_t>().swap(j.i_copy);
  }
  g_geo_jobs.used = 0;
}

// The reader workers' results into this frame (forward-declared above FrameBytes, which calls it first; OnPresent calls it again before
// the frame's tables are reset, for a frame left out of the sink).
inline void JoinReaderWork() {
  if (g_workers.frame_jobs.empty() && !g_geo_jobs.used) return;
  ScopedNs timer(g_times.join);
  g_workers.WaitAll();
  JoinTextureJobs();
  JoinGeometryJobs();
}
