// S103 SPB (docs\research\s103\SP2_start_parity.md section 8): the settings EVERY start path applies, in one place. There are two start paths (the stream build: reader.h StartLive + live.cpp
// LiveSink::Start; the stream-free build: renderer_host\host_start.cpp StartForGame) and a setting that one of them forgot was invisible to every offline check (tonight's presence switches: bits
// 10 / 11 never stamped in the stream-free build). ApplyStartSettings() is called by BOTH starts; it reads the player's flags once, stores the scene-input globals, fills PlayerQuality (the
// crowd / shadow / DoF / film-grain / LOW levers: the stream binder AND the stream-free scene draw both read it), and hands back the renderer-option half (gpu timestamps, counts, robust
// buffers, exclusive fullscreen, present clear, ultrawide fill) as one struct the two option builders copy from. Whatever a build cannot do is named with RefuseSetting().
// tools\scene\start_parity_lint.py (manifest tools\scene\start_paths.txt) fails a store a start path makes that the other start path does not.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The implementation is start_settings.cpp, a KEPT unit (built with and without the command stream). The cvars this needs that only the stream's files defined are defined there for a build without the
// stream (`#if !WWE2K14_STREAM()`: the stream build keeps its definitions in reader.h / live.cpp; start_parity_lint rule R8 checks the two copies agree).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "keep_stream.h"

namespace wwe2k14::start {

// What the player's quality settings ask of the picture. One struct, read by the stream binder (binder.h g_crowd_density ... are references into g_quality) and by the stream-free scene draw
// (post_from_record.cpp: DoF / film; live_map_shadow.cpp: shadows / half rate; live_crowd.cpp: density + rank). Written once at start, before any reader thread exists; the tools (fview, rplay) write it
// from their own flags. Defaults = the launcher's defaults (everything on, full crowd).
struct PlayerQuality {
  int crowd_density = 100;        // CROWD DENSITY, percent of the members drawn (--gpu_scene_crowd_density)
  bool density_track = true;      // the density rank follows a member that moves (crowd_rank.h; --wwe2k14_rdr_density_track)
  bool shadows = true;            // SHADOWS: false = the dynamic casters are not drawn, the maps stay lit (--gpu_dynamic_shadows)
  bool depth_of_field = true;     // DEPTH OF FIELD: false = the post combine without it (--gpu_depth_of_field)
  bool film_grain = true;         // VINTAGE LOOK (key kept: film_grain): false = the retro arenas drawn clean: sepia + the VTGE darkening / grain / vignette (vintage_look.h) (--wwe2k14_film_grain)
  bool shadow_half = false;       // the LOW tier's lever: the shadow maps every other frame (--gpu_shadow_half_rate)
  bool low_tier = false;          // the resolved tier is LOW (low_levers.h IsLow)
};
inline PlayerQuality g_quality;
inline const PlayerQuality& Quality() { return g_quality; }

// The result of ApplyStartSettings: what was applied, for the start log, the stream's LiveOptions and the stream-free HostOptions.
struct Applied {
  bool done = false;
  bool dev = false;                // IsDevBuild(), computed once
  PlayerQuality quality;
  // the renderer-option half (rdr::RendererOptions fields), identical for both builds
  bool gpu_timestamps = true, counts = false, robust_buffers = false, exclusive_fullscreen = false;
  bool gpu_stamps = false, gpu_pass_stamps = false;   // S103 GT: the stream-free build's per-pass GPU stamps (stored into rdr::g_gpu_stamps / g_gpu_pass_stamps by ApplyStartSettings; the stream build keeps its per-frame table)
  bool present_noclear = false;    // LOW's present lever (only under LOW)
  bool present_direct = false;     // S103 CHS C6 (stream-free build only): a frame with no 2D row presents the 3D picture itself, no copy into F (wwe2k14_rdr_present_direct)
  bool present_fill = false;       // ultrawide / taller screen: the picture is stretched to the window (the scene is drawn squeezed by wide / tall)
  float wide = 1.0f, tall = 1.0f;  // the screen-shape factors (1 = 16:9)
  // the scene-input block
  std::string hold_list, hold_bad;
  uint32_t hold_mask = 0;
  std::string unrequire_list, unrequire_bad;
  uint32_t unrequire_applied = 0;
  bool force_inputs = false;
  uint32_t sentinel_every = 0;
  bool flat = false, caw = false, refresh_default = true;
  // settings the player (or a route) asked for that this build cannot do (RefuseSetting)
  std::vector<std::string> refused;
  std::string line;                // the `[start] settings:` line
};

// Reads every flag and applies it. Idempotent (the stream build calls it from StartLive and again from LiveSink::Start; the second call stores the same values and logs nothing).
const Applied& ApplyStartSettings();
// LiveSink::Stop calls this: the stream build can start again after a stop or a capture, and that start reads the flags again (the refused list is kept: a refusal is logged once per run).
void RearmStartSettings();
const Applied& Current();                 // the last result (ApplyStartSettings() first when it matters)
inline bool Dev() { return Current().dev; }

// A setting this build does not do: one `[start] WARN setting 'name' is not in this build: why` line, once per name, and the name in Applied::refused (P7_DESIGN.md 3.6).
void RefuseSetting(const char* name, const char* why);

}  // namespace wwe2k14::start
