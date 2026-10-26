// wwe2k14 launcher - the keyboard controls (Session 67; Jarrod: "We have keyboard support but no way in the launcher to
// actually map the controls"). No UI code here; the KEYBOARD CONTROLS window is in launcher_main.cpp.
//
// The keyboard-as-controller driver is ReXGlue's (thirdparty\rexglue-sdk-src\src\input\mnk\mnk_input_driver.cpp,
// --mnk_mode, the launcher's KEYBOARD CONTROLS switch). Each pad control is a string setting keybind_* holding
// alternatives separated by commas, each with optional Shift+ / Ctrl+ / Alt+ prefixes ("Semicolon,Space",
// "Shift+Up"); the key names are the SDK's (src\ui\keybinds.cpp kKeyNames). The game keeps them in
// <save folder>\keybinds.toml (src\keybinds_save.h, Session 53: `name = "value"` lines, only values that differ from
// the default; the F4 overlay writes it on every change and the game reads it at start). The launcher reads and writes
// that same file, so the two never disagree; lines it does not manage (the F-key tool binds) are kept as they are.
//
// The game sees keys by position (SDL scancodes -> US key names, src\ui\sdl_virtual_key.cpp), so the launcher captures
// by position too (the Windows scan code) and shows each key with the label of the player's own layout.
#pragma once

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace launcher::keys {

namespace fs = std::filesystem;

// The pad controls, in the order the window lists them. `def` = the SDK's default (mnk_input_driver.cpp, SDK 12af82b).
struct Bind {
  const char* cvar;
  const char* label;  // the row's name in the window (under its group's heading for the sticks and the D-pad)
  const char* def;
};
inline constexpr Bind kBinds[] = {
    {"keybind_a", "A", "Semicolon,Space"},                  // 0
    {"keybind_b", "B", "Quote,Backspace"},                  // 1
    {"keybind_x", "X", "L"},                                // 2
    {"keybind_y", "Y", "P"},                                // 3
    {"keybind_left_trigger", "LT", "Q,I"},                  // 4
    {"keybind_right_trigger", "RT", "E,O"},                 // 5
    {"keybind_left_shoulder", "LB", "1"},                   // 6
    {"keybind_right_shoulder", "RB", "3"},                  // 7
    {"keybind_lstick_up", "UP", "W"},                       // 8
    {"keybind_lstick_down", "DOWN", "S"},                   // 9
    {"keybind_lstick_left", "LEFT", "A"},                   // 10
    {"keybind_lstick_right", "RIGHT", "D"},                 // 11
    {"keybind_lstick_press", "PRESS", "F"},                 // 12
    {"keybind_rstick_up", "UP", "Up"},                      // 13
    {"keybind_rstick_down", "DOWN", "Down"},                // 14
    {"keybind_rstick_left", "LEFT", "Left"},                // 15
    {"keybind_rstick_right", "RIGHT", "Right"},             // 16
    {"keybind_rstick_press", "PRESS", "K"},                 // 17
    {"keybind_dpad_up", "UP", "Shift+Up"},                  // 18
    {"keybind_dpad_down", "DOWN", "Shift+Down"},            // 19
    {"keybind_dpad_left", "LEFT", "Shift+Left"},            // 20
    {"keybind_dpad_right", "RIGHT", "Shift+Right"},         // 21
    {"keybind_back", "BACK", "Z,Tab"},                      // 22
    {"keybind_start", "START", "X,Return"},                 // 23
    {"keybind_guide", "GUIDE", ""},                         // 24
};
inline constexpr int kBindCount = int(std::size(kBinds));
inline constexpr int kMaxAlternatives = 3;  // the window's limit (the SDK takes any number)

// The mouse settings the game keeps in the same file (keybinds_save.h IsSaved).
inline constexpr const char* kMouseCvar = "mnk_mouse";              // the mouse moves the right stick (default off)
inline constexpr const char* kSensitivityCvar = "mnk_sensitivity";  // its speed (default 1.0, range 0.01-10)

inline std::vector<std::string> Split(const std::string& v) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
    while (!cur.empty() && cur.back() == ' ') cur.pop_back();
    if (!cur.empty()) out.push_back(cur);
    cur.clear();
  };
  for (char c : v) c == ',' ? flush() : void(cur += c);
  flush();
  return out;
}
inline std::string Join(const std::vector<std::string>& alts) {
  std::string v;
  for (const auto& a : alts) v += (v.empty() ? "" : ",") + a;
  return v;
}

// ---- The file (the same format keybinds_save.h writes and reads)

struct BindFile {
  std::map<std::string, std::string> values;  // every `name = value` line read (the managed ones and any others)
  bool exists = false;
};

inline BindFile LoadBindFile(const fs::path& file) {
  BindFile b;
  std::ifstream f(file, std::ios::binary);
  if (!f) return b;
  b.exists = true;
  std::string line;
  auto trim = [](std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    return s;
  };
  while (std::getline(f, line)) {
    line = trim(line);
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    const std::string name = trim(line.substr(0, eq));
    std::string raw = trim(line.substr(eq + 1)), value;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
      for (size_t i = 1; i + 1 < raw.size(); ++i) value += (raw[i] == '\\' && i + 2 < raw.size()) ? raw[++i] : raw[i];
    } else {
      value = raw;
    }
    if (!name.empty()) b.values[name] = value;
  }
  return b;
}

inline std::string Quoted(const std::string& v) {
  std::string q = "\"";
  for (char c : v) q += (c == '"' || c == '\\') ? std::string("\\") + c : std::string(1, c);
  return q + "\"";
}

// The value of a pad control: the file's, else the default.
inline std::string Value(const BindFile& b, int i) {
  const auto it = b.values.find(kBinds[i].cvar);
  return it != b.values.end() ? it->second : kBinds[i].def;
}
inline bool MouseOn(const BindFile& b) {
  const auto it = b.values.find(kMouseCvar);
  return it != b.values.end() && (it->second == "true" || it->second == "1");
}
inline double Sensitivity(const BindFile& b) {
  const auto it = b.values.find(kSensitivityCvar);
  const double v = it != b.values.end() ? std::atof(it->second.c_str()) : 1.0;
  return v > 0 ? v : 1.0;
}

// Sets a managed value; a default one leaves the file (like the game, which writes only changed values).
inline void Set(BindFile& b, const std::string& name, const std::string& value, const std::string& def) {
  if (value == def) b.values.erase(name);
  else b.values[name] = value;
}
inline void SetBind(BindFile& b, int i, const std::string& value) { Set(b, kBinds[i].cvar, value, kBinds[i].def); }
inline void SetMouse(BindFile& b, bool on) { Set(b, kMouseCvar, on ? "true" : "false", "false"); }
inline void SetSensitivity(BindFile& b, double v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  if (std::fabs(v - 1.0) < 1e-6) b.values.erase(kSensitivityCvar);
  else b.values[kSensitivityCvar] = buf;
}

// Written whole (a temporary file, then renamed over), every value in the map. Returns false on an error.
inline bool SaveBindFile(const fs::path& file, const BindFile& b) {
  std::ostringstream text;
  text << "# WWE 2K14 keyboard binds (written by the game or the launcher when a bind changes; Session 53 / 67)\n";
  for (const auto& [name, value] : b.values) text << name << " = " << Quoted(value) << "\n";
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = fs::path(file).concat(".tmp");
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f) return false;
    f << text.str();
    if (!f) return false;
  }
  fs::rename(tmp, file, ec);
  return !ec;
}

// ---- Key names

// Windows scan codes (set 1; `ext` = the E0 extended flag) of the keys the SDK can bind, by their SDK names: the US
// key at that position, as SDL's scancodes give it to the game.
struct ScanName {
  uint16_t scan;
  bool ext;
  const char* name;
};
inline constexpr ScanName kScanNames[] = {
    {0x02, false, "1"},  {0x03, false, "2"},  {0x04, false, "3"},  {0x05, false, "4"},  {0x06, false, "5"},
    {0x07, false, "6"},  {0x08, false, "7"},  {0x09, false, "8"},  {0x0A, false, "9"},  {0x0B, false, "0"},
    {0x0C, false, "Minus"}, {0x0D, false, "Plus"}, {0x0E, false, "Backspace"}, {0x0F, false, "Tab"},
    {0x10, false, "Q"},  {0x11, false, "W"},  {0x12, false, "E"},  {0x13, false, "R"},  {0x14, false, "T"},
    {0x15, false, "Y"},  {0x16, false, "U"},  {0x17, false, "I"},  {0x18, false, "O"},  {0x19, false, "P"},
    {0x1A, false, "LBracket"}, {0x1B, false, "RBracket"}, {0x1C, false, "Return"}, {0x1C, true, "Return"},
    {0x1E, false, "A"},  {0x1F, false, "S"},  {0x20, false, "D"},  {0x21, false, "F"},  {0x22, false, "G"},
    {0x23, false, "H"},  {0x24, false, "J"},  {0x25, false, "K"},  {0x26, false, "L"},  {0x27, false, "Semicolon"},
    {0x28, false, "Quote"}, {0x29, false, "Backtick"}, {0x2B, false, "Backslash"},
    {0x2C, false, "Z"},  {0x2D, false, "X"},  {0x2E, false, "C"},  {0x2F, false, "V"},  {0x30, false, "B"},
    {0x31, false, "N"},  {0x32, false, "M"},  {0x33, false, "Comma"}, {0x34, false, "Period"}, {0x35, false, "Slash"},
    {0x37, false, "NumpadStar"}, {0x39, false, "Space"}, {0x3A, false, "CapsLock"},
    {0x3B, false, "F1"}, {0x3C, false, "F2"}, {0x3D, false, "F3"}, {0x3E, false, "F4"}, {0x3F, false, "F5"},
    {0x40, false, "F6"}, {0x41, false, "F7"}, {0x42, false, "F8"}, {0x43, false, "F9"}, {0x44, false, "F10"},
    {0x57, false, "F11"}, {0x58, false, "F12"},
    {0x64, false, "F13"}, {0x65, false, "F14"}, {0x66, false, "F15"}, {0x67, false, "F16"}, {0x68, false, "F17"},
    {0x69, false, "F18"}, {0x6A, false, "F19"}, {0x6B, false, "F20"}, {0x6C, false, "F21"}, {0x6D, false, "F22"},
    {0x6E, false, "F23"}, {0x76, false, "F24"},
    {0x46, false, "ScrollLock"},
    {0x47, false, "Numpad7"}, {0x48, false, "Numpad8"}, {0x49, false, "Numpad9"}, {0x4A, false, "NumpadMinus"},
    {0x4B, false, "Numpad4"}, {0x4C, false, "Numpad5"}, {0x4D, false, "Numpad6"}, {0x4E, false, "NumpadPlus"},
    {0x4F, false, "Numpad1"}, {0x50, false, "Numpad2"}, {0x51, false, "Numpad3"}, {0x52, false, "Numpad0"},
    {0x35, true, "NumpadSlash"}, {0x37, true, "PrintScreen"},
    {0x47, true, "Home"}, {0x48, true, "Up"}, {0x49, true, "PageUp"}, {0x4B, true, "Left"}, {0x4D, true, "Right"},
    {0x4F, true, "End"}, {0x50, true, "Down"}, {0x51, true, "PageDown"}, {0x52, true, "Insert"}, {0x53, true, "Delete"},
};

// A WM_KEYDOWN (its wParam and lParam) as an SDK key name; "" = a key the game cannot bind (the ISO key left of Z, the
// number pad's '.', the Windows keys, ...). Pause and Num Lock share a scan code: by their virtual key.
inline std::string NameFromKeyDown(WPARAM vk, LPARAM lp) {
  if (vk == VK_PAUSE) return "Pause";
  if (vk == VK_NUMLOCK) return "NumLock";
  const uint16_t scan = uint16_t((lp >> 16) & 0xFF);
  const bool ext = (lp >> 24) & 1;
  for (const ScanName& s : kScanNames)
    if (s.scan == scan && s.ext == ext) return s.name;
  return {};
}

inline bool IsModifierKey(WPARAM vk) {
  return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL ||
         vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN;
}

// "Shift+" / "Ctrl+" / "Alt+" before a key name, in the SDK's spelling (mnk_input_driver.cpp TakeModifiers).
inline std::string WithModifiers(const std::string& key, bool shift, bool ctrl, bool alt) {
  return std::string(shift ? "Shift+" : "") + (ctrl ? "Ctrl+" : "") + (alt ? "Alt+" : "") + key;
}

// How a bind alternative is shown: the player's own layout's label for the character keys ("A" on an AZERTY keyboard
// for the SDK's "Q"), words for the rest, modifiers first ("SHIFT + UP").
inline std::string Display(const std::string& token) {
  std::string rest = token, prefix;
  for (;;) {
    const size_t plus = rest.find('+');
    if (plus == std::string::npos || plus == 0) break;
    const std::string head = rest.substr(0, plus);
    if (head == "Shift") prefix += "SHIFT + ";
    else if (head == "Ctrl" || head == "Control") prefix += "CTRL + ";
    else if (head == "Alt") prefix += "ALT + ";
    else break;
    rest = rest.substr(plus + 1);
  }
  static const std::map<std::string, std::string> kWords = {
      {"Return", "ENTER"},       {"Space", "SPACE"},        {"Backspace", "BACKSPACE"}, {"Tab", "TAB"},
      {"Escape", "ESC"},         {"Delete", "DELETE"},      {"Insert", "INSERT"},       {"Home", "HOME"},
      {"End", "END"},            {"PageUp", "PAGE UP"},     {"PageDown", "PAGE DOWN"},  {"Up", "UP"},
      {"Down", "DOWN"},          {"Left", "LEFT"},          {"Right", "RIGHT"},         {"CapsLock", "CAPS LOCK"},
      {"NumLock", "NUM LOCK"},   {"ScrollLock", "SCROLL LOCK"}, {"Pause", "PAUSE"},     {"PrintScreen", "PRINT SCREEN"},
      {"NumpadPlus", "NUM +"},   {"NumpadMinus", "NUM -"},  {"NumpadStar", "NUM *"},    {"NumpadSlash", "NUM /"},
      {"NumpadEnter", "ENTER"},  {"LMB", "LEFT CLICK"},     {"RMB", "RIGHT CLICK"},     {"MMB", "MIDDLE CLICK"},
      {"Shift", "SHIFT"},        {"Control", "CTRL"},       {"Alt", "ALT"},
  };
  if (const auto it = kWords.find(rest); it != kWords.end()) return prefix + it->second;
  if (rest.rfind("Numpad", 0) == 0 && rest.size() == 7) return prefix + "NUM " + rest.substr(6);
  if (rest.size() >= 2 && rest[0] == 'F' && std::isdigit(static_cast<unsigned char>(rest[1]))) return prefix + rest;
  // A character key: the label of the key at that position in the player's layout.
  for (const ScanName& s : kScanNames) {
    if (s.ext || rest != s.name) continue;
    const UINT vk = MapVirtualKeyW(s.scan, MAPVK_VSC_TO_VK_EX);
    const UINT ch = vk ? (MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7FFF) : 0;  // (the high bit marks a dead key)
    if (ch > 0x20 && ch < 0x80) return prefix + std::string(1, char(std::toupper(int(ch))));
    if (ch >= 0x80) {  // a letter outside ASCII (Ä, É, ...): UTF-8, upper case
      wchar_t w[2] = {wchar_t(ch), 0};
      CharUpperW(w);
      char buf[8] = {};
      const int n = WideCharToMultiByte(CP_UTF8, 0, w, 1, buf, sizeof(buf), nullptr, nullptr);
      if (n > 0) return prefix + std::string(buf, size_t(n));
    }
    break;
  }
  std::string up = rest;
  for (char& c : up) c = char(std::toupper(static_cast<unsigned char>(c)));
  return prefix + up;
}

}  // namespace launcher::keys
