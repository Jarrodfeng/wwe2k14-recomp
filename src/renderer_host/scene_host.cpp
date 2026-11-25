// S103 landing 2, piece P7 H1: the scene host (scene_host.h). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Section / line references in the comments are to docs\renderer\P7_DESIGN.md and the stream path's live.cpp SceneOurs, whose rules the host keeps without the stream.
#include "scene_host.h"
#include "../renderer_scene/rows_inputs.h"   // S106 G4-2
#include "../renderer_scene/rows_owned.h"
#include "../renderer_scene/rows_quad2d.h"
#include "../renderer_scene/rows_stage_look.h"
#include "wide_frame.h"   // S105 A2
#include "row_fate.h"   // S105 A4B: Fate / FateOf (moved out of this file)
#include "../renderer_game/caw_portrait_core.h"   // S103 CPB
#include "pipeline_catalog.h"   // S105 B3
#include "store_progress.h"     // S105 B3
#include "../renderer/shaders/spv/fx_registry.h"   // S105 B3: kVariants

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <atomic>

#include "../renderer/combine_lite.h"   // S103 SLV: the combine_lite census line
#include "../renderer/problem.h"
#include "../renderer_adapter/census_flush.h"   // S102 CEN1
#include "overlay_mailbox.h"   // S103 AIM: the app's ImGui dialogs, drained onto the renderer before each presented frame
#include "../renderer_game/fallback_census.h"
#include "../renderer_game/scene_frame.h"
#include "../renderer_game/scene_kind.h"
#include "../renderer_scene/cam_gate.h"   // S110 ENTRBLACK: the game's scene-geometry fact (header bits 1 / 2)
#include "../renderer_scene/stage_lmpage.h"   // S103 SMX (IP): the own page switch
#include "../renderer_scene/yuv_rgba.h"   // S103 CPU1: YuvPathName (the [host][cpu] census field)
#include "../renderer_scene/post_pass.h"   // S103 SLV: g_post_dconv_passes
#include "../scene_live/guest_model_store.h"   // S108 SHOT-A: ByteStore (the created-superstar bytes the saver adds)
#include "../scene_live/inputs_core.h"   // S103 HN B2: scene_inputs::g_counters (which bits any stamp ever hit)

namespace wwe2k14::scene_host {

static_assert(unsigned(rdr_scene::q2d::Refuse::Count) == kDropHole, "host_core.h Drop2D: the first 13 reasons are q2d::Refuse");

SceneHost::SceneHost() = default;
SceneHost::~SceneHost() { Stop(); }

void SceneHost::Log(const std::string& line) const {
  if (opts_.log) opts_.log(line.c_str());
  else std::fprintf(stderr, "%s\n", line.c_str());
}

std::string SceneHost::LastWhy() const {
  std::lock_guard<std::mutex> lock(why_mu_);
  return last_why_;
}

bool SceneHost::Start(const HostOptions& options, std::string& error) {
  if (core_.Running()) return true;
  opts_ = options;
  if (opts_.log) rdr::SetLogSink(opts_.log);
  renderer_ = rdr::Renderer::Create(opts_.renderer, error);
  if (!renderer_) return false;
  {   // S105 B3: the shader catalog (the stream build's Binder did this; the stream-free host never did: every stored pipeline stayed "unresolved" and the first match frames built them cold)
    const size_t n = RegisterShaderCatalogFrom(*renderer_, std::begin(rdr::fx::kVariants), std::end(rdr::fx::kVariants), std::begin(rdr::fx::kOwnShaders), std::end(rdr::fx::kOwnShaders));   // S110 RDRPREP: kVariants + our own blobs
    store_progress::g_progress.store_expected.store(renderer_->pipeline_store_active(), std::memory_order_relaxed);
    Log("[host][pipestore] catalog registered: " + std::to_string(n) + " shaders (" + std::to_string(std::size(rdr::fx::kOwnShaders)) + " of them our own)" + (opts_.precompile_mode ? "; precompile mode: the store builds everything at once" : ""));
  }
  draw_ = std::make_unique<rdr_scene::LiveSceneDraw>(*renderer_);
  // S103 SCS: the in-scene 2D rows (screen flashes, lens flares: kQpSceneColour) are drawn inside the scene pass by the scene hook (frame_hooks.h LiveScene2D), onto the scene colour with its depth: the same
  // q2d::Append the F rows use, the same picture resolver, depth from each row's own state (quads2d_draw.h Context::scene_depth). The report is kept for DrawRows (counted there, by guest frame).
  if (opts_.scene2d) draw_->SetScene2D([this](rdr::FrameBuilder& fb, const rdr_scene::StageCtx& sc, const rdr_scene::Quad2D* rows, size_t n, uint64_t guest_frame, uint8_t phase) {
    namespace q = rdr_scene::q2d;
    if (!q2d_registered_) q2d_programs_ = q::Register(*renderer_, true), q2d_registered_ = true;
    q::Context ctx;
    ctx.width = sc.width, ctx.height = sc.height;
    if (shape_w_ && shape_h_ && (shape_w_ != game_w_ || shape_h_ != game_h_) && game_w_ && game_h_) {   // S105 A2: the scene is wider / taller than the game's frame (play on another screen shape): these rows stay the game's centred box; a full-view flash takes the whole scene
      const wwe2k14::scene_host::wide_frame::IRect bx = wwe2k14::scene_host::wide_frame::SceneBoxOf(sc.width, sc.height, game_w_, game_h_, shape_w_, shape_h_);
      ctx.width = bx.w, ctx.height = bx.h;
      ctx.origin_x = bx.x, ctx.origin_y = bx.y;
      ctx.target_w = sc.width, ctx.target_h = sc.height;
    }
    ctx.scene_depth = true;
    const auto resolver = seam_.picture ? seam_.picture : opts_.picture;
    if (resolver) ctx.picture = TrackPictures(resolver);   // S105 A4 F8
    if (seam_.scissor) ctx.scissor = seam_.scissor;
    const q::Report rep = q::Append(fb, q2d_programs_, ctx, rows, n);
    // S105 A4B: with pre-3D rows the hook draws twice (BeforeOpaque = kPhaseEarly, then AfterBlended = kPhaseLate): the frame's report is the sum, the first refused row an index into the early + late rows together
    // (early rows come first in seq: category order), so scene_idx still maps. A kPhaseOnly / kPhaseEarly call starts a new report as before.
    if (phase == rdr_scene::LiveScene2D::kPhaseLate && scene2d_frame_ == guest_frame) {
      const int64_t early_n = int64_t(scene2d_rows_);
      scene2d_rep_.drawn += rep.drawn, scene2d_rep_.scissor_empty += rep.scissor_empty;
      for (unsigned i = 0; i < unsigned(q::Refuse::Count); ++i) scene2d_rep_.refused[i] += rep.refused[i];
      if (scene2d_rep_.first_refused < 0 && rep.first_refused >= 0) scene2d_rep_.first_refused = rep.first_refused + early_n, scene2d_rep_.first_why = rep.first_why;
      scene2d_rows_ += n;
    } else {
      scene2d_rep_ = rep;
      scene2d_frame_ = guest_frame, scene2d_rows_ = n;
    }
    if (phase == rdr_scene::LiveScene2D::kPhaseEarly) scene2d_early_ = rep.drawn;   // S105 A4B (review F4): the pre-3D rows the hook drew (CAF's black quad); counted in `scene2d_early` where the report is consumed (a faulted draw drops it)
    else if (phase != rdr_scene::LiveScene2D::kPhaseLate) scene2d_early_ = 0;
  });
  plane_images_ = std::make_unique<rdr_scene::live::movieplanes::PlaneImages<PlaneSink>>(PlaneSink{renderer_.get()});
  // The planes of a movie row: ServeGuest over the P4 store (the plane flush's copies from guest memory). The stream-slot requirement of ServeGuest ("the stream has the slot") has no meaning here:
  // there is no stream; stand-in ids pass it and are never handed out (leak_stream stays false).
  planes_ = std::make_unique<HostPlanes<PlaneSink>>(
      *plane_images_,
      [this](const rdr_scene::live::movieplanes::DrawHint& h, uint64_t j, rdr_scene::live::movieplanes::GuestSlot gs[4], uint32_t& served_set) {
        namespace mp = rdr_scene::live::movieplanes;
        const uint32_t stand_in[4] = {1, 1, 1, 1};
        return mp::ServeGuest(mp::Store(), *plane_images_, mp::MakeQuery(h, j), stand_in, false, gs, &served_set);
      },
      core_.counters().movie_repeat);
  draw_->SetGameRoot(opts_.game_root_fn ? opts_.game_root_fn() : opts_.game_root);
  stopped_ = false;
  core_.Start([this](Msg& m) { Handle(m); }, opts_.on_thread_start, [this] { PollQuit(); });
  char line[200];
  std::snprintf(line, sizeof line, "[host] started on %s (%s): our own present, no command stream", renderer_->device_name(), opts_.renderer.window.window ? "the game window" : "headless / offscreen");
  Log(line);
  if (opts_.draw_vblanks > 1) Log("[host] the decoupled 30 fps is on: the game keeps its 60 updates, one picture is drawn per " + std::to_string(opts_.draw_vblanks) + " vblanks");
  if (opts_.tween_wanted) Log("[host] the decoupled 120 (tween) is on: each drawn tick presents a halfway picture (the previous tick mixed in at 0.5) before its own");
  return true;
}

// The exit lines (the [host] census, the fallback census's exit line), once: by the quit flush (the host thread) or by Stop().
// S103 SLV: the post levers' census ([rdr][combine_lite] is the stream build's line, the same text; [rdr][post_levers] adds what the lever left out and the depth-convert passes that still run).
void SceneHost::LogPostLevers(uint64_t frames) const {
  char b[600];
  if (rdr::g_combine_lite.load(std::memory_order_relaxed) > 0) {
    rdr::CombineLiteCensusLine(frames, b, sizeof b);
    Log(b);
  }
  std::snprintf(b, sizeof b, "[rdr][post_levers] combine_lite level %d, bloom clears skipped %llu, depth-convert passes still recorded %llu (full-size R32F on OURS frames: a linear-DoF frame or an unknown depth size keeps it), skipped by the _vd fold %llu (the stream build runs none; S103 SLV2)", rdr::g_combine_lite.load(std::memory_order_relaxed),
                (unsigned long long)rdr_scene::g_post_bloom_clears_skipped.load(), (unsigned long long)rdr_scene::g_post_dconv_passes.load(), (unsigned long long)rdr_scene::g_post_dconv_skipped.load());
  Log(b);
}

// S103 LVF: the strike counts by reason and by why (the WARN lines are rate-limited: 21 of 1170 strikes showed in run 007).
void SceneHost::NoteStrike(uint8_t reason, const std::string& why) {
  if (reason < 16) ++strike_by_reason_[reason];
  const std::string key = why.substr(0, 90);
  for (auto& w : strike_whys_)
    if (w.first == key) { ++w.second; return; }
  if (strike_whys_.size() < kStrikeWhys) strike_whys_.emplace_back(key, 1);
  else {
    for (auto& w : strike_whys_)
      if (w.first == "other") { ++w.second; return; }
    strike_whys_.back().first = "other", ++strike_whys_.back().second;   // (the last key becomes the overflow bucket)
  }
}

std::string SceneHost::StrikeLine() const {
  if (!strike_lines_) return {};
  std::string by, whys;
  for (unsigned i = 0; i <= uint8_t(Reason::NeverFed); ++i)
    if (strike_by_reason_[i]) by += std::string(by.empty() ? "" : ", ") + ReasonName(Reason(i)) + " " + std::to_string((unsigned long long)strike_by_reason_[i]);
  for (const auto& w : strike_whys_) whys += std::string(whys.empty() ? "" : "; ") + w.first + " " + std::to_string((unsigned long long)w.second);
  return "[host][strikes] " + std::to_string((unsigned long long)strike_lines_) + " strikes: " + by + " | why: " + whys;
}

// S103 CHS C6 (cumulative): frames presented from `out` (no copy into F), frames that copied (of them: with a row to draw), held frames shown from `out`, fault blacks, rows lost to the guard (must be 0).
void SceneHost::LogDirect() const {
  char b[360];
  std::snprintf(b, sizeof b, "[host][direct] present_direct requested %d, active %d%s%s: from out %llu, copied %llu (rows %llu), held from out %llu, fault black %llu, row conflicts %llu", opts_.present_direct ? 1 : 0, opts_.present_direct && !*DirectWhyNot(opts_) ? 1 : 0, *DirectWhyNot(opts_) ? " because " : "", DirectWhyNot(opts_),
                (unsigned long long)direct_presents_, (unsigned long long)direct_copies_, (unsigned long long)direct_row_frames_, (unsigned long long)direct_holds_, (unsigned long long)direct_fault_black_, (unsigned long long)direct_conflicts_);
  Log(b);
  if (stage_frames_) Log("[host][stage_view] stage_frames " + std::to_string((unsigned long long)stage_frames_) + ", stage_rows_dropped " + std::to_string((unsigned long long)stage_rows_dropped_));   // S103 SVO
}

void SceneHost::LogFinal() {
  for (int i = 0; i < 600 && rec_writers_.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));   // S108 SHOT-A: a route's quit does not cut a record being written (3 s at most)
  if (final_logged_) return;
  final_logged_ = true;
  if (renderer_) renderer_->FlushPipelineStore();
  if (opts_.contributor_census) opts_.contributor_census(census_from_, core_.counters().frames);   // S103 LVF: the last window of every contributor census (prints only when frames came since the last one)
  Log(CensusLine(core_.counters()));
  if (const std::string kl = rdr_scene::live::movieplanes::KeyCensusLine(); !kl.empty()) Log(kl);   // N2: the plane-key census
  Log(SourceLine(core_.counters()));   // PBS
  LogDirect();   // S103 CHS C6
  if (const std::string sl = StrikeLine(); !sl.empty()) Log(sl);   // S103 LVF
  if (draw_) Log(draw_->Census());   // S103 MSF: the drawer's census ([scene][draw] ... plane_bound, then [scene][maps] / [scene][maps][e2]: plane made, skip reasons, blank frames); the stream build prints it from live.cpp, OFF never did (the LVF class)
  if (draw_) Log(draw_->ReadyCensusLine(true));   // S103 HN: the holes / shadow inputs the drawer SKIPPED, by class with the first reason (the stream build prints the same line from live.cpp)
  Log(scene_kind::CensusLine().c_str());   // S103 CW1: the per-kind census (last window)
  LogPostLevers(core_.counters().frames);   // S103 SLV
  Log(scene_frame::OursMissingLine());
  Log(scene_frame::missing_hist::MissingLine());
  if (opts_.report) {
    const std::string r = opts_.report();
    if (!r.empty()) Log("[host] " + r);
  }
  if (fc_handled_) Log(fallback_census::g_census.FinalLine(fc_handled_));   // the exit line of the fallback census ([scene][2d]), as the stream path prints it
}

// S108 SHOT-A: a request no drawn 3D frame served within 10 s is answered, never left silent. S108 SHOT review: also run from Handle: in a menu / a loading screen messages keep coming, so the idle
// poll (PollQuit) never runs there and the request would wait for the next 3D frame, which may be another scene, and be saved under the label as if it were the asked moment.
void SceneHost::ExpireRecordRequests() {
  std::vector<std::string> late;
  {
    std::lock_guard lock(rec_mu_);
    // render-clock: ctl - the 10 s give-up of a save request nothing served; gates a log line, no picture value reads it
    const auto now = std::chrono::steady_clock::now();
    for (auto it = rec_paths_.begin(); it != rec_paths_.end();) {
      if (now - it->second > std::chrono::seconds(10)) late.push_back(std::move(it->first)), it = rec_paths_.erase(it);
      else ++it;
    }
    rec_active_.store(!rec_paths_.empty() || rec_job_ != nullptr, std::memory_order_relaxed);
  }
  for (const std::string& p : late) Log("[wwe2k14][input] scene record NOT saved: " + p + ": no frame whose 3D draw succeeded came within 10 s (a menu, a loading screen or a faulted draw)");
}

// Host thread, every ~100 ms while no message comes (HostCore's idle poll).
void SceneHost::PollQuit() {
  if (rec_active_.load(std::memory_order_relaxed)) {   // S108 SHOT-A: a save's read-backs landed by now; an unserved request is answered after 10 s
    PollShot();
    ExpireRecordRequests();
  }
  if (scene_active_.load(std::memory_order_relaxed)) {   // S103 RBC: a scene copy landed by now, or a request not queued is served from `out` as it is
    PollShot();
    ServeSceneSync();
  }
  if (shot_active_.load(std::memory_order_relaxed)) {   // lane SHOT: no frame came within the poll: a copy in flight has landed by now; a request not queued is served from F as it is (the one drain)
    PollShot();
    ServeShotSync();
  }
  if (renderer_) store_progress::ServeFlush(store_progress::g_progress, [&] { renderer_->FlushPipelineStore(); });   // S105 B3 F1
  if (!quit_req_.exchange(false, std::memory_order_acq_rel)) return;
  LogFinal();
  quit_ack_.store(true, std::memory_order_release);
}

void SceneHost::Stop() {
  if (stopped_ || !renderer_) return;
  stopped_ = true;
  {   // lane SHOT (review F6): no caller waits for a host that is gone
    std::lock_guard lock(shot_mu_);
    shot_stop_ = true;
    shot_cv_.notify_all();
  }
  {   // S103 RBC: scene callers are answered not-ok (their callbacks run here, once)
    std::vector<SceneCb> cbs;
    {
      std::lock_guard lock(scene_mu_);
      scene_stop_ = true;
      cbs.swap(scene_waiting_);
    }
    for (SceneCb& cb : cbs) cb(false, {}, 0, 0, 0);
  }
  core_.Stop();
  if (renderer_) renderer_->WaitIdle();
  planes_.reset();   // before the images it asks
  draw_.reset();     // before the renderer: the drawer releases its images
  plane_images_.reset();
  if (renderer_ && f_ != rdr::kNoImage) renderer_->DestroyImage(f_), f_ = rdr::kNoImage;
  LogFinal();
  if (opts_.on_stop) opts_.on_stop();   // S103 PN4F (B1): the picture resolver's images and worker go while the renderer still exists
  renderer_.reset();
}

bool SceneHost::ReadPresent(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
  return renderer_ && renderer_->ReadPresent(rgba, width, height);
}

// lane SHOT (review F1 / F4 / F5 / F6). Host thread. The readback of F rides the frame about to be rendered (Renderer::QueueReadback: persistent staging, no drain) and is handed over when its fence has
// signalled (PollShot, every Handle and every idle poll). One copy is in flight at a time and answers every request whose sequence it covers; a request that arrives meanwhile gets the next copy.
void SceneHost::PublishShot(uint64_t covers, bool ok, std::vector<uint8_t>&& rgba, uint32_t w, uint32_t h, uint64_t frame) {
  auto ans = std::make_shared<ShotAnswer>();
  ans->covers = covers, ans->frame = frame, ans->ok = ok, ans->rgba = std::move(rgba), ans->w = w, ans->h = h;
  ans->curve = shot_inflight_curve_, ans->curve_set = shot_inflight_curve_set_;
  std::lock_guard lock(shot_mu_);
  if (seam_.shot_late) {   // planted fault: this answer carries the previous answer's picture
    std::shared_ptr<const ShotAnswer> prev = shot_prev_;
    shot_prev_ = ans;
    if (prev && prev->ok && ok) { auto late = std::make_shared<ShotAnswer>(*prev); late->covers = covers; ans = late; }
  }
  shot_ans_ = ans;
  if (shot_covered_ < covers) shot_covered_ = covers;
  shot_active_.store(shot_req_ > shot_covered_ || shot_inflight_, std::memory_order_relaxed);
  shot_cv_.notify_all();
}

// Before the RenderFrame whose picture is wanted (F is final once that frame has run: the copy is recorded after all its steps).
void SceneHost::QueueShot(uint64_t guest_frame) {
  if (shot_inflight_ || f_ == rdr::kNoImage) return;
  uint64_t covers = 0;
  {
    std::lock_guard lock(shot_mu_);
    if (shot_req_ <= shot_covered_) return;
    covers = shot_req_;
  }
  rdr::ImageRef ref;
  const rdr::ImageId shown = Shown();   // S103 CHS C6: F, or `out` while F is stale
  ref.image = shown;
  const uint64_t tag = ++shot_tag_;
  if (scene_rules::ShotRefusalIsWait(scene_inflight_, renderer_->ImageIs8Bit(shown))) {   // S103 RBC fix2 (F2): the refusal may be the scene copy holding this image's key: not a failure, the screenshot waits for the next frame
    rdr::ImageRef probe;
    probe.image = shown;
    if (!renderer_->QueueReadback(probe, tag)) return;
    shot_inflight_ = true, shot_inflight_covers_ = covers, shot_inflight_frame_ = guest_frame;
    shot_inflight_curve_ = curve_, shot_inflight_curve_set_ = curve_set_;
    std::lock_guard lock(shot_mu_);
    shot_covered_ = std::max(shot_covered_, covers);
    shot_active_.store(true, std::memory_order_relaxed);
    return;
  }
  if (!renderer_->ImageIs8Bit(shown) || !renderer_->QueueReadback(ref, tag)) {   // (F is RGBA8 by construction; a refusal is answered, never retried every frame)
    PublishShot(covers, false, {}, 0, 0, guest_frame);
    return;
  }
  shot_inflight_ = true, shot_inflight_covers_ = covers, shot_inflight_frame_ = guest_frame;
  shot_inflight_curve_ = curve_, shot_inflight_curve_set_ = curve_set_;
  std::lock_guard lock(shot_mu_);
  shot_covered_ = std::max(shot_covered_, covers);   // (answered when the copy lands; a request after this one waits for the next copy)
  shot_active_.store(true, std::memory_order_relaxed);
}

// ---- S108 SHOT-A: the subject-shot record save -----------------------------------------------------------------------------------------------------------------------------------------------
bool SceneHost::RequestRecordSave(const std::string& path, std::string& why) {
  if (!renderer_ || !core_.Running()) { why = "the scene host is not running"; return false; }
  if (path.empty()) { why = "no path"; return false; }
  std::lock_guard lock(rec_mu_);
  // render-clock: ctl - when a save was asked, for the 10 s "no drawn frame came" answer; no picture value reads it
  rec_paths_.emplace_back(path, std::chrono::steady_clock::now());
  rec_active_.store(true, std::memory_order_relaxed);
  return true;
}

// Before the RenderFrame of the frame whose record is wanted. `drew` = this handled frame's 3D draw succeeded (DrawResult::Ok, presented from OUR picture): only such a record is the picture shown.
void SceneHost::QueueRecordSave(uint64_t host_frame, const Msg& m, bool drew) {
  if (rec_job_ || !drew || !m.record) return;   // one save at a time; the requests wait for a drawn frame
  std::vector<std::string> paths;
  {
    std::lock_guard lock(rec_mu_);
    for (auto& p : rec_paths_) paths.push_back(std::move(p.first));
    rec_paths_.clear();
    if (paths.empty()) { rec_active_.store(false, std::memory_order_relaxed); return; }
  }
  auto job = std::make_shared<RecJob>();
  job->paths = std::move(paths), job->record = m.record, job->guest_frame = m.record->header.guest_frame, job->host_frame = host_frame;
  rdr_scene::snapshot::BodyScaleRows(*m.record, job->scales, job->scale_holes);   // the scale the host used for each wrestler (BodyScaleFor), now: the table moves on
  std::vector<rdr_scene::LiveMapsHook::LiveLiquidImage> imgs;
  draw_->LiveLiquidImages(imgs);
  for (const auto& im : imgs) {
    RecJob::Pending p;
    p.img = im, p.tag = (1ull << 61) | ++rec_tag_n_;
    rdr::ImageRef ref;
    ref.image = im.image, ref.mip = 0;
    if (!renderer_->ImageIs8Bit(im.image) || !renderer_->QueueReadback(ref, p.tag)) { ++job->refused; continue; }   // (a refusal is reported in the saved line, never retried)
    job->pending.push_back(std::move(p));
  }
  rec_job_ = job;
  if (job->pending.empty()) FinishRecordSave();   // nothing to wait for (no painted map)
}

void SceneHost::FinishRecordSave() {
  std::shared_ptr<RecJob> job = std::move(rec_job_);
  rec_job_.reset();
  if (!job) return;
  {
    std::lock_guard lock(rec_mu_);
    rec_active_.store(!rec_paths_.empty(), std::memory_order_relaxed);
  }
  void (*log)(const char*) = opts_.log;   // (by value: the writer outlives no host state)
  rec_writers_.fetch_add(1);
  std::atomic<int>* writers = &rec_writers_;
  std::thread([job, log, writers] {
    struct Done { std::atomic<int>* w; ~Done() { w->fetch_sub(1); } } done{writers};
    const auto say = [&](const std::string& line) { if (log) log(line.c_str()); else std::fprintf(stderr, "%s\n", line.c_str()); };
    rdr_scene::SceneTables t = *job->record;   // the copy is made here, off the host thread
    if (!t.crowd_runs.empty()) t = rdr_scene::ExpandCrowdRuns(t);   // (files are written plain: version 1)
    t.body_scales = job->scales;
    t.liquid_images.clear(), t.liquid_image_bytes.clear();
    uint32_t images = 0;
    for (const RecJob::Pending& p : job->pending)
      if (p.done && rdr_scene::snapshot::AddLiquidImage(t, p.img.map, p.img.object, p.w, p.h, p.rgba)) ++images;
    const rdr_scene::snapshot::FillCounts fc = rdr_scene::snapshot::FillGuestBytes(t, [](uint64_t h) { return rdr_scene::live::guest_model::ByteStore::Process().Peek(h); });
    t.header.flags |= rdr_scene::kSceneSnapshot;
    char extras[300];
    std::snprintf(extras, sizeof extras, "[wwe2k14][input] scene record extras: wrestler scales %zu (holes %u), liquid images %u (refused %u, cancelled %u), guest-model bytes filled %u (not in the store %u)",
                  t.body_scales.size(), job->scale_holes, images, job->refused, job->cancelled, fc.filled, fc.missing);
    say(extras);
    for (const std::string& path : job->paths) {
      std::string err;
      if (rdr_scene::WriteRecord(path, t, err)) {
        char b[700];
        std::snprintf(b, sizeof b, "[wwe2k14][input] scene record saved: %s (guest frame %llu, host frame %llu)", path.c_str(), (unsigned long long)job->guest_frame, (unsigned long long)job->host_frame);
        say(b);
      } else say("[wwe2k14][input] scene record NOT saved: " + path + ": " + err);
    }
  }).detach();
}

void SceneHost::PollShot() {
  if (!shot_inflight_ && !scene_inflight_ && !rec_job_) return;
  std::vector<rdr::Renderer::ReadbackResult> done;
  renderer_->PollReadbacks(done);
  for (rdr::Renderer::ReadbackResult& r : done) {
    if (scene_inflight_ && r.tag == scene_tag_) {   // S103 RBC: the scene copy (its own tag space: the high bit)
      FinishScene(!r.cancelled && r.width && r.height && out_ok_, std::move(r.rgba), r.width, r.height, scene_frame_);
      continue;
    }
    if (rec_job_ && (r.tag >> 61) == 1) {   // S108 SHOT-A: a liquid image's read-back (its own tag space: bit 61)
      bool all = true;
      for (RecJob::Pending& p : rec_job_->pending) {
        if (p.tag == r.tag) {
          if (r.cancelled || !r.width || !r.height) ++rec_job_->cancelled;
          else p.done = true, p.rgba = std::move(r.rgba), p.w = r.width, p.h = r.height;
          p.tag = 0;   // answered
        }
        all = all && p.tag == 0;
      }
      if (all) FinishRecordSave();
      continue;
    }
    if (!shot_inflight_ || r.tag != shot_tag_) continue;
    shot_inflight_ = false;
    PublishShot(shot_inflight_covers_, !r.cancelled && r.width && r.height, std::move(r.rgba), r.width, r.height, shot_inflight_frame_);
  }
}

// No frame is coming (the host's 100 ms idle poll, or a message that presented nothing): F is still the picture shown. The one drain, rare by construction.
void SceneHost::ServeShotSync() {
  if (shot_inflight_ || f_ == rdr::kNoImage) return;
  uint64_t covers = 0;
  {
    std::lock_guard lock(shot_mu_);
    if (shot_req_ <= shot_covered_) return;
    covers = shot_req_;
  }
  std::vector<uint8_t> pixels;
  uint32_t w = 0, h = 0;
  rdr::ImageRef ref;
  ref.image = Shown();   // S103 CHS C6
  const bool ok = renderer_->ReadImage(ref, pixels, w, h) && w && h && pixels.size() >= size_t(w) * h * 4;
  shot_inflight_curve_ = curve_, shot_inflight_curve_set_ = curve_set_;
  PublishShot(covers, ok, std::move(pixels), w, h, 0);
}

// ---- S103 RBC: scene read-backs ------------------------------------------------------------------------------------------------------------------------------------------------------------
bool SceneHost::RequestScene(SceneCb cb) {
  if (!renderer_ || !core_.Running() || !cb) return false;
  std::lock_guard lock(scene_mu_);
  if (scene_stop_) return false;
  scene_waiting_.push_back(std::move(cb));
  scene_active_.store(true, std::memory_order_relaxed);
  return true;
}

bool SceneHost::CaptureScene(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height, uint32_t timeout_ms, uint64_t* guest_frame) {
  struct Wait {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false, ok = false;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    uint64_t frame = 0;
  };
  auto wait = std::make_shared<Wait>();   // shared: the callback may run after the caller timed out
  if (!RequestScene([wait](bool ok, std::vector<uint8_t>&& px, uint32_t w, uint32_t h, uint64_t frame) {
        std::lock_guard lock(wait->mu);
        wait->ok = ok, wait->rgba = std::move(px), wait->w = w, wait->h = h, wait->frame = frame, wait->done = true;
        wait->cv.notify_all();
      }))
    return false;
  std::unique_lock lock(wait->mu);
  if (!wait->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return wait->done; }) || !wait->ok) return false;
  rgba = std::move(wait->rgba);
  width = wait->w, height = wait->h;
  if (guest_frame) *guest_frame = wait->frame;
  return true;
}

void SceneHost::FinishScene(bool ok, std::vector<uint8_t>&& rgba, uint32_t w, uint32_t h, uint64_t frame) {
  std::vector<SceneCb> cbs;
  cbs.swap(scene_cbs_);
  scene_inflight_ = false;
  {
    std::lock_guard lock(scene_mu_);
    scene_active_.store(!scene_waiting_.empty(), std::memory_order_relaxed);
  }
  ok = ok && w && h && rgba.size() >= size_t(w) * h * 4;
  if (ok && game_w_ && game_h_) {   // S105 A2: the guest's read-back textures (replay thumbs, the arena shot, the reel) are the game's 16:9: a picture made wider / taller for the screen's shape gives its centred 16:9 box (the screenshot QueueShot keeps the whole picture)
    const wide_frame::Box box = wide_frame::BoxOf(w, h, game_w_, game_h_);
    if (box.w != w || box.h != h) {
      uint32_t cw = 0, ch = 0;
      rgba = wide_frame::CropBox(rgba.data(), w, h, 4, game_w_, game_h_, &cw, &ch);
      w = cw, h = ch;
    }
  }
  for (size_t i = 0; i < cbs.size(); ++i) {   // every waiting caller gets the picture (the last one takes the bytes)
    if (i + 1 == cbs.size()) cbs[i](ok, std::move(rgba), w, h, frame);
    else {
      std::vector<uint8_t> copy = rgba;
      cbs[i](ok, std::move(copy), w, h, frame);
    }
  }
}

// Before the RenderFrame whose picture is wanted: the readback of the newest `out` rides that frame (recorded after all its steps). No OURS 3D frame = every waiting caller is answered not-ok at once (never the menu).
void SceneHost::QueueScene(uint64_t guest_frame) {
  if (scene_inflight_) return;
  if (scene_rules::Decide(shot_inflight_, true) == scene_rules::Step::Wait) return;   // F2: a screenshot copy (queued just before this, same frame) has priority: the scene callers wait for the next frame, and a screenshot never fails because a scene copy holds the key
  {
    std::lock_guard lock(scene_mu_);
    if (scene_waiting_.empty()) return;
    scene_cbs_.swap(scene_waiting_);
  }
  const bool have = scene_rules::Decide(false, SceneFresh(guest_frame)) == scene_rules::Step::Queue && renderer_->ImageIs8Bit(last_out_);   // F1
  rdr::ImageRef ref;
  ref.image = last_out_;
  const uint64_t tag = (1ull << 62) | ++scene_tag_n_;
  if (!have) {
    scene_inflight_ = true;
    scene_retries_ = 0;
    FinishScene(false, {}, 0, 0, guest_frame);
    return;
  }
  if (!renderer_->QueueReadback(ref, tag)) {   // F2: the key is held (another copy of this image queued or in flight): the callers go back and try the next frame, a few times
    if (scene_rules::Retry(scene_retries_)) {
      std::lock_guard lock(scene_mu_);
      scene_waiting_.insert(scene_waiting_.begin(), std::make_move_iterator(scene_cbs_.begin()), std::make_move_iterator(scene_cbs_.end()));
      scene_cbs_.clear();
      scene_active_.store(true, std::memory_order_relaxed);
      return;
    }
    scene_inflight_ = true;
    scene_retries_ = 0;
    FinishScene(false, {}, 0, 0, guest_frame);
    return;
  }
  scene_retries_ = 0;
  scene_inflight_ = true, scene_tag_ = tag, scene_frame_ = guest_frame;
}

// No frame is coming (the idle poll, or a message that presented nothing): the picture `out` holds, read with the one drain.
void SceneHost::ServeSceneSync() {
  if (scene_inflight_) return;
  {
    std::lock_guard lock(scene_mu_);
    if (scene_waiting_.empty()) return;
    scene_cbs_.swap(scene_waiting_);
  }
  scene_inflight_ = true;
  std::vector<uint8_t> pixels;
  uint32_t w = 0, h = 0;
  bool ok = false;
  if (SceneFresh(last_handled_frame_) && renderer_->ImageIs8Bit(last_out_)) {   // F1: against the newest frame the host handled
    rdr::ImageRef ref;
    ref.image = last_out_;
    ok = renderer_->ReadImage(ref, pixels, w, h);
  }
  FinishScene(ok, std::move(pixels), w, h, 0);
}

bool SceneHost::CaptureShown(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height, uint32_t timeout_ms, uint64_t* guest_frame) {
  if (!renderer_ || !core_.Running()) return false;
  std::shared_ptr<const ShotAnswer> ans;
  {
    std::unique_lock lock(shot_mu_);
    if (shot_stop_) return false;
    const uint64_t mine = ++shot_req_;   // F4: only an answer that covers this sequence is this caller's
    shot_active_.store(true, std::memory_order_relaxed);
    ++shot_waiters_;
    const bool got = shot_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return shot_stop_ || (shot_ans_ && shot_ans_->covers >= mine); });
    --shot_waiters_;
    if (got && !shot_stop_) ans = shot_ans_;
    if (shot_waiters_ == 0) shot_ans_.reset();   // (a 4K picture is not kept for nobody)
    if (!got) shot_active_.store(shot_req_ > shot_covered_ || shot_inflight_, std::memory_order_relaxed);
  }
  if (!ans || !ans->ok || ans->rgba.size() < size_t(ans->w) * ans->h * 4) return false;
  rgba = ans->rgba;
  width = ans->w, height = ans->h;
  if (ans->curve_set && !seam_.shot_no_curve) {   // the curve on the caller's thread: the host thread never runs a per-pixel loop
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
      for (uint32_t ch = 0; ch < 3; ++ch) rgba[i + ch] = ans->curve[4 * rgba[i + ch] + ch];
  }
  if (guest_frame) *guest_frame = ans->frame;
  return true;
}

// F: RGBA8, a target (the clear and, in H2, the rows) and sampled (the present); kUsageScaled = it follows the render scale like the stream's frame image did, so the copy of `out` (made at the
// game's size) into it is the same blit the binder made. Remade on a size change (the old image is destroyed by the renderer after the frames that could still use it).
void SceneHost::MakeF(uint32_t w, uint32_t h) {
  if (f_ != rdr::kNoImage) renderer_->DestroyImage(f_), f_ = rdr::kNoImage;
  rdr::ImageDesc d;
  d.width = uint16_t(w), d.height = uint16_t(h);
  d.format = rdr::Format::RGBA8;
  d.usage = uint8_t(rdr::kUsageTarget | rdr::kUsageSampled | rdr::kUsageScaled);
  f_ = renderer_->CreateImage(d);
  if (f_ != rdr::kNoImage) renderer_->SetImageClass(f_, rdr::kGpuFrame);   // S103 GT: F (the copy of the 3D picture, the 2D rows, the tween blend) is the frame role; the present step itself is 'output'
  out_w_ = w, out_h_ = h;
}

void SceneHost::ClearF(rdr::FrameBuilder& fb) {
  rdr::TargetsOp t;
  t.color[0].image = f_;
  t.color_count = 1;
  fb.SetTargets(t);
  rdr::ClearOp c;
  c.flags = rdr::kClearColor;
  c.full = 1;
  c.color[3] = 1.0f;   // opaque black
  fb.Clear(c);
}

// The planes of a movie row (host_planes.h): the P4 store's frame uploaded into OUR images; no frame for this draw = the last set served again (R9, counted movie_repeat) while its images are still ours.
rdr::TextureSlot SceneHost::PlaneOf(const rdr_scene::Quad2D& row, int slot, uint64_t j) {
  if (seam_.plane) return seam_.plane(row, slot);
  rdr::TextureSlot t{};
  rdr_scene::live::movieplanes::GuestSlot gs;
  if (!planes_ || !planes_->Slot(row.seq, row.plane_set, row.flush_seq, (row.have & rdr_scene::kQhFlush) != 0, j, slot, gs, PlaneKeyOf(row))) return t;   // image kNoImage: the row is refused `planes`
  if (!rdr_scene::live::movieplanes::SetMatchesKind(row.kind != rdr_scene::kQkYuv, gs.stand_in)) return t;   // S103 SVY (+ review): an Alpha draw over a no-alpha set, or a no-alpha draw over an alpha set, is a mismatch: refused `planes` (the row's plane_set is only the last flush of ANY movie)
  t.image = rdr::ImageId(gs.image);
  t.view_format = rdr::Format::Undefined;   // the image's own (R8)
  std::memcpy(t.swizzle, gs.swizzle, sizeof t.swizzle);
  return t;
}

// S105 A4B: Fate / Near / FateOf (what the host does with one row, R8) moved to row_fate.h so a unit drives them; the rule is the same plus the scene_on_f line.

// The 2D rows of record j onto F. The same pure pieces the stream path's 2D uses (quads2d_draw.h Check / Append: BlocksOf, the programs of Register); no binder, no stream scan, no row-site
// interleaving. A row is drawn or dropped alone, for one reason (R8): a pass the host does not draw over F, hidden HUD, or the drawer's own refusal (picture, planes, blend ...). The tap's holes are draws
// that never became a row: counted apart (rows_dropped[kDropHole]), and they make the frame a 2D-dropped one.
// S103 CHS C6: true when DrawRows below would record a pass on F. The same Fate rule, no side effect. An empty record, an untaken tap and photo mode record nothing.
bool SceneHost::RowsMayDraw(const Rec& rec, uint32_t w, uint32_t h, bool hidden, bool scene_on_f) const {
  if (rec.quad2d_holes == rdr_scene::kQuad2DNotTaken || hidden || rec.quads2d.empty() || StageDrops(rec)) return false;   // S103 SVO: a stamped stage-push record draws no row
  for (const rdr_scene::Quad2D& row : rec.quads2d)
    if (FateOf(row, opts_.draw_unknown_pass, w, h, draw_->Scene2DOn(), false, scene_on_f) == Fate::Draw) return true;
  return false;
}


// S103 SVO: a record stamped kSceneStagePlaying (goto's `scene:` push plays through the Create a Story preview, program mode 2): the game's 2D there is the editor's full-screen red background, which the stream build hid
// with its stage-view copy. Here the rows are simply not drawn: the 3D picture shows as it is. Counted by the rows left out; logged once when it first happens.
void SceneHost::NoteStageRows(const Rec& rec, uint64_t j) {
  if (!stage_frames_) Log("[host][stage_view] on: 2D left out while a stage push plays (first at guest frame " + std::to_string((unsigned long long)j) + ")");
  ++stage_frames_;
  size_t scene_rows = 0;   // the in-scene rows the scene hook drew are part of the 3D picture, not 2D left out: counted from its report like the hidden-HUD branch of DrawRows
  for (const rdr_scene::Quad2D& r : rec.quads2d)
    if (FateOf(r, opts_.draw_unknown_pass, game_w_, game_h_, draw_->Scene2DOn(), false) == Fate::InScene) ++scene_rows;   // (S105 A2: the rows' pixel space is the game's)
  if (scene_rows && scene2d_frame_ == uint64_t(rec.header.guest_frame) && scene2d_rows_ == scene_rows) {
    namespace q = rdr_scene::q2d;
    Counters& c = core_.counters();
    c.rows_drawn += scene2d_rep_.drawn, c.scene2d_early += scene2d_early_, scene2d_early_ = 0;
    for (unsigned i = 1; i < unsigned(q::Refuse::Count); ++i) c.rows_dropped[i] += scene2d_rep_.refused[i];
    scene2d_frame_ = uint64_t(-1);
  }
  stage_rows_dropped_ += rec.quads2d.size() - scene_rows;
}

// S103 CHS C6: the image that holds what is on screen now.
rdr::ImageId SceneHost::Shown() const { return (f_stale_ && out_ok_ && last_out_ != rdr::kNoImage && seam_.direct_plant != 3) ? last_out_ : f_; }

void SceneHost::DrawRows(rdr::FrameBuilder& fb, const Rec& rec, uint64_t j, uint32_t w, uint32_t h, bool hidden, bool forbid_f, bool scene_on_f) {
  namespace q = rdr_scene::q2d;
  Counters& c = core_.counters();
  const size_t n = rec.quads2d.size();
  fallback_census::NoteRows(n);
  c.rows_seen += n;
  ++c.frames_2d;
  // Menu particle groups (batches the node drew with the 2D screen camera): no flat particle drawer exists yet, so they are counted as not drawn, never silently skipped.
  if (!rec.particles.screen.empty()) c.particles_not_drawn += rec.particles.batches.size();
  if (rec.quad2d_holes == rdr_scene::kQuad2DNotTaken) {   // the 2D tap did not run on this frame: nothing was recorded, nothing can be drawn
    fallback_census::NoteRefused(j, "not_taken");
    ++c.frames_2d_dropped;
    return;
  }
  bool dropped_any = rec.quad2d_holes != 0;
  if (rec.quad2d_holes) c.rows_dropped[kDropHole] += rec.quad2d_holes;
  if (!n) {
    if (dropped_any) ++c.frames_2d_dropped;
    return;
  }
  if (hidden) {   // photo mode: the HUD is left out on purpose; the frame is counted as one with dropped rows and the census says why
    size_t scene_rows = 0;   // S103 SCS: the in-scene rows the scene hook drew are part of the 3D picture, not HUD: counted from its report, never as hidden
    for (const rdr_scene::Quad2D& r : rec.quads2d)
      if (FateOf(r, opts_.draw_unknown_pass, w, h, draw_->Scene2DOn(), false) == Fate::InScene) ++scene_rows;
    if (scene_rows && scene2d_frame_ == uint64_t(rec.header.guest_frame) && scene2d_rows_ == scene_rows) {
      c.rows_drawn += scene2d_rep_.drawn, c.scene2d_early += scene2d_early_, scene2d_early_ = 0;
      for (unsigned i = 1; i < unsigned(q::Refuse::Count); ++i) c.rows_dropped[i] += scene2d_rep_.refused[i];
      scene2d_frame_ = uint64_t(-1);
      c.rows_dropped[kDropHidden] += n - scene_rows;
    } else {
      c.rows_dropped[kDropHidden] += n;
    }
    ++c.frames_2d_dropped;
    fallback_census::NoteRefused(j, "hud_hidden");
    return;
  }
  if (!q2d_registered_) q2d_programs_ = q::Register(*renderer_, true), q2d_registered_ = true;
  q::Context ctx;
  ctx.width = w, ctx.height = h;
  if (w == game_w_ && h == game_h_ && game_w_ && game_h_ && (out_w_ != game_w_ || out_h_ != game_h_)) {   // S105 A2: the game's box inside the wider / taller F; a full-view row takes the whole F (quads2d_draw.h Context)
    ctx.origin_x = box_x_, ctx.origin_y = box_y_;
    ctx.target_w = out_w_, ctx.target_h = out_h_;
  }
  ctx.plane = [this, j](const rdr_scene::Quad2D& row, int slot) { return PlaneOf(row, slot, j); };
  const auto resolver = seam_.picture ? seam_.picture : opts_.picture;
  if (resolver) ctx.picture = TrackPictures(resolver);   // null: the drawer refuses `picture`   // S105 A4 F8: wrapped to remember the first row it refuses
  rows_now_ = rec.quads2d.data(), n_now_ = n;   // S105 A4 F6: the record's rows, for ScenePicture's "a Final row before it" test (cleared at the end of DrawRows by RowsScope)
  struct RowsScope { SceneHost* h; ~RowsScope() { h->rows_now_ = nullptr, h->n_now_ = 0; } } rows_scope{this};
  if (seam_.scissor) ctx.scissor = seam_.scissor;
  rdr::TargetsOp t{};
  t.color[0].image = f_;
  t.color_count = 1;
  // S103 CHS C6: with present_direct the pass on F starts at the first run that has rows (no empty pass on a frame whose rows are all dropped); `forbid_f` = this frame presents `out` itself and F must not be touched
  // (RowsMayDraw said nothing would be: a row here is a bug, counted and the row lost, never drawn onto a stale F).
  bool f_set = !opts_.present_direct;
  if (f_set) fb.SetTargets(t);
  // Runs of drawable rows go to the drawer in one call each (rows keep their seq order); a dropped row ends a run. The first dropped row in row order is named in the census.
  size_t run = 0, first_idx = size_t(-1), in_scene = 0;
  std::vector<size_t> scene_idx;   // S103 SCS: the record index of each in-scene row, in order (the hook's rows are the same rows: Scene2DSite + pass), to map its first refusal back to a row
  enum class FirstKind : uint8_t { None, Pass, PassUnknown, Refused } first_kind = FirstKind::None;
  const char* first_pass_why = nullptr;
  q::Refuse first_refuse = q::Refuse::None;
  q::Report total;
  const auto flush = [&](size_t end) {
    if (end <= run) return;
    if (!f_set) {
      if (forbid_f) { ++direct_conflicts_; return; }
      fb.SetTargets(t), f_set = true;
    }
    const q::Report r = q::Append(fb, q2d_programs_, ctx, rec.quads2d.data() + run, end - run);
    total.drawn += r.drawn;
    for (unsigned i = 0; i < unsigned(q::Refuse::Count); ++i) total.refused[i] += r.refused[i];
    if (r.first_refused >= 0 && run + size_t(r.first_refused) < first_idx) first_idx = run + size_t(r.first_refused), first_kind = FirstKind::Refused, first_refuse = r.first_why;
  };
  bool page_ctx = false;   // S103 SMX review A3: the record has the stage page's own rows (the page clock, or an entrance)
  for (const rdr_scene::StageLookRow& sl : rec.stage_look)
    if (sl.kind == rdr_scene::kSlPageClock || sl.kind == rdr_scene::kSlEntrance) page_ctx = true;
  for (size_t i = 0; i < n; ++i) {   // S103 SMX (IP): the first offscreen row of each 600-record window is named once (site / kind / target size), so the 512x512 page class is read, not assumed
    const rdr_scene::Quad2D& row = rec.quads2d[i];
    if (row.pass == rdr_scene::kQpOffscreen) {
      static uint64_t s_logged_window = ~0ull;
      if (s_logged_window != j / 600) {
        s_logged_window = j / 600;
        char b[200];
        std::snprintf(b, sizeof b, "[host][2d] first offscreen row of the window at record %llu: site 0x%08X kind %u target %ux%u (%s)", (unsigned long long)j, row.site, unsigned(row.kind), row.target_size[0], row.target_size[1], rdr_scene::IsStagePageRow(row, rdr_scene::stage_lmpage::On(), page_ctx) ? "page_rows" : "pass_offscreen");
        Log(b);
      }
    }
    const Fate fate = FateOf(row, opts_.draw_unknown_pass, w, h, draw_->Scene2DOn(), page_ctx, scene_on_f);
    if (fate == Fate::Draw) {
      if (scene_on_f && row.pass == rdr_scene::kQpSceneColour) ++c.scene_rows_on_f;   // S105 A4B
      continue;
    }
    flush(i);
    run = i + 1;
    if (fate == Fate::PageRow) {   // S103 SMX (IP): the page pass's row (our own page is composed by the arena): counted by its own reason, the frame stays 2D-whole
      ++c.rows_dropped[kDropPage];
      continue;
    }
    if (fate == Fate::InScene) {   // S103 SCS: the scene hook drew (or refused) this row inside the scene pass; accounted below from its report
      ++in_scene;
      scene_idx.push_back(i);
      continue;
    }
    if (fate == Fate::DropPassUnknown) ++c.rows_dropped[kDropPassUnknown];
    else ++c.rows_dropped[kDropPass];
    if (i < first_idx) {
      first_idx = i;
      first_kind = fate == Fate::DropPassUnknown ? FirstKind::PassUnknown : FirstKind::Pass;
      first_pass_why = row.pass == rdr_scene::kQpSceneColour ? "pass_scene_colour" : row.pass == rdr_scene::kQpOffscreen ? "pass_offscreen" : "pass_other";
    }
  }
  flush(n);
  std::stable_partition(scene_idx.begin(), scene_idx.end(), [&](size_t i) { return rdr_scene::LiveScene2D::IsEarly(rec.quads2d[i]); });   // S105 A4B (review F3): the hook's own order, early rows first, so first_refused indexes the same list
  if (in_scene) {   // S103 SCS: the in-scene rows, by the scene hook's report for THIS record (its guest frame). No report for it (the frame was not drawn by DrawLive) = dropped `pass` as before.
    if (scene2d_frame_ == uint64_t(rec.header.guest_frame) && scene2d_rows_ == in_scene) {
      c.rows_drawn += scene2d_rep_.drawn, c.scene2d_early += scene2d_early_, scene2d_early_ = 0;
      for (unsigned i = 1; i < unsigned(q::Refuse::Count); ++i) c.rows_dropped[i] += scene2d_rep_.refused[i];
      const uint64_t lost = in_scene - scene2d_rep_.drawn - scene2d_rep_.refused_total();   // a zero-area scissor: skipped by the drawer (counted there), kept here as `pass`
      if (lost) c.rows_dropped[kDropPass] += lost;
      if (scene2d_rep_.refused_total() || lost) dropped_any = true;
      if (scene2d_rep_.first_refused >= 0 && size_t(scene2d_rep_.first_refused) < scene_idx.size() && scene_idx[size_t(scene2d_rep_.first_refused)] < first_idx) {   // the first refused row, as its record index (so a Blend refusal names its state code)
        first_idx = scene_idx[size_t(scene2d_rep_.first_refused)];
        first_kind = FirstKind::Refused;
        first_refuse = scene2d_rep_.first_why;
      }
    } else {
      c.rows_dropped[kDropPass] += in_scene;
      dropped_any = true;
      if (first_kind == FirstKind::None) first_kind = FirstKind::Pass, first_idx = scene_idx.empty() ? 0 : scene_idx.front(), first_pass_why = "pass_scene_colour";
    }
    scene2d_frame_ = uint64_t(-1);   // consumed
  }
  c.rows_drawn += total.drawn;
  for (unsigned i = 1; i < unsigned(q::Refuse::Count); ++i) c.rows_dropped[i] += total.refused[i];
  if (total.refused_total() || first_kind != FirstKind::None) dropped_any = true;
  if (dropped_any) ++c.frames_2d_dropped;
  // The census names the first dropped row's reason (a blend the drawer has no program for: its state code), as live.cpp Draw2DOurs does. The reason comes from the drawer's own report: no second check.
  switch (first_kind) {
    case FirstKind::Pass: fallback_census::NoteRefused(j, first_pass_why); break;
    case FirstKind::PassUnknown: fallback_census::NotePassUnknown(j); break;
    case FirstKind::Refused:
      if (first_refuse == q::Refuse::Blend) fallback_census::NoteBlendCode(j, uint32_t(rec.quads2d[first_idx].state & 15u));
      else fallback_census::NoteRefused(j, q::RefuseName(first_refuse));
      break;
    case FirstKind::None: break;
  }
  // S105 A4 piece 3: log-only. The first dropped row, and (F8) the first row the picture resolver refused, each named once per mode entry per (site, why): site / pass / kind / target / rect / blend / index, with any
  // full-screen opaque row before it (refused_row_note.h).
  refused_gate_.Frame(first_kind != FirstKind::None || first_pic_set_);
  if (first_kind != FirstKind::None && first_idx < n) {
    const char* why = first_kind == FirstKind::Refused ? q::RefuseName(first_refuse) : first_kind == FirstKind::PassUnknown ? "pass_unknown" : first_pass_why;
    if (refused_gate_.Note(rec.quads2d[first_idx].site, why)) Log(refused_row::Line(rec.quads2d.data(), n, first_idx, why, j));
  }
  if (first_pic_set_) {
    for (size_t i = 0; i < n; ++i)
      if (rec.quads2d[i].seq == first_pic_seq_) {
        if (refused_gate_.Note(rec.quads2d[i].site, "picture")) Log(refused_row::Line(rec.quads2d.data(), n, i, "picture", j));
        break;
      }
  }
}

std::function<rdr::TextureSlot(const rdr_scene::Quad2D&)> SceneHost::TrackPictures(std::function<rdr::TextureSlot(const rdr_scene::Quad2D&)> r) {
  return [this, r = std::move(r)](const rdr_scene::Quad2D& row) {
    const rdr::TextureSlot s = r(row);
    if (s.image == rdr::kNoImage && !first_pic_set_) first_pic_set_ = true, first_pic_seq_ = row.seq;
    return s;
  };
}

// S103 CPB (CPR.md H1). A request stays pending while its assets load (asked again each frame, at most caw_portrait::kWaitTicks tries per request id: RetryGate); every other failure answers at once.
// The answer is published either way: G2 (the game's tick) holds only while a request is pending, G3 writes the picture or zeros (and cancels a request that timed out).
// The record: the saving screen has no 3D (draw_camera_core.h NoteBodyDraw), so the frame's own record may not hold the edited body; then the newest record that did hold guest-model rows (editor_rec_, kept by Handle) serves.
void SceneHost::ServePortrait(const std::shared_ptr<const Rec>& cur, uint64_t j) {
  PortraitRequest req;
  if (!g_portrait.TakePending(req)) return;
  const auto holds = [&](const Rec& r) {
    for (uint32_t e : r.engine_objects)
      if (caw_portrait::KeepsObject(e, req.model)) return true;
    return false;
  };
  PortraitResult res;
  res.id = req.id, res.width = req.width, res.height = req.height, res.landed_frame = j;
  bool loading = false;
  std::string why;
  const auto t0 = std::chrono::steady_clock::now();
  const Rec* use = holds(*cur) ? cur.get() : (editor_rec_ && holds(*editor_rec_) ? editor_rec_.get() : nullptr);
  if (use) res.ok = draw_->DrawPortrait(*use, req.camera, req.model, req.width, req.height, res.rgba, why, &loading);
  else {
    why = "the model is in neither this frame's record nor the last editor record; this frame: " + caw_portrait::CandidatesText(cur->engine_objects.data(), cur->engine_objects.size()) + "; last editor record: " +
          (editor_rec_ ? caw_portrait::CandidatesText(editor_rec_->engine_objects.data(), editor_rec_->engine_objects.size()) + " (frame " + std::to_string((unsigned long long)editor_rec_frame_) + ")" : std::string("none kept"));
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if (!res.ok && loading && portrait_retry_.Allow(req.id, caw_portrait::kWaitTicks)) return;   // still pending: next frame
  portrait_retry_.Done();
  // (the portrait is drawn into its own image pair: the host's held picture and the front end's live targets are untouched)
  res.why = res.ok ? "" : "failed";
  if (res.ok) ++portrait_served_;
  else ++portrait_failed_;
  Log("[rdr][caw_portrait] request " + std::to_string((unsigned long long)req.id) + " frame " + std::to_string((unsigned long long)req.guest_frame) + " character " + std::to_string(req.character) + " model " + std::to_string(req.model) + " record " + (use == cur.get() ? "this frame" : use ? "last editor" : "none") + " drawn " + std::to_string(res.ok ? 1 : 0) +
      " " + std::to_string(req.width) + "x" + std::to_string(req.height) + " ms " + std::to_string(ms) + (res.ok ? "" : ": " + why) + " (served " + std::to_string((unsigned long long)portrait_served_) + ", failed " + std::to_string((unsigned long long)portrait_failed_) + ")");
  g_portrait.Publish(std::move(res));
}

void SceneHost::Handle(Msg& m) {
  const uint64_t shot_presented0 = core_.counters().presented;
  if (PollDue()) PollShot();   // lane SHOT (+ S103 RBC scene copies): a copy queued by an earlier frame whose fence has signalled
  if (rec_active_.load(std::memory_order_relaxed)) ExpireRecordRequests();   // S108 SHOT review: the 10 s give-up while messages keep coming (no idle poll then)
  const uint64_t j = m.guest_frame;
  last_handled_frame_ = j;   // S103 RBC fix2 (F1)
  if (m.curve_changed) renderer_->SetDisplayCurve(m.curve.data()), curve_ = m.curve, curve_set_ = true;
  // The decoupled 30: this tick is not drawn (no state work, no present: the display keeps the last picture), as a tick the mailbox dropped. A screenshot request makes it draw. The display curve above is state and was applied.
  // (a screenshot request makes a tick draw only while it is not yet queued: ShotUnqueued; a copy already in flight needs no more draws)
  if (rate_.Skip(opts_.draw_vblanks, std::min<uint32_t>(m.carried_vblanks + std::max<uint32_t>(1, m.present_interval), VblankDebt::kMaxTickVblanks), ShotUnqueued(), m.record && !m.no_picture)) {
    if (++core_.counters().rate_skipped % 600 == 1) Log("[host] 30 fps pacing: " + std::to_string((unsigned long long)core_.counters().rate_skipped) + " ticks not drawn so far");
    return;
  }
  if (opts_.game_root_fn) draw_->SetGameRoot(opts_.game_root_fn());
  draw_->AdoptLoaded();
  for (const std::string& l : draw_->TakeLoadLog()) Log(l);   // S103 OFFT: the loading misses by name ([scene][load] ask / adopted), throttled in LiveSceneDraw
  draw_->ReleaseRetiredLive(j);
  if (m.record && !m.record->guest_models.Empty()) editor_rec_ = m.record, editor_rec_frame_ = j;   // S103 CPB: the newest record that held a guest-row body (the CAW editor): the portrait's source on the saving screen, which has no 3D
  else if (editor_rec_ && j > editor_rec_frame_ + 1800) editor_rec_.reset();   // ~30 s: not kept for good (a held record holds its tables)
  if (m.record && g_portrait.HasPending()) ServePortrait(m.record, j);   // S103 CPB: before fb.Begin: the portrait draws into its own images and waits on the GPU once

  // The fallback census (the [scene][2d] line): this frame opens (and the previous closes) here, as SceneOurs does; the notes follow where each reason is known.
  ++fc_handled_;
  if (m.record) fallback_census::BeginFrame(j);
  else fallback_census::NoRecord(j);
  for (std::string l = fallback_census::g_census.TakeLine(fc_handled_ - 1); !l.empty(); l = fallback_census::g_census.TakeLine(fc_handled_ - 1)) { Log(l); const std::string ac = rdr_scene::q2d::AddressCensusLine(); if (!ac.empty()) Log(ac); }   // S103 RFX: the address-mode census follows each window line

  Facts f;
  bool built_this_frame = false;   // S103 merge CB1+CHS: a Warm / DrawLive build ran for this record (outlives the inner block; the drop note below reads it)
  f.have_record = bool(m.record);
  f.no_picture = m.no_picture;
  f.size_known = m.width && m.height && m.width <= 16384 && m.height <= 16384;
  // S105 A2: during play (m.fill_now) on a screen of another shape the scene and F take the screen's shape (wide_frame.h): the camera shows more at the sides (or above and below), the 2D rows stay the game's
  // 1280x720 in a centred box (game_w_ / game_h_ and box_x_ / box_y_ below), a full-view fade takes the whole frame. At 16:9 / in menus the shape is the game's size: nothing changes.
  {
    const wide_frame::FrameShape shape = wide_frame::Shape(m.width, m.height, opts_.wide, opts_.tall, m.fill_now, seam_.wide_box_origin);
    f.width = shape.w, f.height = shape.h;
    game_w_ = shape.game_w, game_h_ = shape.game_h, box_x_ = shape.box_x, box_y_ = shape.box_y, shape_w_ = shape.w, shape_h_ = shape.h;
  }
  rdr::FrameBuilder& fb = builder_;
  // The per-frame output switches the stream path sets here (live.cpp 1054-1057: SetOutput / SetPresentFetch / SetColourCuts) are not read yet: the builder keeps what its Begin took from the
  // process-wide atomics (the defaults). Logged once at start (host_start.cpp), listed in BuildOptions.
  fb.Begin(j);
  first_pic_set_ = false;   // S105 A4 F8: the first row the picture resolver refuses in this handled frame (in-scene rows included)
  scene_serve_ = false;   // S105 A4: DrawLive below runs the in-scene 2D rows (same resolver) while `out` is the target being drawn: no scene-copy row may be served `last_out_` there (dropped `picture`, as before)
  fb.SetBlendTags(opts_.tween_wanted);   // the tween: the own draws carry blend keys (rdr_scene::TagOwnBlend)
  rdr::ImageId out = rdr::kNoImage;
  uint8_t scene_kind_now = 0;            // the Scene3D stamp's kind of this tick (the tween's scene-change test)
  std::string why;
  if (f.have_record && !f.no_picture && f.size_known) {
    const Rec& rec = *m.record;
    // The stamps of the record say whether the game walked or skipped its 3D this frame (live.cpp SceneOurs, kept unchanged).
    std::vector<rdr_own::OwnedPassStamp> stamps;
    stamps.reserve(rec.owned_passes.size());
    uint8_t kind = 0;   // the scene kind the decision used (the Scene3D stamp's pad)
    bool walked_now = false;   // S105 A4: a model-class hook asked in this record's hook frame (the Scene3D stamp's walk bit, scene_kind.h kWalkBit)
    for (const rdr_scene::OwnedStamp& s : rec.owned_passes) {
      rdr_own::OwnedPassStamp st;
      st.pass = s.pass, st.decision = s.decision, st.why_not = s.why_not, st.pad = s.pad, st.inputs_mask = s.inputs_mask, st.guest_frame = s.guest_frame;   // every field (lint stamp_copy_lint.py)
      stamps.push_back(st);
      if (s.pass == uint8_t(rdr_own::Pass::Scene3D)) kind = scene_kind::KindOfPad(s.pad);   // S103 WB: bit 7 is the scene-walk bit, not part of the kind
      if (s.pass == uint8_t(rdr_own::Pass::Scene3D)) walked_now = walked_now || scene_kind::WalkOfPad(s.pad);   // S105 A4
    }
    scene_kind_now = kind;
    bool ours = scene_frame::DrawOurs(stamps.data(), stamps.size(), j);
    bool demoted = false;   // S105 A4: OURS by READY alone, but no walk for kRecords records: taken as not ours, flat (below)
#if !WWE2K14_STREAM()
    // S105 A4 (docs\research\s105\A4.md 5): READY alone makes a frame OURS here, and a sticky stage (arena 23 after Create a Finisher) keeps it READY in the menus after it; the game's 3D walk simply stops
    // (no model / crowd / ring ask, walk bit 0). Such a frame is not ours: it goes to the flat branch below (Warm, FlatFacts3D -> ApplyWalk -> flat -> black + rows), never to hold3d / black_no_3d.
    // F2: only after WalkGate::kRecords consecutive records with no walk bit (one record missing its bit mid-match stays OURS). F1: the demoted frame's stamp is dropped as 3D evidence below (DropStampWhenDemoted).
    const bool no_walk_run = walk_gate_.Demote(walked_now, seam_.force_ours);
    if (ours && no_walk_run) ours = false, demoted = true, ++core_.counters().ours_no_walk;
#endif
    const uint64_t hook_now = scene_frame::HookFrameOf(j);
    uint32_t never_fed = 0;       // S103 HN (OFF): required inputs never stamped since the scene began
    bool built_this = false;      // a Warm / DrawLive build ran for this record (its stage verdict is this frame's)
    const uint32_t scene_req = scene_kind::RequiredFor(scene_kind::Kind(kind), rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load(std::memory_order_relaxed));
    bool fresh = true;
    if ((!seam_.force_ours && !scene_frame::ForceInputs()) || seam_.inject_stale) {   // the stale-inputs check of SceneOurs (live.cpp 1858-1873); inject_stale = the rig's missing input
      const uint64_t hook = hook_now;
      uint32_t ok = 0;
      for (const rdr_scene::InputStampRow& r : rec.input_stamps)
        if (r.bit < 32 && r.frame >= hook) ok |= 1u << r.bit;
      const uint32_t stale = scene_kind::StaleBits(kind, rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load(std::memory_order_relaxed), ok) | seam_.inject_stale;
#if !WWE2K14_STREAM()
      // S103 ONF: a stale record input is COUNTED per bit (scene_frame::OursMissingLine) and the frame is still drawn (FRAME_CONTRACT.md 11): the drawer refuses what would be garbage on its own and the host then holds our last picture.
      if (stale && (ours || seam_.force_ours)) scene_frame::NoteOursStale(scene_frame::CensusMissing(stale, rdr_scene::g_plane_game_no_module.load(std::memory_order_relaxed)));   // review A2: the record-side stale counter drops bit 9 on no-module frames too
      {   // S103 HN: stale is counted (a frame or two late); NEVER stamped since this scene began is a half-built scene: not drawn. A bit the rig injects as stale was fed (it is only late).
        uint32_t fed_now = seam_.inject_stale;
        for (const rdr_scene::InputStampRow& r : rec.input_stamps)
          if (r.bit < 32 && r.frame > 0 && r.frame >= feed_.start) fed_now |= 1u << r.bit;
        const uint32_t req = scene_kind::RequiredFor(scene_kind::Kind(kind), rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load(std::memory_order_relaxed));
        uint32_t stamped_ever = 0;   // the bits some stamp has hit this session (a bit whose writer never runs here is left out of the rule, counted)
        for (unsigned b = 0; b < 32 && b < unsigned(scene_inputs::kInputCount); ++b)
          if (scene_inputs::g_counters.stamps[b].load(std::memory_order_relaxed)) stamped_ever |= 1u << b;
        stamped_ever |= seam_.inject_stale;
        if (ours || seam_.force_ours) {
          never_fed = feed_.Note(req, fed_now, stamped_ever, j);
          if (feed_.last_unstamped) ++core_.counters().never_fed_skipped_unstamped;
        } else feed_.ever |= fed_now;
        if (feed_.gave_up && !feed_gave_counted_) feed_gave_counted_ = true, ++core_.counters().never_fed_gaveup;
        if (never_fed) {
          int b = 0;
          while (!((never_fed >> b) & 1u)) ++b;
          why = "never_fed: required record input(s) not stamped since this scene began (first: bit " + std::to_string(b) + ")";
        }
      }
      if (seam_.stale_walks && stale)   // planted twin (the rig): the first stream-free run's rule, a stale input stops the frame
#else
      if (stale)
#endif
      {
        fresh = false;
        int b = 0;
        while (!((stale >> b) & 1u)) ++b;
        why = "record inputs not fresh for hook frame " + std::to_string((unsigned long long)hook) + " (first: bit " + std::to_string(b) + ")";
      }
    }
    if (seam_.force_ours) ours = true;
    if (!seam_.noscene_ignore) f.game_draws_no_scene = rdr_scene::GameDrawsNoScene(rec.header.flags);   // S110 ENTRBLACK: the game's own decision for this frame (cam_gate.h); the rig's planted twin leaves it unread
    if (ours && f.game_draws_no_scene) {   // S110 ENTRBLACK: the game submits no scene geometry this frame (its stage and every character hidden): no 3D draw; the build still runs so the loader gets the misses and READY is learned
      f.ours = true;
      f.draw = DrawResult::Miss;
      why = "the game draws no scene geometry (stage and characters hidden)";
      draw_->Warm(rec, f.width, f.height);
      built_this = built_this_frame = true;
    } else if (ours && never_fed) {   // S103 HN: no draw; the build still runs so the loader gets the misses and READY is learned
      f.ours = true;
      f.draw = DrawResult::Miss;
      f.never_fed = never_fed;
      draw_->Warm(rec, f.width, f.height);
      built_this = built_this_frame = true;
    } else if (ours && fresh) {
      f.ours = true;
      built_this = built_this_frame = true;
      bool loading = false;
      scene2d_frame_ = uint64_t(-1);   // S103 SCS: no hook report for this record until its draw succeeds
      const int forced_fault = fault_plant_.exchange(0, std::memory_order_relaxed);   // S103 CHS rig: a planted draw: fault (the draw is not made)
      if (forced_fault) why = "draw: planted fault";
      if (!forced_fault && draw_->DrawLive(rec, fb, f.width, f.height, /*post=*/nullptr, out, why, nullptr, &loading) && out != rdr::kNoImage) {
        f.draw = DrawResult::Ok;
        out_ok_ = true;   // (set below with last_out_ when the plan presents it)
      } else {
        f.draw = ClassifyWhy(why);
        f.loading = loading;
        scene2d_frame_ = uint64_t(-1);   // S103 SCS: a failed draw's in-scene rows were not shown: never counted as drawn (DrawRows then counts them `pass`)
        // S103 CHS: did the failed draw put ops on the picture last_out_ holds? Then it is no picture any more (black, never half a frame); else it still is (a later held frame shows it again).
        if (forced_fault ? forced_fault == 2 : draw_->FaultSpoiled(last_out_)) out_ok_ = false;
        out = rdr::kNoImage;
        // (a draw: fault left part of the frame recorded into `out`'s passes: those ops stay in the frame - they touch `out` only, never F, and the frame's uploads / releases must still happen;
        // F is presented as it was: Decide's HoldF. Review H1 B3: no re-Begin.)
      }
    } else if (ours) {   // OURS but the inputs are stale: a miss of an OURS frame (the game skipped its walk)
      f.ours = true;
      f.draw = DrawResult::Miss;
    } else {             // the game walked: build the frame only so the misses reach the loader and READY is learned (Warm), then the flat verdict
      draw_->Warm(rec, f.width, f.height);
      built_this = built_this_frame = true;
      scene_kind::FlatFacts ff;
      draw_->FlatFacts3D(rec, ff);
      scene_kind::DropStampWhenDemoted(ff, demoted);   // S105 A4 F1: a demoted frame's OURS stamp is not 3D evidence (else IsFlatFrame stays false: Hold3D / black_no_3d, the same counter)
      f.flat = scene_kind::IsFlatFrame(ff);
      why = f.flat ? "flat frame" : "the game walked";
      if (!f.flat) fallback_census::NoteScene(j, fallback_census::SceneKey(stamps.data(), stamps.size(), j, scene_req));   // the hook side reason (a flat frame is the host's own: nothing to note)
    }
#if !WWE2K14_STREAM()
    // S103 HN: a scene boundary (a flat frame, or a record with no stage: loading / a scene change): the next scene's inputs start unfed. (The arena id is not in the record: an arena change that skips both is not seen. Unconfirmed.)
    // (30 fps blind spot, documented: a scene boundary that falls on a tick the pacing skips is seen at the next DRAWN tick, at most one tick late, and missed only if that tick is already past it; the boundary test needs
    // the built record, so it is not run on skipped ticks.)
    if (f.flat || (built_this && !draw_->LastBuildHadStage())) feed_.NewScene(hook_now), feed_gave_counted_ = false;
#endif
    if (f.ours && f.draw != DrawResult::Ok) fallback_census::NoteRenderWhy(j, why, f.loading);
    scene_frame::SetReady(draw_->Ready());
  } else if (f.have_record) fallback_census::NoteRender(j, f.no_picture ? "no_picture" : "no_size");
  if (!built_this_frame) draw_->PublishStartWait(j, /*built=*/false);   // S110 ENTRWAIT2: a frame that built nothing (stale OURS inputs, no record / picture / size): the game's held start learns the loader's state

  const Plan plan = Decide(st_, f, core_.counters());
  Counters& c = core_.counters();
  {
    std::lock_guard<std::mutex> lock(why_mu_);
    last_why_ = why;
  }
  last_source_.store(uint8_t(plan.source), std::memory_order_release);
  last_reason_.store(uint8_t(plan.reason), std::memory_order_release);
  if (plan.strike) {
    scene_frame::Miss(j, f.loading);   // counts the hole; in this build it never latches or holds (scene_frame.h R4)
    NoteStrike(uint8_t(plan.reason), why);   // S103 LVF
    if (++strike_lines_ <= 20 || strike_lines_ % 600 == 0)
      Log("[host] WARN frame " + std::to_string((unsigned long long)j) + ": " + ReasonName(plan.reason) + ": " + why + " (strikes " + std::to_string((unsigned long long)strike_lines_) + ")");
  } else if (f.ours && f.draw == DrawResult::Miss && f.loading) scene_frame::Miss(j, true);

  if (plan.remake_f) MakeF(plan.width, plan.height), f_stale_ = false, out_ok_ = false;   // (a new F: what the plan below puts in it is the picture)
  if (plan.present && f_ != rdr::kNoImage) {
    const rdr::Rect full{0, 0, out_w_, out_h_};
    scene_serve_ = false;   // S105 A4: the scene-copy sprite row is served `last_out_` only on a frame that presents a 3D picture (set in the switch below)
    const bool hidden = hide_hud_.load(std::memory_order_relaxed);   // read once: RowsMayDraw and DrawRows must agree
    if (m.record && plan.source != Source::HoldF && seam_.rows_first) { if (StageDrops(*m.record)) NoteStageRows(*m.record, j); else DrawRows(fb, *m.record, j, seam_.wide_pass_f_size ? out_w_ : game_w_, seam_.wide_pass_f_size ? out_h_ : game_h_, hidden, false, SceneRowsOnF(plan.reason)); }   // planted fault: the rows go under the copy   // S105 A4B: scene_on_f on a flat frame
    // S103 CHS C6: the 3D picture is presented from `out` itself when no row needs F. Not with the tween (it blends F), not at a render scale (F is scaled), not under a planted row order.
    const bool direct_ok = opts_.present_direct && !*DirectWhyNot(opts_) && !seam_.rows_first;
    const bool rows_f = m.record && plan.source != Source::HoldF && !seam_.rows_never && RowsMayDraw(*m.record, seam_.wide_pass_f_size ? out_w_ : game_w_, seam_.wide_pass_f_size ? out_h_ : game_h_, hidden, SceneRowsOnF(plan.reason));   // S105 A4B: the flat-frame flag, as DrawRows gets it   // S105 A2: the rows' pixel space is the game's 1280x720, not F's (FateOf's pixel-placed test)
    rdr::ImageId present_src = f_;
    bool forbid_f = false;
    switch (plan.source) {
      case Source::Draw3D:
      case Source::Hold3D: {
        const rdr::ImageId src = plan.source == Source::Draw3D ? out : last_out_;
        if (plan.source == Source::Draw3D) last_out_ = out, out_ok_ = true, last_out_frame_ = j;   // (F1: the frame `out` was drawn for)
        scene_serve_ = src != rdr::kNoImage && (plan.source == Source::Draw3D || out_ok_);   // S105 A4: a current 3D picture (a Hold3D one only while `out` still holds it)
        if (direct_ok && src != rdr::kNoImage && (!rows_f || seam_.direct_plant == 1)) {
          present_src = seam_.direct_plant == 2 ? f_ : src, forbid_f = true, f_stale_ = true;   // no copy: F keeps whatever it held; Shown() says where the picture is
          ++direct_presents_;
          break;
        }
        if (rows_f) ++direct_row_frames_;
        rdr::CopyOp copy;
        copy.source.image = src;
        copy.dest.image = f_;
        copy.source_rect = full, copy.dest_rect = full;
        copy.filter = rdr::CopyFilter::Linear;   // (only matters at a render scale other than 1: the same blit the stream's frame image copy made)
        if (!seam_.skip_copy) fb.Copy(copy);
        ++direct_copies_;
        f_stale_ = false;
        break;
      }
      case Source::Black: ClearF(fb); f_stale_ = false; break;
      case Source::HoldF:
        if (f_stale_) {   // F is older than the picture: the same `out` again (nothing drew it since), or black when a draw fault spoiled `out` (never a stale image)
          if (out_ok_ && last_out_ != rdr::kNoImage && seam_.direct_plant != 4) present_src = last_out_, ++direct_holds_;
          else {   // a fault put ops on the picture F was never given: black, and the frames after it are black frames (not held ones)
            ClearF(fb), f_stale_ = false, ++direct_fault_black_;
            st_.f_is_3d = false;
            if (core_.counters().repeat_full) --core_.counters().repeat_full;
            ++core_.counters().black_no_3d;
          }
        }
        break;
    }
    // H2: the record's 2D rows onto F, after the copy (seq order). A frame that repeats F as it was (HoldF) keeps its old 2D: no step, nothing counted.
    if (m.record && plan.source != Source::HoldF && !seam_.rows_never && !seam_.rows_first) { if (StageDrops(*m.record)) NoteStageRows(*m.record, j); else DrawRows(fb, *m.record, j, seam_.wide_pass_f_size ? out_w_ : game_w_, seam_.wide_pass_f_size ? out_h_ : game_h_, hidden, forbid_f, SceneRowsOnF(plan.reason)); }   // S105 A4B: scene_on_f on a flat frame   // S103 SVO: a stage push: the editor background rows are left out
    rdr::PresentOp present;
    present.source.image = present_src;
    present.source_rect = full;
    fb.Present(present);
    const rdr::Frame built = fb.Finish();
    if (!seam_.overlay_never) overlay_mailbox::Drain(*renderer_);   // S103 AIM: the app's ImGui dialogs (the plain RenderFrame and the tween pair below both draw them in the present pass)
    bool shot_queued = false;
    auto shot_before = [&] {
      if (shot_active_.load(std::memory_order_relaxed)) QueueShot(j), shot_queued = true;
      if (rec_active_.load(std::memory_order_relaxed)) QueueRecordSave(j, m, f.draw == DrawResult::Ok && plan.source == Source::Draw3D);   // S108 SHOT-A: the record of a drawn 3D frame, its liquid images read after the frame's paint
      if (scene_active_.load(std::memory_order_relaxed)) QueueScene(j);   // S103 RBC
    };   // lane SHOT: the copy is recorded after all the steps of the frame rendered next
    {   // S103 CPU1: the render / submit of this presented frame (the [host][cpu] census); the scope ends before ++c.presented
    rdr_scene::StageTimer render_timer(rdr_scene::StageCensusRef().render_ns, rdr_scene::StageCensusRef().render_n);
    // S105 B3: no pipeline-store slice while a 3D scene is the frame's source (a cold build is 25-117 ms on Intel: the freeze at the first match frames); the precompile boot builds them all.
    renderer_->SetPipelinePrebuildAllowed(store_progress::PrebuildAllowed(opts_.precompile_mode, m.record != nullptr, plan.source == Source::Black));
    if (!opts_.tween_wanted) {
      shot_before();
      renderer_->RenderFrame(built);
    } else {
      // The decoupled 120 (live.cpp 1088-1105, the same order): the halfway picture between the last drawn tick and this one first (a full frame: it does this tick's lasting work), then this tick's own
      // (DisplayOnly: the in-between redraw, no state work); FIFO shows them a vblank apart at the display's 120 Hz. The 2D rows are not mixed: both pictures carry this tick's (TickBlender mixes vertex
      // constants of keyed 3D draws only). The previous tick is kept whenever this one drew; anything else (a held / black / 2D frame) forgets it, and so does a size or scene change or a camera cut.
      const bool drawn3d = m.record && plan.source == Source::Draw3D && built.presents.count;
      TweenCam cam;
      if (m.record && m.record->has_camera) {
        const rdr_scene::Camera& rc = m.record->camera;
        cam.have = true, cam.cut = rc.cut;
        std::memcpy(cam.eye, rc.eye, sizeof cam.eye);
        std::memcpy(cam.rot, rc.view.m, sizeof cam.rot);
        cam.fov_y_deg = rc.fov_y_deg, cam.proj_x = rc.proj.m[0] < 0 ? -rc.proj.m[0] : rc.proj.m[0], cam.proj_y = rc.proj.m[5] < 0 ? -rc.proj.m[5] : rc.proj.m[5];   // (review F5: a zoom is a cut; the y scale is negative in the game's convention)
      }
      TweenFacts tf;
      tf.wanted = true, tf.presenting = drawn3d, tf.prev_valid = tween_.valid();
      tf.newer_waiting = core_.Pending(), tf.picture_requested = seam_.picture_requested;
      tf.camera_cut = CameraJumped(tween_cam_, cam);
      tf.scene_changed = tween_kind_ != scene_kind_now || tween_w_ != out_w_ || tween_h_ != out_h_;
      if (seam_.tween_no_forget) tf.camera_cut = tf.scene_changed = false;   // planted fault: the previous tick survives a cut
      const TweenVerdict verdict = TweenGate(tf);
      if (verdict == TweenVerdict::Go) {
        uint32_t matched = 0;
        // render-clock: present - the halfway picture between two game ticks, a function of the tick pair
        const rdr::Frame& half = tween_.Blend(built, seam_.tween_alpha, matched);
        if (seam_.tween_swap) {   // planted fault: this tick's own picture first, the in-between last
          renderer_->RenderFrame(built);
          shot_before();
          renderer_->RenderFrame(half, rdr::RenderMode::DisplayOnly);
        } else {
          renderer_->RenderFrame(half);
          if (seam_.on_tick) seam_.on_tick(built, &half, matched);
          shot_before();
          renderer_->RenderFrame(built, rdr::RenderMode::DisplayOnly);
        }
        ++c.tweens;
        c.tween_draws += matched;
      } else {
        shot_before();
        renderer_->RenderFrame(built);
        if (seam_.on_tick && drawn3d) seam_.on_tick(built, nullptr, 0);
        if (verdict == TweenVerdict::Cold) ++c.tween_cold;
        else if (verdict == TweenVerdict::Cut) ++c.tween_cut;
        else if (verdict == TweenVerdict::Waiting) ++c.tween_waiting;
      }
      if (drawn3d) tween_.Keep(built);   // (after a cut too: this tick is the new first one)
      else tween_.Forget();
      if (drawn3d) tween_cam_ = cam, tween_kind_ = scene_kind_now, tween_w_ = out_w_, tween_h_ = out_h_;
    }
    }   // (S103 CPU1: end of the render timer's scope)
    if (store_progress::g_progress.store_expected.load(std::memory_order_relaxed)) {   // S105 B3: what the store has left, for the precompile (swap thread)
      const rdr::Renderer::PipelineStoreCounts sc = renderer_->published_store_counts();
      store_progress::g_progress.total.store(uint32_t(sc.total), std::memory_order_relaxed), store_progress::g_progress.made.store(uint32_t(sc.made), std::memory_order_relaxed);
      store_progress::g_progress.left.store(uint32_t(sc.left_resolvable), std::memory_order_relaxed), store_progress::g_progress.unresolved.store(uint32_t(sc.unresolved), std::memory_order_relaxed);
      store_progress::g_progress.published.store(sc.valid, std::memory_order_release);
      store_progress::ServeFlush(store_progress::g_progress, [&] { renderer_->FlushPipelineStore(); });
    }
    static_assert(uint8_t(Source::Black) == 3 && sizeof(c.by_source) / sizeof(c.by_source[0]) == 4, "by_source holds one slot per Source");
    ++c.presented, ++c.by_source[uint8_t(plan.source)];   // PBS: presents by Source
    if (opts_.on_present) opts_.on_present();
    if (PollDue()) PollShot();   // (the copy this frame carried may already have landed)
  } else if (f.ours && built_this_frame) {
    renderer_->NoteFrameDropped();   // S103 CB1: an OURS frame was built (the crowd's hook may have kept a card tile as baked) and is not handed to the renderer
  }
  if (core_.counters().presented == shot_presented0 && scene_active_.load(std::memory_order_relaxed) && !scene_inflight_) ServeSceneSync();   // S103 RBC: a message that presented nothing still answers a scene request from `out`
  if (core_.counters().presented == shot_presented0 && shot_active_.load(std::memory_order_relaxed) && !shot_inflight_) ServeShotSync();   // lane SHOT (review F5): a message that presented nothing (or only a handled hold) still answers the request from F
  if (m.record && opts_.frame_done) opts_.frame_done(j);   // S103 PN4: this record's rows are drawn (or the frame was held): the picture resolver trims / releases (A5)
  {   // PBS: the [host] census + presents by source also every 5 s of wall clock (not only every 600 frames / at exit: a phone killed at 7 s printed none)
    const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (last_early_census_ms_ == 0) last_early_census_ms_ = now_ms;
    else if (now_ms - last_early_census_ms_ >= 5000) {
      last_early_census_ms_ = now_ms;
      Log(CensusLine(c));
      if (const std::string kl = rdr_scene::live::movieplanes::KeyCensusLine(); !kl.empty()) Log(kl);   // N2
      Log(SourceLine(c));
    }
  }
  static census_flush::Site host_flush;   // S102 CEN1: a flip phase end prints the census now
  const bool host_flush_due = census_flush::Due(host_flush);
  if (c.frames - last_census_ >= 600 || host_flush_due) {
    Log(SourceLine(c));   // PBS
    if (opts_.refusal_report) {   // BHF4 (COV7): refusals had no logged reason in a stream-free run (the resolver's report printed at exit only)
      const std::string rr = opts_.refusal_report();
      if (!rr.empty()) {
        if (opts_.report) Log("[host] " + opts_.report());
        Log("[host] " + rr);
      }
    }
    // S103 LVF: the contributors' census (weapons, weapon_rows, guest_model, inputs, wrestlers ...): its only other caller is the stream reader, so in OFF it never printed.
    if (opts_.contributor_census) opts_.contributor_census(census_from_, c.frames);
    census_from_ = c.frames;
    last_census_ = c.frames;
    Log(CensusLine(c));
    if (const std::string kl = rdr_scene::live::movieplanes::KeyCensusLine(); !kl.empty()) Log(kl);   // N2
    if (const std::string sl = StrikeLine(); !sl.empty()) Log(sl);   // S103 LVF: the per-reason strike counts (cumulative)
    Log(draw_->Census());   // S103 MSF: the same text as the exit line, every 600 frames
    {   // S103 CPU1 (RHP.md): the record build's CPU cost per presented frame over this window, so a Surface log shows it with no profiler
      const rdr_scene::StageWindow now = rdr_scene::StageTake();
      Log(rdr_scene::StageLine(now, cpu_from_, c.presented - cpu_presented_from_, rdr_scene::rf::c::YuvPathName()));
      cpu_from_ = now, cpu_presented_from_ = c.presented;
    }
    LogDirect();
    Log(draw_->ReadyCensusLine(true));   // S103 HN: the skipped holes by class (the window)
    if (rdr::g_gpu_stamps.load(std::memory_order_relaxed)) Log(draw_->TexUpLine());   // S103 UP: the live texture uploads by caller, stamped runs only
    Log(scene_kind::CensusLine().c_str());   // S103 CW1: the per-kind census next to the [host] one (the Caw hold shows in [host] never_fed bit 14 / 15)
    LogPostLevers(c.frames);   // S103 SLV
    Log(scene_frame::OursMissingLine());
  }
}

}  // namespace wwe2k14::scene_host
