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
#include <windows.h>  // VirtualQuery (path chains, Session 14)
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
  if (p.is_relative()) p = DumpDir() / "paths" / p;
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
  if (PathPlaying()) {
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
  const std::filesystem::path dir = DumpDir() / "paths";
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
  const std::filesystem::path dir = DumpDir() / "paths";
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
  const std::filesystem::path p = DumpDir() / "paths" / (shot + ".txt");
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

// Window input (UI thread). Sits above the ImGui drawer (z 64) and never marks events handled (the game ignores the
// keyboard with ReXGlue's default mnk_mode=false; inspector.h).
class Listener final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override { SetKey(e, true); }
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

// F8 (UI thread).
inline void Toggle() {
  FreeCam& f = F();
  bool show_panel = false;
  {
    std::lock_guard lock(f.mutex);
    f.active = !f.active;
    f.seeded = false;
    f.mouse_dx = f.mouse_dy = 0;
    f.wheel = 0;
    if (!f.active) StopLooking(f);
    show_panel = f.active && REXCVAR_GET(wwe2k14_cinecam_panel);
    REXLOG_INFO("[wwe2k14][cinecam] free camera {} (guest frames driven so far: {})", f.active ? "on" : "off",
                f.frames);
  }
  if (show_panel && !f.panel && f.drawer) f.panel = new Panel(f.drawer);
  if (!show_panel && f.panel) {
    delete f.panel;  // removes it from the drawer
    f.panel = nullptr;
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
  rex::ui::RegisterBind("bind_wwe2k14_cinecam", "F8", "Toggle the CineCam free camera", [] { Toggle(); });
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
  if (f.panel) {
    delete f.panel;
    f.panel = nullptr;
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
