// See live_scene_draw.h. Session 103, lane S.
#include "live_scene_draw.h"
#include "rows_inputs.h"   // S106 G4-2
#include "rows_owned.h"
#include "load_order.h"   // S105 SKB
#include "../assets/car_sel.h"   // S103 CAR fix 7
#include "../thread_name.h"  // S103 H1

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <utility>

#include "post_from_record.h"   // S103 P
#include "live_particles.h"   // S103 PRT: HasSoft
#include "../renderer/problem.h"   // S103 FXF: rdr::Log
#include "../scene_live/post_settings.h"   // S103 FXF
#include "../assets/arena_lm.h"   // S103 B3c: the lm_param counters of the census
#include "../renderer_game/caw_portrait_core.h"   // S103 CPB: KeepsObject / DrawnCountOk
#include "../renderer_game/scene_frame.h"   // S103 S: the dev-only input override (g_force_inputs)
#include "../renderer/player/png.h"
#include "rf_crowd.h"   // S103 D2: the crowd hook of the class table
#include "../renderer_adapter/stage_mirror.h"   // S103 T7: WriteRaw (pure header)
#include "../renderer_adapter/rdr_switches.h"   // S103 LNT: kFxLog (no game or SDK includes)
#include "stage_census.h"   // S103 CPU1: the [host][cpu] census timers
#include "guest_model_draw.h"   // S103 DLF: guestmodel::Counters (roster signal)
#include "rf_c.h"   // S103 S (integration): lane C's arena seam (TakeArenaMisses / PutLoadedArena / FailArena / SetOfflineLoad / TakeRetired)
#include "model_load_report.h"   // S108 AL1: ReportLoadedModel / LoadCensus (the one reader of what the loader could not give)
#include "game_load_follow.h"   // S110 ENTRBLACK round 5: the arena the game just opened is asked for now
#include "scene_start_wait.h"   // S110 ENTRWAIT2: the game's scene start waits for these loads (PublishStartWait)
#include "start_bodies.h"   // S110 ENTRBODIES: the game's characters' bodies, asked while the start is held (AskStartBodies)

namespace rdr_scene {

LiveSceneDraw::LiveSceneDraw(rdr::Renderer& renderer) : renderer_(renderer), source_(std::make_unique<GameFilesSource>()) {
  loader_ = std::thread([this] { wwe2k14::NameThread(L"live_scene_load"); LoaderLoop(); });
#if !WWE2K14_STREAM()
  startwait::g_host_on.store(true, std::memory_order_relaxed);   // S110 ENTRWAIT2: the game's scene start may wait for this drawer's loads (scene_start_wait.h)
#endif
}

LiveSceneDraw::~LiveSceneDraw() {
  startwait::g_host_on.store(false, std::memory_order_relaxed);   // S110 ENTRWAIT2: no drawer, nothing for the game to wait for
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  if (loader_.joinable()) loader_.join();
  if (fe_) rf::crowd::Detach(*fe_);   // S103 D2: the crowd's models and images go before the front-end
  live_hooks_.reset();   // S103 R2: the hooks read the caches' CPU models: they go first
  mirror_hooks_.reset();
  live_cache_.reset();
  mirror_cache_.reset();
  fe_.reset();  // (the front-end's images and buffers go before the renderer; the caller waits for the GPU first)
}

void LiveSceneDraw::SetGameRoot(const std::string& root) {
  if (root.empty() || root == root_) return;
  root_ = root;
  source_->SetRoot(root);
  for (FrameHooks* h : {mirror_hooks_.get(), live_hooks_.get()})   // S103 B2F
    if (h && h->maps()) h->maps()->SetGameRoot(root);
  if (mirror_cache_) mirror_cache_->ClearFailed();   // (R review 3: a new game folder forgets the remembered failures)
  if (live_cache_) live_cache_->ClearFailed();
  {
    std::lock_guard<std::mutex> lock(mu_);
    loader_root_ = root;
  }
  cv_.notify_all();
}

void LiveSceneDraw::EnsureFrontEnd() {
  if (fe_) return;
  fe_ = std::make_unique<FrontEnd>(renderer_);
#if !WWE2K14_STREAM()
  fe_->SetScaledTargets(renderer_.render_scale() != 1.0f && !g_a1_plant_host_unscaled.load(std::memory_order_relaxed));   // S105 A1: the launcher's 1440p / 4K / 360p-648p choice draws the scene at that size (the scene targets carry kUsageScaled)
#endif
  rf::crowd::Attach(*fe_, /*async_bodies=*/true);   // S103 D2: LiveCrowd (lane D), its bodies loaded on a worker, never on the render thread
  lists_ = std::make_unique<DrawLists>();   // S103 G2 (SCENE_PACING.md 3c (10)): one per render thread
  fe_->SetDrawLists(lists_.get());
  mirror_cache_ = std::make_unique<rf::ModelCache>(*fe_, *source_, /*load_on_miss=*/true);
  live_cache_ = std::make_unique<rf::ModelCache>(*fe_, *source_, /*load_on_miss=*/false);
  // S103 R2: the stage hooks + the game's see-through order (frame_hooks.h), one FrameHooks per cache (the maps' casters read the cache's wrestler CPU models); made before the first Get
  FrameHooksOptions mirror_opts;
  mirror_opts.maps_input_repeatable = true;   // S103 E3 (review 10): the mirror draws through RenderToPixels, whose seed frame eats a once-only maps input
  mirror_hooks_ = std::make_unique<FrameHooks>(renderer_, *mirror_cache_, mirror_opts);
  live_hooks_ = std::make_unique<FrameHooks>(renderer_, *live_cache_);
  if (scene2d_fn_) live_hooks_->SetScene2D(scene2d_fn_);   // S103 SCS
  if (!root_.empty())   // S103 B2F: the rope proxy's arena pac is read from the game folder
    for (FrameHooks* h : {mirror_hooks_.get(), live_hooks_.get()})
      if (h->maps()) h->maps()->SetGameRoot(root_);
}

// The loader thread: loads what the live cache and lane C's arena seam missed, with its own source (asset_load.h is not shared between threads).
void LiveSceneDraw::LoaderLoop() {
  GameFilesSource src;
  std::string have_root;
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mu_);
      cv_.wait(lock, [&] { return stop_ || (!loader_root_.empty() && PickJob(want_, bulk_ok_) != kNoJob); });   // S105 SKB2: a bulk job waits for the roster (bulk_ok_)
      if (stop_) return;
      if (loader_root_ != have_root) have_root = loader_root_, src.SetRoot(have_root);
      const size_t at = PickJob(want_, bulk_ok_);   // S105 SKB: gating jobs first (load_order.h)
      job = std::move(want_[at]);
      want_.erase(want_.begin() + std::ptrdiff_t(at));
      if (!job.m.bulk) ++in_flight_;   // S110 ENTRWAIT2: a load the game's scene start waits for is running
    }
    Loaded l;
    l.m = job.m;
    l.arena = job.arena;
    if (job.arena >= 0 && assets::car::IsCustomId(job.arena)) l.car_key = assets::car::Current().Key();   // S103 CAR fix 7: the selection this load is made under (live_assets.cpp reads the same registry)
    if (job.arena >= 0) {
      char pac[32];
      std::snprintf(pac, sizeof pac, "pac\\bg\\bg%02d.pac", job.arena);
      l.ok = src.Load(pac, "", 0, l.model, l.error);
    } else {
      l.ok = src.Load(job.m.pac, job.m.entry, job.m.variant, l.model, l.error);
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (!l.m.bulk && in_flight_) --in_flight_;
    done_.push_back(std::move(l));
  }
}

// S110 ENTRWAIT2 (scene_start_wait.h): after each handled frame, tell the game's scene start whether our loads are done: nothing new asked by this frame's build (`built`), the arena the
// game opened handed to the loader, no non-bulk job queued, running or waiting to be adopted. A frame that built nothing (the host's stale-OURS / no-record frames) publishes the loader's
// state only, so a host that cannot learn more opens the wait by state. The bulk evtobj prewarm is not counted (it waits for the roster, which needs the scene).
void LiveSceneDraw::PublishStartWait(uint64_t guest_frame, bool built, bool drew) {
  const bool asked_bodies = AskStartBodies();   // S110 ENTRBODIES: queued before the queue is read below
  const bool crowd_in = StartCrowdIn(drew);      // S110 ENTRBODIES: the arena's crowd classes (the record carries the crowd only from the start's first frame)
  bool undelivered = false, queued = false;
  uint32_t running = 0;
  {
    std::lock_guard<std::mutex> lock(mu_);
    running = in_flight_, undelivered = !done_.empty();
    for (const Job& j : want_) queued = queued || !j.m.bulk;
  }
  const bool idle = crowd_in && startwait::HostIdle((built && asked_now_) || asked_bodies, loadfollow::g_game_arena.load(std::memory_order_acquire) >= 0, running, undelivered, queued);
  if (built) startwait::NoteHostBuild(guest_frame, idle);
  else startwait::NoteHostNoBuild(guest_frame, idle);
}

// S110 ENTRBODIES (start_bodies.h): the bodies the game's characters will need, asked now (the record names a body only once the game poses it, after the start): each goes to the loader under the
// key the record's Get looks up (ModelCache::Want), so the first frame that names it finds it resident. true = a body was queued by this call (this frame is not idle).
bool LiveSceneDraw::AskStartBodies() {
#if WWE2K14_STREAM()
  return false;
#else
  if (!live_cache_) return false;   // no front end yet: the board is taken by the first build that has one
  std::vector<startbodies::Body> bodies;
  if (!startbodies::TheBoard().TakeIfNew(start_bodies_seen_, bodies)) return false;
  bool any = false;
  for (const startbodies::Body& b : bodies) {
    rf::ModelCache::Missed m;
    if (!live_cache_->Want(b.pac, b.entry, b.variant, m)) continue;   // resident, failed or already asked
    roster_signal_.Ask(m.pac);
    NoteLoadLine(true, "pac " + m.pac + " entry " + m.entry + " variant " + std::to_string(m.variant) + " (gates; a body of the game's characters, asked while the start is held)", m.key, -1);
    std::lock_guard<std::mutex> lock(mu_);
    want_.push_back(Job{std::move(m), -1});
    any = true;
  }
  if (any) cv_.notify_all();
  return any;
#endif
}

// S110 ENTRBODIES: while a start is held (scene_start_wait.h StartPending), every crowd class of the arena the game opened is asked now (rf_crowd.h PrefetchArena): the record carries the crowd only
// from the start's first frame, and a Warm frame keeps READY false while any class is loading (rf_crowd.cpp ApplyCrowd). true = nothing to wait for: no start held, no front end or arena yet,
// a created arena (its crowd arena id is the record's), or every class resident or failed.
// Review: never on a frame about to draw (`drew`, DrawLive): a prefetch for another arena than the crowd holds releases the held arena's class models (LiveCrowd::SetArena -> Release), and DrawLive draws the
// frame its BuildFrame made, with those models, after this call. A drawing frame's own Prefetch (ApplyCrowd) already asked what its record needs.
bool LiveSceneDraw::StartCrowdIn(bool drew) {
#if WWE2K14_STREAM()
  (void)drew;
  return true;
#else
  if (drew || !fe_ || game_arena_ < 0 || assets::car::IsCustomId(game_arena_) || !startwait::StartPending()) return true;
  return rf::crowd::PrefetchArena(*fe_, game_arena_);
#endif
}

void LiveSceneDraw::SetBulkOk(bool v) {   // S105 SKB2
  { std::lock_guard<std::mutex> lock(mu_); if (bulk_ok_ == v) return; bulk_ok_ = v; }
  if (v) cv_.notify_all();
}

void LiveSceneDraw::QueueMisses() {
  {   // S110 ENTRBLACK round 5: the arena pac the game just opened is asked for now, with the game's own load, not when a record first names it
    const int ga = loadfollow::TakeGameArena();
    if (ga >= 0) rf::c::WantArenaFromGame(ga), game_arena_ = ga;   // S110 ENTRBODIES: + kept for the crowd's classes (StartCrowdIn)
  }
  std::vector<rf::ModelCache::Missed> missed = live_cache_->TakeMisses();
  const std::vector<int> arenas = rf::c::TakeArenaMisses();
  asked_now_ = !arenas.empty();   // S110 ENTRWAIT2: this build asked for something (PublishStartWait); the bulk prewarm is not counted
  for (const rf::ModelCache::Missed& m : missed) asked_now_ = asked_now_ || !m.bulk;
  if (missed.empty() && arenas.empty()) return;
#if !WWE2K14_STREAM()   // RRS: the stream build posts roster_ready / roster_failed from its roster worker (wrestler_ours.cpp)
  for (const rf::ModelCache::Missed& m : missed) roster_signal_.Ask(m.pac);
#endif
  for (const rf::ModelCache::Missed& m : missed)   // S103 OFFT: name each asset the first time it is asked for (render thread; the loader thread never touches this)
    NoteLoadLine(true, std::string("pac ") + m.pac + " entry " + m.entry + " variant " + std::to_string(m.variant) + (m.gates ? " (gates)" : " (prop)"), m.key, -1);
  if (!arenas.empty() && !prewarm_queued_) NoteLoadLine(true, std::string("pac pac\\evt\\evtobj.pac entry ") + kEvtobjPrewarmEntry + " (bulk: the entrance's block index, started at match load; S105 SKB3: ~25 ms, the pac is read by blocks)", "evtobj-prewarm", -1);
  for (int a : arenas) NoteLoadLine(true, "arena pac\\bg\\bg" + std::string(a < 10 ? "0" : "") + std::to_string(a) + ".pac", "arena " + std::to_string(a), a);
  {
    std::lock_guard<std::mutex> lock(mu_);
    for (rf::ModelCache::Missed& m : missed) want_.push_back(Job{std::move(m), -1});
    for (int a : arenas) want_.push_back(Job{rf::ModelCache::Missed{}, a});
    if (!arenas.empty() && !prewarm_queued_) {   // S105 SKB fix F3: a match / arena is loading: the entrance's 160 MB evtobj.pac unpack starts now, as the LAST job (bulk), so the first event prop's ask (a gating miss) finds it open: the ~1.7 s no longer lands inside the entrance
      rf::ModelCache::Missed pw;
      pw.pac = "pac\\evt\\evtobj.pac", pw.entry = kEvtobjPrewarmEntry, pw.key = "evtobj-prewarm", pw.stored = true, pw.gates = false, pw.bulk = true;
      prewarm_queued_ = true;
      want_.push_back(Job{std::move(pw), -1});
    }
  }
  cv_.notify_all();
}

// S103 OFFT: one log line per asked / adopted asset, throttled (the first 60 of each kind, then every 50th) and capped in memory. The wait of an asset is the time between the two lines.
void LiveSceneDraw::NoteLoadLine(bool ask, const std::string& what, const std::string& key, int arena) {
  (void)arena;
  // render-clock: ctl - wait / sleep pacing of the scene draw thread, no picture value
  const auto now = std::chrono::steady_clock::now();
  uint64_t& n = ask ? load_ask_lines_ : load_adopt_lines_;
  const uint64_t i = n++;
  if (ask) asked_at_[key] = now;
  std::string waited;
  if (!ask) {
    const auto it = asked_at_.find(key);
    if (it != asked_at_.end()) {
      waited = ", waited " + std::to_string((long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count()) + " ms";
      asked_at_.erase(it);
    }
  }
  constexpr uint64_t kFirst = 60, kEvery = 50;
  constexpr size_t kLoadLogCap = 256;
  if ((i < kFirst || i % kEvery == 0) && load_log_.size() < kLoadLogCap)
    load_log_.push_back(std::string("[scene][load] ") + (ask ? "ask #" : "adopted #") + std::to_string((unsigned long long)(i + 1)) + ": " + what + ", key " + key + waited);
}

void LiveSceneDraw::LiveLiquidImages(std::vector<LiveMapsHook::LiveLiquidImage>& out) const {   // S108 SHOT-A
  if (live_hooks_ && live_hooks_->maps()) live_hooks_->maps()->LiveLiquidImages(out);
}

void LiveSceneDraw::AdoptLoaded() {
  if (!live_cache_) return;
  std::vector<Loaded> done;
  {
    std::lock_guard<std::mutex> lock(mu_);
    done.swap(done_);
  }
  for (Loaded& l : done) {
    if (l.arena < 0 && l.m.bulk && l.m.entry == kEvtobjPrewarmEntry) {   // S105 SKB fix F3: the prewarm opened the pac in the loader; nothing to store. A failure is not re-asked until the next arena load.
      prewarm_queued_ = false;
      NoteLoadLine(false, std::string(l.ok ? "ok " : "FAILED ") + "pac " + l.m.pac + " entry " + l.m.entry + " (the entrance's unpack)", l.m.key, -1);
      continue;
    }
#if !WWE2K14_STREAM()
    if (l.arena < 0) roster_signal_.Done(l.m.pac, l.ok);   // RRS; S103 DLF: a failed body counts as drawn only when the guest rows built a model (Poll below)
#endif
    NoteLoadLine(false, std::string(l.ok ? "ok " : "FAILED ") + (l.arena >= 0 ? "arena " + std::to_string(l.arena) : "pac " + l.m.pac + " entry " + l.m.entry + " variant " + std::to_string(l.m.variant)),
                 l.arena >= 0 ? "arena " + std::to_string(l.arena) : l.m.key, l.arena);   // S103 OFFT: the arrival, named (the ask line above carries the same key)
    if (l.ok)   // S108 AL1: the ONE reader of what the loader could not give (model_load_report.h): dropped parts / an unopened texture pac -> draw_shortfall; textures not found -> a capped plain note
      ReportLoadedModel(l.arena >= 0 ? "arena " + std::to_string(l.arena) : l.m.pac + " " + l.m.entry + " variant " + std::to_string(l.m.variant), l.model);
    if (l.ok && l.arena >= 0) {   // lane C's arena: split into its family models on the next build
      rf::c::PutLoadedArena(l.arena, std::make_unique<assets::Model>(std::move(l.model)));
      ++loads_ok_;
    } else if (l.ok) {
      live_cache_->PutLoaded(l.m, l.model);
      ++loads_ok_;
    } else {
      ++loads_failed_;
      if (l.arena >= 0) rf::c::FailArena(l.arena, l.error, false, l.car_key);
      else live_cache_->MissFailed(l.m, l.error);   // (R review 3: remembered as failed, never re-queued until ClearFailed)
      if (first_load_error_.empty()) first_load_error_ = (l.arena >= 0 ? "arena " + std::to_string(l.arena) : l.m.key) + ": " + l.error;
    }
  }
  if (!done.empty())   // S108 AL1: the shortfall / note tally, only when it changed (a jump in textures not found is visible)
    if (const std::string c = LoadCensus(); !c.empty()) rdr::Log("%s", c.c_str());
#if !WWE2K14_STREAM()
  roster_signal_.Poll(guestmodel::Counters().actors_drawn.load(std::memory_order_relaxed));   // RRS: once per call (a guest frame); S103 DLF: failed bodies the guest rows drew count as guest
#endif
}

// (GPU idle) the front-end models the caches and lane C retired (an arena / roster change): FrontEnd::ReleaseModel needs an idle GPU.
void LiveSceneDraw::ReleaseRetiredIdle() {
  if (!fe_) return;
  if (mirror_cache_) mirror_cache_->ReleaseRetired();
  if (live_cache_) live_cache_->ReleaseRetired();
  for (int m : rf::c::TakeRetired()) fe_->ReleaseModel(m);
}

// S103 L31A: retired arena / roster models were only ever released by the mirror (DrawToPng): in live play every rebuild leaked a GPU set. FrontEnd::ReleaseModel only queues the
// renderer's own delayed destroys (kDestroyDelayFrames), so it needs no GPU wait; a model is still held two guest frames after its retirement (the frame that retired it, and the one before, are recorded).
void LiveSceneDraw::ReleaseRetiredLive(uint64_t guest_frame) {
  if (!fe_) return;
  if (live_cache_) live_cache_->ReleaseRetired();
  for (int m : rf::c::TakeRetired()) retired_held_.push_back({guest_frame, m});
  size_t keep = 0;
  for (const auto& h : retired_held_) {
    if (guest_frame >= h.first + 2) fe_->ReleaseModel(h.second);
    else retired_held_[keep++] = h;
  }
  retired_held_.resize(keep);
}

// S103 S: DEV ONLY: under --wwe2k14_rdr_scene_force_inputs (+dev builds) holes and missing post rows are counted but do not block READY (the integration test).
static bool DevForce() { return scene_frame::ForceInputs(); }
// S103 OFF holes (run 002, 2026-11-17): in the stream-free build nothing catches a refused frame, so an object we cannot draw (an own-skeleton piece, a screen with no picture) or a missing shadow input froze the
// picture for good (holes -> not ready -> Hold3D forever). Skip the object instead, as the forced A/B runs (7309) did, and count it (NoteTerms / the ready census still see the terms).
static bool SkipHoles() { return DevForce() || !WWE2K14_STREAM(); }

// S103 S (lane H review, contract): only a frame with a stage is ever ours (menus with a 3D preview, CAW, character select stay the game's).
// S103 L2d: that is the rule for a 3D (Scene3D) frame. A frame with NO stage, no 3D object and no Scene3D stamp is a FLAT frame (menus, select, title, boot, load: 2D list + movie +
// backdrop particles, claims_Q78 Q8), exempt from this test and from the 3D inputs: scene_kind::FlatVerdict decides it from FlatFacts3D below + the 2D drawer's census.
static bool HasStage(const std::vector<rf::ClassReport>& reports) {
  for (const rf::ClassReport& r : reports)
    if (r.family == rf::Family::Arena && r.objects > 0) return true;
  return false;
}

void LiveSceneDraw::NoteStage(const SceneTables& rec, bool built, const std::vector<rf::ClassReport>& reports) {
  stage_gf_ = rec.header.guest_frame;
  stage_ = !built || HasStage(reports);   // a frame not built is never judged flat (fail closed)
  uint32_t n = 0;
  for (const rf::ClassReport& r : reports) n += r.objects;
  stage_objects_ = n;
}

void LiveSceneDraw::FlatFacts3D(const SceneTables& rec, scene_kind::FlatFacts& f) const {
  f.scene3d_stamp = false;
  bool walked = false;   // S103 WB: a model-class hook (Model / Crowd / Ring) asked in this record's hook frame (scene_kind.h kWalkBit)
  for (const OwnedStamp& s : rec.owned_passes)
    if (s.pass == uint8_t(rdr_own::Pass::Scene3D))
#if !WWE2K14_STREAM()
      walked = walked || scene_kind::WalkOfPad(s.pad),
      f.scene3d_stamp = f.scene3d_stamp ||scene_kind::StampMakes3D(s.decision, s.why_not);   // S103 FLAT F1: a NOT READY stamp (stageless) from post / plane / ... on a sequence-9 page is no 3D walk
#else
      f.scene3d_stamp = true;
#endif
  if (rec.header.guest_frame != stage_gf_) {   // not the record this drawer built last: unknown = a 3D frame
    f.has_stage = true, f.objects3d = 1;
    return;
  }
  f.has_stage = stage_;
  f.objects3d = stage_objects_ + uint32_t(rec.objects.size());
#if !WWE2K14_STREAM()
  // S103 WB (PM_post_match_menu.md 4): the record keeps a sticky arena after a match; no model / crowd / ring walk this frame = no 3D scene on screen, so the 2D checks decide (a walk with a NOT READY stamp stays not flat).
  scene_kind::ApplyWalk(walked, f);
#else
  (void)walked;
#endif
}

void LiveSceneDraw::Warm(const SceneTables& rec, uint32_t w, uint32_t h) {
  EnsureFrontEnd();
  live_cache_->NewFrame();
  rf::crowd::SetSkipInPlace(*fe_, true);   // S103 SG2: a live-side build (the same rule as DrawLive)
  SceneFrame frame;
  std::vector<rf::ClassReport> reports;
  std::string error;
  const bool built = rf::BuildFrame(rec, *live_cache_, rf::kAllFamilies, w, h, frame, reports, error, rf::kBuildWarm);   // (D2 review 7: ask only, no posing / Put / objects)
  QueueMisses();
  PublishStartWait(rec.header.guest_frame);   // S110 ENTRWAIT2
  NoteStage(rec, built, reports);   // S103 L2d
  // READY (FRAME_CONTRACT 3 item 2): built, every asset resident, no hole (an object we cannot draw: R review 6), and a stage in the record
  // S103 NR (3): the SAME rule DrawLive applies (BuildTerms + the maps input's hole), so a frame READY here is not then refused there (the entrance light-set / follow-spot holes, the crowd's light holes
  // and the shadow input's hole used to be DrawLive-only).
  uint32_t terms = BuildTerms(built, reports);
  std::string shadow_why;
  // (S103 HN review: ON keeps its old form `terms == 0 && !SkipHoles()` so its census does not move; only the stream-free build asks on hole frames too, to name the skipped shadow input)
  const bool ask_shadow = WWE2K14_STREAM() ? (terms == 0 && !SkipHoles()) : ((terms & ~uint32_t(kTermHoles)) == 0);
  if (ask_shadow && live_hooks_->ShadowInputHole(rec, shadow_why)) terms |= kTermShadow, rc_.shadow_first = rc_.shadow_first.empty() ? shadow_why : rc_.shadow_first;
  uint32_t skipped = 0;   // S103 HN: in OFF a hole / a missing shadow input does not hold READY, but is still NAMED (the same census, counted as skipped)
  if (SkipHoles()) skipped = terms & uint32_t(kTermHoles | kTermShadow), terms &= ~skipped;
  ready_ = terms == 0;
  NoteTerms(/*live=*/false, terms, reports, error, skipped);
  if (ready_) {   // S103 P: a record whose post inputs are missing (no row, stale, no camera) is not ready: the game walks until they are there
    const PostParams* pp = nullptr;
    const char* why = "";
    if (BuildPost(rec, frame.camera, w, h, /*live=*/true, pp, why) == PostResult::Missing && !DevForce()) {
      ready_ = false;
      ++rc_.post_missing, rc_.post_first = rc_.post_first.empty() ? why : rc_.post_first;
      --rc_.ready;   // (NoteTerms counted this call ready)
    }
  }
}

// S103 NR (3): the terms of the not-ready rule that come from the build (shared by Warm and DrawLive). Holes are ignored under DevForce by the caller.
uint32_t LiveSceneDraw::BuildTerms(bool built, const std::vector<rf::ClassReport>& reports) const {
  if (!built) return kTermNotBuilt | (live_cache_->misses_this_frame() ? kTermMisses : 0u);
  uint32_t t = 0;
  if (live_cache_->misses_this_frame() != 0) t |= kTermMisses;
  if (rf::TotalHoles(reports) != 0) t |= kTermHoles;
  if (!HasStage(reports)) t |= kTermNoStage;
  return t;
}

// S103 NR (1): the census. `error` = the build's error text (not built).
void LiveSceneDraw::NoteTerms(bool live, uint32_t terms, const std::vector<rf::ClassReport>& reports, const std::string& error, uint32_t skipped) {
  ++rc_.calls, ++(live ? rc_.live : rc_.warm);
  if (terms == 0) ++rc_.ready;
  if (terms & kTermNotBuilt) {
    ++rc_.not_built;
    if (rc_.not_built_first.empty()) rc_.not_built_first = error;
  }
  if (terms & kTermMisses) ++rc_.misses, rc_.miss_assets += live_cache_->misses_this_frame();
  terms |= skipped;   // S103 HN: skipped terms (OFF) are named like the blocking ones: they just did not hold `ready` (decided by the caller above)
  if (terms & kTermHoles) {
    ++rc_.holes;
    for (const rf::ClassReport& r : reports)
      if (r.holes && size_t(r.family) < size_t(rf::Family::Count)) {
        rc_.hole_family[size_t(r.family)] += r.holes;
        if (rc_.hole_first[size_t(r.family)].empty()) rc_.hole_first[size_t(r.family)] = r.hole_why.empty() ? r.why : r.hole_why;   // S103 NRBF F5
      }
  }
  if (terms & kTermNoStage) ++rc_.no_stage;
  if (terms & kTermShadow) ++rc_.shadow;
}

std::string LiveSceneDraw::ReadyCensusLine(bool drain) {
  const ReadyCensus& c = rc_;
  const auto u = [](uint64_t v) { return std::to_string((unsigned long long)v); };
  std::string s = "[scene][ready] calls " + u(c.calls) + " (warm " + u(c.warm) + ", live " + u(c.live) + "), ready " + u(c.ready) + "; not built " + u(c.not_built) + (c.not_built_first.empty() ? "" : " (first: " + c.not_built_first + ")") +
                  "; misses " + u(c.misses) + " (assets asked " + u(c.miss_assets) + "); holes " + u(c.holes) + (SkipHoles() ? " skipped" : "") + " [";
  bool any = false;
  for (size_t f = 0; f < size_t(rf::Family::Count); ++f)
    if (c.hole_family[f]) s += std::string(any ? ", " : "") + rf::FamilyName(rf::Family(f)) + " " + u(c.hole_family[f]) + " (first: " + c.hole_first[f] + ")", any = true;
  s += std::string(any ? "" : "-") + "]; no stage " + u(c.no_stage) + "; shadow input " + u(c.shadow) + (SkipHoles() ? " skipped" : "") + (c.shadow_first.empty() ? "" : " (" + c.shadow_first + ")") + "; post missing " + u(c.post_missing) +
       (c.post_first.empty() ? "" : " (" + c.post_first + ")") + "; draw error " + u(c.draw_error) + (c.draw_first.empty() ? "" : " (first: " + c.draw_first + ")");
  if (drain) rc_ = ReadyCensus();
  return s;
}

// S103 P: our post pass's parameters from the record's post row (post_from_record.h). Post = params filled; NoPost = the frame genuinely has none (module off / no effect on): drawn without post,
// as the game's would be; Missing = an input is missing (no row, S unreadable, a stale row, no perspective camera): a live frame is then a miss and READY stays off.
// WWE_RDR_NO_SCENE_POST=1: off (dev A/B; always NoPost).
// S103 FXF: the afterimage / tint census, once per record frame. --wwe2k14_rdr_fx_log=true prints one line per frame: the guest frame and the post row's words that decide the effect (S+0, S+4, S+8..+32, S+224, S+228, S+208) with the gGsf code;
// join it with the frame-fate lines ([scene][ready], repeat_3d{reason}) by frame number: the flicker question (FXP.md section 5).
void LiveSceneDraw::NoteFx(const SceneTables& rec, const post_from_record::Info& pi) {
  const uint64_t gf = rec.header.guest_frame;
  if (gf == fx_last_frame_) return;
  fx_last_frame_ = gf;
  ++fx_frames_;
  if (pi.afterimage) ++fx_ghost_, ++(pi.blend == 1 ? fx_ghost_additive_ : fx_ghost_alpha_);
  if (pi.film_dropped_sepia) ++fx_sepia_dropped_;
  if (pi.film_kept_tint) ++fx_tint_kept_;
  if (pi.afterimage && pi.disp_default) ++fx_disp_default_;
  // S103 PST: tonemap / night / antialiasing frames, and the halation / flare / radial-blur words (COV9 2f: no writer known; a non-zero count names the effect that fired and the word).
  if (pi.tonemap) ++fx_tonemap_;
  if (pi.night) ++fx_night_;
  if (pi.aa) ++fx_aa_;
  if (pi.aa_skipped) ++fx_aa_skipped_;
  if (pi.defocus) ++fx_defocus_;   // S103 FXH: the highlight-reel cue effects (a run that plays a cue names them in the census)
  if (pi.emboss) ++fx_emboss_;
  if (pi.retro) ++fx_retro_;
  if (pi.retro_off_setting) ++fx_retro_setting_off_;
  if (pi.cue_garbage) ++fx_cue_garbage_;
  if (pi.dead_flags & kPpHala300) ++fx_hala300_;
  if (pi.dead_flags & kPpHalation) ++fx_hala304_;   // S103 HCN: halation RUNS (the game's gate)
  if (pi.dead_flags & kPpRadial) ++fx_radial_;
  const bool log_fx = rdr_sw::BoolSeen(rdr_sw::kFxLog, false, rec.header.guest_frame);   // S103 LNT: wwe2k14_rdr_fx_log (was env WWE_RDR_FX_LOG); the sink thread's own diagnostic log
  if (log_fx && !rec.post_params.empty()) {
    const PostParamsRow& r = rec.post_params[0];
    const auto f = [&r](uint32_t off) { return post_settings::Float(r, off); };
    rdr::Log("[scene][fx] frame %llu on %u dof %u depth %.5g %.5g %.5g %.5g %.5g %.5g | after %u alpha %.4g c %.4g %.4g s %.4g blend %u | mono %u gsf %u",
             (unsigned long long)gf, post_settings::Word(r, 0), post_settings::Word(r, 4), f(8), f(12), f(16), f(20), f(24), f(28), post_settings::Word(r, post_settings::kAfterOn), f(post_settings::kAfterAlpha),
             f(post_settings::kAfterX), f(post_settings::kAfterY), f(post_settings::kAfterScale), post_settings::Word(r, post_settings::kAfterBlend), post_settings::Word(r, post_settings::kMonotone), r.gsf_code);
  }
}

LiveSceneDraw::PostResult LiveSceneDraw::BuildPost(const SceneTables& rec, const Camera& cam, uint32_t w, uint32_t h, bool live, const PostParams*& out, const char*& why) {
  static const bool no_scene_post = std::getenv("WWE_RDR_NO_SCENE_POST") != nullptr;
  out = nullptr;
  if (no_scene_post) return PostResult::NoPost;
  post_from_record::Info pi;
  const bool cue_post = rdr_sw::BoolSeen(rdr_sw::kCuePost, true, rec.header.guest_frame);   // S103 FXH: wwe2k14_rdr_cue_post (default ON), read once per guest frame by the sink thread
  if (post_from_record::Build(rec, cam, w, h, post_store_, pi, cue_post)) {
    out = &post_store_;
    ++(live ? post_live_ : post_mirror_);
    if (pi.unsupported) ++post_unsupported_;
    if (pi.strength_default) ++post_strength_default_;
    NoteFx(rec, pi);
    return PostResult::Post;
  }
  if (pi.row && pi.read) NoteFx(rec, pi);
  why = post_last_why_ = pi.why;
  if (pi.missing) { if (live) ++post_live_missing_; return PostResult::Missing; }
  ++(live ? post_live_none_ : post_mirror_none_);
  return PostResult::NoPost;
}

bool LiveSceneDraw::DrawLive(const SceneTables& rec, rdr::FrameBuilder& fb, uint32_t w, uint32_t h, const PostParams* post, rdr::ImageId& out,
                             std::string& why, rdr::ImageId* out_depth, bool* loading) {
  EnsureFrontEnd();
  ++live_frames_;
  if (loading) *loading = false;
  out = rdr::kNoImage;
  fault_scene_drawn_ = false;   // S103 CHS
  live_cache_->NewFrame();
  rf::crowd::SetSkipInPlace(*fe_, true);   // S103 SG2: the stream draws a walked frame's signs in place: the rows flagged kCpfInPlace are not drawn twice
  SceneFrame frame;
  std::vector<rf::ClassReport> reports;
  std::string error;
  bool built = false;
  {
    StageTimer tm(StageCensusRef().build_ns, StageCensusRef().build_n);   // S103 CPU1: the [host][cpu] census
    built = rf::BuildFrame(rec, *live_cache_, rf::kAllFamilies, w, h, frame, reports, error);
  }
  QueueMisses();
  PublishStartWait(rec.header.guest_frame, true, true);   // S110 ENTRWAIT2 (review: drew = true, this frame is drawn below: no crowd arena switch here)
  {   // S103 SCX (SCG.md): another arena than the last built frame's = another scene: the grab is forgotten. A gap in drawn frames is not (the game never blanks its grab)
    const uint64_t ep = rf::c::ArenaEpoch();
    LiveScreenFacts lf;
    lf.arena_changed = ep != arena_epoch_seen_;
    arena_epoch_seen_ = ep;
    if (lf.arena_changed) { live_cache_->SetSceneDrawn(false); SetBulkOk(false); }   // S105 SKB fix F3: a new scene: its event / structure props gate READY again until its first frame is drawn
    if (const LiveScreenWhy w = LiveScreenInvalidation(lf); w != LiveScreenWhy::None) fe_->InvalidateLiveScreen(w);
  }
  NoteStage(rec, built, reports);   // S103 L2d
  const uint32_t holes = built ? rf::TotalHoles(reports) : 0u;
  uint32_t terms = BuildTerms(built, reports);   // S103 NR (3): the rule Warm applies too
  uint32_t skipped = 0;   // S103 HN: the holes OFF skips: still counted by NoteTerms (class + first reason)
  if (SkipHoles()) skipped = terms & uint32_t(kTermHoles), terms &= ~skipped;
  ready_ = terms == 0;
  // S103 L31A: a LOADING miss = any asset the frame asked for that is not resident (the cache's Get misses, the crowd's bodies, the arena Model: ApplyArenaLive notes it). Decided on the count BEFORE
  // `built`: a frame the arena class refused is "not built" and still a loading miss (run 30: the text was "class arena could not build the frame", never "not resident").
  if (loading) *loading = live_cache_->misses_this_frame() > 0;
  if (!built) { why = "frame not built: " + error; ++live_misses_; NoteTerms(true, terms, reports, error, skipped); return false; }
  if (live_cache_->misses_this_frame()) { why = "not resident: " + std::to_string(live_cache_->misses_this_frame()) + " asset(s) loading"; ++live_misses_; NoteTerms(true, terms, reports, error, skipped); return false; }
  if (holes && !SkipHoles()) { why = std::to_string(holes) + " object(s) we cannot draw (holes)"; ++live_misses_; NoteTerms(true, terms, reports, error, skipped); return false; }
  // S103 ONF review M2: a stageless record (a scene change: READY lags a record) is a miss, not wrestlers on a cleared background (in ON it walked: arena_worlds unstamped while Stage() == 0).
  if ((terms & kTermNoStage) && !DevForce()) { why = "no stage in the record (scene change)"; { LiveScreenFacts lf; lf.stageless_record = true; fe_->InvalidateLiveScreen(LiveScreenInvalidation(lf)); } /* S103 SCX */ ++live_misses_; NoteTerms(true, terms, reports, error, skipped); return false; }
  if (!post) {   // S103 P: our post from the record's post row; NoPost = drawn without post (not a miss), Missing = a miss
    const char* pwhy = "";
    if (BuildPost(rec, frame.camera, w, h, /*live=*/true, post, pwhy) == PostResult::Missing && !DevForce()) {
      why = std::string("post inputs missing: ") + pwhy;
      ++live_misses_;
      ready_ = false;
      NoteTerms(true, terms | kTermOther, reports, error, skipped);
      ++rc_.post_missing, rc_.post_first = rc_.post_first.empty() ? std::string(pwhy) : rc_.post_first;
      return false;
    }
  }
  AttachFrameHooks(frame, rec, *live_hooks_);   // S103 R2: SeeOrder::Game, the maps hook, the particle hook (one function, the rig calls it too)
  if (live_hooks_->input().shadow_hole && !SkipHoles()) {   // S103 E3 (review 2): a missing map input is a missing input: the game walks this frame (the casters' circles / plane / pre-light / sweat holes are named in the census, not folded: their inputs are not in any record yet)
    why = std::string("maps: ") + live_hooks_->input().fit_key_why;   // (the same rule Warm asks: FrameHooks::ShadowInputHole)
    ++live_misses_;
    ready_ = false;
    terms |= kTermShadow;
    if (rc_.shadow_first.empty()) rc_.shadow_first = live_hooks_->input().fit_key_why;   // S103 NRBF F5: the FIRST reason is kept
    NoteTerms(true, terms, reports, error, skipped);
    return false;
  }
  if (live_hooks_->input().shadow_hole && SkipHoles()) {   // S103 HN: OFF: the maps hook cannot make the wrestlers' shadow (RecordShadow: no key light = a counted hole, no map published), the readers take their plain variants (front_end.cpp: no ShadowMap = no shadow, never garbage): the frame is drawn without that shadow, and counted
    skipped |= kTermShadow;
    if (rc_.shadow_first.empty()) rc_.shadow_first = live_hooks_->input().fit_key_why;
  }
  frame.post = post;
  const LiveTargets t = fe_->OwnTargets(w, h, post != nullptr || !frame.flashes.empty(), LiveParticles::HasSoft(frame.particles));   // S103 PRT: a soft particle batch copies the scene depth: the depth must be stored (not transient); the post output is not made for it   // S110 CUTGREY: a frame with flash sprites needs the scene depth sampled (the whole-sprite centre test)
  lists_->Tick(rec.header.guest_frame);   // S103 G2: the grace counts guest frames
  fault_color_ = t.color;
  bool recorded = false;
  {
    StageTimer tm(StageCensusRef().record_ns, StageCensusRef().record_n);   // S103 CPU1
    recorded = fe_->Record(frame, fb, t, error);
  }
  if (!recorded) {
    fault_scene_drawn_ = error != "rdr: Record without targets";   // S103 CHS: that one returns before any op
    why = "draw: " + error;
    ++live_misses_;
    ready_ = false;
    NoteTerms(true, terms | kTermOther, reports, error, skipped);
    ++rc_.draw_error;
    if (rc_.draw_first.empty()) rc_.draw_first = error;
    return false;
  }
  NoteTerms(true, terms, reports, error, skipped);
  last_drawn_gf_ = rec.header.guest_frame, have_drawn_gf_ = true;   // (diagnostics only; no longer a live-screen rule)
  live_cache_->SetSceneDrawn(true); SetBulkOk(true);   // S105 SKB2: the roster is in: the evtobj prewarm may start now
   // S105 SKB fix F3: the scene has shown its first OURS frame: event / structure props no longer gate READY (a mid-match ask pops in)
  out = post ? fe_->post_output() : t.color;
  if (out_depth) *out_depth = t.depth;
  for (const rf::ClassReport& r : reports) drawn_ += r.drawn, skipped_ += r.skipped;
  return true;
}

bool LiveSceneDraw::DrawToPng(const SceneTables& rec, uint32_t w, uint32_t h, const std::vector<uint8_t>& curve, const std::filesystem::path& png,
                              std::string& report) {
  const std::filesystem::path dump_dir = std::exchange(map_dump_dir_, std::filesystem::path());   // S103 T7 F3: taken at once, so no early return leaves a stale dump tag for the next frame
  const uint64_t dump_frame = std::exchange(map_dump_frame_, uint64_t(0));
  if (!source_->ready()) { report = "[scene][mirror] no game folder yet"; ++failed_; return false; }
  EnsureFrontEnd();
  lists_->Tick(rec.header.guest_frame);   // S103 G2 (review 5): the mirror ticks the lists as the live drawer does
  mirror_cache_->NewFrame();
  rf::crowd::SetSkipInPlace(*fe_, false);   // S103 SG2: the mirror draws every row (it is the picture of the record alone)
  SceneFrame frame;
  std::vector<rf::ClassReport> reports;
  std::string error;
  rf::c::SetOfflineLoad(true);   // mirror (dev, waits anyway): lane C loads the arena here, on this thread
  bool built = rf::BuildFrame(rec, *mirror_cache_, rf::kAllFamilies, w, h, frame, reports, error);
  // S103 D2: the crowd's bodies load on a worker: a dev sheet waits for them (up to ~3 s), else its line says the crowd is missing
  for (int tries = 0; built && mirror_cache_->misses_this_frame() > 0 && tries < 600; ++tries) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    mirror_cache_->NewFrame();
    built = rf::BuildFrame(rec, *mirror_cache_, rf::kAllFamilies, w, h, frame, reports, error);
  }
  const bool crowd_missing = built && mirror_cache_->misses_this_frame() > 0;
  rf::c::SetOfflineLoad(false);
  if (!built) {
    report = "[scene][mirror] frame not built: " + error;
    ++failed_;
    return false;
  }
  {  // S103 P: the mirror sheet shows our post too (the game's picture has its own); a missing input = no post on the sheet
    const char* pwhy = "";
    BuildPost(rec, frame.camera, w, h, /*live=*/false, frame.post, pwhy);
  }
  AttachFrameHooks(frame, rec, *mirror_hooks_);   // S103 R2: the same hooks + see order as the live frame
  std::vector<uint8_t> rgba;
  uint32_t pw = 0, ph = 0;   // S105 A1: a scaled scene reads back at its real size
  if (!fe_->RenderToPixels(frame, rgba, error, &pw, &ph)) {
    report = "[scene][mirror] draw failed: " + error;
    ++failed_;
    return false;
  }
  if (!dump_dir.empty()) DumpMaps(frame.maps, dump_dir, dump_frame);   // S103 T7: the stage mirror (off: the path is empty and nothing runs)
  ReleaseRetiredIdle();   // (RenderToPixels waited for the GPU)
  if (curve.size() == 1024)
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
      for (uint32_t c = 0; c < 3; ++c) rgba[i + c] = curve[4 * rgba[i + c] + c];
  std::error_code ec;
  std::filesystem::create_directories(png.parent_path(), ec);
  if (!rdr::WritePng(png, rgba.data(), pw, ph)) {  // (WritePngFile closes the file)
    report = "[scene][mirror] cannot write " + png.string();
    ++failed_;
    return false;
  }
  ++sheets_;
  uint32_t d = 0, s = 0;
  std::string line = "[scene][mirror] " + png.filename().string() + ":";
  std::string reasons;
  for (const rf::ClassReport& r : reports) {
    d += r.drawn, s += r.skipped;
    char b[96];
    std::snprintf(b, sizeof b, " %s %u/%u", rf::FamilyName(r.family), r.drawn, r.objects);
    line += b;
    if (r.skipped && reasons.size() < 300) reasons += std::string(" | ") + rf::FamilyName(r.family) + " skipped " + std::to_string(r.skipped) + ": " + r.why;
  }
  drawn_ += d, skipped_ += s;
  line += ", validation errors " + std::to_string(fe_->validation_errors()) + reasons + (crowd_missing ? " | CROWD MISSING: bodies still loading after 3 s" : "");
  report = line;
  return true;
}

// S103 CPB (CPR.md H1): see live_scene_draw.h. Host thread, before the frame's builder begins (RenderToPixels records and waits on its own builder).
bool LiveSceneDraw::DrawPortrait(const SceneTables& rec, const Camera& cam, uint32_t model, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba, std::string& why, bool* loading) {
  if (loading) *loading = false;
  if (!source_->ready()) { why = "no game folder yet"; return false; }
  if (!model || rec.engine_objects.size() < rec.objects.size()) { why = "the record has no engine-object column"; return false; }
  bool found = false;
  for (uint32_t e : rec.engine_objects) found = found || wwe2k14::caw_portrait::KeepsObject(e, model);
  if (!found) { why = "the model is not in the record's engine objects"; return false; }
  EnsureFrontEnd();
  SceneTables t = rec;   // a copy of the record: the game's portrait camera in place of the frame's, and only the edited wrestler left visible to the Wrestlers family
  t.camera = cam, t.has_camera = true;
  const uint32_t hidden = t.AddString("hidden:cpb");   // any object not named by the engine address gets an id of another family: the mask below leaves it out
  for (size_t i = 0; i < t.objects.size(); ++i)
    if (!wwe2k14::caw_portrait::KeepsObject(t.engine_objects[i], model)) t.objects[i].id = hidden;
  live_cache_->NewFrame();
  rf::crowd::SetSkipInPlace(*fe_, false);
  SceneFrame frame;
  std::vector<rf::ClassReport> reports;
  std::string error;
  const bool built = rf::BuildFrame(t, *live_cache_, 1u << uint32_t(rf::Family::Wrestlers), w, h, frame, reports, error);
  QueueMisses();
  if (!built) { why = "frame not built: " + error; return false; }
  if (live_cache_->misses_this_frame()) {
    why = "not resident: " + std::to_string(live_cache_->misses_this_frame()) + " asset(s) loading";
    if (loading) *loading = true;
    return false;
  }
  uint32_t drawn = 0;
  for (const rf::ClassReport& r : reports)
    if (r.family == rf::Family::Wrestlers) drawn += r.drawn;
  if (!wwe2k14::caw_portrait::DrawnCountOk(drawn)) { why = "drawn " + std::to_string(drawn) + " wrestler object(s), not exactly the edited one (" + std::to_string(rf::TotalHoles(reports)) + " hole(s))"; return false; }
  frame.post = nullptr;
  frame.particles = nullptr;   // no particles in the portrait: HasSoft must not make the live frame's depth stored for good (want_depth_stored_ is sticky)
  frame.clear[0] = frame.clear[1] = frame.clear[2] = frame.clear[3] = 0.f;   // transparent black: the portrait is a cut-out (CPR section 4)
  // Our own image pair, not the front end's live targets (those would be remade at this size and again at the live frame's: the host's held picture would be lost). Freed after the request: 736x1280 x (4 + 4) bytes
  // is ~7.5 MB, a one-off menu event, so it is not kept resident (a phone's memory is the scarcer one); making it again costs two CreateImage calls.
  rdr::ImageDesc cd;
  cd.width = uint16_t(w), cd.height = uint16_t(h), cd.format = rdr::Format::RGBA8, cd.usage = rdr::kUsageTarget | rdr::kUsageSampled;
  rdr::ImageDesc dd = cd;
  dd.format = rdr::Format::Depth, dd.usage = rdr::kUsageTarget | rdr::kUsageTransient;
  const rdr::ImageId color = renderer_.CreateImage(cd), depth = renderer_.CreateImage(dd);
  bool ok = color != rdr::kNoImage && depth != rdr::kNoImage;
  if (!ok) why = "could not create the portrait's images";
  if (ok) {
    LiveTargets tg;
    tg.color = color, tg.depth = depth, tg.width = w, tg.height = h;
    rdr::FrameBuilder fb;
    fb.Begin(0);
    if (!fe_->Record(frame, fb, tg, error)) {
      why = "draw failed: " + error;
      ok = false;
    } else {
      renderer_.RenderFrame(fb.Finish());
      renderer_.WaitIdle();
      uint32_t rw = 0, rh = 0;
      rdr::ImageRef ref;
      ref.image = color;
      rgba.clear();
      if (!renderer_.ReadImage(ref, rgba, rw, rh) || rw != w || rh != h) { why = "could not read the portrait back"; ok = false; }
    }
  }
  if (color != rdr::kNoImage) renderer_.DestroyImage(color);
  if (depth != rdr::kNoImage) renderer_.DestroyImage(depth);
  if (ok) ReleaseRetiredIdle();   // (the GPU was waited for)
  return ok;
}

// S103 T7: OUR maps by role, as the game side's names them (stage_mirror.h kTechRoles): ours_<role>.<n>_f<frame>.rgba8 (u32 w, u32 h, RGBA8) + .png, and manifest_ours_f<frame>.txt (one tab line per map:
// "ours role n WxH raw p0 p1 p2 p3"). A role the drawer did not publish has no line (stage_mirror.py lists it as unpaired: a finding). Waits on the GPU (ReadImage; dev only).
void LiveSceneDraw::DumpMaps(const LiveMaps* maps, const std::filesystem::path& dir, uint64_t frame) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string tag = "f" + std::to_string(frame);
  std::string manifest;
  uint32_t count_by_role[8] = {};
  if (!maps) manifest = "# no LiveMaps on this frame (the drawer built none)\n";
  else if (maps->maps.empty()) manifest = "# LiveMaps is empty on this frame (no map made; see the census line)\n";
  else
    for (const MapImage& m : maps->maps) {
      const char* role = "";
      switch (m.role) {
        case MapRole::ShadowMap: role = "shadow_map"; break;
        case MapRole::PlaneShadow: role = "plane_mask"; break;
        case MapRole::Circles: role = "circles"; break;
        case MapRole::PropsMap: role = "props_map"; break;   // S103 PMS
        case MapRole::RimMap: role = "rim_map"; break;
        case MapRole::PreLightCube: role = "prelight_strip"; break;
        case MapRole::SweatMap: role = "sweat_map"; break;
        default: continue;
      }
      const uint32_t n = count_by_role[uint32_t(m.role) & 7]++;
      std::vector<uint8_t> px;
      uint32_t w = 0, h = 0;
      rdr::ImageRef ref;
      ref.image = m.image;
      if (m.image == rdr::kNoImage || !renderer_.ReadImage(ref, px, w, h) || !w || !h) {
        manifest += std::string("# ours ") + role + "." + std::to_string(n) + ": could not be read back\n";
        continue;
      }
      const std::string stem = std::string("ours_") + role + "." + std::to_string(n) + "_" + tag;
      std::filesystem::path raw = dir / stem, png = dir / stem;
      raw += ".rgba8";
      png += ".png";
      const bool ok_raw = rdr_adapter::stage_mirror::WriteRaw(raw, px, w, h);
      const bool ok_png = rdr::WritePng(png, px.data(), w, h);
      char line[256];
      std::snprintf(line, sizeof line, "ours\t%s\t%u\t%ux%u\t%d\t%g\t%g\t%g\t%g\t%s\n", role, n, w, h, m.raw ? 1 : 0, double(m.params[0]), double(m.params[1]), double(m.params[2]), double(m.params[3]),
                    ok_raw && ok_png ? "written" : "WRITE FAILED");
      manifest += line;
    }
  rdr_adapter::stage_mirror::WriteText(dir / ("manifest_ours_" + tag + ".txt"), manifest);
}

std::string LiveSceneDraw::TexUpLine() const {   // S103 UP
  if (!fe_) return std::string();
  const auto u = [](uint64_t v) { return std::to_string((unsigned long long)v); };
  const auto part = [&](const char* name, TexTag t) { return std::string(name) + " " + u(fe_->texture_update_count(t)) + " / " + u(fe_->texture_update_bytes(t) >> 10) + " KB"; };
  return "[scene][texup] live texture uploads since start: " + part("page", TexTag::Page) + ", " + part("screen", TexTag::Screen) + ", " + part("other", TexTag::Other) + "; page sends saved: key " + u(fe_->page_skips_key()) +
         ", same bytes " + u(fe_->page_skips_hash()) + "; models on the shared page " + std::to_string(fe_->page_share_models()) + "; page groups " + std::to_string(fe_->page_groups()) +
         "; screens (S103 UPL U2, share " + (g_screen_share.load(std::memory_order_relaxed) ? "ON" : "off") + "): pictures asked " + u(fe_->screen_pics()) + ", served with no upload " + u(fe_->screen_shared()) + ", rebinds " + u(fe_->screen_moved()) +
         ", images " + u(fe_->screen_images()) + ", batches " + u(fe_->screen_batches()) + " of which re-bound " + u(fe_->screen_rebind_batches()) +
         (fe_->screen_batches() >= 300 && fe_->screen_rebind_batches() * 4 > fe_->screen_batches() ? " WARN: steady frames re-bind screens (staggered serials or a flapping row: each re-bind rebuilds the model's bound materials)" : "") +
         (fe_->page_groups() > 1 ? " WARN: more than one page group alive (a rebuilt set's old group should be gone within a few frames: two page images, two uploads a frame)" : "") +
         (fe_->page_share_violations() ? " WARN: AddModelSharing from a page-group owner " + u(fe_->page_share_violations()) + " time(s)" : "");
}

std::string LiveSceneDraw::Census() const {
  char b[640];
  std::snprintf(b, sizeof b, "[scene][draw] mirror sheets %llu (failed %llu); live frames %llu, misses %llu; objects drawn %llu, skipped %llu; loads %llu ok, %llu failed; "
                "post (S103 P) live %llu with, %llu none, %llu missing, mirror %llu with, %llu none, unsupported flags on %llu frames, S+172 default %llu%s%s%s%s",
                (unsigned long long)sheets_, (unsigned long long)failed_, (unsigned long long)live_frames_, (unsigned long long)live_misses_,
                (unsigned long long)drawn_, (unsigned long long)skipped_, (unsigned long long)loads_ok_, (unsigned long long)loads_failed_,
                (unsigned long long)post_live_, (unsigned long long)post_live_none_, (unsigned long long)post_live_missing_, (unsigned long long)post_mirror_,
                (unsigned long long)post_mirror_none_, (unsigned long long)post_unsupported_, (unsigned long long)post_strength_default_,
                *post_last_why_ ? " (last: " : "", post_last_why_, *post_last_why_ ? ")" : "", first_load_error_.empty() ? "" : "; first load error: ");
  std::string s = b;
  s += first_load_error_;
  s += " fx (S103 FXF) frames " + std::to_string((unsigned long long)fx_frames_) + ", afterimage " + std::to_string((unsigned long long)fx_ghost_) + " (alpha " + std::to_string((unsigned long long)fx_ghost_alpha_) + ", additive " +
       std::to_string((unsigned long long)fx_ghost_additive_) + "), vintage-off sepia dropped " + std::to_string((unsigned long long)fx_sepia_dropped_) + ", moment tint kept " + std::to_string((unsigned long long)fx_tint_kept_) + ", display size defaulted " + std::to_string((unsigned long long)fx_disp_default_);
  s += " pst (S103) tonemap " + std::to_string((unsigned long long)fx_tonemap_) + ", night " + std::to_string((unsigned long long)fx_night_) + ", aa " + std::to_string((unsigned long long)fx_aa_) + " (skipped by rule " + std::to_string((unsigned long long)fx_aa_skipped_) +
       "), halation runs " + std::to_string((unsigned long long)fx_hala304_) + " (radial " + std::to_string((unsigned long long)fx_radial_) + "), effect5 S+300 " + std::to_string((unsigned long long)fx_hala300_) +
       ", dropped for a missing image: aa " + std::to_string((unsigned long long)g_post_aa_dropped.load()) + " luminance " + std::to_string((unsigned long long)g_post_lum_dropped.load());
  s += " cue (S103 FXH) defocus " + std::to_string((unsigned long long)fx_defocus_) + ", emboss " + std::to_string((unsigned long long)fx_emboss_) + ", retro " + std::to_string((unsigned long long)fx_retro_) + " (FILM GRAIN off: left out " +
       std::to_string((unsigned long long)fx_retro_setting_off_) + "), garbage words " + std::to_string((unsigned long long)fx_cue_garbage_) + ", passes " + std::to_string((unsigned long long)g_post_cue_passes.load()) + ", dropped for a missing image " +
       std::to_string((unsigned long long)g_post_cue_dropped.load());
  if (fe_) s += " " + TexUpLine();   // S103 UP (the final census line only; the 600-frame window line is printed with stamps on)
  if (fe_) s += " shadow_raw_bound " + std::to_string((unsigned long long)fe_->shadow_raw_bound());   // S103 L6
  if (fe_) s += " props_bound " + std::to_string((unsigned long long)fe_->props_bound()) + " d1_shadow_nomap " + std::to_string((unsigned long long)fe_->d1_shadow_nomap());   // S103 PMS: floor draws bound to the props map / floor receivers that found no map this frame (the pass without +2)
  if (fe_) s += " " + fe_->WrkCensusLine();   // S103 WRK
  if (fe_) s += " " + fe_->ReflectLine();   // S103 RFL: cubes loaded (name, size), reflect draws, fallbacks
  if (fe_) s += " " + fe_->FlashLine();   // S110 RDRPREP F3: flash sprites given / drawn / skipped for want of a depth
  if (fe_) s += " plane_bound " + std::to_string((unsigned long long)fe_->plane_bound()) + " plane_wanted_plain " + std::to_string((unsigned long long)fe_->plane_wanted_plain()) + " props_mat_bound " + std::to_string((unsigned long long)fe_->props_mat_bound()) + " char_table_picked " + std::to_string((unsigned long long)fe_->char_table_picked()) + " char_cube_analytic " + std::to_string((unsigned long long)fe_->char_cube_analytic()) + " char_wrinkle_blocked " + std::to_string((unsigned long long)fe_->char_wrinkle_blocked()) + " char_nontable_fallback " + std::to_string((unsigned long long)fe_->char_nontable_fallback()) + " char_mode_unknown " + std::to_string((unsigned long long)fe_->char_mode_unknown()) + " wet_unknown " +std::to_string((unsigned long long)fe_->wet_unknown()) + " wet_bound " + std::to_string((unsigned long long)fe_->wet_bound()) + " wet_no_variant " + std::to_string((unsigned long long)fe_->wet_no_variant()) + " wet_lost_shadow " + std::to_string((unsigned long long)fe_->wet_lost_shadow()) + " time_frozen " + std::to_string((unsigned long long)fe_->time_frozen())
    + " card_factor_missing " + std::to_string((unsigned long long)fe_->card_factor_missing()) + " lm_param [clamped " + std::to_string((unsigned long long)assets::arena_lm::g_clamped.load(std::memory_order_relaxed)) + ", no_table " + std::to_string((unsigned long long)assets::arena_lm::g_no_table.load(std::memory_order_relaxed)) + ", zero_record " + std::to_string((unsigned long long)assets::arena_lm::g_zero.load(std::memory_order_relaxed)) + "]";   // S103 L9 / B3c
  if (lists_) s += " " + lists_->Census();   // S103 G2: the lists' census on the same line (no newline inside)
  if (live_hooks_) s += " " + live_hooks_->CensusLine();   // S103 R2: the live frame's hooks (order, maps, particles)
  // S103 main: the MIRROR's hooks too: mode-1 runs never printed whether the maps (shadow / rim / plane) were made for the mirror picture,
  // so a rig-proven pass could be absent live unseen (run 28: Jarrod saw no rim / shading on the wrestlers).
  if (mirror_hooks_) s += " mirror: " + mirror_hooks_->CensusLine();
  return s;
}

}  // namespace rdr_scene
