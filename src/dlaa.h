// wwe2k14 - DLAA phase 1 (Session 18 item 8; docs\research\dlss_feasibility.md). All default off.
//
// Step 1, jitter (--wwe2k14_dlaa_jitter): ReXGlue offsets the host viewport of the scene draws by a sub-pixel amount
// each frame (SDK gpu_dlaa_jitter: Halton 2,3 over 8 phases; scene draws = viewports >= 1000 guest pixels wide
// before the frame's scene depth resolve, so the HUD and post passes stay put). Check route
// tools\input_scripts\short\dlaa_jitter_check.txt + py tools\dlaa\jitter_check.py RUN.
//
// Step 2, camera motion vectors (--wwe2k14_dlaa_motion, or --wwe2k14_dlaa_debug=mv|depth): right after the game's
// scene depth resolve, the SDK takes the cached textures of the scene colour and depth resolves (k_8_8_8_8,
// 2560x1440 at the 1440p preset) as storage images, and a compute pass (dlaa_motion.cs.glsl) rebuilds each pixel's
// view-space position from the depth (20e4 float, reversed) and this frame's camera, projects it with last frame's
// camera and writes the difference (host pixels, current -> previous, jitter removed) to an RG16F image. Camera only:
// wrestlers, crowd and cloth get the camera's motion (phase 2: per-object motion). The debug views write into the
// scene colour texture, which the game's post passes and HUD then draw over: mv = the vectors as colour (grey =
// still, red / cyan = left / right, green / magenta = up / down; full colour at --wwe2k14_dlaa_debug_scale pixels),
// depth = linear depth as grey (white = near, black = 80 m).
//
// The camera comes from here: at the scene manager's hand-off to the renderer (sub_823C79A8, src\cinecam.h) the
// composed view slot (world -> view, row vectors, + FOV degrees at +128) is given to the command processor with the
// guest frame number (swaps before this frame), which pairs it with the frame it draws.

#pragma once

#include <rex/cvar.h>
#include <rex/perf/dlaa.h>  // the SDK's camera sink (rexruntime)
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>  // GetModuleFileNameW

#include "frame_stats.h"  // g_guest_frame


REXCVAR_DEFINE_BOOL(wwe2k14_dlaa_jitter, false, "wwe2k14",
                    "DLAA step 1: sub-pixel jitter of the scene draws (SDK gpu_dlaa_jitter). Session 18");
REXCVAR_DEFINE_BOOL(wwe2k14_dlaa, false, "wwe2k14",
                    "DLAA step 3: NVIDIA DLSS in DLAA mode on the scene colour the post passes sample (jitter + depth "
                    "+ camera motion vectors), through the user's own Streamline runtime (--wwe2k14_dlaa_dir). Session 18");
REXCVAR_DEFINE_STRING(wwe2k14_dlaa_dir, "", "wwe2k14",
                      "DLAA: folder with the Streamline runtime (sl.interposer.dll, sl.common.dll, sl.dlss.dll, "
                      "nvngx_dlss.dll); empty = the exe's folder if sl.interposer.dll is there. Nothing is downloaded");
REXCVAR_DEFINE_BOOL(wwe2k14_dlaa_motion, false, "wwe2k14",
                    "DLAA step 2: camera motion vectors from the scene depth (SDK gpu_dlaa_motion). Session 18");
REXCVAR_DEFINE_STRING(wwe2k14_dlaa_debug, "", "wwe2k14",
                      "DLAA debug view: mv = motion vectors as colour, depth = scene depth as grey (in the scene, "
                      "under the HUD); empty = off");
REXCVAR_DEFINE_DOUBLE(wwe2k14_dlaa_near, 10.0, "wwe2k14",
                      "DLAA: the camera's near plane (match camera: 10, render view +500; docs\\research\\"
                      "dlss_feasibility.md)");
REXCVAR_DEFINE_DOUBLE(wwe2k14_dlaa_far, 300000.0, "wwe2k14", "DLAA: the camera's far plane (match camera: 300000)");
REXCVAR_DEFINE_BOOL(wwe2k14_dlaa_game_near_far, false, "wwe2k14",
                    "DLAA / DLSS (Session 24): the camera's near / far from the game's render view (+500 / +504) "
                    "instead of --wwe2k14_dlaa_near / _far (those stay the fallback); on with --wwe2k14_dlss");
// Session 24: DLSS Super Resolution at the end of the frame (SDK gpu_dlss: the swap's picture goes through DLSS with
// the scene depth, camera motion and jitter; the presenter, screenshots and clips get the output size).
REXCVAR_DEFINE_BOOL(wwe2k14_dlss, false, "wwe2k14",
                    "DLSS Super Resolution (Session 24): render at a lower scale, upscale with NVIDIA DLSS at the end of "
                    "the frame (the user's own Streamline runtime, --wwe2k14_dlaa_dir); --wwe2k14_dlss_mode picks both");
REXCVAR_DEFINE_BOOL(wwe2k14_dlss_objmv, true, "wwe2k14",
                    "DLSS / DLAA (Session 24 rung 2b): motion vectors for the wrestlers and referee (their positions "
                    "this frame and last frame, captured with Vulkan transform feedback). Session 28: on by default "
                    "and only used while DLAA or DLSS is on (the game looks worse without them); =false is a "
                    "debugging switch (camera motion only)");
REXCVAR_DEFINE_BOOL(wwe2k14_dlss_bias, false, "wwe2k14",
                    "DLSS (Session 24 rung 2a): tell DLSS to trust this frame's colour on the wrestlers (their mask "
                    "as its bias-current-colour hint; turns --wwe2k14_dlss_objmv's capture on for the mask)");
REXCVAR_DEFINE_STRING(wwe2k14_dlss_mode, "performance", "wwe2k14",
                      "DLSS mode: performance (720p -> 1440p), quality (1440p -> 4K), ultra_performance (720p -> 4K), "
                      "balanced (720p -> 2207x1241), ultra_quality (1440p -> 3328x1872); sets resolution_scale (if left "
                      "at default) and the jitter length");

namespace wwe2k14::dlaa {

inline bool g_on = false;  // any DLAA feature on: the hand-off feeds the camera
inline std::atomic<uint32_t> g_logged{0};

// Session 24: the DLSS mode -> SDK gpu_dlss_mode, the render scale, the Halton length (NVIDIA: at least
// 8 x (output / render)^2 phases) and the scene's texture LOD bias (log2(render / output) - 0.5, in quarters, at
// most -1.75: measured for performance only, q9 runs 1715 / 1716: -1.5 >= -1 > 0).
struct DlssModeInfo {
  int sdk_mode;
  const char* scale;
  const char* phases;
  const char* lod_bias;
};
inline DlssModeInfo DlssMode() {
  const std::string m = REXCVAR_GET(wwe2k14_dlss_mode);
  if (m == "quality") return {3, "2", "18", "-1"};
  if (m == "ultra_performance") return {4, "1", "72", "-1.75"};
  if (m == "balanced") return {2, "1", "24", "-1.5"};
  if (m == "ultra_quality") return {5, "2", "14", "-1"};
  return {1, "1", "32", "-1.5"};  // performance
}

// Before the presets (OnPreSetup): DLSS picks the render scale (only if resolution_scale was left at default).
inline void ConfigureScale() {
  if (REXCVAR_GET(wwe2k14_dlss) && rex::cvar::GetFlagSource("resolution_scale") == rex::cvar::Source::kDefault)
    rex::cvar::SetFlagByName("resolution_scale", DlssMode().scale);
}

// Before the graphics system starts (OnPreSetup): the SDK settings, each only if left at default.
inline void Configure() {
  const std::string debug = REXCVAR_GET(wwe2k14_dlaa_debug);
  const int debug_mode = debug == "mv" ? 1 : debug == "depth" ? 2 : 0;
  auto set = [](const char* name, const std::string& value) {
    if (rex::cvar::GetFlagSource(name) == rex::cvar::Source::kDefault) rex::cvar::SetFlagByName(name, value);
  };
  if (REXCVAR_GET(wwe2k14_dlss)) {  // Session 24
    const DlssModeInfo mode = DlssMode();
    set("gpu_dlss", "true");
    set("gpu_dlss_mode", std::to_string(mode.sdk_mode));
    set("gpu_dlaa_jitter", "true");
    set("gpu_dlaa_jitter_phases", mode.phases);
    // Session 24 (q7, runs 1701 / 1702): no jitter after frames whose camera moved more than 2 render pixels: in
    // fast camera shots the game blends its previous frame and a changing jitter becomes moire (Bryan's pan 31.3 ->
    // 37.1 dB vs native; heavy shot 33.6 -> 36.4; the match's wrestlers unchanged or better).
    set("gpu_dlaa_jitter_max_camera_px", "2");
    // Session 24: DLSS also takes the game's near / far and treats long camera moves as cuts (DLAA keeps Session
    // 18's inputs; review fix).
    set("wwe2k14_dlaa_game_near_far", "true");
    set("gpu_dlaa_reset_distance", "200");
    // Session 24 (q9, runs 1715 / 1716, vs 4K supersampled): the scene's textures sampled for the output size, as
    // NVIDIA asks: wrestlers 25.9 -> 27.0 dB (SSIM 0.913 -> 0.923), the match 32.7 -> 33.3 (bicubic 32.8).
    set("gpu_dlss_lod_bias", mode.lod_bias);
    // Session 27 (runs 1971-1975): the picture cuts 2-3 frames after the camera; reset the history for 4 frames.
    set("gpu_dlss_reset_hold_frames", "4");
  }
  // Session 28 (Jarrod): the wrestlers' own motion vectors are always on with DLAA or DLSS (Session 24 measured them:
  // they keep a moving wrestler sharp, docs/research/dlss_s24.md); off without either (no cost when the picture is
  // not upscaled or anti-aliased by DLSS).
  const bool objmv = REXCVAR_GET(wwe2k14_dlss_objmv) && (REXCVAR_GET(wwe2k14_dlaa) || REXCVAR_GET(wwe2k14_dlss));
  if (objmv || REXCVAR_GET(wwe2k14_dlss_bias)) {  // Session 24 rung 2
    set("vulkan_transform_feedback", "true");
    set("gpu_dlss_objmv", "true");
    if (!objmv) set("gpu_dlss_objmv_vectors", "false");  // the mask only
    if (REXCVAR_GET(wwe2k14_dlss_bias)) set("gpu_dlss_bias_mask", "true");
  }
  if (REXCVAR_GET(wwe2k14_dlaa) || REXCVAR_GET(wwe2k14_dlss)) {
    if (REXCVAR_GET(wwe2k14_dlaa)) set("gpu_dlaa", "true");
    set("gpu_dlaa_jitter", "true");  // DLAA needs the jitter
    std::string dir = REXCVAR_GET(wwe2k14_dlaa_dir);
    if (dir.empty()) {
      wchar_t exe[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, exe, MAX_PATH);
      const auto here = std::filesystem::path(exe).parent_path();
      std::error_code ec;
      if (std::filesystem::is_regular_file(here / "sl.interposer.dll", ec)) dir = here.string();
    }
    if (dir.empty())
      REXLOG_WARN("[wwe2k14][dlaa] --wwe2k14_dlaa / _dlss: no Streamline runtime (set --wwe2k14_dlaa_dir, or put "
                  "sl.interposer.dll next to the exe): DLAA not available");
    else
      set("vulkan_streamline_dir", dir);
  }
  if (REXCVAR_GET(wwe2k14_dlaa_jitter)) set("gpu_dlaa_jitter", "true");
  if (REXCVAR_GET(wwe2k14_dlaa_motion)) set("gpu_dlaa_motion", "true");
  if (debug_mode) set("gpu_dlaa_debug", std::to_string(debug_mode));
  g_on = REXCVAR_GET(wwe2k14_dlaa) || REXCVAR_GET(wwe2k14_dlss) || REXCVAR_GET(wwe2k14_dlaa_jitter) ||
         REXCVAR_GET(wwe2k14_dlaa_motion) || debug_mode != 0;
  if (REXCVAR_GET(wwe2k14_dlaa) || REXCVAR_GET(wwe2k14_dlss) || REXCVAR_GET(wwe2k14_dlaa_motion) || debug_mode)
    set("vulkan_texture_load_direct", "true");  // storage views
  if (g_on)
    REXLOG_INFO("[wwe2k14][dlaa] dlaa {}, dlss {} (mode {} = gpu_dlss_mode {}, resolution_scale {}, jitter phases {}) "
                "object motion {}, bias mask {} "
                "(runtime '{}'), jitter {}, motion {}, debug '{}' (near {}, far {})",
                REXCVAR_GET(wwe2k14_dlaa), REXCVAR_GET(wwe2k14_dlss), REXCVAR_GET(wwe2k14_dlss_mode),
                rex::cvar::GetFlagByName("gpu_dlss_mode"), rex::cvar::GetFlagByName("resolution_scale"),
                rex::cvar::GetFlagByName("gpu_dlaa_jitter_phases"), objmv,
                REXCVAR_GET(wwe2k14_dlss_bias), rex::cvar::GetFlagByName("vulkan_streamline_dir"),
                rex::cvar::GetFlagByName("gpu_dlaa_jitter"), REXCVAR_GET(wwe2k14_dlaa_motion), debug,
                REXCVAR_GET(wwe2k14_dlaa_near), REXCVAR_GET(wwe2k14_dlaa_far));
}

inline float LoadF32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, rex::memory::GuestPtr(base, a), 4);
  v = _byteswap_ulong(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// The main scene manager's hand-off (src\cinecam.h, after the free camera was applied): the view slot `vb`.
// Session 24: near / far from the render view the hand-off filled (game_near / game_far; the fixed
// --wwe2k14_dlaa_near / _far if they look wrong or --wwe2k14_dlaa_game_near_far=false).
// Review fix: the view, FOV and frame are read before the hand-off (as in Session 18), near / far after it.
struct HandoffView {
  bool valid = false;
  float view[16] = {};
  float fov = 0.0f;
  uint64_t frame = 0;
};
inline HandoffView ReadHandoffView(uint8_t* base, uint32_t vb) {
  HandoffView h;
  if (!g_on || !vb) return h;
  for (int i = 0; i < 16; ++i) h.view[i] = LoadF32(base, vb + 4 * i);
  h.fov = LoadF32(base, vb + 128);
  h.frame = g_guest_frame.load(std::memory_order_relaxed);
  h.valid = true;
  return h;
}
inline void OnHandoff(const HandoffView& h, float game_near, float game_far) {
  if (!g_on || !h.valid) return;
  const float* view = h.view;
  const float fov = h.fov;
  const uint64_t frame = h.frame;
  const bool game_ok = REXCVAR_GET(wwe2k14_dlaa_game_near_far) && game_near > 0.0f && game_far > game_near * 2.0f &&
                       game_far < 1e9f;
  const float near_z = game_ok ? game_near : float(REXCVAR_GET(wwe2k14_dlaa_near));
  const float far_z = game_ok ? game_far : float(REXCVAR_GET(wwe2k14_dlaa_far));
  rex::perf::dlaa::SubmitCamera(frame, view, fov, near_z, far_z);
  // Session 27 item 7: log the camera cuts DLSS treats as a history reset (the SDK's rule, DlaaFillFrame: a turn over
  // gpu_dlaa_reset_degrees, default 15, or a move over gpu_dlaa_reset_distance, 200 with DLSS), from the same view.
  {
    static float prev_fwd[3] = {}, prev_pos[3] = {};
    static uint64_t prev_frame = 0;
    static int cuts_logged = 0;
    float fwd[3], pos[3];
    for (int i = 0; i < 3; ++i) {
      fwd[i] = view[i * 4 + 2];
      pos[i] = -(view[12] * view[i * 4 + 0] + view[13] * view[i * 4 + 1] + view[14] * view[i * 4 + 2]);
    }
    if (prev_frame && frame == prev_frame + 1 && cuts_logged < 400) {
      const float d = std::clamp(fwd[0] * prev_fwd[0] + fwd[1] * prev_fwd[1] + fwd[2] * prev_fwd[2], -1.0f, 1.0f);
      const float turn = std::acos(d) * 57.29578f;
      const float dx = pos[0] - prev_pos[0], dy = pos[1] - prev_pos[1], dz = pos[2] - prev_pos[2];
      const float move = std::sqrt(dx * dx + dy * dy + dz * dz);
      const double max_deg = std::strtod(rex::cvar::GetFlagByName("gpu_dlaa_reset_degrees").c_str(), nullptr);
      const double max_move = std::strtod(rex::cvar::GetFlagByName("gpu_dlaa_reset_distance").c_str(), nullptr);
      static bool told = false;
      if (!told) {
        told = true;
        REXLOG_INFO("[wwe2k14][dlaa] cut thresholds: {} deg, {} units", max_deg, max_move);
      }
      if (turn > max_deg || (max_move > 0.0 && move > max_move)) {
        ++cuts_logged;
        REXLOG_INFO("[wwe2k14][dlaa] camera cut at frame {}: turn {:.1f} deg, move {:.0f} (DLSS history reset)", frame,
                    turn, move);
      }
    }
    std::memcpy(prev_fwd, fwd, sizeof(fwd));
    std::memcpy(prev_pos, pos, sizeof(pos));
    prev_frame = frame;
  }
  static float last_near = -1.0f, last_far = -1.0f;
  const bool changed = near_z != last_near || far_z != last_far;
  last_near = near_z, last_far = far_z;
  if (g_logged.fetch_add(1) < 3 || (changed && g_logged.load() < 200)) {
    if (changed) g_logged.fetch_add(1);
    REXLOG_INFO("[wwe2k14][dlaa] camera for frame {}: fov {:.2f}, position row {:.1f} {:.1f} {:.1f}, near {} far {} "
                "(render view {} / {}{})", frame, fov, view[12], view[13], view[14], near_z, far_z, game_near,
                game_far, game_ok ? "" : ", not used");
  }
}

}  // namespace wwe2k14::dlaa
