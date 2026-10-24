// wwe2k14 - exact resolve readback while a create mode is open (Session 57, tester bug 4; default on), while a match
// loads and after a character file loads (Session 58, GitHub issues #9 / #7 / #10 / #2; default on).
//
// Some create modes draw a picture on the GPU and then read it back with the CPU to keep it in the save: Create a
// Superstar's render (made at "Creating Custom Superstar", shown on the select screen) and Create an Arena's MENU
// SCREENSHOT (the CUSTOM ARENAS thumbnail). With readback_resolve none (our default since Session 4, for speed) the
// CPU read stale memory and both came out as noise (live run 3844: the arena thumbnail noise with readback off, right
// with `cvar readback_resolve full`; the custom superstar's render the same noise as the tester's picture). So while
// the game's sequence is one of --wwe2k14_readback_sequences this sets the SDK's readback_resolve_override to 3
// (full: every resolve copied back at once, exact, a GPU wait each) and back to -1 (readback_resolve applies) 60
// frames after it leaves; matches and menus keep readback off. Checked live at 1440p (run 3845): the custom
// superstar's render and the arena thumbnail right, the Create a Superstar editor at 59 fps on the 4090. Sequence ids: docs\systems\menu_ui.md (28 Create an Arena, 76 Create a
// Superstar).
//
// Session 58: some attires are built the same way when their wrestler loads. Bobby Heenan in 30 Years of WrestleMania
// match 1 (WrestleMania 2) came out as coloured noise or black with magenta streaks (the players' pictures in #9 / #2,
// #7 / #10: Elizabeth, Savage, Cena, custom superstars): with readback off here too (run 3882 ACCURATE 1440p, 3884
// with our loaders off, 3885 at 720p), right with readback_resolve full (3883) and with the override 3 only from the
// main menu to the end of the match load (3886: CH249.PAC opens at frame 3867, the override back at -1 from 4116, he
// is right through his entrance). His default attire in a Quick Match is right without readback (3881). So the
// override is also 3 while an in-game load window is open (load_turbo.h: the pre-entrance load and the match load) and
// for --wwe2k14_readback_char_hold_frames after any character file (PAC\CH\CHnnn.PAC) opens, which covers wrestlers
// loaded during play (a Royal Rumble's entrants). Not during play otherwise.
//
//   --wwe2k14_readback_sequences=LIST   comma-separated sequence ids (N<M = N entered from M, as the fast loader's
//                                       list); empty = never (the setting stays as the command line left it)
//   --wwe2k14_readback_loads=false      not during the in-game loads / after character files
//
// A readback_resolve_override given on the command line is left alone.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>

#include "input_script.h"  // g_readback_frame_hook, g_readback_file_hook
#include "load_probe.h"    // CurrentSequence, g_seq_from, SequenceListed
#include "load_turbo.h"    // g_load_turbo_ingame_window

REXCVAR_DEFINE_STRING(wwe2k14_readback_sequences, "28,76", "wwe2k14",
                      "Session 57: game sequence ids in which every resolve is read back for the CPU at once (SDK "
                      "readback_resolve_override 3): the create modes that keep GPU-drawn pictures (28 Create an "
                      "Arena's menu screenshot, 76 Create a Superstar's render). Empty = never. See "
                      "src/create_readback.h");
REXCVAR_DEFINE_BOOL(wwe2k14_readback_loads, true, "wwe2k14",
                    "Session 58: every resolve is also read back while a match loads (the in-game load windows) and "
                    "shortly after a character file loads: some attires are drawn on the GPU and read back by the CPU "
                    "(30 Years of WrestleMania's Bobby Heenan came out as noise without it). See "
                    "src/create_readback.h");
REXCVAR_DEFINE_INT32(wwe2k14_readback_loads_mode, 3, "wwe2k14",
                     "Session 58: the SDK readback mode (readback_resolve_override) during the loads and character "
                     "windows: 1 fast (previous frame, every resolve), 2 some (previous frame, on a cache miss), 3 full "
                     "(at once, a GPU wait each). The create modes always use 3")
    .range(1, 3);
REXCVAR_DEFINE_INT32(wwe2k14_readback_char_hold_frames, 300, "wwe2k14",
                     "Session 58: frames after a character file (PAC\\CH\\CHnnn.PAC) opens during which every resolve "
                     "is read back (--wwe2k14_readback_loads)")
    .range(0, 3600);

namespace wwe2k14::create_readback {

inline bool g_on = false;
inline int g_mode = 3;              // the mode of the newest reason to be on (kept through the hold)
inline int g_set_mode = -1;         // the readback_resolve_override this last set (-1 = none)
inline uint64_t g_left_frame = 0;  // the frame the last reason to be on ended while on (0 = still a reason, or off)
// Kept on this long after leaving (review): the command processor runs behind VdSwap, so a resolve the game issued in
// the sequence's last frames may be processed after the switch.
constexpr uint64_t kHoldFrames = 60;
inline std::atomic<uint64_t> g_char_open_frame{0};  // the frame the newest character file opened (0 = none yet)

// A character's own file: ...\PAC\CH\CH<digits>.PAC (not CHETC.PAC / CHCOMMON.PAC).
inline bool IsCharacterFile(const std::string& path) {
  const size_t at = path.rfind("\\CH\\CH");
  if (at == std::string::npos || at + 6 >= path.size()) return false;
  return std::isdigit(static_cast<unsigned char>(path[at + 6])) != 0;
}

// LogFileOpen, any thread.
inline void OnFileOpened(const std::string& path, uint64_t frame) {
  if (IsCharacterFile(path)) g_char_open_frame.store(frame, std::memory_order_relaxed);
}

// True for --wwe2k14_readback_char_hold_frames after a character file opened (also load_pipeline_wait.h's window).
inline bool InCharacterWindow(uint64_t frame) {
  const uint64_t opened = g_char_open_frame.load(std::memory_order_relaxed);
  return opened != 0 && frame - opened < uint64_t(REXCVAR_GET(wwe2k14_readback_char_hold_frames));
}

// VdSwap, every guest frame (the game thread): the override follows the current sequence and the loads (logged on
// change).
inline void OnFrame(uint64_t frame) {
  const std::string list = REXCVAR_GET(wwe2k14_readback_sequences);
  const uint32_t seq = list.empty() ? 0 : load_probe::CurrentSequence();
  const uint32_t from = load_probe::g_seq_from.load(std::memory_order_relaxed);
  const char* why = nullptr;
  int why_mode = 3;  // a create mode: always full
  if (seq != 0 && load_probe::SequenceListed(seq, from, list)) {
    why = "a create mode";
  } else if (REXCVAR_GET(wwe2k14_readback_loads)) {
    why_mode = REXCVAR_GET(wwe2k14_readback_loads_mode);
    if (g_load_turbo_ingame_window().load(std::memory_order_relaxed)) {
      why = "an in-game load";
    } else if (InCharacterWindow(frame)) {
      why = "a character file loaded";
    }
  }
  if (why) g_left_frame = 0, g_mode = why_mode;
  else if (g_on && g_left_frame == 0) g_left_frame = frame;
  const bool on = why || (g_on && frame - g_left_frame < kHoldFrames);
  const int mode = on ? g_mode : -1;
  if (mode == g_set_mode) return;
  g_on = on;
  g_set_mode = mode;
  rex::cvar::SetFlagByName("readback_resolve_override", std::to_string(mode));
  REXLOG_INFO("[wwe2k14][readback] {}{} (mode {}, sequence {}, entered from {}) frame={}",
              on ? "on: resolves read back for the CPU: " : "off", on && why ? why : "", mode, seq, from, frame);
}

// OnPostSetup.
inline void Install() {
  g_readback_file_hook = OnFileOpened;  // the character window, also for load_pipeline_wait.h
  if (REXCVAR_GET(wwe2k14_readback_sequences).empty() && !REXCVAR_GET(wwe2k14_readback_loads)) return;
  if (rex::cvar::GetFlagSource("readback_resolve_override") != rex::cvar::Source::kDefault) {
    REXLOG_INFO("[wwe2k14][readback] readback_resolve_override set on the command line: the create modes and loads "
                "leave it");
    return;
  }
  g_readback_frame_hook = OnFrame;
}

}  // namespace wwe2k14::create_readback
