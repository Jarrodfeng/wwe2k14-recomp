// The live sink (Session 100, live.h, docs\renderer\LIVE_ADAPTER.md). No game or SDK includes.
#include "../thread_name.h"  // S102 H1
#include "../renderer_scene/rows_crowd.h"   // S106 G4-2
#include "../renderer_scene/rows_inputs.h"
#include "../renderer_scene/rows_owned.h"
#include "../renderer_scene/rows_quad2d.h"
#include "live.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <rex/logging.h>  // S101 lane 216: RegisterQuitFlush
#include <rex/cvar.h>     // S103 lane S: the scene switch (FRAME_CONTRACT.md 2)
#include <unordered_set>

#include "../renderer/frame_builder.h"
#include "../renderer/player/png.h"
#include "../renderer/problem.h"
#include "../renderer_scene/props_budget.h"  // S102 T17
#include "../scene_live/sign_file_keys.h"  // S103 CSF: the one switch of the sign / hand file pictures
#include "../renderer/outside.h"  // S102 G5: rdr::g_gpu_outside / g_upload_batch
#include "../renderer/copy_batch.h"  // S102 G12: rdr::g_copy_batch
#include "../renderer/render_lean.h"  // S102 RT1: rdr::g_render_lean / g_lean_hooks
#include "render_lean_cvar.h"  // S102 RT1: the cvar
#include "../renderer/depth_nostencil.h"  // S102 R24-C: rdr::g_stencil_dead
#include "../renderer/breadcrumbs.h"  // S102 D1: rdr::crumbs switches, rdr::DeviceLost
#include "../renderer/combine_lite.h"  // S102 R18-B2: rdr::g_combine_lite
#include "../renderer/gpu_small.h"     // S102 R24-E: rdr::g_gpu_small
#include "../renderer/overlay.h"  // lane 256: rdr::g_presented
#include "../renderer/renderer.h"
#include "binder.h"
#include "stage_mirror.h"  // S103 T7: the stage mirror (role table, frame list)
#include "photo_save.h"  // lane 201: photo mode's save (retry, named failures, the shown shape)
#include "../renderer/frame_blend.h"
#include "capture_reader.h"
#include "frame_handoff.h"  // S102 lane T8: a frame handed over as tables (no serialise, no parse)
#include "lasting_prune.h"  // lane 307: a skipped frame binds only the passes its lasting copies need
#include "lasting_draw.h"  // S102 T3 (a): a lasting frame that keeps most of its draws is drawn
#include "../renderer/thread_cpu.h"  // S102 S0-3: the render thread's CPU time (render_thread_ms)
#include "knockouts.h"  // S102 lane KO
#include "low_levers.h"  // S102 G2: OutputCapHeight / PresentBilinear (pure)
#include "../renderer/output_switches.h"  // S102 G2: g_low_tier
#include "skips_auto.h"  // S102 K3: the D3 skips on while our render thread keeps up
#include "stage_split.h"  // S102 R21-B: the two-stage render thread (rdr_record), docs\renderer\STAGE_SPLIT.md
#include "census_flush.h"  // S102 CEN1
#include "rdr_switches.h"  // S102 S0-4: the per-frame switch table (pure; rplay keeps the LiveOptions values until the game refreshes it)
#include "snapshot_keeper.h"  // lane 215: the scene-record keeper (ForPart)
#include "../renderer_game/scene_frame.h"         // S103 lane S: the scene switch's decision (scene_frame::g_mode_cvar)
#include "../renderer_game/scene_skip_core.h"     // S103 CLK2: scene_skip::StreamHookLine (the per-hook stream census line)
#include "../renderer_game/fallback_census.h"   // S103 L2b: the fallback census (one [scene][2d] line per 600 frames; LANDING2_PLAN section 3)
#include "../renderer_scene/live_scene_draw.h"    // S103 lane S: mirror mode's drawer
#include "scene_mailbox.h"   // S103 G2: the consumer-side latest-wins pop (SCENE_PACING.md 3a / 3c)
#include "../scene_live/presence_rules.h"   // S103 S2: the presence switches
#include "../scene_live/input_changes.h"    // S103 S2: the changes census switch
#include "../dev_build.h"      // S103 L33F: IsDevBuild() (this TU does not get the WWE2K14_VERSION define)
#include "../scene_live/hold_switch.h"      // S103 L1C: --wwe2k14_rdr_scene_hold
#include "../scene_live/scene_start_switches.h"   // S103 HN B1: the presence switches + hold in one helper (the stream-free start uses it too)
#include "../start_settings.h"   // S103 SPB: ApplyStartSettings, PlayerQuality
#include "../renderer_scene/quads2d_draw.h"   // S103 L2a: the 2D rows drawn over our frame (Check, Append, Register)

// S103 lane S (docs\renderer\FRAME_CONTRACT.md 2): the scene switch. 0 off (today), 1 mirror (the game draws; on every Nth frame the present's
// own scene record is ALSO drawn alone by our record -> frame path and both pictures are written: live_scene_game_f<N>.png and
// live_scene_ours_f<N>.png in the picture folder), 2 ours (landing 1). Read per frame at submit (the swap thread) and carried with the frame.
REXCVAR_DEFINE_INT32(wwe2k14_rdr_scene, 0, "wwe2k14",
                     "S103: the 3D scene drawn by our renderer from the scene record. 0 off, 1 mirror (dev: our picture written beside the game's every "
                     "wwe2k14_rdr_scene_sheet_every frames), 2 ours (frames not ready fall back whole to the game's own renderer)");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_force_inputs, false, "wwe2k14",
                    "S103 DEV ONLY (ignored by a release build): mode 2 decides OURS without waiting for every scene input (the end-to-end integration test "
                    "while lanes still build inputs; the frame is drawn with whatever the record carries)");
// S103 L2a (docs\renderer\LANDING2_PLAN.md P1): the 2D layer from OUR rows on an OURS frame. Needs --wwe2k14_rdr_2d_record (the taps + the contributor, quads2d_tap.h / quads2d_contrib.h).
REXCVAR_DECLARE(bool, wwe2k14_rdr_2d_record);
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_2d_ours, false, "wwe2k14",
                    "S103 L2a DEV ONLY (ignored by a release build; needs --wwe2k14_rdr_2d_record and --wwe2k14_rdr_scene=2): on an OURS frame whose 2D is whole (no hole, every row drawable, rows == the "
                    "stream's row-site draws) the 2D is drawn from our rows over our frame and the stream's row-site draws are left out; any other frame keeps the stream's 2D whole");
REXCVAR_DEFINE_INT32(wwe2k14_rdr_scene_sheet_every, 300, "wwe2k14",
                     "S103: mirror mode writes the game's and our picture every N guest frames (0 = never)");
// S103 S2 (docs/renderer/S103_FABLE_REVIEW_2.md Q1): the forced mode-2 proof and the presence holds. All default off.
REXCVAR_DEFINE_INT32(wwe2k14_rdr_scene_sentinel, 0, "wwe2k14",
                     "S103 S2 DEV ONLY (ignored by a release build): in mode 2, every Nth hook frame walks on purpose (a planned hold) so every latch == walk census "
                     "runs after N - 1 frames without a walk; mirror pictures are written on the sentinel frames (one per sheet_every / N sentinels). 0 = off");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_changes, false, "wwe2k14",
                    "S103 S2: the changes census per scene input (frames its record value changed on OURS vs walked frames), in the [scene][changes] line");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_particles_by_presence, true, "wwe2k14",
                    "S103 S2: bit 11 (particles) is stamped per frame when the frame holds no unproven particle item (an unnamed texture, an unproven blend code, "
                    "another class), instead of being held for every frame by kStampParticles");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_crowd_by_presence, true, "wwe2k14",
                    "S103 S2: bit 10 (crowd) is stamped per frame when the crowd is otherwise whole and no sign holder, foam hand or sign picture is shown, "
                    "instead of being held for every frame by kCrowdOursWhole");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_crowd_screen_trust, false, "wwe2k14",
                    "S103 C1: a sign holder that only our port of the game's singles screen test (sub_8276E2C0) hides stops holding bit 10. Default off (fail closed) until a run's census shows "
                    "'screen_check eq N screen_diff 0'");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_scene_hold, "", "wwe2k14",
                      "S103 L1C: re-hold landing-1 inputs at run time (no rebuild): a comma list of input bits or names (4 light_states, 7 caster_entries, 10 crowd_snapshot, 11 particles, 17 stage_clock). "
                      "A held input is never stamped, so a frame that needs it stays the game's. Default empty = nothing held");
REXCVAR_DEFINE_STRING(wwe2k14_rdr_scene_unrequire, "", "wwe2k14",
                      "S103 L33F DEV ONLY (ignored by a release build): drop named inputs from the scene's required mask (same list as wwe2k14_rdr_scene_hold: bits or names), so mode 2 can draw OURS while "
                      "they are not stamped and one run shows the next blocker; every other input stays required. Default empty = every input required");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_light_old_hold, false, "wwe2k14",
                    "S103 LH DEV ONLY (ignored by a release build): the old light-entry rule, where a default-set arena slot model's walk-learned entry is never re-dated and so ages, planning a refresh walk (a held frame) every 1200 "
                    "hook frames. Default off: an entry equal to the computed one is refreshed at every latch and plans no hold");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_flat, false, "wwe2k14",
                    "S103 L2d DEV ONLY (ignored by a release build): a FLAT frame (no stage, no 3D object, no scene hook: menus, select, title, boot, load) may be OURS when its 2D rows, movie planes and "
                    "particles are whole (scene_kind.h FlatVerdict). Default off: no flat frame is ever OURS");
REXCVAR_DEFINE_BOOL(wwe2k14_rdr_scene_caw, true, "wwe2k14",
                    "S103 CW1: the CAW kind (the editor, game sequence 76) drops crowd / ring rows / plane constants from the required mask (inputs_core.h kCawDrop). Default on. DEV knock-out for an A/B: --wwe2k14_rdr_scene_caw=false in a +dev build; a release build always has it on. In the stream build nothing publishes the CAW kind, so it changes no decision");
#include "image_info_table.h"  // S101 lane 47: the live image-info supplier
#include "image_content_feed.h"  // S101 lane 66: the live image-content supplier (the UI)
#include "origin_intern.h"  // S101 lane 89: origin::g_labels_wanted
#include "../perf_file_write.h"  // lane 302: NoteFileWrite
#include "../case/case_hook.h"  // S101 lane 5: the bug button's ring takes what the sink takes
#include "../renderer_scene/crowd_ours.h"  // S101 lane 35: the crowd's first promotion
#include "../renderer_scene/post_ours.h"   // S101 lane PO: the post chain's promotion
#include "../renderer_scene/replace_factories.h"  // S101 lane 41: the arena's and wrestlers' in-place hooks
#include "../renderer_scene/live_scene.h"
#include "../caw_text.h"  // S101 lane 118: the Create a Superstar editor's drawn state (the `!caw` verb); one atomic test per frame while off
#include "../renderer_game/own_shadow_rim.h"  // S102 P4P5: SetReadyFromSwitches
#include "../cost_counters.h"  // S101 lane 83: the replace-check column of the [cost] line

#include <rex/perf/hang_beats.h>  // lane 256: owner mode's presents beat the hang guard
#include <rex/perf/frame_events.h>  // S100: owner mode's presents counted where the SDK presenter counts its own (frames_*.csv presented)

namespace rdr_adapter {

namespace {

// What the binder reads through (binder.h's Source): the images the render thread knows, and the store's bytes.
struct LiveSource {
  std::vector<ImageRecord> records;
  std::mutex* mu = nullptr;
  const std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>>* geometry = nullptr;  // lane 273: immutable buffers, shared out under the lock and read after it
  std::unordered_map<uint64_t, std::vector<uint8_t>>* textures = nullptr;  // contents are moved out (the entry is erased after the frame applies it)

  const std::vector<ImageRecord>& images() const { return records; }
  bool ReadGeometry(uint64_t key, uint32_t bytes, std::vector<uint8_t>& out) const {
    // Lane 273: the lock is held for the find and a handle copy only (the Append of the game thread waited behind a copy of up to MBs here);
    // the bytes are read after it, and a DropGeometry meanwhile only drops the store's handle.
    std::shared_ptr<const std::vector<uint8_t>> held;
    {
      std::lock_guard lock(*mu);
      StoreTimer hold(0);
      if (auto it = geometry->find(key); it != geometry->end()) held = it->second;
      hold.Stop();
    }
    if (!held || held->size() < bytes) {
      out.assign(bytes, 0);
      rdr::Problem("live_geometry_missing", "key %llu, %u bytes", static_cast<unsigned long long>(key), bytes);
      return false;
    }
    out.assign(held->begin(), held->begin() + bytes);
    return true;
  }
  bool ReadTexture(const ContentRef& ref, LoadedTexture& out) const {
    {
      std::lock_guard lock(*mu);
      auto it = textures->find(ref.offset);
      if (it == textures->end() || it->second.empty()) {
        rdr::Problem("live_texture_missing", "image %u", ref.image);
        return false;
      }
      out.bytes = std::move(it->second);  // S100 speed: not copied (erased once the frame has applied its contents)
    }
    return CaptureReader::ParseTexture(out);
  }
};

// S101 lane 66: the UI's content feeder reads a frame's texture contents WITHOUT taking them (LiveSource::ReadTexture moves the store's bytes out: ApplyContents must still find
// them). The bytes are lent (moved out under the lock, hashed, moved back), only while the UI switch is on, before ApplyContents.
struct LiveFeedSource {
  const LiveSource& live;
  const std::vector<ImageRecord>& images() const { return live.records; }
  bool ReadTexture(const ContentRef& ref, LoadedTexture& out) const {
    {
      std::lock_guard lock(*live.mu);
      const auto it = live.textures->find(ref.offset);
      if (it == live.textures->end() || it->second.empty()) return false;
      out.bytes = std::move(it->second);   // LENT: nothing is copied under the lock; GiveBack puts it back before ApplyContents runs (same thread)
    }
    return CaptureReader::ParseTexture(out);
  }
  void GiveBack(const ContentRef& ref, LoadedTexture& out) const {
    std::lock_guard lock(*live.mu);
    (*live.textures)[ref.offset] = std::move(out.bytes);
    out.data = nullptr, out.subresources = nullptr;
  }
};

// A frame's first part, drawn and one image read back (DrawAndRead); the reader waits on it.
struct ReadRequest {
  uint32_t image = 0, mip = 0, layer = 0;  // the reader's image id
  std::mutex mu;
  std::condition_variable cv;
  bool done = false, ok = false;
  std::vector<uint8_t> rgba;
  uint32_t width = 0, height = 0;
  std::chrono::steady_clock::time_point posted, taken, drawn, read;  // lane 232: the blocking path's timeline
};

struct Message {
  std::vector<uint8_t> frame;
  std::shared_ptr<handoff::FrameData> data;  // S102 lane T8: non-null = the frame as tables (`frame` is empty); the choice is the swap thread's, made once per guest frame
  uint64_t guest_frame = 0;
  std::string picture;  // non-empty: write this frame's picture under this name
  bool has_images = false;
  std::vector<ImageRecord> images;
  std::vector<uint32_t> released;
  std::vector<uint8_t> display_curve;  // non-empty: the game's display table changed (256 x RGBA8)
  uint64_t seq = 0;                    // lane 232: the submit count of this message (SubmitFrame numbers its frames; a DrawAndRead part carries the count so far)
  std::shared_ptr<ReadRequest> read;   // non-null: a frame's first part, always drawn, never shown; this image read back after it
  bool d3_chain = true;                // S102 K3 rework: the D3 chains run for this frame (decided at SUBMIT: skips_auto::ChainWanted); false = the armed-off frame (no record add, no Begin / End, no latch takes)
  std::shared_ptr<const rdr_scene::SceneTables> d3_record;  // lane 152 (review B1): THIS present's own record (stamps = the hook's frames up to guest_frame - 1): stream frame j needs stamp j-1, which lives in record j, not j-1 (the crowd's pairing)
  std::shared_ptr<const rdr_scene::SceneTables> record;  // S101 lane 35: the scene record the crowd is drawn from (the PREVIOUS snapshot: pairing offset +1)
  // S103 lane S: the scene switch as read at submit, and (mirror sheet frames / ours) the present's OWN record j (SnapshotKeeper::ForD3)
  int scene_mode = 0;
  bool scene_sheet = false;
  bool stage_mirror = false;        // S103 T7: this frame is dumped by role (--wwe2k14_dump_targets); false = nothing below runs
  uint64_t stage_mirror_want = 0;   // S103 T7: the listed frame this dump answers (the frame used is guest_frame)
  std::shared_ptr<const rdr_scene::SceneTables> scene_record;
};

// S101 lane 35 / lane 215: SnapshotKeeper (the owner's scene sink) lives in snapshot_keeper.h.

}  // namespace

struct LiveSink::Impl {
  LiveOptions options;
  std::atomic<bool> active{false};

  // The store: bytes by key. Geometry stays until the reader drops its key; texture contents go once a frame has applied them.
  mutable std::mutex store_mu;
  std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>> geometry;  // lane 273: shared handles, see LiveSource::ReadGeometry
  std::unordered_map<uint64_t, std::vector<uint8_t>> textures;
  std::atomic<uint64_t> next_key{1};

  // S101 lane 47: the image records as the render thread knows them (width / height per capture image id) for the replace hooks' size checks.
  ImageInfoTable image_info;
  ImageContentTable image_content;  // S101 lane 66: the UI hook's content supplier (width / height / format / mips / hash per stream image), fed on the render thread from each frame's contents

  // What the next frame carries.
  bool images_pending = false;
  std::vector<ImageRecord> images;
  std::vector<uint32_t> released;
  std::vector<uint8_t> display_curve;

  // The mailbox.
  std::mutex mail_mu;
  std::condition_variable mail_cv;
  std::condition_variable room_cv;  // a frame left the queue
  std::deque<Message> mail;
  std::deque<Message> run_q;            // S103 G2: render thread only: the popped run, dropped messages first, the newest last
  rdr_adapter::MailStats mail_stats;    // S103 G2: render thread only
  bool stopping = false;
  // S101 lane 216: the end-of-run census flush. quit_req: asked (the SDK quit flush before the hard exit, or the black box's exit zip); quit_ack: the render
  // thread printed every class's final census line since the last frame it handled (cleared by the next frame, so a later request waits again).
  std::atomic<bool> quit_req{false}, quit_ack{false};
  bool quit_registered = false;
  std::thread thread;
  // S101 lane 302: WaitForRoom's give-up state (guarded by mail_mu; written by the game's swap thread only).
  bool run_forced = false;   // S103 G2: render thread only: the run being handled dropped messages (its newest gets waiting = 0)
  bool paced = false;   // S103 G2 (mail_mu): WaitForRoom has been called: a checks run (--pace_game); every message is then a Barrier
  bool pace_off = false;
  int pace_timeouts = 0;
  uint32_t pace_give_ups = 0;
  uint64_t pace_skipped_frames = 0;
  // S101 lane 302: pictures written on a worker. The GPU read-back (and the display curve / stretch) stays on the render thread; the PNG encode,
  // the file write and the rename go to pic_thread so a stalled disk (run 7202: 47-62 s) cannot hold the render thread, and through it the paced
  // game. A named picture's picture_done is held until every queued write is on disk (the route's quit waits for it, pending_work.h).
  struct PicJob {
    std::filesystem::path path;
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
  };
  std::mutex pic_mu;
  std::condition_variable pic_cv;
  std::deque<PicJob> pic_q;
  int pic_jobs = 0;           // queued + being written
  int pic_deferred_done = 0;  // picture_done calls waiting for pic_jobs to reach 0
  bool pic_stop = false;
  std::thread pic_thread;

  // Counts.
  std::atomic<uint64_t> drawn{0}, skipped{0}, draws{0}, bound{0}, no_shader{0}, pngs{0};
  std::atomic<uint64_t> parts{0};  // frames' first parts drawn for a read-back copy (DrawAndRead)
  std::atomic<uint64_t> submit_seq{0};  // lane 232: frames submitted by SubmitFrame so far
  struct AsyncTag {
    AsyncReadback::Key key;
    uint64_t frame = 0, seq = 0;
  };
  AsyncReadback async;  // lane 232 (3d): finished copies of watched images (async_readback.h), published by the render thread after it draws a frame holding a copy into one
  std::vector<AsyncReadback::Key> async_due;
  std::vector<uint32_t> async_images;
  std::vector<rdr::CopyOp> left_out_copies;   // S102 T3 (a)
  std::atomic<uint64_t> lasting_drawn{0}, lasting_drawn_draws{0}, lasting_drawn_kept{0};  // S102 T3 (a): skipped frames drawn instead (and their draws / the draws a lasting bind would have kept)
  std::atomic<uint64_t> persistent_only{0};  // skipped frames whose lasting work (copies into textures) still ran
  uint64_t last_logged = 0;  // render thread only
  uint32_t vblank_debt = 0;  // S9: vblanks of presents since the last drawn frame (render thread only)
  rdr::TickBlender tween;    // S9b: the last drawn tick, for the next tick's halfway picture (render thread only)
  rdr::TickBlender motion_ticks;  // S10b: the last drawn tick, whose vertex constants the DLSS velocity draws read (render thread only)
  // Settings parity S11: the app's overlay, handed over by any thread, applied by the render thread before it renders.
  struct OverlayTexture {
    uint32_t id, width, height;
    std::vector<uint8_t> rgba;
  };
  std::mutex overlay_mu;
  std::shared_ptr<const rdr::OverlayList> overlay_next;
  bool overlay_changed = false;
  std::vector<OverlayTexture> overlay_textures;
  std::filesystem::path last_picture_path;  // S101 lane 19: write the last presented picture here now (mail_mu)
  // S103 lane SHOT (src\shown_frame.h; review F1 / F3 / F4): the last presented picture as pixels, on request only, WITHOUT a drain: the render thread queues ONE Renderer::QueueReadback of the presented image
  // (8-bit pictures only) that rides the next RenderFrame, PollAsync hands the finished copy to every caller whose request sequence it covers; the curve + RGBA8 copy run on the CALLER's thread.
  // With no frame coming (the 100 ms idle poll) the one synchronous read serves it.
  struct ShotAnswer {
    uint64_t covers = 0;
    bool ok = false;
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> curve;   // the display table as it was when the copy was queued (empty: identity)
  };
  std::mutex shot_mu;
  std::condition_variable shot_cv;
  std::atomic<bool> shot_pending{false};     // a request not yet queued, or a copy in flight (the one flag the render thread loads)
  uint64_t shot_req = 0, shot_covered = 0;   // shot_mu
  int shot_waiters = 0;
  bool shot_stop = false;
  std::shared_ptr<const ShotAnswer> shot_ans;
  bool shot_inflight = false;                // render thread
  uint64_t shot_tag = 0, shot_inflight_covers = 0;
  std::vector<uint8_t> shot_inflight_curve;
  bool shot_warned = false;
  std::filesystem::path photo_path;  // photo mode: the next shown picture goes here (overlay_mu); kept until written (lane 201: retried, then named)
  uint32_t photo_tries = 0;          // lane 201: failed attempts of the request in photo_path (render thread; reset by SavePicture under overlay_mu)
  void ApplyOverlay() {  // render thread
    std::vector<OverlayTexture> textures;
    std::shared_ptr<const rdr::OverlayList> list;
    bool changed = false;
    {
      std::lock_guard lock(overlay_mu);
      textures.swap(overlay_textures);
      if (overlay_changed) list = overlay_next, changed = true, overlay_changed = false;
    }
    for (const OverlayTexture& t : textures) renderer->SetOverlayTexture(t.id, t.width, t.height, t.rgba.data());
    if (changed) renderer->SetOverlay(std::move(list));
  }
  // S102 R21-B (review F1): ApplyOverlay's two halves on a split frame. The bind stage takes the app's hand-over and makes + uploads the new textures BEFORE
  // the frame is sealed (their uploads ride with it); the record stage's hook only puts them under their ids and sets the list (stage_split::Packet).
  void TakeOverlay(stage_split::Packet& p) {
    std::vector<OverlayTexture> textures;
    {
      std::lock_guard lock(overlay_mu);
      textures.swap(overlay_textures);
      if (overlay_changed) p.overlay_list = overlay_next, p.overlay_changed = true, overlay_changed = false;
    }
    for (const OverlayTexture& t : textures) p.overlay_images.emplace_back(t.id, renderer->MakeOverlayTexture(t.width, t.height, t.rgba.data()));
  }
  std::atomic<uint64_t> tweens{0}, tween_draws{0};  // S9b: halfway pictures drawn; draws mixed in them

  std::unique_ptr<rdr::Renderer> renderer;
  std::unique_ptr<SnapshotKeeper> keeper;  // S101 lane 35 (null unless options.crowd_ours)
  std::unique_ptr<rdr_scene::LiveSceneDraw> scene_draw;  // S103 lane S: made on the render thread at its first use; destroyed in Stop after the GPU is idle
  // S103 lane S: the scene fields of a message, at submit (the swap thread): the switch read once per guest frame and carried with it
  void NoteScene(Message& m, uint64_t guest_frame) {
    m.scene_mode = REXCVAR_GET(wwe2k14_rdr_scene);
    const int every = REXCVAR_GET(wwe2k14_rdr_scene_sheet_every);
    // render-clock: dev - every Nth frame posts a contact-sheet scene capture
    m.scene_sheet = m.scene_mode == 1 && every > 0 && guest_frame % uint64_t(every) == 0;
    if (m.scene_mode == 2 && scene_frame::SentinelSheet(scene_frame::HookFrameOf(guest_frame), every)) m.scene_sheet = true;   // S103 S2: a sentinel (walked) frame's mirror pair
    if (!options.dump_targets_frames.empty()) StageMirrorNote(m, guest_frame);   // S103 T7 (empty list = off): a listed frame (mode 2: the first sentinel walk at or after it) is a sheet frame
    if (keeper && (m.scene_sheet || m.scene_mode == 2)) m.scene_record = keeper->ForD3(guest_frame);   // (ForD3 does not move the keeper's cursor)
  }
  std::vector<uint8_t> shown_curve;  // render thread: the table the present applies (empty: identity), also applied to written pictures

  void Log(const char* line) const {
    if (options.log) options.log(line);
  }
  template <class B>
  void LogD3(const B& binder) const {   // lane 204 (O): the D3 census, readiness and record report (what fview prints at its end; session.cpp)
    if (d3::g_arena_d3) {   // lane 228: the arena's three lines only when the arena chain is on (the caster chain alone prints its own block below)
      Log(binder.D3Census().c_str());
      Log(binder.D3Readiness().c_str());
      Log(binder.D3RecordReport().c_str());
    }
    if (d3::g_caster_d3) Log(binder.D3CReport().c_str());   // lane 228 (the live seam, 17.12): the caster chain's census, readiness, reasons and the hooks' path histogram
    if (d3::g_ring_d3) Log(binder.D3RReport().c_str());     // lane 246 (the live seam, 16.9): the ring chain's census, readiness, reasons and the hook's path histogram
    if (d3::g_wrestlers_d3) Log(binder.D3WReportText().c_str());   // lane 245 (the live seam, 18.9): the wrestler chain's census, the verify line, the hooks' path histogram and the sphere offset census
  }

  bool WantPng(uint64_t guest_frame, uint64_t drawn_count) const {
    if (options.png_every && drawn_count % options.png_every == 0) return true;
    return std::find(options.png_frames.begin(), options.png_frames.end(), guest_frame) != options.png_frames.end();
  }

  void Run() {
    // S101 lane 35: the crowd's first promotion (declared before the binder: the binder goes first and points at it)
    std::unique_ptr<rdr_scene::CrowdOurs> crowd;
    if (options.crowd_ours) {
      rdr_scene::CrowdOursOptions co;
      co.game_root = options.crowd_game_root ? options.crowd_game_root() : std::string();
      co.floor = options.crowd_floor;  // S101 lane 82
      co.cards = options.crowd_cards;  // S101 lane CD
      co.cards_thin = options.crowd_cards && options.crowd_cards_thin;  // lane 175
      co.cards_thin_at = [](uint64_t f) { return wwe2k14::card_thin::g_counts.SkipAt(f); };   // S102 T2 / T13 review F1: the hook's run mode of that guest frame (--wwe2k14_rdr_card_cut), never sticky
      co.thin_offset = wwe2k14::card_thin::kHookToStreamOffset;
      co.thin_probe = options.crowd_cards && options.crowd_cards_thin_probe;   // SF2
      co.thin_counts = [](uint64_t f, uint32_t& seen, uint32_t& dropped, uint64_t& world) { return wwe2k14::card_thin::g_counts.Lookup(f, seen, dropped, world); };   // the hook's per-frame tally (review B1)
      co.member_probe = options.crowd_members_probe || options.crowd_members_thin;   // lane 272
      co.members_thin = options.crowd_members_thin;
      co.members_thin_at = [](uint64_t f) { return wwe2k14::member_thin::g_counts.SkipAt(f); };   // S102 T13 review F1: the hook's run mode of that guest frame (--wwe2k14_rdr_member_cut), never sticky
      co.member_offset = wwe2k14::member_thin::kHookToStreamOffset;
      co.member_counts = [](uint64_t f, int site, uint32_t& seen, uint32_t& dropped, uint64_t& world) { return wwe2k14::member_thin::g_counts.Lookup(f, site, seen, dropped, world); };   // the hook's per-frame tally of the site
      co.verify_every = 30;  // the +1 pairing is checked on every 30th frame (a census count, never assumed)
      crowd = std::make_unique<rdr_scene::CrowdOurs>(*renderer, co);
    }
    struct CrowdReportAtEnd {
      std::unique_ptr<rdr_scene::CrowdOurs>& c;
      const Impl& impl;
      void Print() const {  // (lane 216: called by the final flush below; the destructor no longer prints, the flush runs first at every exit)
        if (c) impl.Log(("[rdr][crowd] " + c->Report()).c_str());
      }
    } crowd_report{crowd, *this};
    // S101 lane PO: the post chain's promotion (declared before the binder like the crowd's hook: the binder goes first and points at it). No scene record needed.
    std::unique_ptr<rdr_scene::PostOurs> post;
    if (options.post_ours) {
      rdr_scene::PostOursOptions po;
      po.limits.render_scale = options.render_scale;
      post = std::make_unique<rdr_scene::PostOurs>(*renderer, po);
    }
    // S101 lane 41: the in-place replace hooks (arena, wrestlers), made once here when their switch is on and a class registered its factory. The crowd's hook goes first (own seam).
    std::vector<rdr_scene::ReplaceHookHandle> replace_hooks;
    const auto make_replace = [&](bool on, const char* name, rdr_scene::ReplaceFactory factory) -> rdr_scene::ReplaceHookHandle* {
      if (!on) return nullptr;
      if (!factory) {
        Log((std::string("[rdr][") + name + "] the switch is on but no class registered (nothing is replaced)").c_str());
        return nullptr;
      }
      rdr_scene::ReplaceFactoryArgs args;
      args.renderer = renderer.get();
      args.game_root = options.crowd_game_root ? options.crowd_game_root() : std::string();
      args.live = true;
      args.low_ram = options.ui_low_ram;   // lane 126
      args.image_info = image_info.Supplier();  // S101 lane 47: the live supplier (fed where the binder syncs its images, below)
      args.image_content = image_content.Supplier();  // S101 lane 66: the UI's content supplier (fed before each frame's contents are applied, below)
      rdr_scene::ReplaceHookHandle handle = factory(args);
      if (!handle.hook) {
        Log((std::string("[rdr][") + name + "] not started: " + handle.error).c_str());
        return nullptr;
      }
      replace_hooks.push_back(std::move(handle));
      return &replace_hooks.back();
    };
    replace_hooks.reserve(std::size(rdr_scene::kReplaceClasses));  // (the pointers above stay valid: one slot per class in the table; a new class cannot outgrow it: lane 84 review)
    rdr_scene::InstallReplaceFactories();
    rdr_scene::ReplaceHookHandle* arena_hook = make_replace(options.arena_ours, "arena", rdr_scene::g_arena_factory);
    rdr_scene::ReplaceHookHandle* wrestlers_hook = make_replace(options.wrestlers_ours, "wrestlers", rdr_scene::g_wrestlers_factory);
    rdr_scene::ReplaceHookHandle* caw_hook = make_replace(options.caw_ours, "caw", rdr_scene::g_caw_factory);   // lane 153 (N3)
    rdr_scene::ReplaceHookHandle* ring_hook = make_replace(options.ring_ours, "ring", rdr_scene::g_ring_factory);  // S101 ring contract
    rdr_scene::ReplaceHookHandle* flash_hook = make_replace(options.flash_ours, "flash", rdr_scene::g_flash_factory);  // S101 flash contract
    rdr_scene::ReplaceHookHandle* screens_hook = make_replace(options.screens_ours, "screens", rdr_scene::g_screens_factory);  // S101 lane 115 (another pass site: order does not matter)
    rdr_scene::ReplaceHookHandle* crowd_props_hook = make_replace(options.crowd_props_ours, "crowd_props", rdr_scene::g_crowd_props_factory);  // S101 lane 133 (the crowd loops' own call sites: order does not matter)
    rdr_scene::ReplaceHookHandle* props_hook = make_replace(options.props_ours, "props", rdr_scene::g_props_factory);  // S101 lane 84 (after flash, before ui)
    rdr_scene::ReplaceHookHandle* shadow_hook = make_replace(options.shadow_casters_ours, "shadow_casters", rdr_scene::g_shadow_casters_factory);  // S101 lane 99 (after props, before ui)
    rdr_scene::ReplaceHookHandle* movie_hook = make_replace(options.movie_ours, "movie", rdr_scene::g_movie_factory);  // S101 lane 131 (its own call site; before ui)
    rdr_scene::ReplaceHookHandle* rim_hook = make_replace(options.rim_ours, "rim_map", rdr_scene::g_rim_factory);  // lane 124 (after the shadow casters, before ui)
    rdr_scene::ReplaceHookHandle* circles_hook = make_replace(options.circles_ours, "circles", rdr_scene::g_circles_factory);  // lane 136 (after the rim map: its site is the circle call, no colour hook claims it)
    rdr_scene::ReplaceHookHandle* plane_hook = make_replace(options.plane_shadow_ours, "plane_shadow", rdr_scene::g_plane_shadow_factory);  // lane 139 (after the circles and the shadow casters: it takes the plane draws they pass on)
    rdr_scene::ReplaceHookHandle* small_passes_hook = make_replace(options.small_passes_ours, "small_passes", rdr_scene::g_small_passes_factory);  // S101 lane 156 review: lane 135's class was in the factory table but never made live (its switch only logged "switch on"); call sites of its own, order does not matter
    rdr_scene::ReplaceHookHandle* rim_chain_hook = make_replace(options.rim_chain_ours, "rim_chain", rdr_scene::g_rim_chain_factory);  // lane 151 (its own call sites 0x82C1F48C / 0x82814080; no colour hook claims them; before ui)
    rdr_scene::ReplaceHookHandle* ui_text_hook = make_replace(options.ui_text_ours, "ui_text", rdr_scene::g_ui_text_factory);  // S101 lane 89 (UT)
    rdr_scene::ReplaceHookHandle* ui_hook = make_replace(options.ui_ours, "ui", rdr_scene::g_ui_factory);  // S101 UI contract (lane US feeds args.image_content)
    bool wants_content = false;   // S101 lane 66: some registered hook reads the content table (the UI, the ring, the flashes): only then is it fed (no hashing with every switch off)
    for (const rdr_scene::ReplaceHookHandle& h : replace_hooks) wants_content = wants_content || h.wants_content;
    Binder binder(*renderer);
    if (crowd) binder.SetCrowdHook(crowd.get());
    if (post) binder.SetPostHook(post.get());  // S101 lane PO
    if (d3::g_arena_d3) binder.D3SetRecordRoute(d3::kRecordRouteBuilt, d3::g_verify, -1);   // lane 152: Mode::Record is the live default; the route is on only when built; the verify arm by its switch
    if (d3::g_caster_d3) {   // lane 228 (the live seam, 17.12): the caster chain on the live globals; D3CSetHooks stays uncalled on purpose (slots_used is built into the binder, palette_count is an optional override, the geometry test defaults to ready)
      binder.SetD3CasterLive(d3::kRecordRouteBuilt, d3::g_caster_verify, d3::g_caster_plane);
      Log(d3::g_caster_verify ? "[rdr][d3][casters] live chain on (VERIFY arm: markers written, the game's passes always run)" : "[rdr][d3][casters] live chain on (skip arm: a proven pass on cache path 1 is replaced by a marker)");
    }
    if (d3::g_ring_d3) {   // lane 246 (the live seam, 16.9): the ring chain on the ring-only live globals (d3_arena.h g_rpackets / g_rmarks / g_rgate / g_rtally); D3RSetHooks stays uncalled (the palette size is learned from the recorded-in-full frames' parts)
      binder.SetD3RingLive(d3::kRecordRouteBuilt, d3::g_ring_verify);
      Log(d3::g_ring_verify ? "[rdr][d3][ring] live chain on (VERIFY arm: markers written, the game's ring calls always run)" : "[rdr][d3][ring] live chain on (skip arm: a proven ring call on cache path 1 with its ring rows recorded is replaced by a marker)");
    }
    if (d3::g_wrestlers_d3) {   // lane 245 (the live seam, 18.9): the wrestler chain on its own live globals (g_w*), never the arena's / the ring's / the casters'
      binder.SetD3WrestlersLive(d3::kRecordRouteBuilt, d3::g_wrestlers_verify);
      Log(d3::g_wrestlers_verify ? "[rdr][d3][wrestlers] live chain on (VERIFY arm: markers written, the game's colour calls always run)" : "[rdr][d3][wrestlers] live chain on (skip arm: a proven wrestler call on cache path 1 is replaced by a marker)");
    }
    struct PostReportAtEnd {  // S101 lane PO (declared after the binder: it goes first, while the binder and the hook are alive)
      Binder& binder;
      const Impl& impl;
      bool on;
      void Print() const {
        if (on) impl.Log(binder.PostCensus().c_str());
      }
    } post_report{binder, *this, post != nullptr};
    struct D3ReportAtEnd {  // lane 204 (O): the D3 census, readiness and record lines at exit too (K1 had them only in fview); also every 600 drawn frames (below)
      Binder& binder;
      const Impl& impl;
      void Print() const {
        if (d3::AnyD3Chain()) impl.LogD3(binder);
      }
    } d3_report{binder, *this};
    if (arena_hook) binder.AddReplaceHook(arena_hook->hook.get(), &g_arena_ours);
    if (wrestlers_hook) binder.AddReplaceHook(wrestlers_hook->hook.get(), &g_wrestlers_ours);
    if (ring_hook) binder.AddReplaceHook(ring_hook->hook.get(), &g_ring_ours);  // after the arena and the wrestlers (both pass ring objects on as NotOurs)
    if (flash_hook) binder.AddReplaceHook(flash_hook->hook.get(), &g_flash_ours);  // S101 flash contract
    if (screens_hook) binder.AddReplaceHook(screens_hook->hook.get(), &g_screens_ours);  // S101 lane 115
    if (crowd_props_hook) binder.AddReplaceHook(crowd_props_hook->hook.get(), &g_crowd_props_ours);  // S101 lane 133
    if (props_hook) binder.AddReplaceHook(props_hook->hook.get(), &g_props_ours);  // S101 lane 84: after the ring (it passes these objects on as NotOurs)
    if (caw_hook) binder.AddReplaceHook(caw_hook->hook.get(), &g_caw_ours);   // lane 153: AFTER the wrestlers, the ring and the props (review B1)
    if (shadow_hook) binder.AddReplaceHook(shadow_hook->hook.get(), &g_shadow_casters_ours);  // S101 lane 99: the shadow passes' sites (no colour hook claims them); its joins read the wrestlers' and the ring's hooks
    if (movie_hook) binder.AddReplaceHook(movie_hook->hook.get(), &g_movie_ours);  // S101 lane 131: before the UI (which would keep the movie quad)
    if (rim_hook) binder.AddReplaceHook(rim_hook->hook.get(), &g_rim_ours);  // lane 124: the rim light map's site (no colour hook claims it); its joins read the wrestlers' hook
    if (circles_hook) binder.AddReplaceHook(circles_hook->hook.get(), &g_circles_ours);  // lane 136: the circle blob shadows (call 0x828131E4)
    if (plane_hook) binder.AddReplaceHook(plane_hook->hook.get(), &g_plane_shadow_ours);  // lane 139: the plane mask (site 0x82BF4D38) and blur (call 0x827EEC84) constants
    if (rim_chain_hook) binder.AddReplaceHook(rim_chain_hook->hook.get(), &g_rim_chain_ours);  // lane 151: the rim light map's reductions and blur (needs no scene record, no game file)
    if (small_passes_hook) binder.AddReplaceHook(small_passes_hook->hook.get(), &g_small_passes_ours);  // S101 lane 213 finding: lane 156 made the hook but never added it here, so the class was never asked (its own call sites: order does not matter)
    if (ui_text_hook) binder.AddReplaceHook(ui_text_hook->hook.get(), &g_ui_text_ours);  // S101 lane 89 (UT): before the UI textures' hook (a glyph draw ours laid out is not offered on)
    if (ui_hook) binder.AddReplaceHook(ui_hook->hook.get(), &g_ui_ours);  // S101 UI contract: last
    struct ReplaceReportAtEnd {  // (declared after the binder: it goes first, while the binder and the hooks are alive)
      Binder& binder;
      const Impl& impl;
      std::vector<rdr_scene::ReplaceHookHandle*> hooks;
      // S101 lane 83: the binder's cumulative replace-check ms per hook, for the [cost] line (live thread; one load when the switch is off).
      void PublishCost() const {
        if (!wwe2k14::cost::On()) return;
        for (rdr_scene::ReplaceHookHandle* h : hooks)
          if (const ReplaceCounters* c = binder.replace_counters(h->hook->Name())) wwe2k14::cost::PublishReplaceMs(wwe2k14::cost::ReplaceClassOf(h->hook->Name()), c->ms);
      }
      // S101 lane 216: the hooks' own end reports (the census lines themselves come from Binder::FlushCensus, same text as a window line).
      void Print() const {
        PublishCost();
        if (const std::string cost_line = wwe2k14::cost::FinalLine(wwe2k14::cost::TicksPerNs()); !cost_line.empty()) impl.Log(cost_line.c_str());
        for (rdr_scene::ReplaceHookHandle* h : hooks)
          if (h->report) impl.Log(("[rdr][" + std::string(h->hook->Name()) + "] " + h->report()).c_str());
      }
    } replace_report{binder, *this, {}};
    for (rdr_scene::ReplaceHookHandle* h : {arena_hook, wrestlers_hook, caw_hook /* lane 153 (review S4) */, ring_hook, flash_hook, props_hook, shadow_hook, screens_hook, crowd_props_hook, movie_hook, rim_hook /* lane 133 (rim, lane 124's omission) */, circles_hook /* lane 136 */, plane_hook /* lane 139 */, small_passes_hook /* lane 156 review */, rim_chain_hook /* lane 151 */, ui_text_hook, ui_hook})  // S101 lane 84 / 89 / 115
      if (h) replace_report.hooks.push_back(h);
    // S101 lane 216: every class's FINAL census line, once at the end of every normal run. The process hard-exits ("Title terminated; hard-exiting process"),
    // so the destructors never ran and a scene shorter than one 600-frame window (the victory scene, the last seconds of a match) printed nothing. This runs on
    // the render thread: from the SDK's quit flush (rex::RunQuitFlushes, before TerminateTitle), from the black box's exit zip (before it reads the log), and
    // from the destructor below on an orderly stop. Idempotent: it prints again only when frames were handled since the last print. Never throws.
    struct FinalCensus {
      Impl& impl;
      Binder& binder;
      std::function<void()> reports;  // the hooks' own reports, the crowd's, the post chain's, the D3 lines
      uint64_t printed_at = ~0ull;
      void Run(const char* why) {
        try {
          const uint64_t now = impl.drawn.load(std::memory_order_relaxed) + impl.skipped.load(std::memory_order_relaxed);
          if (now == printed_at) return;
          printed_at = now;
          impl.Log((std::string("[rdr][end] end of run (") + why + "): the lines below are each class's final census, same shape as a window line").c_str());
          census_flush::Raise("run", 0, +[](const char* l) { rdr::Log("%s", l); });   // S102 CEN1: the run-end marker (census sites that see another frame print too)
          binder.FlushCensus();
          if (reports) reports();
          if (impl.renderer) impl.renderer->FlushPipelineStore();  // lane 276: the process hard-exits, ~Renderer never runs: the pipeline list / cache are saved here (repeatable)
        } catch (...) {
        }
      }
      ~FinalCensus() { Run("stop"); }
    };
    FinalCensus final_census{*this, binder, [&] {
      replace_report.Print(); crowd_report.Print(); post_report.Print(); d3_report.Print();
      // S103 NRBF (review F4): the scene lines LiveSink::Stop prints are lost on the hard exit; every one of them is here too (lint: nrbf_census_lint.py)
      if (scene_draw) { Log(scene_draw->Census().c_str()); Log(scene_draw->ReadyCensusLine(true).c_str()); }
      Log(scene_frame::CensusLine().c_str());
      Log(scene_frame::missing_hist::MissingLine().c_str());
      Log(scene_kind::CensusLine().c_str());
      if (fc_handled) Log(fallback_census::g_census.FinalLine(fc_handled).c_str());
    }};
    rdr::FrameBuilder builder;
    LiveSource source;
    LoadedTexture content_scratch;  // S101 lane 66
    source.mu = &store_mu;
    source.geometry = &geometry;
    source.textures = &textures;
    rdr::ThreadCpuMeter render_thread_cpu;  // S102 S0-3: this thread's CPU time per loop pass (bind + parse + Render; its waits are not CPU) -> rdr::g_render_thread_ns_total, the CSV's render_thread_ms
    static const uint32_t test_bind_spin_us = [] { const char* e = std::getenv("RDR_TEST_BIND_SPIN_US"); return e ? uint32_t(std::atoi(e)) : 0u; }();  // test only: a busy spin before every Bind (render_thread_ms must rise by it)
    int announced_seen = -1;   // S101 lane 271: the arena the match card named, as last handed to the per-arena loads (a change is handed over once)
    // S102 R21-B (docs\renderer\STAGE_SPLIT.md, wwe2k14_rdr_stage_split): the record stage. Declared after the binder, the hooks and `builder`, so it is stopped
    // (its queued frames recorded) before any of them goes, and before FinalCensus's last flush. Its thread starts at the first split frame. The hooks run on
    // rdr_record: `before` stores the frame's renderer-read switches (hazard 7: one writer), the prebuild flag and the overlay (hazard 8); `after` the present note.
    namespace ss = stage_split;
    ss::RecordStage stage(
        *renderer,
        [this](const rdr::Frame&, const ss::Packet& p) {
          if (p.set_globals) {
            rdr::g_gpu_stamps.store(p.gpu_stamps, std::memory_order_relaxed);
            rdr::g_gpu_pass_stamps.store(p.gpu_pass_stamps, std::memory_order_relaxed);  // S102 R24-B
            rdr::g_gpu_outside.store(p.gpu_outside, std::memory_order_relaxed);
            rdr::g_upload_batch.store(p.upload_batch, std::memory_order_relaxed);
            rdr::g_fence_friendly.store(p.fence_friendly, std::memory_order_relaxed);
            rdr::g_copy_batch.store(p.copy_batch, std::memory_order_relaxed);
            rdr::g_render_lean.store(p.render_lean, std::memory_order_relaxed);   // S102 RT1
            rdr::g_stencil_dead.store(p.stencil_dead, std::memory_order_relaxed);   // S102 R24-C
            rdr::crumbs::g_on.store(p.crumbs, std::memory_order_relaxed), rdr::crumbs::g_marker.store(p.gpu_marker, std::memory_order_relaxed);  // S102 D1
            rdr::crumbs::g_frame_top_barrier.store(p.frame_top_barrier, std::memory_order_relaxed);  // S102 D1 round 6
          }
          if (p.set_prebuild) renderer->SetPipelinePrebuildAllowed(p.prebuild_allowed);
          for (const auto& [id, image] : p.overlay_images) renderer->SetOverlayImage(id, image);  // settings parity S11 (review F1): made + uploaded by the bind stage, sealed with this frame
          if (p.overlay_changed) renderer->SetOverlay(p.overlay_list);
        },
        [this](const rdr::Frame&, const ss::Packet& p) {
          if (p.note_present) NotePresent();
          // review F4: the bug button's timing.csv row of a split frame, from here: bind (rdr_render) + record (this thread), and the gap this present measured
          case_hook::OnRenderTiming(p.guest_frame, p.bind_ms + p.record_ms, p.note_present ? present_gap_ms.load(std::memory_order_relaxed) : -1.0);
        });
    uint64_t stage_cpu_seen = 0, stage_wall_seen = 0;   // the record stage's totals at the last K3 note / census window (bind stage only)
    for (;;) {
      rdr::g_render_thread_ns_total.fetch_add(render_thread_cpu.TakeNs(), std::memory_order_relaxed);  // S102 S0-3: the CPU the last pass used (the 100 ms wait below is not CPU)
      rdr::g_render_thread_passes.fetch_add(1, std::memory_order_relaxed);
      replace_report.PublishCost();  // S101 lane 83
      Message message;
      size_t waiting = 0;
      bool forced_skip = false;   // S103 G2: a message dropped from a run: skipped whatever the guards say
      bool have_message = false;
      std::filesystem::path now_path;  // S101 lane 19: a request for the last presented picture, served without a new frame
      {
        std::unique_lock lock(mail_mu);
        // (lane 216: a timed wait, so a quit request from another thread is seen even while no frame comes)
        mail_cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return stopping || !mail.empty() || !run_q.empty() /* S103 G2 */ || !last_picture_path.empty() || shot_pending.load(std::memory_order_acquire) || quit_req.load(std::memory_order_acquire); });
        now_path.swap(last_picture_path);
        if (quit_req.exchange(false, std::memory_order_acq_rel)) {
          lock.unlock();
          stage.Drain();  // S102 R21-B: the last flush saves the pipeline store (a renderer call): nothing may be recording
          stage.NoteFallback(ss::kFbQuit);
          final_census.Run("route quit / title termination");
          quit_ack.store(true, std::memory_order_release);
          lock.lock();
        }
        if (mail.empty() && run_q.empty() && now_path.empty() && !shot_pending.load(std::memory_order_acquire)) {
          if (stopping) return;  // stopping, nothing left
          continue;
        }
        if (!run_q.empty()) {   // S103 G2: the rest of the run popped last time: dropped messages, then the newest
          message = std::move(run_q.front());
          run_q.pop_front();
          forced_skip = !run_q.empty();             // every message of the run but the last is dropped
          waiting = (forced_skip || run_forced) ? 0 : mail.size();   // S103 G2 (review 3): the newest of a run that dropped messages is never queue-skipped by newcomers: waiting = 0, the next take handles them
          have_message = true;
        } else if (!mail.empty()) {
          rdr_adapter::MailRun<Message> run;
          rdr_adapter::TakeRun(mail, [&](const Message& m) {   // S103 G2: with the scene switch not 2 (or paced) every message is a Barrier: one at a time, as today
            if (m.scene_mode != 2 || m.read || !m.picture.empty() || paced) return rdr_adapter::MailKind::Barrier;
            return m.scene_sheet ? rdr_adapter::MailKind::Last : rdr_adapter::MailKind::Plain;
          }, run, &mail_stats);
          for (Message& d : run.dropped) run_q.push_back(std::move(d));
          run_q.push_back(std::move(run.newest));
          message = std::move(run_q.front());
          run_q.pop_front();
          forced_skip = !run_q.empty();
          run_forced = forced_skip;   // (the run dropped something: its newest gets waiting = 0)
          waiting = forced_skip ? 0 : mail.size();
          have_message = true;
        }
      }
      if (!now_path.empty()) {
      stage.Drain();  // S102 R21-B: the picture is read from the renderer (hazard 10)
      WritePictureTo(binder, now_path);
      // S101 lane 1 (review): say why a requested last picture is missing, instead of only "NO PICTURE" in the zip line 5 s later.
      std::error_code ec;
      if (!std::filesystem::exists(now_path, ec))
        Log(binder.last_present().source.image == rdr::kNoImage ? "[rdr][live] WARN last picture not written: nothing presented yet"
                                                               : "[rdr][live] WARN last picture not written: the presented image is not readable");
    }
      if (shot_pending.load(std::memory_order_acquire)) {   // S103 lane SHOT: a frame follows -> the copy rides it (no drain); no frame -> the picture shown is read now (the one drain)
        if (have_message) {
          QueueShot(binder);
        } else {
          PollAsync();
          stage.Drain();  // (hazard 10: the picture is read from the renderer)
          ServeShotSync(binder);
          if (shot_inflight) std::this_thread::sleep_for(std::chrono::milliseconds(1));   // (the copy lands within a few ms; no spin)
        }
      }
      if (!have_message) continue;  // only the picture request
      room_cv.notify_all();  // (a paced game waits for room: WaitForRoom)
      // A named picture is reported done however this frame ends (a route's quit waits for it).
      struct PictureDone {
        bool named;
        Impl* self;
        ~PictureDone() {
          if (named) self->NamedPictureDone();  // lane 302: after the queued picture writes (see pic_q)
        }
      } picture_done{!message.picture.empty(), this};
      // S102 lane KO (ko_render_drop, test only): the frame leaves the hand-off and is dropped at once - no image table, parse, contents, bind, record or present. The
      // swap thread never waits on us (the hand-off is unbounded; WaitForRoom is --pace_game only, checks), the mail is still drained, a named picture is still reported done
      // (picture_done above). The dropped frame's image table / texture bytes are lost: only for a run with the switch ON from the start (knockouts.h).
      if (rdr_sw::BoolSeen(rdr_sw::kKoRenderDrop, false, message.guest_frame)) {
        ko::KoOnce(1, "wwe2k14_rdr_ko_render_drop", [this](const char* n) { char l[160]; std::snprintf(l, sizeof(l), "[ko] %s on", n); Log(l); });
        if (message.read) {  // S102 KO review F1: a DrawAndRead part is answered (not ok), so the reader's blocking read-back never waits out its timeout
          { std::lock_guard<std::mutex> l(message.read->mu); message.read->ok = false; message.read->done = true; }
          message.read->cv.notify_all();
        }
        {  // S102 KO review F2: the dropped frames' texture bytes are freed here (only ApplyContents frees them); outside the lock (lane 273's rule)
          decltype(textures) freed;
          { std::lock_guard lock(store_mu); freed.swap(textures); }
        }
        static uint64_t ko_dropped = 0;
        if (++ko_dropped % 600 == 0) { char l[160]; std::snprintf(l, sizeof(l), "[ko] render_drop: %llu frames dropped", static_cast<unsigned long long>(ko_dropped)); Log(l); }
        continue;
      }
      // S102 T19: a record that carries crowd run markers (wwe2k14_rdr_crowd_run_markers) becomes the plain member list here, on this thread, before anything reads message.record / d3_record
      // (tools\lint\t19_lint.py checks the order). The table comes from this thread's pool of three (reused once nothing holds it) and is keyed by the record: the present's record is the
      // d3_record now and the record of the next message, and is found there, not expanded twice. A record with no markers is passed as it is (one empty() test).
      if (message.record && !message.record->crowd_runs.empty()) message.record = rdr_scene::ExpandedForReading(message.record);
      if (message.d3_record && !message.d3_record->crowd_runs.empty()) message.d3_record = rdr_scene::ExpandedForReading(message.d3_record);
      // Images first: a frame never names an image the table lacks. Releases after the frame that last used them (they came with a newer one).
      if (message.has_images) {
        source.records = std::move(message.images);
        image_info.Set(source.records.data(), source.records.size());  // S101 lane 47
        binder.SyncImages(source);
      }
      if (message.display_curve.size() == 1024) {
        renderer->SetDisplayCurve(message.display_curve.data());
        shown_curve = std::move(message.display_curve);
        Log("[rdr][live] display table from the game applied");
      }
      const auto t_start = std::chrono::steady_clock::now();
      LoadedFrame frame;
      frame.constant_banks.UsePool(&bank_pool);   // S102 T1: the pool serves this frame only (a LoadedFrame is made per message, so never two live frames on one pool); switch off = fresh banks, the pool unused
      bank_lazy = rdr_sw::BoolSeen(rdr_sw::kLazyBanks, false, message.guest_frame);   // S102 T1: read every frame (S0-4 table)
      bank_blocks = rdr_sw::BoolSeen(rdr_sw::kBankBlocks, false, message.guest_frame);   // S102 T1b: read every frame (S0-4 table)
      bool parsed;
      if (message.data) {   // S102 lane T8: the tables are bound where they are (message.data outlives `frame`: declared before it, dropped after it)
        handoff::BindInto(*message.data, frame, bank_lazy ? 1 : 0, bank_blocks ? 1 : 0);
        parsed = true;
      } else {
        frame.bytes = std::move(message.frame);
        parsed = CaptureReader::ParseFrame(frame, bank_lazy ? 1 : 0, bank_blocks ? 1 : 0);
      }
      if (!parsed) {
        rdr::Problem("live_frame_unreadable", "guest frame %llu", static_cast<unsigned long long>(message.guest_frame));
        continue;
      }
      // S102 C4: the arena overdraw switch, read per guest frame; the texture-alpha scan (binder.h ScanImageAlpha) runs in ApplyContents only while it is on
      const bool arena_overdraw_now = rdr_sw::BoolSeen(rdr_sw::kArenaOverdraw, false, message.guest_frame);
      const uint32_t arena_fragments_now = uint32_t(rdr_sw::IntSeen(rdr_sw::kArenaFragments, 0, message.guest_frame)) & rdr::arena_fragments::kAll;  // S102 R18-B3: per guest frame; mask bits 1 and 2 use the same texture-alpha proof
      const bool arena_scan_hold = rdr_sw::BoolSeen(rdr_sw::kArenaScanHold, false, message.guest_frame);   // S102 R24-D: the scan also while the switch is off (a flip of arena_overdraw then finds every texture proven)
      rdr::colour_cuts::g_arena_overdraw_scan.store(arena_overdraw_now || arena_scan_hold || (arena_fragments_now & 1) != 0, std::memory_order_relaxed);
      rdr::g_frame_facts.store(rdr_sw::BoolSeen(rdr_sw::kFrameFacts, rdr::g_frame_facts.load(std::memory_order_relaxed), message.guest_frame), std::memory_order_relaxed);   // S102 R21-A: facts derived once, read every frame
      binder.SetBlendKeysWanted(options.tween || options.dlss_mode >= 0);   // S102 R21-A: the only readers of a draw's blend key (the tween's halfway picture, the DLSS motion ticks); with the switch on and both off, BlendRanges does nothing
      // Contents always land, drawn or not: an image written once (a file texture) is never sent again.
      if (wants_content) FeedImageContents(frame, LiveFeedSource{source}, image_content, content_scratch);  // S101 lane 66: before ApplyContents takes the store's bytes
      const bool d3_chain_now = d3::AnyD3Chain() && message.d3_chain;   // S102 K3 rework: the chains run for this frame (never read from a live global: it was decided at submit)
      binder.d3_chain_frame = message.d3_chain;
      binder.D3NoteChain(message.d3_chain, message.guest_frame);   // S102 K3 review F1: the first chain frame after a gap is the resume frame (no mark survives it unproved)
      if (d3::AnyD3Chain()) ++(d3_chain_now ? d3t::g_frames_chain : d3t::g_frames_off);
      if (d3_chain_now && message.d3_record) {   // lane 246: + the ring chain (D3AddRecord feeds its core too);   // lane 228: the caster chain's records come the same way (D3AddRecord feeds both cores); lane 152 (D3-F, review B1): the present's own record feeds the record route's light / world sources, joined by the hook's stamp (stream frame j needs stamp j-1, written in record j)
        d3t::Scope t(d3t::kRecords);
        binder.D3AddRecord(message.d3_record);
        binder.D3EvictRecords(message.guest_frame);
      }
      // the latch messages: only on a frame the chains ran (a latch is set by a chain's End), and every 64th handled frame so a late one is still said
      const bool take_latches = d3_chain_now || (++latch_tick & 63) == 0;
      if (d3::AnyD3Chain() && take_latches) { d3t::Scope latch_scope(d3t::kLatch);
      if (take_latches && d3::g_arena_d3) {
        const std::string latch = binder.D3TakeLatchMessage();   // lane 152 (B2)
        if (!latch.empty()) { Log(latch.c_str()); if (options.on_d3_latch) options.on_d3_latch(latch.c_str()); }   // lane 301: the case ring takes the moment
      }
      if (take_latches && d3::g_caster_d3) {   // lane 228: the caster gate's latch message (D3CEndFrame)
        const std::string latch = binder.D3CTakeLatchMessage();
        if (!latch.empty()) { Log(latch.c_str()); if (options.on_d3_latch) options.on_d3_latch(latch.c_str()); }   // lane 301: the case ring takes the moment
      }
      if (take_latches && d3::g_ring_d3) {   // lane 246: the ring gate's latch message (D3REndFrame; it also clears g_ring_hook_on)
        const std::string latch = binder.D3RTakeLatchMessage();
        if (!latch.empty()) { Log(latch.c_str()); if (options.on_d3_latch) options.on_d3_latch(latch.c_str()); }   // lane 301: the case ring takes the moment
      }
      if (take_latches && d3::g_wrestlers_d3) {   // lane 245: the wrestler gate's latch message (D3WEndFrame)
        const std::string latch = binder.D3WTakeLatchMessage();
        if (!latch.empty()) { Log(latch.c_str()); if (options.on_d3_latch) options.on_d3_latch(latch.c_str()); }   // lane 301: the case ring takes the moment
      }
      }
      binder.ApplyContents(frame, source);
      {
        std::vector<std::vector<uint8_t>> freed;  // lane 273: texture bytes are freed after the lock (the game thread's Append waits on store_mu)
        {
          std::lock_guard lock(store_mu);
          struct Hold3 { StoreTimer t{3}; ~Hold3() { t.Stop(); } } hold3;
          for (const ContentRef& ref : frame.contents) {
            if (auto it = textures.find(ref.offset); it != textures.end()) {
              freed.push_back(std::move(it->second));
              textures.erase(it);
            }
          }
        }
      }
      window.parse += Ns(t_start);
      ++window.frames;
      // S102 R21-B (STAGE_SPLIT.md section 1): this frame's path, decided before anything below touches the renderer. Split: bound here into the record
      // stage's slot and handed over (rdr_record records it). Otherwise the stage is drained and the frame runs on this thread exactly as before (the
      // frames that read the renderer's result: hazards 9, 10). Read every frame (PerFrame).
      bool split_frame = false;
      {
        ss::Inputs si;
        si.on = rdr_sw::BoolSeen(rdr_sw::kStageSplit, false, message.guest_frame);
        si.tween = options.tween;
        si.dlss = options.dlss_mode >= 0;
        si.picture = !message.picture.empty() || message.scene_sheet || WantPng(message.guest_frame, drawn.load(std::memory_order_relaxed) + 1);   // S103 lane S: a mirror sheet frame reads the picture back
        { std::lock_guard lock(overlay_mu); si.photo = !photo_path.empty(); }
        si.readback = !async_tags.empty() || !stage_tags.empty() || StagePageDue(message.guest_frame) || message.stage_mirror || AsyncDue(frame, message.guest_frame, message.seq);   // S103 L12: a stage page dump is read by the frame that queues it (never split)
        si.read_part = message.read != nullptr;
        si.ko = rdr_sw::BoolSeen(rdr_sw::kKoDrawNone, false, message.guest_frame);
        const int decision = ss::Decide(si);
        split_frame = decision == -1;
        if (!split_frame) {
          if (decision >= 0) stage.NoteFallback(decision);
          stage.Drain();   // nothing is recording while this thread uses the renderer
        }
      }
      // Lane 276 (SKIP_GAME_WORK_MAP 19.10): the listed pipelines are built in the menus / loading only. A scene is live while this frame has a scene record or the match card names an arena (the in-game sequence); the renderer then builds nothing from the list in slices (what is left stays on demand).
      const bool prebuild_now = !message.record && !(options.announced_arena && options.announced_arena() >= 0);
      if (!split_frame) renderer->SetPipelinePrebuildAllowed(prebuild_now);   // S102 R21-B: a split frame carries it to the record stage (hazard 7)
      if (arena_hook || wrestlers_hook || caw_hook || ring_hook || flash_hook || props_hook || shadow_hook || screens_hook || crowd_props_hook || rim_hook /* lane 133 (rim, lane 124's omission) */ || circles_hook /* lane 136 */ || plane_hook /* lane 139 */ || ui_text_hook || ui_hook) {  // S101 lane 115: screens_hook  // S101 lane 41: the same record (shared, read only) for the replace hooks; the game folder may only be known now
        for (rdr_scene::ReplaceHookHandle* h : {arena_hook, wrestlers_hook, caw_hook /* lane 153 (review S4) */, ring_hook, flash_hook, props_hook, shadow_hook, screens_hook, crowd_props_hook, movie_hook, rim_hook /* lane 133 (rim, lane 124's omission) */, circles_hook /* lane 136 */, plane_hook /* lane 139 */, small_passes_hook /* lane 156 review */, rim_chain_hook /* lane 151 */, ui_text_hook, ui_hook}) {  // S101 lane 84 / 89 / 115
          if (!h) continue;
          if (h->set_game_root) {
            if (const std::string root = options.crowd_game_root ? options.crowd_game_root() : std::string(); !root.empty()) h->set_game_root(root);
          }
          if (h->set_record) h->set_record(message.record);
        }
      }
      if (crowd) {  // S101 lane 35: the record this frame's crowd is drawn from (null: none yet, the stream's crowd stays); the game folder may only be known now
        if (const std::string root = options.crowd_game_root ? options.crowd_game_root() : std::string(); !root.empty()) crowd->SetGameRoot(root);
        crowd->SetRecord(std::move(message.record));
      }
      if (const int announced = options.announced_arena ? options.announced_arena() : -1; announced != announced_seen) {   // S101 lane 271 (SKIP_GAME_WORK_MAP 19.8)
        announced_seen = announced;
        if (announced >= 0) {   // the match card names the arena before any record does: every per-arena load starts on its worker now (the game folder was handed over above)
          for (rdr_scene::ReplaceHookHandle* h : {arena_hook, ring_hook, flash_hook, props_hook, screens_hook, crowd_props_hook})
            if (h && h->announce_arena) h->announce_arena(announced);
          if (crowd) crowd->Announce(announced);
          rdr::Log("[rdr][live] match card names arena %d: the per-arena loads start now", announced);
        }
      }
      if (!split_frame) PollAsync();  // lane 232: read-back copies whose frame's fence has signalled (never waits). S102 R21-B: a split frame has none in flight (else it is not split)
      if (message.read) {  // a frame's first part, split at a copy the game reads back (DrawAndRead): drawn, kept, read, never shown
        ReadRequest& request = *message.read;
        // render-clock: perf - stamps a bind request for its latency counters
        request.taken = std::chrono::steady_clock::now();
        BindStats stats;
        builder.Begin(message.guest_frame);
        binder.frame_part_open = true;  // S101 lane 50: more of this guest frame follows
        binder.Bind(frame, source, builder, stats);
        binder.frame_part_open = false;
        rdr::Frame part = builder.Finish();
        part.keep_contents = true;
        renderer->RenderFrame(part);
        // render-clock: perf - stamps a bind request for its latency counters
        request.drawn = std::chrono::steady_clock::now();
        draws += stats.draws;
        bound += stats.bound;
        no_shader += stats.no_shader;
        ++parts;
        rdr::ImageRef ref;
        ref.image = binder.Mapped(request.image);
        ref.mip = uint8_t(request.mip);
        ref.layer = uint8_t(request.layer);
        std::vector<uint8_t> rgba;
        uint32_t width = 0, height = 0;
        const bool ok = ref.image != rdr::kNoImage && renderer->ReadImage(ref, rgba, width, height);
        // render-clock: perf - stamps a bind request for its latency counters
        request.read = std::chrono::steady_clock::now();
        {
          std::lock_guard lock(request.mu);
          request.ok = ok;
          request.rgba = std::move(rgba);
          request.width = width;
          request.height = height;
          request.done = true;
        }
        request.cv.notify_all();
        continue;
      }
      streaks.by_image = rdr_sw::BoolSeen(rdr_sw::kPerframeByImage, options.perframe_by_image, message.guest_frame);  // S102 S0-4: the switches are read per frame
      // S102 R21-B (hazard 7): the five switches the renderer reads are computed here; on a split frame they travel with the frame and the record stage stores
      // them right before its Render (one writer), so frame N never records with frame N+1's values.
      ss::Packet render_globals;
      render_globals.gpu_stamps = rdr_sw::BoolSeen(rdr_sw::kGpuStamps, rdr::g_gpu_stamps.load(std::memory_order_relaxed), message.guest_frame);  // S102 S0-3: the per-frame switch (rplay keeps its own value until the game refreshes the table)
      render_globals.gpu_pass_stamps = rdr_sw::BoolSeen(rdr_sw::kGpuPassStamps, rdr::g_gpu_pass_stamps.load(std::memory_order_relaxed), message.guest_frame);  // S102 R24-B: the per-step table; it forces the stamps on
      render_globals.gpu_stamps = render_globals.gpu_stamps || render_globals.gpu_pass_stamps;
      if (!split_frame) rdr::g_gpu_stamps.store(render_globals.gpu_stamps, std::memory_order_relaxed), rdr::g_gpu_pass_stamps.store(render_globals.gpu_pass_stamps, std::memory_order_relaxed);
      {  // S102 lane G3: the four map-turn switches, read every frame (rdr_switches.h); they apply under LOW only (the resolved tier), like the G1 levers
        namespace mt = rdr_adapter::map_turns;
        mt::g_on[mt::kWrestler].store(options.low_tier && rdr_sw::BoolSeen(rdr_sw::kTurnWrestler, false, message.guest_frame), std::memory_order_relaxed);
        mt::g_on[mt::kRim].store(options.low_tier && rdr_sw::BoolSeen(rdr_sw::kTurnRim, false, message.guest_frame), std::memory_order_relaxed);
        mt::g_on[mt::kProps].store(options.low_tier && rdr_sw::BoolSeen(rdr_sw::kTurnProps, false, message.guest_frame), std::memory_order_relaxed);
        // (the pre-light turn is not exposed: review F6, until G4's lazy re-bake exists; fview --turns 8 still plans it)
        mt::g_half_rate_drawn_clock.store(rdr_sw::BoolSeen(rdr_sw::kHalfRateDrawnClock, false, message.guest_frame), std::memory_order_relaxed);   // step 1
      }
      render_globals.gpu_outside = rdr_sw::BoolSeen(rdr_sw::kGpuOutside, rdr::g_gpu_outside.load(std::memory_order_relaxed), message.guest_frame);  // S102 G5: per frame (rplay keeps its own value until the game refreshes the table)
      render_globals.upload_batch = rdr_sw::BoolSeen(rdr_sw::kUploadBatch, rdr::g_upload_batch.load(std::memory_order_relaxed), message.guest_frame);  // S102 G5
      if (!split_frame) rdr::g_gpu_outside.store(render_globals.gpu_outside, std::memory_order_relaxed), rdr::g_upload_batch.store(render_globals.upload_batch, std::memory_order_relaxed);
      rdr_scene::props_budget::Set(rdr_sw::BoolSeen(rdr_sw::kPropsBudget, rdr_scene::props_budget::g_on.load(std::memory_order_relaxed), message.guest_frame), rdr_sw::IntSeen(rdr_sw::kPropsBudgetBytes, rdr_scene::props_budget::g_bytes.load(std::memory_order_relaxed), message.guest_frame), rdr_sw::IntSeen(rdr_sw::kPropsBudgetUs, rdr_scene::props_budget::g_us.load(std::memory_order_relaxed), message.guest_frame));  // S102 T17: per frame
      render_globals.fence_friendly = rdr_sw::BoolSeen(rdr_sw::kFenceFriendly, rdr::g_fence_friendly.load(std::memory_order_relaxed), message.guest_frame);  // S102 H2: the per-frame switch
      if (!split_frame) rdr::g_fence_friendly.store(render_globals.fence_friendly, std::memory_order_relaxed);
      const uint32_t max_queued_now = uint32_t(std::max<int64_t>(1, rdr_sw::IntSeen(rdr_sw::kMaxQueued, options.max_queued, message.guest_frame)));  // S102 S0-4
      streaks.guard_decay = rdr_sw::BoolSeen(rdr_sw::kGuardDecay, false, message.guest_frame);   // S102 K1 (piece B): the state guard decays (lasting_prune.h); read every frame
      streaks.lean = rdr_sw::BoolSeen(rdr_sw::kSkipLean, false, message.guest_frame);   // S102 K1 (piece A): the guard walk on flat arrays
      streaks.Note(frame);
      const bool ko_draw_none = rdr_sw::BoolSeen(rdr_sw::kKoDrawNone, false, message.guest_frame);  // S102 lane KO: test only (knockouts.h); read here, used below in this frame only
      if (ko_draw_none) ko::KoOnce(0, "wwe2k14_rdr_ko_draw_none", [this](const char* n) { char l[160]; std::snprintf(l, sizeof(l), "[ko] %s on", n); Log(l); });
      if (wwe2k14::caw::g_on.load(std::memory_order_relaxed))  // S101 lane 118: `!caw on` - every frame the sink parses (drawn or skipped), whole frames only (a read-back part returned above)
        wwe2k14::caw::OnFrame(frame, [&source](uint64_t offset, uint32_t bytes, std::vector<uint8_t>& out) { return source.ReadGeometry(offset, bytes, out); });
      // S100 (strip-360 A1): a frame is skipped when newer frames wait (latest wins), never so long that nothing reaches the screen.
      const auto shown_at = last_present.load(std::memory_order_relaxed);  // S102 R21-B: the record stage may write it
      const bool starved = options.window && shown_at != std::chrono::steady_clock::time_point{} &&
                           // render-clock: ctl - 100 ms since the last present forces a draw (liveness of our own loop)
                           std::chrono::steady_clock::now() - shown_at > std::chrono::milliseconds(100);
      // Settings parity S9 (Jarrod 08:1x: "120fps absolutely stays as long as a decouplement is there properly, same for 30fps"): the 30
      // fps mode keeps the game's 60 updates and draws a frame once draw_vblanks vblanks of presents have passed; the frames between are
      // handled as skipped ones (their lasting work runs, their state steps are caught up by the drawn frame: A1b).
      bool rate_skip = false;
      if (options.draw_vblanks > 1 && message.picture.empty()) {
        vblank_debt += std::max<uint32_t>(1, frame.header.present_interval);
        if (forced_skip) {   // S103 G2 (review 2): a dropped message adds its interval and decides nothing (saturated: the newest of the run then always draws)
          vblank_debt = std::min<uint32_t>(vblank_debt, options.draw_vblanks);
        } else if (vblank_debt < options.draw_vblanks) {
          rate_skip = true;
        } else {
          vblank_debt = 0;
        }
      }
      copy_alias::g_fullsize = copy_alias::ClampLevel(int(rdr_sw::IntSeen(rdr_sw::kAliasFullsize, copy_alias::g_fullsize, message.guest_frame)));   // S102 G8: the full-size alias rule, read every frame (this thread is its only reader)
      copy_alias::g_small = copy_alias::ClampSmall(int(rdr_sw::IntSeen(rdr_sw::kAliasSmall, copy_alias::g_small, message.guest_frame)));   // S102 G12: the class-mask override, read every frame (this thread is its only reader)
      copy_alias::g_shadow_direct = rdr_sw::BoolSeen(rdr_sw::kShadowDirect, copy_alias::g_shadow_direct, message.guest_frame);   // S102 C3: shadow direct, read every frame (this thread is its only reader)
      copy_alias::g_frame_direct = rdr_sw::BoolSeen(rdr_sw::kFrameDirect, copy_alias::g_frame_direct, message.guest_frame);   // S102 C3: frame direct, read every frame
      render_globals.copy_batch = rdr_sw::BoolSeen(rdr_sw::kCopyBatch, rdr::g_copy_batch.load(std::memory_order_relaxed), message.guest_frame);   // S102 G12: the batch switch, per frame
      if (!split_frame) rdr::g_copy_batch.store(render_globals.copy_batch, std::memory_order_relaxed);
      {   // S102 RT1 (wwe2k14_rdr_render_lean): bits 1 and 4 are read by the render thread (the packet carries them to a split frame's record stage), bits 2 and 4 by the hooks on this thread
        const int lean_now = int(rdr_sw::IntSeen(rdr_sw::kRenderLean, rdr::g_render_lean.load(std::memory_order_relaxed) | rdr::g_lean_hooks.load(std::memory_order_relaxed), message.guest_frame));
        render_globals.render_lean = lean_now & (rdr::kLeanSets | rdr::kLeanHandoff);
        if (!split_frame) rdr::g_render_lean.store(render_globals.render_lean, std::memory_order_relaxed);
        rdr::g_lean_hooks.store(lean_now & (rdr::kLeanFacts | rdr::kLeanHandoff), std::memory_order_relaxed);
        streaks.note_flat = (lean_now & rdr::kLeanHandoff) != 0;   // lasting_prune.h Note: flat rectangle storage
      }
      render_globals.stencil_dead = rdr_sw::BoolSeen(rdr_sw::kStencilDead, rdr::g_stencil_dead.load(std::memory_order_relaxed), message.guest_frame);   // S102 R24-C: stencil liveness, per frame
      if (!split_frame) rdr::g_stencil_dead.store(render_globals.stencil_dead, std::memory_order_relaxed);
      render_globals.crumbs = rdr_sw::BoolSeen(rdr_sw::kBreadcrumbs, rdr::crumbs::g_on.load(std::memory_order_relaxed), message.guest_frame);   // S102 D1: the lost-device ring, per frame
      render_globals.gpu_marker = int(rdr_sw::IntSeen(rdr_sw::kGpuMarker, rdr::crumbs::g_marker.load(std::memory_order_relaxed), message.guest_frame));   // S102 D1: the GPU marker words, per frame
      render_globals.frame_top_barrier = int(rdr_sw::IntSeen(rdr_sw::kFrameTopBarrier, rdr::crumbs::g_frame_top_barrier.load(std::memory_order_relaxed), message.guest_frame));   // S102 D1 round 6
      if (!split_frame) rdr::crumbs::g_on.store(render_globals.crumbs, std::memory_order_relaxed), rdr::crumbs::g_marker.store(render_globals.gpu_marker, std::memory_order_relaxed),
                        rdr::crumbs::g_frame_top_barrier.store(render_globals.frame_top_barrier, std::memory_order_relaxed);
      rdr::crumbs::g_message.store(rdr_sw::BoolSeen(rdr_sw::kDeviceLostMessage, rdr::crumbs::g_message.load(std::memory_order_relaxed), message.guest_frame), std::memory_order_relaxed);   // S102 D1: the player's message (frame_stats.h shows it; not read by the renderer)
      render_globals.set_globals = true;   // S102 R21-B: the packet of a split frame (below)
      render_globals.set_prebuild = true, render_globals.prebuild_allowed = prebuild_now;
      render_globals.guest_frame = message.guest_frame;
      rdr::g_combine_lite.store(rdr::ClampCombineLite(int(rdr_sw::IntSeen(rdr_sw::kCombineLite, rdr::g_combine_lite.load(std::memory_order_relaxed), message.guest_frame))), std::memory_order_relaxed);   // S102 R18-B2: the combine's lite level, per frame
      rdr::g_gpu_small.store(rdr::ClampGpuSmall(int(rdr_sw::IntSeen(rdr_sw::kGpuSmall, rdr::g_gpu_small.load(std::memory_order_relaxed), message.guest_frame))), std::memory_order_relaxed);   // S102 R24-E: the small GPU passes' mask, per frame
      copy_alias::g_direct = copy_alias::ClampDirect(rdr_sw::BoolSeen(rdr_sw::kCopyDirect, copy_alias::g_direct > 0, message.guest_frame) ? 1 : 0);   // S102 C5: the direct switch, per frame
      rdr_scene::live::crowd::g_sign_file_pictures.store(rdr_sw::BoolSeen(rdr_sw::kSignFilePictures, rdr_scene::live::crowd::g_sign_file_pictures.load(std::memory_order_relaxed), message.guest_frame), std::memory_order_relaxed);   // S103 CSF: the reader and the applier follow this one switch
      g_crowd_fast = rdr_sw::BoolSeen(rdr_sw::kCrowdFast, g_crowd_fast, message.guest_frame);   // S102 R21-C: the crowd rank work done once and flat, per frame
      // S102 G2 (review F4): the output levers, read per guest frame from the S0-4 table (still LOW only), carried on the frame: the render thread never reads the cvar
      const bool g2_low = rdr::g_low_tier.load(std::memory_order_relaxed);
      const uint32_t out_cap_now = rdr_adapter::low_levers::OutputCapHeight(int(rdr_sw::IntSeen(rdr_sw::kLowOutputCap, 0, message.guest_frame)), g2_low);
      const bool out_bilinear_now = rdr_adapter::low_levers::PresentBilinear(rdr_sw::BoolSeen(rdr_sw::kLowPresentBilinear, false, message.guest_frame), g2_low);
      const uint32_t colour_cuts_now = rdr_adapter::low_levers::ColourCuts(int(rdr_sw::IntSeen(rdr_sw::kLowColourCuts, 0, message.guest_frame)), g2_low);  // S102 G10: per frame, under LOW
      const bool lasting_draw_on = rdr_sw::BoolSeen(rdr_sw::kLastingDraw, false, message.guest_frame);   // S102 T3 F3: read every frame (a window with no overload still logs `[switch] seen`)
      const int64_t lasting_draw_pct = rdr_sw::IntSeen(rdr_sw::kLastingDrawPct, 50, message.guest_frame);
      const bool nogroup_now = rdr_sw::BoolSeen(rdr_sw::kLastingNoGroup, false, message.guest_frame);   // S102 K2 (R16 piece C): no model-part group rule on a lasting bind; read every frame
      const bool skip_wanted = forced_skip || ((waiting >= max_queued_now || rate_skip) && message.picture.empty() && !message.scene_sheet /* S103 lane S */ && !starved);   // S103 G2 (A2, A5): a dropped message skips, whatever the guard says; a named picture is always drawn
      lasting_draw::Verdict lasting_verdict;
      bool lasting_drawn_now = false;  // S102 T3 (a): decided below, once the lasting bind's size is known
      if (skip_wanted) {
        if (message.stage_mirror) Log(("[rdr][stage_mirror] listed frame " + std::to_string(message.stage_mirror_want) + " not drawn: guest frame " + std::to_string(message.guest_frame) + " was skipped (forced skip); the dump is lost").c_str());   // S103 T7 F5
        // S100: a skipped frame still does the work that lasts beyond it (copies into textures and the passes they need: an attire's
        // build and pack during a load, when most frames are skipped; Heenan's jacket came out black without it, run 6911). Strip-360 A1:
        // only that: a copy into the same image and rectangle in each of the last kPerFrameStreak frames handled is per-frame work (shadow
        // maps, reflections, the post chain: ~43 a frame in a match) that the drawn frame redoes, so it is left out; with nothing lasting
        // left the frame costs its contents only. (Runs 6951 / Surface 177: lasting work ran ~2-3 ms a skipped frame, the queue never
        // drained and the screen starved.)
        // A per-frame copy whose image this frame reads before the copy writes it carries state from frame to frame (an exposure or
        // adaptation value nudged each frame). Left out, that state advances on drawn frames only (run 6952: 1-2 levels darker in an
        // outrun fast-forward); kept (`keep_state_copies`), the frame's whole scene feeds it and the skip saves nothing (run 6953: 10.6
        // frames a second shown). Default: left out (a GPU that cannot keep up drops frames, never slow motion: Jarrod); checks pace the
        // game instead (--wwe2k14_rdr_pace_game). (Two images used in turn are written every other frame: never per-frame by the streak.)
        bool lasting = false;
        streaks.images = &source.images();   // lane 307: kUsageLasting destinations are never dropped by the image rule
        streaks.defer_owed = true;   // S102 K2 (review F4): CommitOwed() below, on the really-skipped branch
        left_out_copies.clear();   // S102 T3 (a): noted below, only when the frame really is skipped (a frame drawn instead does its own state steps)
        streaks.Classify(frame, rdr_sw::BoolSeen(rdr_sw::kSkipKeepsState, options.keep_state_copies, message.guest_frame), lasting_seed, [&](const rdr::CopyOp& c) {
          left_out_copies.push_back(c);
        });
        if (rdr_sw::BoolSeen(rdr_sw::kRunEndKeep, false, message.guest_frame)) {   // S102 K2 round 2: the run-end recovery (a queue skip knows the next frame's copy table)
          if (!left_out_copies.empty()) {   // round 3 F3: nothing left out, nothing to peek for
            std::vector<rdr::CopyOp> next;
            if (PeekNextCopies(next)) {   // round 3 F4: whenever a next frame is queued (a rate skip with a backlog too)
              ++run_end_peeks;
              const std::vector<uint32_t> kept = streaks.KeepRunEnds(frame, lasting_seed, next);
              if (!kept.empty()) {
                run_end_kept += kept.size();
                left_out_copies.erase(std::remove_if(left_out_copies.begin(), left_out_copies.end(), [&](const rdr::CopyOp& c) { return std::find(kept.begin(), kept.end(), uint32_t(c.dest.image)) != kept.end(); }), left_out_copies.end());
              }
            } else if (rate_skip) {
              ++run_end_rate_skips;   // the 30 fps rate skip with nothing queued has no next frame: unchanged, counted
            } else {
              ++run_end_no_next;
            }
          }
        }
        for (uint8_t v : lasting_seed) lasting |= v != 0;
        // lane 307: bind only what the lasting copies need (lasting_prune.h); `dropped_ops` is the rest (their D3 packets are discarded below, as a skipped frame's)
        lasting::Stats prune{};
        bool pruned_now = false;   // S102 K2: the pruner ran (its kept_mask is this frame's)
        if (lasting && rdr_sw::BoolSeen(rdr_sw::kSkipPrune, options.prune, message.guest_frame)) {
          pruner.group = !nogroup_now;
          pruner.lasting_targets = rdr_sw::BoolSeen(rdr_sw::kLastingNoGroupTargets, false, message.guest_frame);   // S102 K2 (review F4)
          pruner.images = &source.images();   // S102 K2: the lasting render targets (a bake's product read later straight from the target)
          pruner.Select(frame, lasting_seed, lasting_ops, dropped_ops, prune, options.prune_plant);
          pruned_now = true;
        } else if (lasting) {   // --wwe2k14_rdr_skip_prune=false: the pre-lane-307 bind (every op but the per-frame copies)
          lasting_ops.clear();
          dropped_ops.clear();
          for (uint32_t i = 0; i < frame.ops.count; ++i) {
            if (frame.ops[i].kind == OpKind::Copy && !lasting_seed[i]) dropped_ops.push_back(frame.ops[i]);
            else lasting_ops.push_back(frame.ops[i]);
          }
          prune.ops = frame.ops.count, prune.kept_ops = uint32_t(lasting_ops.size());
          prune.draws = prune.kept_draws = frame.draws.count;
        }
        {   // S102 T3 (a): more than pct % of the draws kept by the lasting bind: the bind costs most of a drawn frame and shows nothing, so draw and show the frame
          lasting_draw::Inputs in;
          in.on = lasting_draw_on && !forced_skip;   // S103 G2 (review 1): a dropped message is never drawn instead; Resolve then owes its per-frame copies
          in.pct = lasting_draw_pct;
          in.waiting = uint32_t(waiting);
          in.max_queued = max_queued_now;
          in.rate_skip = rate_skip;
          in.lasting = lasting;
          in.draws = prune.draws;
          in.kept_draws = prune.kept_draws;
          lasting_verdict = lasting_draw::Resolve(true, in);   // F4: the tested decision (draw instead? are the per-frame copies owed?)
          lasting_drawn_now = lasting_verdict.draw;
        }
        if (lasting_drawn_now) {
          ++lasting_drawn;   // not a skipped frame: the draw path below binds every draw of it (counts: lasting_draw_test.cpp)
          lasting_drawn_draws += prune.draws;
          lasting_drawn_kept += prune.kept_draws;
        } else {
        ++skipped;
        if (message.scene_mode == 2) scene_frame::NoteStream(message.guest_frame, false, /*render_skipped=*/true);   // S103 CLK2 (review H2): the render side dropped the whole frame: counted apart (render_skipped); run 38 never reached SceneOurs for most streams ([scene][kind] saw 119 match frames of ~15000)
        streaks.CommitOwed();   // S102 K2 (review F4): the copies this frame left out are owed only now that it really is skipped (a T3 draw-instead frame owes nothing)
        if (lasting_verdict.owe_missed) for (const rdr::CopyOp& c : left_out_copies) {
          ++per_frame_copies_left;
          NoteMissed(c);  // A1b: the drawn frame catches its state step up
        }
        // lane 204 (P, Check K1): the D3 packets of a skipped frame are discarded ONLY when no lasting bind follows. A lasting bind keeps every draw, the markers included, and its BuildMini takes the packets;
        // discarding them first (as lane 152 did) made every marker of that bind a hole (run 7093: 346 skipped frames ran lasting work). Uncounted either way (not `unconsumed`, which would latch D3 off).
        if (lasting && d3_chain_now && !dropped_ops.empty()) {   // lane 307: the packets of the markers left out of the lasting bind are discarded (uncounted); the kept ones are taken by the bind
          const Table<Op> all = frame.ops;
          frame.ops = Table<Op>{dropped_ops.data(), uint32_t(dropped_ops.size())};
          binder.D3FrameSkipped(frame);
          frame.ops = all;
        }
        if (lasting) {   // lane 307 census
          ++prune_frames;
          prune_ops_in += prune.ops, prune_ops_kept += prune.kept_ops, prune_draws_in += prune.draws, prune_draws_kept += prune.kept_draws;
          prune_passes_in += prune.passes, prune_passes_kept += prune.kept_passes, prune_grouped += prune.grouped_kept, prune_seeds += prune.seeds, prune_promoted += prune.promoted_copies, prune_blind += prune.d3_blind() ? 1 : 0;
          window.prune_draws_in += prune.draws, window.prune_draws_kept += prune.kept_draws;
          window.obj_passes += prune.object_passes, window.obj_kept += prune.object_passes_kept, window.obj_dropped += prune.object_passes_dropped, window.lasting_targets += prune.lasting_target_passes, window.blind += prune.d3_blind() ? 1 : 0;   // S102 K2 census (plain adds, per window)
        }
        if (!lasting && d3_chain_now) binder.D3FrameSkipped(frame);   // lane 246: + the ring's markers (D3FrameSkipped already takes them)   // lane 228: a skipped frame's caster packets are discarded the same way (D3FrameSkipped takes both classes' markers)
        if (lasting) {
          const auto t_kept = std::chrono::steady_clock::now();
          const Table<Op> all_ops = frame.ops;
          frame.ops = Table<Op>{lasting_ops.data(), uint32_t(lasting_ops.size())};
          BindStats stats;
          rdr::FrameBuilder& lb = split_frame ? stage.Acquire() : builder;   // S102 R21-B: the record stage's slot on a split frame
          lb.Begin(message.guest_frame);
          binder.frame_part_open = true;  // S101 lane 50: a persistent-only pass is no guest frame for the crowd gate
          binder.persist_only_bind = true;  // S101 lane 224: the post hook plans nothing on it (its per-frame copies are left out: every chain input would read "not written this frame")
          binder.d3_skip_observe = prune.d3_blind();   // lane 307: no object pass kept: the D3 / thin begin and end do not run (as a skip with no bind)
          if (pruned_now && nogroup_now) binder.ord_full_ops = &all_ops, binder.ord_keep = &pruner.kept_mask;   // S102 K2: the D3 / thin ordinals and guards count over the whole frame's ops, not the bound ones
          binder.Bind(frame, source, lb, stats);
          binder.ord_full_ops = nullptr, binder.ord_keep = nullptr;
          binder.d3_skip_observe = false;
          binder.frame_part_open = false;
          binder.persist_only_bind = false;
          if (!split_frame) QueueAsync(frame, binder, message.guest_frame, message.seq);  // lane 232: a demanded picture is read after this frame too (its lasting copies are in it). R21-B: a split frame queues none (AsyncDue said so)
          if (!split_frame) QueueStagePage(frame, binder, message.guest_frame);   // S103 L12 (dev, empty list = off)
          frame.ops = all_ops;
          if (split_frame) {   // S102 R21-B: handed over, recorded on rdr_record (ko frames are never split)
            ss::Packet p = render_globals;
            p.mode = rdr::RenderMode::PersistentOnly;
            stage.Submit(p);
          } else if (ko_draw_none) (void)builder.Finish();  // S102 lane KO: the lasting passes are not recorded either
          else renderer->RenderFrame(builder.Finish(), rdr::RenderMode::PersistentOnly);
          ++persistent_only;
          window.kept += Ns(t_kept);
          ++window.kept_frames;
        }
        }   // S102 T3 (a): end of "not drawn instead"
        if (!lasting_drawn_now) { ++window.skip_frames; window.skip_ns += Ns(t_start); }   // S102 K1: this thread's whole cost of a frame handled and not drawn (parse + contents + classify + any lasting bind)
      }
      if (!skip_wanted || lasting_drawn_now) {
        streaks.NoteDrawn(frame);   // S102 K1 (review F5): the stale-read ledger: a drawn frame's writes clear what skipped frames owed; a read of an owed image before its write counts
        if (starved && waiting >= max_queued_now) ++starved_draws;
        const auto t_bind = std::chrono::steady_clock::now();
        BindStats stats;
        rdr::FrameBuilder& fb = split_frame ? stage.Acquire() : builder;   // S102 R21-B: the record stage's slot (waits while frame N-1 is still recorded: the record stage is the limit)
        fb.Begin(message.guest_frame);
        fb.SetOutput(out_cap_now, out_bilinear_now);  // S102 G2
        fb.SetPresentFetch(rdr_sw::BoolSeen(rdr_sw::kPresentFetch, false, message.guest_frame));  // S102 R24-A: read every frame, carried by the frame
        fb.SetGroupStamps(uint32_t(std::clamp<int64_t>(rdr_sw::IntSeen(rdr_sw::kGpuGroupStamps, rdr::g_gpu_group_stamps.load(std::memory_order_relaxed), message.guest_frame), 0, 255)));  // S102 R24-F: the group stamps' min run, read every frame, carried by the frame (the binder tags each draw with its group while it is on)
        fb.SetColourCuts(colour_cuts_now);  // S102 G10
        fb.SetArenaOverdraw(arena_overdraw_now);  // S102 C4
        fb.SetArenaFragments(arena_fragments_now);  // S102 R18-B3
        fb.SetHalfTransparents(g2_low && rdr_sw::BoolSeen(rdr_sw::kLowHalfTransparents, false, message.guest_frame));  // S102 C4: LOW only
        {
          // A1b: the state steps the skipped frames before this one left out, caught up here (CatchUp). The isolates below bind the
          // frame as it came (no second catch-up).
          const Table<Op> all_ops = frame.ops;
          if (rdr_sw::BoolSeen(rdr_sw::kCatchUpState, options.catch_up_state, message.guest_frame)) {
            if (CatchUp(frame)) frame.ops = Table<Op>{catch_up_ops.data(), uint32_t(catch_up_ops.size())};
          } else if (owed) {   // S102 S0-4 (I): switched off: the debt of the skipped frames is dropped, not kept for a later switch-on
            owed = false;
            streaks.ClearMissed();
          }
          // render-clock: dev - test-only spin that slows the bind thread on purpose
          if (test_bind_spin_us) for (const auto spin_to = std::chrono::steady_clock::now() + std::chrono::microseconds(test_bind_spin_us); std::chrono::steady_clock::now() < spin_to;) {}  // S102 S0-3 test hook
          if (message.scene_mode == 2 && message.guest_frame != fc_last) fc_last = message.guest_frame, ++fc_handled;   // S103 L2b: the mode-2 frames handled, counted at the dispatch (a split frame's second message is the same frame)
          if (message.scene_mode == 2 && message.scene_record) SceneOurs(binder, source, frame, fb, message);   // S103 lane S (FRAME_CONTRACT.md 1, 5, 6): ours first, then the remnant over it
          else if (message.scene_mode == 2) fallback_census::NoRecord(message.guest_frame), CensusTick(), scene_frame::NoteStream(message.guest_frame, false);   // S103 L2b: a mode-2 frame with no scene record is counted too (S103 CLK2: and in the stream census)
          binder.Bind(frame, source, fb, stats);
          if (scene_stream_note.pending) {   // S103 CLK2 (review M1): our frame counts as drawn only when the binder placed it
            scene_frame::NoteStream(scene_stream_note.frame, scene_stream_note.drawn && binder.scene_bind_state() == 1);
            scene_stream_note.pending = false;
          }
          frame.ops = all_ops;
        }
        const rdr::Frame built = fb.Finish();   // (on a split frame only read for its present count; the stage finishes its slot again at Submit)
        if (!split_frame) QueueAsync(frame, binder, message.guest_frame, message.seq);  // lane 232: a watched picture this frame copies into is read back by this frame's own command buffer
        if (!split_frame) QueueStagePage(frame, binder, message.guest_frame);   // S103 L12 (dev, empty list = off)
        const auto t_record = std::chrono::steady_clock::now();
        if (split_frame) {   // S102 R21-B: handed over; rdr_record applies the overlay, records, submits, presents and notes the present (STAGE_SPLIT.md section 1)
          ss::Packet p = render_globals;
          p.mode = rdr::RenderMode::Full;
          p.note_present = options.window && built.presents.count;
          p.bind_ms = std::chrono::duration<double, std::milli>(t_record - t_bind).count();
          TakeOverlay(p);   // review F1: before Submit seals the frame's uploads
          stage.Submit(p);
        } else {
        ApplyOverlay();  // settings parity S11
        // Settings parity S9b (the decoupled 120): the halfway picture between the last drawn tick and this one first (it does this tick's
        // lasting work: Full), then this tick's own picture (DisplayOnly). FIFO at the display's 120 Hz shows them a vblank apart.
        // A named picture also writes the halfway one as <name>_half (checks: the in-between is otherwise never seen in a picture); the
        // named picture itself is then this tick's DisplayOnly redraw.
        // (S9b fix, Jarrod's phone test: a screen or GPU that cannot keep up made the game unsteady: no halfway picture while a newer
        // frame already waits, so the in-betweens drop first.)
        if (ko_draw_none) {   // S102 lane KO (test only): no pass, no copy; the present runs
          renderer->RenderFrame(built, rdr::RenderMode::NoPasses);
        } else if (options.tween && tween.valid() && built.presents.count && (waiting == 0 || !message.picture.empty())) {
          uint32_t matched = 0;
          // render-clock: present - the halfway picture between two game ticks, a function of the tick pair
          renderer->RenderFrame(tween.Blend(built, 0.5f, matched));
          if (!message.picture.empty()) WritePicture(binder, message.picture + "_half");
          renderer->RenderFrame(built, rdr::RenderMode::DisplayOnly);
          ++tweens;
          tween_draws += matched;
        } else if (options.dlss_mode >= 0 && built.presents.count) {
          // S10b: the previous tick's vertex constants for the DLSS velocity draws (theirs where the binder finds them in it, else this
          // tick's: no motion of their own on a draw's first tick).
          rdr::Frame with_prev = built;
          uint32_t matched = 0;
          // render-clock: present - the previous-tick motion constants, a function of the tick pair
          with_prev.prev_constants = motion_ticks.valid() ? motion_ticks.Blend(built, 0.0f, matched).constants : built.constants;
          renderer->RenderFrame(with_prev);
          motion_ticks.Keep(built);
        } else {
          renderer->RenderFrame(built);
        }
        if (options.tween && built.presents.count) tween.Keep(built);
        if (options.window && built.presents.count) NotePresent();  // S100: a frame that reached our swapchain (owner mode)
        }   // S102 R21-B: end of the one-thread render of this frame
        if (!split_frame) {  // S101 lane 19 (review fix 6): timing.csv (S102 R21-B review F4: a split frame reports from the record stage's after hook), taken before the photo-mode PNG write: this frame's bind + RenderFrame (record + submit + any wait the renderer does inside it, e.g. FIFO), and the gap since the previous picture reached the swapchain
          const double render_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_bind).count();
          case_hook::OnRenderTiming(message.guest_frame, render_ms, options.window && built.presents.count ? present_gap_ms.load(std::memory_order_relaxed) : -1.0);
        }
        if (built.presents.count && !split_frame) ServePhoto(binder);  // photo mode: the picture just shown (S102 R21-B: a pending request makes the NEXT frame a sync one: it is served there)
        window.bind += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t_record - t_bind).count());
        window.record += Ns(t_record);
        window.draws += stats.draws;
        ++window.drawn;
        const uint64_t count = ++drawn;
        draws += stats.draws;
        bound += stats.bound;
        no_shader += stats.no_shader;
        if (!message.picture.empty()) {
          WritePicture(binder, message.picture);
          // The same frame again with only one effect's draws (or all but it, "!NAME"): which effect paints what (S100 Heenan's jacket).
          for (const std::string& name : options.picture_isolates) {
            BindStats only;
            only.isolate = name.c_str();
            builder.Begin(message.guest_frame);
            binder.Bind(frame, source, builder, only);
            renderer->RenderFrame(builder.Finish());
            std::string label = message.picture + "_" + (name[0] == '!' ? "not_" + name.substr(1) : name);
            for (char& ch : label) {
              if (ch == '|') ch = '+';
            }
            WritePicture(binder, label);
          }
        }
        else if (WantPng(message.guest_frame, count)) WritePicture(binder, std::to_string(message.guest_frame));
        if (!split_frame && message.stage_mirror) StageMirror(frame, binder, message);   // S103 T7 (empty list = off): the game's in-between targets; ours is dumped by the sheet below
        if (message.scene_sheet && message.scene_record && built.presents.count && !split_frame) MirrorSheet(binder, message);   // S103 lane S: mirror mode
      }
      {  // S102 K3 (skips_auto.h): this thread's own headroom decides whether the five D3 skips are active; read every frame, the answer is latched at the next guest frame boundary (frame_stats.h)
        if (rdr_sw::BoolSeen(rdr_sw::kSkipsAuto, false, message.guest_frame)) {
          skips_force = int(rdr_sw::IntSeen(rdr_sw::kSkipsAutoForce, 0, message.guest_frame));   // test only: 1 = the skips forced off, 2 = forced on (the Surface A/B's "armed, forced off" arm); the controller still sees the frames
          skips_auto::FrameNote note;
          note.busy_ns = skips_cpu.TakeNs();   // review F2: this thread's CPU time since the last handled frame; the acquire / present / fence waits are not CPU, so a vblank-paced present no longer reads as busy
          {   // S102 R21-B (STAGE_SPLIT.md section 4): two stages: the slower one sets the rate, so busy = max(bind CPU, record CPU) over the same span
            const uint64_t rc = stage.record_cpu_ns_total();
            note.busy_ns = std::max<uint64_t>(note.busy_ns, rc - stage_cpu_seen);
            stage_cpu_seen = rc;
          }
          note.skipped = skip_wanted && (forced_skip || waiting >= max_queued_now) && !lasting_drawn_now ? 1u : 0u;   // review F4: only a queue skip (the 30 fps mode's rate skips are not "behind")
          note.starved = (!skip_wanted || lasting_drawn_now) && starved && waiting >= max_queued_now ? 1u : 0u;   // the 100 ms guard forced this draw (as starved_draws counts)
          note.waiting = uint32_t(waiting);
          note.chain_frame = d3_chain_now;
          { const uint64_t tot = d3t::g_total_ns.load(std::memory_order_relaxed); note.chain_ns = tot - skips_chain_ns_seen; skips_chain_ns_seen = tot; }
          if (skips_arena_seen != announced_seen) { skips_arena_seen = announced_seen; skips_ctl.SceneChanged(); }   // review F5: a new arena announced = a new scene
          const bool decided = skips_ctl.Note(note, uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()));
          const bool wish = skips_force == 1 ? false : skips_force == 2 ? true : decided;
          // rework: the OFF -> ON edge warms the chains up first (the chains learn kWarmFrames full frames: the gate proves, the marks publish), then the swap thread may apply ON; ON -> OFF is at once
          if (wish && !skips_wish) { skips_wish = true; skips_warm_seen = 0; skips_warm_done = false; skips_auto::SetWarm(true); }
          if (!wish && skips_wish) { skips_wish = false; skips_warm_done = false; skips_auto::SetWarm(false); }
          if (wish && d3_chain_now && !note.skipped && skips_warm_seen < skips_auto::kWarmFrames) ++skips_warm_seen;   // review F7: BOUND chain frames only (a skipped frame binds nothing: 30 fps mode counted 8 frames = 4 binds)
          bool apply = wish && skips_warm_done;
          if (wish && !skips_warm_done && skips_warm_seen >= skips_auto::kWarmFrames && binder.D3ChainsProven()) {   // review F7: and every running chain's gate proven
            if (skips_ctl.WarmCheck()) skips_warm_done = true, apply = true;   // review F3: the warm frames' own cost says whether ON can pay; refused = the controller is OFF again with a back-off
            else { skips_wish = false; skips_auto::SetWarm(false); }
          }
          skips_auto::SetWant(apply);
          skips_was_on = true;
        } else if (skips_was_on) {   // the switch went off: the skips follow the start-only switches again; a later enable starts from OFF
          skips_was_on = false;
          skips_ctl.Reset();
          skips_wish = false;
          skips_warm_done = false;
          skips_auto::SetWarm(false);
          skips_auto::SetWant(false);
        }
      }
      for (uint32_t id : message.released) binder.ReleaseImage(id);
      const auto lever_census = [&] {   // S102 CEN1: the small levers' census lines, one body for the 600-drawn window and the flip phase end
        if (rdr::g_combine_lite.load(std::memory_order_relaxed) > 0) {   // S102 R18-B2: the lever's census (relaxed counters bumped once per combine draw; the line is built here)
          char lite_line[200];
          rdr::CombineLiteCensusLine(drawn, lite_line, sizeof(lite_line));
          Log(lite_line);
        }
        if (rdr::g_gpu_small.load(std::memory_order_relaxed) > 0) {   // S102 R24-E: the lever's census (relaxed counters bumped once per post record; the line is built here)
          char small_line[420];
          rdr::GpuSmallCensusLine(drawn, small_line, sizeof(small_line));
          Log(small_line);
        }
        if (skips_was_on || skips_ctl.census().off_to_on || skips_ctl.census().on_to_off) Log(skips_auto::Line(skips_ctl.census(), skips_was_on, skips_force).c_str());   // S102 K3: the census line (every 600 drawn frames while the switch is on or ever flipped)
      };
      static census_flush::Site live_flush;   // S102 CEN1: a flip phase end (any PerFrame switch's new seen value, census_flush.h) prints the small levers' lines now
      if (census_flush::Due(live_flush) && !(drawn % 600 == 0 && drawn && drawn != last_logged)) lever_census();
      if (drawn % 600 == 0 && drawn && drawn != last_logged) {  // once per 600 drawn (skipped frames leave `drawn` where it was)
        if (options.tween) {
          char tween_line[160];
          std::snprintf(tween_line, sizeof(tween_line), "[rdr][live][tween] %llu halfway pictures, %.0f draws mixed in each", static_cast<unsigned long long>(tweens.load()),
                        tweens.load() ? double(tween_draws.load()) / double(tweens.load()) : 0.0);
          Log(tween_line);
        }
        last_logged = drawn;
        if (rdr::FrameFactsOn()) {   // S102 R21-A: the census (counters read here, nothing built per draw)
          char ff[200];
          std::snprintf(ff, sizeof(ff), "[rdr][frame_facts] records %llu, record objects %llu (arena-prefixed %llu), same-record parses skipped %llu (ids are per record: no id cache), blend skipped %llu draws (kept %llu), flat key tables %llu",
                        static_cast<unsigned long long>(rdr::g_ff_records.load(std::memory_order_relaxed)), static_cast<unsigned long long>(rdr::g_ff_objects.load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(rdr::g_ff_arena.load(std::memory_order_relaxed)), static_cast<unsigned long long>(rdr::g_ff_same.load(std::memory_order_relaxed)), static_cast<unsigned long long>(rdr::g_ff_blend_skipped.load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(rdr::g_ff_blend_kept.load(std::memory_order_relaxed)), static_cast<unsigned long long>(rdr::g_ff_keys_flat.load(std::memory_order_relaxed)));
          Log(ff);
        }
        if (d3::AnyD3Chain()) LogD3(binder);   // lane 204 (O); lane 228: the caster block too; lane 246: the ring block
        std::string live_line = "[rdr][live] " + Report();   // lane 307: a std::string (the census made this line longer than the old 512 byte buffer)
        {  // S102 T8 review F1: how many frames left the reader as tables (0 with the switch on = the case ring forced bytes; ab_verdict refuses a verdict then)
          char h[120];
          std::snprintf(h, sizeof(h), " handoff_memory in_memory=%llu ring_forced_bytes=%llu", static_cast<unsigned long long>(handoff::g_in_memory.load(std::memory_order_relaxed)),
                        static_cast<unsigned long long>(handoff::g_ring_forced_bytes.load(std::memory_order_relaxed)));
          live_line += h;
        }
        {  // lane 293: the missing-geometry count in every 600-frame line (a sign board read as zeros shows here, not only at the end of the run)
          rdr::ProblemCount counts[64];
          const int n = rdr::ProblemCounts(counts, 64);
          unsigned long long missing = 0;
          for (int i = 0; i < n && i < 64; ++i) {
            if (counts[i].name && std::strcmp(counts[i].name, "live_geometry_missing") == 0) missing = counts[i].count;
          }
          live_line += "; live_geometry_missing " + std::to_string(missing);
        }
        Log(live_line.c_str());
        lever_census();   // combine_lite, gpu_small, skips_auto (S102 CEN1: also at every flip phase end, below)
        char line[640];
        // S100 speed: this thread's time over the window, per frame handled (parse + contents), per frame drawn (bind, record), per
        // skipped frame whose lasting work ran.
        {   // S102 R21-B: the record stage's wall time of this window's split frames joins "record + submit" (on rdr_record, overlapped with the binds)
          const uint64_t rw = stage.record_wall_ns_total();
          window.record += rw - stage_wall_seen;
          stage_wall_seen = rw;
        }
        const double f = window.frames ? double(window.frames) : 1.0, d = window.drawn ? double(window.drawn) : 1.0;
        const double k = window.kept_frames ? double(window.kept_frames) : 1.0;
        const double seconds = double(Ns(window.start)) / 1e9;
        std::snprintf(line, sizeof(line),
                      "[rdr][live][time] %llu frames handled (%llu drawn, %llu skipped with lasting work) in %.2f s (%.0f drawn a second): ms a "
                      "frame: parse + contents %.3f; drawn: bind %.3f (%.0f draws, %.2f us a draw), record + submit %.3f; skipped: lasting work %.3f; lasting binds kept %llu of %llu draws",
                      static_cast<unsigned long long>(window.frames), static_cast<unsigned long long>(window.drawn),
                      static_cast<unsigned long long>(window.kept_frames), seconds, seconds > 0 ? double(window.drawn) / seconds : 0.0,
                      window.parse / f / 1e6, window.bind / d / 1e6, window.draws / d,
                      window.draws ? window.bind / double(window.draws) / 1e3 : 0.0, window.record / d / 1e6, window.kept / k / 1e6,
                      static_cast<unsigned long long>(window.prune_draws_kept), static_cast<unsigned long long>(window.prune_draws_in));
        Log(line);
        {  // S102 T1: bytes zeroed + copied on the constants path over this window (both modes), per frame parsed with bank defs
          const uint64_t b = g_bank_bytes.load(), r = g_bank_resolved.load(), n = g_bank_frames.load();
          const double nf = n > bank_mark_frames ? double(n - bank_mark_frames) : 1.0;
          char bl[400];
          std::snprintf(bl, sizeof(bl), "[rdr][time] banks: lazy %s, blocks %s; %.0f KB zeroed + copied a frame (%.1f banks built a frame, %llu frames); render thread parse + bind + lasting %.3f ms per drawn frame (the net of the A/B)",
                        bank_lazy ? "on" : "off", bank_blocks ? "on" : "off", double(b - bank_mark_bytes) / nf / 1024.0, double(r - bank_mark_resolved) / nf,
                        static_cast<unsigned long long>(n - bank_mark_frames), double(window.parse + window.bind + window.kept) / d / 1e6);
          Log(bl);
          bank_mark_bytes = b, bank_mark_resolved = r, bank_mark_frames = n;
        }
        if (window.skip_frames || streaks.guard_decay || streaks.lean) {   // S102 K1 census (review F7: whenever the window had skipped frames, switches off too; log only)
          char sl[2200];
          const uint64_t sk = window.skip_frames;
          const uint64_t* gh = streaks.gap_hist;
          std::snprintf(sl, sizeof(sl), "[rdr][skip_cost] frames %llu, skipped %llu, lasting binds %llu, skipped-frame ms mean %.3f (drawn frames: bind + record ms mean %.3f); guard decay %s, lean %s: image rule let %llu copies go the old guard would have held (total), "
                        "would decay with the switch off %llu (total), held by a guard %llu (total), stale reads %llu (total, split below); decayed-guard stale reads %llu (must be 0); gaps between state reads (handled frames) 1:%llu 2:%llu 3-4:%llu 5-8:%llu 9-16:%llu 17-32:%llu 33-64:%llu 65+:%llu",
                        static_cast<unsigned long long>(window.frames), static_cast<unsigned long long>(sk), static_cast<unsigned long long>(window.kept_frames), sk ? double(window.skip_ns) / double(sk) / 1e6 : 0.0,
                        (window.bind + window.record) / d / 1e6, streaks.guard_decay ? "on" : "off", streaks.lean ? "on" : "off", static_cast<unsigned long long>(streaks.guard_decayed),
                        static_cast<unsigned long long>(streaks.would_decay), static_cast<unsigned long long>(streaks.by_image_guarded), static_cast<unsigned long long>(streaks.stale_read), static_cast<unsigned long long>(streaks.stale_guard()),
                        static_cast<unsigned long long>(gh[0]), static_cast<unsigned long long>(gh[1]), static_cast<unsigned long long>(gh[2]), static_cast<unsigned long long>(gh[3]),
                        static_cast<unsigned long long>(gh[4]), static_cast<unsigned long long>(gh[5]), static_cast<unsigned long long>(gh[6]), static_cast<unsigned long long>(gh[7]));
          {   // S102 K2 (review F1 / a): the stale reads split by the path that left the copy out, by the read gap at the stale frame and by shape; designed lag = the drawn frame writes the image later (img388), run end = no copy into it
            const size_t used0 = std::strlen(sl);
            std::snprintf(sl + used0, sizeof(sl) - used0, "; run-end keep %s: peeks %llu, copies kept %llu, rate skips (no next frame) %llu, none queued %llu; stale frames (per read) %llu, owed and read now %zu; stale reads split (total, once per owed episode): by path exact-rect %llu / image rule %llu / decayed guard %llu; by state-read gap at the stale frame <=16 %llu / >16 or none %llu; designed lag %llu, copy run ended on a skipped frame %llu",
                          rdr_sw::BoolSeen(rdr_sw::kRunEndKeep, false, message.guest_frame) ? "on" : "off", static_cast<unsigned long long>(run_end_peeks), static_cast<unsigned long long>(run_end_kept), static_cast<unsigned long long>(run_end_rate_skips),
                          static_cast<unsigned long long>(run_end_no_next), static_cast<unsigned long long>(streaks.stale_frames), streaks.StaleOpen(),
                          static_cast<unsigned long long>(streaks.stale_by_path[1]), static_cast<unsigned long long>(streaks.stale_by_path[2]), static_cast<unsigned long long>(streaks.stale_by_path[3]),
                          static_cast<unsigned long long>(streaks.stale_gap_le16), static_cast<unsigned long long>(streaks.stale_gap_gt16), static_cast<unsigned long long>(streaks.stale_designed), static_cast<unsigned long long>(streaks.stale_run_end));
          }
          {   // S102 K2 census (R16 piece C): what a lasting bind costs and keeps, and what the no-group switch took. Plain per-window adds on this thread; nothing per draw
            const size_t used = std::strlen(sl);
            std::snprintf(sl + used, sizeof(sl) - used, "; lasting bind ms mean %.3f (kept %llu of %llu draws), nogroup %s: lasting binds with an object pass %llu of %llu (object passes kept %llu of %llu, left out although another was kept %llu, kept for a lasting render target %llu, binds with no object pass %llu), "
                          "ordinal view frames %llu (runs counted, not learned %llu); states for the Surface arm: lasting_draw %s (pct %lld), guard decay %s, lean %s, nogroup targets %s, handled-frame queue max %u",
                          window.kept_frames ? double(window.kept) / double(window.kept_frames) / 1e6 : 0.0, static_cast<unsigned long long>(window.prune_draws_kept), static_cast<unsigned long long>(window.prune_draws_in),
                          nogroup_now ? "on" : "off", static_cast<unsigned long long>(window.kept_frames - window.blind), static_cast<unsigned long long>(window.kept_frames), static_cast<unsigned long long>(window.obj_kept),
                          static_cast<unsigned long long>(window.obj_passes), static_cast<unsigned long long>(window.obj_dropped), static_cast<unsigned long long>(window.lasting_targets), static_cast<unsigned long long>(window.blind),
                          static_cast<unsigned long long>(binder.ord_view_frames), static_cast<unsigned long long>(binder.ord_view_dropped_runs),
                          lasting_draw_on ? "on" : "off", static_cast<long long>(lasting_draw_pct), streaks.guard_decay ? "on" : "off", streaks.lean ? "on" : "off", pruner.lasting_targets ? "on" : "off", unsigned(max_queued_now));   // S102 K2 (review F8): the T3 / K1 / K2 switch states beside the numbers (settings file, tier, thermal state: the bench's own record)
          }
          if (!streaks.stale_by_image.empty()) {   // S102 K2 (stale reads with the decay): WHICH images the ledger counted (top 3, with their size and role), read once per window, nothing per draw
            std::vector<std::pair<uint32_t, uint32_t>> top(streaks.stale_by_image.begin(), streaks.stale_by_image.end());
            std::sort(top.begin(), top.end(), [](const auto& x, const auto& y) { return x.second != y.second ? x.second > y.second : x.first < y.first; });
            const size_t used2 = std::strlen(sl);
            int w = std::snprintf(sl + used2, sizeof(sl) - used2, "; stale reads by image (total):");
            for (size_t k = 0; k < top.size() && k < 3 && w > 0 && size_t(w) < sizeof(sl) - used2; ++k) {
              const std::vector<ImageRecord>& im = source.images();
              const uint32_t id = top[k].first;
              const ImageRecord* r = id >= 1 && id <= im.size() && im[id - 1].id == id ? &im[id - 1] : nullptr;
              static const char* const path_name[4] = {"?", "exact-rect", "image rule", "decayed guard"};
              const auto ro = streaks.owed_reason_.find(id);
              const auto sh = streaks.stale_shape_.find(id);
              const lasting::CopyStreaks::GuardRec* gr = streaks.FindRec(id, false);
              w += std::snprintf(sl + used2 + w, sizeof(sl) - used2 - size_t(w), " img%u x%u episodes (%ux%u role %u usage %x; latest: path %s, shape %s, %s)", id, top[k].second, r ? unsigned(r->width) : 0u, r ? unsigned(r->height) : 0u, r ? unsigned(r->role) : 0u, r ? unsigned(r->usage) : 0u,
                                    path_name[ro != streaks.owed_reason_.end() && ro->second < 4 ? ro->second : 0], sh == streaks.stale_shape_.end() ? "?" : sh->second == 1 ? "designed lag" : "run end", gr && gr->pinned ? "pinned" : "not pinned");
            }
          }
          Log(sl);
        }
        if (d3::AnyD3Chain()) Log(d3t::Line().c_str());   // S102 K3 rework (step 1): the render thread's D3 bookkeeping per handled frame, by chain and phase
        if (stage.started()) Log(stage.Line().c_str());   // S102 R21-B: [rdr][stage] (every 600 drawn frames once the stage has run; built here only)
        if (mail_stats.dropped) {   // S103 G2 (only once something was dropped: silent with the switch off, where barrier stops would count a plain backlog) (SCENE_PACING.md 3c (9)): the consumer's latest-wins pop, cumulative; shown rate = takes per second
          char ml[160];
          std::snprintf(ml, sizeof(ml), "[rdr][mail] takes %llu dropped %llu max_run_dropped %llu barrier_stops %llu",
                        static_cast<unsigned long long>(mail_stats.takes), static_cast<unsigned long long>(mail_stats.dropped),
                        static_cast<unsigned long long>(mail_stats.max_run_dropped), static_cast<unsigned long long>(mail_stats.barrier_stops));
          Log(ml);
        }
        window = Window{};
      }
    }
  }

  // Lane 232 (3d a2): BEFORE a drawn frame is rendered, each watched image (async.Images) that one of its copies writes (dest by the reader's id) is queued for a read-back at the end of that frame's own
  // command buffer (Renderer::QueueReadback: a persistent staging buffer, no drain), at most every `every_frames` guest frames (AsyncReadback::Due). PollAsync (the next message) hands the finished ones
  // (their frame's fence signalled) to the mailbox. Render thread only; one atomic-free mutex lookup when nothing is watched. In tween mode the copy goes into the first RenderFrame of the tick (the blend,
  // which does the lasting work), so the DisplayOnly redraw after it changes nothing.
  void QueueAsync(const LoadedFrame& frame, Binder& binder, uint64_t guest_frame, uint64_t seq) {
    if (!async.AnyWatched()) return;
    const auto t0 = std::chrono::steady_clock::now();
    async.Images(async_images);
    for (uint32_t image : async_images) {
      if (async.DemandOnly(image)) {  // deferred delivery (the replay thumbnail): read at the end of the first frame handled after the demand, wherever its copy was
        async.Due(image, guest_frame, async_due, seq);
        for (const AsyncReadback::Key& key : async_due) {
          rdr::ImageRef ref;
          ref.image = binder.Mapped(key.image);
          ref.mip = uint8_t(key.mip);
          ref.layer = uint8_t(key.layer);
          const uint64_t tag = ++async_tag;
          if (ref.image != rdr::kNoImage && renderer->QueueReadback(ref, tag)) async_tags[tag] = {key, guest_frame, seq};
        }
        continue;
      }
      // SF-2: only a copy into the picture from the SAME source the first blocking lock saw (a shared image may be written by other targets' copies too).
      const uint32_t want_source = async.SourceOf(image);
      uint32_t copy_source = 0;
      bool copied = false;
      for (uint32_t i = 0; i < frame.ops.count && !copied; ++i) {
        const Op& op = frame.ops[i];
        if (op.kind != OpKind::Copy || uint32_t(frame.copies[op.index].dest.image) != image) continue;
        if (want_source && uint32_t(frame.copies[op.index].source.image) != want_source) continue;
        copied = true;
        copy_source = uint32_t(frame.copies[op.index].source.image);
      }
      if (!copied) continue;
      async.Due(image, guest_frame, async_due);
      for (const AsyncReadback::Key& key : async_due) {
        rdr::ImageRef ref;
        ref.image = binder.Mapped(key.image);
        ref.mip = uint8_t(key.mip);
        ref.layer = uint8_t(key.layer);
        const uint64_t tag = ++async_tag;
        const rdr::ImageId source = want_source ? binder.Mapped(copy_source) : rdr::kNoImage;
        if (ref.image != rdr::kNoImage && renderer->QueueReadback(ref, tag, source)) async_tags[tag] = {key, guest_frame, seq};
      }
    }
    async.NoteRenderMs(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  // S102 R21-B (STAGE_SPLIT.md hazard 9): would QueueAsync queue a read-back for this frame? The same walk with no renderer call (AsyncReadback::Due is
  // const), so the frame can be sent to the one-thread path BEFORE it is bound. Conservative: an image the binder cannot map still counts.
  bool AsyncDue(const LoadedFrame& frame, uint64_t guest_frame, uint64_t seq) {
    if (!async.AnyWatched()) return false;
    async.Images(async_images);
    for (uint32_t image : async_images) {
      if (async.DemandOnly(image)) {
        async.Due(image, guest_frame, async_due, seq);
        if (!async_due.empty()) return true;
        continue;
      }
      const uint32_t want_source = async.SourceOf(image);
      for (uint32_t i = 0; i < frame.ops.count; ++i) {
        const Op& op = frame.ops[i];
        if (op.kind != OpKind::Copy || uint32_t(frame.copies[op.index].dest.image) != image) continue;
        if (want_source && uint32_t(frame.copies[op.index].source.image) != want_source) continue;
        async.Due(image, guest_frame, async_due);
        if (!async_due.empty()) return true;
        break;
      }
    }
    return false;
  }
  // S103 L12 (--wwe2k14_dump_stage_page, dev diagnosis, default empty = off): the stage light-map page the arena samples at vertex slot 16, read at the end of the first drawn frame at or after each
  // listed guest frame (Renderer::QueueReadback: the image's own contents, as RGBA8). The page is the image most draws of the frame bind at texture slot 16. Off: stage_pending is empty and
  // every function below returns at once (no lock, no walk, no renderer call).
  struct StagePageTag {
    uint64_t want = 0, frame = 0;
    uint32_t guest_image = 0, draws = 0;
  };
  std::vector<uint64_t> stage_pending;   // ascending; a frame is taken off when its dump is queued
  bool stage_init = false;
  std::unordered_map<uint64_t, StagePageTag> stage_tags;
  bool StagePageDue(uint64_t guest_frame) {
    if (!stage_init) {
      stage_init = true;
      stage_pending = options.stage_page_frames;
      std::sort(stage_pending.begin(), stage_pending.end());
    }
    return !stage_pending.empty() && guest_frame >= stage_pending.front();
  }
  void QueueStagePage(const LoadedFrame& frame, Binder& binder, uint64_t guest_frame) {
    if (!StagePageDue(guest_frame)) return;
    std::unordered_map<uint32_t, uint32_t> users;
    for (uint32_t d = 0; d < frame.draws.count; ++d) {
      const Draw& draw = frame.draws[d];
      for (uint32_t t = 0; t < draw.texture_count; ++t) {
        const uint32_t ti = draw.texture_first + t;
        if (ti >= frame.texture_slots.count) break;
        const TextureSlot& ts = frame.texture_slots[ti];
        if (ts.slot == 16 && ts.image) ++users[ts.image];
      }
    }
    uint32_t best = 0, best_n = 0;
    for (const auto& u : users)
      if (u.second > best_n || (u.second == best_n && u.first < best)) best = u.first, best_n = u.second;
    const uint64_t want = stage_pending.front();
    if (!best) {
      if (guest_frame > want + 600) {
        Log(("[rdr][stage_page] frame " + std::to_string(want) + ": no draw binds vertex slot 16 up to frame " + std::to_string(guest_frame) + ", given up").c_str());
        stage_pending.erase(stage_pending.begin());
      }
      return;
    }
    rdr::ImageRef ref;
    ref.image = binder.Mapped(best);
    const uint64_t tag = ++async_tag;
    if (ref.image == rdr::kNoImage || !renderer->QueueReadback(ref, tag)) {
      Log(("[rdr][stage_page] frame " + std::to_string(want) + ": image " + std::to_string(best) + " is not readable here, retrying next drawn frame").c_str());
      if (guest_frame > want + 600) stage_pending.erase(stage_pending.begin());
      return;
    }
    stage_tags[tag] = {want, guest_frame, best, best_n};
    stage_pending.erase(stage_pending.begin());
  }
  void WriteStagePage(const StagePageTag& t, const rdr::Renderer::ReadbackResult& r) {
    std::error_code ec;
    std::filesystem::create_directories(options.stage_page_dir, ec);
    const std::string stem = "stage_page_f" + std::to_string(t.frame);
    const std::filesystem::path png = options.stage_page_dir / (stem + ".png");
    const std::filesystem::path raw = options.stage_page_dir / (stem + ".rgba8");
    bool ok = FinishPng(nullptr, png, r.rgba, r.width, r.height);
    if (FILE* f = rdr::PngOpen(raw)) {  // raw bytes: width, height (u32 LE x2) then RGBA8 rows; the readback's own format conversion, 8 bits a channel
      const uint32_t wh[2] = {r.width, r.height};
      ok = std::fwrite(wh, sizeof(wh), 1, f) == 1 && std::fwrite(r.rgba.data(), 1, r.rgba.size(), f) == r.rgba.size() && ok;
      std::fclose(f);
    } else {
      ok = false;
    }
    char line[320];
    std::snprintf(line, sizeof(line), "[rdr][stage_page] wanted %llu, read after guest frame %llu: image %u (%u draws bind it at slot 16) %ux%u -> %s (%s)",
                  static_cast<unsigned long long>(t.want), static_cast<unsigned long long>(t.frame), t.guest_image, t.draws, r.width, r.height, png.string().c_str(), ok ? "written" : "WRITE FAILED");
    Log(line);
  }
  // S103 T7 (--wwe2k14_dump_targets, dev diagnosis, default empty = off; stage_mirror.h): at each LISTED guest frame (exactly that frame, so the scene record and the game's targets are the same frame) the frame's
  // ops are walked once (stage_mirror::CollectTargets: the role of every colour target a draw wrote, by technique), each role's images are read back at the end of the frame (Renderer::ReadImage: waits on
  // the GPU, dev only) and written to options.dump_targets_dir as game_<role>.<n>_f<frame>.rgba8 + .png; the stage page (the image most draws bind at texture slot 16, as L12) and the present's source
  // follow. manifest_game_f<frame>.txt lists role / n / game image / draws / size / technique. OUR maps by role come from the same frame's mirror sheet (LiveSceneDraw::SetMapDump, set in MirrorSheet).
  // Off: the list is empty, StageMirrorNote is never called and no message carries stage_mirror (no lock, no walk, no renderer call).
  // The swap thread (NoteScene) decides, once per guest frame: the next listed frame is answered by the first frame at or after it that is allowed to be dumped. Mode 1 / 0: any frame. Mode 2:
  // only a SENTINEL walk (scene_frame::IsSentinel of the hook frame): the other frames carry no game draws. The frame used is the message's guest_frame (the manifest names both).
  // Only the swap thread touches the pending list. A due frame is a sheet frame (a mirror pair; mode 0 has no record, so the game side only).
  std::vector<uint64_t> mirror_pending;
  bool mirror_init = false;
  void StageMirrorNote(Message& m, uint64_t guest_frame) {
    if (!mirror_init) {
      mirror_init = true;
      mirror_pending = options.dump_targets_frames;
      std::sort(mirror_pending.begin(), mirror_pending.end());
    }
    if (mirror_pending.empty() || guest_frame < mirror_pending.front()) return;
    if (m.scene_mode == 2 && !scene_frame::IsSentinel(scene_frame::HookFrameOf(guest_frame))) return;
    m.stage_mirror = true;
    m.stage_mirror_want = mirror_pending.front();
    mirror_pending.erase(mirror_pending.begin());
    if (m.scene_mode == 1 || m.scene_mode == 2) m.scene_sheet = true;
  }
  void StageMirror(const LoadedFrame& frame, Binder& binder, const Message& message) {
    std::vector<rdr_adapter::stage_mirror::Found> found;
    uint32_t dropped = 0;
    rdr_adapter::stage_mirror::CollectTargets(frame, found, dropped);
    {  // the stage light-map page: the image most draws bind at texture slot 16 (L12's rule, kept here so one flag reads it)
      std::unordered_map<uint32_t, uint32_t> users;
      for (uint32_t d = 0; d < frame.draws.count; ++d) {
        const Draw& draw = frame.draws[d];
        for (uint32_t t = 0; t < draw.texture_count; ++t) {
          const uint32_t ti = draw.texture_first + t;
          if (ti >= frame.texture_slots.count) break;
          const TextureSlot& ts = frame.texture_slots[ti];
          if (ts.slot == 16 && ts.image) ++users[ts.image];
        }
      }
      uint32_t best = 0, best_n = 0;
      for (const auto& u : users)
        if (u.second > best_n || (u.second == best_n && u.first < best)) best = u.first, best_n = u.second;
      if (best) {
        rdr_adapter::stage_mirror::Found f;
        f.role = "stage_page", f.index = 0, f.guest_image = best, f.draws = best_n, f.technique = "texture slot 16", f.last_writer = "-";
        found.push_back(std::move(f));
      }
    }
    const rdr::ImageRef present = binder.last_present().source;   // the whole ref (image, mip, layer): a renderer image, not a guest id
    std::error_code ec;
    std::filesystem::create_directories(options.dump_targets_dir, ec);
    const std::string tag = "f" + std::to_string(message.guest_frame);
    std::string manifest = "# game targets by role, end of guest frame " + std::to_string(message.guest_frame) + " (listed frame " + std::to_string(message.stage_mirror_want) + ", scene mode " + std::to_string(message.scene_mode) +
                           "); columns: game, role, n, game image, draws, size, technique, last written by, status\n"
                           "# dropped past " + std::to_string(rdr_adapter::stage_mirror::kMaxPerRole) + " images of a role: " + std::to_string(dropped) + "\n";
    auto emit = [&](const std::string& role, uint32_t index, uint32_t guest_image, uint32_t draws, const std::string& technique, const std::string& last_writer, rdr::ImageRef ref) {
      std::vector<uint8_t> px;
      uint32_t w = 0, h = 0;
      char line[360];
      if (ref.image == rdr::kNoImage || !renderer->ReadImage(ref, px, w, h) || !w || !h) {
        std::snprintf(line, sizeof line, "game\t%s\t%u\t%u\t%u\t0x0\t%s\t%s\tNOT READABLE\n", role.c_str(), index, guest_image, draws, technique.c_str(), last_writer.c_str());
        manifest += line;
        return;
      }
      const std::string stem = rdr_adapter::stage_mirror::FileStem("game", role, index, message.guest_frame);
      const std::filesystem::path base = options.dump_targets_dir / stem;
      std::filesystem::path raw = base, png = base;
      raw += ".rgba8";
      png += ".png";
      const bool ok = rdr_adapter::stage_mirror::WriteRaw(raw, px, w, h) && FinishPng(nullptr, png, px, w, h);
      std::snprintf(line, sizeof line, "game\t%s\t%u\t%u\t%u\t%ux%u\t%s\t%s\t%s\n", role.c_str(), index, guest_image, draws, w, h, technique.c_str(), last_writer.c_str(), ok ? "written" : "WRITE FAILED");
      manifest += line;
    };
    for (const rdr_adapter::stage_mirror::Found& f : found) {
      rdr::ImageRef ref;
      ref.image = binder.Mapped(f.guest_image);
      emit(f.role, f.index, f.guest_image, f.draws, f.technique, f.last_writer, ref);
    }
    emit("present", 0, 0, 0, "last present's source", "-", present);
    rdr_adapter::stage_mirror::WriteText(options.dump_targets_dir / ("manifest_game_" + tag + ".txt"), manifest);
    char sum[256];
    std::snprintf(sum, sizeof sum, "[rdr][stage_mirror] guest frame %llu: %zu roles read, manifest %s", static_cast<unsigned long long>(message.guest_frame), found.size() + 1, (options.dump_targets_dir / ("manifest_game_" + tag + ".txt")).string().c_str());
    Log(sum);
  }
  void PollAsync() {
    const auto t0 = std::chrono::steady_clock::now();  // SF-1: always polls (PollReadbacks returns at once when nothing is in flight or cancelled)
    async_done.clear();
    renderer->PollReadbacks(async_done);
    for (rdr::Renderer::ReadbackResult& r : async_done) {
      if (shot_inflight && r.tag == shot_tag) { FinishShot(r); continue; }   // S103 lane SHOT
      if (!stage_tags.empty()) {   // S103 L12: a stage page dump's result (off: stage_tags is empty)
        const auto st = stage_tags.find(r.tag);
        if (st != stage_tags.end()) {
          if (!r.cancelled) WriteStagePage(st->second, r);
          stage_tags.erase(st);
          continue;
        }
      }
      const auto it = async_tags.find(r.tag);
      if (it == async_tags.end()) continue;
      if (r.cancelled) {
        async_tags.erase(it);
        continue;
      }
      async.Publish(it->second.key, it->second.frame, std::move(r.rgba), r.width, r.height, it->second.seq);
      async_tags.erase(it);
    }
    async.NoteRenderMs(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
  }
  uint64_t async_tag = 0;
  std::unordered_map<uint64_t, AsyncTag> async_tags;
  std::vector<rdr::Renderer::ReadbackResult> async_done;
  // S100: owner mode presents through our swapchain, not the SDK presenter, so its frames reached a present are counted here, in the
  // SDK's own counters (frames_*.csv `presented` / `present_gap_ms`, the `[wwe2k14][frames] on screen` line): on a GPU-bound machine the
  // game loop keeps stepping at 60 while the sink skips drawing, and only this says what reached the screen.
  std::atomic<std::chrono::steady_clock::time_point> last_present{};  // S102 R21-B (hazard 11): written by the stage that presented (rdr_record on split frames), read by the 100 ms guard on rdr_render
  // S100 (strip-360 A1): how often each copy's destination (image, mip, layer, rectangle) was written in consecutive frames handled.
  // S100 (strip-360 A1b, rule 6): a per-frame copy that carries state from frame to frame (an exposure / adaptation value: the pass that
  // renders the copy's source reads the copy's destination, the value it nudges) advances one step a game frame. Frames our renderer skips
  // leave it out (A1: its feed is the whole scene), so the drawn frame after them repeats that pass and its copy once for each step left
  // out, right after the copy, with this frame's inputs: the effect follows game frames, not drawn frames (run 6952: an outrun fast-forward
  // drew 1-2 levels darker without it). Only small passes (the state is a value, not a picture: at most kCatchUpTexels) and at most
  // kMaxCatchUp steps (an exponential approach is settled by then).
  static constexpr uint32_t kMaxCatchUp = 16;
  static constexpr uint64_t kCatchUpTexels = 64 * 64;
  std::vector<Op> catch_up_ops;
  std::atomic<uint64_t> catch_up_steps{0}, catch_up_copies{0}, catch_up_not_state{0}, catch_up_too_big{0};
  bool owed = false;  // a skipped frame left a per-frame copy out since the last drawn frame
  void NoteMissed(const rdr::CopyOp& c) {
    const auto it = streaks.copy_seen.find(streaks.Key(c));
    if (it == streaks.copy_seen.end()) return;
    if (it->second.missed < kMaxCatchUp) ++it->second.missed;
    owed = true;
  }
  bool CatchUp(const LoadedFrame& frame) {
    if (!owed) return false;
    owed = false;  // (this frame's copies settle what they owe; a copy missing from it broke its streak, NoteCopies dropped its count)
    catch_up_ops.clear();
    bool any = false;
    const uint32_t count = frame.ops.count;
    for (uint32_t i = 0; i < count; ++i) {
      const Op& op = frame.ops[i];
      catch_up_ops.push_back(op);
      if (op.kind != OpKind::Copy) continue;
      const rdr::CopyOp& c = frame.copies[op.index];
      const auto it = streaks.copy_seen.find(streaks.Key(c));
      if (it == streaks.copy_seen.end() || !it->second.missed) continue;
      const uint32_t steps = it->second.missed;
      it->second.missed = 0;
      if (uint64_t(c.dest_rect.width) * c.dest_rect.height > kCatchUpTexels) {
        ++catch_up_too_big;
        continue;
      }
      // The pass that rendered the copy's source: back over its draws and clears to the SetTargets that named the source.
      uint32_t j = i;
      bool reads_dest = false, renders_source = false;
      while (j > 0) {
        const Op& p = frame.ops[j - 1];
        if (p.kind == OpKind::Draw) {
          const Draw& d = frame.draws[p.index];
          for (uint32_t s = 0; s < d.texture_count; ++s) reads_dest |= frame.texture_slots[d.texture_first + s].image == c.dest.image;
          --j;
          continue;
        }
        if (p.kind == OpKind::Clear) {
          --j;
          continue;
        }
        if (p.kind == OpKind::SetTargets) {
          const rdr::TargetsOp& t = frame.targets[p.index];
          renders_source = t.color_count && t.color[0].image == c.source.image;
          --j;
        }
        break;
      }
      if (!renders_source || !reads_dest) {
        ++catch_up_not_state;
        continue;
      }
      for (uint32_t r = 0; r < steps; ++r) catch_up_ops.insert(catch_up_ops.end(), frame.ops.data + j, frame.ops.data + i + 1);
      catch_up_steps += steps;
      ++catch_up_copies;
      any = true;
    }
    return any;
  }
  lasting::CopyStreaks streaks;   // lane 307: the lasting classification (lasting_prune.h)
  lasting::Pruner pruner;
  std::vector<uint8_t> lasting_seed;
  std::vector<Op> lasting_ops, dropped_ops;
  uint64_t run_end_peeks = 0, run_end_kept = 0, run_end_rate_skips = 0, run_end_no_next = 0;   // S102 K2 round 2: the run-end recovery's census (render thread only)
  // The destination images of the copies of the NEXT queued message (handling order: the popped run first, then `mail`; a queue skip, a rate skip with a backlog or a dropped message has one if anything is queued); false = nothing queued. The tables are copied / bound outside the lock.
  bool PeekNextCopies(std::vector<rdr::CopyOp>& out) {   // round 3 F3: the copy table only, copied under the lock (a few KB), nothing parsed, no frame copied
    if (!run_q.empty()) {   // S103 G2: the next message in handling order is the run's (render thread: no lock)
      if (run_q.front().data) { out = run_q.front().data->copies; return true; }
      return CaptureReader::ReadCopies(run_q.front().frame, out);
    }
    std::lock_guard<std::mutex> lock(mail_mu);
    if (mail.empty()) return false;
    if (mail.front().data) {
      out = mail.front().data->copies;
      return true;
    }
    return CaptureReader::ReadCopies(mail.front().frame, out);
  }
  std::atomic<uint64_t> per_frame_copies_left{0}, starved_draws{0};  // skipped frames' per-frame copies left to the drawn frame; draws forced by the 100 ms guard
  std::atomic<uint64_t> prune_frames{0}, prune_ops_in{0}, prune_ops_kept{0}, prune_draws_in{0}, prune_draws_kept{0}, prune_passes_in{0}, prune_passes_kept{0}, prune_grouped{0}, prune_seeds{0}, prune_promoted{0}, prune_blind{0};
  std::atomic<double> present_gap_ms{-1};  // S101 lane 19: the gap NotePresent just measured (S102 R21-B: atomic, the record stage writes it on split frames)
  void NotePresent() {
    if (rdr::DeviceLost()) return;  // S102 D1 (review F2): no frame counts as presented after a lost device, on every path (sync and R21-B split): the csv's `presented` read ~60 fps while the GPU drew nothing
    auto& events = rex::perf::frame_events::Get();
    // render-clock: ctl - wall-clock pacing of our own present loop, never a picture value
    const auto now = std::chrono::steady_clock::now();
    const auto before = last_present.load(std::memory_order_relaxed);
    double gap = -1;  // S101 lane 19: -1 = the first picture, no gap to report
    if (before != std::chrono::steady_clock::time_point{}) {
      const uint32_t gap_us = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(now - before).count());
      events.present_gap_us.store(gap_us, std::memory_order_relaxed);
      for (uint32_t seen = rdr::g_present_gap_max_us.load(std::memory_order_relaxed); gap_us > seen && !rdr::g_present_gap_max_us.compare_exchange_weak(seen, gap_us, std::memory_order_relaxed);) {}   // S102 R25-C2: the row's largest gap
      gap = std::chrono::duration<double, std::milli>(now - before).count();
    }
    present_gap_ms.store(gap, std::memory_order_relaxed);
    last_present.store(now, std::memory_order_relaxed);
    events.presents.fetch_add(1, std::memory_order_release);
    // Lane 256: the hang guard's "presents" beat is the SDK presenter's; owner mode's swapchain never beat it (a false "BLACK: no present" after 10 s).
    // Beat only when the swapchain really presented (rdr::g_presented counts accepted presents), so a surface that fails to come back says BLACK.
    const uint64_t shown = rdr::g_presented.load(std::memory_order_relaxed);
    if (shown != beat_seen) {
      beat_seen = shown;
      rex::perf::hang_beats::Beat(rex::perf::hang_beats::Get().presents);
    }
  }
  uint64_t beat_seen = 0;  // lane 256: rdr::g_presented at the last hang beat (render thread only)

  // S100 speed: this thread's time, per 600-drawn-frame window (render thread only).
  struct Window {
    uint64_t frames = 0, drawn = 0, kept_frames = 0, draws = 0, prune_draws_in = 0, prune_draws_kept = 0;
    uint64_t parse = 0, bind = 0, record = 0, kept = 0;  // ns
    uint64_t skip_frames = 0, skip_ns = 0;   // S102 K1: frames handled and not drawn (and the ns they took, whole)
    uint64_t obj_passes = 0, obj_kept = 0, obj_dropped = 0, lasting_targets = 0, blind = 0;   // S102 K2: of the lasting binds: passes holding an engine-object draw, how many the bind kept / left out although another was kept, binds with no object pass
    // render-clock: ctl - wall-clock pacing of our own present loop, never a picture value
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  };
  Window window;
  uint32_t latch_tick = 0;            // S102 K3 rework
  bool skips_wish = false;            // S102 K3 rework: the controller's ON wish (the chains warm up; the swap thread applies ON after kWarmFrames chain frames)
  uint32_t skips_warm_seen = 0;
  bool skips_warm_done = false;       // review F3 / F7: the warm-up has its frames, the gates proven and the cost check passed: the swap thread may apply ON
  uint64_t skips_chain_ns_seen = 0;   // the d3_timers total at the last note
  skips_auto::Controller skips_ctl;   // S102 K3: the render thread's headroom controller (render thread only)
  bool skips_was_on = false;          // S102 K3: the switch as read for the latest frame
  int skips_force = 0;                // S102 K3 review F3: wwe2k14_rdr_skips_auto_force as read for the latest frame (test only)
  int skips_arena_seen = -1;          // S102 K3 review F5: announced_seen at the last controller note (a new arena = a new scene)
  rdr::ThreadCpuMeter skips_cpu;      // S102 K3 review F2: the controller's busy is this thread's CPU time
  BankPool bank_pool;   // S102 T1
  bool bank_lazy = false;   // S102 T1: the switch as read for the latest frame
  bool bank_blocks = false;   // S102 T1b: the same for wwe2k14_rdr_bank_blocks
  uint64_t bank_mark_bytes = 0, bank_mark_resolved = 0, bank_mark_frames = 0;   // S102 T1: the totals at the last [rdr][time] banks line
  static uint64_t Ns(std::chrono::steady_clock::time_point since) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - since).count());
  }

  // S103 lane S (FRAME_CONTRACT.md 1, 3, 5, 6): mode 2 on the render thread, before the frame's bind. The decision for frame j travels in record j
  // (kScOwnedPasses). Stamped OURS: record j is drawn into this frame's builder by our graph and the frame image (the source of the copy the present
  // reads) is aliased to our picture for the whole bind, so the remnant (text, movies) lands on top of it. Not ours: the cache is warmed so the
  // loader fetches what the scene needs and READY is learned. A frame stamped OURS that our graph cannot draw is a hole: counted, a hold starts.
  // v0 limits (logged in the census, to fix before landing 1): no post (frame.post null), a hole shows the remnant over a stale frame image (REMNANT_SEAM H3).
  // S103 L2a (docs\renderer\LANDING2_PLAN.md P1; dev only, --wwe2k14_rdr_2d_ours with --wwe2k14_rdr_2d_record): record j's 2D rows drawn into `out` after our post. A frame is 2D-WHOLE only when
  // the contributor took it with no hole, every row passes the drawer's own check (q2d::Check: blend from the state fill, pictures), the stream frame carries origins and the pure scan (q2d::ScanStream: every row-site draw on the frame image, no kept op between them,
  // their number = the rows, row i's blend = the i-th draw's) passes. Then the binder leaves the stream's row-site draws out and appends our rows at the first of them (SetSkip2DRows with an insert). Any other frame keeps the stream's 2D whole (counted by its
  // first reason). Pictures / movie planes: no live resolver yet (P3 / P4), so a sprite, glyph or movie row is refused 'picture' / 'planes' and its frame keeps the stream's 2D.
  bool q2d_on = false;
  bool q2d_registered = false;
  rdr_scene::q2d::Programs q2d_programs;
  struct Q2DCensus {
    uint64_t frames = 0, whole = 0, rows_drawn = 0, not_taken = 0, holes = 0, refused = 0, no_origins = 0;
    uint64_t cross_pairs = 0, cross_target = 0, cross_size = 0, cross_scissor = 0;   // S103 SC review: the dev cross-check of row i against the i-th stream row-site draw (target class / size, scissor ∩ viewport)
    uint64_t scan_by[uint8_t(rdr_scene::q2d::PairWhy::Num)] = {};   // S103 L2a review: the pre-bind stream scan's refusals (stream_target / interleaved / count / blend_pair)
    uint64_t refused_by[uint8_t(rdr_scene::q2d::Refuse::Count)] = {};
  } q2d_census;
  // S103 L2dF (LANDING2_PLAN 5 (a)): the 2D drawer's decision split in two. Check2D is the pure half: it reads the record, the stream frame and the drawer's own check and changes NOTHING (no census note, no binder call);
  // Apply2D is the only place that calls SetSkip2DRows(true, ...). The 3D OURS path (Draw2DOurs) calls Check2D then Apply2D as before; a FLAT frame calls Apply2D only after scene_kind::FlatVerdict said Ours.
  struct Check2DResult {
    scene_kind::TwoD twod = scene_kind::TwoD::NotTaken;
    uint32_t refused_other = 0, movie_rows = 0, movie_planes_missing = 0;
    rdr_scene::q2d::PairWhy scan = rdr_scene::q2d::PairWhy::None;   // the stream scan's refusal (twod == ScanRefused / CountDiffers)
    rdr_scene::q2d::Report report;                                  // the drawer's own per-row check (twod == Refused only when refused_total() > 0)
    rdr_scene::q2d::CrossCheck cross;                               // S103 SC review: dev only (wwe2k14::IsDevBuild), counted on the census line
    const char* first_reason = nullptr;                             // the first refused row's reason name (census), or "blend" with blend_code set
    int blend_code = -1;                                            // >= 0: the first refused row is a blend the drawer has no program for (its state code)
    bool Whole() const { return twod == scene_kind::TwoD::Whole; }
  };
  rdr_scene::q2d::Context MakeCtx2D(uint32_t w, uint32_t h) const {
    rdr_scene::q2d::Context ctx;
    ctx.width = w, ctx.height = h;
    ctx.plane = [](const rdr_scene::Quad2D&, int) { return rdr::TextureSlot{}; };   // (P4: the planes from guest memory)
    ctx.picture = [](const rdr_scene::Quad2D&) { return rdr::TextureSlot{}; };       // (P1 picture path 2 / P3: no live picture resolver yet)
    return ctx;
  }
  // `frame_id` = the stream's frame image (FindFrameImage). `jframe` = the record's frame (kept for the caller's census notes; this function notes nothing).
  Check2DResult Check2D(const Binder&, const LoadedFrame& frame, const rdr_scene::SceneTables& rec, uint32_t w, uint32_t h, uint32_t frame_id, [[maybe_unused]] uint64_t jframe) const {
    namespace q = rdr_scene::q2d;
    Check2DResult r;
    if (rec.quad2d_holes == rdr_scene::kQuad2DNotTaken) return r.twod = scene_kind::TwoD::NotTaken, r;
    for (const rdr_scene::Quad2D& row : rec.quads2d)
      if (row.kind == rdr_scene::kQkYuvAlpha || row.kind == rdr_scene::kQkYuv) ++r.movie_rows;
    if (rec.quad2d_holes) return r.twod = scene_kind::TwoD::Holes, r;   // (the census has the hole's site from the tap)
    if (!Binder::HasOrigins(frame)) return r.twod = scene_kind::TwoD::NoOrigins, r;
    // F1 / F2 / pairing: the stream's ops as the pure scan reads them; our rows go in at the first row-site draw, so the scan refuses what that would reorder or mis-target.
    std::vector<q::StreamOp> ops;
    Binder::ListStreamOps(frame, ops);
    const q::PairScan scan = q::ScanStream(ops.data(), ops.size(), frame_id, rec.quads2d.data(), rec.quads2d.size());
    if (scan.why != q::PairWhy::None) {
      r.scan = scan.why;
      r.first_reason = q::PairWhyName(scan.why);
      r.twod = scan.why == q::PairWhy::Count ? scene_kind::TwoD::CountDiffers : scene_kind::TwoD::ScanRefused;
      return r;
    }
    if (wwe2k14::IsDevBuild()) {   // S103 SC review: the dev cross-check, at the row-site draws of the stream path
      uint32_t fw = 0, fh = 0;
      fw = w, fh = h;
      r.cross = q::CrossCheckStream(ops.data(), ops.size(), frame_id, fw, fh, rec.quads2d.data(), rec.quads2d.size());
    }
    const q::Context ctx = MakeCtx2D(w, h);
    r.report = q::Check(ctx, rec.quads2d.data(), rec.quads2d.size());
    if (!r.report.refused_total()) return r.twod = scene_kind::TwoD::Whole, r;
    r.twod = scene_kind::TwoD::Refused;
    r.movie_planes_missing = r.report.refused[uint8_t(q::Refuse::Planes)];
    r.refused_other = r.report.refused_total() - r.movie_planes_missing;
    for (const rdr_scene::Quad2D& row : rec.quads2d) {   // the census names the FIRST refused row's reason (a blend the drawer has no program for: its state code)
      const q::Report one = q::Check(ctx, &row, 1);
      if (!one.refused_total()) continue;
      if (one.refused[uint8_t(q::Refuse::Blend)]) r.first_reason = "blend", r.blend_code = int(row.state & 15u);
      else
        for (uint8_t i = 1; i < uint8_t(q::Refuse::Count); ++i)
          if (one.refused[i]) { r.first_reason = q::RefuseName(q::Refuse(i)); break; }
      break;
    }
    return r;
  }
  // The ONLY SetSkip2DRows(true, ...) in this file (lint: tools\scene\stamp_copy_lint.py). Callers: Draw2DOurs after an Ours 3D frame's Whole check, SceneOurs after a flat frame's Ours verdict. `out` = the colour target our rows go to;
  // kNoImage = the stream's own current targets at the first skipped row-site draw (a flat frame: ScanStream proved every row-site draw targets the frame image, so nothing is aliased).
  void Apply2D(Binder& binder, const rdr_scene::SceneTables& rec, rdr::ImageId out, uint32_t w, uint32_t h) {
    if (rec.quads2d.empty()) return binder.SetSkip2DRows(true);
    if (!q2d_registered) q2d_programs = rdr_scene::q2d::Register(*renderer, true), q2d_registered = true;
    const rdr_scene::q2d::Context ctx = MakeCtx2D(w, h);
    // The rows are appended by the binder at the stream's first row-site draw (review F1), not here before the bind: everything the stream draws before it is under our rows, everything after over them.
    binder.SetSkip2DRows(true, [this, ctx, &rec, out](rdr::FrameBuilder& b) {
      if (out != rdr::kNoImage) {
        rdr::TargetsOp t{};
        t.color[0].image = out;
        t.color_count = 1;
        b.SetTargets(t);
      }
      q2d_census.rows_drawn += rdr_scene::q2d::Append(b, q2d_programs, ctx, rec.quads2d.data(), rec.quads2d.size()).drawn;
    });
  }
  // The 3D OURS path: counters and the first-reason census notes, then Apply2D on a 2D-whole frame. The census line printed here is [scene][2d-draw] (L2a's own counters), never the census's [scene][2d].
  void Draw2DOurs(Binder& binder, const LoadedFrame& frame, const rdr_scene::SceneTables& rec, rdr::FrameBuilder&, rdr::ImageId out, uint32_t w, uint32_t h, uint32_t frame_id, uint64_t jframe) {
    if (!q2d_on) return;
    namespace q = rdr_scene::q2d;
    Q2DCensus& c = q2d_census;
    ++c.frames;
    const Check2DResult r = Check2D(binder, frame, rec, w, h, frame_id, jframe);
    c.cross_pairs += r.cross.pairs, c.cross_target += r.cross.target_mismatch, c.cross_size += r.cross.size_mismatch, c.cross_scissor += r.cross.scissor_mismatch;
    switch (r.twod) {
      case scene_kind::TwoD::NotTaken: ++c.not_taken, fallback_census::NoteRefused(jframe, "not_taken"); break;
      case scene_kind::TwoD::Holes: fallback_census::NoteRows(rec.quads2d.size()), ++c.holes; break;
      case scene_kind::TwoD::NoOrigins: fallback_census::NoteRows(rec.quads2d.size()), ++c.no_origins, fallback_census::NoteRefused(jframe, "no_origins"); break;
      case scene_kind::TwoD::CountDiffers:
      case scene_kind::TwoD::ScanRefused: fallback_census::NoteRows(rec.quads2d.size()), ++c.scan_by[uint8_t(r.scan)], fallback_census::NoteRefused(jframe, q::PairWhyName(r.scan)); break;
      case scene_kind::TwoD::Refused:
        fallback_census::NoteRows(rec.quads2d.size()), ++c.refused;
        for (uint8_t i = 0; i < uint8_t(q::Refuse::Count); ++i) c.refused_by[i] += r.report.refused[i];
        if (r.blend_code >= 0) fallback_census::NoteBlendCode(jframe, uint32_t(r.blend_code));
        else if (r.first_reason) fallback_census::NoteRefused(jframe, r.first_reason);
        break;
      case scene_kind::TwoD::Whole: fallback_census::NoteRows(rec.quads2d.size()); break;
    }
    if (r.Whole()) {
      ++c.whole;
      Apply2D(binder, rec, out, w, h);
    } else binder.SetSkip2DRows(false);
    if (c.frames % 600 == 0) {
      std::string by;
      for (uint8_t i = 1; i < uint8_t(q::Refuse::Count); ++i)
        if (c.refused_by[i]) by += std::string(by.empty() ? "" : ", ") + q::RefuseName(q::Refuse(i)) + " " + std::to_string((unsigned long long)c.refused_by[i]);
      char l[900];
      std::snprintf(l, sizeof l, "[scene][2d-draw] ours frames %llu, 2d-whole %llu (rows drawn %llu, stream row draws left out %llu); stream 2D kept: not taken %llu, holes %llu, no origins %llu, scan refused (stream_target %llu, interleaved %llu, count %llu, blend_pair %llu), row refused %llu (%s); cross-check vs the stream (dev): pairs %llu, target class mismatch %llu, target size mismatch %llu, scissor mismatch %llu",
                    (unsigned long long)c.frames, (unsigned long long)c.whole, (unsigned long long)c.rows_drawn, (unsigned long long)binder.skip_2d_draws(), (unsigned long long)c.not_taken,
                    (unsigned long long)c.holes, (unsigned long long)c.no_origins, (unsigned long long)c.scan_by[uint8_t(q::PairWhy::Target)], (unsigned long long)c.scan_by[uint8_t(q::PairWhy::Interleaved)],
                    (unsigned long long)c.scan_by[uint8_t(q::PairWhy::Count)], (unsigned long long)c.scan_by[uint8_t(q::PairWhy::Blend)], (unsigned long long)c.refused, by.empty() ? "-" : by.c_str(), (unsigned long long)c.cross_pairs, (unsigned long long)c.cross_target, (unsigned long long)c.cross_size, (unsigned long long)c.cross_scissor);
      Log(l);
    }
  }
  uint64_t scene_kind_calls = 0;   // S103 L2dF: every SceneOurs call (the [scene][kind] cadence)
  uint64_t scene_ours_frames = 0, scene_walk_frames = 0, scene_holes = 0, scene_repeats = 0, scene_stale_inputs = 0;
  // S103 CLK2 (review M1 / H2): SceneOurs says what it did for the stream; the dispatch resolves it AFTER binder.Bind, when the binder has placed (or not found) our frame.
  struct StreamNote { uint64_t frame = 0; bool drawn = false; bool pending = false; } scene_stream_note;
  // The last OURS picture (the live drawer's sticky targets, FrontEnd::OwnTargets): repeated on a missed OURS frame.
  rdr::ImageId scene_last_out = rdr::kNoImage, scene_last_depth = rdr::kNoImage;
  uint32_t scene_last_w = 0, scene_last_h = 0;
  std::string scene_first_hole;
  uint64_t fc_handled = 0, fc_last = ~0ull;   // S103 L2b: the mode-2 frames this thread handled (the census compares it with the frames it closed)
  void CensusTick() {   // S103 L2b: the [scene][2d] line, every 600 closed frames
    for (std::string l = fallback_census::g_census.TakeLine(fc_handled - 1); !l.empty(); l = fallback_census::g_census.TakeLine(fc_handled - 1)) { Log(l.c_str()); const std::string ac = rdr_scene::q2d::AddressCensusLine(); if (!ac.empty()) Log(ac.c_str()); }   /* S103 RFX: the address-mode census follows each window line */   // (a scene change can have queued a window line before the full one)
  }
  void SceneOurs(Binder& binder, const LiveSource& source, const LoadedFrame& frame, rdr::FrameBuilder& fb, const Message& m) {
    const rdr_scene::SceneTables& rec = *m.scene_record;
    static census_flush::Site kind_flush;   // S102 CEN1: a flip phase end prints the census now
    const bool kind_flush_due = census_flush::Due(kind_flush);
    if (++scene_kind_calls % 600 == 0 || kind_flush_due) {
      Log(scene_kind::CensusLine().c_str());   // S103 L2dF: the per-kind census (L2d), every 600 SceneOurs calls of any outcome (it was only inside the OURS block before)
      if (scene_draw) Log(scene_draw->ReadyCensusLine(true).c_str());   // S103 NR: which term held READY, same cadence
      if (scene_draw && rdr::g_gpu_stamps.load(std::memory_order_relaxed)) Log(scene_draw->TexUpLine().c_str());   // S103 UP: stamped runs only
    }
    fallback_census::BeginFrame(m.guest_frame), CensusTick();   // S103 L2b: opens this frame in the census (closes the previous)
    if (!scene_draw) scene_draw = std::make_unique<rdr_scene::LiveSceneDraw>(*renderer);
    if (options.crowd_game_root) scene_draw->SetGameRoot(options.crowd_game_root());
    scene_draw->AdoptLoaded();
    scene_draw->ReleaseRetiredLive(m.guest_frame);   // S103 L31A
    const uint32_t frame_id = Binder::FindFrameImage(frame);
    uint32_t w = 0, h = 0;
    for (const ImageRecord& r : source.records)
      if (r.id == frame_id) { w = r.width, h = r.height; break; }
    std::vector<rdr_own::OwnedPassStamp> stamps;
    stamps.reserve(rec.owned_passes.size());
    uint8_t kind = 0;   // S103 L2d: the scene kind the decision used (the Scene3D stamp's pad; 0 = match = every registered input, as before)
    for (const rdr_scene::OwnedStamp& s : rec.owned_passes) {
      rdr_own::OwnedPassStamp st;
      st.pass = s.pass, st.decision = s.decision, st.why_not = s.why_not, st.pad = s.pad, st.inputs_mask = s.inputs_mask, st.guest_frame = s.guest_frame;   // every field (L2dF: pad was dropped; lint stamp_copy_lint.py)
      stamps.push_back(st);
      if (s.pass == uint8_t(rdr_own::Pass::Scene3D)) kind = scene_kind::KindOfPad(s.pad);   // S103 WB: bit 7 is the scene-walk bit, not part of the kind
    }
    const bool ours = scene_frame::DrawOurs(stamps.data(), stamps.size(), m.guest_frame);
    const uint32_t scene_req = scene_kind::RequiredFor(scene_kind::Kind(kind), rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load(std::memory_order_relaxed));
    const auto note_scene = [&] { fallback_census::NoteScene(m.guest_frame, fallback_census::SceneKey(stamps.data(), stamps.size(), m.guest_frame, scene_req)); };   // S103 L2b: the hook side reason
    if (!w || !h) {   // no frame image found (a menu with no present copy): nothing to draw into
      scene_stream_note = {m.guest_frame, false, true};   // S103 CLK2: nothing drawn
      if (!ours) note_scene();
      if (ours) { fallback_census::NoteRender(m.guest_frame, "no_frame_image"); ++scene_holes; if (scene_first_hole.empty()) scene_first_hole = "no frame image"; if (scene_frame::Miss(m.guest_frame)) Log("[scene][frame] ERROR latched off: no frame image"); }
      return;
    }
    if (!ours) {
      scene_stream_note = {m.guest_frame, false, true};   // S103 CLK2: the stream census (a walked record draws nothing of ours)
      ++scene_walk_frames;
      scene_draw->Warm(rec, w, h);
      scene_frame::SetReady(scene_draw->Ready());
      // S103 L2dF (LANDING2_PLAN 5 (a)): a FLAT frame (no stage, no 3D object, no Scene3D stamp: menus, select, title, load) has its own owner. Its verdict needs the 2D layer's check (dev --wwe2k14_rdr_2d_ours + _record) and
      // whole particles: an EMPTY pass (no node call / the node's own early-out) is whole (S103 MP A); menu particle groups are recorded with the node's screen camera and held (MP B: no flat particle drawer yet, blend code 0 [L], textures [U]). Fails closed.
      scene_kind::FlatFacts f;
      scene_draw->FlatFacts3D(rec, f);
      if (scene_kind::IsFlatFrame(f)) {
        static_assert(rdr_scene::kPiEmptyPass == 256 && rdr_scene::kPiInfoMask == (8u | 256u), "scene_kind::PartsOf hard-codes the info bits (kPiPivot | kPiEmptyPass): keep it in step with particle_tables.h");   // S103 MPBF 10
        f.particles = scene_kind::PartsOf(rec.particles.EmptyPass(), uint32_t(rec.particles.batches.size()), rec.particles.status.empty() ? 0u : rec.particles.status[0].reasons, !rec.particles.screen.empty());   // S103 MP: an empty pass is whole; screen-camera batches are Held (the flat particle drawer is not built); anything else not recorded
        if (q2d_on) {
          const Check2DResult c2 = Check2D(binder, frame, rec, w, h, frame_id, m.guest_frame);
          f.twod = c2.twod, f.refused_other = c2.refused_other, f.movie_rows = c2.movie_rows, f.movie_planes_missing = c2.movie_planes_missing;
        }
        const scene_kind::FlatWhy flat_why = scene_kind::FlatVerdict(f);
        scene_kind::NoteFlat(flat_why);
        if (scene_kind::Apply2DAllowed(flat_why)) Apply2D(binder, rec, rdr::kNoImage, w, h);   // only after an Ours verdict: a menu whose 2D is left out of the stream but not drawn by us would lose its UI
        else fallback_census::NoteScene(m.guest_frame, std::string("flat_") + scene_kind::FlatWhyName(flat_why));
      } else note_scene();
      return;
    }
    rdr::ImageId out = rdr::kNoImage, out_depth = rdr::kNoImage;
    std::string why;
    bool loading = false;   // S103 L31A
    // S103 S (docs\renderer\OURS_INPUT_AUDIT.md item 7): the decision proved every required input was captured one frame EARLIER; this record's
    // own stamps (one snapshot at the end of its latch, live_scene SetStampFill) must show each required input captured for this record's hook
    // frame or later, or the record repeats older data (a reader that failed this frame): a miss, never an OURS picture. Skipped with the DEV
    // force-inputs switch, like the decision's own check.
    bool fresh = true;
    if (!scene_frame::g_force_inputs.load(std::memory_order_relaxed)) {
      const uint64_t hook = scene_frame::HookFrameOf(m.guest_frame);
      uint32_t ok = 0;
      for (const rdr_scene::InputStampRow& r : rec.input_stamps)
        if (r.bit < 32 && r.frame >= hook) ok |= 1u << r.bit;
      const uint32_t stale = scene_kind::StaleBits(kind, rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load(std::memory_order_relaxed), ok);   // the kind's mask (L2d; unit-tested: l2d_unit [freshness])
      if (stale) {
        fresh = false;
        int b = 0;
        while (!((stale >> b) & 1u)) ++b;
        why = "record inputs not fresh for hook frame " + std::to_string((unsigned long long)hook) + " (first: bit " + std::to_string(b) + ")";
        ++scene_stale_inputs;
      }
    }
    if (fresh && scene_draw->DrawLive(rec, fb, w, h, /*post=*/nullptr, out, why, &out_depth, &loading) && out != rdr::kNoImage) {
      ++scene_ours_frames;
      scene_stream_note = {m.guest_frame, true, true};   // S103 CLK2: DrawLive drew this record (counted drawn only once the binder placed it: the dispatch, after Bind)
      Draw2DOurs(binder, frame, rec, fb, out, w, h, frame_id, m.guest_frame);   // S103 L2a: decides before the bind; our rows are appended by the binder at the stream's first row-site draw (dev switch; off = one bool test)
      binder.SetSceneFrame(out, out_depth);
      scene_last_out = out, scene_last_depth = out_depth, scene_last_w = w, scene_last_h = h;
    } else {
      scene_stream_note = {m.guest_frame, false, true};   // S103 CLK2: an OURS record DrawLive did not draw
      ++scene_holes;
      // FRAME_CONTRACT 3 / scene_frame.h: a missed OURS frame repeats the last presented picture. The game skipped its walks, so image 8 holds the
      // last WALKED frame (T review B3). Every DrawLive failure before "draw:" returns before recording, so our target still holds the last OURS
      // picture: alias it again (same size only: a resize makes new targets). After a failed Record the target may be partly drawn: not reused.
      if (scene_last_out != rdr::kNoImage && scene_last_w == w && scene_last_h == h && why.rfind("draw:", 0) != 0) {
        binder.SetSceneFrame(scene_last_out, scene_last_depth);
        ++scene_repeats;
      }
      fallback_census::NoteRenderWhy(m.guest_frame, why, loading);   // S103 L2b
      if (scene_first_hole.empty()) scene_first_hole = why;
      if (scene_frame::Miss(m.guest_frame, loading)) Log(("[scene][frame] ERROR latched off: " + why).c_str());   // S103 L31A: a resource still loading is a hole, not a strike (loading_latch episodes in a window still latch)
      if (scene_frame::TakeLoadingWarn()) Log(("[scene][frame] WARN: loading misses keep coming (last: " + why + ")").c_str());
    }
    scene_frame::SetReady(scene_draw->Ready());
    static census_flush::Site render_flush;   // S102 CEN1: a flip phase end prints the census now
    const bool render_flush_due = census_flush::Due(render_flush);
    if ((scene_ours_frames + scene_walk_frames + scene_holes) % 600 == 0 || render_flush_due) {
      char l[420];
      std::snprintf(l, sizeof l, "[scene][render] ours %llu, walked %llu, holes %llu (repeated our last %llu; record inputs stale %llu; first: %s), frame image unfound %llu; %s",
                    (unsigned long long)scene_ours_frames, (unsigned long long)scene_walk_frames, (unsigned long long)scene_holes,
                    (unsigned long long)scene_repeats, (unsigned long long)scene_stale_inputs,
                    scene_first_hole.empty() ? "-" : scene_first_hole.c_str(), (unsigned long long)binder.scene_frame_unfound(), scene_frame::CensusLine().c_str());
      Log(l);
      Log(scene_frame::missing_hist::MissingLine().c_str());   // S103 X1: the set of missing input bits per frame, this window
      Log(::scene_skip::StreamHookLine().c_str());   // S103 CLK2: the per-hook stream census
    }
  }

  // S103 lane S (FRAME_CONTRACT.md 2, mirror): the game's picture of this frame (read back with the display curve, as the game shows it) and
  // the present's own scene record drawn ALONE by our record -> frame path at the same size: two PNGs in the picture folder, one log line.
  // Waits on the GPU (dev comparison only, once per sheet frame).
  void MirrorSheet(Binder& binder, const Message& message) {
    const rdr::ImageRef ref = binder.last_present().source;
    if (ref.image == rdr::kNoImage) return;
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    if (!renderer->ReadImage(ref, pixels, width, height) || !width || !height) { Log("[scene][mirror] the game's picture could not be read back"); return; }
    if (shown_curve.size() == 1024)
      for (size_t i = 0; i + 3 < pixels.size(); i += 4)
        for (uint32_t c = 0; c < 3; ++c) pixels[i + c] = shown_curve[4 * pixels[i + c] + c];
    std::error_code ec;
    std::filesystem::create_directories(options.png_folder, ec);
    const std::string n = std::to_string(message.guest_frame);
    QueuePicture(options.png_folder / ("live_scene_game_f" + n + ".png"), std::move(pixels), width, height);
    if (!scene_draw) scene_draw = std::make_unique<rdr_scene::LiveSceneDraw>(*renderer);
    if (options.crowd_game_root) scene_draw->SetGameRoot(options.crowd_game_root());
    std::string report;
    if (message.stage_mirror) scene_draw->SetMapDump(options.dump_targets_dir, message.guest_frame);   // S103 T7 (empty list = off): our maps by role, same frame
    scene_draw->DrawToPng(*message.scene_record, width, height, shown_curve, options.png_folder / ("live_scene_ours_f" + n + ".png"), report);
    Log(report.c_str());
    // S103 main: the drawer's census (maps made / chained, shadow / plane / rim bound, key light) with every mirror picture: it printed only at Stop,
    // which a run's exit never reached, so no mirror run ever showed whether the wrestler shadow / rim passes ran live.
    Log(("[scene][mirror] f" + n + " " + scene_draw->Census()).c_str());
  }

  void WritePicture(Binder& binder, const std::string& label) {
    std::error_code ec;
    std::filesystem::create_directories(options.png_folder, ec);
    WritePictureTo(binder, options.png_folder / ("live_" + label + ".png"), rdr::kNoImage, false, /*async=*/true);
    if (const rdr::ImageId dlss = renderer->dlss_output(); dlss != rdr::kNoImage)  // S10b: DLSS's picture of it (before the HUD)
      WritePictureTo(binder, options.png_folder / ("live_" + label + "_dlss.png"), dlss, false, /*async=*/true);
  }

  // Photo mode (lane 201, GATE_FAULTS row 31): the request SavePicture left is served by the first presented frame, and KEPT until a
  // frame has been written (a failed read or write, or no picture yet, tries again on the next presented frame, up to photo::kMaxTries),
  // then dropped with the reason in the log. Every outcome is logged; before lane 201 a failure after "saving ..." was silent.
  void ServePhoto(Binder& binder) {
    std::filesystem::path photo;
    uint32_t tries = 0;
    {
      std::lock_guard lock(overlay_mu);
      photo = photo_path;
      tries = photo_tries;
    }
    if (photo.empty()) return;
    if (photo::Admit(g_hide_hud.load(std::memory_order_relaxed)) == photo::Action::GiveUp) {  // photo mode ended before a frame served the request
      Log(("[rdr][live][photo] WARN " + photo::Utf8(photo) + ": photo mode ended before the picture was written; dropped").c_str());
      std::lock_guard lock(overlay_mu);
      if (photo_path == photo) photo_path.clear(), photo_tries = 0;
      return;
    }
    // The picture the player sees: DLSS's output when this present drew it and nothing was drawn after DLSS's input copy (the present shows
    // dlss_out + (final - before_hud): a 30 Years filter pass kept by photo mode's HUD hiding draws after the copy, and the bare output would
    // miss it), already the shown size; else the game's presented image (the whole picture, HUD hidden), stretched back to the window's shape
    // when the screen-shape setting squeezed it (RecordPresent, present_fill).
    const bool dlss = renderer->dlss_in_last_present() && renderer->dlss_output() != rdr::kNoImage && binder.kept_after_dlss_copy() == 0;
    const rdr::ImageId image = dlss ? renderer->dlss_output() : rdr::kNoImage;
    const photo::Result result = WritePictureTo(binder, photo, image, /*stretch=*/!dlss && options.present_fill);
    const photo::Action action = photo::AfterAttempt(result, tries);
    if (result != photo::Result::Ok) {
      char line[1024];
      std::snprintf(line, sizeof(line), "[rdr][live][photo] WARN %s: %s (attempt %u)%s", photo::Utf8(photo).c_str(), photo::Why(result), tries,
                    action == photo::Action::GiveUp ? "; giving up" : "; trying on the next presented frame");
      if (action == photo::Action::GiveUp || tries == 1) Log(line);  // the first failure and the last: not 90 lines
    } else if (dlss) {
      Log("[rdr][live][photo] the picture is DLSS's output (shown size)");
    } else if (binder.kept_after_dlss_copy() != 0 && renderer->dlss_in_last_present()) {
      Log("[rdr][live][photo] DLSS ran but draws came after its input copy (30 Years filter): the picture is the game's presented image");
    }
    std::lock_guard lock(overlay_mu);
    if (photo_path != photo) return;  // a newer request replaced it meanwhile: it starts its own count
    if (action == photo::Action::Retry) photo_tries = tries;
    else photo_path.clear(), photo_tries = 0;
  }

  // The last presented picture, as shown (through the display table the present applies), to `path`. `stretch` (lane 201): the picture is
  // the squeezed source and is stretched to the shown shape first (options.shown_wide / shown_tall).
  // Lane 302: the PNG encode + write + rename of a picture whose pixels are already read back. `part_file` is the temp file opened early by the
  // synchronous path, or null (the worker opens it here). True when the picture is on disk under `path`.
  bool FinishPng(FILE* part_file, const std::filesystem::path& path, const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height) {
    std::filesystem::path part = path;
    part += ".part";
    if (!part_file) part_file = rdr::PngOpen(part);
    if (!part_file) return false;
    bool written = rdr::WritePngFile(part_file, pixels.data(), width, height);
    if (written) {
      std::error_code ec;
      std::filesystem::rename(part, path, ec);
      if (ec) {  // (rename over an existing file can fail on some systems: remove it and try once more)
        std::filesystem::remove(path, ec);
        std::filesystem::rename(part, path, ec);
      }
      written = !ec;
    }
    if (!written) {
      std::error_code ec;
      std::filesystem::remove(part, ec);
    }
    if (written) {
      ++pngs;
      char line[512];
      std::snprintf(line, sizeof(line), "[rdr][live] picture: %s (%ux%u)", photo::Utf8(path).c_str(), width, height);
      Log(line);
    }
    return written;
  }

  void PicWorker() {
    std::unique_lock lock(pic_mu);
    for (;;) {
      pic_cv.wait(lock, [&] { return pic_stop || !pic_q.empty(); });
      if (pic_q.empty()) return;  // stopping, nothing left
      PicJob job = std::move(pic_q.front());
      pic_q.pop_front();
      lock.unlock();
      const auto t0 = std::chrono::steady_clock::now();
      const bool ok = FinishPng(nullptr, job.path, job.pixels, job.width, job.height);
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      wwe2k14::perf_sys::NoteFileWrite(double(ms));  // the [perf][sys] line's file_write_ms
      if (!ok) Log(("[rdr][live] WARN picture not written: " + photo::Utf8(job.path)).c_str());
      else if (ms >= 500) Log(("[rdr][live] WARN slow picture write (" + std::to_string(ms) + " ms, off the render thread): " + photo::Utf8(job.path)).c_str());
      job.pixels = {};
      lock.lock();
      int run_done = 0;
      if (--pic_jobs == 0) run_done = std::exchange(pic_deferred_done, 0);
      lock.unlock();
      for (int i = 0; i < run_done; ++i) {
        if (options.picture_done) options.picture_done();
      }
      lock.lock();
    }
  }

  // Bounded (S101 lane 302 review S3): a 1440p picture is 14.7 MB of pixels, 4K 33 MB; with png_every=1 or a stalled disk the queue would grow
  // without limit. At kMaxPicJobs queued + running the render thread writes this one itself (the old behaviour: it waits on the disk).
  static constexpr int kMaxPicJobs = 4;
  void QueuePicture(const std::filesystem::path& path, std::vector<uint8_t>&& pixels, uint32_t width, uint32_t height) {
    {
      std::lock_guard lock(pic_mu);
      if (pic_jobs < kMaxPicJobs) {
        if (!pic_thread.joinable()) pic_thread = std::thread([this] { wwe2k14::NameThread(L"rdr_pic"); PicWorker(); });
        pic_q.push_back(PicJob{path, std::move(pixels), width, height});
        ++pic_jobs;
        pic_cv.notify_one();
        return;
      }
    }
    FinishPng(nullptr, path, pixels, width, height);  // synchronous: backpressure
  }

  // The render thread's end-of-frame "named picture handled": now, or when the queued writes are on disk.
  void NamedPictureDone() {
    if (!options.picture_done) return;
    {
      std::lock_guard lock(pic_mu);
      if (pic_jobs > 0) {
        ++pic_deferred_done;
        return;
      }
    }
    options.picture_done();
  }

  void StopPictureWriter() {
    {
      std::lock_guard lock(pic_mu);
      pic_stop = true;
    }
    pic_cv.notify_all();
    if (pic_thread.joinable()) pic_thread.join();
  }

  photo::Result WritePictureTo(Binder& binder, const std::filesystem::path& path, rdr::ImageId image = rdr::kNoImage, bool stretch = false, bool async = false) {
    rdr::ImageRef ref = binder.last_present().source;
    if (image != rdr::kNoImage) ref.image = image, ref.mip = 0, ref.layer = 0;  // S10b: DLSS's output
    if (ref.image == rdr::kNoImage) return photo::Result::NoPicture;
    // S101 lane 19 (case 7041 / the Bryan zip): written under a temp name and renamed when complete, so a reader (the case worker) never sees a half-written PNG.
    // Lane 201: opened BEFORE the read-back and the stretch (a path that cannot be written fails at once, not after a device wait), and by a
    // wide path (path.string() throws on names the ANSI code page cannot hold: a CJK user name, OneDrive).
    std::filesystem::path part = path;
    part += ".part";
    FILE* part_file = nullptr;  // (the async path opens its file on the worker)
    if (!async) {
      part_file = rdr::PngOpen(part);
      if (!part_file) return photo::Result::WriteFailed;
    }
    std::vector<uint8_t> pixels;
    uint32_t width = 0, height = 0;
    if (!renderer->ReadImage(ref, pixels, width, height)) {
      if (part_file) {
        std::fclose(part_file);
        std::error_code ec;
        std::filesystem::remove(part, ec);
      }
      return photo::Result::ReadFailed;
    }
    if (shown_curve.size() == 1024) {
      for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        for (uint32_t c = 0; c < 3; ++c) pixels[i + c] = shown_curve[4 * pixels[i + c] + c];
      }
    }
    if (stretch && width && height) {
      uint32_t out_w = width, out_h = height;
      photo::ShownSize(width, height, options.shown_wide, options.shown_tall, out_w, out_h);
      if (out_w != width || out_h != height) {
        pixels = photo::Stretch(pixels.data(), width, height, out_w, out_h);
        width = out_w, height = out_h;
      }
    }
    if (async) {
      QueuePicture(path, std::move(pixels), width, height);
      return photo::Result::Ok;
    }
    return FinishPng(part_file, path, pixels, width, height) ? photo::Result::Ok : photo::Result::WriteFailed;
  }

  // S103 lane SHOT. Render thread. PublishShot answers every request up to `covers`.
  void PublishShot(uint64_t covers, bool ok, std::vector<uint8_t>&& rgba, uint32_t w, uint32_t h, std::vector<uint8_t>&& curve) {
    auto ans = std::make_shared<ShotAnswer>();
    ans->covers = covers, ans->ok = ok, ans->rgba = std::move(rgba), ans->w = w, ans->h = h, ans->curve = std::move(curve);
    std::lock_guard lock(shot_mu);
    shot_ans = ans;
    if (shot_covered < covers) shot_covered = covers;
    shot_pending.store(shot_req > shot_covered || shot_inflight, std::memory_order_release);
    shot_cv.notify_all();
  }
  // The picture shown is the last present's source image; only an 8-bit one reads back as shown (review F3: 16F is scaled by its own peak, RGB10A2 is refused). Anything else is answered "no", with one log line.
  bool ShotSource(Binder& binder, rdr::ImageRef& ref) {
    ref = binder.last_present().source;
    if (ref.image == rdr::kNoImage) return false;
    if (!renderer->ImageIs8Bit(ref.image)) {
      if (!shot_warned) shot_warned = true, Log("[rdr][live] WARN screenshot / clip capture: the presented image is not 8 bits a channel, no picture is taken");
      return false;
    }
    return true;
  }
  void QueueShot(Binder& binder) {
    if (shot_inflight) return;
    uint64_t covers = 0;
    {
      std::lock_guard lock(shot_mu);
      if (shot_req <= shot_covered) return;
      covers = shot_req;
    }
    rdr::ImageRef ref;
    shot_tag = (uint64_t(1) << 62) + (++shot_tag_n);
    if (!ShotSource(binder, ref) || !renderer->QueueReadback(ref, shot_tag)) { PublishShot(covers, false, {}, 0, 0, {}); return; }
    shot_inflight = true, shot_inflight_covers = covers, shot_inflight_curve = shown_curve;
    std::lock_guard lock(shot_mu);
    shot_covered = std::max(shot_covered, covers);
  }
  void FinishShot(rdr::Renderer::ReadbackResult& r) {
    shot_inflight = false;
    PublishShot(shot_inflight_covers, !r.cancelled && r.width && r.height, std::move(r.rgba), r.width, r.height, std::move(shot_inflight_curve));
  }
  void ServeShotSync(Binder& binder) {
    if (shot_inflight) return;
    uint64_t covers = 0;
    {
      std::lock_guard lock(shot_mu);
      if (shot_req <= shot_covered) return;
      covers = shot_req;
    }
    std::vector<uint8_t> pixels;
    uint32_t w = 0, h = 0;
    rdr::ImageRef ref;
    const bool ok = ShotSource(binder, ref) && renderer->ReadImage(ref, pixels, w, h) && w && h && pixels.size() >= size_t(w) * h * 4;
    PublishShot(covers, ok, std::move(pixels), w, h, std::vector<uint8_t>(shown_curve));
  }
  uint64_t shot_tag_n = 0;

  std::string Report() const {
    char line[640];
    std::snprintf(line, sizeof(line), "frames drawn %llu, skipped %llu (lasting work run in %llu; per-frame copies left to the drawn frame %llu; drawn by the 100 ms guard %llu); draws %llu, bound %llu, missing a shader %llu; pictures %llu; read-back parts %llu",
                  static_cast<unsigned long long>(drawn.load()), static_cast<unsigned long long>(skipped.load()), static_cast<unsigned long long>(persistent_only.load()),
                  static_cast<unsigned long long>(per_frame_copies_left.load()), static_cast<unsigned long long>(starved_draws.load()),
                  static_cast<unsigned long long>(draws.load()), static_cast<unsigned long long>(bound.load()),
                  static_cast<unsigned long long>(no_shader.load()), static_cast<unsigned long long>(pngs.load()),
                  static_cast<unsigned long long>(parts.load()));
    std::string out = line;
    if (streaks.by_image_left) { std::snprintf(line, sizeof(line), "; per-frame copies found by the image rule %llu", static_cast<unsigned long long>(streaks.by_image_left)); out += line; }
    if (streaks.by_image_guarded) { std::snprintf(line, sizeof(line), "; image rule held back by a guard %llu", static_cast<unsigned long long>(streaks.by_image_guarded)); out += line; }
    if (lasting_drawn.load()) { std::snprintf(line, sizeof(line), "; lasting frames drawn instead of skipped %llu (%llu draws, the lasting bind would have kept %llu)", static_cast<unsigned long long>(lasting_drawn.load()), static_cast<unsigned long long>(lasting_drawn_draws.load()), static_cast<unsigned long long>(lasting_drawn_kept.load())); out += line; }   // S102 T3 (a)
    if (prune_frames.load()) {   // lane 307
      std::snprintf(line, sizeof(line), "; lasting binds %llu: kept %llu of %llu ops, %llu of %llu draws, %llu of %llu passes (%llu only by the model-part group), %llu lasting copies, %llu per-frame copies walked through, %llu binds without D3 (no object pass kept)",
                    static_cast<unsigned long long>(prune_frames.load()), static_cast<unsigned long long>(prune_ops_kept.load()), static_cast<unsigned long long>(prune_ops_in.load()),
                    static_cast<unsigned long long>(prune_draws_kept.load()), static_cast<unsigned long long>(prune_draws_in.load()), static_cast<unsigned long long>(prune_passes_kept.load()),
                    static_cast<unsigned long long>(prune_passes_in.load()), static_cast<unsigned long long>(prune_grouped.load()), static_cast<unsigned long long>(prune_seeds.load()),
                    static_cast<unsigned long long>(prune_promoted.load()), static_cast<unsigned long long>(prune_blind.load()));
      out += line;
    }
    if (catch_up_copies.load() || catch_up_not_state.load() || catch_up_too_big.load()) {  // A1b
      std::snprintf(line, sizeof(line), "; state steps caught up %llu (in %llu copies; per-frame copies not state %llu, too big %llu)",
                    static_cast<unsigned long long>(catch_up_steps.load()), static_cast<unsigned long long>(catch_up_copies.load()),
                    static_cast<unsigned long long>(catch_up_not_state.load()), static_cast<unsigned long long>(catch_up_too_big.load()));
      out += line;
    }
    return out;
  }
};

LiveSink::LiveSink() : impl_(new Impl) { impl_->geometry.reserve(size_t(1) << 20); }  // lane 273: no insert under store_mu rehashes ~400k ranges (12-80 ms "sink appends" in runs 7189 / 7190)
LiveSink::~LiveSink() {
  Stop();
  delete impl_;
}

namespace {
void (*g_inner_log)(const char*) = nullptr;  // S101 lane 19: the options' own log function, called after the case zip has read the line
void LogTee(const char* line) {
  rdr_case::info::NoteRendererLine(line);
  if (g_inner_log) g_inner_log(line);
  else std::fprintf(stderr, "%s\n", line);  // review fix 10: no log function given: the lines still go somewhere
}
}  // namespace

bool LiveSink::Start(const LiveOptions& options, std::string& error) {
  if (impl_->active) return true;
  thin::g_pins.Reset();  // lane 122 (review 8): a new session starts with no pins (generation 0)
  impl_->options = options;
  // S101 lane 19: the renderer's own "[rdr] device / swapchain / display timing" lines also feed the case zip's device block (no second query).
  // START-ONLY(ON): the stream sink's own log hook (the stream-free host tees through host_start.cpp HostLog)
  g_inner_log = options.log;
  impl_->options.log = &LogTee;
  rdr::SetLogSink(&LogTee);
  rdr_case::info::SetRender(uint32_t(std::ceil(1280.0f * options.render_scale)), uint32_t(std::ceil(720.0f * options.render_scale)), options.render_scale);
  rdr::RendererOptions ro;
  ro.validation = options.validation;
  // START-ONLY(ON): dev test mask (lane 200); its cvar exists only in the stream build
  ro.format_caps_mask = options.format_caps_mask;  // lane 200
  ro.bc_decode = options.bc_decode;                // S102 U1
  ro.gpu_name = options.gpu_name;                  // S102 U1 (R9 finding 3)
  ro.window.instance = options.window_instance;  // null window: headless (stage A, the mirror); a window: the renderer presents to it (stage B)
  ro.window.window = options.window;
  ro.window.current = options.window_source;  // lane 235
  ro.window.current_user = options.window_source_user;
  ro.vsync = options.vsync;
  ro.render_scale = options.render_scale;  // S100 settings parity S1: the launcher's RESOLUTION (images marked kUsageScaled)
  ro.present_fill = options.present_fill;  // settings parity S5
  ro.present_sgsr = options.present_sgsr;  // settings parity S10a
  ro.present_noclear = options.present_noclear;  // S102 G1 lever 2
  ro.exclusive_fullscreen = options.exclusive_fullscreen;  // S102 G2b
  ro.robust_buffers = options.robust_buffers;  // S102 D1
  // START-ONLY(ON): the pre-light cubes' compute pipelines are the stream binder's (its consumers are binder-only: SP2 M8)
  ro.light_cube_compute = options.prelight_compute;  // S102 L1b review F2
  // START-ONLY(ON): layered pre-light is the stream binder's (consumers binder-only)
  ro.multiview = options.prelight_layered;  // S102 C1 review F2
  ro.present_hz = options.present_hz;  // S100 phone 120 fix 2
  ro.gpu_timestamps = options.gpu_timestamps;  // lane 269
  ro.counts = options.counts;  // S102 M1
  ro.pipeline_store_dir = options.pipeline_store_dir, ro.pipeline_prebuild_slice_us = options.pipeline_prebuild_slice_us;  // lane 276
  // START-ONLY(ON): DLSS / DLAA is not in the stream-free build (start_settings.cpp RefuseSetting says so in the log)
  ro.dlss_mode = options.dlss_mode, ro.streamline_dir = options.streamline_dir;  // settings parity S10b
  g_dlss = options.dlss_mode >= 0, g_dlss_phases = options.dlss_phases;
  g_velocity_legacy = options.dlss_jitter_legacy;  // lane 185
  // S103 SPB: the player's quality (crowd density / shadows / depth of field / film grain / shadow half rate, S6 / S7 / S8 / S12 / S3) is ONE PlayerQuality, filled by ApplyStartSettings() (start_settings.cpp) and
  // read through the binder's references (binder.h g_crowd_density ...) and by the stream-free scene draw. The rank tracker's switch follows it (S101 lane 234).
  const wwe2k14::start::Applied& applied = wwe2k14::start::ApplyStartSettings();   // before the binder's first frame (StartLive already called it: idempotent)
  // START-ONLY(ON): the stream binder's tracker object; the stream-free LiveCrowd owns its own tracker and reads the same PlayerQuality::density_track (live_crowd.cpp)
  g_crowd_rank.enabled = applied.quality.density_track;
  g_crowd_ours = options.crowd_ours;  // S101 lane 35
  g_post_ours = options.post_ours;    // S101 lane PO
  g_arena_ours = options.arena_ours, g_wrestlers_ours = options.wrestlers_ours;  // S101 lane 41
  g_caw_ours = options.caw_ours;  // lane 153
  // START-ONLY(ON): the Create a Superstar replacer's counts-index cache (the stream's caw_ours)
  g_caw_cache_root = options.caw_cache_root;
  g_ring_ours = options.ring_ours;  // S101 ring contract
  d3::g_arena_d3 = options.arena_d3 && options.arena_thin && !g_dlss;  // lane 138 (D3): needs the arena's thin draws; the guest hook's switch (d3::g_hook_on) is set with it
  d3::g_verify = d3::g_arena_d3 && options.arena_d3_verify;   // lane 152 (10)
  d3::g_hook_on.store(d3::g_arena_d3 && d3::kRecordRouteBuilt, std::memory_order_release);   // review A: the guest hook stays off until the record route exists (lane 152: built)
  d3::g_caster_d3 = options.caster_d3 && !g_dlss;   // lane 228 (the live seam, 17.12): the caster chain; options.caster_d3 already needs the ours hooks, scene records, light states and owner mode (reader.h)
  d3::g_caster_verify = d3::g_caster_d3 && options.caster_d3_verify;
  // S102 P4P5 (OWN_SHADOW_RIM.md section 5): readiness of the owned nodes = "can draw the pass WITHOUT STREAM ANCHORS". The classes that draw these passes (casters, rim map, circles, shadows_ours) still take their
  // anchors from the stream's own draws, which a skipped node does not record: false for both until the readers of section 5 feed them from the records. The node hooks (scene_live/own_node_hook.h) read it.
  rdr_own::shadow_rim::SetReadyFromSwitches(false, false);
  d3::g_caster_plane = d3::g_caster_d3 && options.caster_d3_plane;   // lane 268
  d3::g_caster_hook_on.store(d3::g_caster_d3 && d3::kRecordRouteBuilt, std::memory_order_release);   // the caster hooks stay off until the chain exists; the gate's latch clears it (binder.h D3CEndFrame)
  d3::g_ring_d3 = options.ring_d3 && !g_dlss;   // lane 246 (the live seam, 16.9): the ring chain; options.ring_d3 already needs the R2 record (--wwe2k14_rdr_ring_d3), ring thin, light states and owner mode (reader.h). NOT the cvar --wwe2k14_rdr_ring_d3 (16.8 N5)
  d3::g_ring_verify = d3::g_ring_d3 && options.ring_d3_verify;
  d3::g_ring_hook_on.store(d3::g_ring_d3 && d3::kRecordRouteBuilt, std::memory_order_release);   // the ring hook stays off until the chain exists; the gate's latch clears it (binder_ring_d3.inc D3REndFrame)
  d3::g_wrestlers_d3 = options.wrestlers_d3 && !g_dlss;   // lane 245 (the live seam, 18.9): the wrestler chain; options.wrestlers_d3 already needs the ours hooks + thin draws, scene records, light states and owner mode (reader.h)
  d3::g_wrestlers_verify = d3::g_wrestlers_d3 && options.wrestlers_d3_verify;
  d3::g_wrestlers_hook_on.store(d3::g_wrestlers_d3 && d3::kRecordRouteBuilt, std::memory_order_release);   // the wrestler scope stays off until the chain exists; the gate's latch clears it (binder.h D3WEndFrame)
  thin::g_arena_thin = options.arena_thin && !g_dlss, thin::g_ring_thin = options.ring_thin && !g_dlss, thin::g_wrestlers_thin = options.wrestlers_thin && !g_dlss;  // lane 129 (KC): the DLSS-off gate for all three classes  // lane 122 (D1 thin draws; not with DLSS: the velocity walk draws the stream's vertices)
  g_ui_ours = options.ui_ours;  // S101 UI contract
  rdr_scene::g_ui_quads = options.ui_quads;  // S101 lane 148: read by MakeUiOurs when the render thread builds the hook
  g_movie_ours = options.movie_ours;  // S101 lane 131
  g_ui_text_ours = options.ui_text_ours;  // S101 lane 89 (UT)
  // START-ONLY(ON): run-string labels for the stream's ui_text hook; the stream-free host does not read run strings (SP2 M11)
  origin::g_labels_wanted.store(options.ui_text_ours || wwe2k14::caw::g_on.load(std::memory_order_relaxed), std::memory_order_relaxed);  // S101 lane 118 review: never false over a `!caw on`; S101 lane 89: the run strings the text hook lays out are read only while labels are on
  g_flash_ours = options.flash_ours;  // S101 flash contract
  g_props_ours = options.props_ours;  // S101 lane 84
  g_screens_ours = options.screens_ours;  // S101 lane 115
  g_crowd_props_ours = options.crowd_props_ours;  // S101 lane 133
  g_shadow_casters_ours = options.shadow_casters_ours;  // S101 lane 99
  g_rim_ours = options.rim_ours;  // lane 124
  g_circles_ours = options.circles_ours;  // lane 136
  g_plane_shadow_ours = options.plane_shadow_ours;  // lane 139
  g_small_passes_ours = options.small_passes_ours;  // lane 135
  g_rim_chain_ours = options.rim_chain_ours;  // lane 151
  g_particles_ours = options.particles_ours;  // S101 lane 67
  // START-ONLY(ON): the stream's resolve-copy aliasing
  copy_alias::g_mode = options.copy_alias, copy_alias::g_classes = options.copy_alias_classes;  // S101 lane 190
  g_shadows_ours = options.shadows_ours, g_shadow_map_size = options.shadow_map_size, g_shadow_plane_percent = options.shadow_plane_percent, g_shadow_render_scale = options.render_scale;  // S101 lane 123
  impl_->renderer = rdr::Renderer::Create(ro, error);
  if (!impl_->renderer) return false;
  if (options.arena_ours) impl_->Log("[rdr][arena] the arena is drawn by us in place from the scene record (switch on)");
  if (options.arena_thin) impl_->Log(thin::g_arena_thin ? "[rdr][thin] arena thin draws ON: pinned keys skip the reader's geometry build, the binder draws their templates (every 32nd frame of a key takes the full path as its proof); census [rdr][thin] every 600 frames"
                                                       : "[rdr][thin] arena thin draws requested but OFF (DLSS is on: the velocity walk draws the stream's vertices)");  // lane 122
  if (options.ring_thin) impl_->Log(thin::g_ring_thin ? "[rdr][thin] ring thin draws ON: the key's ordinal carries the instance; rope slot 0 and the skirt tie stay as the hook chose them per key (proved every 32nd frame); census [rdr][thin] every 600 frames"
                                                      : "[rdr][thin] ring thin draws requested but OFF (DLSS is on: the velocity walk draws the stream's vertices)");  // lane 129
  if (options.wrestlers_thin) impl_->Log(thin::g_wrestlers_thin ? "[rdr][thin] wrestlers thin draws ON: the template's geometry with the stream's own palette rows; cloth and refused parts stay on the full path; census [rdr][thin] every 600 frames"
                                                                : "[rdr][thin] wrestlers thin draws requested but OFF (DLSS is on: the velocity walk draws the stream's vertices)");  // lane 129
  if (options.post_ours) impl_->Log("[rdr][post] the post chain (depth of field, bloom, glare, the combine, the film) is recorded by our own pass from the last dof draw; the stream's chain draws and the copies they feed are dropped; a chain the plan cannot read stays the stream's (switch on)");  // S101 lane PO
  if (options.caw_ours) impl_->Log("[rdr][caw] the Create a Superstar editor's wrestler is drawn by us in place, part by part from the edit pacs (switch on; no scene record needed)");   // lane 153
  if (options.wrestlers_ours) impl_->Log("[rdr][wrestlers] the wrestlers are drawn by us in place from the scene record (switch on)");
  if (options.ring_ours) impl_->Log("[rdr][ring] the ring is drawn by us in place from the scene record (switch on)");
  if (options.flash_ours) impl_->Log("[rdr][flash] the crowd flashes are drawn by us in place from the arena file (switch on)");
  if (options.screens_ours) impl_->Log("[rdr][screens] the stage screens' geometry (ribbon band, wall, mini-tron) is drawn by us in place from the arena file; the video planes stay the stream's (switch on)");  // S101 lane 115
  if (options.crowd_props_ours) impl_->Log("[rdr][crowd_props] the crowd's sign / foam-hand draws are drawn by us in place from the arena file, each checked byte for byte (switch on)");  // S101 lane 133
  if (options.props_ours) impl_->Log("[rdr][props] the commentary table, the ring steps and the announcer's microphone are drawn by us in place from the game files (switch on)");  // S101 lane 84
  if (options.shadow_casters_ours) impl_->Log("[rdr][shadow_casters] our caster geometry is drawn in the shadow passes (plane mask, wrestler maps) in place of the game's, from the game files (switch on)");  // S101 lane 99
  if (options.shadows_ours) impl_->Log(("[rdr][shadows_ours] our shadow passes (switch on): maps " + std::to_string(shadows_ours::MapSize()) + " px, plane mask " + std::to_string(g_shadow_plane_percent) + "% of the 360 size").c_str());  // S101 lane 123
  if (options.circles_ours) impl_->Log("[rdr][circles] the circle blob shadows are drawn from the scene record in place of the game's (each checked against the game's own constants; switch on)");  // lane 136
  if (options.plane_shadow_ours) impl_->Log("[rdr][plane_shadow] the plane shadow chain's constants (PlaneInv, camera, EyePos, palette; the blur's matrix) come from the scene record in place of the stream's, each checked against the game's own (switch on)");  // lane 139
  if (options.small_passes_ours) impl_->Log("[rdr][small_passes] the wrestlers' sweat map and skin pre-light passes are drawn with meshes of our own, each draw checked against the game's (switch on)");  // lane 135
  if (options.rim_chain_ours) impl_->Log("[rdr][rim_chain] the rim light map's coverage reductions and blur are drawn by our own programs on the game's draws (switch on)");  // lane 151
  if (options.rim_ours) impl_->Log("[rdr][rim_map] our caster geometry is drawn in the rim light map in place of the game's, from the game files (switch on)");  // lane 124
  if (options.particles_ours) impl_->Log("[rdr][particles] runs of particle draws are drawn as one draw each by our vertex stage (switch on)");  // S101 lane 67
  if (options.movie_ours) impl_->Log("[rdr][movie] the movie planes (titantron, menu backdrop) are drawn by our own quad and YUV shader from the planes the decoder wrote this frame (switch on)");  // S101 lane 131
  if (options.ui_ours) impl_->Log("[rdr][ui] the menus' and HUD's pictures are bound from our own uploads (switch on)");
  if (options.ui_quads) impl_->Log("[rdr][ui] the sprites and flat rectangles are drawn with our own quad and 2D program (switch on, lane 148)");   // S101 lane 148
  if (options.ui_text_ours) impl_->Log("[rdr][ui_text] the game's text is laid out by us, each glyph quad checked against the game's (switch on); the run-string labels are ON for the whole session for it (origin::g_labels_wanted)");  // S101 lane 89
  // S103 L1C review (kept): the hold mask is in place BEFORE any sink or the mode pointer exists, so a held bit can never stamp once in that window. S103 SPB: ApplyStartSettings() (above, before the renderer is
  // made) stores the presence switches and THEN the hold in one helper, so the window is closed there.
  // START-ONLY(ON): the stream's own keeper sink (the in-place replacers and the mirror need the records); the stream-free host attaches its keeper in host_start.cpp StartForGame
  if (options.crowd_ours || options.arena_ours || options.wrestlers_ours || options.ring_ours || options.flash_ours || options.props_ours || options.shadow_casters_ours || options.screens_ours || options.rim_ours || options.crowd_props_ours || options.circles_ours || options.plane_shadow_ours /* lane 139 */ || REXCVAR_GET(wwe2k14_rdr_scene) != 0 /* S103 lane S: mirror / ours need the records */) {  /* lane 136: circles_ours */  /* lane 133 */  // lane 124: rim_ours  // S101 lane 115: screens  // S101 lane 99: shadow casters  // S101 lane 84: props; S101 lane 35 / 41: records to the render thread
    impl_->keeper = std::make_unique<SnapshotKeeper>();
    rdr_scene::live::AddSink(impl_->keeper.get());
    if (options.crowd_ours) impl_->Log(options.crowd_cards ? "[rdr][crowd] the crowd is drawn by us from the scene record (record j-1 for frame j); the stream's block draws are dropped, and the far cards too: one instanced draw per person type with the game's own pictures (any card ours cannot account for stays the game's)"
                                   : "[rdr][crowd] the crowd is drawn by us from the scene record (record j-1 for frame j); the stream's block draws are dropped (the far cards stay the stream's)");
  }
  {   // S103 lane S: the dev-only input override (a release build, no "+dev" in its version, never sets it)
    // S103 SPB: every scene-input store of this block moved into ApplyStartSettings() (start_settings.cpp), which both start paths call; this block only reports what it applied. The dev bool is asked of
    // dev_build.cpp (S103 L33F review F1: this file never had the version define) ONCE there and read from `applied`.
    const bool dev = applied.dev;
    if (applied.force_inputs) impl_->Log("[scene][frame] DEV: inputs not required for OURS (--wwe2k14_rdr_scene_force_inputs)");
    // S103 L2a: the 2D from our rows (dev only: `dev` = wwe2k14::IsDevBuild(), dev_build.cpp)
    // START-ONLY(ON): the 2D taps are pinned on in the stream-free build (quads2d_tap.h `#if !WWE2K14_STREAM() const int v = 1`); this is the stream's own 2D-from-rows switch
    impl_->q2d_on = dev && REXCVAR_GET(wwe2k14_rdr_2d_ours) && REXCVAR_GET(wwe2k14_rdr_2d_record);
    if (impl_->q2d_on) impl_->Log("[scene][2d] DEV: on a 2D-whole OURS frame the 2D is drawn from our rows (--wwe2k14_rdr_2d_ours); census line [scene][2d] every 600 OURS frames");
    else if (REXCVAR_GET(wwe2k14_rdr_2d_ours)) impl_->Log(dev ? "[scene][2d] --wwe2k14_rdr_2d_ours ignored: it needs --wwe2k14_rdr_2d_record" : "[scene][2d] --wwe2k14_rdr_2d_ours ignored: a release build");
    // S103 S2: the sentinel walked frames (dev only, like the override), the changes census and the two presence holds
    {   // S103 L1C: the presence switches, then the run-time hold (a held bit 10 / 11 turns its presence rule off): applied by ApplyStartSettings (S103 HN B1: scene_start_switches.h), reported here
      const std::string& bad = applied.hold_bad;
      const std::string& hold_list = applied.hold_list;
      const uint32_t hold = applied.hold_mask;
      char l[320];
      std::snprintf(l, sizeof l, "[scene][frame] L1C: hold mask 0x%X (--wwe2k14_rdr_scene_hold=\"%s\"%s%s), light_states %s, caster_entries %s%s, crowd %s, particles %s, stage_clock %s", hold, hold_list.c_str(), bad.empty() ? "" : ", unknown tokens: ", bad.c_str(),
                    scene_inputs::Held(scene_inputs::kInLightStates) ? "HELD" : "on", scene_inputs::Held(scene_inputs::kInCasterEntries) ? "HELD" : "on", (!scene_inputs::Held(scene_inputs::kInCasterEntries) && scene_inputs::Held(scene_inputs::kInLightStates)) ? " (HELD via 4: its synthesis is off)" : "", scene_inputs::Held(scene_inputs::kInCrowdSnapshot) ? "HELD" : "on",
                    scene_inputs::Held(scene_inputs::kInParticles) ? "HELD" : "on", scene_inputs::Held(scene_inputs::kInStageClock) ? "HELD" : "on");
      impl_->Log(l);
    }
    {   // S103 L33F: the dev-only un-require (after the hold, same parser; applied by ApplyStartSettings). A release build ignores it (dev false: mask 0, logged only when set)
      const std::string& bad = applied.unrequire_bad;
      const std::string& list = applied.unrequire_list;
      if (!list.empty()) {
        char l[320];
        std::snprintf(l, sizeof l, "[scene][frame] L33F: %s unrequire mask 0x%X (--wwe2k14_rdr_scene_unrequire=\"%s\"%s%s), inputs req now 0x%08x", dev ? "DEV:" : "IGNORED (release build):", applied.unrequire_applied, list.c_str(), bad.empty() ? "" : ", unknown tokens: ", bad.c_str(),
                      rdr_own::S(rdr_own::Pass::Scene3D).required_inputs.load(std::memory_order_relaxed));   // lint:required-ok (a log of the registered mask)
        impl_->Log(l);
      }
    }
    // S103 LH: the old light-entry rule (dev only; a release build always refreshes)
    if (!applied.refresh_default) REXLOG_INFO("[scene][light] DEV: old hold rule (--wwe2k14_rdr_light_old_hold): default-slot light entries age and plan refresh walks");
    // S103 L2d: the flat frame kind (dev only, like the un-require; a release build ignores it)
    if (REXCVAR_GET(wwe2k14_rdr_scene_flat))
      impl_->Log(dev ? "[scene][kind] L2d: DEV: flat frames (no stage, no 3D) may be OURS when 2D + movie + particles are whole (--wwe2k14_rdr_scene_flat) (wired: SceneOurs decides flat frames; menu particles are not recorded yet, so they stay the game's)"
                     : "[scene][kind] L2d: --wwe2k14_rdr_scene_flat IGNORED (release build)");
    if (scene_frame::g_sentinel_every.load(std::memory_order_relaxed) || rdr_scene::live::inputchanges::g_on.load(std::memory_order_relaxed) ||
        rdr_scene::live::presence::g_particles.load(std::memory_order_relaxed) || rdr_scene::live::presence::g_crowd.load(std::memory_order_relaxed)) {
      char l[256];
      std::snprintf(l, sizeof l, "[scene][frame] S2: sentinel every %u, changes census %s, particles by presence %s, crowd by presence %s",
                    scene_frame::g_sentinel_every.load(std::memory_order_relaxed), rdr_scene::live::inputchanges::g_on.load(std::memory_order_relaxed) ? "on" : "off",
                    rdr_scene::live::presence::g_particles.load(std::memory_order_relaxed) ? "on" : "off", rdr_scene::live::presence::g_crowd.load(std::memory_order_relaxed) ? "on" : "off");
      impl_->Log(l);
    }
  }
  impl_->stopping = false;
  {
    std::lock_guard lock(impl_->pic_mu);
    impl_->pic_stop = false;  // (lane 302: a Start after a Stop gets a working picture writer)
  }
  impl_->thread = std::thread([this] { wwe2k14::NameThread(L"rdr_render"); impl_->Run(); });
  impl_->active = true;
  rex::RegisterQuitFlush(&impl_->quit_req, &impl_->quit_ack);  // S101 lane 216: the SDK's quit flush (before the hard exit) asks for the final census
  impl_->quit_registered = true;
  char line[256];
  std::snprintf(line, sizeof(line), "[rdr][live] started on %s (validation %d, %s)", impl_->renderer->device_name(), options.validation,
                options.window ? "OWNS THE WINDOW" : "headless mirror");
  impl_->Log(line);
  std::snprintf(line, sizeof(line), "[rdr][verdict] started on %s (the window is %s); the movie / ui verdicts follow every 600 frames", impl_->renderer->device_name(),
                options.window ? "ours" : "headless");  // S102 U1
  impl_->Log(line);
  return true;
}

void LiveSink::Stop() {
  origin::g_labels_wanted.store(false, std::memory_order_relaxed);  // S101 lane 89 (review F7)
  if (!impl_->active) return;
  if (impl_->quit_registered) {
    rex::UnregisterQuitFlush(&impl_->quit_req);
    impl_->quit_registered = false;
  }
  {
    std::lock_guard lock(impl_->mail_mu);
    impl_->stopping = true;
  }
  impl_->mail_cv.notify_all();
  {   // S103 lane SHOT (review F6): no caller waits for a render thread that is going
    std::lock_guard lock(impl_->shot_mu);
    impl_->shot_stop = true;
    impl_->shot_cv.notify_all();
  }
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->StopPictureWriter();  // lane 302: the queued pictures are written before the sink goes
  impl_->renderer->WaitIdle();
  if (impl_->scene_draw) {  // S103 lane S: the mirror drawer's census, and its images freed before the renderer goes
    impl_->Log(impl_->scene_draw->Census().c_str());
    impl_->Log(impl_->scene_draw->ReadyCensusLine(true).c_str());   // S103 NR: the last (partial) window
    impl_->scene_draw.reset();
  }
  impl_->Log(scene_frame::CensusLine().c_str());
  impl_->Log(scene_frame::missing_hist::MissingLine().c_str());   // S103 X1: the last (partial) window
  impl_->Log(scene_kind::CensusLine().c_str());   // S103 L2dF: the per-kind census, last (partial) window
  if (impl_->fc_handled) impl_->Log(fallback_census::g_census.FinalLine(impl_->fc_handled).c_str());   // S103 L2b: the exit line of the fallback census
  if (impl_->keeper) {  // S101 lane 35
    rdr_scene::live::RemoveSink(impl_->keeper.get());
    char crowd_line[400];
    std::snprintf(crowd_line, sizeof(crowd_line), "[rdr][records] frames submitted with no previous record: %llu, with no record of their own (a refused record): %llu; first parts (read-back splits) with a record %llu, without %llu", static_cast<unsigned long long>(impl_->keeper->no_previous), static_cast<unsigned long long>(impl_->keeper->wrong_frame), static_cast<unsigned long long>(impl_->keeper->part_records), static_cast<unsigned long long>(impl_->keeper->part_none));
    impl_->Log(crowd_line);
    impl_->keeper.reset();
  }
  const std::string stop_line = "[rdr][live] stopped: " + impl_->Report();   // lane 307: a std::string (the census is longer than the old 256 byte buffer)
  impl_->Log(stop_line.c_str());
  impl_->renderer.reset();
  impl_->active = false;
  wwe2k14::start::RearmStartSettings();   // S103 SPB review L3: a start after this stop (a capture, a second StartLive) reads the flags again
}

bool LiveSink::active() const { return impl_->active; }

uint64_t LiveSink::Append(Stream stream, std::vector<uint8_t>&& bytes) {
  const uint64_t key = impl_->next_key.fetch_add(1, std::memory_order_relaxed);  // lane 273: no lock for the key
  // S101 lane 11: the case ring copies the bytes OUTSIDE store_mu (the render thread takes it per draw). No frame names this key until Append
  // returns, so the key is safe to use before the store holds it.
  StoreTimer t(6);
  case_hook::OnContent(stream == Stream::Geometry, key, bytes.data(), bytes.size());
  t.Stop();  // [6] the case ring (the bug button's recorder: off for players)
  std::shared_ptr<const std::vector<uint8_t>> shared;
  t.holder = 5;
  if (stream == Stream::Geometry) shared = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));  // allocated before the lock
  t.Stop();  // [5] the allocation
  t.holder = 4;
  std::lock_guard lock(impl_->store_mu);
  t.Stop();  // [4] the wait for the lock
  t.holder = 1;
  if (stream == Stream::Geometry) impl_->geometry.emplace(key, std::move(shared));
  else impl_->textures.emplace(key, std::move(bytes));
  t.Stop();  // [1] the insert under the lock
  return key;
}

void LiveSink::DropGeometry(uint64_t key) {
  case_hook::DropGeometry(key);  // S101 lane 5
  std::shared_ptr<const std::vector<uint8_t>> freed;    // lane 273: the bytes are released after the lock, not under it (the game thread's Append waits on store_mu)
  {
    std::lock_guard lock(impl_->store_mu);
    struct Hold2 { StoreTimer t{2}; ~Hold2() { t.Stop(); } } hold2;
    if (auto it = impl_->geometry.find(key); it != impl_->geometry.end()) {
      freed = std::move(it->second);
      impl_->geometry.erase(it);
    }
  }
}

void LiveSink::SetImages(const ImageRecord* records, size_t count) {
  case_hook::SetImages(records, count);  // S101 lane 5
  impl_->images.assign(records, records + count);
  impl_->images_pending = true;
}

void LiveSink::ReleaseImage(uint32_t id) {
  case_hook::ReleaseImage(id);  // S101 lane 5
  impl_->released.push_back(id);
}

void LiveSink::SetHideHud(bool hide) { g_hide_hud.store(hide, std::memory_order_relaxed); }

bool LiveSink::SavePicture(const std::filesystem::path& path) {
  if (!impl_->active) return false;
  std::lock_guard lock(impl_->overlay_mu);
  if (!impl_->photo_path.empty() && impl_->photo_path != path)  // a request still waiting is replaced by this newer one (its own count)
    impl_->Log(("[rdr][live][photo] WARN " + photo::Utf8(impl_->photo_path) + ": replaced by a newer request before it was written").c_str());
  if (impl_->photo_path != path) impl_->photo_tries = 0;  // the same path again (a same-second double save) keeps the count
  impl_->photo_path = path;
  return true;
}

bool LiveSink::SaveLastPicture(const std::filesystem::path& path) {
  if (!impl_->active) return false;
  {
    std::lock_guard lock(impl_->mail_mu);
    impl_->last_picture_path = path;
  }
  impl_->mail_cv.notify_all();
  return true;
}

// S103 lane SHOT: the last presented picture as RGBA8 pixels, read back by the render thread on request only (src\shown_frame.h). Any thread; false = not running / nothing presented / timed out.
bool LiveSink::CaptureShown(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height, uint32_t timeout_ms) {
  if (!impl_->active) return false;
  std::shared_ptr<const Impl::ShotAnswer> ans;
  {
    std::unique_lock lock(impl_->shot_mu);
    if (impl_->shot_stop) return false;
    const uint64_t mine = ++impl_->shot_req;   // F4: only an answer that covers this sequence is this caller's
    impl_->shot_pending.store(true, std::memory_order_release);
    ++impl_->shot_waiters;
    lock.unlock();
    { std::lock_guard m(impl_->mail_mu); }   // (the render thread sleeps on mail_cv, up to 100 ms: wake it; mail_mu is taken so the wake cannot be lost)
    impl_->mail_cv.notify_all();
    lock.lock();
    const bool got = impl_->shot_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return impl_->shot_stop || (impl_->shot_ans && impl_->shot_ans->covers >= mine); });
    --impl_->shot_waiters;
    if (got && !impl_->shot_stop) ans = impl_->shot_ans;
    if (impl_->shot_waiters == 0) impl_->shot_ans.reset();
  }
  if (!ans || !ans->ok || ans->rgba.size() < size_t(ans->w) * ans->h * 4) return false;
  rgba = ans->rgba;
  width = ans->w, height = ans->h;
  if (ans->curve.size() == 1024) {   // the curve on the caller's thread
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
      for (uint32_t c = 0; c < 3; ++c) rgba[i + c] = ans->curve[4 * rgba[i + c] + c];
  }
  return true;
}

// S101 lane 216: asks the render thread for every class's final census line and waits (at most `timeout_ms`) until it is printed. Any thread; never blocks longer than that.
bool LiveSink::FlushFinalCensus(uint32_t timeout_ms) {
  if (!impl_->active) return false;
  impl_->quit_ack.store(false, std::memory_order_release);  // (SF1: cleared here, not by the render thread)
  impl_->quit_req.store(true, std::memory_order_release);
  impl_->mail_cv.notify_all();
  // render-clock-begin: ctl - shutdown wait deadline for the quit acknowledgement
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!impl_->quit_ack.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
  // render-clock-end
  return impl_->quit_ack.load(std::memory_order_acquire);
}

void LiveSink::SetOverlay(std::shared_ptr<const rdr::OverlayList> list) {
  std::lock_guard lock(impl_->overlay_mu);
  impl_->overlay_next = std::move(list);
  impl_->overlay_changed = true;
}

void LiveSink::SetOverlayTexture(uint32_t id, uint32_t width, uint32_t height, std::vector<uint8_t>&& rgba) {
  if (rgba.size() < size_t(width) * height * 4) return;
  std::lock_guard lock(impl_->overlay_mu);
  impl_->overlay_textures.push_back({id, width, height, std::move(rgba)});
}

void LiveSink::SetDisplayCurve(const uint8_t* rgba256) { impl_->display_curve.assign(rgba256, rgba256 + 1024); }

// Strip-360 A1 (checks only, --wwe2k14_rdr_pace_game): the game waits until fewer than `max_waiting` frames are queued, so an outrun
// fast-forward draws every frame and its pictures compare exactly; bounded by `timeout_ms` (never a hang).
// S101 lane 302 (run 7202): the wait is the swap thread's, which holds the det baton. A render thread that is stuck (a stalled disk under a
// picture write, an outside load) turned every frame into a full timeout = 1 fps for the whole game. After kPaceGiveUp timeouts in a row
// the pacing is switched off (one log line) until the render thread has caught up (fewer than max_waiting queued), then it resumes.
void LiveSink::WaitForRoom(size_t max_waiting, uint32_t timeout_ms) {
  constexpr int kPaceGiveUp = 3;
  std::unique_lock lock(impl_->mail_mu);
  impl_->paced = true;   // S103 G2
  if (impl_->pace_off) {
    if (impl_->mail.size() >= max_waiting && !impl_->stopping) {  // still behind: no waiting
      ++impl_->pace_skipped_frames;
      return;
    }
    impl_->pace_off = false, impl_->pace_timeouts = 0;
    const uint64_t skipped = impl_->pace_skipped_frames;
    lock.unlock();
    char line[160];
    std::snprintf(line, sizeof(line), "[rdr][live] pace_game: the render thread caught up, pacing resumes (%llu frames went unpaced)", static_cast<unsigned long long>(skipped));
    impl_->Log(line);
    return;
  }
  const bool room = impl_->room_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return impl_->stopping || impl_->mail.size() < max_waiting; });
  if (room) {
    impl_->pace_timeouts = 0;
    return;
  }
  if (++impl_->pace_timeouts >= kPaceGiveUp) {
    impl_->pace_off = true;
    ++impl_->pace_give_ups;
    impl_->pace_skipped_frames = 0;
    lock.unlock();
    char line[200];
    std::snprintf(line, sizeof(line), "[rdr][live] WARN pace_game: the render thread did not make room for %d frames in a row (%u ms each); pacing is off until it catches up (episode %u)",
                  kPaceGiveUp, timeout_ms, impl_->pace_give_ups);
    impl_->Log(line);
  }
}

void LiveSink::SubmitFrame(std::vector<uint8_t>&& frame, uint64_t guest_frame, const std::string& picture) {
  case_hook::OnFrame(frame.data(), frame.size(), guest_frame);  // S101 lane 5
  Message message;
  message.frame = std::move(frame);
  message.guest_frame = guest_frame;
  message.seq = ++impl_->submit_seq;
  message.picture = picture;
  if (impl_->images_pending) {
    message.has_images = true;
    message.images = impl_->images;
    impl_->images_pending = false;
  }
  message.released.swap(impl_->released);
  message.display_curve.swap(impl_->display_curve);
  message.d3_chain = skips_auto::ChainFor(guest_frame);   // S102 K3 rework (review F2: + any frame that carries a marker)
  if (impl_->keeper) message.d3_record = d3::AnyD3Chain() && message.d3_chain ? impl_->keeper->ForD3(guest_frame) : nullptr;   // lane 152 (B1): taken BEFORE ForSubmit (it does not move `cur`)
  impl_->NoteScene(message, guest_frame);   // S103 lane S: the present's own record, taken before ForSubmit (lane 152's order)
  if (impl_->keeper) message.record = impl_->keeper->ForSubmit(guest_frame);  // S101 lane 35
  {
    std::lock_guard lock(impl_->mail_mu);
    impl_->mail.push_back(std::move(message));
  }
  impl_->mail_cv.notify_one();
}

bool LiveSink::FrameBytesNeeded() { return case_hook::Enabled(); }

// S102 lane T8: SubmitFrame without the bytes (the case ring is off and no capture runs: the reader checked).
void LiveSink::SubmitFrameData(std::shared_ptr<handoff::FrameData> frame, uint64_t guest_frame, const std::string& picture) {
  Message message;
  message.data = std::move(frame);
  message.guest_frame = guest_frame;
  message.seq = ++impl_->submit_seq;
  message.picture = picture;
  if (impl_->images_pending) {
    message.has_images = true;
    message.images = impl_->images;
    impl_->images_pending = false;
  }
  message.released.swap(impl_->released);
  message.display_curve.swap(impl_->display_curve);
  message.d3_chain = skips_auto::ChainFor(guest_frame);   // S102 K3 rework (review F2: + any frame that carries a marker)
  if (impl_->keeper) message.d3_record = d3::AnyD3Chain() && message.d3_chain ? impl_->keeper->ForD3(guest_frame) : nullptr;
  impl_->NoteScene(message, guest_frame);   // S103 lane S
  if (impl_->keeper) message.record = impl_->keeper->ForSubmit(guest_frame);
  {
    std::lock_guard lock(impl_->mail_mu);
    impl_->mail.push_back(std::move(message));
  }
  impl_->mail_cv.notify_one();
}

bool LiveSink::DrawAndRead(std::vector<uint8_t>&& frame, uint64_t guest_frame, uint32_t image, uint32_t mip, uint32_t layer, std::vector<uint8_t>& rgba,
                           uint32_t& width, uint32_t& height, uint32_t timeout_ms, ReadTimes* times) {
  if (!impl_->active) return false;
  case_hook::OnFrame(frame.data(), frame.size(), guest_frame, true);  // S101 lane 5/11: the part before a read-back copy is a frame file too (flagged kFramePart)
  auto request = std::make_shared<ReadRequest>();
  request->image = image;
  request->mip = mip;
  request->layer = layer;
  request->posted = std::chrono::steady_clock::now();
  Message message;
  message.frame = std::move(frame);
  message.guest_frame = guest_frame;
  message.read = request;
  message.scene_mode = REXCVAR_GET(wwe2k14_rdr_scene);   // S103 L2b (review F8): the part carries the frame's mode as every other message does (the part itself is not a census frame: the second part is)
  message.d3_chain = skips_auto::ChainFor(guest_frame);   // S102 K3 review F6: a read-back part is a Message too (its default was "chains on")
  if (impl_->keeper) message.record = impl_->keeper->ForPart(guest_frame);   // lane 215: the first part's draws need the record the second part will get (circles / plane were blind for the whole frame)
  if (impl_->images_pending) {  // the part may name a new image; releases and the display table wait for the frame's end
    message.has_images = true;
    message.images = impl_->images;
    impl_->images_pending = false;
  }
  {
    std::lock_guard lock(impl_->mail_mu);
    if (impl_->stopping) return false;
    impl_->mail.push_back(std::move(message));
  }
  impl_->mail_cv.notify_one();
  std::unique_lock lock(request->mu);
  if (!request->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return request->done; })) {
    impl_->Log("[rdr][live] WARN a read-back copy was not drawn in time: the game reads what its memory held");
    return false;
  }
  if (times) {
    const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    times->wait_ms = ms(request->posted, request->taken), times->draw_ms = ms(request->taken, request->drawn), times->read_ms = ms(request->drawn, request->read);
  }
  if (!request->ok) return false;
  rgba = std::move(request->rgba);
  width = request->width;
  height = request->height;
  return true;
}

AsyncReadback& LiveSink::Async() { return impl_->async; }

uint64_t LiveSink::SubmittedCount() const { return impl_->submit_seq.load(std::memory_order_relaxed); }

std::string LiveSink::Report() const { return impl_->Report(); }

}  // namespace rdr_adapter
