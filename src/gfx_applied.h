// wwe2k14 - S102 lane S0-1: the one place that says what a settings file means at the start of a run, and what a fresh install writes.
//
// Three users share it, so they cannot drift apart:
//   1. the game (gfx_menu.h ApplyLauncherSettings): resolves the quality tier (ResolveLow) and logs the `[gfx][applied]` line (AppliedLine);
//   2. the launcher (launcher_main.cpp SuggestForDevice): the integrated-graphics first-run values (SuggestIntegrated);
//   3. tools\surface\gfx_applied_cli.cpp: writes the Surface bench's "auto" profile (FreshInstall + FreshJson) and prints the same line the game
//      would log for any settings file (so `bench.ps1 -Profile auto -DryRun` and the game's log agree by construction).
#pragma once

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "gfx_settings.h"

namespace wwe2k14::gfxs {

// The launcher's first-run values on integrated graphics (Session 95, Jarrod: "720p the default": true 720p, no upscaler, 30 fps
// everywhere). Pure: the launcher passes its own Settings fields; the bench's profile generator passes a Choice's.
inline void SuggestIntegrated(int& resolution, int& draw_height, int& frame_rate, int& refresh_hz, int& upscale, bool& original_look) {
  resolution = 0, draw_height = 0, frame_rate = 3, refresh_hz = 0, upscale = 0, original_look = false;
}

// The quality tier's resolved flag for one start (was inline in gfx_menu.h): `c.low` set from the setting AUTO / LOW / STANDARD and the
// card; LOW does nothing with the original look or another renderer (then `why` says STANDARD). `renderer_from_flag` is
// RendererFromFlags(...) (-1: no flag).
inline bool ResolveLow(Choice& c, bool phone, bool integrated, const std::string& gpu_name, int renderer_from_flag, std::string* why) {
  std::string text;
  c.low = ResolveQualityTier(c.quality_tier, phone, integrated, gpu_name, &text) == 1;
  if (c.low && (c.original_look || c.renderer != 2 || (renderer_from_flag >= 0 && renderer_from_flag != 2))) {
    c.low = false;
    text = c.original_look ? "STANDARD (original look is on)" : "STANDARD (not the NATIVE renderer)";
  }
  if (why) *why = text;
  return c.low;
}

// What a first run on this kind of card writes (the launcher's setup: SuggestForDevice, then the tier's starting values). `what` gets the
// LowStartValues text. A fresh file is AUTO (quality_tier 0), NATIVE, TRUE NATIVE, settings version current.
inline Choice FreshInstall(bool integrated, const std::string& gpu_name, std::string* what = nullptr, int quality_tier = 0) {
  Choice c;
  c.renderer = 2;
  c.true_native = true;
  c.quality_tier = quality_tier;  // 0 AUTO (a fresh file); 2 STANDARD is the bench's standard_player profile (same rule, the player picked STANDARD)
  if (integrated) SuggestIntegrated(c.resolution, c.draw_height, c.frame_rate, c.refresh_hz, c.upscale, c.original_look);
  std::string why;
  if (ResolveLow(c, false, integrated, gpu_name, -1, &why)) {
    std::string moved;
    LowStartValues(c.resolution, c.draw_height, c.upscale, c.crowd_density, &moved);
    if (what) *what = why + ":" + moved;
  } else if (what) {
    *what = why;
  }
  return c;
}

// The settings file text for a Choice: the keys Parse reads, nothing else (a test file; the launcher's own file has more keys).
inline std::string FreshJson(const Choice& c) {
  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "{\n  \"renderer\": %d,\n  \"frame_rate\": %d,\n  \"resolution\": %d,\n  \"draw_height\": %d,\n  \"upscale\": %d,\n"
                "  \"quality_tier\": %d,\n  \"crowd_density\": %d,\n  \"true_native\": %s,\n  \"settings_version\": %d\n}\n",
                c.renderer, c.frame_rate, c.resolution, c.draw_height, c.upscale, c.quality_tier, c.crowd_density,
                c.true_native ? "true" : "false", kSettingsVersion);
  return buf;
}

// LOW's levers and whether the new renderer reads each one (R2 / R1 pages of part 2; the grep that decides it is the lane's gate check).
// A lever "asked but ignored" is a flag the settings ask for that nothing in src\renderer*, src\renderer_adapter, src\scene_live reads.
struct LowLever {
  const char* flag;
  bool read_by_new_renderer;
  const char* where;  // the read, or why it is ignored
};
inline constexpr LowLever kLowLevers[] = {
    {"gpu_shadow_half_rate", true, "renderer_adapter\\reader.h options.shadow_half"},
    {"present_effect", true, "renderer_adapter\\reader.h options.present_sgsr"},
    // S102 G1: three levers the new renderer reads under LOW (low_levers.h; switches default off until their Surface rows; GraphicsFlags writes them when flipped).
    {"wwe2k14_rdr_low_shadow_small", true, "renderer_adapter\\reader.h low_levers::ShadowMapSize (needs wwe2k14_rdr_shadows_ours)"},
    {"wwe2k14_rdr_low_present_noclear", true, "renderer_adapter\\reader.h options.present_noclear -> renderer.cpp RecordPresent"},
    {"wwe2k14_rdr_low_no_fullsize_copies", true, "renderer_adapter\\reader.h low_levers::CopyAliasFor (copy_alias mode 2, classes 1|2|8)"},
    // S102 G3 (map_turns.h): the offscreen maps take turns, one PerFrame switch each (rdr_switches.h), read by live.cpp every frame and effective under LOW only; nothing in GraphicsFlags writes them yet (default off until a Surface row).
    {"wwe2k14_rdr_turn_wrestler", true, "renderer_adapter\\live.cpp map_turns::g_on (per frame, LOW only) -> binder.h TurnsBegin"},
    {"wwe2k14_rdr_turn_rim", true, "renderer_adapter\\live.cpp map_turns::g_on (per frame, LOW only) -> binder.h TurnsBegin"},
    {"wwe2k14_rdr_turn_props", true, "renderer_adapter\\live.cpp map_turns::g_on (per frame, LOW only) -> binder.h TurnsBegin"},
    {"wwe2k14_rdr_half_rate_drawn_clock", true, "renderer_adapter\\live.cpp map_turns::g_half_rate_drawn_clock (per frame) -> binder.h shadow_skip_frame_"},
    // S102 G2: the output pass's two levers (PerFrame; output_switches.h).
    {"wwe2k14_rdr_low_output_cap", true, "renderer_adapter\\reader.h rdr_switches.h LowOutputCap (PerFrame) -> live.cpp low_levers::OutputCapHeight -> Frame::out_cap_h -> renderer.cpp RecordPresent / swapchain.cpp ApplyOutputCap"},
    {"wwe2k14_rdr_low_present_bilinear", true, "renderer_adapter\\reader.h rdr_switches.h LowPresentBilinear (PerFrame) -> live.cpp low_levers::PresentBilinear -> Frame::out_bilinear -> renderer.cpp RecordPresent"},
    {"wwe2k14_rdr_present_fetch", true, "renderer_adapter\\reader.h rdr_switches.h PresentFetch (PerFrame) -> live.cpp -> FrameBuilder::SetPresentFetch -> Frame::present_fetch -> renderer.cpp TryPresentFetch / swapchain.cpp RecordPictureCopy"},
    // S102 C4: the arena overdraw (no look change) and the half-size transparents (a look lever, LOW only).
    {"wwe2k14_rdr_arena_fragments", true, "renderer_adapter\\reader.h rdr_switches.h ArenaFragments (PerFrame bit mask) -> live.cpp -> FrameBuilder::SetArenaFragments -> binder.h AlphaOpaque (exact proof) -> DrawOp::kDrawOpBlendOff -> renderer.cpp RecordPass (arena_fragments.h)"},
    {"wwe2k14_rdr_arena_overdraw", true, "renderer_adapter\\reader.h rdr_switches.h ArenaOverdraw (PerFrame) -> live.cpp -> FrameBuilder::SetArenaOverdraw -> binder.h AlphaOpaque (texture_alpha.h) -> renderer.cpp RecordPass (the G10 prepass)"},
    {"wwe2k14_rdr_low_half_transparents", true, "renderer_adapter\\reader.h rdr_switches.h LowHalfTransparents (PerFrame) -> live.cpp -> Frame::half_transparents -> renderer.cpp RecordPass (half_trans.h)"},
    // S102 G10: the scene colour pass's cuts (PerFrame bit mask; colour_cuts.h).
    {"wwe2k14_rdr_low_colour_cuts", true, "renderer_adapter\\reader.h rdr_switches.h LowColourCuts (PerFrame) -> live.cpp low_levers::ColourCuts -> Frame::colour_cuts -> renderer.cpp RecordPass (depth prepass)"},
    {"gpu_native_owned_shadow_half", false, "read by the SDK (the old native path), not by src\\renderer*; the new renderer's map size is wwe2k14_rdr_shadow_map_size"},
    {"gpu_native_owned_shadow_skip", false, "no read in the new renderer"},
    {"gpu_native_owned_ring_half", false, "no read in the new renderer"},
    {"gpu_native_crowd_pose_half", false, "no read in the new renderer"},
    {"gpu_native_owned_strip_half", false, "no read in the new renderer"},
    {"gpu_native_half_phase", false, "no read in the new renderer"},
};

// The internal (drawn) size of a settings choice: resolution_scale 1 / 2 / 3 = 1280x720 / 2560x1440 / 3840x2160; a NATIVE draw height
// gives its width at 16:9.
inline std::pair<int, int> InternalSize(const Choice& c) {
  static constexpr int kW[] = {1280, 2560, 3840}, kH[] = {720, 1440, 2160};
  const int r = std::clamp(c.resolution, 0, 2);
  const int dh = ValidDrawHeight(c.resolution, c.draw_height);
  if (c.upscale == 0 && !c.original_look && dh) return {(dh * 1280 + 719) / 720, dh};  // S102 G13: the width the renderer makes (ceil(1280 * dh / 720): 600 -> 1067)
  return {kW[r], kH[r]};
}

// The one line. `flags` is GraphicsFlags(c, ...) (what the settings ask for); `swapchain_w/h` 0 = not known yet (the window's size, said by
// the present's own line `[gfx][present]`). Deterministic: no clock, no machine facts beyond the arguments.
inline std::string AppliedLine(const Choice& c, const std::vector<std::pair<std::string, std::string>>& flags, const std::string& tier_why,
                               int swapchain_w, int swapchain_h) {
  const auto size = InternalSize(c);
  std::string on, ignored;
  for (const LowLever& l : kLowLevers) {
    for (const auto& [name, value] : flags) {
      if (name != l.flag) continue;
      std::string item = std::string(l.flag) + (name == "present_effect" ? "=" + value : std::string());
      std::string& list = l.read_by_new_renderer ? on : ignored;
      list += (list.empty() ? "" : ", ") + item;
    }
  }
  const int crowd = RoundCrowdDensity(c.crowd_density);
  char buf[1400];
  std::string swap = swapchain_w > 0 ? std::to_string(swapchain_w) + "x" + std::to_string(swapchain_h) : std::string("window");
  std::snprintf(buf, sizeof(buf),
                "[gfx][applied] tier=%s render_scale=%d internal=%dx%d swapchain=%s crowd=%d%% frame_rate=%d true_native=%d levers on: %s; "
                "ignored (asked, new renderer does not read): %s",
                tier_why.c_str(), c.resolution + 1, size.first, size.second, swap.c_str(), crowd, c.frame_rate, c.true_native ? 1 : 0,
                on.empty() ? "none" : on.c_str(), ignored.empty() ? "none" : ignored.c_str());
  return buf;
}

}  // namespace wwe2k14::gfxs
