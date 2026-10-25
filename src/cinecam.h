// wwe2k14 - CineCam (Session 13, M6): our own control of the camera the game renders. Default OFF.
//
// Free camera (step 3): --wwe2k14_cinecam=true installs it (default off: nothing registered). F8 (setting
// bind_wwe2k14_cinecam) toggles it, as does `cvar wwe2k14_cinecam_on true|false` in an input script (also at boot);
// on = it takes over the view at the renderer hand-off (below), starting from the view on screen; off = the game's
// view again. Keyboard + mouse (Jarrod, Session 13; the pad keeps driving the game, and ReXGlue's default
// mnk_mode=false keeps the keyboard out of the game): hold the right mouse button to look (cursor hidden and locked
// while held), W/S forward / back along the view, A/D left / right, E/Q world up / down, Shift x4, Ctrl x0.25 (also
// mouse look x0.25), wheel = zoom (FOV x0.9 per notch, with Ctrl x0.98; 2..120 deg), Z/X roll left / right, R roll 0. Movement uses the host
// clock, so it does not depend on the game's speed. --wwe2k14_cinecam_panel (default true) shows a "CineCam" ImGui
// panel while it is on (readout, speed, mouse sensitivity, smoothing, reset to the hard cam, FOV / roll / view resets).
// Full arena (panel, default on): the game draws a cheap arena (no lighting truss, upper bowl mostly as cards, a
// different crowd detail set) while the shown camera's key (camera +300) is 0, the gameplay hard cam; director shots
// (key 10) and entrance cameras get the full arena (run 948 draw counts). With the free camera on, the hard cam's key
// is set to 10 and put back when the free camera goes off or another camera is shown (run 954: top-of-frame
// brightness 12-25 -> 39-71 at the stage pose, back to 14-23 after the restore). Ruled out on the way (runs 937-953,
// PLAYBOOK Session 13): the stage visibility trees, the view block's +64 matrix, the manager's reference key / heading,
// the crowd level, writing our view into the game camera, camera +4/+8/+12, the priority word +296. Swapping the shown
// camera to the director also works (run 950) but fights the game's camera switching (Jarrod saw flicker).
// Depth of field (panel: game / off / on). The post-effect settings object S = [0x8392DD88]: +168 selects the
// depth-aware blur chain (run 969 anatomy: +168 = 1 -> passes A7AD68BE / A5C9AC54 reading depth, 0 -> plain
// 9C85DA5F / 632189BA); the chain (sub_82C2BBE8 -> sub_82C2B8A8) loads S+24, +28, +32, +172 into pixel constant 21.
// Each frame sub_827F14C0(controller, render view) eases S+84 / +88 / +92 / +96 / +172 / +32 toward per-camera presets
// and converts them around a focus distance F (the nearest wrestler or ring post from the render view's camera
// position, view +592) into depth-buffer values with the view's near / far planes (view +500 / +504):
// z(x) = far/(far-near) - near*far/((far-near)*x); +8 = +24 = z(F - S84), +12 = z(F - S92), +16 = z(F + S96),
// +28 = z(F + S88) (listing 0x827F1508..0x827F19AC; run 971 write watch). "on" replaces that result after the game's
// update with the panel's values: focus auto (the game's F, recovered from its own +24) or manual, and the four
// offsets (meaning from the formula: in focus from F - front to F + behind, full blur at F - front2 / F + behind2;
// unconfirmed on screen) and the strength. "off" = +168 0. "game" leaves it alone.
// Smoothing (Jarrod, run 934: "a jagged vibe"): steps use the running average of the frame time, and movement,
// roll, mouse look and zoom ease in and out (0 = raw).
// The view block matrix is WORLD -> VIEW (Jarrod's live test, run 933: the first version treated it as
// camera-to-world, and mouse look orbited the ring, the world origin). Row-vector convention, p_view = p_world * M3 +
// t (t = row 3): column 0 of M3 = the world direction of view +x = screen RIGHT, column 1 = view +y = screen DOWN,
// column 2 = view +z = forward (run 933, Jarrod, with the first version adding to t: D (t.x +) moved the camera left,
// i.e. the picture right; E (t.y +) moved it up; run 930: +994 on t.z pushed everything further away). Right x down
// = forward, so the world is right-handed with Y pointing DOWN. The camera's world position is C_i = -(t . row_i):
// the match camera sits at about (-2013, -474, 0), 4.7 m up and 20 m out, looking along +X at the ring (the origin),
// 6 degrees down (run 926 log). The free camera keeps C, yaw, pitch, roll and builds M3 / t from them.
//
// Step 1a (which value does the renderer read?): --wwe2k14_cinecam_test_fov=DEG (0 = off) overrides the game
// camera's field of view. The game camera (vtable 0x8201D820, docs/systems/camera.md) is updated per frame by
// sub_8257D8E8 (called through vtable slot 0x8201D824), which writes its FOV in degrees to this+144
// (tools/analysis/list_function.py 0x8257D8E8: 0x8257D908 mr r31,r3; 0x8257DEBC / 0x8257DECC stfs ...,144(r31)).
// The override runs the game's update unchanged, then writes DEG to this+144, so anything that reads +144 later in
// the frame sees our value. If the picture zooms, the renderer's projection comes from +144 after the update.
// Run 925 (2026-10-11 22:34): it zooms (game ~14 deg -> 60 deg wide, back to the game's on the next frame off).
//
// Step 1b (position): --wwe2k14_cinecam_test_offset=X,Y,Z (empty = off). The game camera commits its 4x4 matrix with
// sub_82280108(this+16, matrix) (0x8257DF78 addi r30,r31,16 ... 0x8257DF84 bl; again at 0x8257E16C / 0x8257E278
// for shake / director offsets): it stores the 64 bytes at view block +0..+63 (= camera +16..+79) and two rows
// derived by sub_82280008 at view block +144..+175 (camera +160..+191). After the game's update the override reads
// the matrix at camera +16, adds X,Y,Z to row 3 (bytes 48..59, the translation if the matrix is row-major
// camera-to-world: unconfirmed, the log shows it) and commits it through sub_82280108 again (scratch copy on the
// guest stack below r1), so the derived rows stay consistent. Either test setting also logs the matrix a few times.
// Run 926 (22:38): Y +100 raises the view, X +100 moves it sideways; off = the game's view.
//
// Step 2 (one hook for every camera). The scene manager's per-frame composer sub_823C8548
// (tools/analysis/list_function.py 0x823C8548) runs every enabled camera's update (vtable slot 1 = sub_8257D8E8 for
// the game camera), copies the shown camera's 272-byte view block (+1392; or the special camera [0x83767B74] when
// its +340 is set, or the override camera +1412) into the output slot [mgr+24] + [mgr+32] * 272 (sub_823C6C80;
// matrix +0, FOV +128), runs the transition blend (+1448), copies the slot's matrix to mgr +1488 and its FOV to
// +1552, and then, when the last enabled camera's vcall +52 returned 0, calls sub_823C79A8 (0x823C8910..0x823C891C),
// which reads that slot (0x823C79BC..0x823C7A14: matrix, FOV x a constant), inverts the matrix (sub_82280008) and
// calls into the graphics engine: the hand-off to the renderer. Writing after the composer returns is too late
// (runs 927 / 928: the output slot or the shown camera's view block, written every frame, changed nothing on
// screen). --wwe2k14_cinecam_test_stage=1: the test settings are applied to the slot (through sub_82280108) and
// +1488 / +1552 at the entry of sub_823C79A8, for the main manager [0x836F3AA8] only (others logged once each).

#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>  // REX_KERNEL_MEMORY (freeze cam, Session 16)
#include <rex/system/thread_state.h>  // the guest context at VdSwap (freeze cam, Session 16)
#include "frame_stats.h"  // CurrentLogNumber
#include "matte.h"  // Session 16: g_last_dof_view
#include "dlaa.h"   // Session 18: the camera for the motion vectors
#include "cinecam_keys.h"  // Session 35: keyframed shots
#include "scene_freeze.h"  // Session 36: "freeze scene" = the true freeze shared with photo mode

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "platform/compat.h"  // VirtualQuery (path chains, Session 14)
#include <climits>

REXCVAR_DEFINE_BOOL(wwe2k14_cinecam, false, "wwe2k14",
                    "CineCam free camera: install it (F8 toggles it in game; keyboard + mouse). Read once at startup");
REXCVAR_DEFINE_BOOL(wwe2k14_cinecam_on, false, "wwe2k14",
                    "CineCam: the free camera on / off (needs --wwe2k14_cinecam=true); F8 toggles it without "
                    "changing this setting");
REXCVAR_DEFINE_BOOL(wwe2k14_cinecam_panel, true, "wwe2k14",
                    "CineCam: show the CineCam panel (readout, speed) while the free camera is on");
REXCVAR_DEFINE_BOOL(wwe2k14_cinecam_test_camwrite, false, "wwe2k14",
                    "CineCam test: while the free camera is on, also write its last matrix and FOV into the game "
                    "camera's own view block right after the game camera's update (sub_8257D8E8)");
REXCVAR_DEFINE_INT32(wwe2k14_cinecam_test_dof, -1, "wwe2k14",
                     "CineCam test: set the panel's depth of field mode (0 game, 1 off, 2 on) from a script; -1 = leave");
REXCVAR_DEFINE_INT32(wwe2k14_cinecam_test_full_arena, -1, "wwe2k14",
                     "CineCam test: set the panel's full arena checkbox from a script (0 off, 1 on); -1 = leave");
REXCVAR_DEFINE_STRING(wwe2k14_cinecam_test_dof_values, "", "wwe2k14",
                      "CineCam test: A,B,C,D,STRENGTH[,FOCUS] for the depth of field 'on' values (+84, +88, +92, +96, "
                      "+172; FOCUS > 0 = manual focus in cm); applied once when set. Empty = none");
REXCVAR_DEFINE_STRING(wwe2k14_cinecam_test_poke, "", "wwe2k14",
                      "CineCam test: while the free camera is on, write 32-bit words into the shown camera at every "
                      "hand-off: OFF=VALUE,... (decimal or 0x hex). The original words are restored when the "
                      "setting changes or is cleared. Empty = none");
REXCVAR_DEFINE_STRING(wwe2k14_cinecam_test_pose, "", "wwe2k14",
                      "CineCam test: X,Y,Z,YAW,PITCH,FOV (world Y down, degrees) applied once to the free camera "
                      "when set (it must be on); for scripted checks without the mouse. Empty = none");
REXCVAR_DEFINE_INT32(wwe2k14_cinecam_dump_view, 0, "wwe2k14",
                     "CineCam diagnosis: set to N > 0 to log the composed view slot (272 bytes, words and floats) "
                     "at the next hand-off, labelled N, and the one after as N + 10, and save the slot, the scene manager (4 KB) and the "
                     "shown camera (4 KB), the globals 0x83500000.. and the director (key 10) to "
                     "out/harness/cinecam/<run>_N_{slot,mgr,cam,data,dir}.bin (game memory: local "
                     "only); it resets itself to 0");
REXCVAR_DEFINE_DOUBLE(wwe2k14_cinecam_test_fov, 0.0, "wwe2k14",
                      "CineCam step 1a test: after the game camera's update (sub_8257D8E8) write this field of view "
                      "(degrees) to the camera's +144. 0 = off");

REXCVAR_DEFINE_INT32(wwe2k14_cinecam_test_stage, 0, "wwe2k14",
                     "CineCam tests: where the test FOV / offset are applied. 0 = after the game camera's update "
                     "(step 1), 1 = the composer's output slot at the hand-off to the renderer, sub_823C79A8's entry "
                     "(step 2, every camera)");
REXCVAR_DEFINE_STRING(wwe2k14_cinecam_test_offset, "", "wwe2k14",
                      "CineCam step 1b test: after the game camera's update add X,Y,Z to its matrix row 3 and commit "
                      "it through the game's setter sub_82280108. Empty = off");

// Paths (Session 14, stretch goal S2: playback plumbing only; no panel or key changes). All default off.
REXCVAR_DEFINE_STRING(wwe2k14_cinecam_path_record, "", "wwe2k14",
                      "CineCam path: record the free camera (position, yaw, pitch, roll, FOV, depth of field settings) "
                      "once per guest frame from the anchor on, to FILE (relative = out/harness/cinecam/paths/). "
                      "Never overwrites (FILE_2, ...). Empty = off. See the Paths section in src/cinecam.h");
REXCVAR_DEFINE_STRING(wwe2k14_cinecam_path_play, "", "wwe2k14",
                      "CineCam path: replay FILE (as recorded) from the anchor on: turns the free camera on at the "
                      "anchor and sets its pose every frame from the record for that anchor-relative frame. Empty = off");
REXCVAR_DEFINE_STRING(wwe2k14_cinecam_path_anchor, "step5:0", "wwe2k14",
                      "CineCam path: time origin. step5:N = entrance slot N's step byte reaches 5 (states.toml "
                      "entrance_step_N), clock = the match clock word 0x836F3928 becomes non-zero, frame = guest frame 0, "
                      "frame:N = guest frame N (Session 16: replays are frame-exact)");
// Freeze cam (Session 16 item 6). Default off.
REXCVAR_DEFINE_BOOL(wwe2k14_cinecam_freeze, false, "wwe2k14",
                    "CineCam freeze cam (Session 16): once the match is decided and the scene has stopped handing its "
                    "view to the renderer (the post-match freeze), drive the hand-off from VdSwap every frame while "
                    "the free camera is on, so the camera moves around the frozen scene");
REXCVAR_DEFINE_INT32(wwe2k14_cinecam_freeze_after, 120, "wwe2k14",
                     "CineCam freeze cam: frames without a hand-off (match decided) before the freeze cam takes over");
REXCVAR_DEFINE_BOOL(wwe2k14_cinecam_path_log, false, "wwe2k14",
                    "CineCam path: log every recorded / replayed frame's view (matrix rows, FOV) and the entrance "
                    "step bytes of slots 0-5 when they change, relative to the anchor");
REXCVAR_DEFINE_STRING(wwe2k14_cinecam_shot, "", "wwe2k14",
                      "CineCam shots from a script: record / stop / NN (play shot_NN), the same as the panel's shot "
                      "buttons. Set it with `cvar wwe2k14_cinecam_shot ...` in an input script");
REXCVAR_DEFINE_STRING(wwe2k14_cinecam_test_move, "", "wwe2k14",
                      "CineCam test (S2): VX,VY,VZ,VYAW per second (world units, degrees) added to the free camera "
                      "every frame, for a scripted move without the keyboard. Empty = none");

// Keyframed shots (Session 35; the "Keyframed shots" section below). Default: nothing happens until a shot is used.
REXCVAR_DEFINE_BOOL(wwe2k14_cinecam_timeline, true, "wwe2k14",
                    "CineCam: show the keyframe timeline panel while the free camera is on");
REXCVAR_DEFINE_STRING(wwe2k14_cinecam_timeline_cmd, "", "wwe2k14",
                      "CineCam keyframed shots from a script: commands separated by ';' (key [TRACK], time S, preview "
                      "[start], take, stop, rate R, loop 0|1, length S, constant 0|1, follow S, target aim|move|focus "
                      "none|slot N|pin X Y Z|here, ease TRACK I E, curve TRACK I smooth|linear|hold, set|add TRACK I "
                      "DIM VALUE, clear, save NAME, "
                      "load NAME, freeze 0|1). Set it with `cvar wwe2k14_cinecam_timeline_cmd ...`");

REXCVAR_DEFINE_STRING(wwe2k14_cinecam_dir, "", "wwe2k14",
                      "CineCam: the folder for shots (shots\\*.shot) and recorded paths (paths\\shot_NN.txt). Empty = "
                      "out/harness/cinecam in a +dev build, Documents\\WWE 2K14\\CineCam in a release");

REX_EXTERN(sub_82280108);  // view block matrix setter (view block, matrix)

namespace wwe2k14::cinecam {

inline std::atomic<uint32_t> g_test_fov_logged{0};

inline void StoreGuestF32(uint8_t* base, uint32_t address, float value) {
  const uint32_t be = std::byteswap(std::bit_cast<uint32_t>(value));
  std::memcpy(base + address, &be, 4);
}

inline float LoadGuestF32(const uint8_t* base, uint32_t address) {
  uint32_t be;
  std::memcpy(&be, base + address, 4);
  return std::bit_cast<float>(std::byteswap(be));
}

inline std::atomic<uint32_t> g_matrix_logged{0};

// Parses "X,Y,Z" (floats); false if empty or malformed.
inline bool ParseOffset(const std::string& text, float out[3]) {
  if (text.empty()) return false;
  return std::sscanf(text.c_str(), "%f,%f,%f", &out[0], &out[1], &out[2]) == 3;
}

constexpr uint32_t kSceneManagerPtr = 0x836F3AA8;

// out/harness/cinecam next to the build (exe folder ../../harness/cinecam), created on first use.
inline std::filesystem::path DumpDir() {
  std::error_code ec;
  wchar_t buf[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  std::filesystem::path dir = std::filesystem::weakly_canonical(
      std::filesystem::path(std::wstring(buf, n)).parent_path() / ".." / ".." / "harness" / "cinecam", ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

// Session 35: where shots (*.shot) and recorded paths (shot_NN.txt) live. A +dev build: out/harness/cinecam (DumpDir).
// A release build (no "+dev" in the version): Documents\WWE 2K14\CineCam, beside photo mode's Pictures\WWE 2K14 (the
// exe-relative ../../harness would land outside the unzipped game folder). --wwe2k14_cinecam_dir overrides both.
inline std::filesystem::path UserDir() {
  const std::string d = REXCVAR_GET(wwe2k14_cinecam_dir);
  if (!d.empty()) return std::filesystem::path(d);
#ifdef WWE2K14_VERSION
  if (std::string_view(WWE2K14_VERSION).find("+dev") == std::string_view::npos) {
    PWSTR docs = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) dir = docs;
    CoTaskMemFree(docs);
    if (!dir.empty()) return dir / "WWE 2K14" / "CineCam";
  }
#endif
  return DumpDir();
}

inline void DumpBytes(const uint8_t* base, uint32_t address, uint32_t size, int32_t label, const char* what) {
  if (!address) return;
  const auto path = DumpDir() / fmt::format("{:03}_{}_{}.bin", std::max(wwe2k14::CurrentLogNumber(), 0), label, what);
  if (FILE* fh = _wfopen(path.c_str(), L"wb")) {
    std::fwrite(base + address, 1, size, fh);
    std::fclose(fh);
  }
}  // camera.md: [0x836F3AA8] = the scene manager

inline uint32_t LoadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t be;
  std::memcpy(&be, base + address, 4);
  return std::byteswap(be);
}

inline void LogMatrix(const uint8_t* base, uint32_t vb, const char* what) {
  float m[16];
  for (int i = 0; i < 16; ++i) m[i] = LoadGuestF32(base, vb + 4 * i);
  REXLOG_INFO("[wwe2k14][cinecam] {} view block {:08X} matrix [{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}] "
              "[{:.3f} {:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f} {:.3f}] fov {:.3f}",
              what, vb, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14],
              m[15], LoadGuestF32(base, vb + 128));
}

// Applies the test settings to a view block (matrix +0, FOV +128). Returns false when no test is on.
inline bool ApplyTest(PPCContext& ctx, uint8_t* base, uint32_t vb, const char* where) {
  const double fov = REXCVAR_GET(wwe2k14_cinecam_test_fov);
  float offset[3];
  const bool move = ParseOffset(REXCVAR_GET(wwe2k14_cinecam_test_offset), offset);
  if (fov <= 0.0 && !move) return false;
  const bool log = g_matrix_logged.fetch_add(1) < 3;
  if (log) LogMatrix(base, vb, where);
  if (fov > 0.0) {
    const float game_fov = LoadGuestF32(base, vb + 128);
    StoreGuestF32(base, vb + 128, float(fov));
    // A few lines so the log shows the hook fired and what the game had written.
    if (g_test_fov_logged.fetch_add(1) < 5) {
      REXLOG_INFO("[wwe2k14][cinecam] test fov ({}): view block {:08X} game fov {:.3f} -> {:.3f}", where, vb, game_fov,
                  fov);
    }
  }
  if (move) {
    // Scratch matrix on the guest stack well below the caller's frame (16-byte aligned for the setter's lvx128).
    const uint32_t scratch = (ctx.r1.u32 - 1024) & ~15u;
    std::memcpy(base + scratch, base + vb, 64);
    for (int i = 0; i < 3; ++i) {
      StoreGuestF32(base, scratch + 48 + 4 * i, LoadGuestF32(base, scratch + 48 + 4 * i) + offset[i]);
    }
    const PPCContext saved = ctx;
    ctx.r3.u64 = vb;
    ctx.r4.u64 = scratch;
    sub_82280108(ctx, base);
    ctx = saved;
    if (log) LogMatrix(base, vb, "moved");
  }
  return true;
}

inline std::atomic<uint32_t> g_other_managers_logged{0};

// --wwe2k14_cinecam_test_poke (guest thread, at the hand-off). Keeps the originals of the words it wrote so a new
// spec (or an empty one) puts them back first.
struct PokeState {
  std::string spec;
  uint32_t object = 0;
  std::vector<std::pair<uint32_t, uint32_t>> originals;  // (offset, big-endian word)
};

inline PokeState& Poke() {
  static PokeState p;
  return p;
}

inline void ApplyTestPoke(uint8_t* base, uint32_t shown) {
  PokeState& p = Poke();
  const std::string spec = REXCVAR_GET(wwe2k14_cinecam_test_poke);
  if (spec != p.spec || shown != p.object) {
    for (const auto& [off, be] : p.originals) std::memcpy(base + p.object + off, &be, 4);
    if (!p.originals.empty()) {
      REXLOG_INFO("[wwe2k14][cinecam] test poke: restored {} word(s) of {:08X}", p.originals.size(), p.object);
    }
    p.originals.clear();
    p.spec = spec;
    p.object = shown;
    if (!shown) return;
    size_t pos = 0;
    while (pos < spec.size()) {
      size_t comma = spec.find(',', pos);
      if (comma == std::string::npos) comma = spec.size();
      const std::string token = spec.substr(pos, comma - pos);
      pos = comma + 1;
      const size_t eq = token.find('=');
      if (eq == std::string::npos) continue;
      const uint32_t off = uint32_t(std::strtoul(token.substr(0, eq).c_str(), nullptr, 0)) & ~3u;
      uint32_t be;
      std::memcpy(&be, base + shown + off, 4);
      p.originals.emplace_back(off, be);
    }
    REXLOG_INFO("[wwe2k14][cinecam] test poke on {:08X}: '{}'", shown, spec);
  }
  if (!shown || spec.empty()) return;
  size_t pos = 0;
  while (pos < spec.size()) {
    size_t comma = spec.find(',', pos);
    if (comma == std::string::npos) comma = spec.size();
    const std::string token = spec.substr(pos, comma - pos);
    pos = comma + 1;
    const size_t eq = token.find('=');
    if (eq == std::string::npos) continue;
    const uint32_t off = uint32_t(std::strtoul(token.substr(0, eq).c_str(), nullptr, 0)) & ~3u;
    const uint32_t be = std::byteswap(uint32_t(std::strtoul(token.substr(eq + 1).c_str(), nullptr, 0)));
    std::memcpy(base + shown + off, &be, 4);
  }
}

// ---- Free camera (step 3) ---------------------------------------------------------------------------------------

constexpr float kDegToRad = 0.017453292519943295f;

// Depth of field "on" defaults: a director shot's values (run 961 dumps). Distances in cm around the focus: sharp in
// front / behind (S+84 / +88), full blur in front / behind (S+92 / +96).
constexpr float kDofDefaultDist[4] = {100.0f, 500.0f, 180.0f, 4500.0f};
constexpr float kDofDefaultStrength = 0.0009f;
constexpr float kDofDefaultFocus = 1000.0f;

// Shared between the UI thread (input, panel) and the guest thread that runs the hand-off. Guarded by mutex.
struct FreeCam {
  std::mutex mutex;
  bool installed = false;
  bool active = false;  // F8
  bool seeded = false;  // false = take position / angles / FOV from the game's view at the next hand-off
  float pos[3] = {};                   // the camera's world position (world Y points down)
  float yaw = 0, pitch = 0, roll = 0;  // degrees (Axes below)
  float fov = 30;                      // degrees, as the game's view block +128
  float fov_target = 30;               // the wheel sets this; fov eases toward it
  float game_fov = 30;                 // the game's FOV when seeded
  float vel[3] = {};                   // world units per second (eased toward the keys' target)
  float roll_rate = 0;                 // degrees per second (eased)
  float look_yaw = 0, look_pitch = 0;  // mouse look not yet applied (degrees; eased in)
  float dt_avg = 1.0f / 60;            // running average of the time between hand-offs (seconds)
  bool reset_hard = false;
  bool full_arena = true;     // panel: show the full arena (the hard cam's key 0 -> 10 while on)
  int dof_mode = 0;           // panel: 0 = as the game, 1 = off, 2 = on
  // Session 33 (photo mode): the controller's sticks / triggers / bumpers (-1..1 after the dead zone; set by
  // src\photo_mode.h while it owns the pad): left stick move, right stick look, triggers up / down, bumpers zoom.
  float pad_move_x = 0, pad_move_y = 0, pad_look_x = 0, pad_look_y = 0, pad_up = 0, pad_zoom = 0;
  // "on": the post-effect settings' DOF values, starting from a director shot's (run 961 dumps: 100 / 500 / 180 /
  // 4500 / 0.0009; the hard cam has 0 / 800 / 700 / 3800 / 0.0003). Applied after the game's update (sub_827F14C0).
  float dof_dist[4] = {kDofDefaultDist[0], kDofDefaultDist[1], kDofDefaultDist[2], kDofDefaultDist[3]};
  float dof_strength = kDofDefaultStrength;  // S+172
  bool dof_manual = false;                   // focus: false = auto (the game's nearest-wrestler distance)
  float dof_focus = kDofDefaultFocus;        // manual focus distance (cm)
  float dof_auto_focus = 0;   // the last auto focus distance (readout)
  uint32_t key_camera = 0;    // the camera whose key we changed (guest address), 0 = none
  uint32_t key_original = 0;  // its key before (host order)
  float last_m[16] = {};  // the matrix written at the last hand-off (test_camwrite)
  float last_fov = 0;
  bool have_last = false;
             // panel: seed from the game's hard cam (vtable 0x8201D820) at the next hand-off
  // Input (UI thread writes, guest thread reads and clears the accumulators).
  bool key[256] = {};
  bool looking = false;  // right mouse button held
  float mouse_dx = 0, mouse_dy = 0;
  int32_t wheel = 0;  // accumulated wheel notches (+ = away from the user = zoom in)
  int32_t prev_x = 0, prev_y = 0;
  bool relative = false;  // the window granted relative mouse mode while looking
  // Tuning (panel).
  float speed = 300.0f;       // world units per second (units look like cm: unconfirmed)
  float sensitivity = 0.12f;  // degrees per mouse pixel
  // 0 = raw (instant), 1 = floaty: time constants 0.3 s move, 0.1 s look, 0.15 s zoom. 0.2 = Jarrod's pick (run 935)
  float smoothing = 0.2f;
  std::chrono::steady_clock::time_point last;
  uint64_t frames = 0;
  rex::ui::Window* window = nullptr;
  rex::ui::ImGuiDrawer* drawer = nullptr;
  rex::ui::ImGuiDialog* panel = nullptr;
  rex::ui::ImGuiDialog* timeline = nullptr;  // Session 35: the keyframe timeline
  rex::ui::ImGuiDialog* setup = nullptr;     // Session 36: F8 outside a match (settings only)
};

inline FreeCam& F() {
  static FreeCam f;
  return f;
}

inline bool KeyDown(const FreeCam& f, rex::ui::VirtualKey k) { return f.key[static_cast<uint16_t>(k) & 0xFF]; }

// Camera axes in world space (right-handed, Y down) from yaw / pitch (degrees): forward f, right r (level),
// down d = f x r (so r x d = f, as the game's columns). yaw 0 = looking along +X with right = -Z (the match camera);
// turning right = yaw up; pitch > 0 = looking up (-Y).
inline void Axes(float yaw, float pitch, float f[3], float r[3], float d[3]) {
  const float cy = std::cos(yaw * kDegToRad), sy = std::sin(yaw * kDegToRad);
  const float cp = std::cos(pitch * kDegToRad), sp = std::sin(pitch * kDegToRad);
  f[0] = cp * cy, f[1] = -sp, f[2] = -cp * sy;
  r[0] = -sy, r[1] = 0, r[2] = -cy;
  d[0] = sp * cy, d[1] = cp, d[2] = -sp * sy;
}

inline float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The world -> view matrix (see the header): M3 columns = (right, down, forward) after roll (roll > 0 = the right
// side dips, like tilting your head right), t_j = -(C . column_j).
inline void BuildMatrix(const FreeCam& f, float m[16]) {
  float fw[3], r[3], d[3];
  Axes(f.yaw, f.pitch, fw, r, d);
  const float cr = std::cos(f.roll * kDegToRad), sr = std::sin(f.roll * kDegToRad);
  float r2[3], d2[3];
  for (int i = 0; i < 3; ++i) {
    r2[i] = r[i] * cr + d[i] * sr;
    d2[i] = d[i] * cr - r[i] * sr;
  }
  const float* cols[3] = {r2, d2, fw};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) m[i * 4 + j] = cols[j][i];
    m[i * 4 + 3] = 0;
  }
  for (int j = 0; j < 3; ++j) m[12 + j] = -Dot3(f.pos, cols[j]);
  m[15] = 1;
}

// Seeds the free camera from the game's composed view (slot vb): position, yaw, pitch and roll from the matrix
// (round trip checked offline on matrices from runs 926 / 933: rotation within 0.001). Caller holds the mutex.
inline void Seed(FreeCam& f, const uint8_t* base, uint32_t vb) {
  float m[16];
  for (int i = 0; i < 16; ++i) m[i] = LoadGuestF32(base, vb + 4 * i);
  const float fw_game[3] = {m[2], m[6], m[10]};
  const float right_game[3] = {m[0], m[4], m[8]};
  const float t[3] = {m[12], m[13], m[14]};
  for (int i = 0; i < 3; ++i) {
    const float row[3] = {m[i * 4 + 0], m[i * 4 + 1], m[i * 4 + 2]};
    f.pos[i] = -Dot3(t, row);
  }
  f.pitch = std::asin(std::clamp(-fw_game[1], -1.0f, 1.0f)) / kDegToRad;
  f.yaw = std::atan2(-fw_game[2], fw_game[0]) / kDegToRad;
  float fw[3], r[3], d[3];
  Axes(f.yaw, f.pitch, fw, r, d);
  f.roll = std::atan2(Dot3(right_game, d), Dot3(right_game, r)) / kDegToRad;
  f.fov = f.fov_target = f.game_fov = LoadGuestF32(base, vb + 128);
  std::memset(f.vel, 0, sizeof(f.vel));
  f.roll_rate = f.look_yaw = f.look_pitch = 0;
  f.seeded = true;
  f.last = std::chrono::steady_clock::now();
  REXLOG_INFO("[wwe2k14][cinecam] free camera seeded from view block {:08X}: camera at ({:.1f}, {:.1f}, {:.1f}) (Y down) "
              "yaw {:.1f} pitch {:.1f} roll {:.1f} fov {:.2f}",
              vb, f.pos[0], f.pos[1], f.pos[2], f.yaw, f.pitch, f.roll, f.fov);
}

constexpr uint32_t kGameCameraVtable = 0x8201D820;  // camera.md: the game camera ("hard cam") family
constexpr uint32_t kPostEffectSettingsPtr = 0x8392DD88;  // [..]+168 = depth of field active (run 959)

// The game camera's view block (camera +16) from the manager's table, or 0 when there is none (entrances).
inline uint32_t FindHardCam(const uint8_t* base, uint32_t mgr) {
  const uint32_t table = LoadGuestU32(base, mgr + 1380);
  const uint32_t count = LoadGuestU32(base, mgr + 1384);
  if (!table || count > 256) return 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t cam = LoadGuestU32(base, table + i * 8 + 4);
    if (cam && LoadGuestU32(base, cam) == kGameCameraVtable) return cam + 16;
  }
  return 0;
}

// Eases x toward target with time constant tau (seconds); tau <= 0 = instant.
inline float Ease(float x, float target, float dt, float tau) {
  if (tau <= 0) return target;
  return x + (target - x) * (1.0f - std::exp(-dt / tau));
}

// ---- Paths (Session 14, S2) -------------------------------------------------------------------------------------
// Record / replay of the free camera, timed in guest frames from a game event (the anchor, see the setting). The file
// is text: a header line, then one line per guest frame from the anchor on:
//   rel x y z yaw pitch roll fov dof_mode dof_manual dof_focus d0 d1 d2 d3 strength
// Recording samples the free camera's state at the hand-off (after input, before the matrix is built), once per guest
// frame. Replay turns the free camera on when the anchor fires and, every frame, takes the record with the largest
// rel <= the current one (exact matches counted), so the same matrix is built (BuildMatrix) and committed through
// the game's setter as when it was recorded. After the last record the camera holds the last pose. Guest thread only
// (the hand-off), under FreeCam::mutex.
struct PathPose {
  int64_t rel = 0;
  float pos[3] = {}, yaw = 0, pitch = 0, roll = 0, fov = 30;
  int dof_mode = 0, dof_manual = 0;
  float dof_focus = 0, dof_dist[4] = {}, dof_strength = 0;
};
struct PathState {
  bool init = false;
  std::string anchor_spec;
  int anchor_slot = -1;  // step5:N
  bool anchor_clock = false, anchor_frame0 = false;
  int64_t anchor_at = -1;  // frame:N (Session 16)
  int64_t anchor = -1;  // guest frame of the anchor, -1 = not yet
  std::FILE* rec = nullptr;
  std::filesystem::path rec_path;
  std::vector<PathPose> play;
  size_t cursor = 0;
  bool play_ended = false;
  uint64_t last_frame = ~0ull;
  uint64_t frames = 0, exact = 0, held = 0;
  int8_t steps[6] = {-2, -2, -2, -2, -2, -2};
  bool playing = false;  // a replay is running (setting or a shot button)
  bool ui = false;       // started from the panel (Record / a shot button): timed from the click, control back at the end
  std::vector<std::string> shots;  // shot_*.txt in the paths folder (panel buttons)
  std::chrono::steady_clock::time_point shots_scanned;
  std::string status;  // last panel action, shown under the buttons
};
inline PathState& P() {
  static PathState p;
  return p;
}

inline bool PathRecording() { return !REXCVAR_GET(wwe2k14_cinecam_path_record).empty(); }
inline bool PathPlaying() { return !REXCVAR_GET(wwe2k14_cinecam_path_play).empty(); }
// Anything path-related to do at the hand-off: the settings, or a panel recording / replay.
inline bool PathActive() { return PathRecording() || PathPlaying() || P().rec || P().playing; }

// Fault-safe guest word / byte (the slot chains can be null or stale between scenes).
inline bool PathRead(const uint8_t* base, uint32_t guest, uint32_t size, uint32_t* out) {
  if (guest < 0x40000000u || (size == 4 && (guest & 3))) return false;
  const uint8_t* host = base + guest;
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(host, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
    return false;
  }
  *out = size == 1 ? host[0] : LoadGuestU32(base, guest);
  return true;
}
// Entrance step byte of slot i (states.toml entrance_step_i: [0x836F38D0 + 4i] > 28 > +2), -1 = unreadable.
inline int8_t SlotStep(const uint8_t* base, int i) {
  uint32_t c = 0, act = 0, step = 0;
  if (!PathRead(base, 0x836F38D0u + 4 * i, 4, &c) || !PathRead(base, c + 28, 4, &act) ||
      !PathRead(base, act + 2, 1, &step)) {
    return -1;
  }
  return int8_t(step);
}

inline std::filesystem::path PathFile(const std::string& name) {
  std::filesystem::path p(name);
  if (p.is_relative()) p = UserDir() / "paths" / p;
  return p.lexically_normal();
}

// Reads a path file (format above) into `out`; returns false if it cannot be opened.
inline bool LoadPathFile(const std::filesystem::path& p, std::vector<PathPose>& out) {
  out.clear();
  std::FILE* f = std::fopen(p.string().c_str(), "r");
  if (!f) return false;
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    if (line[0] == '#') continue;
    PathPose q;
    long long rel = 0;
    if (std::sscanf(line, "%lld %f %f %f %f %f %f %f %d %d %f %f %f %f %f %f", &rel, &q.pos[0], &q.pos[1], &q.pos[2],
                    &q.yaw, &q.pitch, &q.roll, &q.fov, &q.dof_mode, &q.dof_manual, &q.dof_focus, &q.dof_dist[0],
                    &q.dof_dist[1], &q.dof_dist[2], &q.dof_dist[3], &q.dof_strength) == 16) {
      q.rel = rel;
      out.push_back(q);
    }
  }
  std::fclose(f);
  return true;
}

// Session 35 (defined with the keyframed shots below): --wwe2k14_cinecam_path_play=NAME.shot.
inline bool ShotPathInit(const std::string& file);
inline void ShotPathAnchor(FreeCam& f);

inline void PathInit() {
  PathState& s = P();
  if (s.init) return;
  s.init = true;
  s.anchor_spec = REXCVAR_GET(wwe2k14_cinecam_path_anchor);
  if (s.anchor_spec.rfind("step5:", 0) == 0) {
    s.anchor_slot = std::clamp(std::atoi(s.anchor_spec.c_str() + 6), 0, 5);
  } else if (s.anchor_spec == "clock") {
    s.anchor_clock = true;
  } else if (s.anchor_spec.rfind("frame:", 0) == 0) {
    s.anchor_at = std::max<int64_t>(0, std::atoll(s.anchor_spec.c_str() + 6));
  } else {
    s.anchor_frame0 = true;
    s.anchor = 0;
  }
  if (PathRecording()) {
    std::filesystem::path p = PathFile(REXCVAR_GET(wwe2k14_cinecam_path_record));
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    const std::filesystem::path stem = p.parent_path() / p.stem();
    const std::string ext = p.extension().string();
    for (int k = 2; std::filesystem::exists(p) && k < 1000; ++k) p = stem.string() + "_" + std::to_string(k) + ext;
    s.rec = std::fopen(p.string().c_str(), "w");
    s.rec_path = p;
    if (s.rec) {
      std::fprintf(s.rec, "# wwe2k14 cinecam path v1 anchor=%s run=%d\n", s.anchor_spec.c_str(), CurrentLogNumber());
      std::fprintf(s.rec, "# rel x y z yaw pitch roll fov dof_mode dof_manual dof_focus d0 d1 d2 d3 strength\n");
    }
    REXLOG_INFO("[wwe2k14][cinecam][path] recording to {} (anchor {}){}", p.string(), s.anchor_spec,
                s.rec ? "" : ": OPEN FAILED");
  }
  if (PathPlaying() && std::filesystem::path(REXCVAR_GET(wwe2k14_cinecam_path_play)).extension() == ".shot") {
    // Session 35: a keyframed shot (relative = out/harness/cinecam/shots/), played as a take from the anchor.
    const bool ok = ShotPathInit(REXCVAR_GET(wwe2k14_cinecam_path_play));
    REXLOG_INFO("[wwe2k14][cinecam][path] keyframed shot {}: {}, anchor {}", REXCVAR_GET(wwe2k14_cinecam_path_play),
                ok ? "loaded" : "NOT LOADED", s.anchor_spec);
  } else if (PathPlaying()) {
    const std::filesystem::path p = PathFile(REXCVAR_GET(wwe2k14_cinecam_path_play));
    LoadPathFile(p, s.play);
    s.playing = !s.play.empty();
    REXLOG_INFO("[wwe2k14][cinecam][path] replay {}: {} frames (rel {} .. {}), anchor {}", p.string(), s.play.size(),
                s.play.empty() ? 0 : s.play.front().rel, s.play.empty() ? 0 : s.play.back().rel, s.anchor_spec);
  }
}

// Every hand-off of the main manager while a path setting is set (guest thread): anchor detection, step logging,
// and (replay) turning the free camera on at the anchor. Caller holds the mutex.
inline void PathTick(const uint8_t* base, FreeCam& f) {
  PathInit();
  PathState& s = P();
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  if (s.anchor < 0) {
    bool fire = false;
    if (s.anchor_slot >= 0) {
      fire = SlotStep(base, s.anchor_slot) == 5;
    } else if (s.anchor_clock) {
      uint32_t clock = 0;
      fire = PathRead(base, 0x836F3928u, 4, &clock) && clock != 0;
    } else if (s.anchor_at >= 0) {
      fire = int64_t(frame) >= s.anchor_at;
    }
    if (fire) {
      s.anchor = int64_t(frame);
      REXLOG_INFO("[wwe2k14][cinecam][path] anchor {} at frame {}", s.anchor_spec, frame);
    }
  }
  if (REXCVAR_GET(wwe2k14_cinecam_path_log) && frame != s.last_frame) {
    for (int i = 0; i < 6; ++i) {
      const int8_t st = SlotStep(base, i);
      if (st != s.steps[i]) {
        REXLOG_INFO("[wwe2k14][cinecam][path] step slot {} {} -> {} frame={} rel={}", i, s.steps[i], st, frame,
                    s.anchor >= 0 ? int64_t(frame) - s.anchor : INT64_MIN);
        s.steps[i] = st;
      }
    }
  }
  if (s.anchor >= 0) ShotPathAnchor(f);  // Session 35: a keyframed shot starts at the anchor
  if (s.playing && s.anchor >= 0 && !f.active && !s.play.empty() && !s.play_ended) {
    f.active = true;
    f.seeded = true;  // the path sets the pose; never seed from the game's view
    f.last = std::chrono::steady_clock::now();
    REXLOG_INFO("[wwe2k14][cinecam][path] free camera on for the replay at frame {}", frame);
  }
}

// Inside ApplyFreeCam, after the input step and before the matrix is built (guest thread, mutex held): replay sets
// the pose, recording writes it. Returns the anchor-relative frame (INT64_MIN before the anchor).
inline int64_t PathFrame(FreeCam& f) {
  PathState& s = P();
  if (s.anchor < 0) return INT64_MIN;
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  const int64_t rel = int64_t(frame) - s.anchor;
  const bool new_frame = frame != s.last_frame;
  s.last_frame = frame;
  if (s.playing && !s.play.empty()) {
    while (s.cursor + 1 < s.play.size() && s.play[s.cursor + 1].rel <= rel) ++s.cursor;
    const PathPose& q = s.play[s.cursor];
    {  // before the first record: hold the first pose (the recording starts a frame or two after the anchor)
      std::memcpy(f.pos, q.pos, sizeof(f.pos));
      f.yaw = q.yaw, f.pitch = q.pitch, f.roll = q.roll;
      f.fov = f.fov_target = q.fov;
      f.dof_mode = q.dof_mode, f.dof_manual = q.dof_manual != 0, f.dof_focus = q.dof_focus;
      std::memcpy(f.dof_dist, q.dof_dist, sizeof(f.dof_dist));
      f.dof_strength = q.dof_strength;
      std::memset(f.vel, 0, sizeof(f.vel));
      f.roll_rate = f.look_yaw = f.look_pitch = 0;
      if (new_frame) {
        ++s.frames;
        if (q.rel == rel) ++s.exact; else ++s.held;
      }
      if (s.cursor + 1 == s.play.size() && rel > q.rel && !s.play_ended) {
        s.play_ended = true;
        REXLOG_INFO("[wwe2k14][cinecam][path] replay ended at rel {} (frames {}, exact {}, held {})", rel, s.frames,
                    s.exact, s.held);
        if (s.ui) {  // a shot button: the camera stays on the last pose and the keyboard / mouse take over again
          s.playing = false;
          s.status = "shot finished: you have the camera";
        }
      }
    }
  }
  if (s.rec && new_frame && rel >= 0) {
    std::fprintf(s.rec, "%lld %.4f %.4f %.4f %.5f %.5f %.5f %.5f %d %d %.3f %.3f %.3f %.3f %.3f %.7f\n",
                 static_cast<long long>(rel), f.pos[0], f.pos[1], f.pos[2], f.yaw, f.pitch, f.roll, f.fov, f.dof_mode,
                 f.dof_manual ? 1 : 0, f.dof_focus, f.dof_dist[0], f.dof_dist[1], f.dof_dist[2], f.dof_dist[3],
                 f.dof_strength);
    if (++s.frames % 60 == 0) std::fflush(s.rec);
  }
  return rel;
}

// ---- Shot buttons (Jarrod, 2026-10-12 22:40: "for testing ... at the bottom of the overlay as buttons you can hit to
// go into the shots"; his later design is a timeline browser at the bottom of the screen). UI thread, FreeCam::mutex
// held. Shots are path files out/harness/cinecam/paths/shot_NN.txt; a shot plays from its first frame the moment its
// button is clicked (anchor = that frame), and when it ends the camera stays on its last pose under the user's control.
inline void ScanShots(PathState& s) {
  s.shots.clear();
  std::error_code ec;
  const std::filesystem::path dir = UserDir() / "paths";
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    const std::string n = e.path().filename().string();
    if (n.rfind("shot_", 0) == 0 && e.path().extension() == ".txt") s.shots.push_back(e.path().stem().string());
  }
  std::sort(s.shots.begin(), s.shots.end());
  s.shots_scanned = std::chrono::steady_clock::now();
}

inline void UiStopRecord(PathState& s) {
  if (!s.rec) return;
  std::fclose(s.rec);
  s.rec = nullptr;
  s.ui = false;
  s.status = "saved " + s.rec_path.stem().string() + " (" + std::to_string(s.frames) + " frames)";
  REXLOG_INFO("[wwe2k14][cinecam][path] panel: {} saved, {} frames", s.rec_path.string(), s.frames);
  ScanShots(s);
}

inline void UiStartRecord(PathState& s) {
  if (s.rec) return;
  s.playing = false;
  std::error_code ec;
  const std::filesystem::path dir = UserDir() / "paths";
  std::filesystem::create_directories(dir, ec);
  std::filesystem::path p;
  for (int k = 1; k < 1000; ++k) {
    char name[32];
    std::snprintf(name, sizeof(name), "shot_%02d.txt", k);
    p = dir / name;
    if (!std::filesystem::exists(p)) break;
  }
  s.rec = std::fopen(p.string().c_str(), "w");
  s.rec_path = p;
  if (!s.rec) {
    s.status = "could not create " + p.string();
    return;
  }
  std::fprintf(s.rec, "# wwe2k14 cinecam path v1 anchor=panel run=%d\n", CurrentLogNumber());
  std::fprintf(s.rec, "# rel x y z yaw pitch roll fov dof_mode dof_manual dof_focus d0 d1 d2 d3 strength\n");
  s.anchor = int64_t(g_guest_frame.load(std::memory_order_relaxed));
  s.frames = 0;
  s.ui = true;
  s.status = "recording " + p.stem().string();
  REXLOG_INFO("[wwe2k14][cinecam][path] panel: recording {} from frame {}", p.string(), s.anchor);
}

inline void UiPlayShot(PathState& s, FreeCam& f, const std::string& shot) {
  UiStopRecord(s);
  const std::filesystem::path p = UserDir() / "paths" / (shot + ".txt");
  if (!LoadPathFile(p, s.play) || s.play.empty()) {
    s.playing = false;
    s.status = "could not read " + shot;
    return;
  }
  // Shift the shot so its first record is rel 0: it starts the moment the button is clicked.
  const int64_t first = s.play.front().rel;
  for (PathPose& q : s.play) q.rel -= first;
  s.cursor = 0;
  s.play_ended = false;
  s.frames = s.exact = s.held = 0;
  s.anchor = int64_t(g_guest_frame.load(std::memory_order_relaxed));
  s.playing = true;
  s.ui = true;
  f.seeded = true;
  s.status = "playing " + shot + " (" + std::to_string(s.play.size()) + " frames)";
  REXLOG_INFO("[wwe2k14][cinecam][path] panel: playing {} ({} frames) from frame {}", p.string(), s.play.size(),
              s.anchor);
}

// ---- Keyframed shots (Session 35, SESSION35_PLAN.md; the engine is src\cinecam_keys.h) ---------------------------
// One shot is open at a time (the timeline panel edits it). The playhead moves in one of three ways:
//   preview  the host clock x rate (0.25 / 0.5 / 1): designing over a frozen or running scene;
//   take     guest frames from the moment Play (take) is pressed, 1 / [0x8392DC40] s per frame (the frame-rate word,
//            60 in the match: run 2558);
//   re-shoot --wwe2k14_cinecam_path_play=NAME.shot: as a take, from the path anchor (frame-exact against a replay).
// While a shot plays, and once after every playhead jump (scrub, go to key, a script's `time S`), the free camera
// takes the shot's pose for every keyed track; otherwise it flies by hand as before, so a pose can be set up and keyed
// (K). Targets: a character slot's position vector [[0x836F38D0 + 4 * slot] + 176] (x, y, z: world space at about
// pelvis height, run 2558: two look-at shots landed on the wrestlers; docs\systems\determinism.md Result 5 for the
// pointer) or a pin, smoothed with a critically damped follow of time constant shot.follow (per guest frame in takes,
// so re-shoots stay exact). Guest thread (the hand-off) and UI thread (panel, keys, script commands), under
// FreeCam::mutex.
namespace k = wwe2k14::cinecam::keys;

constexpr uint32_t kFrameRateWord = 0x8392DC40;  // states.toml frame_rate (60 / 30)
constexpr uint32_t kSlotTablePtr = 0x836F38D0;   // docs\systems\match_state.md: 9 character pointers

enum class ShotPlay { kStopped, kPreview, kTake };

struct ShotFollow {
  bool valid = false;
  float x[3] = {}, v[3] = {};
};

struct ShotState {
  k::Shot shot;
  std::string name = "untitled";
  bool dirty = false;  // changed since the last save / load
  double playhead = 0;
  ShotPlay play = ShotPlay::kStopped;
  bool loop = false;
  float rate = 1.0f;
  bool apply = false;          // take the pose at the playhead once (a jump)
  bool from_path = false;      // played by --wwe2k14_cinecam_path_play=NAME.shot
  bool path_pending = false;   // ... waiting for the path anchor
  uint64_t last_frame = ~0ull;
  int sel_track = -1, sel_key = -1;
  ShotFollow follow[3];        // aim, move, focus targets
  float tgt[3][3] = {};        // the last resolved (smoothed) target positions
  bool tgt_ok[3] = {};
  float aspect = 16.0f / 9.0f; // from the panel's display size
  bool freeze = false;             // CineCam asks for the scene freeze (src\scene_freeze.h)
  bool take_on_release = false;    // "Release + take": the take starts on the first frame that runs again
  std::string status;
  bool text_active = false;        // a panel text field is being typed in (no hotkeys)
  bool record_take = false;        // a take also records the per-frame path (shot_NN.txt: the "bake")
  int take_rec = 0;                // 0 none, 1 recording the take, 2 close the file at the next frame
  std::vector<std::string> files;  // *.shot in the shots folder (panel list)
  std::chrono::steady_clock::time_point files_scanned;
  uint64_t evals = 0;
};

inline ShotState& Sh() {
  static ShotState s;
  return s;
}

inline std::filesystem::path ShotDir() { return UserDir() / "shots"; }
inline std::filesystem::path ShotFile(const std::string& name) {
  std::filesystem::path p(name);
  if (p.extension() != ".shot") p += ".shot";
  if (p.is_relative()) p = ShotDir() / p;
  return p.lexically_normal();
}

inline const k::Target& RoleTarget(const k::Shot& s, int role) { return role == 0 ? s.aim : role == 1 ? s.move : s.focus; }

// A target's raw world position now (guest thread; fault-safe reads). False when it cannot be read.
inline bool ReadTarget(const uint8_t* base, const k::Target& t, float out[3]) {
  if (t.kind == k::TargetKind::kPin) {
    std::memcpy(out, t.pin, sizeof(t.pin));
    return true;
  }
  if (t.kind != k::TargetKind::kSlot) return false;
  uint32_t c = 0, v = 0;
  if (!PathRead(base, kSlotTablePtr + 4 * uint32_t(t.slot), 4, &c) || !PathRead(base, c + 176, 4, &v)) return false;
  uint32_t w[3];
  for (int i = 0; i < 3; ++i) {
    if (!PathRead(base, v + 4 * i, 4, &w[i])) return false;
    out[i] = std::bit_cast<float>(w[i]);
    if (!std::isfinite(out[i]) || std::abs(out[i]) > 1e6f) return false;
  }
  return true;
}

// Critically damped follow (a camera operator's lag), time constant tau; tau <= 0 = exact.
inline void Follow(ShotFollow& fo, const float target[3], float dt, float tau) {
  if (!fo.valid || tau <= 0 || dt <= 0 || dt > 0.25f) {
    std::memcpy(fo.x, target, sizeof(fo.x));
    std::memset(fo.v, 0, sizeof(fo.v));
    fo.valid = true;
    return;
  }
  const float w = 2.0f / tau;
  const int steps = std::max(1, int(std::ceil(dt / 0.004f)));  // small steps: stable at any frame time
  const float h = dt / float(steps);
  for (int n = 0; n < steps; ++n) {
    for (int i = 0; i < 3; ++i) {
      fo.v[i] += h * (w * w * (target[i] - fo.x[i]) - 2 * w * fo.v[i]);
      fo.x[i] += h * fo.v[i];
    }
  }
}

// The free camera's state as an engine pose (caller holds the mutex).
inline k::Pose CamPose(const FreeCam& f) {
  k::Pose p;
  std::memcpy(p.pos, f.pos, sizeof(p.pos));
  p.yaw = f.yaw, p.pitch = f.pitch, p.roll = f.roll, p.fov = f.fov;
  p.focus_cm = f.dof_manual ? f.dof_focus : (f.dof_auto_focus > 0 ? f.dof_auto_focus : f.dof_focus);
  p.aperture = f.dof_dist[0] / kDofDefaultDist[0];
  return p;
}

inline void SetCamPose(FreeCam& f, const k::Pose& p) {
  if (p.has[k::kMove]) std::memcpy(f.pos, p.pos, sizeof(f.pos));
  if (p.has[k::kAim]) f.yaw = p.yaw, f.pitch = p.pitch;
  if (p.has[k::kRoll]) f.roll = p.roll;
  if (p.has[k::kLens]) f.fov = f.fov_target = p.fov;
  if (p.has[k::kFocus]) {
    f.dof_mode = 2;
    f.dof_manual = true;
    f.dof_focus = p.focus_cm;
    for (int i = 0; i < 4; ++i) f.dof_dist[i] = kDofDefaultDist[i] * p.aperture;
  }
  std::memset(f.vel, 0, sizeof(f.vel));
  f.roll_rate = f.look_yaw = f.look_pitch = 0;
}

inline const float* TargetOrNull(const ShotState& s, int role) { return s.tgt_ok[role] ? s.tgt[role] : nullptr; }

inline void ShotStart(ShotState& s, ShotPlay how, bool from_start);

// Every hand-off while the free camera is on (guest thread, mutex held), after the manual input step.
inline void ShotTick(const uint8_t* base, FreeCam& f, float host_dt) {
  ShotState& s = Sh();
  const uint64_t frame = g_guest_frame.load(std::memory_order_relaxed);
  // Session 36: "Release + take" starts the take in the first hand-off of a running frame (the release applies at
  // the start of that frame's owner update, before the scene manager's hand-off), so it lines up with the scene.
  if (s.take_on_release && !freeze::Active()) {
    s.take_on_release = false;
    ShotStart(s, ShotPlay::kTake, true);
  }
  const bool new_frame = frame != s.last_frame;
  s.last_frame = frame;
  const bool timed_by_frames = s.play == ShotPlay::kTake;
  float frame_dt = 1.0f / 60;
  if (uint32_t rate = 0; PathRead(base, kFrameRateWord, 4, &rate) && (rate == 30 || rate == 60)) frame_dt = 1.0f / float(rate);
  // Targets (always, so the panel can key against them).
  for (int r = 0; r < 3; ++r) {
    const k::Target& t = RoleTarget(s.shot, r);
    float raw[3];
    if (!t.on() || !ReadTarget(base, t, raw)) {
      s.tgt_ok[r] = false;
      s.follow[r].valid = false;
      continue;
    }
    const float dt = timed_by_frames ? (new_frame ? frame_dt : 0.0f) : host_dt;
    Follow(s.follow[r], raw, dt, t.kind == k::TargetKind::kSlot ? s.shot.follow : 0.0f);
    std::memcpy(s.tgt[r], s.follow[r].x, sizeof(s.tgt[r]));
    s.tgt_ok[r] = true;
  }
  if (s.take_rec == 1 && s.play != ShotPlay::kTake) s.take_rec = 2;  // paused or stopped by hand
  if (s.take_rec == 2 && new_frame) {
    UiStopRecord(P());
    s.take_rec = 0;
    s.status = "take recorded: " + P().status;
  }
  if (s.play == ShotPlay::kStopped && !s.apply) return;
  if (s.play == ShotPlay::kPreview && s.evals > 0) {  // the first pose is the start
    s.playhead += double(host_dt) * s.rate;
  } else if (s.play == ShotPlay::kTake && new_frame && s.evals > 0) {
    s.playhead += frame_dt;
  }
  const double end = std::max(s.shot.length, k::LastKeyTime(s.shot));
  if (s.play != ShotPlay::kStopped && s.playhead >= end) {
    if (s.loop && s.play == ShotPlay::kPreview && end > 0) {
      s.playhead = std::fmod(s.playhead, end);
    } else {
      s.playhead = end;
      REXLOG_INFO("[wwe2k14][cinecam][shot] '{}' ended at {:.3f} s (frame {}, {} poses)", s.name, end, frame, s.evals);
      if (s.take_rec == 1) s.take_rec = 2;  // this frame's PathFrame writes the last pose
      s.play = ShotPlay::kStopped;
      s.status = "shot ended: you have the camera";
      s.apply = true;  // the last pose once more
    }
  }
  const k::Pose p = k::Evaluate(s.shot, s.playhead, CamPose(f), TargetOrNull(s, 0), TargetOrNull(s, 1),
                                TargetOrNull(s, 2), s.aspect);
  SetCamPose(f, p);
  ++s.evals;
  s.apply = false;
  if (REXCVAR_GET(wwe2k14_cinecam_path_log)) {
    REXLOG_INFO("[wwe2k14][cinecam][shot] t={:.4f} frame={} pos=({:.2f}, {:.2f}, {:.2f}) yaw={:.3f} pitch={:.3f} "
                "roll={:.3f} fov={:.3f} focus={:.1f} ap={:.3f} aim_target={}",
                s.playhead, frame, p.pos[0], p.pos[1], p.pos[2], p.yaw, p.pitch, p.roll, p.fov, p.focus_cm, p.aperture,
                s.tgt_ok[0] ? fmt::format("({:.2f}, {:.2f}, {:.2f})", s.tgt[0][0], s.tgt[0][1], s.tgt[0][2])
                            : std::string("none"));
  }
}

// Starts playing (UI thread or the path anchor; mutex held).
inline void ShotStart(ShotState& s, ShotPlay how, bool from_start) {
  if (from_start || s.playhead >= std::max(s.shot.length, k::LastKeyTime(s.shot))) s.playhead = 0;
  s.play = how;
  s.evals = 0;
  for (auto& fo : s.follow) fo.valid = false;  // targets start exactly where they are
  s.status = how == ShotPlay::kTake ? "take: timed in game frames" : "preview";
  if (how == ShotPlay::kTake && s.record_take && !s.from_path && !P().rec) {
    UiStartRecord(P());  // the per-frame path of this take (Session 14 format) as the next shot_NN.txt
    s.take_rec = P().rec ? 1 : 0;
  }
  REXLOG_INFO("[wwe2k14][cinecam][shot] '{}' {} from {:.3f} s at frame {}", s.name,
              how == ShotPlay::kTake ? "take" : "preview", s.playhead, g_guest_frame.load(std::memory_order_relaxed));
}

// Keys `track` (or every track, -1) at the playhead from the free camera (mutex held).
inline void ShotKey(ShotState& s, FreeCam& f, int track) {
  const k::Pose c = CamPose(f);
  int keyed = 0;
  for (int tr = 0; tr < k::kTrackCount; ++tr) {
    if (track >= 0 && tr != track) continue;
    // "Key all" skips the focus while the depth of field is not on (nothing to key).
    if (track < 0 && tr == k::kFocus && f.dof_mode != 2) continue;
    float v[3];
    k::CaptureValues(s.shot, tr, s.playhead, c, TargetOrNull(s, 0), TargetOrNull(s, 1), TargetOrNull(s, 2), s.aspect, v);
    const size_t i = k::SetKey(s.shot, tr, s.playhead, v);
    s.sel_track = tr, s.sel_key = int(i);
    ++keyed;
  }
  s.shot.length = std::max(s.shot.length, s.playhead);
  s.dirty = true;
  s.status = fmt::format("keyed {} at {:.2f} s", track < 0 ? std::string("all") : k::kTrackNames[track], s.playhead);
  REXLOG_INFO("[wwe2k14][cinecam][shot] key {} ({} track(s)) at {:.3f} s: pos ({:.1f}, {:.1f}, {:.1f}) yaw {:.2f} "
              "pitch {:.2f} roll {:.2f} fov {:.2f}",
              track < 0 ? "all" : k::kTrackNames[track], keyed, s.playhead, c.pos[0], c.pos[1], c.pos[2], c.yaw,
              c.pitch, c.roll, c.fov);
}

inline void ShotSave(ShotState& s, const std::string& name) {
  std::error_code ec;
  std::filesystem::create_directories(ShotDir(), ec);
  const auto p = ShotFile(name);
  if (k::SaveShot(s.shot, p.string())) {
    s.name = p.stem().string();
    s.dirty = false;
    s.status = "saved " + s.name;
  } else {
    s.status = "could not save " + p.string();
  }
  s.files_scanned = {};
  REXLOG_INFO("[wwe2k14][cinecam][shot] {}", s.status);
}

inline bool ShotLoad(ShotState& s, const std::string& name) {
  const auto p = ShotFile(name);
  std::string err;
  k::Shot shot;
  if (!k::LoadShot(p.string(), shot, err)) {
    s.status = err;
    REXLOG_WARN("[wwe2k14][cinecam][shot] {}", err);
    return false;
  }
  s.shot = std::move(shot);
  s.name = p.stem().string();
  s.dirty = false;
  s.playhead = 0;
  s.play = ShotPlay::kStopped;
  s.sel_track = s.sel_key = -1;
  for (auto& fo : s.follow) fo.valid = false;
  size_t n = 0;
  for (const auto& kk : s.shot.keys) n += kk.size();
  s.status = fmt::format("loaded {} ({} keys, {:.2f} s)", s.name, n, s.shot.length);
  REXLOG_INFO("[wwe2k14][cinecam][shot] {}", s.status);
  return true;
}

// Freeze the scene for designing (Session 36: the true freeze, shared with photo mode; src\scene_freeze.h).
inline void ShotFreeze(ShotState& s, bool on) {
  if (on == s.freeze) return;
  s.freeze = on;
  freeze::Request(freeze::kCineCam, on);
  REXLOG_INFO("[wwe2k14][cinecam][shot] freeze scene {}", on ? "on" : "off");
}

// Session 36: let the scene run again (the auto freeze and CineCam's own); `take` = the open shot's take starts on the
// first frame that runs (ShotTick). Mutex held.
inline void ShotRelease(ShotState& s, bool take) {
  s.take_on_release = take;
  if (take) s.play = ShotPlay::kStopped, s.playhead = 0, s.apply = true;  // sits on its first frame until then
  ShotFreeze(s, false);
  freeze::ReleaseAuto();
  REXLOG_INFO("[wwe2k14][cinecam][shot] release{} at frame {}", take ? " + take" : "",
              g_guest_frame.load(std::memory_order_relaxed));
}

// Session 36: the auto freeze setting as a checkbox (both panels and the setup panel).
inline void AutoFreezeCheckbox() {
  bool on = REXCVAR_GET(wwe2k14_cinecam_auto_freeze);
  if (ImGui::Checkbox("Freeze at the start of each entrance", &on))
    rex::cvar::SetFlagByName("wwe2k14_cinecam_auto_freeze", on ? "true" : "false");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("The scene (trons and sound too) stops on each wrestler's first entrance frame, the same moment "
                      "every time. Release, or Release + take to start your shot with it.");
}

inline void ShotSetTarget(ShotState& s, int role, const k::Target& t);  // with the timeline panel, below

// Script / test commands (`cvar wwe2k14_cinecam_timeline_cmd "..."`, UI thread, mutex held); several separated by ';'.
inline void ShotCommand(ShotState& s, FreeCam& f, const std::string& text) {
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t semi = text.find(';', pos);
    if (semi == std::string::npos) semi = text.size();
    std::string cmd = text.substr(pos, semi - pos);
    pos = semi + 1;
    while (!cmd.empty() && cmd.front() == ' ') cmd.erase(cmd.begin());
    if (cmd.empty()) continue;
    char w[5][64] = {};
    const int n = std::sscanf(cmd.c_str(), "%63s %63s %63s %63s %63s", w[0], w[1], w[2], w[3], w[4]);
    const std::string c = w[0];
    auto track_of = [](const char* name) { return k::TrackByName(name); };
    if (c == "key") {
      ShotKey(s, f, n >= 2 ? track_of(w[1]) : -1);
    } else if (c == "time" && n >= 2) {
      s.playhead = std::max(0.0, std::atof(w[1]));
      s.apply = true;
    } else if (c == "preview" || c == "play") {
      ShotStart(s, ShotPlay::kPreview, n >= 2 && std::string(w[1]) == "start");
    } else if (c == "take") {
      ShotStart(s, ShotPlay::kTake, true);
    } else if (c == "stop") {
      s.play = ShotPlay::kStopped;
    } else if (c == "rate" && n >= 2) {
      s.rate = std::clamp(float(std::atof(w[1])), 0.05f, 4.0f);
    } else if (c == "loop" && n >= 2) {
      s.loop = std::atoi(w[1]) != 0;
    } else if (c == "length" && n >= 2) {
      s.shot.length = std::max(0.1, std::atof(w[1]));
    } else if (c == "constant" && n >= 2) {
      s.shot.constant_speed = std::atoi(w[1]) != 0;
      s.shot.Touch();
    } else if (c == "follow" && n >= 2) {
      s.shot.follow = std::max(0.0f, float(std::atof(w[1])));
    } else if (c == "target" && n >= 3) {
      k::Target t;
      const std::string kind = w[2];
      if (kind == "slot" && n >= 4) {
        t.kind = k::TargetKind::kSlot, t.slot = std::clamp(std::atoi(w[3]), 0, 8);
      } else if (kind == "here") {  // a pin at the focus distance straight ahead
        float fw[3], r[3], d[3];
        Axes(f.yaw, f.pitch, fw, r, d);
        const float dist = CamPose(f).focus_cm;
        t.kind = k::TargetKind::kPin;
        for (int i = 0; i < 3; ++i) t.pin[i] = f.pos[i] + fw[i] * dist;
      } else if (kind == "pin") {
        float v[3] = {};
        std::sscanf(cmd.c_str(), "%*s %*s %*s %f %f %f", &v[0], &v[1], &v[2]);
        t.kind = k::TargetKind::kPin;
        std::memcpy(t.pin, v, sizeof(v));
      }
      const std::string role = w[1];
      ShotSetTarget(s, role == "aim" ? 0 : role == "move" ? 1 : 2, t);
    } else if (c == "ease" && n >= 4) {
      const int tr = track_of(w[1]), i = std::atoi(w[2]);
      if (tr >= 0 && i >= 0 && size_t(i) < s.shot.keys[tr].size()) s.shot.keys[tr][i].ease = float(std::atof(w[3]));
      s.shot.Touch();
    } else if (c == "curve" && n >= 4) {
      const int tr = track_of(w[1]), i = std::atoi(w[2]);
      for (int cv = 0; cv < 3 && tr >= 0 && i >= 0 && size_t(i) < s.shot.keys[tr].size(); ++cv) {
        if (std::string(w[3]) == k::kCurveNames[cv]) s.shot.keys[tr][i].curve = k::Curve(cv);
      }
      s.shot.Touch();
    } else if ((c == "set" || c == "add") && n >= 5) {  // set|add TRACK I DIM VALUE
      const int tr = track_of(w[1]), i = std::atoi(w[2]), dim = std::atoi(w[3]);
      if (tr >= 0 && i >= 0 && size_t(i) < s.shot.keys[tr].size() && dim >= 0 && dim < k::kTrackDims[tr]) {
        float& v = s.shot.keys[tr][size_t(i)].v[dim];
        v = (c == "add" ? v : 0.0f) + float(std::atof(w[4]));
        s.shot.Touch();
        s.apply = s.play == ShotPlay::kStopped;
      }
    } else if (c == "clear") {
      s.shot = k::Shot{};
      s.name = "untitled";
      s.playhead = 0;
      s.play = ShotPlay::kStopped;
      s.sel_track = s.sel_key = -1;
    } else if (c == "save") {
      ShotSave(s, n >= 2 ? w[1] : s.name);
    } else if (c == "load" && n >= 2) {
      ShotLoad(s, w[1]);
    } else if (c == "freeze" && n >= 2) {
      ShotFreeze(s, std::atoi(w[1]) != 0);
    } else if (c == "release") {  // Session 36: release [take]
      ShotRelease(s, n >= 2 && std::string(w[1]) == "take");
    } else {
      REXLOG_WARN("[wwe2k14][cinecam][shot] unknown command '{}'", cmd);
      continue;
    }
    s.dirty = s.dirty || c == "key" || c == "length" || c == "constant" || c == "target" || c == "ease" ||
              c == "curve" || c == "set" || c == "add";
    REXLOG_INFO("[wwe2k14][cinecam][shot] command '{}' (playhead {:.3f} s)", cmd, s.playhead);
  }
}

// --wwe2k14_cinecam_path_play=NAME.shot (PathInit / PathTick; guest thread, mutex held).
inline bool ShotPathInit(const std::string& file) {
  ShotState& s = Sh();
  s.from_path = true;
  s.path_pending = ShotLoad(s, file);
  return s.path_pending;
}
inline void ShotPathAnchor(FreeCam& f) {
  ShotState& s = Sh();
  if (!s.path_pending) return;
  s.path_pending = false;
  f.active = true;
  f.seeded = false;  // seed from the game's view first: tracks without keys keep it
  f.last = std::chrono::steady_clock::now();
  ShotStart(s, ShotPlay::kTake, true);
}

// Re-shoots (src\reshoot.h, any thread): the path play is a keyframed shot, and it has played to its end.
inline bool ShotPathActive() {
  std::lock_guard lock(F().mutex);
  return Sh().from_path;
}
inline bool ShotPathEnded() {
  std::lock_guard lock(F().mutex);
  const ShotState& s = Sh();
  return s.from_path && !s.path_pending && s.evals > 0 && s.play == ShotPlay::kStopped;
}

// --wwe2k14_cinecam_test_move: VX,VY,VZ,VYAW per second (host time step dt). Caller holds the mutex.
inline void TestMove(FreeCam& f, float dt) {
  const std::string spec = REXCVAR_GET(wwe2k14_cinecam_test_move);
  float v[4];
  if (spec.empty() || std::sscanf(spec.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) != 4) return;
  for (int i = 0; i < 3; ++i) f.pos[i] += v[i] * dt;
  f.yaw += v[3] * dt;
}

// One frame of the free camera at the hand-off (guest thread): input -> state -> the slot (vb) and mgr +1488 / +1552.
inline void ApplyFreeCam(PPCContext& ctx, uint8_t* base, uint32_t mgr, uint32_t vb) {
  using VK = rex::ui::VirtualKey;
  FreeCam& f = F();
  float m[16];
  float fov;
  bool log_first = false;
  int64_t path_rel = INT64_MIN;
  {
    std::lock_guard lock(f.mutex);
    if (!f.active) return;
    if (f.reset_hard) {
      f.reset_hard = false;
      const uint32_t hard = FindHardCam(base, mgr);
      REXLOG_INFO("[wwe2k14][cinecam] reset to the hard cam: {}", hard ? "game camera found" : "none (current view)");
      Seed(f, base, hard ? hard : vb);
      log_first = !hard;
    } else if (!f.seeded) {
      Seed(f, base, vb);
      log_first = true;
    }
    {
      const std::string pose = REXCVAR_GET(wwe2k14_cinecam_test_pose);
      static std::string applied;
      float v[6];
      if (!pose.empty() && pose != applied &&
          std::sscanf(pose.c_str(), "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
        applied = pose;
        f.pos[0] = v[0], f.pos[1] = v[1], f.pos[2] = v[2];
        f.yaw = v[3], f.pitch = v[4], f.roll = 0;
        f.fov = f.fov_target = v[5];
        std::memset(f.vel, 0, sizeof(f.vel));
        REXLOG_INFO("[wwe2k14][cinecam] test pose applied: {}", pose);
      }
    }
    // Steps use the running average of the frame time: the hand-off's own timing jitters (it runs on the game's
    // thread), and uneven steps read as judder (Jarrod, run 934: "a jagged vibe"). A gap over 0.1 s (a load, a
    // pause) restarts the average.
    const auto now = std::chrono::steady_clock::now();
    const float raw_dt = std::chrono::duration<float>(now - f.last).count();
    f.last = now;
    if (raw_dt > 0.1f || raw_dt <= 0) {
      f.dt_avg = 1.0f / 60;
    } else {
      f.dt_avg += (raw_dt - f.dt_avg) * 0.05f;
    }
    const float dt = f.dt_avg;
    const float sm = std::clamp(f.smoothing, 0.0f, 1.0f);
    // Ctrl: fine control (slow moves, finer zoom, mouse look x0.25 for framing when zoomed in; Jarrod, run 935).
    const bool fine = KeyDown(f, VK::kControl) || KeyDown(f, VK::kLControl) || KeyDown(f, VK::kRControl);
    // Look (right mouse button held).
    const float look_scale = f.sensitivity * (fine ? 0.25f : 1.0f);
    f.look_yaw += f.mouse_dx * look_scale;
    f.look_pitch -= f.mouse_dy * look_scale;
    // Session 33: the right stick looks at up to 90 degrees per second (Ctrl / fine: a quarter).
    f.look_yaw += f.pad_look_x * 90.0f * dt * (fine ? 0.25f : 1.0f);
    f.look_pitch += f.pad_look_y * 90.0f * dt * (fine ? 0.25f : 1.0f);
    f.mouse_dx = f.mouse_dy = 0;
    {
      const float a = sm > 0 ? 1.0f - std::exp(-dt / (0.1f * sm)) : 1.0f;
      f.yaw += f.look_yaw * a;
      f.pitch = std::clamp(f.pitch + f.look_pitch * a, -89.0f, 89.0f);
      f.look_yaw -= f.look_yaw * a;
      f.look_pitch -= f.look_pitch * a;
    }
    if (f.yaw > 180) f.yaw -= 360;
    if (f.yaw < -180) f.yaw += 360;
    // Speed modifiers.
    float speed = f.speed;
    if (KeyDown(f, VK::kShift) || KeyDown(f, VK::kLShift) || KeyDown(f, VK::kRShift)) speed *= 4;
    if (fine) speed *= 0.25f;
    // Zoom: each wheel notch scales the FOV by 10%, with Ctrl by 2% (wheel away = zoom in).
    if (f.wheel) f.fov_target = std::clamp(f.fov_target * std::pow(fine ? 0.98f : 0.9f, float(f.wheel)), 2.0f, 120.0f);
    // Session 33: the bumpers zoom (RB in, LB out) by half / double the FOV per second.
    if (f.pad_zoom != 0) f.fov_target = std::clamp(f.fov_target * std::pow(0.5f, f.pad_zoom * dt), 2.0f, 120.0f);
    f.wheel = 0;
    f.fov = Ease(f.fov, f.fov_target, dt, 0.15f * sm);
    // Move: W/S along the view, A/D left / right (level), E/Q world up / down (world Y is down).
    float fw[3], r[3], d[3];
    Axes(f.yaw, f.pitch, fw, r, d);
    const bool key_move = KeyDown(f, VK::kW) || KeyDown(f, VK::kS) || KeyDown(f, VK::kA) || KeyDown(f, VK::kD) ||
                          KeyDown(f, VK::kE) || KeyDown(f, VK::kQ);
    const float mf = float(KeyDown(f, VK::kW)) - float(KeyDown(f, VK::kS)) + f.pad_move_y;
    const float mr = float(KeyDown(f, VK::kD)) - float(KeyDown(f, VK::kA)) + f.pad_move_x;
    const float mu = float(KeyDown(f, VK::kE)) - float(KeyDown(f, VK::kQ)) + f.pad_up;
    float dir[3];
    for (int i = 0; i < 3; ++i) dir[i] = mf * fw[i] + mr * r[i];
    dir[1] -= mu;
    // One speed in every direction (a diagonal was 1.4x before); a stick pushed part way moves slower (Session 33).
    const float len = std::sqrt(Dot3(dir, dir));
    for (int i = 0; i < 3; ++i) {
      const float target = (len > 1.0f || key_move) && len > 0 ? dir[i] / len * speed : dir[i] * speed;
      f.vel[i] = Ease(f.vel[i], target, dt, 0.3f * sm);
      f.pos[i] += f.vel[i] * dt;
    }
    // Roll: X right / Z left at 45 degrees per second (eased); R resets it.
    f.roll_rate = Ease(f.roll_rate, 45.0f * (float(KeyDown(f, VK::kX)) - float(KeyDown(f, VK::kZ))), dt, 0.3f * sm);
    f.roll += f.roll_rate * dt;
    if (KeyDown(f, VK::kR)) {
      f.roll = 0;
      f.roll_rate = 0;
    }
    TestMove(f, dt);
    ShotTick(base, f, dt);  // Session 35: a keyframed shot sets the keyed tracks
    path_rel = PathActive() ? PathFrame(f) : INT64_MIN;
    ++f.frames;
    BuildMatrix(f, m);
    fov = f.fov;
    std::memcpy(f.last_m, m, sizeof(m));
    f.last_fov = fov;
    f.have_last = true;
  }
  if (path_rel != INT64_MIN && REXCVAR_GET(wwe2k14_cinecam_path_log)) {
    REXLOG_INFO("[wwe2k14][cinecam][path] {} rel={} frame={} m=[{:.4f} {:.4f} {:.4f}] [{:.4f} {:.4f} {:.4f}] "
                "[{:.4f} {:.4f} {:.4f}] [{:.3f} {:.3f} {:.3f}] fov={:.4f}",
                P().playing ? "play" : "rec", path_rel, g_guest_frame.load(std::memory_order_relaxed), m[0], m[1],
                m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14], fov);
  }
  if (log_first) {
    // The rebuilt matrix next to the game's (they should match): a check of BuildMatrix / Seed.
    LogMatrix(base, vb, "free camera seed: game");
    REXLOG_INFO("[wwe2k14][cinecam] free camera seed: rebuilt [{:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f}] "
                "[{:.3f} {:.3f} {:.3f}] [{:.3f} {:.3f} {:.3f}]",
                m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14]);
  }
  // Commit through the game's own setter (it also writes the two derived rows), on a scratch copy on the guest stack
  // well below the caller's frame (16-byte aligned for the setter's lvx128).
  const uint32_t scratch = (ctx.r1.u32 - 1024) & ~15u;
  for (int i = 0; i < 16; ++i) StoreGuestF32(base, scratch + 4 * i, m[i]);
  const PPCContext saved = ctx;
  ctx.r3.u64 = vb;
  ctx.r4.u64 = scratch;
  sub_82280108(ctx, base);
  ctx = saved;
  StoreGuestF32(base, vb + 128, fov);
  // Keep the composer's copies (0x823C88E4..0x823C890C) in step: matrix -> +1488, FOV -> +1552.
  std::memcpy(base + mgr + 1488, base + vb, 64);
  std::memcpy(base + mgr + 1552, base + vb + 128, 4);
}

// Caller holds the mutex. Releases the mouse after a right-button look (UI thread).
inline void StopLooking(FreeCam& f) {
  if (!f.looking || !f.window) return;
  f.looking = false;
  f.window->SetRelativeMouseMode(false);
  f.relative = false;
  f.window->ReleaseMouse();
  f.window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kVisible);
}

// Full arena (see the header). Guest thread, at every hand-off of the main manager, whether or not the free camera
// is on (so the key is put back after F8 off). Only a live game camera (vtable 0x8201D820) is restored.
inline void UpdateFullArena(uint8_t* base, uint32_t mgr) {
  FreeCam& f = F();
  std::lock_guard lock(f.mutex);
  const uint32_t shown = LoadGuestU32(base, mgr + 1392);
  if (const int32_t mode = REXCVAR_GET(wwe2k14_cinecam_test_dof); mode >= 0 && mode <= 2) f.dof_mode = mode;
  if (const int32_t fa = REXCVAR_GET(wwe2k14_cinecam_test_full_arena); fa == 0 || fa == 1) f.full_arena = fa == 1;
  {
    static std::string applied;
    const std::string values = REXCVAR_GET(wwe2k14_cinecam_test_dof_values);
    float v[5];
    float focus = 0;
    if (!values.empty() && values != applied &&
        std::sscanf(values.c_str(), "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &focus) >= 5) {
      applied = values;
      for (int i = 0; i < 4; ++i) f.dof_dist[i] = v[i];
      f.dof_strength = v[4];
      f.dof_manual = focus > 0;
      if (focus > 0) f.dof_focus = focus;
      REXLOG_INFO("[wwe2k14][cinecam] test dof values: {}", values);
    }
  }
  const bool want = f.active && f.full_arena;
  auto restore = [&] {
    if (f.key_camera && LoadGuestU32(base, f.key_camera) == kGameCameraVtable &&
        LoadGuestU32(base, f.key_camera + 300) == 10) {
      const uint32_t be = std::byteswap(f.key_original);
      std::memcpy(base + f.key_camera + 300, &be, 4);
      REXLOG_INFO("[wwe2k14][cinecam] full arena off: camera {:08X} key back to {}", f.key_camera, f.key_original);
    }
    f.key_camera = 0;
  };
  if (f.key_camera && (!want || shown != f.key_camera)) restore();
  if (!want || !shown || f.key_camera == shown) return;
  if (LoadGuestU32(base, shown) != kGameCameraVtable || LoadGuestU32(base, shown + 300) != 0) return;
  f.key_camera = shown;
  f.key_original = 0;
  const uint32_t be = std::byteswap(uint32_t(10));
  std::memcpy(base + shown + 300, &be, 4);
  REXLOG_INFO("[wwe2k14][cinecam] full arena on: hard cam {:08X} key 0 -> 10", shown);
}

inline void ShotHotkey(rex::ui::VirtualKey vk, bool shift);  // Session 35: the timeline's keys (defined below)

// Window input (UI thread). Sits above the ImGui drawer (z 64) and never marks events handled (the game ignores the
// keyboard with ReXGlue's default mnk_mode=false; inspector.h).
class Listener final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    if (!e.prev_state()) ShotHotkey(e.virtual_key(), e.is_shift_pressed());  // Session 35: timeline keys
    SetKey(e, true);
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override { SetKey(e, false); }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (e.button() != rex::ui::MouseEvent::Button::kRight) return;
    FreeCam& f = F();
    std::lock_guard lock(f.mutex);
    if (!f.active || f.looking || !f.window) return;
    f.looking = true;
    f.prev_x = e.x();
    f.prev_y = e.y();
    f.mouse_dx = f.mouse_dy = 0;
    f.window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    f.window->CaptureMouse();
    f.relative = f.window->SetRelativeMouseMode(true);
    if (!f.relative) {
      static std::atomic<bool> warned{false};
      if (!warned.exchange(true)) {
        REXLOG_WARN("[wwe2k14][cinecam] relative mouse mode unavailable: mouse look uses cursor movement");
      }
    }
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override {
    if (e.button() != rex::ui::MouseEvent::Button::kRight) return;
    std::lock_guard lock(F().mutex);
    StopLooking(F());
  }
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    FreeCam& f = F();
    std::lock_guard lock(f.mutex);
    if (!f.looking) return;
    if (f.relative) {
      f.mouse_dx += e.dx();
      f.mouse_dy += e.dy();
    } else {
      f.mouse_dx += float(e.x() - f.prev_x);
      f.mouse_dy += float(e.y() - f.prev_y);
    }
    f.prev_x = e.x();
    f.prev_y = e.y();
  }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    FreeCam& f = F();
    std::lock_guard lock(f.mutex);
    if (!f.active || !e.scroll_y()) return;
    // Notches: Win32 reports multiples of 120 (WHEEL_DELTA); smaller values count as one notch each.
    const int32_t y = e.scroll_y();
    f.wheel += std::abs(y) >= 120 ? y / 120 : (y > 0 ? 1 : -1);
  }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    FreeCam& f = F();
    std::lock_guard lock(f.mutex);
    std::memset(f.key, 0, sizeof(f.key));
    StopLooking(f);
  }

 private:
  static void SetKey(rex::ui::KeyEvent& e, bool down) {
    const uint16_t vk = static_cast<uint16_t>(e.virtual_key());
    if (vk >= 256) return;
    std::lock_guard lock(F().mutex);
    F().key[vk] = down;
  }
};

inline Listener& L() {
  static Listener l;
  return l;
}

// A two-handle bar around the focus point (UI thread, inside the panel): the left handle is the distance in front of
// the focus, the right one the distance behind, on a log scale (0 .. max_cm each side). With inner_front / inner_behind
// (the sharp zone) it also draws the fall-off: sharp band, gradient to the handles, shaded full blur beyond. Returns
// true when a handle moved.
inline bool ZoneBar(const char* label, float* front, float* behind, float max_cm, const float* inner_front,
                    const float* inner_behind) {
  ImGui::PushID(label);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const float w = std::max(120.0f, ImGui::GetContentRegionAvail().x - 120.0f);
  const float h = ImGui::GetFrameHeight();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float cx = p.x + w * 0.5f, half = w * 0.5f - 6.0f;
  const float k = std::log1p(max_cm / 50.0f);
  auto to_x = [&](float d, float side) { return cx + side * half * std::log1p(std::max(d, 0.0f) / 50.0f) / k; };
  auto from_x = [&](float x, float side) {
    const float t = std::clamp(side * (x - cx) / half, 0.0f, 1.0f);
    return 50.0f * std::expm1(t * k);
  };
  const ImU32 bg = ImGui::GetColorU32(ImGuiCol_FrameBg);
  const ImU32 sharp = IM_COL32(120, 200, 255, 230), sharp_dim = IM_COL32(120, 200, 255, 70);
  const ImU32 blur = IM_COL32(40, 40, 60, 255);
  dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 3.0f);
  const float xl = to_x(*front, -1.0f), xr = to_x(*behind, 1.0f);
  if (inner_front && inner_behind) {
    // Full blur beyond the handles, a gradient from the sharp edge to each handle, the sharp band faint inside.
    const float il = to_x(*inner_front, -1.0f), ir = to_x(*inner_behind, 1.0f);
    dl->AddRectFilled(ImVec2(p.x + 2, p.y + 3), ImVec2(xl, p.y + h - 3), blur);
    dl->AddRectFilled(ImVec2(xr, p.y + 3), ImVec2(p.x + w - 2, p.y + h - 3), blur);
    dl->AddRectFilledMultiColor(ImVec2(xl, p.y + 3), ImVec2(il, p.y + h - 3), blur, sharp_dim, sharp_dim, blur);
    dl->AddRectFilledMultiColor(ImVec2(ir, p.y + 3), ImVec2(xr, p.y + h - 3), sharp_dim, blur, blur, sharp_dim);
    dl->AddRectFilled(ImVec2(il, p.y + 3), ImVec2(ir, p.y + h - 3), sharp_dim);
  } else {
    dl->AddRectFilled(ImVec2(xl, p.y + 3), ImVec2(xr, p.y + h - 3), sharp);
  }
  dl->AddLine(ImVec2(cx, p.y), ImVec2(cx, p.y + h), IM_COL32(255, 255, 255, 200), 2.0f);  // the focus
  bool changed = false;
  for (int i = 0; i < 2; ++i) {
    const float side = i == 0 ? -1.0f : 1.0f;
    float* v = i == 0 ? front : behind;
    const float x = to_x(*v, side);
    ImGui::SetCursorScreenPos(ImVec2(x - 7.0f, p.y));
    ImGui::InvisibleButton(i == 0 ? "front" : "behind", ImVec2(14.0f, h));
    const bool active = ImGui::IsItemActive();
    if (active) {
      *v = from_x(ImGui::GetIO().MousePos.x, side);
      changed = true;
    }
    if (active || ImGui::IsItemHovered()) {
      ImGui::SetTooltip("%s %s: %.0f cm", label, i == 0 ? "in front" : "behind", *v);
    }
    dl->AddRectFilled(ImVec2(x - 4.0f, p.y + 1), ImVec2(x + 4.0f, p.y + h - 1),
                      active ? IM_COL32(255, 255, 255, 255) : IM_COL32(220, 220, 220, 220), 2.0f);
  }
  ImGui::SetCursorScreenPos(p);
  ImGui::Dummy(ImVec2(w, h));
  ImGui::SameLine();
  ImGui::Text("%s", label);
  ImGui::TextDisabled("    in front %.0f cm      behind %.0f cm", *front, *behind);
  ImGui::PopID();
  return changed;
}

// The readout / tuning panel (shown while the free camera is on and --wwe2k14_cinecam_panel).
class Panel final : public rex::ui::ImGuiDialog {
 public:
  explicit Panel(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    FreeCam& f = F();
    if (!logged_) {
      logged_ = true;
      REXLOG_INFO("[wwe2k14][cinecam] panel shown (first draw), display {}x{}", int(io.DisplaySize.x),
                  int(io.DisplaySize.y));
    }
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 540), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CineCam")) {
      ImGui::End();
      return;
    }
    std::lock_guard lock(f.mutex);
    ImGui::Text("Free camera ON (F8 = off)");
    ImGui::Text("pos  %.0f  %.0f  %.0f  (Y down)", f.pos[0], f.pos[1], f.pos[2]);
    ImGui::Text("yaw %.1f  pitch %.1f  roll %.1f", f.yaw, f.pitch, f.roll);
    ImGui::Text("FOV %.1f deg (game %.1f)", f.fov, f.game_fov);
    ImGui::SliderFloat("speed", &f.speed, 25.0f, 3000.0f, "%.0f /s", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("mouse", &f.sensitivity, 0.02f, 0.5f, "%.2f deg/px");
    ImGui::SliderFloat("smoothing", &f.smoothing, 0.0f, 1.0f, "%.2f");
    ImGui::Checkbox("Full arena (removes culling)", &f.full_arena);
    AutoFreezeCheckbox();  // Session 36
    ImGui::Text("Depth of field:");
    ImGui::SameLine();
    ImGui::RadioButton("game", &f.dof_mode, 0);
    ImGui::SameLine();
    ImGui::RadioButton("off", &f.dof_mode, 1);
    ImGui::SameLine();
    ImGui::RadioButton("on", &f.dof_mode, 2);
    if (f.dof_mode == 2) {
      bool changed = f.dof_manual || f.dof_focus != kDofDefaultFocus || f.dof_strength != kDofDefaultStrength;
      for (int i = 0; i < 4; ++i) changed = changed || f.dof_dist[i] != kDofDefaultDist[i];
      if (changed) {
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) {
          for (int i = 0; i < 4; ++i) f.dof_dist[i] = kDofDefaultDist[i];
          f.dof_strength = kDofDefaultStrength;
          f.dof_manual = false;
          f.dof_focus = kDofDefaultFocus;
        }
      }
    }
    if (f.dof_mode == 2) {
      // Distances in cm around the focus (meaning from the game's formula; see the header). Director-shot defaults.
      ImGui::Checkbox("manual focus", &f.dof_manual);
      ImGui::SameLine();
      ImGui::Text("(auto: %.0f cm)", f.dof_auto_focus);
      if (f.dof_manual) ImGui::SliderFloat("focus (cm)", &f.dof_focus, 50.0f, 10000.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
      // Focus zone bars: centre = the focus, left = in front, right = behind (log scale, cm).
      if (ZoneBar("sharp", &f.dof_dist[0], &f.dof_dist[1], 20000.0f, nullptr, nullptr)) {
        f.dof_dist[2] = std::max(f.dof_dist[2], f.dof_dist[0]);  // full blur stays outside the sharp zone
        f.dof_dist[3] = std::max(f.dof_dist[3], f.dof_dist[1]);
      }
      if (ZoneBar("full blur", &f.dof_dist[2], &f.dof_dist[3], 20000.0f, &f.dof_dist[0], &f.dof_dist[1])) {
        f.dof_dist[2] = std::max(f.dof_dist[2], f.dof_dist[0]);
        f.dof_dist[3] = std::max(f.dof_dist[3], f.dof_dist[1]);
      }
      ImGui::SliderFloat("blur strength", &f.dof_strength, 0.0f, 0.01f, "%.5f", ImGuiSliderFlags_Logarithmic);
    }
    if (ImGui::Button("reset to hard cam")) f.reset_hard = true;
    if (ImGui::Button("FOV = game")) f.fov_target = f.game_fov;
    ImGui::SameLine();
    if (ImGui::Button("roll 0")) f.roll = 0;
    ImGui::SameLine();
    if (ImGui::Button("back to game view")) f.seeded = false;
    ImGui::TextDisabled("hold right mouse: look   W A S D: move");
    ImGui::TextDisabled("E / Q: up / down   Shift fast, Ctrl slow / fine");
    ImGui::TextDisabled("wheel: zoom (Ctrl: fine)   Z / X: roll   R: roll 0");
    // Shots (Session 14, for testing): Record / Stop, then one button per saved shot (plays it from the click).
    ImGui::Separator();
    PathState& s = P();
    if (s.shots_scanned == std::chrono::steady_clock::time_point{} ||
        std::chrono::steady_clock::now() - s.shots_scanned > std::chrono::seconds(2)) {
      ScanShots(s);
    }
    if (s.rec) {
      if (ImGui::Button("Stop")) UiStopRecord(s);
      ImGui::SameLine();
      ImGui::Text("recording %s  %llu frames", s.rec_path.stem().string().c_str(),
                  static_cast<unsigned long long>(s.frames));
    } else {
      if (ImGui::Button("Record")) UiStartRecord(s);
      if (s.playing && s.ui) {
        ImGui::SameLine();
        if (ImGui::Button("Stop shot")) {
          s.playing = false;
          s.status = "shot stopped: you have the camera";
        }
      }
    }
    ImGui::Text("Shots:");
    for (size_t i = 0; i < s.shots.size(); ++i) {
      ImGui::SameLine();
      const std::string label = s.shots[i].substr(5);  // "shot_03" -> "03"
      if (ImGui::Button(label.c_str())) UiPlayShot(s, f, s.shots[i]);
    }
    if (s.shots.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("none yet");
    }
    if (!s.status.empty()) ImGui::TextDisabled("%s", s.status.c_str());
    ImGui::End();
  }

 private:
  bool logged_ = false;
};

// ---- The timeline panel (Session 35 step 3) ----------------------------------------------------------------------
// A strip along the bottom of the screen while the free camera is on (--wwe2k14_cinecam_timeline, with the CineCam
// panel): transport, one lane per track (click a key to select it, drag it in time, double-click to go to it; click or
// drag the ruler or an empty spot to move the playhead), the selected key's values and curve, and the targets. Times
// snap to game frames (1/60 s). UI thread, FreeCam::mutex held while drawing.

// Focal length (35 mm full-frame equivalent, 24 mm tall) of a vertical FOV, and back (the game's FOV is vertical:
// PLAYBOOK Session 18, run 1395).
inline float FovToMm(float fov) { return 12.0f / std::tan(std::clamp(fov, 0.5f, 170.0f) * kDegToRad * 0.5f); }
inline float MmToFov(float mm) { return 2.0f * std::atan(12.0f / std::max(mm, 0.5f)) / kDegToRad; }
inline double SnapFrame(double t) { return std::max(0.0, std::round(t * 60.0) / 60.0); }

// Changes a target and keeps the shot looking the same at its keys: the pose at each key time of the track the target
// changes the meaning of (aim: yaw / pitch <-> framing; move: x y z <-> orbit; focus: distance <-> offset) is
// evaluated with the old target, then captured again with the new one (exact for pins; for a wrestler, against where
// they are now).
inline void ShotSetTarget(ShotState& s, int role, const k::Target& t) {
  const uint8_t* base = REX_KERNEL_MEMORY()->virtual_membase();
  const int track = role == 0 ? k::kAim : role == 1 ? k::kMove : k::kFocus;
  std::vector<k::Pose> poses;
  for (const k::Key& key : s.shot.keys[track]) {
    poses.push_back(k::Evaluate(s.shot, key.t, k::Pose{}, TargetOrNull(s, 0), TargetOrNull(s, 1), TargetOrNull(s, 2),
                                s.aspect));
  }
  (role == 0 ? s.shot.aim : role == 1 ? s.shot.move : s.shot.focus) = t;
  s.follow[role].valid = false;
  s.tgt_ok[role] = t.on() && ReadTarget(base, t, s.tgt[role]);
  for (size_t i = 0; i < poses.size(); ++i) {
    float v[3];
    k::CaptureValues(s.shot, track, s.shot.keys[track][i].t, poses[i], TargetOrNull(s, 0), TargetOrNull(s, 1),
                     TargetOrNull(s, 2), s.aspect, v);
    std::memcpy(s.shot.keys[track][i].v, v, sizeof(v));
  }
  s.shot.Touch();
  s.dirty = true;
  s.apply = true;
  s.status = "target: " + std::string(role == 0 ? "look at " : role == 1 ? "orbit around " : "focus on ") +
             (t.kind == k::TargetKind::kSlot ? "wrestler slot " + std::to_string(t.slot)
              : t.kind == k::TargetKind::kPin ? std::string("a pin") : std::string("nothing"));
  REXLOG_INFO("[wwe2k14][cinecam][shot] {} ({} key(s) converted)", s.status, poses.size());
}

// A pin straight ahead of the camera at its focus distance.
inline k::Target PinHere(const FreeCam& f) {
  float fw[3], r[3], d[3];
  Axes(f.yaw, f.pitch, fw, r, d);
  const float dist = CamPose(f).focus_cm;
  k::Target t;
  t.kind = k::TargetKind::kPin;
  for (int i = 0; i < 3; ++i) t.pin[i] = f.pos[i] + fw[i] * dist;
  return t;
}

inline std::string TargetLabel(const k::Target& t) {
  if (t.kind == k::TargetKind::kSlot) return "wrestler slot " + std::to_string(t.slot);
  if (t.kind == k::TargetKind::kPin) return fmt::format("pin ({:.0f}, {:.0f}, {:.0f})", t.pin[0], t.pin[1], t.pin[2]);
  return "nothing";
}

// Keyboard (UI thread, from the listener): K key all, Space play / pause, [ / ] previous / next key, , / . one frame
// (Shift: one second), Backspace delete the selected key. Only while the timeline is shown and no text field is
// being typed in.
inline void ShotHotkey(rex::ui::VirtualKey vk, bool shift) {
  using VK = rex::ui::VirtualKey;
  FreeCam& f = F();
  std::lock_guard lock(f.mutex);
  ShotState& s = Sh();
  if (!f.active || !f.timeline || s.text_active) return;
  auto jump = [&](double t) {
    s.play = ShotPlay::kStopped;
    s.playhead = SnapFrame(t);
    s.apply = true;
  };
  switch (vk) {
    case VK::kK: ShotKey(s, f, -1); break;
    case VK::kSpace:
      if (s.play != ShotPlay::kStopped) s.play = ShotPlay::kStopped;
      else ShotStart(s, ShotPlay::kPreview, false);
      break;
    case VK::kOem4:
    case VK::kOem6: {
      const bool next = vk == VK::kOem6;
      double best = next ? 1e18 : -1e18;
      for (const auto& kk : s.shot.keys) {
        for (const k::Key& key : kk) {
          if (next && key.t > s.playhead + 1e-6) best = std::min(best, key.t);
          if (!next && key.t < s.playhead - 1e-6) best = std::max(best, key.t);
        }
      }
      if (std::abs(best) < 1e17) jump(best);
      break;
    }
    case VK::kOemComma: jump(s.playhead - (shift ? 1.0 : 1.0 / 60)); break;
    case VK::kOemPeriod: jump(s.playhead + (shift ? 1.0 : 1.0 / 60)); break;
    case VK::kBack:
      if (s.sel_track >= 0 && s.sel_key >= 0 && size_t(s.sel_key) < s.shot.keys[s.sel_track].size()) {
        k::DeleteKey(s.shot, s.sel_track, size_t(s.sel_key));
        s.status = fmt::format("deleted a {} key", k::kTrackNames[s.sel_track]);
        s.sel_track = s.sel_key = -1;
        s.dirty = true;
        s.apply = true;
      }
      break;
    default: break;
  }
}

class Timeline final : public rex::ui::ImGuiDialog {
 public:
  explicit Timeline(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    FreeCam& f = F();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y;
    ImGui::SetNextWindowPos(ImVec2(8, std::max(8.0f, H - 348)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(std::min(W - 16, 1500.0f), 340), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CineCam timeline")) {
      ImGui::End();
      return;
    }
    std::lock_guard lock(f.mutex);
    ShotState& s = Sh();
    if (W > 0 && H > 0) s.aspect = W / H;
    s.text_active = io.WantTextInput;
    Transport(s, f);
    Lanes(s, f);
    if (ImGui::BeginTable("##below", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
      ImGui::TableNextColumn();
      Inspector(s, f);
      ImGui::TableNextColumn();
      Targets(s, f);
      ImGui::EndTable();
    }
    ImGui::End();
  }

 private:
  void Transport(ShotState& s, FreeCam& f) {
    const bool playing = s.play != ShotPlay::kStopped;
    if (ImGui::Button(playing ? "Pause" : "Play")) {
      if (playing) s.play = ShotPlay::kStopped;
      else ShotStart(s, ShotPlay::kPreview, false);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview the move on the real clock (Space)");
    ImGui::SameLine();
    if (ImGui::Button("Take")) ShotStart(s, ShotPlay::kTake, true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play from the start timed in game frames, for recording");
    ImGui::SameLine();
    ImGui::Checkbox("record path", &s.record_take);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A take also saves the camera's frame-by-frame path (shot_NN)");
    ImGui::SameLine();
    if (ImGui::Button("|<")) {
      s.play = ShotPlay::kStopped;
      s.playhead = 0;
      s.apply = true;
    }
    for (float r : {1.0f, 0.5f, 0.25f}) {
      ImGui::SameLine();
      if (ImGui::RadioButton(r == 1.0f ? "1x" : r == 0.5f ? "0.5x" : "0.25x", s.rate == r)) s.rate = r;
    }
    ImGui::SameLine();
    ImGui::Checkbox("loop", &s.loop);
    ImGui::SameLine();
    bool freeze = s.freeze;
    if (ImGui::Checkbox("freeze scene", &freeze)) ShotFreeze(s, freeze);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Freezes the scene while you design the move (trons and sound keep playing; photo mode holds those too)");
    ImGui::SameLine();
    ImGui::Text("   %.2f s  (frame %d)%s", s.playhead, int(std::lround(s.playhead * 60)),
                s.play == ShotPlay::kTake ? "  TAKE" : "");
    ImGui::SameLine();
    float len = float(s.shot.length);
    ImGui::SetNextItemWidth(110);
    if (ImGui::DragFloat("length", &len, 0.05f, 0.5f, 600.0f, "%.2f s")) {
      s.shot.length = std::max(double(len), k::LastKeyTime(s.shot));
      s.dirty = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("   lens %.0f mm (FOV %.1f)   focus %.1f m", FovToMm(f.fov), f.fov, CamPose(f).focus_cm / 100);
    // Session 36: the auto freeze at entrance starts, and releasing a frozen scene (optionally with the take).
    AutoFreezeCheckbox();
    if (freeze::Active()) {
      ImGui::SameLine();
      if (freeze::AutoFrozen())
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "   FROZEN at the start of slot %d's entrance",
                           freeze::g_auto_slot.load());
      else
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "   FROZEN");
      ImGui::SameLine();
      if (ImGui::Button("Release")) ShotRelease(s, false);
      ImGui::SameLine();
      if (ImGui::Button("Release + take")) ShotRelease(s, true);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The scene runs again and this shot's take starts on that same frame");
    } else if (s.take_on_release) {
      ImGui::SameLine();
      ImGui::TextDisabled("   take starts when the scene runs");
    }
    // File row.
    if (last_name_ != s.name) {
      last_name_ = s.name;
      std::snprintf(name_, sizeof(name_), "%s", s.name.c_str());
    }
    ImGui::SetNextItemWidth(180);
    ImGui::InputText("##name", name_, sizeof(name_));
    ImGui::SameLine();
    if (ImGui::Button("Save")) ShotSave(s, name_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160);
    if (ImGui::BeginCombo("##load", "Load...")) {
      if (s.files_scanned == std::chrono::steady_clock::time_point{} ||
          std::chrono::steady_clock::now() - s.files_scanned > std::chrono::seconds(2)) {
        s.files.clear();
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(ShotDir(), ec)) {
          if (e.path().extension() == ".shot") s.files.push_back(e.path().stem().string());
        }
        std::sort(s.files.begin(), s.files.end());
        s.files_scanned = std::chrono::steady_clock::now();
      }
      for (const std::string& n : s.files) {
        if (ImGui::Selectable(n.c_str())) ShotLoad(s, n);
      }
      if (s.files.empty()) ImGui::TextDisabled("no saved shots yet");
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("New")) ShotCommand(s, f, "clear");
    ImGui::SameLine();
    if (ImGui::Button("Key all (K)")) ShotKey(s, f, -1);
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", s.dirty ? "(not saved)  " : "", s.status.c_str());
  }

  void Lanes(ShotState& s, FreeCam& f) {
    constexpr float kLabelW = 150, kRulerH = 18, kLaneH = 24;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float lane_x0 = origin.x + kLabelW;
    const float lane_w = std::max(200.0f, ImGui::GetContentRegionAvail().x - kLabelW - 8);
    const double end = std::max({s.shot.length, k::LastKeyTime(s.shot), 1.0});
    auto tx = [&](double t) { return lane_x0 + float(t / end) * lane_w; };
    auto xt = [&](float x) { return SnapFrame(std::clamp(double(x - lane_x0) / lane_w, 0.0, 1.0) * end); };
    auto scrub = [&](float x) {
      s.play = ShotPlay::kStopped;
      s.playhead = xt(x);
      s.apply = true;
    };
    // Ruler.
    ImGui::SetCursorScreenPos(ImVec2(lane_x0, origin.y));
    ImGui::InvisibleButton("##ruler", ImVec2(lane_w, kRulerH));
    if (ImGui::IsItemActive()) scrub(ImGui::GetIO().MousePos.x);
    const double step = end <= 12 ? 1 : end <= 40 ? 5 : end <= 120 ? 10 : 30;
    for (double t = 0; t <= end + 1e-9; t += step) {
      const float x = tx(t);
      dl->AddLine(ImVec2(x, origin.y + kRulerH - 6), ImVec2(x, origin.y + kRulerH), IM_COL32(180, 180, 180, 200));
      dl->AddText(ImVec2(x + 2, origin.y), IM_COL32(180, 180, 180, 220), fmt::format("{}s", int(t)).c_str());
    }
    // Lanes.
    static const char* kLabels[k::kTrackCount] = {"Move", "Aim", "Roll", "Lens", "Focus"};
    float y = origin.y + kRulerH + 2;
    for (int tr = 0; tr < k::kTrackCount; ++tr) {
      ImGui::PushID(tr);
      std::string label = kLabels[tr];
      if (tr == k::kMove && s.shot.move.on()) label += " (orbit)";
      if (tr == k::kAim && s.shot.aim.on()) label += " (look at)";
      if (tr == k::kFocus && s.shot.focus.on()) label += " (follow)";
      ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 3));
      if (ImGui::SmallButton("key")) ShotKey(s, f, tr);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Key only %s at the playhead", k::kTrackNames[tr]);
      ImGui::SameLine();
      ImGui::TextUnformatted(label.c_str());
      ImGui::SetCursorScreenPos(ImVec2(lane_x0, y));
      ImGui::InvisibleButton("##lane", ImVec2(lane_w, kLaneH));
      const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
      const ImVec2 mouse = ImGui::GetIO().MousePos;
      dl->AddRectFilled(ImVec2(lane_x0, y), ImVec2(lane_x0 + lane_w, y + kLaneH),
                        ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), 3.0f);
      auto& keys = s.shot.keys[tr];
      const float cy = y + kLaneH * 0.5f;
      for (size_t i = 0; i + 1 < keys.size(); ++i) {
        const ImU32 col = keys[i].curve == k::Curve::kHold ? IM_COL32(200, 120, 80, 200) : IM_COL32(120, 200, 255, 160);
        dl->AddLine(ImVec2(tx(keys[i].t), cy), ImVec2(tx(keys[i + 1].t), cy), col,
                    keys[i].curve == k::Curve::kLinear ? 1.0f : 2.5f);
      }
      int under = -1;
      for (size_t i = 0; i < keys.size(); ++i) {
        if (std::abs(mouse.x - tx(keys[i].t)) <= 7 && mouse.y >= y && mouse.y <= y + kLaneH) under = int(i);
      }
      if (ImGui::IsItemActivated()) {
        if (under >= 0) {
          s.sel_track = tr, s.sel_key = under;
          drag_track_ = tr, drag_key_ = under;
          if (ImGui::IsMouseDoubleClicked(0)) {
            s.play = ShotPlay::kStopped;
            s.playhead = keys[size_t(under)].t;
            s.apply = true;
            drag_track_ = -1;
          }
        } else {
          drag_track_ = -1;
          scrub(mouse.x);
        }
      } else if (active && drag_track_ == tr && drag_key_ >= 0 && size_t(drag_key_) < keys.size()) {
        const double t = xt(mouse.x);
        if (std::abs(t - keys[size_t(drag_key_)].t) > 1e-9) {
          drag_key_ = int(k::MoveKey(s.shot, tr, size_t(drag_key_), t));
          s.sel_track = tr, s.sel_key = drag_key_;
          s.shot.length = std::max(s.shot.length, k::LastKeyTime(s.shot));
          s.dirty = true;
        }
      } else if (active && drag_track_ < 0) {
        scrub(mouse.x);
      }
      for (size_t i = 0; i < keys.size(); ++i) {
        const float x = tx(keys[i].t), r = 6.0f;
        const bool sel = s.sel_track == tr && s.sel_key == int(i);
        const ImVec2 q[4] = {ImVec2(x, cy - r), ImVec2(x + r, cy), ImVec2(x, cy + r), ImVec2(x - r, cy)};
        dl->AddQuadFilled(q[0], q[1], q[2], q[3], sel ? IM_COL32(255, 210, 90, 255) : IM_COL32(230, 230, 240, 255));
        dl->AddQuad(q[0], q[1], q[2], q[3], IM_COL32(20, 20, 30, 255), 1.5f);
      }
      if (under >= 0 && hovered) {
        ImGui::SetTooltip("%s key at %.2f s (%s)", k::kTrackNames[tr], keys[size_t(under)].t,
                          k::kCurveNames[int(keys[size_t(under)].curve)]);
      }
      ImGui::PopID();
      y += kLaneH + 3;
    }
    // The playhead across the ruler and the lanes, and the shot's end when keys run past it.
    const float px = tx(std::min(s.playhead, end));
    dl->AddLine(ImVec2(px, origin.y), ImVec2(px, y), IM_COL32(255, 80, 80, 255), 2.0f);
    if (s.shot.length < end) {
      dl->AddLine(ImVec2(tx(s.shot.length), origin.y), ImVec2(tx(s.shot.length), y), IM_COL32(255, 255, 255, 80));
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, y + 4));
  }

  void Inspector(ShotState& s, FreeCam& f) {
    if (s.sel_track < 0 || s.sel_key < 0 || size_t(s.sel_key) >= s.shot.keys[s.sel_track].size()) {
      ImGui::TextDisabled("Click a key to edit it.");
      ImGui::TextDisabled("K key all   Space play / pause   [ ] previous / next key");
      ImGui::TextDisabled(", . one frame (Shift: one second)   Backspace delete key");
      return;
    }
    const int tr = s.sel_track;
    auto& keys = s.shot.keys[tr];
    k::Key& key = keys[size_t(s.sel_key)];
    bool changed = false;
    ImGui::Text("%s key %d of %d", k::kTrackNames[tr], s.sel_key + 1, int(keys.size()));
    ImGui::SameLine();
    if (ImGui::SmallButton("go to")) {
      s.play = ShotPlay::kStopped;
      s.playhead = key.t;
      s.apply = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("update from camera")) {
      float v[3];
      k::CaptureValues(s.shot, tr, key.t, CamPose(f), TargetOrNull(s, 0), TargetOrNull(s, 1), TargetOrNull(s, 2),
                       s.aspect, v);
      std::memcpy(key.v, v, sizeof(v));
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("delete")) {
      k::DeleteKey(s.shot, tr, size_t(s.sel_key));
      s.sel_track = s.sel_key = -1;
      s.dirty = true;
      s.apply = true;
      return;
    }
    float t = float(key.t);
    ImGui::SetNextItemWidth(120);
    if (ImGui::DragFloat("time", &t, 1.0f / 60, 0.0f, 600.0f, "%.3f s")) {
      s.sel_key = int(k::MoveKey(s.shot, tr, size_t(s.sel_key), SnapFrame(t)));
      s.shot.length = std::max(s.shot.length, k::LastKeyTime(s.shot));
      s.dirty = true;
      return;  // the key may have moved in the list
    }
    ImGui::SameLine();
    int curve = int(key.curve);
    ImGui::SetNextItemWidth(100);
    if (ImGui::Combo("curve", &curve, "smooth\0linear\0hold\0")) {
      key.curve = k::Curve(curve);
      changed = true;
    }
    ImGui::SameLine();
    bool auto_ease = key.ease < 0;
    if (ImGui::Checkbox("auto ease", &auto_ease)) {
      key.ease = auto_ease ? -1.0f : k::EaseOf(keys, size_t(s.sel_key));
      changed = true;
    }
    if (!auto_ease) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120);
      changed |= ImGui::SliderFloat("ease", &key.ease, 0.0f, 1.0f, "%.2f");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("1 = slows to a stop at this key, 0 = passes through at speed");
    }
    ImGui::SetNextItemWidth(320);
    switch (tr) {
      case k::kMove:
        if (s.shot.move.on()) {
          changed |= ImGui::DragFloat3("distance / angle / height", key.v, 1.0f, 0, 0, "%.1f");
        } else {
          changed |= ImGui::DragFloat3("x y z (cm, Y down)", key.v, 1.0f, 0, 0, "%.1f");
        }
        break;
      case k::kAim:
        if (s.shot.aim.on()) {
          changed |= ImGui::DragFloat2("framing x / y", key.v, 0.002f, -1.0f, 1.0f, "%.3f");
          if (ImGui::SmallButton("centre")) key.v[0] = key.v[1] = 0, changed = true;
          ImGui::SameLine();
          if (ImGui::SmallButton("left third")) key.v[0] = -1.0f / 3, changed = true;
          ImGui::SameLine();
          if (ImGui::SmallButton("right third")) key.v[0] = 1.0f / 3, changed = true;
        } else {
          changed |= ImGui::DragFloat2("yaw / pitch", key.v, 0.1f, 0, 0, "%.2f");
        }
        break;
      case k::kRoll: changed |= ImGui::DragFloat("roll (degrees)", &key.v[0], 0.1f, -180.0f, 180.0f, "%.2f"); break;
      case k::kLens: {
        float mm = FovToMm(key.v[0]);
        if (ImGui::DragFloat("focal length", &mm, 0.2f, 5.0f, 800.0f, "%.1f mm", ImGuiSliderFlags_Logarithmic)) {
          key.v[0] = MmToFov(mm);
          changed = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("FOV %.2f", key.v[0]);
        break;
      }
      case k::kFocus:
        changed |= ImGui::DragFloat2(s.shot.focus.on() ? "offset cm / aperture" : "distance cm / aperture", key.v, 1.0f,
                                     0, 0, "%.2f");
        break;
    }
    if (changed) {
      s.shot.Touch();
      s.dirty = true;
      if (s.play == ShotPlay::kStopped) s.apply = true;
    }
  }

  void TargetRow(ShotState& s, FreeCam& f, int role, const char* what) {
    ImGui::PushID(role);
    const k::Target cur = RoleTarget(s.shot, role);
    ImGui::SetNextItemWidth(200);
    if (ImGui::BeginCombo(what, TargetLabel(cur).c_str())) {
      if (ImGui::Selectable("nothing", !cur.on())) ShotSetTarget(s, role, k::Target{});
      for (int slot = 0; slot < 6; ++slot) {
        k::Target t;
        t.kind = k::TargetKind::kSlot, t.slot = slot;
        if (ImGui::Selectable(TargetLabel(t).c_str(), cur.kind == t.kind && cur.slot == slot)) ShotSetTarget(s, role, t);
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("pin here")) ShotSetTarget(s, role, PinHere(f));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A fixed point straight ahead at the focus distance");
    if (cur.on() && !s.tgt_ok[role]) {
      ImGui::SameLine();
      ImGui::TextDisabled("(not found)");
    }
    ImGui::PopID();
  }

  void Targets(ShotState& s, FreeCam& f) {
    TargetRow(s, f, 0, "look at");
    TargetRow(s, f, 1, "orbit around");
    TargetRow(s, f, 2, "focus on");
    ImGui::SetNextItemWidth(160);
    ImGui::SliderFloat("follow lag", &s.shot.follow, 0.0f, 1.0f, "%.2f s");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How loosely the camera follows a wrestler (0 = locked on)");
    if (ImGui::Checkbox("constant speed (move)", &s.shot.constant_speed)) {
      s.shot.Touch();
      s.dirty = true;
      s.apply = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("An even dolly speed between the first and last key, easing at the ends");
  }

  char name_[64] = {};
  std::string last_name_;
  int drag_track_ = -1, drag_key_ = -1;
};

// Session 36: F8 outside a match (menus, match setup) opens this instead of the free camera, which has nothing to
// drive there: the settings that matter before the match starts. Hidden once the match runs; F8 there closes it and
// turns the free camera on as before.
class Setup final : public rex::ui::ImGuiDialog {
 public:
  explicit Setup(rex::ui::ImGuiDrawer* drawer) : rex::ui::ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO&) override {
    if (step_rate::g_in_game.load(std::memory_order_relaxed)) return;
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("CineCam setup")) {
      ImGui::End();
      return;
    }
    ImGui::TextDisabled("The free camera starts in a match (F8 there). F8 closes this.");
    AutoFreezeCheckbox();
    ImGui::End();
  }
};

// F8 (UI thread; key = the F8 key itself, not the cvar or startup).
inline void Toggle(bool key = false) {
  FreeCam& f = F();
  bool show_panel = false;
  if (key) {  // Session 36: outside a match the F8 key opens / closes the setup panel instead
    const bool in_game = step_rate::g_in_game.load(std::memory_order_relaxed);
    bool active;
    {
      std::lock_guard lock(f.mutex);
      active = f.active;
    }
    if (f.setup) {
      delete f.setup;
      f.setup = nullptr;
      if (!in_game) {
        REXLOG_INFO("[wwe2k14][cinecam] setup panel closed");
        return;
      }
    } else if (!in_game && !active) {
      if (f.drawer) f.setup = new Setup(f.drawer);
      REXLOG_INFO("[wwe2k14][cinecam] setup panel open (outside a match)");
      return;
    }
  }
  {
    std::lock_guard lock(f.mutex);
    f.active = !f.active;
    f.seeded = false;
    f.mouse_dx = f.mouse_dy = 0;
    f.wheel = 0;
    if (!f.active) {
      StopLooking(f);
      Sh().play = ShotPlay::kStopped;  // Session 35: a shot stops, and a frozen scene runs again
      ShotFreeze(Sh(), false);
    }
    show_panel = f.active && REXCVAR_GET(wwe2k14_cinecam_panel);
    REXLOG_INFO("[wwe2k14][cinecam] free camera {} (guest frames driven so far: {})", f.active ? "on" : "off",
                f.frames);
  }
  if (show_panel && !f.panel && f.drawer) f.panel = new Panel(f.drawer);
  if (!show_panel && f.panel) {
    delete f.panel;  // removes it from the drawer
    f.panel = nullptr;
  }
  const bool show_timeline = show_panel && REXCVAR_GET(wwe2k14_cinecam_timeline);
  if (show_timeline && !f.timeline && f.drawer) {
    auto* t = new Timeline(f.drawer);
    std::lock_guard lock(f.mutex);  // the hotkeys read f.timeline under the mutex
    f.timeline = t;
  }
  if (!show_timeline && f.timeline) {
    rex::ui::ImGuiDialog* t;
    {
      std::lock_guard lock(f.mutex);
      t = f.timeline;
      f.timeline = nullptr;
    }
    delete t;
  }
}

// From ReXApp::OnCreateDialogs (UI thread). Returns at once unless --wwe2k14_cinecam=true.
inline void InstallFreeze();  // Session 16 freeze cam, defined at the end of this file
inline void Install(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer) {
  if (!REXCVAR_GET(wwe2k14_cinecam) || !window) return;
  FreeCam& f = F();
  {
    std::lock_guard lock(f.mutex);
    if (f.installed) return;
    f.installed = true;
    f.window = window;
    f.drawer = drawer;
  }
  window->AddInputListener(&L(), 100);
  window->AddListener(&L());
  rex::ui::RegisterBind("bind_wwe2k14_cinecam", "F8", "Toggle the CineCam free camera", [] { Toggle(true); });
  // In-run toggle for input scripts / the settings panel: `cvar wwe2k14_cinecam_on true|false` (posted to the UI
  // thread; the callback runs under the cvar registry lock).
  rex::cvar::RegisterChangeCallback("wwe2k14_cinecam_on", [window](std::string_view, std::string_view value) {
    const bool on = value == "true" || value == "1";
    window->app_context().CallInUIThreadDeferred([on] {
      bool active;
      {
        std::lock_guard lock(F().mutex);
        active = F().active;
      }
      if (active != on) Toggle();
    });
  });
  // Shot buttons from a script (Session 14): `cvar wwe2k14_cinecam_shot record|stop|NN` = Record / Stop / play shot_NN,
  // the same actions as the panel's buttons, on the UI thread.
  rex::cvar::RegisterChangeCallback("wwe2k14_cinecam_shot", [window](std::string_view, std::string_view value) {
    const std::string v(value);
    window->app_context().CallInUIThreadDeferred([v] {
      std::lock_guard lock(F().mutex);
      PathState& s = P();
      if (v == "record") {
        UiStartRecord(s);
      } else if (v == "stop") {
        UiStopRecord(s);
        s.playing = false;
      } else if (!v.empty()) {
        UiPlayShot(s, F(), v.rfind("shot_", 0) == 0 ? v : "shot_" + v);
      }
    });
  });
  // Session 35: keyframed shot commands from a script (the same actions as the timeline panel).
  rex::cvar::RegisterChangeCallback("wwe2k14_cinecam_timeline_cmd", [window](std::string_view, std::string_view value) {
    const std::string v(value);
    if (v.empty()) return;
    window->app_context().CallInUIThreadDeferred([v] {
      {
        std::lock_guard lock(F().mutex);
        ShotCommand(Sh(), F(), v);
      }
      rex::cvar::SetFlagByName("wwe2k14_cinecam_timeline_cmd", "");  // so the same command can be sent again
    });
  });
  REXLOG_INFO("[wwe2k14][cinecam] installed: F8 (bind_wwe2k14_cinecam) toggles the free camera");
  InstallFreeze();  // Session 16: --wwe2k14_cinecam_freeze (defined at the end of this file)
  if (REXCVAR_GET(wwe2k14_cinecam_on)) Toggle();
}

// From ReXApp::OnShutdown (UI thread).
inline void Shutdown() {
  FreeCam& f = F();
  if (!f.installed) return;
  rex::ui::UnregisterBind("bind_wwe2k14_cinecam");
  rex::cvar::UnregisterChangeCallbacks("wwe2k14_cinecam_on");
  rex::cvar::UnregisterChangeCallbacks("wwe2k14_cinecam_shot");
  rex::cvar::UnregisterChangeCallbacks("wwe2k14_cinecam_timeline_cmd");
  if (f.panel) {
    delete f.panel;
    f.panel = nullptr;
  }
  if (f.timeline) {
    delete f.timeline;
    f.timeline = nullptr;
  }
  if (f.setup) {
    delete f.setup;
    f.setup = nullptr;
  }
  if (f.window) {
    f.window->RemoveInputListener(&L());
    f.window->RemoveListener(&L());
  }
  std::lock_guard lock(f.mutex);
  f.active = false;
  f.installed = false;
  f.window = nullptr;
  f.drawer = nullptr;
}

}  // namespace wwe2k14::cinecam

REX_EXTERN(__imp__sub_8257D8E8);
// Game camera per-frame update (see the header): test stage 0.
REX_HOOK_RAW(sub_8257D8E8) {
  using namespace wwe2k14::cinecam;
  const uint32_t cam = ctx.r3.u32;
  __imp__sub_8257D8E8(ctx, base);
  if (!cam) return;
  if (REXCVAR_GET(wwe2k14_cinecam_test_camwrite)) {
    float m[16];
    float fov = 0;
    bool ours = false;
    {
      FreeCam& f = F();
      std::lock_guard lock(f.mutex);
      if (f.active && f.seeded && f.have_last) {
        std::memcpy(m, f.last_m, sizeof(m));
        fov = f.last_fov;
        ours = true;
      }
    }
    if (ours) {
      const uint32_t scratch = (ctx.r1.u32 - 1024) & ~15u;
      for (int i = 0; i < 16; ++i) StoreGuestF32(base, scratch + 4 * i, m[i]);
      const PPCContext saved = ctx;
      ctx.r3.u64 = cam + 16;
      ctx.r4.u64 = scratch;
      sub_82280108(ctx, base);
      ctx = saved;
      StoreGuestF32(base, cam + 144, fov);
      return;
    }
  }
  if (REXCVAR_GET(wwe2k14_cinecam_test_stage) != 0) return;
  ApplyTest(ctx, base, cam + 16, "game camera");
}

REX_EXTERN(__imp__sub_827F14C0);
// The depth of field converter (see the header): with the free camera on and depth of field "on", its result is
// replaced with the panel's focus, offsets and strength.
REX_HOOK_RAW(sub_827F14C0) {
  using namespace wwe2k14::cinecam;
  const uint32_t view = ctx.r4.u32;
  __imp__sub_827F14C0(ctx, base);
  wwe2k14::matte::g_last_dof_view.store(view, std::memory_order_relaxed);  // Session 16: near / far for mattes
  FreeCam& f = F();
  std::lock_guard lock(f.mutex);
  if (!f.active || f.dof_mode != 2 || !view) return;
  const uint32_t st = LoadGuestU32(base, kPostEffectSettingsPtr);
  if (!st) return;
  const float zn = LoadGuestF32(base, view + 500), zf = LoadGuestF32(base, view + 504);
  if (!(zn > 0) || !(zf > zn)) return;
  auto z = [&](float d) {
    d = std::clamp(d, zn, zf);
    return zf / (zf - zn) - zn * zf / ((zf - zn) * d);
  };
  // The game's focus: invert its +24 = z(F - S84) (exact unless clamped at the near plane).
  const float z24 = LoadGuestF32(base, st + 24);
  const float d24 = zn * zf / (zf - z24 * (zf - zn));
  f.dof_auto_focus = d24 + LoadGuestF32(base, st + 84);
  const float focus = f.dof_manual ? f.dof_focus : f.dof_auto_focus;
  const float a = f.dof_dist[0], b = f.dof_dist[1], c = f.dof_dist[2], d = f.dof_dist[3];
  for (int i = 0; i < 4; ++i) StoreGuestF32(base, st + 84 + 4 * i, f.dof_dist[i]);
  StoreGuestF32(base, st + 8, z(focus - a));
  StoreGuestF32(base, st + 12, z(focus - c));
  StoreGuestF32(base, st + 16, z(focus + d));
  StoreGuestF32(base, st + 24, z(focus - a));
  StoreGuestF32(base, st + 28, z(focus + b));
  StoreGuestF32(base, st + 172, f.dof_strength);
  const uint32_t one = std::byteswap(uint32_t(1));
  std::memcpy(base + st + 168, &one, 4);
}

REX_EXTERN(__imp__sub_82C2BBE8);
// The post-effect blur chain (depth of field; see the header). Diagnosis only (--wwe2k14_cinecam_dof_log): logs the
// settings it reads, at most once a second.
REXCVAR_DEFINE_BOOL(wwe2k14_cinecam_dof_log, false, "wwe2k14",
                    "CineCam diagnosis: log the post-effect settings the depth-of-field chain (sub_82C2BBE8) reads, "
                    "at most once a second");
REX_HOOK_RAW(sub_82C2BBE8) {
  using namespace wwe2k14::cinecam;
  if (REXCVAR_GET(wwe2k14_cinecam_dof_log)) {
    static std::chrono::steady_clock::time_point last;
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(1)) {
      last = now;
      const uint32_t st = LoadGuestU32(base, kPostEffectSettingsPtr);
      if (st) {
        REXLOG_INFO("[wwe2k14][cinecam] dof chain reads: +4 {} +176 {} +180 {} +560 {} +168 {} | +84 {:.0f} +88 {:.0f} "
                    "+92 {:.0f} +96 {:.0f} +172 {:.5f} +784 {} +840 {} | effect +16 {}",
                    LoadGuestU32(base, st + 4), LoadGuestU32(base, st + 176), LoadGuestU32(base, st + 180),
                    LoadGuestU32(base, st + 560), LoadGuestU32(base, st + 168), LoadGuestF32(base, st + 84),
                    LoadGuestF32(base, st + 88), LoadGuestF32(base, st + 92), LoadGuestF32(base, st + 96),
                    LoadGuestF32(base, st + 172), LoadGuestU32(base, st + 784), LoadGuestU32(base, st + 840),
                    LoadGuestU32(base, ctx.r3.u32 + 16));
      }
    }
  }
  // The free camera's depth of field (see the header): written here, right before the chain reads them, because the
  // game rewrites the distances every frame while the hard cam (key 0) is shown (run 966). With full arena on, the
  // hard cam's key reads 0 for this call (the blur follows the key too: none with key 10, runs 962-965).
  FreeCam& f = F();
  uint32_t key_cam = 0;
  {
    std::lock_guard lock(f.mutex);
    const uint32_t st = LoadGuestU32(base, kPostEffectSettingsPtr);
    if (f.active && st && f.dof_mode != 0) {
      const uint32_t on = std::byteswap(uint32_t(f.dof_mode == 2 ? 1 : 0));
      std::memcpy(base + st + 168, &on, 4);
      if (f.dof_mode == 2) StoreGuestF32(base, st + 172, f.dof_strength);
    }
    if (f.active && f.key_camera && LoadGuestU32(base, f.key_camera + 300) == 10) key_cam = f.key_camera;
  }
  const uint32_t zero = 0, ten = std::byteswap(uint32_t(10));
  if (key_cam) std::memcpy(base + key_cam + 300, &zero, 4);
  __imp__sub_82C2BBE8(ctx, base);
  if (key_cam) std::memcpy(base + key_cam + 300, &ten, 4);
}

REX_EXTERN(__imp__sub_823C79A8);
namespace wwe2k14::cinecam {
// Freeze cam (Session 16): the guest frame of the main manager's last real hand-off, and whether the current call is
// ours (FreezeOnSwap), which must not count as one.
inline std::atomic<uint64_t> g_last_handoff{0};
inline bool g_freeze_calling = false;
// The game's own composed view at its last real hand-off (the slot before the free camera overwrites it, and the
// composer's copies +1488 / +1552), so the freeze cam's hand-back can restore the game's frozen view.
inline uint8_t g_game_slot[272] = {}, g_game_1488[64] = {}, g_game_1552[4] = {};
inline bool g_game_slot_valid = false;
}  // namespace wwe2k14::cinecam
// The scene manager's hand-off of the composed view to the renderer (see the header): the free camera, else test
// stage 1.
REX_HOOK_RAW(sub_823C79A8) {
  using namespace wwe2k14::cinecam;
  const uint32_t mgr = ctx.r3.u32;
  if (!g_freeze_calling && mgr && mgr == LoadGuestU32(base, kSceneManagerPtr)) {
    g_last_handoff.store(wwe2k14::g_guest_frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
    if (REXCVAR_GET(wwe2k14_cinecam_freeze)) {  // Session 16: keep the game's view for the freeze cam's hand-back
      const uint32_t views = LoadGuestU32(base, mgr + 24), index = LoadGuestU32(base, mgr + 32);
      if (views && int32_t(index) >= 0 && int32_t(index) < int32_t(LoadGuestU32(base, mgr + 28))) {
        std::memcpy(g_game_slot, base + views + index * 272, 272);
        std::memcpy(g_game_1488, base + mgr + 1488, 64);
        std::memcpy(g_game_1552, base + mgr + 1552, 4);
        g_game_slot_valid = true;
      }
    }
  }
  bool free_cam;
  {
    std::lock_guard lock(F().mutex);
    if (PathActive() && mgr && F().installed && mgr == LoadGuestU32(base, kSceneManagerPtr)) {
      PathTick(base, F());  // S2: anchor, step log, replay turns the free camera on
    }
    free_cam = F().active;
  }
  const bool test = REXCVAR_GET(wwe2k14_cinecam_test_stage) == 1 || REXCVAR_GET(wwe2k14_cinecam_dump_view) != 0;
  if (mgr && F().installed && mgr == LoadGuestU32(base, kSceneManagerPtr)) UpdateFullArena(base, mgr);
  if ((free_cam || test) && mgr) {
    if (mgr != LoadGuestU32(base, kSceneManagerPtr)) {
      if (g_other_managers_logged.fetch_add(1) < 3) {
        REXLOG_INFO("[wwe2k14][cinecam] hand-off for another manager {:08X} (main {:08X}); not overridden", mgr,
                    LoadGuestU32(base, kSceneManagerPtr));
      }
    } else {
      const uint32_t views = LoadGuestU32(base, mgr + 24);
      const uint32_t index = LoadGuestU32(base, mgr + 32);
      if (views && int32_t(index) >= 0 && int32_t(index) < int32_t(LoadGuestU32(base, mgr + 28))) {
        const uint32_t vb = views + index * 272;
        if (const int32_t setting = REXCVAR_GET(wwe2k14_cinecam_dump_view); setting > 0 || setting < -1) {
          // N > 0: dump this hand-off as N, then the next one as N + 10 (the render views are double-buffered, so a
          // pair covers both frame parities); the setting holds -(N + 10) in between.
          const int32_t label = setting > 0 ? setting : -setting;
          rex::cvar::SetFlagByName("wwe2k14_cinecam_dump_view", setting > 0 ? std::to_string(-(setting + 10)) : "0");
          const uint32_t shown = LoadGuestU32(base, mgr + 1392);
          REXLOG_INFO("[wwe2k14][cinecam] view dump {}: slot {:08X} (index {}), manager {:08X}, shown camera {:08X} "
                      "vtable {:08X}",
                      label, vb, index, mgr, shown, shown ? LoadGuestU32(base, shown) : 0);
          DumpBytes(base, vb, 272, label, "slot");
          DumpBytes(base, mgr, 4096, label, "mgr");
          DumpBytes(base, shown, 4096, label, "cam");
          DumpBytes(base, 0x83500000, 0x00560000, label, "data");  // globals (image end 0x83A60000, run log)
          DumpBytes(base, LoadGuestU32(base, kPostEffectSettingsPtr), 1024, label, "post");  // post-effect settings
          // The director (key 10 in the table at [mgr+1380]: 8-byte entries, key at +0, camera at +4).
          const uint32_t table = LoadGuestU32(base, mgr + 1380), count = LoadGuestU32(base, mgr + 1384);
          for (uint32_t i = 0; table && i < count && i < 256; ++i) {
            if (LoadGuestU32(base, table + i * 8) == 10) DumpBytes(base, LoadGuestU32(base, table + i * 8 + 4), 8192, label, "dir");
          }
          for (uint32_t off = 0; off < 272; off += 16) {
            REXLOG_INFO("[wwe2k14][cinecam] view dump {} +{:3}: {:08X} {:08X} {:08X} {:08X} | {:.4g} {:.4g} {:.4g} {:.4g}",
                        label, off, LoadGuestU32(base, vb + off), LoadGuestU32(base, vb + off + 4),
                        LoadGuestU32(base, vb + off + 8), LoadGuestU32(base, vb + off + 12),
                        LoadGuestF32(base, vb + off), LoadGuestF32(base, vb + off + 4),
                        LoadGuestF32(base, vb + off + 8), LoadGuestF32(base, vb + off + 12));
          }
        }
        if (free_cam) {
          ApplyFreeCam(ctx, base, mgr, vb);
          ApplyTestPoke(base, LoadGuestU32(base, mgr + 1392));
        } else if (ApplyTest(ctx, base, vb, "hand-off")) {
          // Keep the composer's copies (0x823C88E4..0x823C890C) in step: matrix -> +1488, FOV -> +1552.
          std::memcpy(base + mgr + 1488, base + vb, 64);
          std::memcpy(base + mgr + 1552, base + vb + 128, 4);
        }
      }
    }
  }
  // Session 18 (DLAA): the composed view (after the free camera) for the motion vectors. Session 24: sent after the
  // hand-off, which fills the render view (mgr+48 + 656 * [mgr+1360]) with this camera's near / far (+500 / +504).
  wwe2k14::dlaa::HandoffView dlaa_view;
  const bool dlaa_mgr = wwe2k14::dlaa::g_on && mgr && mgr == LoadGuestU32(base, kSceneManagerPtr);
  if (dlaa_mgr) {
    const uint32_t views = LoadGuestU32(base, mgr + 24), index = LoadGuestU32(base, mgr + 32);
    if (views && int32_t(index) >= 0 && int32_t(index) < int32_t(LoadGuestU32(base, mgr + 28)))
      dlaa_view = wwe2k14::dlaa::ReadHandoffView(base, views + index * 272);  // before the hand-off (Session 18)
  }
  __imp__sub_823C79A8(ctx, base);
  if (dlaa_view.valid) {
    const uint32_t rv = mgr + 48 + 656 * (LoadGuestU32(base, mgr + 1360) & 1);
    wwe2k14::dlaa::OnHandoff(dlaa_view, LoadGuestF32(base, rv + 500), LoadGuestF32(base, rv + 504));
  }
}

// ---- Freeze cam (Session 16, item 6) ----
// Session 14 S5: in the post-match freeze (the EXIT / REMATCH / HIGHLIGHTS menu) the renderer still draws the whole
// scene every frame, but the scene manager no longer runs its composer or the hand-off sub_823C79A8, which is where the
// free camera writes its view and where the view goes to the renderer (it flips the double-buffered render view at
// mgr+48 + 656 * [mgr+1360], stores the inverted matrix at view+240, the projection through sub_82BB3090, calls the
// view's vcall +44 and submits it with sub_82BA86B8). With --wwe2k14_cinecam_freeze, once the match is decided (G+472
// 0x8376ACCC != 0) and the main manager has not handed off for --wwe2k14_cinecam_freeze_after frames, VdSwap (a guest
// thread, every frame) calls the hand-off itself for the main manager while the free camera is on, so the frozen scene
// is drawn from the free camera. Logged: "[cinecam][freeze] ..." on entering / leaving the freeze and every 600 calls.
REX_EXTERN(sub_823C79A8);
namespace wwe2k14::cinecam {
inline bool g_frozen = false;
inline uint64_t g_freeze_calls = 0;
inline bool g_freeze_drove = false;  // the freeze cam has drawn a view since the last hand-back
inline void (*g_freeze_prev)(uint64_t) = nullptr;  // a VdSwap hook installed before (chained)

inline void FreezeOnSwap(uint64_t frame) {
  if (g_freeze_prev) g_freeze_prev(frame);
  FreeCam& f = F();
  if (!f.installed) return;
  bool active;
  {
    std::lock_guard lock(f.mutex);
    active = f.active;
  }
  uint8_t* base = REX_KERNEL_MEMORY()->virtual_membase();
  const uint64_t last = g_last_handoff.load(std::memory_order_relaxed);
  const uint32_t mgr = LoadGuestU32(base, kSceneManagerPtr);
  const uint64_t after = uint64_t(std::max(1, int(REXCVAR_GET(wwe2k14_cinecam_freeze_after))));
  const bool frozen = mgr && last && frame > last + after && LoadGuestU32(base, 0x8376ACCCu) != 0;
  if (frozen != g_frozen) {
    g_frozen = frozen;
    REXLOG_INFO("[wwe2k14][cinecam][freeze] {} at frame {} (last hand-off {}, free camera {}, calls so far {})",
                frozen ? "scene frozen: the freeze cam drives the hand-off" : "freeze over", frame, last,
                active ? "on" : "off", g_freeze_calls);
  }
  if (!frozen) {
    g_freeze_drove = false;
    return;
  }
  const bool hand_back = !active && g_freeze_drove;
  if (!active && !hand_back) return;
  auto* ts = rex::runtime::ThreadState::Get();
  if (!ts || !ts->context()) return;
  if (hand_back) {
    // F8 off in the freeze: put the game's own frozen view back into the slot and hand it to the renderer once (the
    // free camera is off, so the hook passes it through unchanged).
    const uint32_t views = LoadGuestU32(base, mgr + 24), index = LoadGuestU32(base, mgr + 32);
    if (!g_game_slot_valid || !views || int32_t(index) < 0 || int32_t(index) >= int32_t(LoadGuestU32(base, mgr + 28))) {
      g_freeze_drove = false;
      REXLOG_WARN("[wwe2k14][cinecam][freeze] hand-back at frame {}: no saved game view; the last free-camera view stays",
                  frame);
      return;
    }
    std::memcpy(base + views + index * 272, g_game_slot, 272);
    std::memcpy(base + mgr + 1488, g_game_1488, 64);
    std::memcpy(base + mgr + 1552, g_game_1552, 4);
  }
  PPCContext& ctx = *ts->context();
  const PPCContext saved = ctx;
  ctx.r3.u64 = mgr;
  g_freeze_calling = true;
  sub_823C79A8(ctx, base);
  g_freeze_calling = false;
  ctx = saved;
  if (hand_back) {
    g_freeze_drove = false;
    REXLOG_INFO("[wwe2k14][cinecam][freeze] hand-back at frame {}: the game's frozen view restored", frame);
    return;
  }
  g_freeze_drove = true;
  if (++g_freeze_calls % 600 == 1)
    REXLOG_INFO("[wwe2k14][cinecam][freeze] hand-off driven from VdSwap: call {} at frame {}", g_freeze_calls, frame);
}

inline void InstallFreeze() {
  if (!REXCVAR_GET(wwe2k14_cinecam) || !REXCVAR_GET(wwe2k14_cinecam_freeze)) return;
  g_freeze_prev = wwe2k14::g_vdswap_hook;
  wwe2k14::g_vdswap_hook = &FreezeOnSwap;
  REXLOG_INFO("[wwe2k14][cinecam][freeze] freeze cam armed (after {} frames without a hand-off, match decided)",
              REXCVAR_GET(wwe2k14_cinecam_freeze_after));
}
}  // namespace wwe2k14::cinecam
