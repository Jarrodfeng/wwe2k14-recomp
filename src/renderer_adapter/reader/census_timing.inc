// Settings parity S10b (DLSS, Jarrod 09:3x): the scene's draws (the same rule as WideState's scene: viewports at least 1200 guest pixels
// wide, not screen-space, before the scene's depth copy) moved by a sub-pixel jitter (dlss_jitter.h, Halton 2 / 3 by phase), which DLSS
// needs. The phase goes in the frame header (kFrameDlssJitter) for the binder. Read at StartLive.
struct JitterState {
  bool on = false;
  uint32_t phases = 8, phase = 0;  // this frame's phase
  float scale = 1.0f;               // the render scale (jitter in render pixels)
  bool scene_done = false;
  bool hud = false;                 // lane 185: the post chain's last pass (the composite marker) has been drawn this frame (its own flag: g_wide.hud is set only when the wide setting is on)
  bool armed = false;               // lane 185: the previous frame had its marker (jitter_rule.h Window::armed): only then is this frame jittered and stamped
  bool legacy = false;              // lane 185: the S10b window (ends at the first scene depth copy), diagnosis --wwe2k14_rdr_dlss_jitter_legacy
  float lod_bias = 0.0f;            // the scene's textures sampled for the output size (DLSS upscaling)
};
inline JitterState g_jitter;

// Settings parity S11: called at each game present (owner mode: the app asks the UI thread to paint its overlay dialogs, overlay_drawer.h).
inline std::function<void()> g_on_present;

inline cap::LiveSink g_live;  // the live mirror (LIVE_ADAPTER.md); off unless --wwe2k14_rdr_live
// (g_owner_window, g_owner_instance, g_owner_window_source, g_owner_window_source_user: renderer_game\owner_globals.h)

// ---- the device object: shadow offsets, from docs\native\d3d\B_d3d_api.md section 2 (register k of a block is 4 bytes after k-1) -------
inline constexpr uint32_t kDevFetch = 1152;
inline constexpr uint32_t kDevVsFloat = 1920, kDevPsFloat = 6016, kDevBoolLoop = 10112;
inline constexpr uint32_t kDevVgtMax = 10444, kDevVgtMin = 10448, kDevReset = 10456, kDevColorMask = 10460, kDevBlendRgba = 10464;
inline constexpr uint32_t kDevStencilBack = 10492, kDevStencilFront = 10496, kDevAlphaRef = 10500, kDevVport = 10504;
inline constexpr uint32_t kDevDepthControl = 10548, kDevBlend0 = 10552, kDevColorControl = 10556, kDevClipCntl = 10564,
                          kDevScMode = 10568, kDevVteCntl = 10572, kDevBlend1 = 10584;
inline constexpr uint32_t kDevScreenTl = 10424, kDevScreenBr = 10428;
inline constexpr uint32_t kDevVtxCntl = 10688, kDevPolyOffset = 10832;
inline constexpr uint32_t kDevDecl = 12240, kDevScissorOn = 12288, kDevStrides = 12904;
inline constexpr uint32_t kDevIb = 12812, kDevRt0 = 12816, kDevDs = 12832, kDevTexObj = 12920;
inline constexpr uint32_t kDevViewport = 13024, kDevScissor = 13052, kDevPs = 13068, kDevVs = 13072;
inline constexpr uint32_t kDevInlinePtr = 13848, kDevInlineDwords = 13856, kDevPresentInterval = 14000;

enum class Prim : uint32_t { Point = 1, Line = 2, LineStrip = 3, TriList = 4, TriFan = 5, TriStrip = 6, TriW = 7, Rect = 8, LineLoop = 12, Quad = 13, QuadStrip = 14, Polygon = 15 };

// VkPrimitiveTopology / VkComponentSwizzle / VkFormat numbers, written out so the adapter needs no Vulkan header.
inline constexpr uint8_t kTopoLineList = 1, kTopoLineStrip = 2, kTopoTriList = 3, kTopoTriStrip = 4, kTopoTriFan = 5;
inline constexpr uint8_t kSwzZero = 1, kSwzOne = 2, kSwzR = 3;
inline constexpr uint16_t kVkRGBA8Unorm = 37, kVkRGBA8Snorm = 38, kVkRGBA8Uint = 41, kVkRGBA8Sint = 42, kVkRGB10A2Unorm = 64, kVkRG16Unorm = 77, kVkRG16Snorm = 78,
                          kVkRG16Float = 83, kVkRGBA16Unorm = 91, kVkRGBA16Snorm = 92, kVkRGBA16Float = 97, kVkR32Float = 100, kVkRG32Float = 103,
                          kVkRGB32Float = 106, kVkRGBA32Float = 109;

// ---- census ------------------------------------------------------------------------------------------------------------------------
struct Census {
  std::map<std::string, uint64_t> window;
  std::map<std::string, uint64_t> seen;
  uint64_t window_start = 0;
  uint64_t draws = 0, inlines = 0, clears = 0, copies = 0, presents = 0;
};
inline Census g_census;

// Where the reader's time goes on the game's render thread (S100 speed work): nanoseconds per census window, by part.
struct ReaderTimes {
  uint64_t draw = 0, shaders = 0, slots = 0, content = 0, banks = 0, geometry = 0, present = 0, targets = 0, state = 0;
  uint64_t geo_build = 0;                                 // S100 speed: of geometry, the ns in builds on the game thread (gathers included)
  uint64_t sink = 0, sink_n = 0;                          // lane 273: ticks in, and number of, SinkAppend calls (the live sink's hand-over of a built range)
  uint64_t geo_gather = 0, geo_key = 0, geo_find = 0, geo_conv = 0, geo_store = 0, geo_hand = 0;  // lane 273: ticks of a build's parts on this thread: gather, content key, stored-copy lookup, convert, store (sink included), copy + hand to a worker
  uint64_t join = 0;                                      // S100 speed: the game thread at JoinReaderWork (waiting for workers + handing over)
  uint64_t geo_hits = 0, geo_builds = 0, geo_inline = 0;  // memo hits, rebuilds (memoised kinds), inline-vertex draws (always built)
  uint64_t geo_async = 0, geo_attached = 0;  // S100 speed: builds sent to the workers; draws that took a pending build of the same frame
  uint64_t state_reused = 0, bool_reused = 0, lits_reused = 0;  // S100 speed: draws that took the last draw's state / bool-loop / literals
  uint64_t slot_adds = 0, slots_reused = 0;                      // S100 speed: texture slots added; of those, the slot's last record again
  uint64_t memo_entries = 0;                                     // S100 speed: the geometry memo's size at the last present (not summed)
  uint64_t images = 0;                                           // S100 speed: images in the registry at the last present (not summed)
  uint64_t geo_protects = 0;                                     // S100 speed: geometry ranges actually protected (system calls)
  uint64_t old_draws = 0, old_copies = 0;  // S100 speed: ticks in the old path's writers after the reader (draw / inline; clear / resolve)
  uint64_t state_by_marks = 0, marks_missed = 0;  // strip-360 C7: draws whose state came from the library's marks; verify: of those, differing
};
inline ReaderTimes g_times;
struct PlaneCounts {  // strip-360 D8 (OnPlaneFlush): run totals
  uint64_t flushes = 0, named = 0, unmatched = 0, verified = 0, missed = 0;
};
inline PlaneCounts g_planes;
inline void CensusSourceLog(uint64_t from, uint64_t to);  // strip-360 E1 (defined below EnsureContent)
struct WorstBuild {  // lane 273: the slowest single geometry build since the last present (reset there); named in the slow-frame line
  uint64_t ticks = 0;
  uint32_t bytes = 0, count = 0;
  bool is_inline = false, worker = false;
};
inline WorstBuild g_worst_build;
inline uint64_t g_lock_window_ns[7];  // lane 273: the live store lock's worst holds / wait (live.h g_store_lock_max_ns) since the last 600-frame line
inline void TakeStoreLockMax(uint64_t frame_ns[7]) {  // each present: this frame's, folded into the window's
  for (int i = 0; i < 7; ++i) {
    frame_ns[i] = rdr_adapter::g_store_lock_max_ns[i].exchange(0, std::memory_order_relaxed);
    g_lock_window_ns[i] = std::max(g_lock_window_ns[i], frame_ns[i]);
  }
}
inline ReaderTimes g_frame_times;  // this frame only: a frame over kSlowFrameMs is logged at once (bursts at loads)
inline constexpr double kSlowFrameMs = 15.0;
// S100 speed: the reader's timers count CPU ticks (x64: the time-stamp counter, 7.5 ns a read on the 5950X against 18.6 for
// steady_clock::now, and every draw passes ~7 timers); TicksPerNs converts at print time, calibrated once against steady_clock.
inline uint64_t ReaderTicks() {
#if defined(_M_X64) || defined(__x86_64__)
  // render-clock: perf - the reader cost counter reads the TSC
  return __rdtsc();
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}
inline double TicksPerNs() {
  static const double ratio = [] {
#if defined(_M_X64) || defined(__x86_64__)
    // render-clock-begin: perf - calibrates TSC ticks against the wall clock for counters
    const auto c0 = std::chrono::steady_clock::now();
    const uint64_t t0 = __rdtsc();
    while (std::chrono::steady_clock::now() - c0 < std::chrono::milliseconds(2)) {
    }
    const auto c1 = std::chrono::steady_clock::now();
    const uint64_t t1 = __rdtsc();
    // render-clock-end
    return double(t1 - t0) / double(std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count());
#else
    return 1.0;
#endif
  }();
  return ratio;
}
struct ScopedNs {  // (ticks, despite the name: the fields it adds to are printed through TicksPerNs)
  uint64_t& acc;
  uint64_t t0 = ReaderTicks();
  explicit ScopedNs(uint64_t& a) : acc(a) {}
  ~ScopedNs() { acc += ReaderTicks() - t0; }
};
// S102 H3: the reader's draw and present totals, in nanoseconds, for FrameStats::OnSwap's swap-thread split (--wwe2k14_frame_split). Reads two counters the reader keeps
// today (g_times.draw / .present, rdtsc ticks, written on this same thread): NO new clock on the per-draw path; called once per guest frame, and only while the switch is on.
// F2: g_times is zeroed every 600 frames by CensusMaybe (inside OnPresent, BEFORE OnSwap): g_split_ticks banks both totals first (every `g_times = {}` is preceded by
// the Bank call: tools\frame_split\frame_split_check.py lints that), so the reported totals never go backwards.
inline wwe2k14::frame_split::RunningTicks g_split_ticks;
inline wwe2k14::frame_split::ReaderNs ReaderSplitNs() {
  const double per_ns = TicksPerNs();
  return {uint64_t(double(g_split_ticks.Draw(g_times.draw)) / per_ns), uint64_t(double(g_split_ticks.Present(g_times.present)) / per_ns)};
}
inline const bool g_reader_split_registered = (wwe2k14::frame_split::g_reader_read = &ReaderSplitNs, true);
// S100 speed diagnosis (--wwe2k14_rdr_debug_fine_times): the per-draw parts split further; nanoseconds and counts per census window.
struct FineTimes {
  uint64_t vs_lookup = 0, ps_lookup = 0, bank_refresh = 0, bank_lits = 0, bank_emit = 0, fixed_decode = 0, fixed_dedupe = 0, dyn_decode = 0,
           dyn_dedupe = 0, bool_loop = 0, layout = 0, slot_add = 0;
  uint64_t geo_watch = 0, tex_watch = 0, geo_watch_calls = 0, tex_watch_calls = 0;  // the write watches' page protection (system calls)
  uint64_t draws = 0, same_vs_key = 0, same_pair = 0, banks_reused = 0, lits = 0, touched = 0, slots = 0, same_fixed = 0, same_dynamic = 0;
  // the last draw's, for the repeat counts
  uint32_t last_vs = 0, last_ps = 0, last_decl = 0, last_fixed = ~0u, last_dynamic = ~0u;
  uint8_t last_strides[24] = {};
};
inline FineTimes g_fine;
inline bool g_fine_on = false;  // read once a frame (OnPresent)
struct FineNs {  // ticks, as ScopedNs
  uint64_t* acc = nullptr;
  uint64_t t0 = 0;
  explicit FineNs(uint64_t& a) {
    if (!g_fine_on) return;
    acc = &a;
    t0 = ReaderTicks();
  }
  ~FineNs() {
    if (acc) *acc += ReaderTicks() - t0;
  }
};

inline std::mutex g_mutex;  // every hook body runs under it (creates and releases come from several threads, ADAPTER_CALLS section 7)
// S102 U1 Review F3: the reader lock for a hook a game thread enters (texture create / release / lock / flush): counts a wait that met a running finish (census only).
inline std::unique_lock<std::mutex> LockReaderCounted() {
  std::unique_lock<std::mutex> lock(g_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    if (rdr_adapter::tail::The().Finishing()) rdr_adapter::tail::The().CountMutexWait();
    lock.lock();
  }
  return lock;
}

// S100 speed: on a reader worker thread, notes go to the job and are recorded by the game thread when it joins the job (the census is
// the game thread's).
struct NoteCount {
  std::string name;
  uint64_t n = 0;
};
inline thread_local std::vector<NoteCount>* t_note_sink = nullptr;  // a run of one note (a note per index) stays one entry
inline rdr_adapter::draw_memos::NoteMemo g_note_memo;   // S102 T14 (wwe2k14_rdr_note_memo): the cells of the names counted before; invalidated where the window is cleared (CensusMaybe)
inline void NoteCounted(const char* name, uint64_t n) {
  rdr_adapter::draw_memos::CountNote(g_note_memo, g_census.window, g_census.seen, name, n, rdr_adapter::draw_memos::g_note_on.load(std::memory_order_relaxed),   // S102 T14 (the body is shared with the offline test)
                                     [&] { REXLOG_WARN("[rdr][census] first seen: {} (guest frame {})", name, wwe2k14::g_guest_frame.load(std::memory_order_relaxed)); });
}
inline void Note(const char* name) {
  if (t_note_sink) {
    if (!t_note_sink->empty() && t_note_sink->back().name == name) ++t_note_sink->back().n;
    else t_note_sink->push_back({name, 1});
    return;
  }
  NoteCounted(name, 1);
}
inline void Note(const char* name, uint32_t n) { Note((std::string(name) + "_" + std::to_string(n)).c_str()); }

// S101 names: per census window, how many draws carried a name (TakeDrawOrigin). Check A reads this line: untagged + unclassified is the
// unnamed count (RENDERER_ARC.md Check A: "0 or listed").
// (NamesCount / g_names live in names_draw.h since S101 lane 18, so the real draw-origin step can be tested without the reader.)
using rdr_adapter::names_draw::NamesCount;
using rdr_adapter::names_draw::g_names;
struct RoleCount {  // S101 names A1b: surface images given a role by their creator vs by the S100 16:9 rule (SurfaceRoleUsage)
  uint64_t declared = 0, guessed = 0;
};
inline RoleCount g_roles;
// S101 names A1c (texture provenance): each 4 KB page's last host writer (OnHostWriteBy, ProvenanceOf below).
struct ProvenanceState {  // (S101 lane17: the pages and the file names moved to prov_map.h, a flat lock-free array)
  std::mutex mu;  // guards by_kind and by_writer
  uint64_t by_kind[uint8_t(cap::Provenance::Count)] = {};  // per census window: texture binds labelled, by kind
  std::unordered_map<std::string, uint64_t> by_writer;      // S101 lane17: the same window, the CPU and GPU labels by their writer's name
};
inline ProvenanceState g_prov;
inline std::atomic<bool> g_prov_on{false};  // S101 lane17 review: provenance is being recorded (the hook is installed for it)
inline void OnHostWriteBy(uint32_t addr, uint32_t length, wwe2k14::host_write::Writer writer, const char* file, uint32_t source);
// Called from Wwe2k14App::OnPreSetup (after the owner block, before the game starts, so frame-0 reads are caught).
inline void InstallProvenanceAtStart() {
  bool dev_owner = false;
#if defined(WWE2K14_VERSION) && !defined(__ANDROID__)
  dev_owner = REXCVAR_GET(wwe2k14_rdr_owner) && std::string_view(WWE2K14_VERSION).find("+dev") != std::string_view::npos;  // (the case ring's rule, bookmark.h)
#endif
  // S102 U2 (R22 cut 1): an explicit --wwe2k14_rdr_provenance=false wins over the by-build enables (+dev owner, the case ring), as --wwe2k14_case=false does (bookmark.h): a +dev bench can match a player's build.
  const bool explicit_off = rex::cvar::GetFlagSource("wwe2k14_rdr_provenance") != rex::cvar::Source::kDefault && !REXCVAR_GET(wwe2k14_rdr_provenance);
  if (wwe2k14::prov::WantedAtStart(REXCVAR_GET(wwe2k14_rdr_provenance), REXCVAR_GET(wwe2k14_case), dev_owner, REXCVAR_GET(wwe2k14_rdr_capture_frames) > 0, explicit_off)) {
    g_prov_on.store(true, std::memory_order_relaxed);
    wwe2k14::prov::InstallWriterHook(&OnHostWriteBy);
  }
}
inline void ProvenanceLog(uint64_t from, uint64_t to);
inline void NamesLog(uint64_t from, uint64_t to) {
  NamesCount& n = g_names;
  const uint64_t slot15_hits = rdr_adapter::origin::g_slot15_hits.exchange(0), slot15_bound = rdr_adapter::origin::g_slot15_registered.exchange(0);  // every window, review 6
  if (!n.draws) { ProvenanceLog(from, to); return; }  // (the provenance counts of this window still close with it)
  std::string classes;
  for (uint8_t i = 1; i < uint8_t(cap::DrawClass::Count); ++i)
    if (n.by_class[i]) classes += (classes.empty() ? "" : ", ") + std::string(cap::DrawClassName(cap::DrawClass(i))) + " " + std::to_string(n.by_class[i]);
  REXLOG_INFO("[rdr][names] frames {}-{}: draws {}, untagged {} (stale {}), tagged but unclassified {}, origin table {} keys (overflow {}); by class: {}; "
              "surfaces so far: {} declared, {} guessed (16:9 rule), {} creates seen",
              from, to, n.draws, n.untagged, n.stale, n.unclassified, rdr_adapter::origin::Global().size(), rdr_adapter::origin::Global().overflow(),
              classes.empty() ? "none" : classes, g_roles.declared, g_roles.guessed, rdr_adapter::roles::Global().count());
  if (n.ui_draws) {  // S101 lane18: the 2D draws and what names their widget (Check A: "without widget" is listed by function)
    std::string without;
    for (const auto& [fn, c] : n.ui_no_element_by_fn) {
      char buf[48];
      std::snprintf(buf, sizeof(buf), "%08X x%llu", fn, static_cast<unsigned long long>(c));
      without += (without.empty() ? "" : ", ") + std::string(buf);
    }
    REXLOG_INFO("[rdr][names] frames {}-{}: 2D draws {}, with widget {} (element {}, label {}), without widget {} (named by screen only {}; element without text id {}; by function: {}); widget table {} entries (overflow {}); labels {} (overflow {}); text draws {} (run strings {}, truncated {}; run table {} entries, overflow {}); slot15 hits {}, bound {}", from, to,
                n.ui_draws, n.ui_named, n.ui_with_element, n.ui_with_label, n.ui_draws - n.ui_named, n.ui_screen_only, n.ui_element_no_text,
                without.empty() ? "none" : without, rdr_adapter::origin::Widgets().size(), rdr_adapter::origin::Widgets().overflow(),
                rdr_adapter::origin::Widgets().labels(), rdr_adapter::origin::Widgets().label_overflow(), n.by_class[uint8_t(cap::DrawClass::Text)], n.ui_runs, n.ui_runs_truncated, rdr_adapter::origin::Runs().size(), rdr_adapter::origin::Runs().overflow(), slot15_hits, slot15_bound);
  }
  ProvenanceLog(from, to);
  n = NamesCount{};
}

inline void ProvenanceLog(uint64_t from, uint64_t to) {
  if (!g_prov_on.load(std::memory_order_relaxed)) return;
  {
    std::lock_guard lock(g_prov.mu);
    REXLOG_INFO("[rdr][names] frames {}-{}: texture binds by provenance (per frame, summed): file {}, gpu {}, cpu {}, unnamed {}", from, to,
                g_prov.by_kind[uint8_t(cap::Provenance::File)], g_prov.by_kind[uint8_t(cap::Provenance::Gpu)],
                g_prov.by_kind[uint8_t(cap::Provenance::Cpu)], g_prov.by_kind[uint8_t(cap::Provenance::Unnamed)]);
    std::string by_writer;  // S101 lane17: which writer makes the CPU / GPU labels (a menu full of "cpu" says memcpy, unlock_flush, ...)
    for (const auto& [name, count] : g_prov.by_writer) by_writer += (by_writer.empty() ? "" : ", ") + name + " " + std::to_string(count);
    if (!by_writer.empty()) REXLOG_INFO("[rdr][names] frames {}-{}: cpu / gpu labels by writer: {}", from, to, by_writer);
    g_prov.by_writer.clear();
    {  // S101 lane17 (Check C): where memcpys into texture pages got their bytes, in this window (writes, not binds)
      const wwe2k14::prov::PageMap::Census c = wwe2k14::prov::Map().TakeCensus();
      std::string samples;
      for (uint32_t i = 0; i < c.samples; ++i) samples += std::string(i ? "; " : "") + "dst " + fmt::format("{:08X}", c.dst[i]) + " <- src " + fmt::format("{:08X}", c.src[i]) + " n=" + std::to_string(c.len[i]);
      REXLOG_INFO("[rdr][names] frames {}-{}: memcpys into physical pages (texture memory and other GPU-visible buffers) by source: loader-owned {}, no entry (heap: no loader bytes known; physical: never written) {}, "
                  "non-loader writer {}, copy of a no-entry copy {}, copy of a CPU copy {}; first unowned: {}", from, to, c.kind[0], c.kind[1], c.kind[2], c.kind[3], c.kind[4],
                  samples.empty() ? "none" : samples);
    }
    for (uint64_t& v : g_prov.by_kind) v = 0;
  }
}


inline rdr_adapter::periodic::Times g_periodic;  // lane 266: the periodic jobs' ms in this census window (printed by CensusMaybe, reset there)
inline double PeriodicMs(std::chrono::steady_clock::time_point t0) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }

inline void DrawMemosLog(uint64_t from, uint64_t to);   // S102 T14: defined after the shader tables (they need the shader info types)

inline census_flush::Site g_cf_census;   // S102 CEN1
inline void CensusMaybe() {
  Census& c = g_census;
  const uint64_t frame = wwe2k14::g_guest_frame.load(std::memory_order_relaxed);
  if (frame < c.window_start + 600 && !(census_flush::Due(g_cf_census) && frame > c.window_start)) return;   // S102 CEN1: a flip phase end closes the window now
  const auto census_t0 = std::chrono::steady_clock::now();  // lane 266: the whole block below, printed in the [rdr][periodic] line
  NamesLog(c.window_start, frame);
  rdr_scene::live::CensusLog(c.window_start, frame);  // S101 lane 26: the same window
  std::string cases;
  uint64_t total = 0;
  for (const auto& [name, n] : c.window) {
    total += n;
    cases += (cases.empty() ? "" : ", ") + name + " " + std::to_string(n);
  }
  REXLOG_INFO("[rdr][census] frames {}-{}: presents {}, draws {} (inline {}), clears {}, copies {}; cases {}{}", c.window_start, frame, c.presents, c.draws, c.inlines,
              c.clears, c.copies, total, total ? ": " + cases : "");
  CensusSourceLog(c.window_start, frame);
  DrawMemosLog(c.window_start, frame);   // S102 T14: the three memos' census lines (only while a switch is on)
  if (c.presents) {
    const double per = 1e6 * double(c.presents);
    const double per_t = per * TicksPerNs();  // the time fields are ticks
    const ReaderTimes& r = g_times;
    REXLOG_INFO("[rdr][time] frames {}-{}: reader ms a frame on the render thread: draws {:.2f} (shaders {:.2f}, targets {:.2f}, state {:.2f}, texture "
                "slots {:.2f} of which contents {:.2f}, constant banks {:.2f}, geometry {:.2f} of which builds {:.2f}), present {:.2f}, worker join {:.2f}; "
                "geometry a frame: {:.0f} memo hits, {:.0f} rebuilt, {:.0f} inline; {:.0f} built on the workers, {:.0f} draws took a pending build; "
                "draws a frame that took the last draw's state {:.0f}, bool/loop {:.0f}, literals {:.0f}; texture slots {:.0f}, {:.0f} as their last add; "
                "geometry memo {} entries, {} images, {:.0f} geometry ranges protected; the old path's writers after the reader: draws {:.2f} ms, clears and resolves {:.2f} ms; "
                "geometry sink appends {:.0f} a frame taking {:.3f} ms; live store lock, worst in the window: held by geometry read {:.2f}, append {:.2f}, drop {:.2f}, texture erase {:.2f} ms, an append waited {:.2f}, allocation {:.2f}, case-ring copy {:.2f} ms; state by the library's marks {:.0f} draws a frame (verify misses {}); movie planes so far: {} flushes, {} planes named, {} unmatched, "
                "verify {} reads, {} misses",
                c.window_start, frame, r.draw / per_t, r.shaders / per_t, r.targets / per_t, r.state / per_t, r.slots / per_t, r.content / per_t,
                r.banks / per_t, r.geometry / per_t, r.geo_build / per_t, r.present / per_t, r.join / per_t, r.geo_hits * 1e6 / per, r.geo_builds * 1e6 / per,
                r.geo_inline * 1e6 / per, r.geo_async * 1e6 / per, r.geo_attached * 1e6 / per, r.state_reused * 1e6 / per, r.bool_reused * 1e6 / per,
                r.lits_reused * 1e6 / per, r.slot_adds * 1e6 / per, r.slots_reused * 1e6 / per, r.memo_entries, r.images, r.geo_protects * 1e6 / per, r.old_draws / per_t, r.old_copies / per_t,
                r.sink_n * 1e6 / per, r.sink / per_t, g_lock_window_ns[0] * 1e-6, g_lock_window_ns[1] * 1e-6, g_lock_window_ns[2] * 1e-6, g_lock_window_ns[3] * 1e-6, g_lock_window_ns[4] * 1e-6, g_lock_window_ns[5] * 1e-6, g_lock_window_ns[6] * 1e-6, r.state_by_marks * 1e6 / per, r.marks_missed, g_planes.flushes, g_planes.named, g_planes.unmatched, g_planes.verified, g_planes.missed);
    if (g_fine_on) {
      const FineTimes& f = g_fine;
      REXLOG_INFO("[rdr][time][fine] frames {}-{}: ms a frame: vs lookup {:.3f}, ps lookup {:.3f}; banks: compare {:.3f}, literal tables {:.3f}, emit "
                  "{:.3f}; state: fixed decode {:.3f}, fixed dedupe {:.3f}, dynamic decode {:.3f}, dynamic dedupe {:.3f}; bool/loop {:.3f}, layout {:.3f}, "
                  "slot adds {:.3f}. A frame: {:.0f} draws, {:.0f} with the last draw's vs key, {:.0f} with its shader pair, {:.0f} banks reused whole, "
                  "{:.0f} literals read, {:.0f} bank blocks touched, {:.0f} texture slots, {:.0f} with the last draw's fixed state, {:.0f} with its "
                  "dynamic state. Write watches set: geometry {:.3f} ms ({:.0f} ranges), textures {:.3f} ms ({:.0f} textures)",
                  c.window_start, frame, f.vs_lookup / per_t, f.ps_lookup / per_t, f.bank_refresh / per_t, f.bank_lits / per_t, f.bank_emit / per_t,
                  f.fixed_decode / per_t, f.fixed_dedupe / per_t, f.dyn_decode / per_t, f.dyn_dedupe / per_t, f.bool_loop / per_t, f.layout / per_t,
                  f.slot_add / per_t, f.draws * 1e6 / per, f.same_vs_key * 1e6 / per, f.same_pair * 1e6 / per, f.banks_reused * 1e6 / per,
                  f.lits * 1e6 / per, f.touched * 1e6 / per, f.slots * 1e6 / per, f.same_fixed * 1e6 / per, f.same_dynamic * 1e6 / per,
                  f.geo_watch / per_t, f.geo_watch_calls * 1e6 / per, f.tex_watch / per_t, f.tex_watch_calls * 1e6 / per);
    }
  }
  {
    FineTimes keep = g_fine;  // the last draw's survive the window
    g_fine = {};
    g_fine.last_vs = keep.last_vs;
    g_fine.last_ps = keep.last_ps;
    g_fine.last_decl = keep.last_decl;
    g_fine.last_fixed = keep.last_fixed;
    g_fine.last_dynamic = keep.last_dynamic;
    std::memcpy(g_fine.last_strides, keep.last_strides, 24);
  }
  g_periodic.census.Add(PeriodicMs(census_t0));
  REXLOG_INFO("{}", g_periodic.Line(c.window_start, frame));  // lane 266 (the line's own formatting is not in the census ms: a few us)
  g_periodic.Reset();
  g_split_ticks.Bank(g_times.draw, g_times.present);  // S102 H3 F2: before the reset, so fs_reader_draw_ms / fs_reader_present_ms stay continuous
  g_times = {};
  std::memset(g_lock_window_ns, 0, sizeof(g_lock_window_ns));
  g_note_memo.Invalidate();   // S102 T14: the window map's nodes are gone
  c.window.clear();
  c.window_start = frame;
  c.draws = c.inlines = c.clears = c.copies = c.presents = 0;
}

// ---- memory ------------------------------------------------------------------------------------------------------------------------
inline const uint8_t* Phys(uint32_t gpu_address) { return REX_KERNEL_MEMORY()->TranslatePhysical<const uint8_t*>(gpu_address); }
inline uint32_t Swap32(uint32_t v) { return std::byteswap(v); }
inline uint32_t LoadBE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return std::byteswap(v);
}
inline float F(uint32_t bits) { return std::bit_cast<float>(bits); }
inline float Sat(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline uint32_t Log2Ceil(uint32_t v) { return v <= 1 ? 0 : 32 - uint32_t(std::countl_zero(v - 1)); }
inline uint32_t Log2Floor(uint32_t v) { return v ? 31 - uint32_t(std::countl_zero(v)) : 0; }
inline uint32_t NextPow2(uint32_t v) { return v <= 1 ? 1 : 1u << Log2Ceil(v); }
inline uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// Endian modes of a fetch constant or an index buffer: 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32.
inline uint16_t Swap16For(uint16_t v, uint32_t endian) { return (endian == 1 || endian == 2) ? std::byteswap(v) : v; }
inline uint32_t Swap32For(uint32_t v, uint32_t endian) {
  switch (endian) {
    case 1: return ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    case 2: return std::byteswap(v);
    case 3: return (v << 16) | (v >> 16);
    default: return v;
  }
}
