// The live scene drawer on our render thread (Session 103, lane S; docs\renderer\FRAME_CONTRACT.md 2, 3, 5, 7).
// MIRROR (--wwe2k14_rdr_scene=1): on a sheet frame, after the game's frame was presented and its picture saved, the present's own scene
// record is drawn ALONE (no stream, no binder) through the record -> frame path (record_frame.h) and the front-end, into the front-end's own
// images, read back with the game's display curve and written as a PNG beside the game's picture. Waits on the GPU (dev comparison only).
// OURS (=2): DrawLive records record j into the LIVE frame's builder, into the front-end's own scene images; it never waits and never loads
// on the render thread: an asset not resident makes the frame a miss here and READY false (the game then walks), and the loader thread
// fetches it; AdoptLoaded (render thread, between frames) hands loaded models to the front-end and READY returns.
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../renderer/frame_builder.h"
#include "../renderer/renderer.h"
#include "draw_lists.h"   // S103 G2: the kept draw lists, one per drawer
#include "frame_hooks.h"  // S103 R2: the stage hooks + see order of a frame
#include "live_assets.h"
#include "roster_signal.h"   // RRS: roster_ready / roster_failed in the stream-free build
#include "record_frame.h"
#include "post_from_record.h"   // S103 FXF: Info (NoteFx)
#include "../renderer_game/scene_kind.h"   // S103 L2d: the flat frame's facts

namespace rdr_scene {

class LiveSceneDraw {
 public:
  explicit LiveSceneDraw(rdr::Renderer& renderer);
  ~LiveSceneDraw();  // stops the loader; the caller waits for the GPU first
  void SetGameRoot(const std::string& root);  // the game folder may only be known after start (live.cpp hands it over each frame)
  // S103 SCS: the stream-free host's draw of the in-scene 2D rows (frame_hooks.h LiveScene2D) into the LIVE frame's scene pass. Null (the default, the stream build) = no hook, the rows stay with whoever drew them.
  void SetScene2D(LiveScene2D::DrawFn fn) { scene2d_fn_ = fn; if (live_hooks_) live_hooks_->SetScene2D(std::move(fn)); }
  bool Scene2DOn() const { return bool(scene2d_fn_); }

  // MIRROR: `rec` alone at w x h -> `png` (the display curve applied when it has 1024 bytes). `report` = one log line.
  bool DrawToPng(const SceneTables& rec, uint32_t w, uint32_t h, const std::vector<uint8_t>& curve, const std::filesystem::path& png,
                 std::string& report);

  // S103 CPB (docs\research\s103\CPR.md H1, the Create a Superstar roster portrait): the wrestlers family of `rec` alone, drawn with the game's portrait camera `cam` (game space) into an offscreen w x h image cleared to (0, 0, 0, 0),
  // read back as RGBA8 (the front end's RenderToPixels: it waits for the GPU once, on the host thread, outside any live frame). `model` = the engine object of the edited wrestler: the record must hold it, else false
  // (`why`). *loading = the miss is a resource still loading (the caller asks again next frame). The front end's own images are remade at w x h and again at the next live frame's size: the caller forgets its held picture.
  bool DrawPortrait(const SceneTables& rec, const Camera& cam, uint32_t model, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba, std::string& why, bool* loading = nullptr);
  // OURS: record `rec` into `fb` (the live frame, already begun) at w x h; `out` = the image our frame ends in (the post output when
  // `post` is given, else the scene colour). False = a miss (why says which: not resident, build fault, draw fault): nothing was recorded.
  // S103 CHS: did the last failed DrawLive put an op on `img` (a picture an earlier Ok draw returned)? Every fault path returns before the post combine writes post_out_ (front_end.cpp Record 1408 is before any op,
  // post_pass.cpp Record's `return false`s are all before the combine at the end), so only the scene colour image can have been written, and only when the failure came after the scene passes: a no-post picture
  // (out == colour) is spoiled then, a post_out_ picture never. false for a miss that failed before Record (nothing was recorded).
  bool FaultSpoiled(rdr::ImageId img) const { return fault_scene_drawn_ && img != rdr::kNoImage && img == fault_color_; }
  bool DrawLive(const SceneTables& rec, rdr::FrameBuilder& fb, uint32_t w, uint32_t h, const PostParams* post, rdr::ImageId& out,
                std::string& why, rdr::ImageId* out_depth = nullptr, bool* loading = nullptr);   // S103 L31A: *loading = the miss is a resource still loading (a hole, not a strike)
  // S103 L31A: live play's release of the front-end models the caches and lane C retired (no GPU wait: the renderer delays the destroys). Call once per frame before DrawLive / Warm.
  void ReleaseRetiredLive(uint64_t guest_frame);
  // OURS, on a frame the game walks (not stamped ours): builds the frame only (no draw) so the misses reach the loader and READY is learned.
  // Without it READY could never turn on (only an OURS frame would ever ask, and none is OURS until READY).
  void Warm(const SceneTables& rec, uint32_t w, uint32_t h);
  // S110 ENTRWAIT2 (scene_start_wait.h): are our loads done? Warm / DrawLive call it after their build; the host calls it with built = false on a handled frame that built nothing.
  void PublishStartWait(uint64_t guest_frame, bool built = true, bool drew = false);   // review: drew = DrawLive (the frame is drawn after the call: StartCrowdIn does not prefetch)
  bool LastBuildHadStage() const { return stage_; }   // S103 HN: the last Warm / DrawLive build had a stage (false = a scene boundary: loading, flat, scene change)
  bool Ready() const { return ready_; }   // the last live build found every asset resident (FRAME_CONTRACT 3 item 2)
  // S103 L2d (scene_kind.h): the 3D half of the FLAT frame verdict for `rec`, from the build Warm / DrawLive made of that same record (call after it): no stage, no 3D object, no
  // Scene3D stamp = a flat frame (exempt from HasStage and the 3D inputs; its 2D / movie / particle half comes from the 2D drawer's census). A record this drawer did not build last,
  // or could not build, reads as a 3D frame (fail closed).
  void FlatFacts3D(const SceneTables& rec, scene_kind::FlatFacts& f) const;
  void AdoptLoaded();                      // render thread, between frames: loaded models into the front-end; READY may return
  // S108 SHOT-A (the subject-shot saver): the live drawer's persistent liquid images that hold paint (live_maps.h LiveLiquidImages); empty when there is no maps hook yet. Host thread, between frames.
  void LiveLiquidImages(std::vector<LiveMapsHook::LiveLiquidImage>& out) const;

  // S103 T7 (the stage mirror, --wwe2k14_dump_targets): the NEXT DrawToPng also reads back every map of its frame (LiveMaps by role) and writes ours_<role>.<n>_f<frame>.rgba8 + .png and
  // manifest_ours_f<frame>.txt into `dir`. One shot: cleared when that call ends. Never set = nothing runs.
  void SetMapDump(const std::filesystem::path& dir, uint64_t frame) { map_dump_dir_ = dir; map_dump_frame_ = frame; }

  std::string Census() const;
  // S103 NR (1): why READY was (not) true, per term, over the calls since the last drain (Warm + DrawLive): "[scene][ready] calls N (warm W, live L), ready R; not built a; misses b (cache misses m);
  // holes c [arena x (first: ...), wrestlers y (first: ...)]; no stage d; shadow input e; post missing f; draw error g (first: ...)". Terms are counted per call (a call can fail several).
  // live.cpp logs it every 600 SceneOurs calls and at exit. `drain` resets the window.
  std::string ReadyCensusLine(bool drain);
  // S103 UP: "[scene][texup] ..." = the live texture uploads since the start by caller (page / screen / other: calls / KB) and the page sends the shared image saved. Logged next to [scene][ready] every 600
  // frames ONLY when the GPU stamps are on (a stamped run): never a player line. Empty without a front end.
  std::string TexUpLine() const;
  // S103 OFFT (HL_holes_no_stream.md Q1): the loading misses by name. "[scene][load] ask ..." when an asset is first asked for, "[scene][load] adopted ..." when it arrives (with the ms it waited), pac /
  // entry / variant / key from rf::ModelCache::Missed (a lane C arena names its pac). Throttled: the first 60 of each, then every 50th. Render thread only; the host drains and logs them.
  std::vector<std::string> TakeLoadLog() { std::vector<std::string> v; v.swap(load_log_); return v; }
 private:
  void NoteLoadLine(bool ask, const std::string& what, const std::string& key, int arena);
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> asked_at_;   // key -> when it was queued (the wait is logged on adoption)
  uint64_t load_ask_lines_ = 0, load_adopt_lines_ = 0;
  std::vector<std::string> load_log_;   // capped (kLoadLogCap): a run nobody drains cannot grow it
  // S103 NR (3): THE not-ready rule, one function for Warm and DrawLive (the holes of every family, the loads, the stage). Bits of the result; 0 = nothing blocks. The shadow input's hole is the
  // maps hook's (BuildMapsInput): Warm asks FrameHooks::ShadowInputHole, DrawLive reads the input AttachFrameHooks built (the same rule), both OR kTermShadow in.
  enum Term : uint32_t { kTermNotBuilt = 1, kTermMisses = 2, kTermHoles = 4, kTermNoStage = 8, kTermShadow = 16, kTermOther = 32 /* post missing / draw error: counted by their own counters */ };
  uint32_t BuildTerms(bool built, const std::vector<rf::ClassReport>& reports) const;
  void NoteTerms(bool live, uint32_t terms, const std::vector<rf::ClassReport>& reports, const std::string& error, uint32_t skipped = 0);   // S103 HN: `skipped` = terms OFF does not let hold READY (counted, never blocking)
  struct ReadyCensus {
    uint64_t calls = 0, warm = 0, live = 0, ready = 0;
    uint64_t not_built = 0, misses = 0, miss_assets = 0, holes = 0, no_stage = 0, shadow = 0, post_missing = 0, draw_error = 0;
    uint64_t hole_family[size_t(rf::Family::Count)] = {};
    std::string hole_first[size_t(rf::Family::Count)];
    std::string not_built_first, shadow_first, post_first, draw_first;
  } rc_;
  void DumpMaps(const LiveMaps* maps, const std::filesystem::path& dir, uint64_t frame);   // S103 T7
  std::filesystem::path map_dump_dir_;   // S103 T7: empty = off
  uint64_t map_dump_frame_ = 0;
  void EnsureFrontEnd();
  void LoaderLoop();
  rdr::Renderer& renderer_;
  std::unique_ptr<GameFilesSource> source_;        // the render thread's (mirror loads on miss)
  std::unique_ptr<DrawLists> lists_;               // S103 G2: made with the front-end (declared first: destroyed after it); Tick(guest frame) before each live Record
  std::unique_ptr<FrontEnd> fe_;
  std::unique_ptr<rf::ModelCache> mirror_cache_;   // load_on_miss = true
  std::unique_ptr<rf::ModelCache> live_cache_;     // load_on_miss = false: misses go to the loader
  // S103 R2: the stage hooks (maps, particles) + the game's see-through order, one per cache (each reads its own cache's CPU models); declared after the caches: destroyed before them
  std::unique_ptr<FrameHooks> mirror_hooks_, live_hooks_;
  LiveScene2D::DrawFn scene2d_fn_;   // S103 SCS
  std::string root_;
  bool ready_ = false;
  void NoteStage(const SceneTables& rec, bool built, const std::vector<rf::ClassReport>& reports);   // S103 L2d
  uint64_t stage_gf_ = ~0ull;   // S103 L2d: the guest frame of the record the stage facts below describe
  bool stage_ = true;           // its HasStage (true when the frame was not built: never judged flat)
  uint32_t stage_objects_ = 1;  // objects over every class report
  rdr::ImageId fault_color_ = rdr::kNoImage; bool fault_scene_drawn_ = false;   // S103 CHS (review fix 1): the last failed Record: the scene colour image, and whether any scene op was recorded into it before it failed
  uint64_t arena_epoch_seen_ = 0;   // S103 SCX: rf::ArenaEpoch at the last build
  uint64_t last_drawn_gf_ = 0; bool have_drawn_gf_ = false;   // S103 M (N3): the guest frame of the last frame DrawLive recorded (the front end's slot-800 live screen is only valid right after it)
  // the loader thread (its own source: Load runs only there)
  std::thread loader_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::string loader_root_;
  struct Job { rf::ModelCache::Missed m; int arena = -1; };   // arena >= 0: lane C's whole-arena load (rf_c.h), else a cache miss
  std::deque<Job> want_;
  bool bulk_ok_ = false;   // S105 SKB2: guarded by mu_: the scene's roster is in (first OURS frame drawn); bulk jobs (the evtobj prewarm) start only then
  void SetBulkOk(bool v);
  bool prewarm_queued_ = false;   // S105 SKB fix F3: an evtobj.pac prewarm job is queued or running (render thread only)
  bool asked_now_ = false;        // S110 ENTRWAIT2: the last QueueMisses asked for a non-bulk asset (render thread only)
  uint64_t start_bodies_seen_ = 0;   // S110 ENTRBODIES: the start_bodies board version AskStartBodies took last (render thread only)
  int game_arena_ = -1;              // S110 ENTRBODIES: the arena pac the game opened last (QueueMisses), for the crowd's classes while a start is held (render thread only)
  bool AskStartBodies();             // S110 ENTRBODIES: the game's characters' bodies -> the loader (PublishStartWait); true = a body was queued now
  bool StartCrowdIn(bool drew);      // S110 ENTRBODIES: the arena's crowd classes asked while a start is held; true = none still loading (drew: not asked, true)
  uint32_t in_flight_ = 0;        // S110 ENTRWAIT2: guarded by mu_: non-bulk jobs the loader thread is loading now
  struct Loaded { rf::ModelCache::Missed m; int arena = -1; assets::Model model; bool ok = false; std::string error; std::string car_key; };
  void QueueMisses();          // the live cache's misses and lane C's arena misses -> the loader
  std::vector<std::pair<uint64_t, int>> retired_held_;   // S103 L31A: (guest frame retired, model): released two frames later
  void ReleaseRetiredIdle();   // (GPU idle) the models the caches and lane C retired
  std::vector<Loaded> done_;
  // census
  uint64_t sheets_ = 0, failed_ = 0, drawn_ = 0, skipped_ = 0;
  uint64_t live_frames_ = 0, live_misses_ = 0, loads_ok_ = 0, loads_failed_ = 0;
  std::string first_load_error_;
  RosterSignal roster_signal_;   // RRS (render thread only): the live loader's wrestler bodies -> render_signal roster_ready / roster_failed (stream-free build only)
  // S103 P: our post from the record (post_from_record.h): the parameters live here so frame.post's pointer stays valid through Record.
  // RENDER THREAD ONLY (the counters are plain: Census() is read on the render thread too). Live and mirror frames are counted apart.
  enum class PostResult { Post, NoPost, Missing };
  PostResult BuildPost(const SceneTables& rec, const Camera& cam, uint32_t w, uint32_t h, bool live, const PostParams*& out, const char*& why);
  void NoteFx(const SceneTables& rec, const post_from_record::Info& pi);   // S103 FXF
  PostParams post_store_;
  uint64_t post_live_ = 0, post_live_none_ = 0, post_live_missing_ = 0, post_mirror_ = 0, post_mirror_none_ = 0, post_unsupported_ = 0, post_strength_default_ = 0;
  const char* post_last_why_ = "";
  // S103 FXF: the afterimage / tint census (counted once per record frame: Warm and DrawLive both call BuildPost). blend 0 = alpha, 1 = additive (S+272).
  uint64_t fx_frames_ = 0, fx_ghost_ = 0, fx_ghost_alpha_ = 0, fx_ghost_additive_ = 0, fx_sepia_dropped_ = 0, fx_tint_kept_ = 0, fx_disp_default_ = 0, fx_last_frame_ = ~0ull;
  uint64_t fx_tonemap_ = 0, fx_night_ = 0, fx_aa_ = 0, fx_aa_skipped_ = 0, fx_hala300_ = 0, fx_hala304_ = 0 /* halation runs */, fx_radial_ = 0;   // S103 PST
  uint64_t fx_defocus_ = 0, fx_emboss_ = 0, fx_retro_ = 0, fx_retro_setting_off_ = 0, fx_cue_garbage_ = 0;   // S103 FXH: frames that drew the cue effects (defocus + hue, emboss, retro film)
};

}  // namespace rdr_scene
