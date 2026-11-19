// S103 P9 (docs\renderer\COMPILE_OUT_LIST.md B9): the switches of the command stream, as seen by the code that is kept when WWE2K14_KEEP_STREAM is OFF.
// With the stream kept these cvars are defined in reader.h and live.cpp and this header is empty. Without it they do not exist: the in-place replacers
// (--wwe2k14_rdr_*_ours), the D3 skips (--wwe2k14_rdr_*_d3), the reader's capture and live-mirror switches. A few kept headers (the app's start-up, member_thin_hook.h)
// still READ them; here every one reads as a constant false (an empty string for the folder), so those branches are dead in this build, and nothing can SET one:
// REXCVAR_SET on a const reference does not compile, which is the point. Nothing is registered, so `--wwe2k14_rdr_arena_ours` on a command line is an unknown flag, not a
// switch that quietly does nothing. The three switches the kept code owns (wwe2k14_rdr_owner, wwe2k14_rdr_scene, wwe2k14_scene_records) are declared here and
// defined in owner_cvars_off.cpp.
#pragma once

#include "../keep_stream.h"

#if !WWE2K14_STREAM()
#include <string>

#include <rex/cvar.h>

REXCVAR_DECLARE(bool, wwe2k14_rdr_owner);
REXCVAR_DECLARE(int32_t, wwe2k14_rdr_scene);
REXCVAR_DECLARE(bool, wwe2k14_scene_records);

#define WWE2K14_REMOVED_SWITCH_FALSE(name) \
  inline const bool& FLAGS_##name##_storage_() { static constexpr bool kOff = false; return kOff; }
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_arena_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_caster_d3)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_caster_d3_verify)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_caw_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_circles_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_crowd_floor)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_crowd_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_crowd_props_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_debug_keep_crowd_shortcut)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_flash_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_plane_shadow_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_props_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_rim_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_ring_d3)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_ring_d3_live)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_ring_d3_live_verify)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_ring_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_screens_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_shadow_casters_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_ui_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_ui_text_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_wrestlers_d3)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_wrestlers_d3_verify)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_wrestlers_ours)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_capture)
WWE2K14_REMOVED_SWITCH_FALSE(wwe2k14_rdr_live)
#undef WWE2K14_REMOVED_SWITCH_FALSE
inline const std::string& FLAGS_wwe2k14_rdr_pipeline_store_dir_storage_() { static const std::string kNone; return kNone; }
#endif
