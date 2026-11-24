// wwe2k14 - S110 TRONSLOT: custom titantron SLOTS (docs\research\s110\TRONSLOT.md). Portable, std only: the game (src\tron_slots.h,
// src\tron_shim.h), the importer, the launchers and the viewer all use THIS one rule (no "keep in step" copies as with songs).
//
// A custom tron is a movie number N from a fixed band that no game data on the player's files uses (a host test asserts it:
// tools\importer\test_core.cpp), FILLED when <mods>\titantron\ holds a video named <N>.<ext> (the per-number file the existing redirect
// already plays, src\mods.h FindTron) AND <mods>\titantron\custom.txt names it ("N<TAB>display name", one per line; our own file).
// Empty slots exist nowhere: no rows, no placeholders. A number is never given to another video while its line exists (a save or an
// import that holds N must never play another video), also when its file is gone.
//
// S110 TRON-B: a REMOVED custom tron keeps its number as a RETIRED line, exactly "#retired<TAB>N<TAB>name" (the name it had). It starts
// with '#', so every reader (also one older than this form) skips it as a comment: never listed, never a problem, never a MOVIE row. This
// reader keeps it in Library::retired so the number is not handed out again: NextFreeId takes a retired number only when no never-used
// number is left (the lowest one), and a retired number gets a video again only through an explicit restore (tron_slots_ui_core.h).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace wwe2k14::tron_slots_core {

// The band: 823-843, 845-899, 910-949 (116 numbers), the clean sheet runs of the note's "Free in every space" (the numbers around
// them are named in the game's data: 844 and 900-909 are held out; a host test checks every band number on the player's files).
inline constexpr int kBand[][2] = {{823, 843}, {845, 899}, {910, 949}};
inline constexpr int kBandCount = 21 + 55 + 40;  // 116

inline constexpr bool IsSlotId(int n) {
  for (const auto& r : kBand)
    if (n >= r[0] && n <= r[1]) return true;
  return false;
}
inline std::vector<int> BandIds() {
  std::vector<int> v;
  for (const auto& r : kBand)
    for (int n = r[0]; n <= r[1]; ++n) v.push_back(n);
  return v;
}

// A tron video's extensions in priority order (the game's own format first): THE one list. The redirect / the tron shim (src\mods.h), the
// launcher (src\launcher\launcher_core.h) and the viewer (src\assets\mods_lookup.h) take it from here.
inline constexpr const char* kTronExts[] = {".bik2", ".mp4", ".mov", ".mkv", ".alpha.mp4", ".alpha.mov", ".alpha.mkv"};
// What the game's MOVIE list can show: the same limit as the new songs' names (src\music_slots.h CleanName: printable ASCII, 40).
inline constexpr size_t kMaxName = 40;
inline constexpr const char* kFileName = "custom.txt";
inline constexpr const char* kRetiredTag = "#retired";  // the retired line: kRetiredTag<TAB>N<TAB>name (see the top)

struct Slot {
  int id = 0;
  std::string name;
  bool present = false;           // a video named <id>.<ext> exists in the folder
  std::filesystem::path file;     // that video (empty when not present)
  bool retired = false;           // a removed tron's kept number (written as the retired line; never present, never listed)
};

// What a folder holds. `listed` = line AND file (what the game and the UIs show), sorted by id. `line_only` = a line whose file is gone (keeps
// its number: never reused). `file_only` = a video in the band with no line (reported, never listed). `retired` = removed trons' numbers
// (with their old names; never listed, never reported). `problems` = text for lines that could not be read (outside the band, a duplicate
// number, no number).
struct Library {
  std::vector<Slot> listed, line_only, retired;
  std::vector<int> file_only;
  std::vector<std::string> problems;
  bool has_file = false;  // custom.txt exists
  // every line (listed + line_only + retired) by id: what NextFreeId and WriteSlots take
  std::vector<Slot> Lines() const {
    std::vector<Slot> v = listed;
    v.insert(v.end(), line_only.begin(), line_only.end());
    v.insert(v.end(), retired.begin(), retired.end());
    std::sort(v.begin(), v.end(), [](const Slot& a, const Slot& b) { return a.id < b.id; });
    return v;
  }
};

// One line of printable text: ASCII 32..126 only, trimmed, at most kMaxName characters. Empty stays empty.
inline std::string CleanName(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '\t') c = ' ';
    if (c < 32 || c > 126) continue;
    out += c;
  }
  size_t a = 0, b = out.size();
  while (a < b && out[a] == ' ') ++a;
  while (b > a && out[b - 1] == ' ') --b;
  out = out.substr(a, b - a);
  if (out.size() > kMaxName) {
    out.resize(kMaxName);
    while (!out.empty() && out.back() == ' ') out.pop_back();
  }
  return out;
}

// The video for tron number n in the folder (kTronExts order), or an empty path.
inline std::filesystem::path FileFor(const std::filesystem::path& titantron_dir, int n) {
  std::error_code ec;
  for (const char* ext : kTronExts) {
    const auto p = titantron_dir / (std::to_string(n) + ext);
    if (std::filesystem::is_regular_file(p, ec)) return p;
  }
  return {};
}

// custom.txt + the files present. A missing folder / file = an empty library. `find_unlisted` = also look for band videos with no line
// (file_only: up to 116 x 7 file checks, for the UIs); the game's list hook passes false (it runs at every load of the lists).
inline Library ReadSlots(const std::filesystem::path& titantron_dir, bool find_unlisted = true) {
  namespace fs = std::filesystem;
  Library lib;
  std::error_code ec;
  std::set<int> named;
  std::vector<Slot> retired;  // retired lines in file order (kept below unless a live line names the same number)
  std::ifstream in(titantron_dir / kFileName, std::ios::binary);
  if (in) {
    lib.has_file = true;
    std::string line;
    bool first = true;
    const std::string rtag = std::string(kRetiredTag) + "\t";
    while (std::getline(in, line)) {
      if (first && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // a BOM (Notepad)
      first = false;
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
      size_t a = 0;
      while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
      if (a < line.size() && line.compare(a, rtag.size(), rtag) == 0) {  // a retired line: #retired<TAB>N<TAB>name (a bad one is a comment)
        const size_t b = a + rtag.size(), t = line.find('\t', b);
        const std::string num = line.substr(b, t == std::string::npos ? std::string::npos : t - b);
        char* end = nullptr;
        const long n = std::strtol(num.c_str(), &end, 10);
        if (!num.empty() && end != num.c_str() && *end == '\0' && IsSlotId(int(n))) {
          Slot s;
          s.id = int(n), s.retired = true;
          s.name = t == std::string::npos ? std::string() : CleanName(line.substr(t + 1));
          if (s.name.empty()) s.name = "Custom tron " + std::to_string(s.id);
          retired.push_back(std::move(s));
        }
        continue;
      }
      if (a >= line.size() || line[a] == '#') continue;
      size_t tab = line.find('\t', a);  // the number ends at the TAB (or, hand-edited, at the first space)
      if (tab == std::string::npos) tab = line.find(' ', a);
      const std::string num = line.substr(a, tab == std::string::npos ? std::string::npos : tab - a);
      char* end = nullptr;
      const long n = std::strtol(num.c_str(), &end, 10);
      if (num.empty() || end == num.c_str() || (*end != '\0' && *end != ' ') || n < 0 || n > 9999) {
        lib.problems.push_back("custom.txt: no tron number in '" + line + "'");
        continue;
      }
      if (!IsSlotId(int(n))) {
        lib.problems.push_back("custom.txt: " + std::to_string(n) + " is not a custom tron number (823-843, 845-899, 910-949)");
        continue;
      }
      if (!named.insert(int(n)).second) {
        lib.problems.push_back("custom.txt: number " + std::to_string(n) + " appears twice (the first line is kept)");
        continue;
      }
      Slot s;
      s.id = int(n);
      s.name = tab == std::string::npos ? std::string() : CleanName(line.substr(tab + 1));
      if (s.name.empty()) s.name = "Custom tron " + std::to_string(s.id);
      s.file = FileFor(titantron_dir, s.id);
      s.present = !s.file.empty();
      (s.present ? lib.listed : lib.line_only).push_back(std::move(s));
    }
  }
  for (Slot& s : retired)  // a live line for the same number wins (a hand edit); a retired number twice keeps the first
    if (named.insert(s.id).second) lib.retired.push_back(std::move(s));
  const auto by_id = [](const Slot& a, const Slot& b) { return a.id < b.id; };
  std::sort(lib.listed.begin(), lib.listed.end(), by_id);
  std::sort(lib.line_only.begin(), lib.line_only.end(), by_id);
  std::sort(lib.retired.begin(), lib.retired.end(), by_id);
  if (find_unlisted)
    for (const auto& r : kBand)
      for (int n = r[0]; n <= r[1]; ++n)
        if (!named.count(n) && !FileFor(titantron_dir, n).empty()) lib.file_only.push_back(n);
  return lib;
}

// The lowest band number with no line in `lines` (and not in `avoid`: files with no line, which a new video must not overwrite). A retired
// line counts as a line; only when no never-used number is left is the lowest RETIRED number (not in `avoid`) given (else ADD would end
// for good after 116 removals). -1 when all 116 hold a live line or a video.
inline int NextFreeId(const std::vector<Slot>& lines, const std::vector<int>& avoid = {}) {
  std::set<int> used(avoid.begin(), avoid.end()), retired;
  for (const Slot& s : lines) {
    used.insert(s.id);
    if (s.retired) retired.insert(s.id);
  }
  for (const auto& r : kBand)
    for (int n = r[0]; n <= r[1]; ++n)
      if (!used.count(n)) return n;
  const std::set<int> avoided(avoid.begin(), avoid.end());
  for (int n : retired)  // (sorted)
    if (!avoided.count(n)) return n;
  return -1;
}

// Writes custom.txt (every line of `lines`, sorted by id; a number outside the band, a duplicate or a nameless slot is refused: false).
// Written beside and renamed, so a reader never sees half a file.
inline bool WriteSlots(const std::filesystem::path& titantron_dir, const std::vector<Slot>& lines) {
  namespace fs = std::filesystem;
  std::set<int> seen;
  std::vector<Slot> v = lines;
  std::sort(v.begin(), v.end(), [](const Slot& a, const Slot& b) { return a.id < b.id; });
  std::string text =
      "# WWE 2K14 recomp: custom titantrons. Tron number <TAB> shown name. The video is <number>.<ext> in this folder.\n"
      "# A line keeps its number for good (a wrestler may hold it), also when the video is gone. Written by the launcher.\n"
      "# \"#retired<TAB>number<TAB>name\" = a removed tron: its number goes to a new video only when no other number is left.\n";
  for (const Slot& s : v) {
    if (!IsSlotId(s.id) || !seen.insert(s.id).second) return false;
    const std::string name = CleanName(s.name);
    if (name.empty()) return false;
    text += (s.retired ? std::string(kRetiredTag) + "\t" : std::string()) + std::to_string(s.id) + "\t" + name + "\n";
  }
  std::error_code ec;
  fs::create_directories(titantron_dir, ec);
  const fs::path part = titantron_dir / (std::string(kFileName) + ".part"), dst = titantron_dir / kFileName;
  {
    std::ofstream o(part, std::ios::binary | std::ios::trunc);
    o << text;
    if (!o.flush()) return false;
  }
  fs::rename(part, dst, ec);  // (replaces on Windows with std::filesystem; a failure leaves the old file)
  if (ec) {
    fs::remove(part, ec);
    return false;
  }
  return true;
}

}  // namespace wwe2k14::tron_slots_core
