// Session 98 (plan step 14, the mod workbench): how the Arena Viewer finds the player's mods, EXACTLY as the game does.
// Portable C++ (std only; no Win32): the few platform things (decode one video frame, a file picker, the Recycle Bin,
// the cache folder) come in through `Platform`, filled by ui\mods_win32.cpp.
//
// The rules mirrored (sources, so nothing is guessed):
//   tron file     <mods>\titantron\NNN.<ext> (NNN at most 4 digits: mods.h ParseTitantronPath), ext tried in this order:
//                 .bik2 .mp4 .mov .mkv .alpha.mp4 .alpha.mov .alpha.mkv (src\mods.h FindTron + kTronExts; same as the launcher)
//   banner        <mods>\titantron\NNN.banner.txt (original | video | none | a file name, relative = titantron\), else
//                 NNN.banner.mp4 / .mov / .mkv, else original (src\tron_shim.h ReadBanner)
//   sheet         only when the original tron is a 320x320 sheet (src\tron_shim.h:624; --wwe2k14_tron_sheet assumed on, its default):
//                 the video on rows 0-223 (the big screen), four 24-row banner strips below: original strips = the game's own,
//                 video = a centred band, file = the banner video (cut in four when wider than 26.7:1), none = black.
//                 Otherwise the video fills the whole frame. A Bink 2 mod is played by the game's own decoder: the viewer has
//                 none, so it shows a black sheet.
//   assign        copy to <mods>\titantron\NNN.<ext> via NNN.<ext>.part, remove the tron's other replacements, rename
//                 (src\launcher\launcher_core.h AssignTron: mirrored here, the launcher's header is Win32 + json)
//   Attire textures / signage: the game reads NO mod files for them (src\mods.h and its users read only titantron\, music\,
//   lighting\, logos\import.png), so the viewer lists them as "not supported by the game yet".
// The mods are additive (CLAUDE.md): everything is read from the mods folder, only while the toggle is on; the game's files and the
// launcher's caches are never written (a mods folder that equals / contains / sits inside the game data or the viewer's own cache
// is refused). The viewer's own frame cache lives under %LOCALAPPDATA%\wwe2k14-asset-viewer: one small scaled frame per mod.
// Threading: the decode runs on a worker (StartDecode); the registries are mutex-guarded.
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace assets {
namespace mods_lookup {

namespace fs = std::filesystem;

inline constexpr const char* kTronExts[] = {".bik2", ".mp4", ".mov", ".mkv", ".alpha.mp4", ".alpha.mov", ".alpha.mkv"};
inline constexpr int kMaxTron = 9999;  // the game parses at most 4 digits

inline std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}
inline std::string U8(const fs::path& p) {  // never throws on a non-ANSI name
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
inline std::string Name(const fs::path& p) { return U8(p.filename()); }

struct Config {
  fs::path dir;        // the mods folder ("" = none)
  bool on = false;     // USE MY MODS IN THE PREVIEW
  bool async = false;  // windowed viewer: decode on a worker, never block the UI
  fs::path game_root;  // --game (never written to)
  std::string refused;  // why the last SetDir was refused
};
inline Config& Cfg() {
  static Config c;
  return c;
}

inline std::string NormKey(const fs::path& p) {
  std::error_code ec;
  fs::path c = fs::weakly_canonical(p, ec);
  if (ec) c = p;
  std::string s = Lower(U8(c.lexically_normal()));
  while (s.size() > 1 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
  return s;
}
inline bool SameOrInside(const std::string& a, const std::string& b) {  // a is b, or inside b
  return !b.empty() && (a == b || (a.size() > b.size() && a.compare(0, b.size(), b) == 0 && (a[b.size()] == '\\' || a[b.size()] == '/')));
}


// --mods <folder> / {"cmd":"mods","dir":..}: the toggle defaults to on when the folder exists. Returns "" or why it was refused:
// a mods folder that equals, contains or is inside the game data root (or the viewer's own cache) could get game files removed.
inline std::string SetDir(const fs::path& d);

// ---------------------------------------------------------------- platform seam
struct Frame {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // straight RGBA8, alpha 255
  bool ok() const { return w > 0 && h > 0 && rgba.size() == size_t(w) * h * 4; }
};
struct Platform {
  std::function<bool(const fs::path&, double at_seconds, Frame&, std::string&)> decode;  // one video frame
  std::function<std::string()> pick_video;                                             // a file picker: UTF-8 path or ""
  std::function<std::string()> pick_character;                                         // Session 98: a .pac / .zip picker: UTF-8 path or ""
  // Session 98 (Superstar Importer): several .pac / .zip at once (bulk import; empty = cancelled) and a PNG / JPG picker.
  std::function<std::vector<std::string>()> pick_characters;
  std::function<std::string()> pick_image;
  std::function<bool(const fs::path&, std::string&)> recycle;                            // to the Recycle Bin
  std::function<void(const fs::path&)> open_folder;
  std::string recycle_name = "the Recycle Bin";  // where `recycle` puts a file, for the UI (the phone: the app's removed folder)
  std::string pick_error;                        // why the last pick gave "" when it was not a plain cancel (the phone's copy failed)
  fs::path cache_dir;  // the viewer's own frame cache (never the mods folder, never the launcher's cache)
};
inline Platform& Plat() {
  static Platform p;
  return p;
}

inline std::string SetDir(const fs::path& d) {
  Cfg().refused.clear();
  if (!d.empty()) {
    const std::string m = NormKey(d);
    const std::string g = NormKey(Cfg().game_root);
    const std::string c = Plat().cache_dir.empty() ? std::string() : NormKey(Plat().cache_dir.parent_path());
    std::string why;
    if (!g.empty() && (SameOrInside(m, g) || SameOrInside(g, m))) why = "the mods folder must not be, contain or sit inside the game files";
    else if (!c.empty() && (SameOrInside(m, c) || SameOrInside(c, m))) why = "the mods folder must not be, contain or sit inside the viewer's cache";
    if (!why.empty()) {
      Cfg().dir.clear(), Cfg().on = false, Cfg().refused = why;
      return why;
    }
  }
  std::error_code ec;
  Cfg().dir = d;
  Cfg().on = !d.empty() && fs::is_directory(d, ec);
  return "";
}
inline fs::path TronDir() { return Cfg().dir.empty() ? fs::path() : Cfg().dir / "titantron"; }
// <mods>\lighting as UTF-8 ("" without a mods folder): the lighting director's default save folder.
inline std::string LightingDir() { return Cfg().dir.empty() ? std::string() : U8(Cfg().dir / "lighting"); }

// ---------------------------------------------------------------- file lookup (cached index: no stats per frame)
inline std::string FileSig(const fs::path& p) {  // size + modification time
  std::error_code ec;
  const auto sz = fs::file_size(p, ec);
  const auto tm = fs::last_write_time(p, ec).time_since_epoch().count();
  return std::to_string(ec ? 0 : sz) + "_" + std::to_string(static_cast<long long>(tm));
}

inline int ExtRank(const std::string& ext) {
  for (int i = 0; i < int(sizeof kTronExts / sizeof kTronExts[0]); ++i)
    if (ext == kTronExts[i]) return i;
  return -1;
}
inline std::mutex& IdxMu() {
  static std::mutex m;
  return m;
}
inline std::map<int, fs::path>& Index() {
  static std::map<int, fs::path> m;
  return m;
}
// Re-reads <mods>\titantron once (called by SetDir users, the 1 s poll and after ADD / REMOVE): the winning replacement per tron, in the game's order.
inline void Reindex() {
  std::map<int, std::pair<int, fs::path>> best;
  std::error_code ec;
  if (!Cfg().dir.empty()) {
    for (fs::directory_iterator it(TronDir(), ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code e2;
      if (!it->is_regular_file(e2)) continue;
      const std::string name = Name(it->path());
      const size_t dot = name.find('.');
      if (dot == std::string::npos || dot == 0 || dot > 4) continue;  // 1-4 digits
      const std::string stem = name.substr(0, dot);
      if (!std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
      const int rank = ExtRank(Lower(name.substr(dot)));
      if (rank < 0) continue;
      const int id = std::atoi(stem.c_str());
      auto b = best.find(id);
      if (b == best.end() || rank < b->second.first) best[id] = {rank, it->path()};
    }
  }
  std::lock_guard<std::mutex> lk(IdxMu());
  Index().clear();
  for (auto& kv : best) Index()[kv.first] = kv.second.second;
}
// The replacement for tron `id` in the mods folder (the game's order); ignores the toggle (the panel lists it either way).
inline fs::path TronFile(int id) {
  std::lock_guard<std::mutex> lk(IdxMu());
  auto it = Index().find(id);
  return it == Index().end() ? fs::path() : it->second;
}
// What the game would load: the replacement only while the toggle is on.
inline fs::path TronFor(int id) { return Cfg().on ? TronFile(id) : fs::path(); }

inline bool IsStackedAlpha(const fs::path& p) { return Lower(Name(p)).find(".alpha.") != std::string::npos; }
inline bool IsBink(const fs::path& p) { return Lower(U8(p.extension())) == ".bik2"; }

enum class BannerMode { kOriginal, kVideo, kNone, kFile };
inline const char* BannerName(BannerMode m) {
  switch (m) {
    case BannerMode::kOriginal: return "original";
    case BannerMode::kVideo: return "video";
    case BannerMode::kNone: return "none";
    case BannerMode::kFile: return "file";
  }
  return "?";
}
struct Banner {
  BannerMode mode = BannerMode::kOriginal;
  fs::path file;
  bool explicit_choice = false;  // a .txt or a banner video exists in the folder
};
inline Banner ReadBanner(int id) {
  Banner b;
  const fs::path dir = TronDir();
  if (dir.empty()) return b;
  const std::string stem = std::to_string(id) + ".banner";
  std::error_code ec;
  if (std::ifstream f(dir / fs::u8path(stem + ".txt"), std::ios::binary); f) {
    char buf[520] = {};
    f.read(buf, sizeof(buf) - 1);
    std::string t(buf, size_t(f.gcount()));
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    size_t i = 0;
    while (i < t.size() && std::isspace(static_cast<unsigned char>(t[i]))) ++i;
    t = t.substr(i);
    b.explicit_choice = true;
    const std::string l = Lower(t);
    if (l == "original" || l.empty()) return b;
    if (l == "video") return b.mode = BannerMode::kVideo, b;
    if (l == "none") return b.mode = BannerMode::kNone, b;
    fs::path p = fs::u8path(t);
    if (p.is_relative()) p = dir / p;
    if (fs::is_regular_file(p, ec)) return b.mode = BannerMode::kFile, b.file = p, b;
    return b;  // the named file is missing: the game logs it and keeps the original strips
  }
  for (const char* ext : {".mp4", ".mov", ".mkv"}) {
    const fs::path p = dir / fs::u8path(stem + ext);
    if (fs::is_regular_file(p, ec)) return b.mode = BannerMode::kFile, b.file = p, b.explicit_choice = true, b;
  }
  return b;
}

struct Entry {
  int id = 0;
  fs::path file;  // the winning replacement
  std::string ext;
  bool stacked = false, bink = false;
  uintmax_t bytes = 0;
};
inline std::vector<Entry> ScanTrons() {
  std::vector<std::pair<int, fs::path>> v;
  {
    std::lock_guard<std::mutex> lk(IdxMu());
    for (auto& kv : Index()) v.push_back(kv);
  }
  std::vector<Entry> out;
  std::error_code ec;
  for (auto& kv : v) {
    Entry en;
    en.id = kv.first, en.file = kv.second;
    const std::string name = Name(en.file);
    en.ext = Lower(name.substr(name.find('.')));
    en.stacked = IsStackedAlpha(en.file), en.bink = IsBink(en.file);
    en.bytes = fs::file_size(en.file, ec);
    out.push_back(en);
  }
  return out;
}
// Every tron id that has a banner choice in the folder (NNN.banner.txt / NNN.banner.<video>).
inline std::vector<std::pair<int, Banner>> ScanBanners() {
  std::vector<std::pair<int, Banner>> out;
  std::error_code ec;
  std::vector<int> ids;
  for (fs::directory_iterator it(TronDir(), ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = Name(it->path());
    const size_t dot = name.find(".banner.");
    if (dot == std::string::npos || dot == 0 || dot > 4) continue;
    const std::string stem = name.substr(0, dot);
    if (std::all_of(stem.begin(), stem.end(), [](char c) { return c >= '0' && c <= '9'; })) ids.push_back(std::atoi(stem.c_str()));
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  for (int id : ids) out.push_back({id, ReadBanner(id)});
  return out;
}
// One cheap signature of the whole titantron folder (names, sizes, times) for the 1 s poll.
inline std::string DirSig() {
  std::string s;
  std::error_code ec;
  if (Cfg().dir.empty()) return s;
  for (fs::directory_iterator it(TronDir(), ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code e2;
    if (!it->is_regular_file(e2)) continue;
    s += Name(it->path()) + ":" + std::to_string(it->file_size(e2)) + ":" +
         std::to_string(static_cast<long long>(it->last_write_time(e2).time_since_epoch().count())) + ";";
  }
  return s;
}

// ---------------------------------------------------------------- add / remove
// Mirror of the launcher's AssignTron (launcher_core.h): copies `src` to <mods>\titantron\NNN.<ext>, removes the tron's other
// replacements. Returns "" or the reason. The game's own files are never touched.
inline std::string AssignTron(int tron, const fs::path& src, bool stacked_alpha = false) {
  std::error_code ec;
  if (Cfg().dir.empty()) return "no mods folder";
  if (tron < 0 || tron > kMaxTron) return "tron numbers are 0-9999 (the game reads at most 4 digits)";
  if (!fs::is_regular_file(src, ec)) return "file not found: " + U8(src);
  std::string ext = Lower(U8(src.extension()));
  if (ext != ".bik2" && (stacked_alpha || IsStackedAlpha(src))) ext = ".alpha" + ext;
  if (ExtRank(ext) < 0) return "use a .mp4, .mov, .mkv or .bik2 file";
  const fs::path dir = TronDir();
  fs::create_directories(dir, ec);
  const fs::path dst = dir / fs::u8path(std::to_string(tron) + ext);
  const fs::path tmp = dir / fs::u8path(std::to_string(tron) + ext + ".part");
  ec.clear();
  if (fs::exists(dst, ec) && !ec) {
    std::error_code e2;
    if (fs::equivalent(src, dst, e2) && !e2) return "that file is already the mod";
  }
  fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
  if (ec) return "copy failed: " + ec.message();
  for (const char* e : kTronExts) {
    if (ext == e) continue;
    fs::remove(dir / fs::u8path(std::to_string(tron) + e), ec);
  }
  fs::rename(tmp, dst, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return "rename failed: " + ec.message();
  }
  Reindex();
  return "";
}
// True when `p` is a file inside the mods folder (the only files the panel may remove).
inline bool InsideMods(const fs::path& p) {
  if (Cfg().dir.empty()) return false;
  return SameOrInside(NormKey(p), NormKey(Cfg().dir)) && NormKey(p) != NormKey(Cfg().dir);
}
// Moves the tron's replacement to the Recycle Bin (never deletes for good).
inline std::string RemoveTron(int tron) {
  const fs::path f = TronFile(tron);
  if (f.empty()) return "tron " + std::to_string(tron) + " has no mod";
  if (!InsideMods(f)) return "refusing: not inside the mods folder";
  const std::string g = NormKey(Cfg().game_root);
  if (!g.empty() && SameOrInside(NormKey(f), g)) return "refusing: that is a game file";
  std::string err;
  if (!Plat().recycle) return "no Recycle Bin on this platform";
  if (!Plat().recycle(f, err)) return "could not remove: " + err;
  Reindex();
  return "";
}

// ---------------------------------------------------------------- frames and the sheet
// Decoded frames per tron, valid while the file signature matches. Only SMALL scaled frames are kept (the tron's video at 320x224,
// a banner video at most 1280 wide), in memory and on disk under Plat().cache_dir as <kind><id>_<path hash>_<size>_<time>.rgba
// (u32 w, u32 h, RGBA): the viewer's own folder. A failure is remembered by the same signature (no retry until the file changes).
struct Cached {
  std::string sig, bsig;
  Frame video, banner;
  std::string err;
  bool failed = false;
};
inline std::mutex& RegMu() {
  static std::mutex m;
  return m;
}
inline std::map<int, Cached>& Registry() {
  static std::map<int, Cached> r;
  return r;
}
inline std::atomic<int>& DecodeGen() {  // bumped when a worker finishes: the panel re-stages
  static std::atomic<int> g{0};
  return g;
}

inline Frame Resize(const Frame& src, int sx, int sy, int sw, int sh, int dw, int dh);

inline std::string PathHash(const fs::path& p) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : U8(p)) h = (h ^ c) * 1099511628211ull;
  char b[20];
  std::snprintf(b, sizeof b, "%08x", unsigned(h ^ (h >> 32)));
  return b;
}
// Decodes (or loads from the cache) one video frame of `file`, scaled small. kind 't' = tron video (320x224), 'b' = banner video.
inline bool LoadScaled(const fs::path& file, char kind, int id, bool stacked, Frame& out, std::string& err) {
  const fs::path dir = Plat().cache_dir;
  std::error_code ec;
  const std::string tag = std::string(1, kind) + std::to_string(id) + "_" + PathHash(file) + "_";
  const fs::path cf = dir.empty() ? fs::path() : dir / fs::u8path(tag + FileSig(file) + ".rgba");
  if (!cf.empty()) {
    if (std::ifstream f(cf, std::ios::binary); f) {
      uint32_t hd[2] = {};
      f.read(reinterpret_cast<char*>(hd), 8);
      if (f && hd[0] > 0 && hd[0] <= 1280 && hd[1] > 0 && hd[1] <= 1280) {
        out.w = int(hd[0]), out.h = int(hd[1]);
        out.rgba.resize(size_t(out.w) * out.h * 4);
        f.read(reinterpret_cast<char*>(out.rgba.data()), std::streamsize(out.rgba.size()));
        if (f.gcount() == std::streamsize(out.rgba.size())) return true;
      }
    }
  }
  out = Frame();
  Frame full;
  if (!Plat().decode) return err = "no video decoder on this platform", false;
  if (!Plat().decode(file, 1.0, full, err) || !full.ok()) {
    if (err.empty()) err = "could not decode a frame";
    return false;
  }
  if (kind == 't') {
    const int vh = stacked ? std::max(2, (full.h / 2) & ~1) : full.h;  // stacked alpha: the colour half on top
    out = Resize(full, 0, 0, full.w, vh, 320, 224);
  } else {
    const int w = std::min(full.w, 1280), h = std::max(1, int(int64_t(full.h) * w / full.w));
    out = Resize(full, 0, 0, full.w, full.h, w, h);
  }
  if (!cf.empty()) {
    fs::create_directories(dir, ec);
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))  // one cached frame per file: drop older versions
      if (Name(it->path()).rfind(tag, 0) == 0) fs::remove(it->path(), ec);
    if (std::ofstream f(cf, std::ios::binary); f) {
      const uint32_t hd[2] = {uint32_t(out.w), uint32_t(out.h)};
      f.write(reinterpret_cast<const char*>(hd), 8);
      f.write(reinterpret_cast<const char*>(out.rgba.data()), std::streamsize(out.rgba.size()));
    }
  }
  return true;
}
inline void CurrentSigs(int id, std::string& sig, std::string& bsig) {
  const fs::path f = TronFile(id);
  const Banner b = ReadBanner(id);
  sig = f.empty() ? std::string() : FileSig(f);
  bsig = std::string(BannerName(b.mode)) + (b.file.empty() ? "" : FileSig(b.file));
}
// 0 = nothing decoded for the file as it is now, 1 = ready, 2 = failed (remembered)
inline int StateOf(int id) {
  std::string sig, bsig;
  CurrentSigs(id, sig, bsig);
  std::lock_guard<std::mutex> lk(RegMu());
  auto it = Registry().find(id);
  if (sig.empty() || it == Registry().end() || it->second.sig != sig || it->second.bsig != bsig) return 0;
  return it->second.failed ? 2 : 1;
}
// Decodes (or loads from the cache) tron `id`'s frame and its banner video. Blocking: scripts and the worker. False + err on failure.
inline bool EnsureFrames(int id, std::string& err) {
  const fs::path f = TronFile(id);
  if (f.empty()) return err = "no mod for tron " + std::to_string(id), false;
  if (IsBink(f)) return err = "a Bink 2 mod: the viewer has no Bink decoder, so no preview", false;
  const int st = StateOf(id);
  if (st == 1) return true;
  if (st == 2) {
    std::lock_guard<std::mutex> lk(RegMu());
    return err = Registry()[id].err, false;
  }
  Cached c;
  CurrentSigs(id, c.sig, c.bsig);
  const Banner b = ReadBanner(id);
  if (!LoadScaled(f, 't', id, IsStackedAlpha(f), c.video, c.err)) c.failed = true;
  else if (b.mode == BannerMode::kFile && !LoadScaled(b.file, 'b', id, false, c.banner, c.err)) c.banner = Frame(), c.err.clear();  // a bad banner: black strips
  err = c.err;
  const bool ok = !c.failed;
  std::lock_guard<std::mutex> lk(RegMu());
  Registry()[id] = std::move(c);
  return ok;
}
// Starts the decode on a worker thread (one per tron at a time); DecodeGen() changes when it is done.
inline void StartDecode(int id) {
  static std::mutex m;
  static std::set<int> busy;
  {
    std::lock_guard<std::mutex> lk(m);
    if (!busy.insert(id).second) return;
  }
  std::thread([id] {
    std::string e;
    try { EnsureFrames(id, e); } catch (...) {}
    {
      std::lock_guard<std::mutex> lk(m);
      busy.erase(id);
    }
    DecodeGen()++;
  }).detach();
}
inline std::string DecodeError(int id) {
  std::lock_guard<std::mutex> lk(RegMu());
  auto it = Registry().find(id);
  return it == Registry().end() ? std::string() : it->second.err;
}

// Area-average resample of src rect (sx, sy, sw, sh) into dst rect (dx, dy, dw, dh) of a w-wide RGBA image.
inline void Scale(const Frame& src, int sx, int sy, int sw, int sh, Frame& dst, int dx, int dy, int dw, int dh) {
  if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
  for (int y = 0; y < dh; ++y) {
    const int y0 = sy + int(int64_t(y) * sh / dh), y1 = std::max(y0 + 1, sy + int(int64_t(y + 1) * sh / dh));
    for (int x = 0; x < dw; ++x) {
      const int x0 = sx + int(int64_t(x) * sw / dw), x1 = std::max(x0 + 1, sx + int(int64_t(x + 1) * sw / dw));
      uint32_t acc[3] = {0, 0, 0}, n = 0;
      for (int yy = y0; yy < y1 && yy < src.h; ++yy)
        for (int xx = x0; xx < x1 && xx < src.w; ++xx) {
          const uint8_t* p = &src.rgba[(size_t(yy) * src.w + xx) * 4];
          acc[0] += p[0], acc[1] += p[1], acc[2] += p[2], ++n;
        }
      uint8_t* q = &dst.rgba[(size_t(dy + y) * dst.w + dx + x) * 4];
      if (n) q[0] = uint8_t(acc[0] / n), q[1] = uint8_t(acc[1] / n), q[2] = uint8_t(acc[2] / n);
      q[3] = 255;
    }
  }
}
inline Frame Resize(const Frame& src, int sx, int sy, int sw, int sh, int dw, int dh) {
  Frame o;
  o.w = dw, o.h = dh;
  o.rgba.assign(size_t(dw) * dh * 4, 0);
  Scale(src, sx, sy, sw, sh, o, 0, 0, dw, dh);
  return o;
}

// What the stage gets for tron `id` while the mods are on (src\tron_shim.h DoFrame): `original` = the game's own sheet picture
// (null / !ok when the game cache has none). Empty Frame when there is nothing to show (no mod, toggle off, frame not decoded yet
// or failed: the stage then shows the game's tron). In the windowed viewer a missing decode is started on the worker.
inline Frame ModSheet(int id, const Frame* original) {
  Frame none;
  if (!Cfg().on) return none;
  const fs::path f = TronFor(id);
  if (f.empty()) return none;
  constexpr int S = 320, kBig = 224, kStrip = 24, kStrips = 4;
  const bool sheet = original && original->ok() && original->w == S && original->h == S;  // tron_shim.h:624
  if (IsBink(f)) {  // the game would play the mod with its own Bink decoder; the viewer has none: a black "no preview" sheet
    Frame k;
    k.w = k.h = S;
    k.rgba.assign(size_t(S) * S * 4, 0);
    for (size_t i = 3; i < k.rgba.size(); i += 4) k.rgba[i] = 255;
    return k;
  }
  int st = StateOf(id);
  if (st == 0) {
    if (Cfg().async) {
      StartDecode(id);
      return none;
    }
    std::string e;
    EnsureFrames(id, e);
    st = StateOf(id);
  }
  if (st != 1) return none;
  Frame v, bv;
  {
    std::lock_guard<std::mutex> lk(RegMu());
    const Cached& c = Registry()[id];
    v = c.video, bv = c.banner;
  }
  if (!sheet) return v;  // no 320x320 original: the video fills the frame
  Frame out;
  out.w = out.h = S;
  out.rgba.assign(size_t(S) * S * 4, 0);
  for (size_t i = 3; i < out.rgba.size(); i += 4) out.rgba[i] = 255;
  Scale(v, 0, 0, v.w, v.h, out, 0, 0, S, kBig);
  const Banner b = ReadBanner(id);
  if (b.mode == BannerMode::kOriginal) {
    std::copy_n(&original->rgba[size_t(kBig) * S * 4], size_t(S - kBig) * S * 4, &out.rgba[size_t(kBig) * S * 4]);
  } else if (b.mode == BannerMode::kVideo) {
    const int band = std::clamp(int(int64_t(v.h) * kStrip / kBig) & ~1, 2, v.h & ~1);
    const int y0 = ((v.h - band) / 2) & ~1;
    for (int i = 0; i < kStrips; ++i) Scale(v, 0, y0, v.w, band, out, 0, kBig + i * kStrip, S, kStrip);
  } else if (b.mode == BannerMode::kFile && bv.ok()) {
    const bool split = int64_t(bv.w) * kStrip > int64_t(bv.h) * 2 * S;  // wider than 26.7:1: four consecutive quarters
    for (int i = 0; i < kStrips; ++i) {
      const int qx = split ? ((bv.w / kStrips) * i) & ~1 : 0, qw = split ? (bv.w / kStrips) & ~1 : bv.w;
      Scale(bv, qx, 0, qw, bv.h, out, 0, kBig + i * kStrip, S, kStrip);
    }
  }  // none (or a banner video that failed): black
  return out;
}

}  // namespace mods_lookup
}  // namespace assets
