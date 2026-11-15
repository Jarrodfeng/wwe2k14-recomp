// S102 lane G1: LOW's missing levers, the decisions as pure functions (no cvar, no Vulkan) so tools\g1\low_levers_test.cpp can prove each one
// with a planted twin. reader.h reads the switches and the SDK flags and calls these; nothing else decides.
//
// "Under LOW" is the RESOLVED tier: both launchers write --wwe2k14_quality_note=LOW_* / STANDARD_* (launcher_core.h, mobile_settings.h) and a direct
// settings-file start sets it too (gfx_menu.h ApplyLauncherSettings, from ResolveLow's c.low). Only when there is no note at all (a bare command line) the
// LOW tier's own flag gpu_native_crowd_pose_half is the fallback. A hand-passed --gpu_shadow_half_rate on a STANDARD start is NOT LOW.
//
// Switches (all default off; the main session turns them on under LOW only after the Surface row):
//   wwe2k14_rdr_low_shadow_small         the shadow maps 256 texels instead of 512 (needs wwe2k14_rdr_shadows_ours: the maps are images of ours)
//   wwe2k14_rdr_low_present_noclear      the present clears only the bars around the picture (renderer.h present_noclear)
//   wwe2k14_rdr_low_no_fullsize_copies   resolve copies nothing needs are dropped: copy_alias mode 2, classes bake | shadow | post (copy_alias.h)
//   wwe2k14_rdr_low_output_cap           S102 G2, PerFrame: N > 0 = under LOW the present is drawn at most N pixels tall and a bilinear blit stretches it over the panel
//   wwe2k14_rdr_low_present_bilinear     S102 G2, PerFrame: under LOW the upscaling present is a plain bilinear sample, not SGSR (output_switches.h)
//   wwe2k14_rdr_low_colour_cuts          S102 G10, PerFrame: a bit mask, under LOW the scene colour pass's depth prepass of the opaque light-mapped arena draws (1) (colour_cuts.h)
//   (wwe2k14_rdr_low_skin_once: NOT built; docs\renderer\LOW_LEVERS.md says why)
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace rdr_adapter::low_levers {

// flag(name): the SDK flag's text, empty when unset (reader.h: rex::cvar::GetFlagByName).
using FlagReader = std::function<std::string(const char*)>;

inline bool IsLow(const FlagReader& flag) {
  const std::string note = flag("wwe2k14_quality_note");
  if (!note.empty()) return note.rfind("LOW_", 0) == 0;
  return flag("gpu_native_crowd_pose_half") == "true";
}

inline constexpr uint32_t kLowShadowMapSize = 256;  // the 256 low-spec value shadows_ours.h names; the 360's is 512

// The shadow map edge to use. The lever only lowers the size, and only where the maps are ours (shadows_ours): the stream's maps keep their size.
inline uint32_t ShadowMapSize(bool lever, bool low, bool shadows_ours, uint32_t asked) {
  if (lever && low && shadows_ours && asked > kLowShadowMapSize) return kLowShadowMapSize;
  return asked;
}

inline bool PresentNoClear(bool lever, bool low) { return lever && low; }

// S102 G2 (b): the output cap's height, 0 = off. Only under LOW, only with a sensible ask (360 to 8192: a smaller cap would draw the picture smaller than its own 720p source).
inline uint32_t OutputCapHeight(int value, bool low) { return low && value >= 360 && value <= 8192 ? uint32_t(value) : 0u; }
// S102 G2 (c): SGSR off under LOW.
inline bool PresentBilinear(bool lever, bool low) { return lever && low; }

// S102 G10: the scene colour pass's cuts (colour_cuts.h Bits), only under LOW; the mask is clamped to the bits that exist (1).
inline uint32_t ColourCuts(int value, bool low) {
  if (!low || value <= 0) return 0u;
  uint32_t mask = uint32_t(value) & 1u;
  return mask;
}

inline constexpr uint32_t kLowCopyClasses = 1u | 2u | 8u;  // crowd bake, shadow, post

struct CopyAlias { int mode; uint32_t classes; };
// copy_alias: LOW with the lever raises mode 0 / 1 to 2 (drop) and the classes to bake | shadow | post (1|2|8 = 11); a mode the player asked for higher is kept.
// Never bit 16 (scene colour) or 32 (other): lane 190's notes forbid them until NoteReadback is counted live (SKIP_GAME_WORK_MAP 14.7 S2). The player's own classes are
// kept as they are when they ask for more (their mask is OR-ed in, minus nothing).
// Class 1 (the crowd bake) is exclusive with --wwe2k14_rdr_crowd_bake_skip=2 (reader.h turns it off then, as for the plain switch).
inline CopyAlias CopyAliasFor(bool lever, bool low, int asked_mode, uint32_t asked_classes) {
  if (!(lever && low)) return {asked_mode, asked_classes};
  return {asked_mode < 2 ? 2 : asked_mode, kLowCopyClasses | (asked_mode >= 2 ? asked_classes : 0u)};  // a player who asked for drop mode keeps their own classes too
}

}  // namespace rdr_adapter::low_levers
