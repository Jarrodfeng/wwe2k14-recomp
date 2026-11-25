// S110 CVARSHARD: the defaults of the cvars whose default is a name, not a literal. The generated shards (src\cvars\cvars_<n>.cpp, tools\compile\cvar_shard.py) include this header, so a
// definition whose default is one of these names moves with its text; the tool refuses any other non-literal default (it lists it, and --check fails).
// Pure: no state, no game headers (dev_build.h only declares IsDevBuild()). Never include it from a header that a unit of the game target shares with another: the names are plain constants.
#pragma once
#include <cstdint>

#include "../dev_build.h"   // wwe2k14::IsDevBuild() (defined in dev_build.cpp, the one unit that gets the WWE2K14_VERSION define)

// Session 100 (Jarrod 2026-11-11 07:2x: "we're probably due a new phone build anyway it's not seen the new owners renderer"): phone builds
// of this branch start in owner mode, PC builds do not (yet). Not a player default: an Android release decides it again. (wwe2k14_rdr_owner)
// The case ring's byte cap in MB (wwe2k14_case_mb): 192 on phones. (src\case\case_hook.h; defined in cvars_hand.cpp)
// S105 ADEV: Android has no settings file (the phone launcher passes its choices as flags), so the default is off there even in a release
// (a release APK is !IsDevBuild(); an args.txt run would otherwise read a file that does not exist). (wwe2k14_use_launcher_settings)
#if defined(__ANDROID__)
inline constexpr bool kRdrOwnerDefault = true;
inline constexpr int32_t kCaseMbDefault = 192;  // phones
#define WWE2K14_USE_LAUNCHER_SETTINGS_DEFAULT false
#else
inline constexpr bool kRdrOwnerDefault = false;
inline constexpr int32_t kCaseMbDefault = 512;
#define WWE2K14_USE_LAUNCHER_SETTINGS_DEFAULT (!wwe2k14::IsDevBuild())
#endif

// The default of a native function (src\native_fn.h, src\nf\*.h: wwe2k14_nf_<name>): verified with 0 mismatches over a launched match and both entrances (twice) and faster in
// an in-run A/B: on where it was verified (x86-64: runs 4187 / 4197; A/B 4259: main game thread -11.5% CPU, skinning
// thread -16.2%), off elsewhere until a run there verifies it (Android: the FP mode handling differs).
#if 0  // Session 66 02:15: off again - with them on the determinism replay diverged (check_item nf_on, 4269 / 4270)
#define WWE2K14_NF_VERIFIED_DEFAULT "on"
#else
#define WWE2K14_NF_VERIFIED_DEFAULT "off"
#endif
