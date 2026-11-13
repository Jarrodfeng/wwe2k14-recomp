// wwe2k14 - jump into any 30 Years of WrestleMania match (Session 34; docs/systems/wm30.md). Default off.
//
//   --wwe2k14_wm30=N      the match with index N (0-45: `py tools\wm30\list.py` prints it first) instead of the first
//                         match of Hulkamania Runs Wild, the first time a 30 Years match is picked in this run
//   --wwe2k14_wm30_auto   (default true with --wwe2k14_wm30) drive the menus there by itself once the main menu has
//                         been idle for a second: 30 YEARS OF WRESTLEMANIA, the first era, its first match (the two
//                         that are never locked). False = pick that match yourself.
//
// How (runs 029-033, probe plugin; the evidence is in docs/systems/wm30.md): picking a match runs the mode's start
// state machine sub_827AA2E8, which loads the era's tables with sub_82A9DFB8(obj, era) (called from 0x827AA35C, return
// address 0x827AA360) and then starts the era script at a moment code. Both come from the selection object
// [[0x839295F0] + 4] + 2344: +4 era, +8 moment code. At that call this hook writes era and code of match N (read from
// the game's own match index, kind 4 of the menu's table object [[0x839295F0] + 16]) once, and passes the era on.
// Everything after that is the game's own: the era's videos, the loading card, the objectives, the match. The game
// steps the code by itself afterwards (10100 -> 10101), so the write happens once.
//
// Not known: whether finishing a jumped match writes 30 Years progress to the save out of order (docs/systems/wm30.md).
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#include <atomic>
#include <sstream>
#include <string>

#include "live_control.h"  // S101 goto: g_wm30_live (the !wm30 directive)
#include "wm30_fire_args.h"  // S101 lane 108: `!wm30 fire BANK`
#include "wm30_step_args.h"  // S101 lane 121: `!wm30 step K`
#include "gfx_menu.h"      // Put32
#include "input_script.h"  // g_input_script (the menu drive)
#include "scenario.h"      // U32, SequenceId, the main-menu constants
#include "wm30_args.h"     // S101 goto: ParseArmArgs, PackArm (the !wm30 argument rule)

REXCVAR_DEFINE_INT32(wwe2k14_wm30, -1, "wwe2k14",
                     "Session 34: jump into 30 Years of WrestleMania match N (0-45, py tools\\wm30\\list.py) the first "
                     "time a 30 Years match is picked; -1 = off. See src/wm30_jump.h");
REXCVAR_DEFINE_BOOL(wwe2k14_wm30_auto, true, "wwe2k14",
                    "Session 34: with --wwe2k14_wm30, drive the menus from the idle main menu to the first 30 Years "
                    "match by itself (false = pick it yourself)");

REX_EXTERN(__imp__sub_82A9DFB8);

namespace wwe2k14::wm30 {

constexpr uint32_t kModeState = 0x839295F0;   // S: [S+4] progress object, [S+16] the menu's table object
constexpr uint32_t kSelection = 2344;         // [S+4] + 2344: +4 era, +8 moment code
constexpr uint32_t kStartPathRet = 0x827AA360;  // sub_827AA2E8's call of sub_82A9DFB8 (the 30 Years start)
constexpr uint32_t kIndexKind = 4;            // the match index: 24 fields per match (0 index, 1 era, 2 code, 23 record)

// S101 goto: g_boot_pending = the --wwe2k14_wm30 jump is still unused (Install sets it, the hook clears it; `!wm30 off` never touches it).
inline std::atomic<bool> g_boot_pending{false};
inline std::atomic<bool> g_fire_started{false};  // S101 lane 108: the era-load hook saw a 30 Years start (src/wm30_fire.h reads it)
inline uint64_t g_idle = 0, g_last_frame = 0;
inline bool g_drive_started = false;

inline uint16_t U16(uint32_t a) {
  const uint8_t* p = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(a);
  return uint16_t(p[0] << 8 | p[1]);
}

// Era, moment code and record of match `index` from the game's own index table (big-endian in memory: the loader
// swaps every table after unpacking it). False if the table is not there or has no such match.
inline bool Lookup(int index, int& era, int& code, int& record) {
  const uint32_t s = scenario::U32(kModeState);
  const uint32_t u = s ? scenario::U32(s + 16) : 0;
  const uint32_t t = u ? scenario::U32(u + 52 + 4 * kIndexKind) : 0;
  if (!t) return false;
  const int count = int16_t(U16(t)), fields = int16_t(U16(t + 2));
  if (count <= 0 || count > 200 || fields < 24 || fields > 64) return false;
  for (int i = 0; i < count; ++i) {
    const uint32_t row = t + 4 + 2 * uint32_t(fields * i);
    if (int16_t(U16(row)) != index) continue;
    era = int16_t(U16(row + 2));
    code = int16_t(U16(row + 4));
    record = int16_t(U16(row + 46));
    return era >= 0 && era <= 4 && code > 0;
  }
  return false;
}

// S101 goto: the game side of "this code belongs to this era": the codes the index table itself names (each match's X00, X01 =
// X00 + 1, X02 = X00 + 2) of the matches in `era`. A raw SCEX code that is in the era's script but in no match's trio is refused
// here too (an unknown code may soft-lock the loading screen [U]); tools/goto/resolve.py applies the same rule.
inline bool EraHasCode(int era, int code) {
  const uint32_t s = scenario::U32(kModeState);
  const uint32_t u = s ? scenario::U32(s + 16) : 0;
  const uint32_t t = u ? scenario::U32(u + 52 + 4 * kIndexKind) : 0;
  if (!t) return false;
  const int count = int16_t(U16(t)), fields = int16_t(U16(t + 2));
  if (count <= 0 || count > 200 || fields < 24 || fields > 64) return false;
  for (int i = 0; i < count; ++i) {
    const uint32_t row = t + 4 + 2 * uint32_t(fields * i);
    if (int16_t(U16(row + 2)) != era) continue;
    const int x00 = int16_t(U16(row + 4));
    if (code >= x00 && code <= x00 + 2) return true;
  }
  return false;
}

// S101 goto: the live arming. `!wm30 N [CODE]` on the live channel (src/live_control.h) arms the same jump with match N and,
// optionally, an SCEX moment code that replaces the match's own X00 (tools/goto/resolve.py checks it against the era's list
// first). Used once, at the next 30 Years start; `!wm30 off` disarms the live arming only (the boot jump stays as it was).
// Does not start the menu drive: the caller presses. One atomic word (PackArm) so the hook never sees a half-written arm.
// Nothing here reads guest memory: the pipe thread only stores; the hook (guest thread) looks the match up and logs what it finds.
inline std::atomic<int64_t> g_live_arm{kNotArmed};

// S101 lane 108: `!wm30 fire ...` is served by src/wm30_fire.h (it fills this pointer at start-up; the owner tick does the work).
inline std::string (*g_fire_live)(const std::string& args) = nullptr;
// S101 lane 121: `!wm30 step K ...` is served by src/wm30_step.h (same shape).
inline std::string (*g_step_live)(const std::string& args) = nullptr;

inline std::string LiveCommand(const std::string& args) {
  if (wm30_fire::IsFireCommand(args)) return g_fire_live ? g_fire_live(args) : "error: the fire verb is not built in";
  if (wm30_step::IsStepCommand(args)) return g_step_live ? g_step_live(args) : "error: the step verb is not built in";  // S101 lane 121
  ArmArgs a;
  const std::string error = ParseArmArgs(args, a);
  if (!error.empty()) return error;
  if (a.off) {
    g_live_arm.store(kNotArmed);
    return "disarmed";
  }
  g_live_arm.store(PackArm(a.n, a.code));
  REXLOG_INFO("[wwe2k14][wm30] live: armed match {} moment code {}", a.n, a.code);
  return a.code ? fmt::format("armed match {}, moment code {} (the jump line in the log says what the hook found)", a.n, a.code)
                : fmt::format("armed match {} (the jump line in the log says what the hook found)", a.n);
}

// Registers LiveCommand with the live channel at start-up (a plain pointer store; nothing runs until a client sends !wm30).
inline const bool g_live_registered = (wwe2k14::live::g_wm30_live = &LiveCommand, true);

// S101 lane 111 (L-3): `E F wm30 N [CODE]|off` of a recording being played re-arms the jump in the same VdSwap (a plain atomic store).
inline void OnReplayEvent(uint64_t frame, const std::string& text) {
  std::string verb, rest;
  if (!replay::verbs::Split(text, verb, rest) || verb != "wm30" || rest.rfind("fire", 0) == 0) return;  // `fire` is never recorded (owner-tick verb)
  const std::string answer = LiveCommand(rest);
  REXLOG_INFO("[wwe2k14][wm30] replay event at frame {}: wm30 {} -> {}", frame, rest, answer);
}
inline const bool g_replay_registered = (replay::g_event_handlers.push_back(&OnReplayEvent), true);

inline void OnFrame(uint64_t frame);

inline void Install() {
  const int n = REXCVAR_GET(wwe2k14_wm30);
  if (n < 0) return;
  g_boot_pending = true;
  g_wm30_frame_hook = OnFrame;
  REXLOG_INFO("[wwe2k14][wm30] jump armed: match {} the first time a 30 Years match is picked{}", n,
              REXCVAR_GET(wwe2k14_wm30_auto) ? "; the menus are driven there once the main menu is idle" : "");
}

// The game's frame (VdSwap): with _auto, start the menu drive once the root main menu has been idle for 60 frames
// (the scenario launcher's idle test: sequence 75, SmartMenu on menu 1, no exit action). The drive is an input
// script (src/input_script.h), so it waits for any script that runs (a harness route) and gives the pad back after.
inline void OnFrame(uint64_t frame) {
  if (!g_boot_pending || g_drive_started || !REXCVAR_GET(wwe2k14_wm30_auto) || frame == g_last_frame) return;
  g_last_frame = frame;
  using namespace scenario;
  const uint32_t sm = U32(kSmartMenuPtr);
  const bool idle = SequenceId() == kSeqMainMenu && sm && U32(sm + 364) == kMenuMain && U32(kMenuSelection) == 0;
  g_idle = idle ? g_idle + 1 : 0;
  if (g_idle < 60) return;
  // Main menu item 1 = 30 YEARS OF WRESTLEMANIA (docs/systems/menus.md), then A (the first era card, then its first
  // match) until the match is picked: picking one opens the era's EVTDSC<era>.PAC (run 2553: a fixed wait dropped the
  // second A when the list was slow to come up, fast-forwarded).
  static const char* kDrive =
      "press_until_state DOWN cursor >=1 24 30 5 if_needed\n"
      "press_until_state UP cursor 1 24 30 5 if_needed\n"
      "press_until_state A menu !=1 24 30 5\n"
      "wait_frames 180\n"
      "press_until_file A EVTDSC 6 300 4\n";
  std::string error;
  if (g_input_script.StartLive(kDrive, error)) {
    g_drive_started = true;
    REXLOG_INFO("[wwe2k14][wm30] driving the menus to the first 30 Years match (frame {})", frame);
  } else if (g_idle % 600 == 60) {
    REXLOG_INFO("[wwe2k14][wm30] menu drive waits: {}", error);
  }
}

}  // namespace wwe2k14::wm30

// The era load of the 30 Years start path: the jump (see the top of this file).
REX_HOOK_RAW(sub_82A9DFB8) {
  using namespace wwe2k14::wm30;
  if (uint32_t(ctx.lr) == kStartPathRet) {
    g_fire_started.store(true);  // S101 lane 108: a 30 Years start (armed or not): `!wm30 fire` may serve once the match is up
    // S101 goto: the live arming (one atomic word, taken once) wins over the boot setting; either one is used at the first start.
    const int64_t live = g_live_arm.exchange(kNotArmed);
    const bool boot = g_boot_pending.exchange(false);
    if (live != kNotArmed || boot) {
      const int n = live != kNotArmed ? ArmMatch(live) : REXCVAR_GET(wwe2k14_wm30);
      const int live_code = live != kNotArmed ? ArmCode(live) : 0;
      int era = 0, code = 0, record = 0;
      const uint32_t s = wwe2k14::scenario::U32(kModeState);
      const uint32_t b = s ? wwe2k14::scenario::U32(s + 4) : 0;
      if (b && Lookup(n, era, code, record)) {
        bool refused = false;
        if (live_code > 0) {  // S101 goto: moment K of match n (X01 / X02 / a raw SCEX code), only a code of this era's matches
          if (live_code > kMaxCode || !EraHasCode(era, live_code)) {
            REXLOG_WARN("[wwe2k14][wm30] moment code {} is not a code of any era {} match: refused, the picked match plays", live_code, era);
            refused = true;
          } else {
            code = live_code;
          }
        }
        if (!refused) {
          const uint32_t sel = b + kSelection;
          REXLOG_INFO("[wwe2k14][wm30] jump to match {} (era {}, moment code {}, record {}) instead of era {} code {}", n,
                      era, code, record, wwe2k14::scenario::U32(sel + 4), wwe2k14::scenario::U32(sel + 8));
          wwe2k14::gfx_menu::Put32(sel + 4, uint32_t(era));
          wwe2k14::gfx_menu::Put32(sel + 8, uint32_t(code));
          ctx.r4.u64 = uint64_t(era);
        }
      } else {
        REXLOG_WARN("[wwe2k14][wm30] match {} not found in the game's match index (0-45): the picked match plays", n);
      }
    }
  }
  __imp__sub_82A9DFB8(ctx, base);
}
