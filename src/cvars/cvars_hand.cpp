// S110 CVARSHARD: the case ring's five cvars, DEFINED by hand. They used to be defined in src\case\case_hook.h under `#ifdef WWE2K14_CASE_CVARS` (bookmark.h defined the macro: "defined here, once"; the
// standalone builds, case_selftest and the phone's case code, include case_hook.h without the SDK and never saw them). The generator (tools\compile\cvar_shard.py) would have copied that
// macro into a shard, where nothing defines it, so these five are not generated: the shards are always built into the game target, which is the one unit that defined them.
// case_hook.h keeps `#ifdef WWE2K14_CASE_CVARS` for the declarations and ConfigFromCvars (the includer that reads them: bookmark.h).
// Every other cvar of the headers is in src\cvars\cvars_<n>.cpp (generated: do not edit those).
#include <cstdint>
#include <string>

#include <rex/cvar.h>

#include "cvar_defaults.h"   // kCaseMbDefault (192 on phones, 512 elsewhere)

REXCVAR_DEFINE_BOOL(wwe2k14_case, false, "wwe2k14", "Session 101: keep the last seconds in memory for the bug button (case zip). Off by default; on by itself for a +dev PC build with --wwe2k14_rdr_owner");
REXCVAR_DEFINE_BOOL(wwe2k14_case_auto, false, "wwe2k14", "Session 101: an automatic hitch bookmark also writes a case zip (needs the case ring on). Default off");
REXCVAR_DEFINE_DOUBLE(wwe2k14_case_window_s, 5.0, "wwe2k14", "Session 101: seconds of frames the case ring keeps");
REXCVAR_DEFINE_INT32(wwe2k14_case_mb, kCaseMbDefault, "wwe2k14", "Session 101: the case ring's byte cap in MB (phones: 192)");
REXCVAR_DEFINE_STRING(wwe2k14_case_dir, "", "wwe2k14", "Session 101: where case zips go (default: out\\cases in a +dev build, cases\\ beside the exe otherwise; phone: the app's files folder cases\\)");

// S110 ENTRWAIT2: declared by src\scene_start_wait_hooks.h; defined here by hand (a landing applies a note's edits and cannot run tools\compile\cvar_shard.py, which would move a header definition).
REXCVAR_DEFINE_INT32(wwe2k14_scene_start_wait, -1, "wwe2k14",
                     "S110 ENTRWAIT2: the game's scene start (an entrance, a match with no entrance) waits behind its loading card until our scene's loads are done, so its own fade "
                     "in runs over a drawn scene. -1 = on in normal play, off in replay / det mode and in input-script runs; 1 = on (a check run); 0 = off")
    .range(-1, 1);
