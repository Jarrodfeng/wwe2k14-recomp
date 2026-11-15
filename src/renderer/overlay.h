// Settings parity S11 (Session 100): the app's overlay - its Dear ImGui dialogs (the performance overlay, the CineCam panel, prompts) - drawn by
// the new renderer over the picture at each present. The game side records ImGui's draw lists into an OverlayList on the UI thread
// (renderer_adapter/overlay_drawer.h); the live sink hands the newest one to the renderer (Renderer::SetOverlay), which draws it in the present
// pass at the window's full size (renderer.cpp RecordPresent). No SDK or game includes.
#pragma once
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <cstdint>
#include <vector>

namespace rdr {

// Pictures the new renderer has handed to the screen (each present; the decoupled 120's in-betweens included). The performance overlay
// shows their rate beside the game's own (Jarrod's phone test: the graph, the game's frames, never showed the in-betweens).
inline std::atomic<uint64_t> g_presented{0};

// Lane 269: the GPU time of every submitted frame (two timestamps around its command buffer, read a few frames late without stalling).
// Total ns and count since the start, and the newest frame's time in microseconds. Zero count = no timestamps (off, or the queue has none).
inline std::atomic<uint64_t> g_gpu_ns_total{0};
inline std::atomic<uint64_t> g_gpu_frames{0};
inline std::atomic<uint32_t> g_gpu_last_us{0};
// S102 S0-3 (--wwe2k14_rdr_gpu_stamps, default OFF): a stamp after each run of same-class steps, so the GPU line splits by role and shows the start gap [0]-[1]. Off = lane 269's
// two-timestamp bracket only. The PC cost was ~0.1 ms a frame in the Bryan replay (above the 0.05 bar). The live sink sets this from the per-frame switch table (rdr_sw::kGpuStamps);
// rplay: --gpu-stamps 1.
inline std::atomic<bool> g_gpu_stamps{false};
// S102 H2 piece 3 (--wwe2k14_rdr_fence_friendly, default OFF): the render thread's slot-fence wait polls and sleeps instead of one blocking vkWaitForFences (fence_wait.h). Set per frame by the live sink from the switch table.
inline std::atomic<bool> g_fence_friendly{false};
// S102 G6: with stamps on, ALSO close a stamped run after every step and keep each step's first target image, so the summary line lists the ms a frame per (class, kind, image): the offline
// census that names what `other` holds (rplay --gpu-census 1; changes the stamps' cost, never the picture; no game switch).
inline std::atomic<bool> g_gpu_census{false};
// S102 R24-B (--wwe2k14_rdr_gpu_pass_stamps, default OFF; rplay --gpu-pass-stamps 1): the live census: a stamp after EVERY step and one per-step table ([rdr][gpu][passes]) with mean / p90 GPU us at every 600 frames. Forces stamps on.
inline std::atomic<bool> g_gpu_pass_stamps{false};
// S102 G11 (rplay --frame-trace FILE; the file is rplay's, closed by it): one json line per frame-class pass, draw and in-pass clear, plus the GPU ms of each frame-class pass when the census is on
// (docs\renderer\FRAME_CLASS.md). Offline instrument: it records nothing differently and the game never sets it.
inline std::atomic<std::FILE*> g_frame_trace{nullptr};
// S102 G10 (instrument, no game switch; rplay --draw-mod MODE NAMES): the offline colour-pass ranking. MODE 1 = leave out every draw whose program name contains one of NAMES
// (comma list; "*" = every draw), MODE 2 = draw them with a 1x1 scissor (vertex + raster setup only: the fragment work is gone). Applied in every pass; the picture is meant to be wrong. 0 = off.
inline std::atomic<int> g_draw_mod_mode{0};
inline std::atomic<uint32_t> g_draw_mod_version{0};  // bumped by SetDrawMod; RecordPass re-matches the program names when it moves
inline std::mutex g_draw_mod_mutex;
inline std::string g_draw_mod_names;
// S102 G10 (instrument; rplay --draw-census 1): per program name, what the scene colour passes (GPU class colour) draw: draws, indices, area_px (counts.h bound: viewport x scissor, overdraw included).
struct DrawCensusRow { uint64_t draws = 0, indices = 0, area_px = 0, vs_invocations = 0, fs_invocations = 0; };
inline std::atomic<bool> g_draw_census_stats{false};  // S102 G10: also a pipeline-statistics query per run of same-program draws (vertex + fragment shader invocations); waits for the GPU every frame
// S102 G10: the arm tag of the frame being rendered (rplay --arms sets it per frame); every stamped frame's colour / work / total ms is kept under the tag it was recorded with.
inline std::atomic<int> g_arm_tag{0};
inline std::atomic<bool> g_arms_on{false};  // review F1: set only by rplay --arms; CollectGpuTime keeps samples only then
inline std::atomic<uint64_t> g_arm_kept{0};  // samples kept (rplay prints it: 0 without --arms)
struct ArmSamples { std::vector<float> colour, work, total, copies, shadow, other; };
inline std::mutex g_arm_mutex;
inline std::map<int, ArmSamples> g_arm_samples;
inline std::atomic<bool> g_draw_census{false};
// S102 C4 (instrument; rplay --draw-log FILE, needs --draw-census 2): one text line per colour-pass draw, in order: frame, pass, op, program, state, area bound, triangles, vertex and fragment invocations. Never set by the game.
inline std::atomic<std::FILE*> g_draw_log{nullptr};
inline std::mutex g_draw_census_mutex;
inline std::map<std::string, DrawCensusRow> g_draw_census_rows;
inline void SetDrawMod(int mode, const std::string& names) {
  std::lock_guard<std::mutex> lock(g_draw_mod_mutex);
  g_draw_mod_names = names;
  g_draw_mod_mode.store(mode);
  g_draw_mod_version.fetch_add(1);
}
// S102 S0-3: over every GPU sample, the pass stamps' sum (the start gap excluded), ns. g_submit_busy_*: the time inside Renderer::Render (the slot's fence wait and the swapchain acquire
// left out) = the CSV's rdr_submit_ms. g_acquire_ns_total / g_present_call_ns_total: time in vkAcquireNextImage / the Present call. g_render_thread_*: the live render THREAD's CPU time
// (bind + parse + Render: live.cpp's loop), the CSV's render_thread_ms. The frames CSV takes the change since the last guest frame.
inline std::atomic<uint64_t> g_gpu_work_ns_total{0};
inline std::atomic<uint64_t> g_submit_busy_ns_total{0};
inline std::atomic<uint64_t> g_submit_busy_calls{0};
inline std::atomic<uint64_t> g_acquire_ns_total{0};
inline std::atomic<uint64_t> g_present_call_ns_total{0};
inline std::atomic<uint64_t> g_render_thread_ns_total{0};
inline std::atomic<uint64_t> g_render_thread_passes{0};
inline std::atomic<uint32_t> g_present_gap_max_us{0};  // S102 R25-C2: the largest present gap (us) seen since the frames csv last took it (NotePresent raises it, the row writer takes and clears it): a 2-present row keeps its larger gap
inline std::atomic<uint64_t> g_record_thread_ns_total{0};  // S102 R21-B review F2: rdr_record's CPU (wwe2k14_rdr_stage_split); render_thread_ms stays the bind stage alone

struct OverlayVertex {  // the layout of Dear ImGui's ImDrawVert and the SDK's ImmediateVertex
  float x, y;           // in the list's coordinate space (0..width, 0..height: the window, top-left origin)
  float u, v;
  uint32_t color;       // RGBA8 (R in the low byte)
};

struct OverlayDraw {
  uint32_t texture = 0;     // an id given to Renderer::SetOverlayTexture; 0 = plain white
  uint32_t first_index = 0, count = 0;
  int32_t base_vertex = 0;
  bool clipped = false;
  float clip[4] = {0, 0, 0, 0};  // left, top, right, bottom in the list's coordinate space
};

struct OverlayList {
  float width = 0, height = 0;  // the coordinate space (the window in ImGui's logical units)
  std::vector<OverlayVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<OverlayDraw> draws;
};

}  // namespace rdr
