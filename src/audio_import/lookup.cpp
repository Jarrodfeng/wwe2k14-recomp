// wwe2k14 - lane 283: the cache lookups of the song importer (see audio_import.h). Stat + small text files only: NO decoder, NO Import,
// no thread. This is the only part of the importer the GAME links; the converter (audio_import.cpp) lives in the launcher.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "internal.h"

namespace audio_import {
namespace fs = std::filesystem;

namespace {
Counters g_counters;
}
Counters& Stats() { return g_counters; }

namespace detail {

std::string U8(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
// The key form of a path: on Windows file names are case-insensitive (343.MP3 and 343.mp3 are one file), so A-Z are folded.
static std::string KeyPath(const fs::path& p) {
  std::string s = U8(p);
#if defined(_WIN32)
  for (auto& c : s) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
#endif
  return s;
}

bool ReadAll(const fs::path& p, std::vector<uint8_t>& out, std::string* err) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff n = in.tellg();
  if (n < 0) return false;
  if (uintmax_t(n) > kMaxSourceBytes) {
    if (err) *err = "the file is larger than 1 GB";
    return false;
  }
  in.seekg(0);
  out.resize(size_t(n));
  if (n && !in.read(reinterpret_cast<char*>(out.data()), n)) return false;
  return true;
}

uint64_t Fnv1a64(const std::vector<uint8_t>& b) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint8_t c : b) h = (h ^ c) * 0x100000001b3ull;
  return h;
}

static std::string Hex16(uint64_t v) {
  char t[24];
  std::snprintf(t, sizeof t, "%016llx", static_cast<unsigned long long>(v));
  return t;
}

std::string KeyOf(uint64_t content, double target) {
  char t[48];
  std::snprintf(t, sizeof t, "_t%d_v%d", int(std::lround(target * 10.0)), kCacheVersion);
  std::string s = Hex16(content);
  s += t;
  for (auto& c : s) {
    if (c == '-') c = 'm';
  }
  return s;
}

static void FillFromSidecar(const fs::path& txt, Imported& r) {
  std::ifstream in(txt, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    const double d = std::atof(v.c_str());
    if (k == "src_lufs") r.src_lufs = d;
    else if (k == "src_true_peak_db") r.src_true_peak_db = d;
    else if (k == "gain_db") r.gain_db = d;
    else if (k == "out_lufs") r.out_lufs = d;
    else if (k == "out_true_peak_db") r.out_true_peak_db = d;
    else if (k == "limited") r.limited = d != 0;
    else if (k == "seconds") r.seconds = d;
    else if (k == "rate") r.rate = int(d);
    else if (k == "title") r.title = v;
    else if (k == "artist") r.artist = v;
    else if (k == "album") r.album = v;
  }
}

Imported Hit(const fs::path& cache_dir, const std::string& key) {
  Imported r;
  const fs::path wav = cache_dir / (key + ".wav"), txt = cache_dir / (key + ".txt");
  std::error_code ec;
  if (fs::is_regular_file(wav, ec) && fs::is_regular_file(txt, ec) && fs::file_size(wav, ec) > 44) {
    r.ok = true, r.from_cache = true, r.cache = wav, r.key = key;
    FillFromSidecar(txt, r);
    const fs::path png = cache_dir / (key + ".png");
    if (fs::is_regular_file(png, ec)) r.thumb = png;
  }
  return r;
}

// ---- the memo ----
namespace {
struct Memo {
  uintmax_t size = 0;
  fs::file_time_type mtime{};
  Imported r;
  std::chrono::steady_clock::time_point retry_after{};  // set for an I/O failure: it is not tried again before this
};
std::mutex g_memo_mutex;
std::map<std::string, Memo>& MemoMap() {
  static auto* m = new std::map<std::string, Memo>;
  return *m;
}
}  // namespace

std::string MemoKey(const fs::path& src, const fs::path& cache_dir, double target) {
  return KeyPath(src) + "|" + KeyPath(cache_dir) + "|" + std::to_string(int(std::lround(target * 10)));
}
bool MemoGet(const std::string& k, uintmax_t size, fs::file_time_type mtime, Imported& out) {
  std::error_code ec;
  std::lock_guard lock(g_memo_mutex);
  auto it = MemoMap().find(k);
  if (it == MemoMap().end() || it->second.size != size || it->second.mtime != mtime) return false;
  if (it->second.r.io_failed && std::chrono::steady_clock::now() >= it->second.retry_after) return false;  // time to try again
  if (it->second.r.ok && it->second.r.normalised && !fs::exists(it->second.r.cache, ec)) return false;
  out = it->second.r;
  return true;
}
void MemoPut(const std::string& k, uintmax_t size, fs::file_time_type mtime, const Imported& r) {
  std::lock_guard lock(g_memo_mutex);
  Memo m{size, mtime, r, {}};
  if (r.io_failed) m.retry_after = std::chrono::steady_clock::now() + std::chrono::seconds(60);  // not re-converted in a loop
  MemoMap()[k] = std::move(m);
}

// ---- <cache>\index.txt ----
// Keys are PATHS RELATIVE TO THE MUSIC FOLDER (the cache folder's parent: "343.mp3", "new\x.mp3", "library\y.mp3", folded to lower case
// on Windows), so the game (absolute mods path) and the launcher (the path as it stored it) agree; a file outside the music folder
// has no key (it is never indexed). The first line carries the cache version: a different version = every entry is stale.
namespace {
struct IndexEntry {
  uintmax_t size = 0;
  long long mtime = 0;
  int target10 = 0;
  std::string status, value;  // ok: key / plain: note / refused: err
  std::string rel;            // the path relative to the music folder, in its ORIGINAL case (the map key is folded)
};
std::mutex g_index_mutex;
struct IndexCache {
  fs::path dir;
  uintmax_t stamp_size = 0;
  long long stamp_mtime = 0;
  bool loaded = false, unreadable = false;
  std::map<std::string, IndexEntry> map;  // "relpath|target10"
};
IndexCache& IC() {
  static auto* c = new IndexCache;
  return *c;
}
constexpr uintmax_t kMaxIndexBytes = 8u << 20;  // an index is a few hundred lines; anything bigger is not ours
std::string Esc(std::string s) {
  for (auto& c : s) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return s;
}
long long Ticks(fs::file_time_type t) { return (long long)t.time_since_epoch().count(); }
std::string HeaderLine() { return "# wwe2k14 song cache index v" + std::to_string(kCacheVersion); }

std::string Fold(std::string k) {
#if defined(_WIN32)
  for (auto& c : k) c = char(c >= 'A' && c <= 'Z' ? c + 32 : c);
#else
  // Lane 297b: elsewhere (the phone) names are case-sensitive, but the EXTENSION alone is folded: a hand-copied 343.MP3 must key like
  // 343.mp3 (the importer and the game both look files up by this key, and an extension is never what tells two songs apart).
  const size_t dot = k.rfind('.');
  if (dot != std::string::npos && k.find('/', dot) == std::string::npos)
    for (size_t i = dot; i < k.size(); ++i) k[i] = char(k[i] >= 'A' && k[i] <= 'Z' ? k[i] + 32 : k[i]);
#endif
  return k;
}
// "343.mp3" / "new/x.mp3" for a file under the music folder (original case in *orig); "" for any other path. Returns the folded map key.
std::string RelKey(const fs::path& cache_dir, const fs::path& src, int target10, std::string* orig = nullptr) {
  std::error_code ec;
  const fs::path root = fs::absolute(cache_dir.parent_path(), ec).lexically_normal();
  const fs::path file = fs::absolute(src, ec).lexically_normal();
  const fs::path rel = file.lexically_relative(root);
  if (rel.empty() || *rel.begin() == "..") return {};
  const auto gu = rel.generic_u8string();
  std::string k(gu.begin(), gu.end());
  if (orig) *orig = k;
  return Fold(k) + "|" + std::to_string(target10);
}

// Loads index.txt when it changed on disk (a stat per call; compared by size + mtime, FAT32 / exFAT timestamps are coarse).
void IndexLoadLocked(const fs::path& dir) {
  IndexCache& c = IC();
  std::error_code ec1, ec2;
  const fs::path file = dir / "index.txt";
  const uintmax_t size = fs::file_size(file, ec1);
  const long long mt = ec1 ? 0 : Ticks(fs::last_write_time(file, ec2));
  if (ec1 || ec2) {
    c.dir = dir, c.loaded = true, c.unreadable = false, c.map.clear(), c.stamp_size = 0, c.stamp_mtime = 0;
    return;
  }
  if (c.loaded && c.dir == dir && c.stamp_size == size && c.stamp_mtime == mt) return;
  c.dir = dir, c.stamp_size = size, c.stamp_mtime = mt, c.loaded = true, c.unreadable = false, c.map.clear();
  if (size > kMaxIndexBytes) return;
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    c.unreadable = true;  // exists but cannot be opened right now: the writer must not replace it with one entry
    c.loaded = false;     // ... and the next call tries the open again (the stamp alone must not pin the failure)
    return;
  }
  std::string line;
  bool first = true;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first) {
      first = false;
      if (line != HeaderLine()) {
        c.map.clear();  // another cache version (or not our file): everything is "not converted yet"
        return;
      }
      continue;
    }
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> col;
    size_t at = 0;
    for (int i = 0; i < 5; ++i) {  // size, mtime, target10, status, value, then the relative path (the rest of the line)
      const size_t t = line.find('\t', at);
      if (t == std::string::npos) break;
      col.push_back(line.substr(at, t - at));
      at = t + 1;
    }
    if (col.size() != 5) continue;
    IndexEntry e;
    e.size = (uintmax_t)std::strtoull(col[0].c_str(), nullptr, 10);
    e.mtime = std::strtoll(col[1].c_str(), nullptr, 10);
    e.target10 = std::atoi(col[2].c_str());
    e.status = col[3], e.value = col[4];
    e.rel = line.substr(at);
    c.map[Fold(e.rel) + "|" + col[2]] = e;
  }
}
}  // namespace

void IndexRecord(const fs::path& cache_dir, const fs::path& src, uintmax_t size, fs::file_time_type mtime, double target, const Imported& r) {
  try {
    const int t10 = int(std::lround(target * 10));
    std::string orig;
    const std::string key = RelKey(cache_dir, src, t10, &orig);
    if (key.empty()) return;  // outside the music folder (a file picked in Downloads): the game never reads it, nothing to index
    std::lock_guard lock(g_index_mutex);
    IndexLoadLocked(cache_dir);
    IndexCache& c = IC();
    if (c.unreadable) return;  // never rewrite an index we could not read with one entry
    IndexEntry e;
    e.size = size, e.mtime = Ticks(mtime), e.target10 = t10, e.rel = orig;
    if (r.ok && r.normalised) e.status = "ok", e.value = r.key;
    else if (r.ok) e.status = "plain", e.value = Esc(r.note);
    else e.status = "refused", e.value = Esc(r.err);
    c.map[key] = e;
    std::error_code ec;
    fs::create_directories(cache_dir, ec);
    const fs::path tmp = cache_dir / "index.txt.tmp", file = cache_dir / "index.txt";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out << HeaderLine() << "\n# size, mtime, target x10, status, key / note / reason, path relative to the music folder\n";
      for (const auto& [k, v] : c.map) {
        (void)k;
        out << v.size << '\t' << v.mtime << '\t' << v.target10 << '\t' << v.status << '\t' << v.value << '\t' << v.rel << '\n';
      }
      out.close();
      if (!out) return;
    }
    for (int attempt = 0; attempt < 6; ++attempt) {  // the game may have the file open for a moment (it does not share delete)
      fs::rename(tmp, file, ec);
      if (!ec) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    if (ec) {
      fs::remove(tmp, ec);
      return;
    }
    std::error_code e1, e2;
    c.stamp_size = fs::file_size(file, e1);
    c.stamp_mtime = Ticks(fs::last_write_time(file, e2));
  } catch (...) {
  }
}

// Drops the entries of songs that are gone (once per conversion pass, not per entry written). The path is checked in its original case.
void IndexPrune(const fs::path& cache_dir) {
  try {
    std::lock_guard lock(g_index_mutex);
    IndexLoadLocked(cache_dir);
    IndexCache& c = IC();
    if (c.unreadable) return;
    const fs::path root = cache_dir.parent_path();
    std::error_code ec;
    bool changed = false;
    for (auto it = c.map.begin(); it != c.map.end();) {
      if (!fs::exists(root / fs::path(std::u8string(it->second.rel.begin(), it->second.rel.end())), ec)) it = c.map.erase(it), changed = true;
      else ++it;
    }
    if (!changed) return;
    const fs::path tmp = cache_dir / "index.txt.tmp", file = cache_dir / "index.txt";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      out << HeaderLine() << "\n# size, mtime, target x10, status, key / note / reason, path relative to the music folder\n";
      for (const auto& [k, v] : c.map) {
        (void)k;
        out << v.size << '\t' << v.mtime << '\t' << v.target10 << '\t' << v.status << '\t' << v.value << '\t' << v.rel << '\n';
      }
      out.close();
      if (!out) return;
    }
    for (int attempt = 0; attempt < 6; ++attempt) {
      fs::rename(tmp, file, ec);
      if (!ec) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    if (ec) fs::remove(tmp, ec);
    else {
      std::error_code e1, e2;
      c.stamp_size = fs::file_size(file, e1);
      c.stamp_mtime = Ticks(fs::last_write_time(file, e2));
    }
  } catch (...) {
  }
}

static bool IndexFind(const fs::path& cache_dir, const fs::path& src, uintmax_t size, fs::file_time_type mtime, double target, IndexEntry& out) {
  const std::string key = RelKey(cache_dir, src, int(std::lround(target * 10)));
  if (key.empty()) return false;
  std::lock_guard lock(g_index_mutex);
  IndexLoadLocked(cache_dir);
  auto it = IC().map.find(key);
  if (it == IC().map.end() || it->second.size != size || it->second.mtime != Ticks(mtime)) return false;
  out = it->second;
  return true;
}

Song SongFrom(const Imported& r, const fs::path& src) {
  Song s;
  s.ok = r.ok, s.err = r.err, s.audio = r.cache, s.thumb = r.thumb, s.normalised = r.normalised, s.note = r.note;
  s.title = r.title.empty() ? U8(src.stem()) : r.title;
  s.artist = r.artist, s.album = r.album, s.seconds = r.seconds, s.src_lufs = r.src_lufs, s.gain_db = r.gain_db;
  s.out_lufs = r.out_lufs, s.out_true_peak_db = r.out_true_peak_db, s.limited = r.limited;
  return s;
}

}  // namespace detail

using namespace detail;

Imported Lookup(const fs::path& src, const fs::path& cache_dir, double target) {
  Imported r;
  try {
    std::error_code ec1, ec2;
    const uintmax_t size = fs::file_size(src, ec1);
    const auto mtime = fs::last_write_time(src, ec2);
    if (ec1 || ec2) {
      r.err = "the file could not be read";
      return r;
    }
    const std::string k = MemoKey(src, cache_dir, target);
    if (MemoGet(k, size, mtime, r)) return r;
    r = Imported{};
    IndexEntry e;
    if (!IndexFind(cache_dir, src, size, mtime, target, e)) return r;  // not converted yet: !ok, empty err
    if (e.status == "ok") {
      Imported h = Hit(cache_dir, e.value);
      if (!h.ok) return r;  // the entry was deleted: not converted
      r = h;
    } else if (e.status == "plain") {
      r.ok = true, r.normalised = false, r.cache = src, r.note = e.value;
    } else {
      r.err = e.value.empty() ? "this file cannot be turned into audio" : e.value;
    }
    MemoPut(k, size, mtime, r);
  } catch (...) {
    r = Imported{};
  }
  return r;
}

fs::path PlayableNow(const fs::path& src, const fs::path& cache_dir, double target, Imported* info) {
  const Imported r = Lookup(src, cache_dir, target);
  if (info) *info = r;
  if (r.ok) return r.cache;                // the normalised copy, or (plain) the source itself
  if (!r.err.empty()) return fs::path();   // refused: the game's own theme stays
  return src;                              // not converted yet: it plays as it is
}

std::string ContentKey(const fs::path& src, double target) {
  std::vector<uint8_t> b;
  if (!ReadAll(src, b) || b.empty()) return {};
  return KeyOf(Fnv1a64(b), target);
}

Imported Peek(const fs::path& src, const fs::path& cache_dir, double target) {
  const std::string key = ContentKey(src, target);
  if (key.empty()) return {};
  return Hit(cache_dir, key);
}

Song LibraryCached(const fs::path& src, const fs::path& cache_dir, double target) {
  const Imported r = Lookup(src, cache_dir, target);
  Song s = SongFrom(r, src);
  if (!r.ok && r.err.empty()) s.err = "";
  return s;
}

}  // namespace audio_import
