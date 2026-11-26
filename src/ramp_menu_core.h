// S111 RAMPMENU (docs\research\s111\RAMPMENU.md; Jarrod 2026-11-26 21:3x): the pure rules of the RAMP CAMERA row in the in-match pause
// menu's VISUAL PRESENTATION page. src\ramp_menu.h is the game side; tools\menus\ramp_menu_test.cpp compiles this header alone (no SDK).
//
// The in-match pause menu (vtable 0x8200D898, constructor sub_82385598) is ONE object with a page stack: the page kind at +392 (0 the main
// list, 1 CONTROLS, 2 MATCH INFORMATION, 3 VISUAL PRESENTATION, 4 HUD; sub_82385438 picks the filler). Page 3's filler sub_82385340
// writes seven rows: row i's action = i at +196 + 4 i, its 20-byte record at +228 + 20 i (+16 = the text id, from the table 0x8200D7E8
// {text id, type} x 7), the count at +44, then hands them to sub_82391928 (the call returns to 0x823853BC). The action and row arrays hold 8
// (+196 .. +227, +228 .. +387), so one more row fits after the seven, as PHOTO MODE fits after the main list's seven (src\photo_mode.h).
#pragma once

#include <cstdint>
#include <regex>
#include <string>

namespace wwe2k14::ramp_menu {

constexpr uint32_t kPauseMenuVtable = 0x8200D898;
constexpr uint32_t kVpFillerRet = 0x823853BC;  // sub_82385340 +78 calls sub_82391928; the listing sets lr = 0x823853BC
constexpr uint32_t kMenuCount = 44, kMenuActions = 196, kMenuRows = 228, kRowSize = 20, kRowText = 16, kMaxRows = 8;
constexpr uint32_t kStockRows = 7;
// The stock rows' text ids in order (the table at 0x8200D7E8): CAMERA CUTS, TARGETING INDICATORS, DISPLAY COUNTS, HUD, SPECIAL EFFECTS,
// REVERSAL HELP TEXT, POST MATCH REPLAY.
constexpr uint32_t kStockTexts[kStockRows] = {2285, 2302, 2526, 2301, 2310, 41265, 2477};
// Our row: index 7, action 7 (the filler's rule: action = index). The activate handler sub_823839D0 looks the action up in page 3's table
// without a range check (sub_823830D8): index 7 is the next table's first entry {2319, 5}, type 5 = the same "close and report" type as
// every stock row of the page, so A on it closes the menu and the flow hands action 7 to the page handler (src\ramp_menu.h).
constexpr uint32_t kRampAction = 7;
// CAMERA CUTS' byte G+15204 (G = 0x8376AAF4, the constant sub_827D7578 returns): its ON / OFF question opens on row 0 when the byte is 1
// (sub_8235C218 +80..+D8) and its answer is written here (sub_8235CBE0 +34..+50).
constexpr uint32_t kGlobals = 0x8376AAF4;
constexpr uint32_t kCameraCutsByte = kGlobals + 15204;
constexpr uint32_t kVpHeaderText = 2529;  // the question's header, VISUAL PRESENTATION (sub_8238B5F0 +60: sub_823A1E48(q, 2529, 2285))

inline uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
inline uint32_t RowText(const uint8_t* m, uint32_t i) { return Be32(m + kMenuRows + kRowSize * i + kRowText); }
inline uint32_t RowAction(const uint8_t* m, uint32_t i) { return Be32(m + kMenuActions + 4 * i); }

// The page as the stock filler left it (m = the menu object, guest bytes, big-endian)? "" = yes, else why not.
inline std::string StockVpWhy(const uint8_t* m) {
  if (Be32(m) != kPauseMenuVtable) return "not the pause menu (vtable " + std::to_string(Be32(m)) + ")";
  if (Be32(m + kMenuCount) != kStockRows) return "rows " + std::to_string(Be32(m + kMenuCount)) + ", not 7";
  for (uint32_t i = 0; i < kStockRows; ++i) {
    if (RowAction(m, i) != i) return "row " + std::to_string(i) + " has action " + std::to_string(RowAction(m, i));
    if (RowText(m, i) != kStockTexts[i]) return "row " + std::to_string(i) + " has text " + std::to_string(RowText(m, i));
  }
  return "";
}

// RAMP CAMERA after the seven stock rows, which are not touched. true = added (the caller then passes 8 rows to sub_82391928).
inline bool AddVpRow(uint8_t* m, uint32_t text) {
  if (!StockVpWhy(m).empty()) return false;
  uint8_t* row = m + kMenuRows + kRowSize * kRampAction;
  for (uint32_t k = 0; k < kRowSize; k += 4) PutBe32(row + k, 0);
  PutBe32(row + kRowText, text);
  PutBe32(m + kMenuActions + 4 * kRampAction, kRampAction);
  PutBe32(m + kMenuCount, kStockRows + 1);
  return true;
}

// The page holds our row (so the cursor may be put on it).
inline bool HasRampRow(const uint8_t* m, uint32_t text) {
  return Be32(m + kMenuCount) == kStockRows + 1 && RowAction(m, kRampAction) == kRampAction && RowText(m, kRampAction) == text;
}

// The question's answer = the row picked (0 ON, 1 OFF: the game's own reading in sub_8235CBE0 +34..+50); any other value changes nothing.
inline bool AnswerToValue(uint32_t answer, bool current) { return answer == 0 ? true : answer == 1 ? false : current; }
// The byte CAMERA CUTS' question reads to open on the current value (1 = ON, opens on row 0).
inline uint8_t QuestionByte(bool on) { return on ? 1 : 0; }
// S111 RAMPMENU review F1: the answer handed to the game's flow state 4 for OUR question. sub_8235CBE0 +34..+50 writes CAMERA CUTS' byte
// for 0 (-> 1) and 1 (-> 0) only; any other value writes nothing and goes on to the same destroy + page-3 rebuild. So CAMERA CUTS' setting
// is never written by our row, not even for the length of the call (our own answer is read from r4 before the call).
constexpr uint32_t kAnswerNoWrite = 2;

// Sets (or adds) a true / false key in the JSON text, leaving the rest as it was (the splice of gfx_settings.h SetInt).
inline std::string SetBoolKey(std::string json, const char* key, bool value) {
  const char* word = value ? "true" : "false";
  const std::regex re(std::string("(\"") + key + "\"\\s*:\\s*)(true|false)");
  std::smatch m;
  if (std::regex_search(json, m, re))
    return json.substr(0, size_t(m.position(2))) + word + json.substr(size_t(m.position(2) + m.length(2)));
  const size_t brace = json.find('{');
  const std::string entry = std::string("\"") + key + "\": " + word;
  if (brace == std::string::npos) return "{\n  " + entry + "\n}\n";
  const size_t next = json.find_first_not_of(" \t\r\n", brace + 1);
  const bool empty_object = next == std::string::npos || json[next] == '}';
  return json.substr(0, brace + 1) + "\n  " + entry + (empty_object ? "" : ",") + json.substr(brace + 1);
}

// S111 RAMPMENU review F3 / F4 (main session decisions): the switch's start value, decided here and nowhere else, in this order:
//   1. replay playback (--wwe2k14_replay_play set): the recording decides (RAMPCAM: src\replay_carried.h carries the switch, replay.h
//      Want applies it); the file is not read;
//   2. a value from anywhere but the default (the command line; a check profile's pin, xdet_check.h; a recording's value already applied):
//      it wins (the PC launcher passes an explicit true / false on every start but replay playback, so a launcher start stops here);
//   3. else the settings file the row writes, only where CLAUDE.md's safety rule allows ("a +dev build never applies the file by itself"):
//      a release build (exe or APK: the phone keeps its read), or --wwe2k14_settings_file given, or use_launcher_settings on.
// `on` is set only for kFile. The caller runs after replay::PreSetup (wwe2k14_app.h), so no later Want sees our value as given.
struct RampStart {
  bool replay_playback = false;
  bool command_line = false;  // the switch's source is not the default
  bool dev_build = true;
  bool settings_file_given = false;
  bool use_launcher_settings = false;
};
enum class StartFrom { kRecording, kCommandLine, kNotAllowed, kNoKey, kFile };
// `read_json()` returns the settings file's text; it is called only at step 3 (a +dev build without a settings file never opens the
// player's file at all).
template <class ReadJson>
inline StartFrom StartDecisionRead(const RampStart& s, ReadJson read_json, bool& on) {
  if (s.replay_playback) return StartFrom::kRecording;
  if (s.command_line) return StartFrom::kCommandLine;
  if (s.dev_build && !s.settings_file_given && !s.use_launcher_settings) return StartFrom::kNotAllowed;
  const std::string json = read_json();
  const std::regex re("\"ramp_camera\"\\s*:\\s*(true|false)");
  std::smatch m;
  if (!std::regex_search(json, m, re)) return StartFrom::kNoKey;
  on = m[1].str() == "true";
  return StartFrom::kFile;
}
inline StartFrom StartDecision(const RampStart& s, const std::string& json, bool& on) {
  return StartDecisionRead(s, [&json] { return json; }, on);
}

}  // namespace wwe2k14::ramp_menu
