// S101 lane 127 (docs/renderer/PHONE_PROMOTIONS.md): a way to switch the renderer promotions on for a +dev APK started from the
// launcher, the same flags the PC takes (--wwe2k14_rdr_arena_ours, ...). Players see nothing: a release build (no "+dev" in
// WWE2K14_VERSION) and every non-Android build skip this whole file's work.
//
// <files>/dev_flags.txt (the app's working folder on Android, where logs/startup.txt is written), one flag per line:
//     --wwe2k14_scene_records            (a flag alone = true)
//     --wwe2k14_rdr_arena_ours=true      (--name=value; the leading "--" is optional)
//     # a comment; blank lines skipped
// Read on every start and NOT deleted (args.txt, the test-run file, is read once and deleted, and replaces the launcher's arguments
// entirely; this file is added to them). A flag already set on the command line (the launcher's, args.txt's) keeps its value.
// Only the promotion family may be set this way: names starting wwe2k14_rdr_ or wwe2k14_scene_ (never paths, drivers or the game root).
// Lane 57's bug button in debuggable APKs lives in GameActivity.java and is unrelated: this file is native and touches no Java.
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstdio>
#include <string>
#include <vector>

#include "gfx_menu.h"  // IsDevBuild

namespace wwe2k14::android_dev_flags {

// One "[--]name[=value]" line -> {name, value}; empty name = skip the line.
struct Flag {
  std::string name, value;
};
inline Flag ParseLine(std::string line) {
  auto trim = [](std::string& s) {
    const char* ws = " \t\r\n";
    const size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return s.clear();
    s = s.substr(a, s.find_last_not_of(ws) - a + 1);
  };
  trim(line);
  if (line.empty() || line[0] == '#') return {};
  if (line.rfind("--", 0) == 0) line = line.substr(2);
  Flag f;
  const size_t eq = line.find('=');
  f.name = eq == std::string::npos ? line : line.substr(0, eq);
  f.value = eq == std::string::npos ? "true" : line.substr(eq + 1);
  trim(f.name);
  trim(f.value);
  const bool family = f.name.rfind("wwe2k14_rdr_", 0) == 0 || f.name.rfind("wwe2k14_scene_", 0) == 0 ||
                      f.name.rfind("wwe2k14_owner_", 0) == 0;  // lane 241 (3b): --wwe2k14_owner_boot_submit_ours / _guest_gpu_only on a +dev APK
  if (!family) return {};
  return f;
}

// OnPreSetup, first thing (logging is up; nothing has read a promotion switch yet).
inline void Apply() {
#if defined(__ANDROID__)
  if (!wwe2k14::gfx_menu::IsDevBuild()) return;
  std::FILE* fp = std::fopen("dev_flags.txt", "rb");
  if (!fp) return;
  std::string text;
  char buf[4096];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, fp)) > 0;) text.append(buf, n);
  std::fclose(fp);
  size_t at = 0, applied = 0, kept = 0, skipped = 0;
  while (at <= text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    const Flag f = ParseLine(text.substr(at, end - at));
    at = end + 1;
    if (f.name.empty()) {
      ++skipped;
      continue;
    }
    if (rex::cvar::GetFlagSource(f.name) != rex::cvar::Source::kDefault) {  // (also true for an unknown name: GetFlagSource says so below)
      ++kept;
      REXLOG_INFO("[devflags] {} kept ('{}', set elsewhere)", f.name, rex::cvar::GetFlagByName(f.name));
      continue;
    }
    const bool set = rex::cvar::SetFlagByName(f.name, f.value);
    ++applied;
    REXLOG_INFO("[devflags] {} = '{}' {}", f.name, f.value, set ? "(dev_flags.txt)" : "NOT FOUND: no such switch in this build");
  }
  REXLOG_INFO("[devflags] dev_flags.txt: {} applied, {} kept, {} lines skipped", applied, kept, skipped);
#endif
}

}  // namespace wwe2k14::android_dev_flags
