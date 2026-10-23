// Session 53 item 2.6 (a playtester: "I have to reconfigure my keyboard mapping every single time I open the game. It
// will not save my custom keybinds"). The keyboard-as-controller binds (the SDK's F4 settings overlay, categories
// Input/Keybinds/...) change only the running game: they are kept only if the player presses the overlay's "Save to
// config", which writes EVERY changed setting (the launcher's graphics flags too) to wwe2k14.toml next to the exe,
// a file each new build's folder starts without. Now each bind change is written by itself to
// <user data>\keybinds.toml (the save folder, so it outlives a new build) and read back at start; a bind the command
// line sets wins. Saved: the Input/Keybinds/* settings, mnk_sensitivity and mnk_mouse (not mnk_mode: the launcher's
// KEYBOARD AS CONTROLLER switch).
#pragma once

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(wwe2k14_save_keybinds, true, "wwe2k14",
                    "Session 53: keep the keyboard binds (F4 overlay, Input/Keybinds) between starts: each change is "
                    "written to <user data>\\keybinds.toml and read back at start (=false: the SDK's behaviour, only "
                    "\"Save to config\" keeps them)");

namespace wwe2k14::keybinds_save {

namespace fs = std::filesystem;

inline fs::path g_file;

inline bool IsSaved(const std::string& name) {
  if (name == "mnk_sensitivity" || name == "mnk_mouse") return true;
  const rex::cvar::FlagEntry* e = rex::cvar::GetFlagInfo(name);
  return e && e->category.rfind("Input/Keybinds", 0) == 0;
}

inline std::vector<std::string> Names() {
  std::vector<std::string> out;
  for (const std::string& n : rex::cvar::ListFlags())
    if (IsSaved(n)) out.push_back(n);
  return out;
}

inline std::string Quoted(const std::string& v) {
  std::string q = "\"";
  for (char c : v) q += (c == '"' || c == '\\') ? std::string("\\") + c : std::string(1, c);
  return q + "\"";
}

// Every saved setting that differs from its default (an empty file = all defaults again).
inline void Save() {
  if (g_file.empty()) return;
  std::ostringstream text;
  text << "# WWE 2K14 keyboard binds (written by the game when a bind changes; Session 53)\n";
  int n = 0;
  for (const std::string& name : Names()) {
    const rex::cvar::FlagEntry* e = rex::cvar::GetFlagInfo(name);
    const std::string v = rex::cvar::GetFlagByName(name);
    if (!e || v == e->default_value) continue;
    text << name << " = " << Quoted(v) << "\n";
    ++n;
  }
  std::error_code ec;
  fs::create_directories(g_file.parent_path(), ec);
  const fs::path tmp = fs::path(g_file).concat(".tmp");
  {
    std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
    if (!f) {
      REXLOG_WARN("[wwe2k14][keybinds] could not write {}", tmp.string());
      return;
    }
    f << text.str();
  }
  fs::rename(tmp, g_file, ec);
  if (ec) REXLOG_WARN("[wwe2k14][keybinds] could not replace {}: {}", g_file.string(), ec.message());
  else REXLOG_INFO("[wwe2k14][keybinds] saved {} changed bind(s) to {}", n, g_file.string());
}

// `name = "value"` lines (TOML strings with \" and \\), or `name = value`.
inline void Load() {
  std::ifstream f(g_file, std::ios::binary);
  if (!f) {
    REXLOG_INFO("[wwe2k14][keybinds] no saved binds ({}): the defaults", g_file.string());
    return;
  }
  std::string line, applied;
  int n = 0;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    auto trim = [](std::string s) {
      while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
      while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
      return s;
    };
    const std::string name = trim(line.substr(0, eq));
    std::string raw = trim(line.substr(eq + 1)), value;
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
      for (size_t i = 1; i + 1 < raw.size(); ++i) value += (raw[i] == '\\' && i + 2 < raw.size()) ? raw[++i] : raw[i];
    } else {
      value = raw;
    }
    if (!IsSaved(name)) continue;  // only binds: a hand-edited file cannot set other settings
    if (rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) continue;  // the command line wins
    if (rex::cvar::SetFlagByName(name, value)) applied += (n++ ? ", " : "") + name + "=" + value;
  }
  REXLOG_INFO("[wwe2k14][keybinds] loaded {} bind(s) from {}{}{}", n, g_file.string(), n ? ": " : "", applied);
}

// OnPostSetup (the overlays' own binds are registered by then): read the file, then save on every change.
inline void Install(const fs::path& user_data_root) {
  if (!REXCVAR_GET(wwe2k14_save_keybinds) || user_data_root.empty()) return;
  g_file = user_data_root / "keybinds.toml";
  Load();
  for (const std::string& name : Names())
    rex::cvar::RegisterChangeCallback(name, [](std::string_view, std::string_view) { Save(); });
}

}  // namespace wwe2k14::keybinds_save
