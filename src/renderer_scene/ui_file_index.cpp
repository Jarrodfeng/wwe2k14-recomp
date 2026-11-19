// See ui_file_index.h. The file index of the menu pacs (S103 PN1: split out of ui_ours_core.cpp, Session 101 lane 66).
#include "ui_file_index.h"

// stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it), exactly like crowd_body.cpp / arena_game_space.cpp: pac.h needs
// stbi_zlib_decode_buffer, and the game exe and the phone library must not export stbi_*.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

#include "../assets/dds.h"
#include "../assets/pac.h"
#include "../launcher/disc_image.h"
#include "../renderer_adapter/image_content_table.h"

namespace rdr_scene {
namespace {

namespace pac = assets::pac;
using assets::Bytes;
using rdr_adapter::ContentHasher;

bool ToFormat(assets::TexFormat f, rdr::Format& out) {
  switch (f) {
    case assets::TexFormat::kBC1: return out = rdr::Format::BC1, true;
    case assets::TexFormat::kBC2: return out = rdr::Format::BC2, true;
    case assets::TexFormat::kBC3: return out = rdr::Format::BC3, true;
    case assets::TexFormat::kB8G8R8A8: return out = rdr::Format::BGRA8, true;
    case assets::TexFormat::kR8G8B8A8: return out = rdr::Format::RGBA8, true;
    default: return false;   // (A4R4G4B4: no menu pac holds one; not indexed rather than guessed)
  }
}

std::string Num(unsigned long long v) { return std::to_string(v); }

// A texture directory leaf (the header, then count x {name[16], "dds", u32 size, u32 offset, u32 0}); false when `b` is not one (src\assets\dds.h TexSet::AddDir's test).
struct DirEnt {
  std::string name;
  size_t off = 0, size = 0;
};
bool ParseTexDir(const Bytes& b, std::vector<DirEnt>& out) {
  out.clear();
  if (b.size() < 0x30) return false;
  const uint32_t count = assets::Le32(b, 0);
  if (count == 0 || count > 4096 || 0x10 + size_t(count) * 32 > b.size()) return false;
  for (uint32_t i = 0; i < count; ++i) {
    const size_t r = 0x10 + size_t(i) * 32;
    if (std::memcmp(b.data() + r + 16, "dds", 3) != 0) return false;
    char nm[17] = {};
    std::memcpy(nm, b.data() + r, 16);
    const uint32_t size = assets::Le32(b, r + 20), off = assets::Le32(b, r + 24);
    if (off > b.size() || size > b.size() - off) return false;
    out.push_back({nm, off, size});
  }
  return true;
}

// A DDS -> the picture's facts and the hash of every mip prefix (mip 0 first, tightly packed: the bytes the renderer is given). false: not a picture the UI uploads.
bool Describe(const uint8_t* p, size_t n, assets::Texture& t, rdr::Format& format, std::vector<uint64_t>& prefix) {
  std::string why;
  if (!assets::ParseDds(p, n, t, why) || !why.empty()) return false;   // (a short mip chain is not drawn half-way: UiAssets refuses it too)
  if (!ToFormat(t.format, format) || t.width > 65535 || t.height > 65535) return false;
  prefix.clear();
  ContentHasher h;
  size_t at = 0;
  for (uint32_t m = 0; m < t.mips; ++m) {
    const size_t bytes = assets::MipBytes(t.format, t.width >> m, t.height >> m);
    if (at + bytes > t.data.size()) return false;
    h.Update(t.data.data() + at, bytes);
    at += bytes;
    prefix.push_back(h.Digest());
  }
  return at == t.data.size();
}

thread_local bool g_names_only = false;   // lane 126: set by BuildPacIndex around a Walk (one index build at a time per thread; thread_local below)
void AddDds(std::vector<UiFileEntry>& out, uint8_t pac_index, const std::string& path, const uint8_t* p, size_t n) {
  if (g_names_only) {   // lane 126: the arena bg pacs keep only the texture-directory members named "*lightmap*"
    const size_t colon = path.find(':');
    if (colon == std::string::npos || assets::Lower(path.substr(colon + 1)).find("lightmap") == std::string::npos) return;
  }
  assets::Texture t;
  UiFileEntry e;
  if (!Describe(p, n, t, e.format, e.prefix)) return;
  e.pac = pac_index, e.path = path, e.width = uint16_t(t.width), e.height = uint16_t(t.height), e.mips = uint8_t(t.mips), e.bytes = uint32_t(t.data.size());
  out.push_back(std::move(e));
}

void Walk(const Bytes& buf, const pac::Pach& p, const std::string& prefix, int depth, uint8_t pac_index, std::vector<UiFileEntry>& out) {
  for (const pac::Ent& ent : p.ents) {
    Bytes d = pac::Decode(buf.data() + ent.off, ent.size);
    if (d.empty()) continue;
    const std::string path = prefix + "/" + Num(ent.id);
    if (d.size() >= 4 && std::memcmp(d.data(), "DDS ", 4) == 0) {
      AddDds(out, pac_index, path, d.data(), d.size());
    } else if (d.size() >= 8 && std::memcmp(d.data(), "PACH", 4) == 0) {
      pac::Pach np;
      if (depth < 6 && pac::ParsePach(d, 0, d.size(), np)) Walk(d, np, path, depth + 1, pac_index, out);
    } else {
      std::vector<DirEnt> dir;
      if (!ParseTexDir(d, dir)) continue;
      for (const DirEnt& de : dir) AddDds(out, pac_index, path + ":" + de.name, d.data() + de.off, de.size);
    }
  }
}

// S103 PN5: the roster renders sit OUTSIDE the PACH archives. The EPAC header (0x800..0x4000) holds a directory of 12-byte little-endian rows: a directory row is
// {tag, count, 0}, an entry row {name, sector, units} (sector = 2 KB units after the 0x4000 header, units = 256-byte units; the data is one BPE / ZLIB / plain block).
// menuHD.pac's SSFA / SSFB / SSFC directories (key = the character id as 4 digits) hold 512x512 / 512x512 / 256x256 DXT5 renders: the select screen's large render is
// SSFB '9000' [C: c2d_select images 533 / 534 equal it byte for byte; out/s103/pn5]. pac::Open walks PACHs only, so neither the index nor the loader saw them.
// Only these three tags are read (an unknown directory is skipped).
constexpr size_t kEpacHeader = 0x4000, kEpacDirAt = 0x800;
bool IsRosterDir(const char* tag) { return !std::memcmp(tag, "SSFA", 4) || !std::memcmp(tag, "SSFB", 4) || !std::memcmp(tag, "SSFC", 4); }
// Calls fn(dir tag, entry name, decoded bytes) for every entry of a roster directory (only the named one when want_dir / want_name are given: the others are not decoded).
// PN5 review B3: a directory row's count (in words: count / 3 rows) BOUNDS its entries (the next count / 3 rows are entries, whatever their size: a zero-size entry is skipped, it never ends the directory),
// and it is VERIFIED: a directory that ends (the header runs out) before its count, or entry rows past the count, are counted in stats->mismatches. Names that are not 4
// alphanumeric characters are skipped (stats->bad_names); a name twice in one directory keeps the FIRST (stats->dupes), for the index and the loader alike.
template <class Fn>
void ForEachRosterEntry(const Bytes& b, const char* want_dir, const char* want_name, Fn&& fn, UiRosterStats* stats = nullptr) {
  UiRosterStats local;
  UiRosterStats& st = stats ? *stats : local;
  if (b.size() < kEpacHeader || std::memcmp(b.data(), "EPAC", 4) != 0) return;
  char dir[5] = {};
  bool in_roster = false;
  uint32_t remaining = 0;   // entry rows still owed by the current directory row's count
  std::set<std::string> seen;
  auto closed = [&]() {
    if (remaining) ++st.mismatches, st.note += std::string(st.note.empty() ? "" : "; ") + dir + ": " + Num(remaining) + " entries short of the directory's count";
    remaining = 0;
  };
  for (size_t r = kEpacDirAt; r + 12 <= kEpacHeader; r += 12) {
    const uint8_t* row = b.data() + r;
    const uint32_t a = assets::Le32(b, r + 4), u = assets::Le32(b, r + 8);
    const bool zero_row = a == 0 && u == 0 && !std::memcmp(row, "\0\0\0\0", 4);
    if (remaining == 0) {
      if (zero_row) continue;   // an empty row
      if (u == 0) {   // a directory row {tag, count, 0}
        std::memcpy(dir, row, 4);
        in_roster = IsRosterDir(dir);
        remaining = a / 3;   // [C: menuHD.pac, 9 directories: the count is in 32-bit words, 3 per 12-byte row: SSFA 297 = 99 rows]
        if (in_roster) {
          ++st.dirs;
          if (a % 3) ++st.mismatches, st.note += std::string(st.note.empty() ? "" : "; ") + dir + ": count " + Num(a) + " is not a whole number of rows";
        }
        continue;
      }
      if (in_roster) ++st.mismatches, st.note += std::string(st.note.empty() ? "" : "; ") + dir + ": an entry row past the directory's count";
      continue;   // (rows past the count are not read)
    }
    --remaining;
    if (!in_roster || u == 0) continue;   // another directory's entry, or a zero-size entry (kept in the count, nothing to read)
    char name[5] = {};
    std::memcpy(name, row, 4);
    bool ok = true;
    for (int k = 0; k < 4; ++k) ok = ok && std::isalnum(uint8_t(name[k]));
    if (!ok) {
      ++st.bad_names;
      continue;
    }
    if (!seen.insert(std::string(dir) + "/" + name).second) {
      ++st.dupes;
      continue;
    }
    if (want_dir && std::memcmp(dir, want_dir, 4) != 0) continue;
    if (want_name && std::memcmp(name, want_name, 4) != 0) continue;
    const size_t off = kEpacHeader + size_t(a) * 0x800, size = size_t(u) * 256;
    if (off >= b.size() || size > b.size() - off) continue;
    Bytes d = pac::Decode(b.data() + off, size);
    if (!d.empty()) ++st.entries, fn(dir, name, d);
  }
  if (in_roster) closed();   // the header ended inside a roster directory
}

// ---- the hash cache (one file per pac; only hashes and paths: nothing from the game's pictures) --------------------------------------------------------------
constexpr uint32_t kCacheMagic = 0x32585555u;   // "UUX2"
constexpr uint32_t kCacheVersion = 5;          // bump when the hash, the walk or the entry layout changes

bool CacheKey(const std::filesystem::path& file, uint64_t& size, uint64_t& time) {
  std::error_code ec;
  size = std::filesystem::file_size(file, ec);
  if (ec) return false;
  time = uint64_t(std::filesystem::last_write_time(file, ec).time_since_epoch().count());
  return !ec;
}
std::filesystem::path CachePath(const std::string& dir, uint8_t pac) {
  std::string name = UiPacPath(pac);
  name = name.substr(name.find_last_of('\\') + 1);
  return std::filesystem::path(dir) / ("ui_index_" + name + (g_names_only ? "_lm" : "") + ".bin");   // lane 126: a name-filtered index is a different file
}
template <class T>
void Put(std::string& s, const T& v) { s.append(reinterpret_cast<const char*>(&v), sizeof(T)); }
template <class T>
bool Get(const std::string& s, size_t& at, T& v) {
  if (at + sizeof(T) > s.size()) return false;
  std::memcpy(&v, s.data() + at, sizeof(T));
  at += sizeof(T);
  return true;
}
bool ReadCache(const std::string& dir, uint8_t pac, uint64_t size, uint64_t time, std::vector<UiFileEntry>& out) {
  std::ifstream f(CachePath(dir, pac), std::ios::binary);
  if (!f) return false;
  std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  size_t at = 0;
  uint32_t magic = 0, version = 0, count = 0;
  uint64_t csize = 0, ctime = 0;
  if (!Get(s, at, magic) || !Get(s, at, version) || !Get(s, at, csize) || !Get(s, at, ctime) || !Get(s, at, count)) return false;
  if (magic != kCacheMagic || version != kCacheVersion || csize != size || ctime != time || count > 1000000) return false;
  out.clear();
  for (uint32_t i = 0; i < count; ++i) {
    UiFileEntry e;
    uint16_t len = 0;
    uint8_t fmt = 0;
    e.pac = pac;
    if (!Get(s, at, len) || at + len > s.size()) return false;
    e.path.assign(s.data() + at, len), at += len;
    if (!Get(s, at, e.width) || !Get(s, at, e.height) || !Get(s, at, fmt) || !Get(s, at, e.mips) || !Get(s, at, e.bytes) || e.mips == 0) return false;
    e.format = rdr::Format(fmt);
    e.prefix.resize(e.mips);
    for (uint8_t m = 0; m < e.mips; ++m)
      if (!Get(s, at, e.prefix[m])) return false;
    out.push_back(std::move(e));
  }
  return at == s.size();
}
void WriteCache(const std::string& dir, uint8_t pac, uint64_t size, uint64_t time, const std::vector<UiFileEntry>& entries) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  std::string s;
  Put(s, kCacheMagic), Put(s, kCacheVersion), Put(s, size), Put(s, time), Put(s, uint32_t(entries.size()));
  for (const UiFileEntry& e : entries) {
    Put(s, uint16_t(e.path.size()));
    s += e.path;
    Put(s, e.width), Put(s, e.height), Put(s, uint8_t(e.format)), Put(s, e.mips), Put(s, e.bytes);
    for (uint8_t m = 0; m < e.mips; ++m) Put(s, e.prefix[m]);
  }
  std::ofstream f(CachePath(dir, pac), std::ios::binary | std::ios::trunc);
  if (f) f.write(s.data(), std::streamsize(s.size()));   // (a failed write is only a missed cache)
}

}  // namespace

// ---- the index ------------------------------------------------------------------------------------------------------------------------------------------------
void UiFileIndex::AddPac(uint8_t pac_index, std::vector<UiFileEntry>&& entries) {
  std::lock_guard<std::mutex> lock(mu_);
  for (UiFileEntry& e : entries) {
    const std::string path_key = Num(e.pac) + "|" + e.path;
    if (e.prefix.empty()) continue;
    const uint64_t full = Key(e.width, e.height, e.format, e.mips, e.prefix.back());
    bool dup = false;
    auto range = by_key_.equal_range(full);
    for (auto it = range.first; it != range.second; ++it) {
      UiFileEntry& o = entries_[it->second];
      if (o.width == e.width && o.height == e.height && o.format == e.format && o.mips == e.mips && o.prefix.back() == e.prefix.back()) {
        ++o.same;
        by_path_.emplace(path_key, it->second);   // (any leaf of the class finds the class's first entry)
        dup = true;
        break;
      }
    }
    if (dup) continue;
    const uint32_t id = uint32_t(entries_.size());
    for (uint32_t k = 1; k <= e.mips; ++k) by_key_.emplace(Key(e.width, e.height, e.format, k, e.prefix[k - 1]), id);
    by_path_.emplace(path_key, id);
    entries_.push_back(std::move(e));
  }
  (void)pac_index;
  ++pacs_done_;
  ++generation_;
}


void UiFileIndex::FindAll(uint32_t width, uint32_t height, rdr::Format format, uint32_t mips, uint64_t hash, std::vector<uint32_t>& ids) const {
  ids.clear();
  if (mips == 0) return;
  std::lock_guard<std::mutex> lock(mu_);
  const auto range = by_key_.equal_range(Key(width, height, format, mips, hash));
  for (auto it = range.first; it != range.second; ++it) {
    const UiFileEntry& e = entries_[it->second];
    if (e.width == width && e.height == height && UiByteClass(e.format) == UiByteClass(format) && e.mips >= mips && e.prefix[mips - 1] == hash) ids.push_back(it->second);
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

bool UiFileIndex::Find(uint32_t width, uint32_t height, rdr::Format format, uint32_t mips, uint64_t hash, uint32_t& id) const {
  std::vector<uint32_t> ids;
  FindAll(width, height, format, mips, hash, ids);
  if (ids.empty()) return false;
  id = ids.front();
  return true;
}

bool UiFileIndex::Get(uint32_t id, UiFileEntry& out) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (id >= entries_.size()) return false;
  out = entries_[id];
  return true;
}

bool UiFileIndex::FindPath(uint8_t pac_index, const std::string& path, uint32_t& id) const {
  std::lock_guard<std::mutex> lock(mu_);
  const auto it = by_path_.find(Num(pac_index) + "|" + path);
  if (it == by_path_.end()) return false;
  id = it->second;
  return true;
}

size_t UiFileIndex::texture_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return entries_.size();
}

std::string UiFileIndex::Summary() const {
  return "index " + Num(texture_count()) + " textures in " + Num(pacs_done()) + " pacs";
}

bool BuildPacIndex(const UiIndexOptions& options, uint8_t pac_index, std::vector<UiFileEntry>& out, std::string& err, bool* from_cache, UiRosterStats* roster) {
  out.clear();
  if (from_cache) *from_cache = false;
  if (!UiPacValid(pac_index)) return err = "no such pac index " + Num(pac_index), false;
  const std::string rel = UiPacPath(pac_index);
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::path(options.game_root));
  if (!files.Exists(rel)) return err = "missing " + rel, false;
  g_names_only = pac_index >= options.names_only_from;   // lane 126 (before the cache is read: the cache file name carries it)
  struct ResetFilter { ~ResetFilter() { g_names_only = false; } } reset_filter;
  uint64_t size = 0, time = 0;
  const bool cacheable = !options.cache_dir.empty() && !files.is_image() && CacheKey(std::filesystem::path(options.game_root) / rel, size, time);
  if (cacheable && ReadCache(options.cache_dir, pac_index, size, time, out)) {
    if (from_cache) *from_cache = true;
    return true;
  }
  pac::Pac pc;
  if (!pac::Open(files, rel, pc, err)) return false;
  for (size_t n = 0; n < pc.pachs.size(); ++n) Walk(pc.bytes, pc.pachs[n], "p" + Num(n), 0, pac_index, out);
  UiRosterStats rs;
  ForEachRosterEntry(pc.bytes, nullptr, nullptr, [&](const char* dir, const char* name, const Bytes& d) {   // S103 PN5: path "d/SSFB/9000"
    if (d.size() >= 4 && std::memcmp(d.data(), "DDS ", 4) == 0) AddDds(out, pac_index, std::string("d/") + dir + "/" + name, d.data(), d.size());
  }, &rs);
  if (roster) *roster = rs;
  if (cacheable) WriteCache(options.cache_dir, pac_index, size, time, out);
  return true;
}

// ---- the loader -----------------------------------------------------------------------------------------------------------------------------------------------
struct UiFileLoader::Impl {
  launcher::GameFiles files;
  UiFileFaults faults;
  std::map<uint8_t, std::unique_ptr<pac::Pac>> pacs;
  std::map<uint8_t, uint64_t> used;   // lane 126 (review F1.5): the tick each pac was last asked for (LRU)
  uint64_t tick = 0;
  uint32_t planted_failed = 0;   // faults.fail_loads: how many loads were failed on purpose so far

  const pac::Pac* Open(uint8_t index, std::string& err) {
    auto it = pacs.find(index);
    if (it != pacs.end()) return used[index] = ++tick, it->second.get();
    if (!UiPacValid(index)) return err = "no such pac index " + Num(index), nullptr;
    if (pacs.size() >= 2) {   // never hold more than two decoded pacs; the LEAST recently used goes (a burst alternating two pacs no longer unpacks the big one again and again)
      uint8_t lru = pacs.begin()->first;
      for (const auto& [k, v] : pacs)
        if (used[k] < used[lru]) lru = k;
      pacs.erase(lru), used.erase(lru);
    }
    auto p = std::make_unique<pac::Pac>();
    if (!pac::Open(files, UiPacPath(index), *p, err)) return nullptr;
    used[index] = ++tick;
    return (pacs[index] = std::move(p)).get();
  }

  // The leaf the entry path names ("p<N>/<id>[/<id>...][:name]"), decoded.
  bool Leaf(const UiFileEntry& e, Bytes& blob, std::string& name, std::string& err) {
    std::string head = e.path;
    name.clear();
    if (head.size() == 11 && head.compare(0, 2, "d/") == 0 && head[6] == '/') {   // S103 PN5: a roster directory entry "d/SSFB/9000" (an EPAC header directory, not a PACH)
      const pac::Pac* pc = Open(e.pac, err);
      if (!pc) return false;
      blob.clear();
      ForEachRosterEntry(pc->bytes, head.substr(2, 4).c_str(), head.substr(7, 4).c_str(), [&](const char*, const char*, const Bytes& d) { blob = d; });
      return blob.empty() ? (err = e.path + ": no such roster entry in " + UiPacPath(e.pac), false) : true;
    }
    if (const size_t colon = head.find(':'); colon != std::string::npos) name = head.substr(colon + 1), head.resize(colon);
    std::vector<uint32_t> ids;
    size_t pos = 1;
    if (head.size() < 2 || head[0] != 'p') return err = "bad entry path " + e.path, false;
    while (pos <= head.size()) {
      const size_t slash = head.find('/', pos);
      const std::string part = head.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
      if (part.empty() || part.find_first_not_of("0123456789") != std::string::npos) return err = "bad entry path " + e.path, false;
      ids.push_back(uint32_t(std::strtoul(part.c_str(), nullptr, 10)));
      if (slash == std::string::npos) break;
      pos = slash + 1;
    }
    if (ids.size() < 2) return err = "bad entry path " + e.path, false;
    const pac::Pac* pc = Open(e.pac, err);
    if (!pc) return false;
    if (ids[0] >= pc->pachs.size()) return err = UiPacPath(e.pac) + ": no top-level PACH " + Num(ids[0]), false;
    const Bytes* buf = &pc->bytes;
    const pac::Pach* pach = &pc->pachs[ids[0]];
    Bytes cur;
    pac::Pach cur_pach;
    for (size_t i = 1; i < ids.size(); ++i) {
      if (!pac::Find(*pach, ids[i])) return err = e.path + ": no entry " + Num(ids[i]) + " at depth " + Num(i), false;
      Bytes next = pac::Get(*buf, *pach, ids[i]);
      if (next.empty()) return err = e.path + ": entry " + Num(ids[i]) + " is empty or does not decode", false;
      if (i + 1 < ids.size()) {
        pac::Pach np;
        if (!pac::ParsePach(next, 0, next.size(), np)) return err = e.path + ": entry " + Num(ids[i]) + " is not a PACH", false;
        cur = std::move(next), cur_pach = std::move(np);
        buf = &cur, pach = &cur_pach;
      } else {
        blob = std::move(next);
      }
    }
    return true;
  }
};

UiFileLoader::UiFileLoader(const std::string& game_root, const UiFileFaults& faults) : impl_(new Impl) {
  impl_->files.SetRoot(std::filesystem::path(game_root));
  impl_->faults = faults;
}
UiFileLoader::~UiFileLoader() = default;
void UiFileLoader::ReleasePacs() { impl_->pacs.clear(), impl_->used.clear(); }
size_t UiFileLoader::resident_pacs() const { return impl_->pacs.size(); }

bool UiFileLoader::Load(const UiFileEntry& e, uint32_t want_mips, UiLoaded& out, std::string& err) {
  out = {};
  if (impl_->planted_failed < impl_->faults.fail_loads) return ++impl_->planted_failed, err = "planted load failure", false;
  Bytes blob;
  std::string name;
  if (!impl_->Leaf(e, blob, name, err)) return false;
  const uint8_t* dds = blob.data();
  size_t dds_size = blob.size();
  if (!name.empty()) {
    std::vector<DirEnt> dir;
    if (!ParseTexDir(blob, dir)) return err = e.path + ": the leaf is not a texture directory (a name was given)", false;
    const DirEnt* hit = nullptr;
    for (const DirEnt& d : dir)
      if (assets::Lower(d.name) == assets::Lower(name)) { hit = &d; break; }
    if (!hit) return err = e.path + ": texture '" + name + "' is not in its directory", false;
    dds = blob.data() + hit->off, dds_size = hit->size;
  }
  assets::Texture t;
  rdr::Format format = rdr::Format::Undefined;
  std::vector<uint64_t> prefix;
  if (!Describe(dds, dds_size, t, format, prefix)) return err = e.path + ": not a texture the UI uploads", false;
  // The index is only hashes: the picture read now must be the indexed one (a pac replaced since the index was cached would otherwise join the wrong bytes).
  if (t.width != e.width || t.height != e.height || format != e.format || t.mips != e.mips || prefix != e.prefix)
    return err = e.path + ": the picture in " + UiPacPath(e.pac) + " is not the one indexed (stale index)", false;
  out.width = t.width, out.height = t.height, out.mips = t.mips, out.format = format;
  out.data = std::move(t.data);
  // Plants that change what is uploaded (the per-draw bytes / mips checks must catch them).
  if (impl_->faults.rb_swap && (format == rdr::Format::BGRA8 || format == rdr::Format::RGBA8))
    for (size_t i = 0; i + 3 < out.data.size(); i += 4) std::swap(out.data[i], out.data[i + 2]);
  // The hash of what is loaded, per mip prefix of the FULL chain (the draw's bytes check reads it; computed from the bytes themselves, never copied from the index).
  ContentHasher h;
  size_t at = 0;
  std::vector<size_t> ends;
  for (uint32_t m = 0; m < out.mips; ++m) {
    const size_t bytes = UiMipBytes(format, out.width >> m, out.height >> m);
    h.Update(out.data.data() + at, bytes);
    at += bytes;
    ends.push_back(at);
    out.prefix.push_back(h.Digest());
  }
  // Keep only the mips the upload wants (the plant short_mips keeps mip 0 only).
  uint32_t keep = want_mips < 1 ? 1 : want_mips > out.mips ? out.mips : want_mips;
  if (impl_->faults.short_mips) keep = 1;
  out.data.resize(ends[keep - 1]);
  out.mips = keep;
  return true;
}
}  // namespace rdr_scene
