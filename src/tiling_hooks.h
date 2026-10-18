// wwe2k14 - the 3D scene's tile layout (predicated tiling): log what the game builds, and optionally make it build the
// scene without MSAA, which is one tile instead of four strips. Session 7, task NOMSAA;
// docs/research/predicated_tiling.md option (e) and section 4 "Track E". Hooks: config/s7_tiling_hooks.toml.
//
//   --wwe2k14_force_no_msaa=true   build the scene targets without MSAA (1 tile, no predicated tiling). Default off.
//   Always on (log only): one "[wwe2k14][tiling]" line whenever a scene-target build differs from the previous build
//   of the same scene object (plus a line at the 10th, 100th, ... identical rebuild in a row).
//
// How the engine builds its scene targets (static reading of the generated code, Session 7; addresses only):
// - sub_82C22AF8(scene object, MSAA type, layout flag, render device) releases and rebuilds one scene object's colour
//   and depth targets. Four callers (return address -> caller):
//     0x82BA8360  sub_82BA8018, device creation: type 0, flag 1 ("no scene targets yet", see below)
//     0x82BA7DA4  sub_82BA7C18, device reset: render device + 320 / + 324
//     0x82BA8698  sub_82BA84E8 (frame end): render device + 320 / + 324, only when scene object + 4 / + 8 (what it was
//                 last built for) differ from them, i.e. after the settings render command 75 (sub_82BB6EE0) changed
//                 them
//     0x82BB6F70  sub_82BB6F00 = render command 76 (dispatcher sub_82BB53D0): literal type 2 / flag 1, after
//                 sub_82C229F8 wrote a custom layout at scene object + 296 (width 736, height 1280 as the engine names
//                 them; 4 horizontal bands of 320 rows; type 2). Only sub_8341B580 issues command 76 (through
//                 sub_82BB71F0), for a second scene object it creates (not render device + 316). sub_8341BF58 opens and
//                 closes that object with commands 77 / 78 (sub_82BB6F88 / sub_82BB6FA8, tail calls to the same
//                 open/close functions sub_82C224A0 / sub_82C22650), so it tiles through the same engine path. What it
//                 renders is unconfirmed.
// - In sub_82C22AF8: when scene object + 296 and + 300 are both > 0 (custom layout), type/flag become 2/1 and the size
//   comes from the custom layout. sub_82C222D0 then fills the 256-byte layout at scene object + 12 (width, height, MSAA
//   type, tile count, then one 16-byte rectangle x1,y1,x2,y2 per tile): a copy of the custom layout (type ignored), a
//   table entry matching (width, height, type), or, if none matches, 1 tile covering the frame with the requested
//   type. No table entry has type 0.
// - Then, only if the layout's MSAA type is non-zero, it creates the colour target (scene object + 272, 32-bit) and
//   the depth/stencil target (+ 276, placed right after the colour target in EDRAM) with that MSAA type. sub_82CA88E0
//   -> sub_82CA82F0 doubles the sample rows for type >= 1 and also the columns for type 2, so 0/1/2 = 1x/2x/4x.
//   With type 0 there are no scene targets at all (the open-scene function sub_82C224A0 would bind null targets):
//   type 0 is the engine's "not set up yet" state at device creation, not a usable "no MSAA" mode.
// - Last, it stores the requested type/flag at scene object + 4 / + 8.
//
// Why not just force the type to 0 where it's chosen (entry r4, or r26 at 0x82C22BA4, as the study's Track E said):
//   (1) the custom-layout branch ignores the type; (2) the creation gate would skip the targets, leaving the scene with
//   null targets; (3) scene object + 4 would become 0, so sub_82BA84E8's compare (object + 4 vs render device + 320)
//   would fail on every pass and rebuild the targets every time. Instead:
//   1. wwe2k14_tiling_layout_chosen (0x82C22BCC, right after the layout lookup, where all three layout paths meet): if
//      the setting is on and the game picked a non-zero type, rewrite the layout the way the engine's own 1-tile
//      fallback writes it, with type 0: same width and height, 1 tile, tile 0 = (0, 0, width, height). Scene object
//      + 4 / + 8 keep the requested values, so the game's settings-change compare stays quiet.
//   2. wwe2k14_tiling_create_gate (0x82C22BE4, the compare of the layout type against zero): for a build we forced,
//      report non-zero so the targets are created; both creation calls read the layout type again and get 0 (1x).
//   3. wwe2k14_tiling_scene_built (0x82C22C58, after creation, reached on both paths): log only.
//   4. wwe2k14_tiling_rebuild_check (0x82BA8674, sub_82BA84E8's own settings-change compare): when the setting changed
//      since that scene object's last build and its scene is not open (scene object + 288 == 0), make the compare fail
//      so the game rebuilds through its own path. Without it a change would only apply at the game's next own rebuild.
//
// When it takes effect:
//   - Set at startup: from the first real build (the one after the game's settings command; device creation builds
//     type 0, which is left alone). Covers both the main scene object and the custom-layout object.
//   - Changed at runtime (e.g. an input-script cvar step): the main scene object (render device + 316) is rebuilt at
//     the next frame end. sub_82BA84E8 is the frame-end function (called from the render loop sub_82BB5CC0): it closes
//     and presents a pending frame (sub_82C227E8 clears scene object + 288), sets the presentation interval, then runs
//     the settings compare the hook uses. So the next frame's scene uses the new layout; never mid-frame. The
//     custom-layout object is only rebuilt when command 76 is sent again (only sub_8341B580 sends it; how often is
//     unconfirmed), so set the cvar at startup when that object matters.
//
// EDRAM guard: 1x colour + depth must fit the 10 MiB EDRAM (2048 tiles; one tile = 80 x 16 pixels at 32 bits, and both
// targets are 32-bit: formats 0x18280186 and 0x1A220197, texture formats 8_8_8_8 and 24_8_FLOAT). 1280x720 needs
// 2 x 720 = 1440 tiles; the custom-layout object (736 x 1280) needs 2 x 800 = 1600. A layout that would not fit is left
// as the game chose it and logged as "too big for EDRAM". (The creation calls pass an explicit EDRAM base, so the
// engine's own 2048-tile check in sub_82CA88E0 is skipped; this guard stands in for it.)
//
// Untested at runtime (static reading only). What to watch when it is first tried:
//   - the picture loses 4x MSAA (jagged edges). Alpha-to-coverage is not a concern in the captured scenes: no draw in
//     the tiled scene passes of run 110 (CH103, Brock, match) has RB_COLORCONTROL alpha_to_mask_enable set;
//   - the engine's 1-tile path is real code (open: plain clear sub_82CB52B8 instead of BeginTiling; close: plain colour
//     resolve instead of EndTiling), but every table entry at 1280x720 tiles, so it probably never ran with scene
//     targets on a console (inferred). Its close still runs the per-tile depth-resolve loop once, under
//     SetPredication(3) with no tiling bracket open (the bin select is all-ones there, so the resolve passes), then
//     SetPredication(0);
//   - the scene's EDRAM layout changes (depth base 736 -> 720 tiles at 1280x720). That is the same layout as the
//     1280-wide 1x pass after the scene (RB_SURFACE_INFO 0x14000500, RB_DEPTH_INFO 0x000102D0 in the Session 6
//     anatomy), which now finds the scene's own colour/depth in EDRAM instead of the last strip's 4x data;
//   - the other 4x surfaces in the frame (pitches 640, 280, 160 and 80 in the anatomy; the pitch-640 one also puts its
//     depth at EDRAM tile 720) are built elsewhere and stay 4x;
//   - the custom-layout object is forced too (its own targets and resolve texture); what it renders is unconfirmed;
//   - a runtime toggle recreates the targets once; ReXGlue's render-target cache may show one odd frame.

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <string>

REXCVAR_DEFINE_BOOL(wwe2k14_force_no_msaa, false, "wwe2k14",
                    "Build the 3D scene without MSAA: 1 tile instead of 4 predicated-tiling strips (less GPU command "
                    "processor work; loses the game's 4x MSAA). Applies at the next scene-target rebuild (main scene: "
                    "next frame)");

namespace wwe2k14::tiling {

// Scene object fields (constructor sub_82C22398; sub_82C222D0, sub_82C22AF8, sub_82C224A0, sub_82C227E8, sub_82C229F8).
inline constexpr uint32_t kBuiltType = 4;      // requested MSAA type the targets were last built for
inline constexpr uint32_t kLayoutWidth = 12;   // the 256-byte layout starts here
inline constexpr uint32_t kLayoutHeight = 16;
inline constexpr uint32_t kLayoutType = 20;    // MSAA type: 0/1/2 = 1x/2x/4x
inline constexpr uint32_t kLayoutTiles = 24;
inline constexpr uint32_t kLayoutTile0 = 28;   // x1, y1, x2, y2 (4 words)
inline constexpr uint32_t kColorTarget = 272;
inline constexpr uint32_t kDepthTarget = 276;
inline constexpr uint32_t kSceneOpen = 288;    // 1 from open (sub_82C224A0) until end of frame (sub_82C227E8)
inline constexpr uint32_t kCustomWidth = 296;  // custom layout (sub_82C229F8), used when width and height are > 0
inline constexpr uint32_t kCustomHeight = 300;
// sub_82C22AF8 allocates a 192-byte frame; its prologue (__savegprlr_24) saved the return address at entry r1 - 8.
inline constexpr uint32_t kSavedReturnFromR1 = 192 - 8;
inline constexpr uint64_t kEdramTiles = 2048;  // 10 MiB / 5120-byte tiles

inline uint32_t Load(uint32_t address) {
  return rex::memory::load_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address));
}

inline void Store(uint32_t address, uint32_t value) {
  rex::memory::store_and_swap<uint32_t>(REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(address), value);
}

inline const char* CallerName(uint32_t return_address) {
  switch (return_address) {
    case 0x82BA8360: return "device creation (sub_82BA8018)";
    case 0x82BA7DA4: return "device reset (sub_82BA7C18)";
    case 0x82BA8698: return "settings check (sub_82BA84E8)";
    case 0x82BB6F70: return "custom-layout command (sub_82BB6F00)";
    default: return "unknown caller";
  }
}

inline const char* MsaaName(uint32_t type) {
  switch (type) {
    case 0: return "1x";
    case 1: return "2x";
    case 2: return "4x";
    default: return "?";
  }
}

inline bool FitsEdramWithoutMsaa(uint32_t width, uint32_t height) {
  const uint64_t tiles_per_target = (uint64_t(width) + 79) / 80 * ((uint64_t(height) + 15) / 16);
  return 2 * tiles_per_target <= kEdramTiles;
}

enum class Force : uint8_t { kOff, kApplied, kAlreadyNoMsaa, kBadSize, kTooBigForEdram };

inline const char* ForceName(Force force) {
  switch (force) {
    case Force::kOff: return "off";
    case Force::kApplied: return "applied (1 tile, 1x)";
    case Force::kAlreadyNoMsaa: return "on, nothing to do (game picked type 0)";
    case Force::kBadSize: return "on, NOT applied (layout size not positive)";
    case Force::kTooBigForEdram: return "on, NOT applied (1x colour + depth too big for EDRAM)";
  }
  return "?";
}

// The build in progress on this thread. The three sub_82C22AF8 hooks run in order inside one call, on one thread.
struct Build {
  uint32_t object = 0;
  uint32_t caller = 0;
  uint32_t game_type = 0;
  uint32_t game_tiles = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  std::array<uint32_t, 4> game_tile0{};
  bool custom = false;
  Force force = Force::kOff;
};
inline thread_local Build t_build;

struct ObjectState {
  uint32_t object = 0;
  int built_with_force = -1;  // setting value at this object's last build; -1 = not built yet
  std::string last_key;
  uint64_t identical = 0;     // identical rebuilds since last_key was logged
  uint64_t builds = 0;
};
inline std::mutex g_mutex;
inline std::array<ObjectState, 8> g_objects;  // a handful of scene objects at most (2 seen statically)

// Call with g_mutex held. nullptr if the object is unknown (and create is false) or the table is full.
inline ObjectState* FindObject(uint32_t object, bool create) {
  for (auto& state : g_objects) {
    if (state.object == object) return &state;
  }
  if (!create) return nullptr;
  for (auto& state : g_objects) {
    if (state.object == 0) {
      state.object = object;
      return &state;
    }
  }
  return nullptr;
}

}  // namespace wwe2k14::tiling

// Mid-asm hooks (config/s7_tiling_hooks.toml). Global, non-inline: the generated code declares them extern, so this
// header must be included in exactly one translation unit (main.cpp, directly or through wwe2k14_app.h).

// 0x82C22BCC in sub_82C22AF8: the layout at scene object + 12 is final (table, fallback or custom).
void wwe2k14_tiling_layout_chosen(PPCRegister& r1, PPCRegister& r31) {
  using namespace wwe2k14::tiling;
  const uint32_t object = r31.u32;
  Build build;
  build.object = object;
  build.caller = Load(r1.u32 + kSavedReturnFromR1);
  build.width = Load(object + kLayoutWidth);
  build.height = Load(object + kLayoutHeight);
  build.game_type = Load(object + kLayoutType);
  build.game_tiles = Load(object + kLayoutTiles);
  for (uint32_t i = 0; i < 4; ++i) build.game_tile0[i] = Load(object + kLayoutTile0 + 4 * i);
  // Same signed test as the engine (sub_82C22AF8 and sub_82C222D0).
  build.custom = int32_t(Load(object + kCustomWidth)) > 0 && int32_t(Load(object + kCustomHeight)) > 0;

  const bool force = REXCVAR_GET(wwe2k14_force_no_msaa);
  if (!force) {
    build.force = Force::kOff;
  } else if (build.game_type == 0) {
    build.force = Force::kAlreadyNoMsaa;
  } else if (int32_t(build.width) <= 0 || int32_t(build.height) <= 0) {
    build.force = Force::kBadSize;
  } else if (!FitsEdramWithoutMsaa(build.width, build.height)) {
    build.force = Force::kTooBigForEdram;
  } else {
    // The engine's own 1-tile layout (what sub_82C222D0 writes when no table entry matches), with type 0.
    Store(object + kLayoutType, 0);
    Store(object + kLayoutTiles, 1);
    Store(object + kLayoutTile0 + 0, 0);
    Store(object + kLayoutTile0 + 4, 0);
    Store(object + kLayoutTile0 + 8, build.width);
    Store(object + kLayoutTile0 + 12, build.height);
    build.force = Force::kApplied;
  }
  t_build = build;

  std::lock_guard lock(g_mutex);
  if (ObjectState* state = FindObject(object, true)) state->built_with_force = force ? 1 : 0;
}

// 0x82C22BE4 in sub_82C22AF8: r11 = layout MSAA type; zero skips creating the scene targets.
void wwe2k14_tiling_create_gate(PPCRegister& r11, PPCRegister& r31) {
  using namespace wwe2k14::tiling;
  if (t_build.object == r31.u32 && t_build.force == Force::kApplied && r11.u32 == 0) r11.u64 = 1;
}

// 0x82C22C58 in sub_82C22AF8: targets created (or skipped); r26 / r25 = requested type / flag. Log only.
void wwe2k14_tiling_scene_built(PPCRegister& r1, PPCRegister& r25, PPCRegister& r26, PPCRegister& r31) {
  using namespace wwe2k14::tiling;
  const uint32_t object = r31.u32;
  Build build = t_build;
  t_build = Build{};
  const bool have_build = build.object == object;
  if (!have_build) build.caller = Load(r1.u32 + kSavedReturnFromR1);

  const uint32_t type = Load(object + kLayoutType);
  const uint32_t tiles = Load(object + kLayoutTiles);
  std::array<uint32_t, 4> tile0{};
  for (uint32_t i = 0; i < 4; ++i) tile0[i] = Load(object + kLayoutTile0 + 4 * i);
  const uint32_t color = Load(object + kColorTarget);
  const uint32_t depth = Load(object + kDepthTarget);

  std::string picked = "unknown (layout hook did not run)";
  if (have_build) {
    picked = fmt::format("type {} ({}), {} tile(s) from the {}, {}x{}, tile 0 = ({},{})-({},{})", build.game_type,
                         MsaaName(build.game_type), build.game_tiles, build.custom ? "custom layout" : "table/fallback",
                         build.width, build.height, build.game_tile0[0], build.game_tile0[1], build.game_tile0[2],
                         build.game_tile0[3]);
  }
  // What makes a build "the same" (the target addresses are left out: they can change on every rebuild).
  const std::string key = fmt::format(
      "scene object 0x{:08X}, {}: requested type {} ({}), layout flag {} ({}); game picked {}; built type {} ({}), "
      "{} tile(s), tile 0 = ({},{})-({},{}), {}; force_no_msaa: {}",
      object, CallerName(build.caller), r26.u32, MsaaName(r26.u32), r25.u32,
      r25.u32 ? "horizontal bands" : "vertical strips", picked, type, MsaaName(type), tiles, tile0[0], tile0[1],
      tile0[2], tile0[3], color && depth ? "colour and depth targets created" : "NO scene targets",
      have_build ? ForceName(build.force) : "?");

  std::lock_guard lock(g_mutex);
  ObjectState* state = FindObject(object, true);
  if (!state) {
    REXLOG_INFO("[wwe2k14][tiling] {} (colour 0x{:08X}, depth 0x{:08X}; object table full)", key, color, depth);
    return;
  }
  ++state->builds;
  if (key == state->last_key) {
    ++state->identical;
    uint64_t power = 10;
    while (power < state->identical) power *= 10;
    if (state->identical == power) {
      REXLOG_INFO("[wwe2k14][tiling] scene object 0x{:08X}: the same build again, {} times in a row ({} builds of "
                  "this object so far)", object, state->identical + 1, state->builds);
    }
    return;
  }
  REXLOG_INFO("[wwe2k14][tiling] {} (colour 0x{:08X}, depth 0x{:08X}; build {} of this object{})", key, color, depth,
              state->builds,
              state->identical ? fmt::format("; the previous line repeated {} more times", state->identical) : "");
  state->last_key = key;
  state->identical = 0;
}

// 0x82BA8674 in sub_82BA84E8: r11 = scene object + 4, r4 = render device + 320, r3 = scene object.
void wwe2k14_tiling_rebuild_check(PPCRegister& r3, PPCRegister& r4, PPCRegister& r11) {
  using namespace wwe2k14::tiling;
  const int want = REXCVAR_GET(wwe2k14_force_no_msaa) ? 1 : 0;
  const uint32_t object = r3.u32;
  int built = -1;
  {
    std::lock_guard lock(g_mutex);
    ObjectState* state = FindObject(object, false);
    if (!state || state->built_with_force < 0 || state->built_with_force == want) return;
    // Scene open (between sub_82C224A0 and the end-of-frame sub_82C227E8): try again on a later pass.
    if (Load(object + kSceneOpen) != 0) return;
    built = state->built_with_force;
    state->built_with_force = want;  // the rebuild's layout hook records the same value
  }
  r11.u64 = uint32_t(r4.u32 + 1);  // any value other than r4: the game takes its rebuild branch
  REXLOG_INFO("[wwe2k14][tiling] wwe2k14_force_no_msaa is now {} (scene object 0x{:08X} was built with {}): asking "
              "the game to rebuild its scene targets (requested type {}, built for type {})",
              want ? "true" : "false", object, built ? "true" : "false", r4.u32, Load(object + kBuiltType));
}
