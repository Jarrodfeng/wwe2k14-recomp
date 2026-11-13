// wwe2k14 phone launcher - the MUSIC part of the MODS page, the file rules (lane 286, 2026-11-13).
//
// The same files and rules as the PC launcher's MUSIC tab (src\launcher\launcher_core.h: NewSong, ReadNewSongs, AssignMusic ...,
// which is Windows-only) and the game (src\music_slots.h, src\entrance_music.h), but with no Windows in it, so the phone launcher
// and a host test can use it. KEEP IN STEP with those three: the song ids (6001 up, slots.txt), the shown names, the extensions.
//
//   <mods>/music/new/*.ext        new songs (extra entries at the end of the game's MUSIC list)
//   <mods>/music/new/slots.txt    "6001<TAB>Kingdom.mp3[<TAB>SHOWN NAME]": each file keeps its id
//   <mods>/music/<id>.ext         a wrestler's own theme (the wrestler's character id)
//   <mods>/music/.cache/          the importer's converted copies + index.txt (src\audio_import, lane 283; the launcher converts, the game looks up)
//   <mods>/music/library/         the song library (src\launcher_mobile\music_library.h, lane 297)
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace mlauncher::music {

namespace fs = std::filesystem;

inline constexpr const char* kExts[] = {".mp3", ".m4a", ".wav", ".flac", ".wma", ".aac", ".ogg", ".opus", ".oga", ".mp4"};  // (= audio_import::kExts)
inline constexpr int kFirstSongId = 6001, kLastSongId = 9000;

inline std::string U8s(const fs::path& p) {
  const std::u8string s = p.u8string();
  return std::string(s.begin(), s.end());
}
inline std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}
inline fs::path MusicDir(const fs::path& mods) { return mods / "music"; }
inline fs::path NewSongsDir(const fs::path& mods) { return mods / "music" / "new"; }

inline bool IsSongFile(const fs::path& f) {
  const std::string ext = Lower(U8s(f.extension()));
  return std::find(std::begin(kExts), std::end(kExts), ext) != std::end(kExts);
}

struct NewSong {
  int id = 0;
  std::string file, name;  // file name in the folder, name shown in the game
  bool present = false;
};

inline std::string SongCleanName(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '_') c = ' ';
    if (c < 32 || c > 126) continue;
    out += char(std::toupper(static_cast<unsigned char>(c)));
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  if (out.size() > 40) out.resize(40);
  return out.empty() ? "NEW SONG" : out;
}
inline std::string SongFileName(const std::string& file) { return SongCleanName(U8s(fs::u8path(file).stem())); }

inline bool WriteSongSlots(const fs::path& mods, const std::vector<NewSong>& songs) {
  std::vector<NewSong> v = songs;
  std::sort(v.begin(), v.end(), [](const NewSong& a, const NewSong& b) { return a.id < b.id; });
  std::ofstream out(NewSongsDir(mods) / "slots.txt", std::ios::trunc);
  out << "# WWE 2K14 recomp: new entrance-music slots. Music id <TAB> file name [<TAB> shown name]. Written by the\n"
         "# game when it finds a new song in this folder. A song keeps its id, so wrestlers keep the songs you gave\n"
         "# them; a line whose file is gone keeps its id. Edit the shown name in the third column if you like.\n";
  for (const auto& s : v) {
    if (s.id <= 0) continue;
    out << s.id << '\t' << s.file;
    if (s.name != SongFileName(s.file)) out << '\t' << s.name;
    out << '\n';
  }
  return bool(out);
}

// slots.txt and the folder, ids given to new files (slots.txt rewritten then). Sorted by id.
inline std::vector<NewSong> ReadNewSongs(const fs::path& mods) {
  const fs::path dir = NewSongsDir(mods);
  std::vector<NewSong> v;
  std::map<std::string, size_t> by_file;
  std::set<int> ids;
  {
    std::ifstream in(dir / "slots.txt");
    std::string line;
    while (std::getline(in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      std::vector<std::string> cols;
      std::stringstream ss(line);
      std::string col;
      while (std::getline(ss, col, '\t')) {
        while (!col.empty() && col.back() == ' ') col.pop_back();
        while (!col.empty() && col.front() == ' ') col.erase(col.begin());
        cols.push_back(col);
      }
      if (cols.size() < 2 || cols[1].empty()) continue;
      const int id = std::atoi(cols[0].c_str());
      if (id < kFirstSongId || id > kLastSongId || ids.count(id) || by_file.count(Lower(cols[1]))) continue;
      NewSong s;
      s.id = id, s.file = cols[1];
      s.name = cols.size() >= 3 && !cols[2].empty() ? SongCleanName(cols[2]) : SongFileName(cols[1]);
      ids.insert(id);
      by_file[Lower(s.file)] = v.size();
      v.push_back(s);
    }
  }
  std::vector<std::string> files;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_regular_file(ec) && IsSongFile(e.path())) files.push_back(U8s(e.path().filename()));
  std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
  bool added = false;
  for (const auto& f : files) {
    auto it = by_file.find(Lower(f));
    if (it != by_file.end()) {
      v[it->second].present = true;
      continue;
    }
    int id = kFirstSongId;
    while (ids.count(id) && id <= kLastSongId) ++id;
    if (id > kLastSongId) break;
    NewSong s;
    s.id = id, s.file = f, s.name = SongFileName(f), s.present = true;
    ids.insert(id);
    by_file[Lower(f)] = v.size();
    v.push_back(s);
    added = true;
  }
  if (added) WriteSongSlots(mods, v);
  std::sort(v.begin(), v.end(), [](const NewSong& a, const NewSong& b) { return a.id < b.id; });
  return v;
}

// The shown name (slots.txt's third column; empty = the file name again).
inline std::string RenameNewSong(const fs::path& mods, int id, const std::string& name) {
  std::vector<NewSong> v = ReadNewSongs(mods);
  for (auto& s : v)
    if (s.id == id) {
      s.name = name.empty() ? SongFileName(s.file) : SongCleanName(name);
      return WriteSongSlots(mods, v) ? "" : "could not write slots.txt";
    }
  return "no song with id " + std::to_string(id);
}

// Deletes the file (its converted copy in .cache stays: it is shared by content); its line (and id) stays, so a wrestler who had it plays nothing rather than another song.
inline std::string RemoveNewSong(const fs::path& mods, const std::string& file) {
  std::error_code ec;
  if (file.empty() || file.find_first_of("\\/") != std::string::npos) return "bad file name";
  if (!fs::remove(NewSongsDir(mods) / fs::u8path(file), ec)) return ec ? "remove failed: " + ec.message() : "file not found";
  return "";
}

// ---- wrestlers' themes: <mods>/music/<id>.ext
inline fs::path ThemeFor(const fs::path& mods, int id) {
  std::error_code ec;
  for (const char* ext : kExts) {
    const fs::path f = MusicDir(mods) / (std::to_string(id) + ext);
    if (fs::is_regular_file(f, ec)) return f;
  }
  return {};
}
inline bool HasAnyTheme(const fs::path& mods) {
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(MusicDir(mods), ec)) {
    if (!e.is_regular_file(ec)) continue;
    const std::string stem = U8s(e.path().stem());
    if (!stem.empty() && stem.find_first_not_of("0123456789") == std::string::npos && IsSongFile(e.path())) return true;
  }
  return false;
}
inline void RevertTheme(const fs::path& mods, int id) {
  std::error_code ec;
  for (const char* e : kExts) fs::remove(MusicDir(mods) / (std::to_string(id) + e), ec);
}
// A wrestler's theme from a file already on the phone (a NEW SONG, or a file the picker copied): a copy named <id>.ext (the
// previous theme of that id, in any extension, goes). The copy is written whole to a .part file first.
inline std::string AssignTheme(const fs::path& mods, int id, const fs::path& src) {
  std::error_code ec;
  if (id < 0) return "no wrestler";
  if (!fs::is_regular_file(src, ec)) return "file not found";
  if (!IsSongFile(src)) return "not a song file";
  const std::string ext = Lower(U8s(src.extension()));
  const fs::path dir = MusicDir(mods);
  fs::create_directories(dir, ec);
  const fs::path dst = dir / (std::to_string(id) + ext), tmp = dir / (std::to_string(id) + ext + ".part");
  fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
  if (ec) return "copy failed: " + ec.message();
  RevertTheme(mods, id);
  fs::rename(tmp, dst, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return "rename failed: " + ec.message();
  }
  return "";
}

inline int CountPresent(const std::vector<NewSong>& v) {
  return int(std::count_if(v.begin(), v.end(), [](const NewSong& s) { return s.present; }));
}

}  // namespace mlauncher::music
