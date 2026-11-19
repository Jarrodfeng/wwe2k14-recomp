// S103 landing 2, piece P7 H1: the game side of the scene host (docs\renderer\P7_DESIGN.md sections 1, 2, 5 H1): HostOptions from the player's settings, the SnapshotKeeper sink (without a sink
// the scene writer builds no record: live::Enabled() is false), the frame_tick poster, and the hang-guard beat after each presented frame.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// Compiled only in a WWE2K14_KEEP_STREAM=OFF build (CMakeLists.txt appends it from tools\scene\keep_stream_off_sources.txt), and StartForGame / StopForGame exist only under `#if !WWE2K14_STREAM()`: a call
// from a stream build fails to link, so the host is never started there, g_post stays null and the keeper is never attached (tools\renderer\host_core_unit.py lints the call site as well).
// The app calls StartForGame at the end of OnPreLaunchModule: the window is known and the contributors are registered (OnPostSetup), the pipeline store folder is set (OnPreLaunchModule), the guest has not run yet.
#include "../keep_stream.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/frame_events.h>   // the presents / gap counters the SDK presenter fed (owner mode feeds them from here)
#include <rex/perf/hang_beats.h>     // owner mode's presents beat the hang guard

#include "../renderer/overlay.h"     // rdr::g_presented
#include "../renderer/breadcrumbs.h" // rdr::DeviceLost
#include "../renderer_adapter/display_rate.h"   // rate::Decide / PanelHz: header-only, no stream state (the display-follow rule of the tween)
#include "../renderer_adapter/snapshot_keeper.h"
#include "../dev_build.h"         // S103: IsDevBuild (--wwe2k14_rdr_test_fail_start is a +dev switch)
#define WWE2K14_FRAME_TICK_CORE_ONLY 1   // the Tick / poster / display table only: the game part (guest reads) pulls in frame_stats.h, whose cvars are defined in main.cpp's unit alone
#include "../renderer_game/frame_tick.h"
#include "../renderer_game/owner_globals.h"
#include "../renderer_scene/live_scene.h"
#include "../renderer_adapter/stage_view.h"   // S103 SVO: g_enabled (no game or SDK includes)
#include "../renderer_scene/picture_resolver.h"   // S103 PN4: HostOptions::picture over the stream-free picture source (header-only)
#include "../renderer_scene/ui_file_index.h"       // S103 PN4: the player's own menu pacs as a file index (the source's join)
#include "../start_settings.h"                     // S103 SPB: ApplyStartSettings, the settings both start paths apply (replaces HN B1's direct call of scene_start_switches.h)
#include "../case/case_info.h"                     // S103 SPB (SP2 M5): the bug button's case zip device / render block
#include "../scene_live/picture_global.h"          // S103 PN4: the process-wide PictureSource the game's feeders (picture_tap.h) fill
#define WWE2K14_READBACK_CORE_ONLY   // S103 RBC: the counters only (the lock body and its cvar live in d3d_textures.h's TU)
#include "../renderer_game/readback_ours.h"
#include "../shown_frame.h"   // lane SHOT: g_shown_frame_owner
#include "../thread_name.h"
#include "host_start.h"
#include "scene_host.h"

// Settings the stream build defines in reader.h. A build without the stream defines the ones the host reads here (the launcher passes them on the command line: they must exist).
namespace wwe2k14::rdr_reader { REXCVAR_DECLARE(bool, wwe2k14_rdr_test_fail_start); }   // S103: DEFINED once, in the main TU (start_failure_show.h via wwe2k14_app.h); this TU includes no header that defines a cvar (tools\lint\cvar_define_once.py)
REXCVAR_DECLARE(std::string, wwe2k14_gpu);   // gpu_pick.h
#if !WWE2K14_STREAM()
REXCVAR_DEFINE_BOOL(wwe2k14_vsync, true, "wwe2k14",
                    "Settings parity S13: the launcher's V-SYNC. Only how pictures reach the screen, never the game's pace. True = FIFO, false = mailbox / immediate");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_display_hz, 0, "wwe2k14",
                     "Settings parity S9b (set by the host, not a player switch): the display rate the new renderer's pictures want (120 with "
                     "the decoupled 120); src/android_frame_rate.h asks the phone's screen for it. (Same name and default as reader.h: a build without the stream has no reader.h; review F3)");
REXCVAR_DEFINE_BOOL(wwe2k14_pump_follow_display, true, "wwe2k14",
                    "lane 168 (GATE_FAULTS row 4): the decoupled 120 follows the display's refresh (display_rate.h): on a panel under 120 Hz the game "
                    "pumps at 60, no halfway pictures. false = always keep the request. (Same as reader.h; review F3)");
REXCVAR_DEFINE_INT32(wwe2k14_panel_peak_hz, 0, "wwe2k14",
                     "lane 168 (set by GameActivity.java on Android, not a player switch): the fastest refresh the panel offers at its current "
                     "size; 0 = not known (the 120 request is kept). (Same as reader.h; review F3)");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_bc_decode, 1, "wwe2k14",
                     "S102 lane U1: block-compressed textures on a graphics chip that cannot sample them are decoded on the CPU when uploaded. 1 = automatic, 0 = off, 2 = force");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_live_validation, 0, "wwe2k14", "The new renderer's Vulkan validation (0 off, 1 core + sync, 2 + best practices)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_pipeline_store, true, "wwe2k14",
                    "Lane 276: keep a list of the pipelines the new renderer built and the driver's pipeline cache on disk (made on this machine, never shipped)");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_pipeline_prebuild_slice_us, 2000, "wwe2k14",
                     "Lane 276: the listed pipelines are built in slices of about this many microseconds at the start of each frame; 0 = never")
    .range(0, 100000);
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene2d, true, "wwe2k14",
                    "S103 SCS: the in-scene 2D rows (the entrance screen flashes and the lens flares) are drawn inside the new renderer's scene pass, as the game does. false = they are dropped (the old behaviour; for A/B)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_stage_view, false, "wwe2k14",
                    "S103 SVO (tool view, never a player look): while goto's `scene:` stage push plays through the Create a Story preview, the host leaves the 2D rows (the editor's full-screen red background) out of the picture, so the scene shows.");
#else
REXCVAR_DECLARE(bool, wwe2k14_vsync);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_bc_decode);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_live_validation);
REXCVAR_DECLARE(bool, wwe2k14_rdr_pipeline_store);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_pipeline_prebuild_slice_us);
#endif
REXCVAR_DEFINE_INT32(wwe2k14_rdr_pipeline_delay_ms, 0, "wwe2k14",
                     "Test only (the release entrance check): every pipeline build in the new renderer sleeps this many ms first, a slow driver compile simulated. 0 = off");

namespace wwe2k14::scene_host {
namespace {

std::unique_ptr<SceneHost> g_host;
std::unique_ptr<rdr_adapter::SnapshotKeeper> g_keeper;
// S103 PN4 (docs\renderer\PICTURES_NO_STREAM.md 8-9): the 2D pictures. The index of the player's own menu pacs (built on a worker, published pac by pac: the source re-joins its guest copies when the index moves), the
// resolver the host thread asks (HostOptions::picture), trimmed once per handled record (HostOptions::frame_done). The resolver needs the renderer, which exists only after SceneHost::Start, so the options carry
// lambdas over this pointer: it is set (release) before the poster is registered, so no tick can reach the host before it is there.
std::unique_ptr<rdr_scene::UiFileIndex> g_index;
std::unique_ptr<rdr_scene::PictureResolver> g_resolver_owner;
std::atomic<rdr_scene::PictureResolver*> g_resolver{nullptr};
std::thread g_index_thread;                         // B1: the index worker (joined or, when a pac read is still running at exit, detached on purpose: see StopPictures)
std::atomic<bool> g_index_stop{false}, g_index_done{false};

// B1 (S103 PN4F): the picture side's end of life, called by SceneHost::Stop (HostOptions::on_stop) AFTER the host thread is stopped and the GPU idle and BEFORE the renderer is destroyed, and once more by StopForGame (a no-op then).
// The resolver (its images, its load worker) goes first, while the renderer exists; the index worker is told to stop between pacs and waited for a moment. g_index is RELEASED, never deleted: the game's drawing thread may still be inside
// the picture source's Seen with the index pointer, and the process ends right after the quit flush.
void StopPictures() {
  g_resolver.store(nullptr, std::memory_order_release);
  g_resolver_owner.reset();
  g_index_stop.store(true, std::memory_order_release);
  for (int i = 0; i < 150 && !g_index_done.load(std::memory_order_acquire) && g_index_thread.joinable(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (g_index_thread.joinable()) {
    if (g_index_done.load(std::memory_order_acquire)) g_index_thread.join();
    else g_index_thread.detach();   // a pac read is still running (it owns only the leaked index and its own locals); it stops at the next pac
  }
  (void)g_index.release();   // intentionally leaked (see above)
}

// S103 SPB (SP2 M5): the renderer's own "[rdr] device / swapchain / display timing" lines also feed the case zip's device block (case_info.h), as live.cpp's LogTee does for the stream build.
void HostLog(const char* line) {
  rdr_case::info::NoteRendererLine(line);
  REXLOG_INFO("{}", line);
}

// The poster: runs on the game's present thread inside frame_tick::StreamFreePresentCore, after SceneEnd(j) handed record j to the keeper. It only fills a message; the host thread draws.
void PostTick(const frame_tick::Tick& t) {
  SceneHost* host = g_host.get();
  if (!host) return;
  Msg m;
  m.guest_frame = t.guest_frame;
  m.present_interval = t.present_interval;
  m.width = t.width, m.height = t.height;
  m.no_picture = t.no_picture;
  m.curve_changed = t.display_curve_changed;
  if (t.display_curve_changed) m.curve = t.display_curve;
  m.record = g_keeper ? g_keeper->ForD3(t.guest_frame) : nullptr;   // the present's OWN record (null unless the writer wrote it for this very frame)
  host->Post(std::move(m));
#if !WWE2K14_STREAM()   // (an ON build compiles this unit only for its syntax check)
  if (g_on_present) g_on_present();   // S103 AIM: the ImGui dialogs' paint request (the stream build's rdr_reader::g_on_present)
#endif
}

// What the SDK presenter fed (frame_events presents + gap) and the hang guard's "presents" beat: owner mode's swapchain feeds neither by itself (live.cpp NotePresent).
void NotePresent() {
  if (rdr::DeviceLost()) return;   // no frame counts as presented after a lost device
  static std::chrono::steady_clock::time_point last{};
  static uint64_t beat_seen = 0;
  auto& events = rex::perf::frame_events::Get();
  // render-clock: ctl - host start-up wait and heartbeat stamp, no picture value
  const auto now = std::chrono::steady_clock::now();
  if (last != std::chrono::steady_clock::time_point{}) {
    const uint32_t gap_us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(now - last).count());
    events.present_gap_us.store(gap_us, std::memory_order_relaxed);
    for (uint32_t seen = rdr::g_present_gap_max_us.load(std::memory_order_relaxed); gap_us > seen && !rdr::g_present_gap_max_us.compare_exchange_weak(seen, gap_us, std::memory_order_relaxed);) {}
  }
  last = now;
  events.presents.fetch_add(1, std::memory_order_release);
  const uint64_t shown = rdr::g_presented.load(std::memory_order_relaxed);
  if (shown != beat_seen) {   // beat only when the swapchain really presented, so a surface that fails to come back says BLACK
    beat_seen = shown;
    rex::perf::hang_beats::Beat(rex::perf::hang_beats::Get().presents);
  }
}

// S103 SPB (SP2 M6): the stream path squeezes the 3D scene by the screen-shape factors (the SDK's clip x / k) and the present stretches the picture back over the window (present_fill). This host's scene draw
// does not widen its camera yet, so a stretched 16:9 picture would be DISTORTED: until it does, the picture is fitted with bars and the request is refused out loud (RefuseSetting), never silently.
constexpr bool kSceneFollowsWide = false;

// The renderer half of the stream path's StartLive (reader.h), without any stream switch. The settings both starts share (gpu timestamps, counts, robust buffers, exclusive fullscreen, LOW's present
// clear, the ultrawide fill, the quality levers) come from wwe2k14::start::ApplyStartSettings() (start_settings.cpp). The 30 fps decouple and the phone's 120 Hz spacing are read below. Not read here yet (later pieces): per frame, the three builder settings live.cpp sets (FrameBuilder::SetOutput = the output cap height / bilinear, SetPresentFetch, SetColourCuts): the host keeps the builder's defaults
// (kNotReadYet below says so in the log).
HostOptions BuildOptions() {
  HostOptions o;
  rdr::RendererOptions& ro = o.renderer;
  const wwe2k14::start::Applied& applied = wwe2k14::start::ApplyStartSettings();
  ro.validation = REXCVAR_GET(wwe2k14_rdr_live_validation);
  ro.bc_decode = uint32_t(std::clamp(REXCVAR_GET(wwe2k14_rdr_bc_decode), 0, 2));
  ro.pipeline_delay_ms = uint32_t(std::clamp(REXCVAR_GET(wwe2k14_rdr_pipeline_delay_ms), 0, 1000)); // START-ONLY(OFF): test only (the release entrance check's slow-compile simulation); the stream build has no such check
  ro.gpu_name = REXCVAR_GET(wwe2k14_gpu);
  ro.window.instance = wwe2k14::rdr_reader::g_owner_instance;
  ro.window.window = wwe2k14::rdr_reader::g_owner_window;
  ro.window.current = wwe2k14::rdr_reader::g_owner_window_source;
  ro.window.current_user = wwe2k14::rdr_reader::g_owner_window_source_user;
  // V-SYNC: the display rate follows the game's V-SYNC switch (the SDK's `vsync`); off: mailbox / immediate (reader.h 2558-2559).
  ro.vsync = true;
  if (rex::cvar::GetFlagInfo("vsync")) ro.vsync = rex::cvar::Query<bool>("vsync");
  if (!REXCVAR_GET(wwe2k14_vsync)) ro.vsync = false;
  // RESOLUTION = the render scale (settings parity S1 / S1b, reader.h 2560-2571).
  ro.render_scale = 1.0f;
  if (rex::cvar::GetFlagInfo("resolution_scale")) {
    const int scale = rex::cvar::Query<int>("resolution_scale");
    if (scale >= 1 && scale <= 4) ro.render_scale = float(scale);
  }
  if (rex::cvar::GetFlagInfo("gpu_native_truth_draw_height")) {
    const int draw_height = std::atoi(rex::cvar::GetFlagByName("gpu_native_truth_draw_height").c_str());
    if (draw_height >= 360 && draw_height <= 2880 && float(draw_height) < ro.render_scale * 720.0f) ro.render_scale = float(draw_height) / 720.0f;
  }
  ro.present_sgsr = rex::cvar::GetFlagInfo("present_effect") && rex::cvar::GetFlagByName("present_effect") == "sgsr";   // settings parity S10a
  // S103 SPB: the same RendererOptions fields live.cpp sets from the same Applied values (start_parity_lint R4)
  ro.gpu_timestamps = applied.gpu_timestamps;   // SP2 M8: the GPU frame time
  ro.counts = applied.counts;
  ro.robust_buffers = applied.robust_buffers;
  ro.exclusive_fullscreen = applied.exclusive_fullscreen;
  ro.present_noclear = applied.present_noclear;   // SP2 M4: LOW's present lever
  o.present_direct = applied.present_direct;   // S103 CHS C6: a frame with no 2D row presents the 3D picture itself (scene_host.h HostOptions::present_direct)
  ro.present_fill = applied.present_fill && kSceneFollowsWide;   // SP2 M6 (see kSceneFollowsWide)
  if (applied.present_fill && !kSceneFollowsWide) wwe2k14::start::RefuseSetting("ULTRAWIDE / TALL SCREEN FILL", "the scene host does not widen the 3D camera yet, so the 16:9 picture is fitted with bars");
  rdr_case::info::SetRender(uint32_t(std::ceil(1280.0f * ro.render_scale)), uint32_t(std::ceil(720.0f * ro.render_scale)), ro.render_scale);   // SP2 M5: the case zip's render size (live.cpp does the same)
  if (REXCVAR_GET(wwe2k14_rdr_pipeline_store)) {
    ro.pipeline_store_dir = wwe2k14::rdr_reader::g_pipeline_store_root;
    ro.pipeline_prebuild_slice_us = uint32_t(REXCVAR_GET(wwe2k14_rdr_pipeline_prebuild_slice_us));
  }
  o.game_root_fn = []() -> std::string { return wwe2k14::rdr_reader::g_crowd_game_root; };
  o.log = &HostLog;
#if !WWE2K14_STREAM()   // the cvar is defined only in the OFF build (line 68); an ON build compiles this unit for its syntax check only
  o.scene2d = REXCVAR_GET(wwe2k14_rdr_scene2d);   // S103 SCS
  rdr_adapter::stage_view::g_host_enabled.store(REXCVAR_GET(wwe2k14_rdr_stage_view), std::memory_order_relaxed);   // S103 SVO: the guest thread stamps the records (live_scene.cpp) from this; set before the first frame
#endif
  o.on_thread_start = [] { wwe2k14::NameThread(L"rdr_host"); };
  o.on_present = &NotePresent;
  o.on_stop = &StopPictures;   // B1: the resolver and the index worker end before the renderer does
  // S103 PN4 (PICTURES_NO_STREAM.md 8): the sprite / glyph rows' pictures and the end-of-record trim. The resolver is the one this unit makes after the host's renderer exists (null until then: a row would be dropped `picture`).
  o.picture = [](const rdr_scene::Quad2D& row) {
    rdr_scene::PictureResolver* r = g_resolver.load(std::memory_order_acquire);
    return r ? r->Resolve(row) : rdr::TextureSlot{};
  };
  o.frame_done = [](uint64_t frame) {   // the record's frame = the guest frame the source stamps deaths with (frame_tick.h: wwe2k14::g_guest_frame as the present reads it): no conversion
    if (rdr_scene::PictureResolver* r = g_resolver.load(std::memory_order_acquire)) r->EndFrame(frame);
  };
  o.contributor_census = [](uint64_t from, uint64_t to) { rdr_scene::live::CensusLog(from, to); };   // S103 LVF: the contributors' census lines (its only other caller is the stream reader)
  o.refusal_report = [] {
    rdr_scene::PictureResolver* r = g_resolver.load(std::memory_order_acquire);
    return r ? r->RefusalReport() : std::string();
  };
  o.report = [] {
    rdr_scene::PictureResolver* r = g_resolver.load(std::memory_order_acquire);
    const std::string readbacks = wwe2k14::readback_ours::CensusLine();   // S103 RBC: the exit census of the game's GPU read-backs (served / readback_unserved{lr} / lock_other)
    if (!r) return readbacks;
    const auto c = rdr_scene::live::picsrc::TheSource().counters();
    char b[420];
    std::snprintf(b, sizeof b,
                  "picture source: born %llu died %llu seen %llu hits %llu joined %llu guest_copied %llu reads %llu hashes %llu trimmed %llu refill_unannounced %llu resident %llu B, index %s",
                  (unsigned long long)c.born, (unsigned long long)c.died, (unsigned long long)c.seen, (unsigned long long)c.hits, (unsigned long long)c.joined, (unsigned long long)c.guest_copied,
                  (unsigned long long)c.reads, (unsigned long long)c.hashes, (unsigned long long)c.trimmed, (unsigned long long)c.refill_unannounced, (unsigned long long)c.resident_bytes,
                  g_index ? g_index->Summary().c_str() : "-");
    return r->Report() + "; " + b + "; " + readbacks;
  };
  // The decoupled 120 (the tween; Jarrod 2026-11-17: it MUST work in this build; reader.h 2730-2755 is the stream path's setup, kept here without the reader): a launcher frame rate above 60 keeps the GAME at its
  // 60 updates (the step cvars are cleared) and makes the host draw a halfway picture per tick, when the display can show 120 (rdr_adapter::rate::Decide: a panel under 120 Hz pumps at 60, no halfway pictures;
  // an unreadable refresh keeps the request). The 120 present spacing is the swapchain's (FIFO, present_hz 120), the phone asks its screen for 120 Hz through wwe2k14_rdr_display_hz.
  const auto flag_text = [](const char* name) { return rex::cvar::GetFlagInfo(name) ? rex::cvar::GetFlagByName(name) : std::string(); };
  const int requested = std::atoi(flag_text("wwe2k14_step_hz").c_str());
  // The decoupled 30 (settings parity S9, the launchers' 30 fps mode; reader.h 2746-2751 is the stream path's setup): the game keeps its 60 updates (the step cvars are cleared) and the host draws one picture per two
  // vblanks (HostOptions::draw_vblanks -> host_core.h VblankDebt, the stream's vblank_debt of live.cpp 916-931).
  if (requested == 30) {
    for (const char* name : {"wwe2k14_step_hz", "wwe2k14_step_hz_entrance"})
      if (rex::cvar::GetFlagInfo(name)) rex::cvar::SetFlagByName(name, "0");
    o.draw_vblanks = 2;
    REXLOG_INFO("[rdr][settings] 30 fps decoupled: the game keeps its 60 updates a second, the new renderer draws one picture per two vblanks");
  }
  if (requested > 60) {
    for (const char* name : {"wwe2k14_step_hz", "wwe2k14_step_hz_entrance"})
      if (rex::cvar::GetFlagInfo(name)) rex::cvar::SetFlagByName(name, "0");
    if (rex::cvar::GetFlagInfo("wwe2k14_step_vblank")) rex::cvar::SetFlagByName("wwe2k14_step_vblank", "false");
    const bool follow = flag_text("wwe2k14_pump_follow_display") != "false";
    const double panel_hz = rdr_adapter::rate::PanelHz(ro.window.window, std::atoi(flag_text("wwe2k14_panel_peak_hz").c_str()));
    auto decision = rdr_adapter::rate::Decide(follow ? panel_hz : 0.0, requested);
    if (!follow) decision.why = "--wwe2k14_pump_follow_display=false: request kept";
    REXLOG_INFO("[pump] display refresh {:.2f} Hz{}, requested {} fps -> pumping at {} ({})", decision.panel_hz, decision.panel_known ? "" : " (unknown)", requested, decision.pump_hz, decision.why);
    if (decision.tween) {
      o.tween_wanted = true;
      ro.present_hz = 120;   // the presents spaced 1/120 s, not by a phone's (wrong, 60 Hz) refresh reading (reader.h 2749)
      if (rex::cvar::GetFlagInfo("wwe2k14_rdr_display_hz")) rex::cvar::SetFlagByName("wwe2k14_rdr_display_hz", "120");
    }
  }
  // S103 CHS C6 (review fix 5): asked for and active are said apart, with the reason when they differ (never a silent switch-off)
  REXLOG_INFO("[start] present_direct: requested {}, active {}{}{}", o.present_direct ? 1 : 0, o.present_direct && !*DirectWhyNot(o) ? 1 : 0, *DirectWhyNot(o) ? " because " : "", DirectWhyNot(o));
  return o;
}

// Per-frame builder settings of the stream path that the host does not read yet (see BuildOptions).
constexpr const char* kNotReadYet = "FrameBuilder::SetOutput (output cap / bilinear), SetPresentFetch, SetColourCuts: the builder's defaults; ui_low_ram (bgEtc is indexed on a phone too)";

}  // namespace

#if !WWE2K14_STREAM()
// S103 PN4: the picture side of the host, made once the host's renderer exists (the poster is not registered yet: no tick can be handled). The file index is built on a worker thread: the first tier of the menu pacs
// plus bgEtc (the arena pacs' pictures arrive as guest copies: the source joins them to the index only when it has them). A pac that cannot be read is logged and left out (its pictures stay guest copies). The hash cache
// (paths and hashes only, no pixels, made on this machine) lives in the runtime's cache folder next to the pipeline store.
static void StartPictures(SceneHost& host) {
  namespace fs = std::filesystem;
  g_index = std::make_unique<rdr_scene::UiFileIndex>();
  rdr_scene::live::picsrc::PictureSource& source = rdr_scene::live::picsrc::TheSource();
  source.SetIndex(g_index.get());
  const std::string root = wwe2k14::rdr_reader::g_crowd_game_root;
  g_resolver_owner = std::make_unique<rdr_scene::PictureResolver>(*host.renderer(), source, *g_index, root);
  g_resolver_owner->SetWorkerStart([] { wwe2k14::NameThread(L"rdr_pic_load"); });   // D1: the pac reads are on this thread, never on the host's
  g_resolver.store(g_resolver_owner.get(), std::memory_order_release);
  std::string cache;
  if (!wwe2k14::rdr_reader::g_ui_index_root.empty()) {   // B4: <cache root>\ui_index (wwe2k14_app.h sets it from runtime()->cache_root()), not a sibling of the pipeline store (a tests' store folder is somewhere else)
    std::error_code ec;
    const fs::path dir = fs::path(wwe2k14::rdr_reader::g_ui_index_root);
    fs::create_directories(dir, ec);
    if (!ec) cache = dir.string();
  }
  rdr_scene::UiFileIndex* index = g_index.get();
  g_index_stop.store(false), g_index_done.store(false);
  g_index_thread = std::thread([index, root, cache] {   // (the index is leaked at exit, never deleted: StopPictures)
    wwe2k14::NameThread(L"rdr_pic_index");
    rdr_scene::UiIndexOptions o;
    o.game_root = root, o.cache_dir = cache;
    uint32_t built = 0, failed = 0;
    for (uint8_t pac = 0; pac <= uint8_t(rdr_scene::kUiBgEtc) && !g_index_stop.load(std::memory_order_acquire); ++pac) {
      std::vector<rdr_scene::UiFileEntry> entries;
      std::string err;
      if (!rdr_scene::BuildPacIndex(o, pac, entries, err)) {
        ++failed;
        REXLOG_WARN("[host][pictures] {}", err);
        continue;
      }
      index->AddPac(pac, std::move(entries));
      ++built;
    }
    REXLOG_INFO("[host][pictures] file index: {} pacs indexed, {} failed; {}", built, failed, index->Summary());
    g_index_done.store(true, std::memory_order_release);
  });
  REXLOG_INFO("[host][pictures] the sprite / glyph rows' pictures come from the stream-free picture source (guest memory at first sight, joined to the player's own pacs); the movie planes from the plane flush's copies; "
              "particles on flat frames are not built (counted particles_not_drawn)");
}
#endif  // !WWE2K14_STREAM()

#if !WWE2K14_STREAM()
// OFF builds only (wwe2k14_app.h OnPreLaunchModule, last: the window is known, the contributors are registered, the pipeline store folder is set, the guest has not run yet). The keeper is attached
// before the poster is registered, so the first posted tick finds its record (the writer builds no record without a sink).
// The one failure path of the stream-free start (also taken by the app when there is no game window): the log, the verdict line, the blackbox note and the player's message / exit.
void StartFailed(const std::string& error, const std::string& card) {
  REXLOG_ERROR("[host] the scene host could not start: {}", error);
  REXLOG_ERROR("[rdr][verdict] FAILED to start: {} (card asked for: '{}')", error, card);   // the one line a report reads first (reader.h 2845)
  if (g_start_failure_sink) g_start_failure_sink(error, card);   // the app sets it: the blackbox note + the player's message / exit (their headers define cvars, so they are not included here)
}
void StartFailedNoWindow() { StartFailed("There is no game window to draw into (the window system gave none).", REXCVAR_GET(wwe2k14_gpu)); }

bool StartForGame() {
  if (g_host) return true;
  std::string error;
  // S103 SPB: this build never runs LiveSink::Start. ApplyStartSettings() is the function both starts call: the presence switches and hold (S103 HN B1: bits 10 / 11 were never stamped here), the scene-input
  // dev switches, PlayerQuality (read by the scene draw), the LOW tier and the renderer-option half. BuildOptions() below reads its result. (dev is computed once inside it.)
  wwe2k14::start::ApplyStartSettings();
  auto host = std::make_unique<SceneHost>();
  HostOptions options = BuildOptions();
  // S103 (ANS gap 1): the stream path's StartLive (reader.h 2837-2850) turns a failed start into the player's message and an exit (a box and the process ends on the PC, the exit request on a phone); a
  // black window for ever is never the answer. --wwe2k14_rdr_test_fail_start (+dev builds) fails it on purpose with the plain sentence the real failures use.
  using namespace wwe2k14::rdr_reader;   // (the switch lives in start_failure_show.h; the macro cannot name a namespaced switch)
  const bool forced_fail = wwe2k14::IsDevBuild() && REXCVAR_GET(wwe2k14_rdr_test_fail_start);
  if (forced_fail) error = "This graphics hardware is below the minimum. Missing: (test: --wwe2k14_rdr_test_fail_start).";
  if (forced_fail || !host->Start(options, error)) {
    StartFailed(error, options.renderer.gpu_name);
    return false;
  }
  StartPictures(*host);
  // START-ONLY(OFF): the host's own keeper / host / poster / screenshot hook (the stream build's LiveSink owns its keeper; the poster is the stream path's present)
  g_keeper = std::make_unique<rdr_adapter::SnapshotKeeper>();
  rdr_scene::live::AddSink(g_keeper.get());
  rdr_scene::live::SetLog(+[](const char* line) { REXLOG_INFO("{}", line); });   // S103 LVF: the writer's census / refusal lines go to the run log (reader.h sets it only in the stream build)
  // START-ONLY(OFF): (see the keeper above)
  g_host = std::move(host);
  // The app leaves with a hard exit: the SDK's quit flush asks the host thread for the final lines and the pipeline store (the stream path's LiveSink does the same, live.cpp 2412).
  rex::RegisterQuitFlush(g_host->QuitRequest(), g_host->QuitAck());
  // Photo mode's HUD hiding (the SDK's gpu_hide_hud, set live by src\photo_mode.h): the stream path registers this in reader.h 2714-2717; in this build the host drops the 2D rows while it is on.
  if (rex::cvar::GetFlagInfo("gpu_hide_hud")) {
    g_host->SetHideHud(rex::cvar::GetFlagByName("gpu_hide_hud") == "true");
    rex::cvar::RegisterChangeCallback("gpu_hide_hud", [](std::string_view, std::string_view value) {
      if (g_host) g_host->SetHideHud(value == "true");
    });
  }
  // lane SHOT: the screenshot command / photo mode / clips take the host's last presented picture (src\shown_frame.h), read back only when asked.
  // START-ONLY(OFF): the stream build sets this in wwe2k14_app.h (the LiveSink's picture); this build's picture is the host's last present
  g_shown_frame_owner = [](rex::ui::RawImage& image) {
    SceneHost* host = g_host.get();
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    uint64_t guest = kShownFrameUnknown;   // S103 FLAT: the answered frame (the host's ShotAnswer::frame), for the "screenshot saved" line
    if (!host || !host->CaptureShown(rgba, w, h, 2000, &guest)) return false;
    t_shown_guest_frame = guest;
    FillRawImage(image, rgba.data(), w, h);
    return true;
  };
  // S103 RBC: the game's GPU read-backs get the scene of the newest OURS 3D frame (SceneHost::RequestScene / CaptureScene: `out`, raw, no 2D, no curve), see renderer_game/readback_ours.h and thumb_defer_off.h.
  // START-ONLY(OFF): the stream build serves its read-backs from the reader
  g_scene_request_owner = [](SceneAnswer cb) {
    SceneHost* host = g_host.get();
    return host && host->RequestScene(std::move(cb));
  };
  g_scene_capture_owner = [](std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h, uint32_t timeout_ms, uint64_t* frame) {
    SceneHost* host = g_host.get();
    return host && host->CaptureScene(rgba, w, h, timeout_ms, frame);
  };
  frame_tick::TheCurve().RepostIfKnown();   // a table the game set at boot reaches the first frame
  // START-ONLY(OFF): the poster of every present is the host's (the stream build presents through LiveSink)
  frame_tick::g_post.store(&PostTick, std::memory_order_release);
  REXLOG_INFO("[host] the scene host is the poster of every present (record j with its own tick); not read yet: {}", kNotReadYet);
  return true;
}

void StopForGame() {
  frame_tick::g_post.store(nullptr, std::memory_order_release);
  if (g_host) rex::UnregisterQuitFlush(g_host->QuitRequest());
  if (g_host) g_host->Stop();   // (runs HostOptions::on_stop = StopPictures before the renderer goes)
  StopPictures();                // B1: a no-op when Stop already ran it (or when the host never stopped: the quit flush path)
  if (g_keeper) rdr_scene::live::RemoveSink(g_keeper.get());
  // g_host and g_keeper are RELEASED, never deleted: a present on the game thread may still be inside PostTick, and a static destructor must not run SceneHost's members against a stopped renderer; the process ends right after the quit flush.
  (void)g_keeper.release();
  (void)g_host.release();
}
#endif  // !WWE2K14_STREAM()

}  // namespace wwe2k14::scene_host
