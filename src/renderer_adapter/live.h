// The live sink (Session 100, docs\renderer\LIVE_ADAPTER.md): what CaptureWriter takes, kept in memory and drawn by the new renderer on its own
// thread through the same binder rplay uses on a capture. One code path: a capture is a recording of what live mode draws.
// Stage A (mirror): the renderer runs headless next to the old one and writes its picture of chosen frames to PNG files.
// No game or SDK includes: the reader calls it under its own lock; the render thread is in live.cpp.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "adapter_frame.h"
#include "async_readback.h"
#include "capture.h"
#include "../renderer/overlay.h"

namespace rdr_adapter {

// Lane 273: the longest time (ns) since the last read-out that the live sink's store mutex was HELD, by holder, and the longest an Append WAITED for it.
// 0 ReadGeometry (find + handle, the bytes are copied after), 1 Append's insert, 2 DropGeometry, 3 the texture-contents erase / moves; [4] = the longest Append wait.
// The reader prints and clears them (TakeStoreLockMax) in the slow-frame and 600-frame lines.
// [5] = the longest Append allocation, [6] = the longest case-ring OnContent inside an Append. The clock reads run only while
// g_store_timing is set (the reader sets it from --wwe2k14_rdr_debug_fine_times): nothing is timed on the default path.
inline std::atomic<uint64_t> g_store_lock_max_ns[7];
inline std::atomic<bool> g_store_timing{false};
struct StoreTimer {
  int holder;
  bool on;
  std::chrono::steady_clock::time_point t0;
  explicit StoreTimer(int h) : holder(h), on(g_store_timing.load(std::memory_order_relaxed)) {
    if (on) t0 = std::chrono::steady_clock::now();
  }
  uint64_t Lap();  // ns since the last Lap / construction, 0 when off (defined below NoteStoreLock)
  void Stop();     // notes the time since the last Lap under `holder`
};
inline void NoteStoreLock(int holder, uint64_t ns) {
  uint64_t cur = g_store_lock_max_ns[holder].load(std::memory_order_relaxed);
  while (ns > cur && !g_store_lock_max_ns[holder].compare_exchange_weak(cur, ns, std::memory_order_relaxed)) {
  }
}
inline uint64_t StoreTimer::Lap() {
  if (!on) return 0;
  const auto now = std::chrono::steady_clock::now();
  const uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count());
  t0 = now;
  return ns;
}
inline void StoreTimer::Stop() {
  if (on) NoteStoreLock(holder, Lap());
}

struct LiveOptions {
  uint32_t format_caps_mask = 0;          // lane 200: renderer.h RendererOptions::format_caps_mask (dev switch, 0 = the device as it is)
  int validation = 0;                     // the renderer's validation level (0 off, 1 core + sync, 2 + best practices)
  float render_scale = 1.0f;              // S100 settings parity S1: the game's 1280x720 targets drawn this many times larger
  std::filesystem::path png_folder;       // where the mirror's pictures go
  std::vector<uint64_t> png_frames;       // guest frames to write
  uint32_t png_every = 0;                 // also every Nth drawn frame; 0 off
  uint32_t max_queued = 4;                // frames waiting beyond this are applied (images, contents) but not drawn, and counted
  void* window_instance = nullptr;        // stage B (owner): the game window (Windows: HINSTANCE + HWND; Android: ANativeWindow* in window)
  void* window = nullptr;                 // null: headless (the mirror)
  void* (*window_source)(void*) = nullptr;  // lane 235 (Android): the window's handle right now (a new ANativeWindow after the background); null = `window` only
  void* window_source_user = nullptr;
  bool keep_state_copies = false;         // strip-360 A1: a skipped frame keeps per-frame copies its own frame read before writing (state carried)
  bool prune = true;                      // lane 307: a skipped frame binds only the passes its lasting copies need (false = every op but the per-frame copies, the old bind)
  bool prune_plant = false;               // lane 307 TEST ONLY: the walk-back stops after the first level (a planted fault: a lasting copy whose producers are not bound)
  bool perframe_by_image = true;          // lane 307: a copy into an image written every frame with overlapping rectangles is per-frame work (lasting_prune.h)
  bool catch_up_state = true;             // strip-360 A1b: the drawn frame repeats the state steps the skipped frames before it left out
  bool vsync = true;                      // S100: owner mode follows the SDK's vsync switch (the launcher's V-SYNC): FIFO, else mailbox / immediate
  bool film_grain = true;                 // settings parity S3: false = the 30 Years old-film pass drawn clean (binder.h g_film_grain)
  float shown_wide = 1.0f, shown_tall = 1.0f;  // lane 201: the screen-shape factors the present stretches back (reader.h g_wide); photo mode saves the stretched picture
  bool present_fill = false;              // settings parity S5: the present stretches to the window (the reader squeezes the scene)
  bool present_sgsr = false;              // settings parity S10a: the phone's SGSR UPSCALING (--present_effect=sgsr) in our present
  int dlss_mode = -1;                     // settings parity S10b: DLSS (renderer.h RendererOptions::dlss_mode), -1 off
  std::string streamline_dir;              // NVIDIA's runtime folder
  bool dlss_jitter_legacy = false;         // lane 185: the S10b jitter / velocity window (diagnosis, --wwe2k14_rdr_dlss_jitter_legacy)
  uint32_t dlss_phases = 8;              // the jitter's phases (dlss_jitter.h)
  bool tween = false;                     // settings parity S9b (the decoupled 120): each tick drawn twice, halfway from the previous tick
                                          // (renderer/frame_blend.h), then its own; the game keeps its 60 updates
  bool gpu_timestamps = true;             // lane 269: renderer.h RendererOptions::gpu_timestamps (--wwe2k14_rdr_gpu_timestamps)
  uint32_t present_hz = 0;                // S100 phone 120 fix 2: the presents spaced at this rate (renderer.h RendererOptions::present_hz)
  uint32_t draw_vblanks = 0;              // settings parity S9 (the decoupled 30): a frame is drawn once this many vblanks of presents have
                                          // passed (2: 60-a-second presents every other one, the game's stock 30 each one); 0 / 1 every frame
  int crowd_density = 100;                // settings parity S6 / S7 / S8 (binder.h): CROWD DENSITY, SHADOWS, DEPTH OF FIELD
  bool shadows = true;
  bool density_track = true;              // S101 lane 234 (crowd_rank.h): the density rank follows a member that moves
  bool depth_of_field = true;
  bool shadow_half = false;               // settings parity S12: the shadow maps every other frame (binder.h g_shadow_half)
  bool crowd_cards_thin_probe = false;    // lane 175 (SF2): census of the thin record's stamp offset (cvar >= 1)
  bool crowd_cards_thin = false;          // lane 175: with crowd_cards, a frame whose stream holds only the first card of each type loop is drawn from the record (card_thin_core.h)
  bool crowd_members_probe = false;       // lane 272 (member_thin_core.h): census of the members' thin record (cvar >= 1): class-uniformity verify + the stamp offsets probe + the gate's reports; nothing is drawn differently
  bool crowd_members_thin = false;        // lane 272: a frame whose stream holds only the first member of each mesh run is drawn from the record (cvar >= 2 and member_thin::kSkipBuilt)
  bool crowd_cards = false;               // S101 lane CD: with crowd_ours, the far cards ours (CrowdOursOptions::cards: one instanced draw per person type, CrowdHook::InjectCards)
  bool crowd_floor = false;               // S101 lane 82: with crowd_ours, the seated floor people (site 0x8276ED60) ours too (crowd_hook.h kCrowdFloorSite; CrowdOursOptions::floor)
  bool post_ours = false;                 // S101 lane PO (post_hook.h): the post chain (depth of field, bloom, glare, the combine, the 30 Years film) recorded by our own pass (binder.h g_post_ours); no scene record needed
  bool crowd_ours = false;                // S101 lane 35: the crowd drawn by us from the live scene record (binder.h g_crowd_ours); needs --wwe2k14_scene_records
  bool arena_ours = false;                // S101 lane 41: the arena drawn by us in place (binder.h g_arena_ours; replace_factories.h); needs --wwe2k14_scene_records
  bool flash_ours = false;                // S101 flash contract: the crowd flashes ours (binder.h g_flash_ours); needs --wwe2k14_scene_records
  bool screens_ours = false;              // S101 lane 115: the stage screens' geometry ours (binder.h g_screens_ours); needs --wwe2k14_scene_records
  bool crowd_props_ours = false;          // S101 lane 133: the crowd's sign / foam-hand draws ours (binder.h g_crowd_props_ours); needs --wwe2k14_scene_records
  bool props_ours = false;                // S101 lane 84: the commentary table, ring steps and announcer microphone ours (binder.h g_props_ours); needs --wwe2k14_scene_records
  bool shadow_casters_ours = false;       // S101 lane 99: our caster geometry in the shadow passes (binder.h g_shadow_casters_ours); needs --wwe2k14_scene_records and the wrestlers' and / or the ring's switch for their joins
  bool ui_low_ram = false;                // lane 126: --wwe2k14_low_ram is in force (the UI class skips bgEtc)
  bool small_passes_ours = false;         // lane 135: the sweat map and skin pre-light passes ours (binder.h g_small_passes_ours); no scene record needed
  bool rim_chain_ours = false;            // lane 151: the rim light map's reductions and blur by our programs (binder.h g_rim_chain_ours); no scene record, no game file
  bool ui_quads = false;                  // S101 lane 148: with ui_ours, the sprites and T_Rect rectangles drawn with our own quad and 2D program (ui_quads.h g_ui_quads); no scene record, no game file of its own
  bool movie_ours = false;               // S101 lane 131: the movie planes (T_SpriteYUVAlpha) drawn by our quad and YUV shader (binder.h g_movie_ours); no scene record, no game file
  bool rim_ours = false;                  // lane 124: our caster geometry in the rim light map (binder.h g_rim_ours); needs --wwe2k14_scene_records and the wrestlers' switch (the joins)
  bool plane_shadow_ours = false;         // lane 139 (SH3 slice 2): the plane shadow chain's constants from the scene record (binder.h g_plane_shadow_ours); needs --wwe2k14_scene_records
  bool circles_ours = false;              // lane 136 (SH3): the circle blob shadows from the scene record (binder.h g_circles_ours); needs --wwe2k14_scene_records
  bool ui_ours = false;                   // S101 UI contract: the 2D draws' textures ours (binder.h g_ui_ours); no scene record needed
  bool ui_text_ours = false;              // S101 lane 89 (UT): the game's text laid out by us (binder.h g_ui_text_ours); no scene record needed; turns the run-string labels on (origin_intern.h g_labels_wanted)
  bool shadows_ours = false;            // S101 lane 123 (SH2): our shadow passes (shadows_ours.h); no scene record needed
  int copy_alias = 0;                   // S101 lane 190: resolve copies dropped by alias (copy_alias.h): 0 off, 1 census, 2 drop
  uint32_t copy_alias_classes = 1;      // S101 lane 190: which classes mode 2 drops (copy_alias.h Class bits)
  uint32_t shadow_map_size = 512, shadow_plane_percent = 100;  // S101 lane 123
  bool particles_ours = false;          // S101 lane 67: particle runs drawn as one draw each (binder.h / particle_runs.h g_particles_ours); no scene record needed
  bool ring_ours = false;                 // S101 ring contract: the ring drawn by us in place (binder.h g_ring_ours); needs --wwe2k14_scene_records
  std::string caw_cache_root;             // lane 153: the game cache root / caw for the counts-index cache (empty = none)
  std::string pipeline_store_dir;         // lane 276: renderer.h RendererOptions::pipeline_store_dir (empty = no pipeline store)
  uint32_t pipeline_prebuild_slice_us = 2000;  // lane 276: RendererOptions::pipeline_prebuild_slice_us
  bool caw_ours = false;                  // lane 153 (N3): the editor wrestler by us in place (binder.h g_caw_ours); no scene record needed
  bool wrestlers_ours = false;            // S101 lane 41: the wrestlers drawn by us in place (binder.h g_wrestlers_ours); needs --wwe2k14_scene_records
  bool arena_d3 = false;                  // lane 138: D3 skip the object for the arena (d3_arena.h g_arena_d3); needs arena_ours + arena_thin + scene records + light states
  bool arena_d3_verify = false;           // lane 152: the verify arm (markers written, nothing skipped, the stream's draws compared with the sequence); needs arena_d3
  bool caster_d3 = false;                 // lane 228: D3 skip the caster pass (d3_arena.h g_caster_d3; the live seam of SKIP_GAME_WORK_MAP.md 17.12); needs shadow_casters_ours + rim_ours + scene records + light states + owner mode
  bool ring_d3 = false;                   // lane 246: D3 skip the ring's model call (d3_arena.h g_ring_d3; the live seam of SKIP_GAME_WORK_MAP.md 16.9); needs ring_thin + the ring-rows record (--wwe2k14_rdr_ring_d3) + light states + owner mode
  bool ring_d3_verify = false;            // lane 246: the verify arm (markers written, the original always runs, the stream's draws compared with the sequence); needs ring_d3
  bool caster_d3_plane = false;           // lane 268: the plane pass joins the caster cut (d3_arena.h g_caster_plane); needs caster_d3 + --wwe2k14_scene_plane
  bool caster_d3_verify = false;          // lane 228: the verify arm (markers written, the original always runs, the stream's draws compared with the sequence); needs caster_d3
  bool wrestlers_d3 = false;              // lane 245: D3 skip the wrestler's colour call (d3_arena.h g_wrestlers_d3; the live seam of SKIP_GAME_WORK_MAP.md 18.9); needs wrestlers_ours + wrestlers_thin + scene records + light states + owner mode
  bool wrestlers_d3_verify = false;       // lane 245: the verify arm (markers written, the original always runs, the stream's draws compared with the sequence); needs wrestlers_d3
  bool arena_thin = false, ring_thin = false, wrestlers_thin = false;  // lane 122: D1 thin draws (thin_draw.h g_*_thin); each needs its class's _ours switch (ring / wrestlers: lane 129)
  int (*announced_arena)() = nullptr;      // S101 lane 271: the arena the match card names (-1 none), asked each frame; a change starts the per-arena loads early (announce.h)
  void (*on_d3_latch)(const char* line) = nullptr;  // lane 301: called on the render thread with a D3 chain's "latched off" line right after it is logged (d3_latch_hook.h: the case ring takes a zip at the moment)
  std::string (*crowd_game_root)() = nullptr;  // S101 lane 35: the game files the crowd's bodies are read from (asked when the first record names an arena)
  void (*log)(const char* line) = nullptr;  // where the renderer's and the sink's lines go
  void (*picture_done)() = nullptr;         // called on the render thread once a named picture is handled (written or not)
  std::vector<std::string> picture_isolates;  // with a named picture, the same frame again with only the draws matching each name (rplay --isolate)
};

class LiveSink {
 public:
  LiveSink();
  ~LiveSink();
  LiveSink(const LiveSink&) = delete;
  LiveSink& operator=(const LiveSink&) = delete;

  // Creates the renderer (headless) and starts the render thread. False: nothing started, `error` says why.
  bool Start(const LiveOptions& options, std::string& error);
  // Waits for queued frames, then stops the thread and destroys the renderer.
  void Stop();
  bool active() const;

  // ---- the game side (the reader, under its lock) ----
  // Keeps the bytes; the returned key takes the place of a capture file offset. Never fails.
  uint64_t Append(Stream stream, std::vector<uint8_t>&& bytes);
  // The reader forgot a geometry key (unused for long): its bytes go. Frames already queued never name it (they are newer than its last use).
  void DropGeometry(uint64_t key);
  // The whole image table, sent before the first frame that names a new id.
  void SetImages(const ImageRecord* records, size_t count);
  // The game released the object behind this image.
  void ReleaseImage(uint32_t id);
  // The display table the game sets (256 entries, RGBA8 each: the value shown for each 8-bit channel value), applied by the present from the
  // next frame on.
  void SetDisplayCurve(const uint8_t* rgba256);
  // Hands a finished frame (the capture format's frame file bytes) to the render thread; returns at once. A non-empty `picture` writes this
  // frame's picture as live_<picture>.png (an input script names the frame it also screenshots on the old renderer).
  void SubmitFrame(std::vector<uint8_t>&& frame, uint64_t guest_frame, const std::string& picture = {});
  // Checks only: blocks until fewer than `max_waiting` frames are queued (at most `timeout_ms`).
  void WaitForRoom(size_t max_waiting, uint32_t timeout_ms);
  // A copy the game reads back with the CPU (S100, owner mode: the attires built on the GPU at a character load): the frame so far (the
  // capture format's frame file bytes, no present) is drawn after every frame queued before it, with every image kept at its end (the rest
  // of the frame comes as the next frame), then `image` (the reader's id) at `mip` / `layer` is read back as RGBA8, channels in the image's
  // own order. Blocks until done or `timeout_ms`; false: not read (the image has no RGBA8 reading, the sink stopped, or the wait ran out).
  // Lane 232: where the blocking path's time went, when `times` is given: the mailbox (frames queued ahead + this part's turn), the part's draw, ReadImage.
  struct ReadTimes {
    double wait_ms = 0, draw_ms = 0, read_ms = 0;
  };
  bool DrawAndRead(std::vector<uint8_t>&& frame, uint64_t guest_frame, uint32_t image, uint32_t mip, uint32_t layer, std::vector<uint8_t>& rgba,
                   uint32_t& width, uint32_t& height, uint32_t timeout_ms = 5000, ReadTimes* times = nullptr);
  // Lane 232 (3d): the async read-back mailbox (async_readback.h): the reader learns an image after a blocking read, notes its copies and takes the render thread's finished copy at the lock;
  // the render thread reads a watched image back after it draws a frame holding a copy into it. Always valid (also while the sink is stopped).
  AsyncReadback& Async();
  // Lane 232: how many frames SubmitFrame has taken (the deferred thumbnail asks for the first frame submitted after this count).
  uint64_t SubmittedCount() const;

  // Settings parity S11: the app's overlay (its ImGui dialogs, recorded on the UI thread: overlay_drawer.h), drawn over every picture from
  // the next one on; null = none. A texture the lists name by id (RGBA8). Any thread.
  void SetOverlay(std::shared_ptr<const rdr::OverlayList> list);
  void SetOverlayTexture(uint32_t id, uint32_t width, uint32_t height, std::vector<uint8_t>&& rgba);
  // Photo mode (src\photo_mode.h) with the new renderer: the HUD left out (the SDK's gpu_hide_hud), and the next shown picture (through
  // the display table) written to `path` as PNG. SavePicture: false when the sink is not running.
  void SetHideHud(bool hide);
  bool SavePicture(const std::filesystem::path& path);
  // S101 lane 19: writes the LAST presented picture to `path` (PNG) on the render thread without waiting for another frame (a run's exit presents none).
  // False when the sink is not running. The file appears under its final name only when complete.
  bool SaveLastPicture(const std::filesystem::path& path);
  // S101 lane 216: every class's final census line, printed now on the render thread (the process hard-exits, so a scene shorter than one 600-frame window never
  // printed its line). Waits at most `timeout_ms`; true when printed. Idempotent: a second call with no frame handled in between prints nothing new.
  bool FlushFinalCensus(uint32_t timeout_ms);

  // One line of counts (frames drawn / skipped, draws bound / missing a shader, problems), for the census.
  std::string Report() const;

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace rdr_adapter
