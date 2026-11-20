// S103 landing 2, piece P7 H1 (docs\renderer\P7_DESIGN.md sections 1-3): the scene host, our own render thread and present for a build with no command stream
// (WWE2K14_KEEP_STREAM=OFF). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// One host thread (`rdr_host`) is the only caller of Renderer::RenderFrame. Each present of the game posts one HostMsg (the frame_tick Tick, the present's OWN scene record, the size and the
// display curve; host_start.cpp is the poster and the game-side start); the mailbox keeps the newest (host_core.h). Per message the host draws our 3D scene + particles + post from the record
// (LiveSceneDraw::DrawLive), copies the picture into ONE final image F and presents F through rdr::Renderer. A frame it cannot draw shows the last 3D picture (or F as it was), never black
// except where there is nothing yet, and never the stream (host_core.h Decide, P7_DESIGN.md 3.2). The 2D rows of the record onto F are H2: this file leaves the hook point (DrawRows) and counts
// the rows it does not draw.
//
// This header and scene_host.cpp include no REX / SDK / game header, so the headless rig (tools\scene\host_rig.cpp) builds the SAME host. Everything that needs the game (cvars, the window, the
// frame_tick poster, the hang-guard beat) is in host_start.cpp and reaches the host through HostOptions callbacks.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../renderer/frame_blend.h"
#include "../renderer/renderer.h"
#include "../renderer_scene/live_scene_draw.h"
#include "../renderer_scene/quads2d_draw.h"
#include "../renderer_scene/stage_census.h"   // S103 CPU1
#include "../scene_live/movie_planes.h"
#include "host_core.h"
#include "scene_rules.h"   // S103 RBC fix2: the scene read-back rules (freshness, shot priority, retries)
#include "host_planes.h"
#include "portrait_mailbox.h"   // S103 CPB
#include "../renderer_game/scene_kind.h"   // S105 A4: WalkGate
#include "refused_row_note.h"   // S105 A4 piece 3
#include "../renderer_game/caw_portrait_core.h"

namespace wwe2k14::scene_host {

using Rec = rdr_scene::SceneTables;
using Msg = HostMsg<Rec>;

struct HostOptions {
  rdr::RendererOptions renderer;                  // window handles (the owner window) or present_offscreen_w / h (the rig)
  std::string game_root;                          // the folder LiveSceneDraw loads models from (re-read each frame from game_root_fn when set)
  std::function<std::string()> game_root_fn;
  void (*log)(const char*) = nullptr;             // null: stderr
  std::function<void()> on_thread_start;          // names the host thread
  std::function<void()> on_present;               // once per frame that reached the swapchain / offscreen present (the hang guard's beat, the frame counters)
  // S103 P7 H2: the picture of a sprite / glyph row (P1 / P3's live resolver: the key -> OUR image; null image = no picture). Called on the host thread only. Null function = no resolver yet: every
  // textured row is dropped `picture`, counted, never drawn with a guess.
  std::function<rdr::TextureSlot(const rdr_scene::Quad2D&)> picture;
  // S103 PN4: called once per handled message that carried a record, AFTER its rows were drawn (host thread), with that record's guest frame (the SAME counter the picture source stamps deaths with:
  // frame_tick's guest_frame = wwe2k14::g_guest_frame as the present reads it). The picture resolver's EndFrame (trim the dead keys, release their images).
  std::function<void(uint64_t)> frame_done;
  // S103 PN4F (B1): called once by SceneHost::Stop, on the stopping thread, AFTER the host thread is stopped and the GPU idle, the final lines (the report) are printed, and BEFORE the renderer is destroyed: whatever holds the
  // renderer's images (the picture resolver) goes here, so no static destructor touches a dead renderer at exit.
  std::function<void()> on_stop;
  std::function<void(uint64_t, uint64_t)> contributor_census;   // S103 LVF: the scene writer's census over guest frames [from, to] (rdr_scene::live::CensusLog), every 600 frames and at exit; null in the rig (it links no writer)
  std::function<std::string()> refusal_report;   // BHF4 (COV7): the picture refusals (counts by reason + the first key's creator site), printed every 600 frames while they happen; empty = none new
  std::function<std::string()> report;   // S103 PN4: the picture resolver's line (and the source's counters), logged with the exit census; empty string = nothing to say
  // S103 P7 H2 (review (c)): false (the game) = a row whose pass the tap does not know (`Unknown`, every row today) is drawn over F ONLY when it is pixel-placed with a PosScale of (2 / w, 2 / h)
  // of F (a row of the final 2D layer); any other Unknown row is dropped `pass_unknown`, counted, never drawn on a guess. true (the offline rig only) = every Unknown row is drawn, as the stream's
  // own 2D arm draws them. TODO(lane SC): Quad2D layout 4's target class (kQhTarget: the image the draw went to) says "final image" directly; use it here once it lands.
  bool draw_unknown_pass = false;
  // The decoupled 120 (the tween): the player asked for it (step_hz > 60). Built: the own draws carry blend keys (rdr_scene::TagOwnBlend), the host keeps the previous tick's built Frame (rdr::TickBlender) and
  // presents the halfway picture (Blend at kTweenAlpha, a full frame) before the tick's own (DisplayOnly), FIFO spacing them at 120 Hz. Gate: host_core.h TweenGate. docs\renderer\P7_DESIGN.md "Tween: what the host needs".
  bool tween_wanted = false;
  // S105 A2: the screen-shape factors (ultrawide::Configure -> Applied::wide / tall; 1 = the game's shape). During play (Msg::fill_now) the scene and F are round(1280 x wide) x 720 or 1280 x round(720 x tall)
  // (wide_frame.h); the 2D rows stay a centred 1280x720. A rig sets them to test.
  float wide = 1.f, tall = 1.f;
  // The decoupled 30 (settings parity S9, the launcher's 30 fps mode; reader.h 2746-2751 / live.cpp 916-931 for the stream path): the game keeps its 60 updates and the host draws one picture per this many vblanks
  // of presents (2); the ticks between are not drawn (host_core.h VblankDebt). 0 / 1 = off. A screenshot request makes a tick draw.
  uint32_t draw_vblanks = 0;
  // S103 SCS: draw the in-scene 2D rows (screen flashes, lens flares) inside the scene pass (the game's look). false (--wwe2k14_rdr_scene2d=false) = the old drop: those rows are refused `pass_scene_colour`.
  bool scene2d = true;
  // S105 B3: the precompile boot (--wwe2k14_precompile): the pipeline store builds every listed pipeline at once, in any scene.
  bool precompile_mode = false;
  // S103 CHS C6 (the 360 habit: the 360 resolved the 3D picture into a frame buffer and then drew the HUD onto it; a frame with no 2D row has nothing to draw there): a frame whose record holds no row the host
  // would draw presents the 3D picture (`out`) itself, with no full-size copy into F. F is then stale: a later held frame presents the same `out` again, a shot reads it, a fault shows black (never a stale image).
  // Off with the tween (it needs F), with a render scale other than 1 (F is scaled, `out` is not) and while a plant says so. wwe2k14_rdr_present_direct (default true); the rig sets it with --present-direct.
  bool present_direct = true;   // the cvar default (wwe2k14_rdr_present_direct true): a HostOptions made by hand (a rig, a unit) is the game's default; tools/scene/chs_lint.py checks the two agree
};

// S103 CHS C6 (review fix 5): why present_direct is not active although it was asked for ("" = active, or not asked for). One rule, read by the frame code, the [start] line and the census line.
inline const char* DirectWhyNot(const HostOptions& o) {
  if (!o.present_direct) return "";
  if (o.renderer.render_scale != 1.0f) return "render scale is not 1 (F is scaled, `out` is not)";
  if (o.tween_wanted) return "the decoupled 120 Hz tween blends F";
  return "";
}

// Test / rig seam: NEVER set by the game (a lint keeps it out of src\ outside this folder). force_ours = decide OURS and skip the freshness check for every frame with a record.
struct TestSeam {
  bool force_ours = false;
  bool wide_box_origin = false;     // planted fault (S105 A2 rig): the 2D box stays at the frame's origin (the HUD hugs the left on a wide frame)
  bool wide_pass_f_size = false;    // planted fault (S105 A2 rig): the 2D drawing gets F's size, not the game's (FateOf's pixel-placed test fails on a wide F: the HUD is dropped)
  uint32_t inject_stale = 0;   // ONF rig: input bits the record counts as stale (a missing input); the OFF host draws the frame anyway and counts them
  bool stale_walks = false;    // planted fault (ONF rig, OFF only): a stale input turns the frame into a miss (the old rule)
  bool skip_copy = false;   // planted fault (the rig): F is presented without the copy of `out`
  int direct_plant = 0;     // (RIG-ONLY, see below)     // S103 CHS C6 planted faults (the rig; each must FAIL): 1 = present `out` directly even when a row needs F (the HUD is lost), 2 = the direct present shows the stale F instead of `out` (no copy was made: the picture is lost), 3 = a shot reads F while it is stale, 4 = a fault always blacks a stale F (the rule before the review fix: out_ok_ ignored)
  bool rows_first = false;  // planted fault (H2 rig): the 2D rows are drawn BEFORE the copy of the 3D picture (the order twin: the copy covers them)
  bool rows_never = false;  // planted fault (H2 rig): no 2D step runs (the rows twin)
  bool cam_ignore = false;     // planted fault (S105 A3 rig): the record's camera-gate bits are not read (the host draws the 3D on a frame the game draws no scene for); the A3 rig check must FAIL on it
  bool stage_plant = false;   // planted fault (S103 SVO rig): a stage-stamped record still draws its 2D rows (the stamp is ignored); the SVO check must FAIL on it
  bool overlay_never = false;  // planted fault (S103 AIM rig): the overlay mailbox is never emptied onto the renderer (the dialogs stay off the picture)
  // H2 rig: the planes of a movie row, instead of the P4 store (the rig has no guest memory to run the plane flush on). Null = the real path (movieplanes::Store + ServeGuest).
  std::function<rdr::TextureSlot(const rdr_scene::Quad2D& row, int slot)> plane;
  std::function<bool(const rdr_scene::Quad2D&, rdr::Rect&)> scissor;   // H2 rig: the game's scissor of a row's draw (the rows carry none yet; q2d::Context::scissor)
  std::function<rdr::TextureSlot(const rdr_scene::Quad2D&)> picture;   // H2 rig: replaces HostOptions::picture for one run (the rig builds its resolver after the host started)
  // Tween rig (tools\scene\host_rig.cpp --tween): called on the host thread for every drawn 3D tick: the tick's built Frame, and, when a halfway picture was made, the halfway Frame (else null) and the draws mixed. For a
  // halfway picture it runs right after that frame was rendered, before the tick's own redraw: the rig reads the presented halfway picture there (SceneHost::ReadPresent). Planted faults (each must make the rig FAIL): tween_alpha 0 = the halfway is the previous tick (no motion); tween_no_forget = the previous
  // tick survives a camera cut / scene change; tween_swap = the tick's own picture is presented BEFORE the halfway one (the viewer ends on the in-between).
  std::function<void(const rdr::Frame& built, const rdr::Frame* half, uint32_t matched)> on_tick;
  float tween_alpha = kTweenAlpha;
  bool tween_no_forget = false;
  bool tween_swap = false;
  bool picture_requested = false;   // the tween gate's "a picture was asked for" (the rig: a halfway picture is wanted even with a newer tick waiting)
  bool shot_no_curve = false;   // planted fault (SHOT rig): the capture is handed over without the display curve
  bool shot_late = false;       // planted fault (SHOT rig): a capture gets the PREVIOUS answer's picture (one frame late)
};

// The plane images' sink (movie_planes.h PlaneImages): one 8-bit channel images in the renderer.
struct PlaneSink {
  rdr::Renderer* renderer = nullptr;
  uint32_t Create(uint32_t w, uint32_t h) const {
    rdr::ImageDesc d;
    d.width = uint16_t(w), d.height = uint16_t(h), d.format = rdr::Format::R8, d.usage = rdr::kUsageSampled;
    return uint32_t(renderer->CreateImage(d));
  }
  void Update(uint32_t id, const uint8_t* data, size_t bytes) const { renderer->UpdateImage(rdr::ImageId(id), 0, 0, data, bytes); }
  void Destroy(uint32_t id) const { renderer->DestroyImage(rdr::ImageId(id)); }
};

class SceneHost {
 public:
  SceneHost();
  ~SceneHost();
  SceneHost(const SceneHost&) = delete;
  SceneHost& operator=(const SceneHost&) = delete;

  // Makes the renderer and the drawer and starts the host thread. False + error: the renderer could not be made.
  bool Start(const HostOptions& options, std::string& error);
  // Stops the thread (the message in hand is finished), waits for the GPU, saves the pipeline store, prints the final [host] line. Safe twice.
  void Stop();
  bool Running() const { return core_.Running(); }

  // The poster's entry (the game's present thread): never draws, never waits.
  void Post(Msg&& m) { core_.Post(std::move(m)); }
  // Rig: wait until `n` messages were handled.
  bool WaitHandled(uint64_t n, std::chrono::milliseconds timeout) { return core_.WaitHandled(n, timeout); }
  Counters& counters() { return core_.counters(); }
  void SetTestSeam(const TestSeam& s) { seam_ = s; }

  // Rig / checks: the last picture the present showed (offscreen renderer only); the source and reason of the last handled frame.
  bool ReadPresent(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
  Source LastSource() const { return Source(last_source_.load(std::memory_order_acquire)); }
  Reason LastReason() const { return Reason(last_reason_.load(std::memory_order_acquire)); }
  std::string LastWhy() const;
  rdr::Renderer* renderer() { return renderer_.get(); }

  // S103 lane SHOT (src\shown_frame.h): the picture the host last presented, as RGBA8 (F through the display table: what the player sees, at F's size), for the screenshot command, photo mode, clips.
  // Any thread, any number at once. ON REQUEST ONLY and WITHOUT A DRAIN (review F1): a request bumps a sequence number; the host thread queues ONE Renderer::QueueReadback of F into the command buffer of the
  // frame it is about to render (so the copy is that present's own picture) and hands the finished copy (its fence signalled) to every caller whose sequence it covers; the curve and the RGBA8 copy
  // are done on the CALLER's thread. When no frame comes within the host's 100 ms idle poll the one synchronous read (a drain) serves it. A late answer never reaches a later caller (F4).
  // `guest_frame` (optional): the guest frame whose present the picture is (0 = the idle read). False: nothing presented yet, the read failed, stopped, or `timeout_ms` passed.
  bool CaptureShown(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height, uint32_t timeout_ms = 2000, uint64_t* guest_frame = nullptr);

  // S103 RBC: the game's GPU read-backs (replay-clip thumbnail, Create an Arena screenshot) want the SCENE, not the screen: `out` of the newest OURS 3D frame, raw RGBA8 (no 2D rows, no HUD, and NO display curve:
  // the game's own pipeline applies its ramp after the CPU reads the pixels, so a curved picture would get the ramp twice). Not ok = no OURS 3D frame yet (a menu, a loading screen, a faulted draw) or the read
  // could not be made: the caller counts it unserved, it never gets the menu. RequestScene: any thread, NEVER blocks; `cb` runs ONCE on the host thread (keep it short: copy the bytes and return) with the picture of
  // the next frame the host renders (the readback rides that frame's command buffer, no drain) or, when no frame comes within the host's idle poll, one synchronous read. CaptureScene: the blocking form (a menu save).
  using SceneCb = std::function<void(bool ok, std::vector<uint8_t>&& rgba, uint32_t w, uint32_t h, uint64_t guest_frame)>;
  bool RequestScene(SceneCb cb);
  bool CaptureScene(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height, uint32_t timeout_ms = 500, uint64_t* guest_frame = nullptr);

  // The SDK's quit flush (rex::RegisterQuitFlush, host_start.cpp): the app leaves with a hard exit, so Stop() never runs. The SDK sets the request flag; the host thread sees it within ~100 ms (HostCore's
  // idle poll), saves the pipeline store, prints the [host] and [scene][2d] exit lines and sets the ack.
  std::atomic<bool>* QuitRequest() { return &quit_req_; }
  std::atomic<bool>* QuitAck() { return &quit_ack_; }

  // H2: photo mode. While set, the 2D rows are not drawn (counted `hud_hidden`); the 3D picture is untouched. The owner (the photo mode / SetHideHud) calls it from any thread.
  void SetHideHud(bool hide) { hide_hud_.store(hide, std::memory_order_relaxed); }
  // S103 CHS (rig only, tools\scene\host_rig.cpp --fault-after): the next OURS draw is not made and reads as a draw: fault. kind 1 = the front end recorded nothing into `out` (the real case: every fault path returns
  // before the combine writes the post image), kind 2 = it put ops on `out` (a no-post picture: `out` is spoiled). 0 = off. Consumed by the next frame that would have drawn.
  void PlantFaultNext(int kind) { fault_plant_.store(kind, std::memory_order_relaxed); }
  uint64_t DirectCopies() const { return direct_copies_; }   // S103 CHS: full-size out -> F copies made (read after WaitHandled; the GT role check reads it)
  // S105 A4 (host thread, called by the 2D picture resolver while DrawRows runs): the 3D picture of the frame being presented, for a row that draws the game's scene-colour copy back as a sprite. kNoImage unless this
  // frame presents a 3D picture (Draw3D / Hold3D) - a Black / HoldF frame has none, so the row is dropped, never a stale image.
  // Review F6: only a FINAL-pass row, and not a redundant opaque full-screen one (refused_row::ServeDecision); the refusals are counted (`scene_copy_not_final`, `scene_copy_redundant` on the [host] line).
  rdr::ImageId ScenePicture(const rdr_scene::Quad2D& row) {
    const refused_row::Serve s = refused_row::ServeDecision(rows_now_, n_now_, row, scene_serve_ && last_out_ != rdr::kNoImage);
    if (s == refused_row::Serve::NotFinal) ++core_.counters().scene_copy_not_final;
    else if (s == refused_row::Serve::Redundant) ++core_.counters().scene_copy_redundant;
    return s == refused_row::Serve::Yes ? last_out_ : rdr::kNoImage;
  }

 private:
  void Handle(Msg& m);
  void PollQuit();         // host thread: the quit flush's request flag
  void LogFinal();         // the exit lines, once
  void LogDirect() const;  // S103 CHS C6: the present_direct census line
  void LogPostLevers(uint64_t frames) const;   // S103 SLV: the post levers' census lines
  void MakeF(uint32_t w, uint32_t h);
  // lane SHOT (host thread): QueueShot = the readback joins the frame about to be rendered; PollShot = a finished one is published; ServeShotSync = the drain, only when no frame is coming
  void QueueShot(uint64_t guest_frame);
  void PollShot();
  void ServeShotSync();
  // S103 RBC (host thread): the scene read-backs (see RequestScene). Polled by PollShot (one PollReadbacks drain serves both).
  void QueueScene(uint64_t guest_frame);
  void ServeSceneSync();
  void FinishScene(bool ok, std::vector<uint8_t>&& rgba, uint32_t w, uint32_t h, uint64_t frame);
  // S103 RBC fix2 (F1): a scene copy is only taken from `out` while it is the picture of a RECENT OURS 3D frame: the state says `out` is complete (st_.last3d_valid) and it was drawn within 2 guest frames of `frame`.
  // A menu, a loading screen or a replay the game draws itself (no OURS frame for a while) has no scene: the callers are answered not-ok, never the last match's arena.
  bool SceneFresh(uint64_t frame) const { return scene_rules::Fresh(out_ok_, st_.last3d_valid, last_out_ != rdr::kNoImage, frame, last_out_frame_); }
  bool PollDue() const { return shot_inflight_.load(std::memory_order_relaxed) || scene_inflight_; }
  void PublishShot(uint64_t covers, bool ok, std::vector<uint8_t>&& rgba, uint32_t w, uint32_t h, uint64_t frame);
  void Log(const std::string& line) const;
  void ServePortrait(const std::shared_ptr<const Rec>& cur, uint64_t j);   // S103 CPB (CPR.md H1): the pending Create a Superstar portrait request, drawn offscreen before this frame's builder begins
  uint64_t portrait_served_ = 0, portrait_failed_ = 0;   // host thread only
  caw_portrait::RetryGate portrait_retry_;
  std::shared_ptr<const Rec> editor_rec_;   // CPB: the newest record with guest-model rows
  uint64_t editor_rec_frame_ = 0;
  void ClearF(rdr::FrameBuilder& fb);
  // H2: the 2D rows of `rec` onto F (targets set here), in seq order. A row that cannot be drawn is dropped alone and counted by reason (R8); the census notes are the stream path's (live.cpp Draw2DOurs).
  void DrawRows(rdr::FrameBuilder& fb, const Rec& rec, uint64_t j, uint32_t w, uint32_t h, bool hidden, bool forbid_f, bool scene_on_f = false);   // S105 A4B: scene_on_f = a flat (TwoDOnly) frame: its scene-colour rows are drawn on F in seq order
  // S103 CHS C6: true when DrawRows would put a pass on F for this record (conservative: any row the host would hand to the drawer). `hidden` = the photo-mode flag, read ONCE per frame by the caller.
  bool RowsMayDraw(const Rec& rec, uint32_t w, uint32_t h, bool hidden, bool scene_on_f = false) const;   // S105 A4B: the same flag DrawRows gets (the two must agree)
  rdr::ImageId Shown() const;   // S103 CHS C6: the image that holds the picture on screen now (F, or `out` while F is stale)
  bool StageDrops(const Rec& rec) const { return (rec.header.flags & rdr_scene::kSceneStagePlaying) != 0 && !seam_.stage_plant; }   // S103 SVO: a stage push plays (the record says so): the 2D rows stay off the picture
  void NoteStageRows(const Rec& rec, uint64_t j);   // S103 SVO: counts the rows left out, logs once
  rdr::TextureSlot PlaneOf(const rdr_scene::Quad2D& row, int slot, uint64_t j);

  HostOptions opts_;
  std::unique_ptr<rdr::Renderer> renderer_;
  std::unique_ptr<rdr_scene::LiveSceneDraw> draw_;   // destroyed before the renderer
  HostCore<Rec> core_;
  TestSeam seam_;
  rdr::FrameBuilder builder_;
  // render-thread state
  State st_;
  rdr::ImageId f_ = rdr::kNoImage;      // the final image F
  rdr::ImageId last_out_ = rdr::kNoImage;   // the last good `out` (valid while st_.last3d_valid)
  std::atomic<int> fault_plant_{0};         // S103 CHS: PlantFaultNext
  bool out_ok_ = false;                     // S103 CHS: `last_out_` still holds the picture it was drawn with (set by a drawn frame; cleared by a size change and by a fault that put ops on it). Separate from st_.last3d_valid, which a fault clears
  const rdr_scene::Quad2D* rows_now_ = nullptr;   // S105 A4 F6/F8: the rows of the record DrawRows is drawing (null outside it), and the seq of the first row the picture resolver refused in this handled frame
  size_t n_now_ = 0;
  uint32_t first_pic_seq_ = 0;
  bool first_pic_set_ = false;
  std::function<rdr::TextureSlot(const rdr_scene::Quad2D&)> TrackPictures(std::function<rdr::TextureSlot(const rdr_scene::Quad2D&)> r);   // wraps the resolver: remembers the first row it refuses (F8)
  scene_kind::WalkGate walk_gate_;         // S105 A4 F2: consecutive records with no walk bit (host thread only)
  refused_row::Gate refused_gate_;         // S105 A4 piece 3: the first-refused-row note, once per mode entry (host thread only)
  bool scene_serve_ = false;              // S105 A4: this frame's `last_out_` is the picture presented (Draw3D / Hold3D); set in the present switch before DrawRows, reset at the start of every present (host thread only)
  bool f_stale_ = false;                    // S103 CHS C6: the picture on screen is `last_out_`, F was not copied (host thread only)
  uint64_t direct_presents_ = 0, direct_holds_ = 0, direct_copies_ = 0, direct_row_frames_ = 0, direct_fault_black_ = 0, direct_conflicts_ = 0;   // S103 CHS C6 census (cumulative; one [host][direct] line with the 600-frame census)
  uint64_t stage_rows_dropped_ = 0, stage_frames_ = 0;   // S103 SVO (host thread): rows / frames left out because the record carried kSceneStagePlaying
  uint32_t out_w_ = 0, out_h_ = 0;
  uint32_t game_w_ = 0, game_h_ = 0;   // S105 A2: the game's frame (the 2D rows' pixel space); out_w_ / out_h_ is the scene / F size (wider or taller during play on a screen of another shape)
  int32_t box_x_ = 0, box_y_ = 0;      // S105 A2: the centred game box's origin inside F
  uint32_t shape_w_ = 0, shape_h_ = 0; // S105 A2: this frame's scene / F size (set when a message is handled, before the scene draws: out_w_ / out_h_ follow at MakeF)
  // S103 LVF: every strike counted by its reason (the WARN lines are rate-limited) and by its `why` text (e.g. "the record has no camera"; at most kStrikeWhys distinct, the rest "other").
  static constexpr size_t kStrikeWhys = 12;
  uint64_t strike_by_reason_[16] = {};
  std::vector<std::pair<std::string, uint64_t>> strike_whys_;
  void NoteStrike(uint8_t reason, const std::string& why);
  std::string StrikeLine() const;   // "[host][strikes] N: reason a, reason b ...; why ..." ("" when none)
  rdr_scene::StageWindow cpu_from_;       // S103 CPU1: the [host][cpu] census window's start (stage_census.h)
  uint64_t cpu_presented_from_ = 0;
  int64_t last_early_census_ms_ = 0;   // PBS: the 5 s wall-clock census (a phone at 66 presents a second needs ~9 s for the first 600-frame one)
  uint64_t strike_lines_ = 0, last_census_ = 0, census_from_ = 0;   // census_from_: S103 LVF, the start of the contributor census window
  std::string last_why_;
  mutable std::mutex why_mu_;
  std::atomic<uint8_t> last_source_{uint8_t(Source::Black)}, last_reason_{0};
  bool stopped_ = false;
  // H2
  std::atomic<bool> hide_hud_{false};
  rdr_scene::q2d::Programs q2d_programs_;
  bool q2d_registered_ = false;
  // S103 SCS: the scene hook's last report (the host thread writes it while DrawLive records, DrawRows reads it after): the guest frame it drew, the rows it was given, the drawer's report
  rdr_scene::q2d::Report scene2d_rep_;
  uint64_t scene2d_frame_ = uint64_t(-1), scene2d_rows_ = 0, scene2d_early_ = 0;   // (scene2d_early_: the early rows of that report, counted when it is consumed)
  std::unique_ptr<rdr_scene::live::movieplanes::PlaneImages<PlaneSink>> plane_images_;   // destroyed before the renderer
  // The tween (host thread only): the previous drawn tick's keys / ranges / constants, its camera, the scene class and the size it was drawn at.
  rdr::TickBlender tween_;
  TweenCam tween_cam_;
  uint8_t tween_kind_ = 0;
  uint32_t tween_w_ = 0, tween_h_ = 0;
  std::unique_ptr<HostPlanes<PlaneSink>> planes_;   // the row cache + the R9 repeat (host_planes.h); destroyed before the images it asks
  std::atomic<bool> quit_req_{false}, quit_ack_{false};
  // lane SHOT: the on-request readback. shot_mu_ guards everything but the host-thread-only fields; shot_active_ (a request not yet queued, or a copy in flight) is the one per-frame flag the host thread loads.
  struct ShotAnswer {
    uint64_t covers = 0;   // every request with a sequence <= covers is answered by this picture
    uint64_t frame = 0;
    bool ok = false;
    std::vector<uint8_t> rgba;   // RGBA8 as read (no curve yet)
    uint32_t w = 0, h = 0;
    std::array<uint8_t, 1024> curve{};
    bool curve_set = false;
  };
  std::mutex shot_mu_;
  std::condition_variable shot_cv_;
  std::atomic<bool> shot_active_{false};
  VblankDebt rate_;   // the decoupled 30: host thread only
  bool ShotUnqueued() {   // a screenshot request not yet queued (shot_req_ > shot_covered_): only then must a tick draw
    std::lock_guard<std::mutex> lock(shot_mu_);
    return shot_req_ > shot_covered_;
  }
  uint64_t shot_req_ = 0;                          // the newest request's sequence (shot_mu_)
  uint64_t shot_covered_ = 0;                      // requests up to this were queued / answered (shot_mu_)
  int shot_waiters_ = 0;
  bool shot_stop_ = false;
  std::shared_ptr<const ShotAnswer> shot_ans_;     // the newest answer (shot_mu_); dropped when no caller waits
  std::shared_ptr<const ShotAnswer> shot_prev_;    // planted fault shot_late only
  // host thread only: the copy in flight
  std::atomic<bool> shot_inflight_{false};
  uint64_t shot_tag_ = 0, shot_inflight_covers_ = 0, shot_inflight_frame_ = 0;
  std::array<uint8_t, 1024> shot_inflight_curve_{};
  bool shot_inflight_curve_set_ = false;
  // S103 RBC: scene requests (scene_mu_), and the one copy in flight (host thread only)
  std::mutex scene_mu_;
  std::vector<SceneCb> scene_waiting_;
  bool scene_stop_ = false;                 // (scene_mu_) the host is stopping: no new request
  std::atomic<bool> scene_active_{false};   // a request waits, or a copy is in flight: the one per-frame flag the host thread loads
  std::vector<SceneCb> scene_cbs_;          // the callbacks the copy in flight answers
  bool scene_inflight_ = false;
  int scene_retries_ = 0;                   // F2: QueueReadback refusals of the current batch (scene_rules::kRetries; the callbacks wait for the next frame meanwhile)
  uint64_t last_out_frame_ = 0, last_handled_frame_ = 0;   // F1: the guest frame `last_out_` was drawn for; the newest frame Handle saw
  uint64_t scene_tag_ = 0, scene_frame_ = 0, scene_tag_n_ = 0;
  std::array<uint8_t, 1024> curve_{};   // host thread: the display table as last set (the table the present applies: 4 bytes a level)
  bool curve_set_ = false;               // false = identity
  bool final_logged_ = false;
  SceneFeed feed_;                  // S103 HN (OFF only): which required record inputs have been stamped since this scene began
  bool feed_gave_counted_ = false;
  uint64_t fc_handled_ = 0;   // the frames the census was opened for (the exit line compares it with the frames it closed)
};

}  // namespace wwe2k14::scene_host
