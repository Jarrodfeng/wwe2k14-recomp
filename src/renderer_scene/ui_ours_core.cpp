// See ui_ours_core.h. Session 101, lane 66.
#include "ui_ours_core.h"

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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "../assets/dds.h"
#include "../assets/pac.h"
#include "../launcher/disc_image.h"

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

// ---- the hash cache (one file per pac; only hashes and paths: nothing from the game's pictures) --------------------------------------------------------------
constexpr uint32_t kCacheMagic = 0x32585555u;   // "UUX2"
constexpr uint32_t kCacheVersion = 3;           // bump when the hash, the walk or the entry layout changes

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

// ---- faults ---------------------------------------------------------------------------------------------------------------------------------------------------
bool UiFaults::Parse(const std::string& name, UiFaults& out) {
  out = {};
  if (name.empty()) return true;
  if (name.rfind("unchecked_", 0) == 0) {
    if (!Parse(name.substr(10), out) || name == "unchecked_stale_after_join" || name == "unchecked_stale_stream") return out = {}, false;
    out.no_checks = true;
    return true;
  }
  if (name == "stale_stream") out.names_stripped = out.stale_after_join = true;
  else if (name == "first_texture" || name == "wrong_texture") out.first_texture = true;   // lane 126: wrong_texture = the first joined texture for every sprite
  else if (name == "no_view_check") out.no_view_check = true;   // lane 126
  else if (name == "no_deep") out.no_deep = true;               // lane 126
  else if (name == "deep_wrong_arena") out.deep_wrong_arena = true;   // lane 126
  else if (name == "tier_boundary") out.tier_boundary = true;         // lane 126
  else if (name == "icon_as_font") out.icon_as_font = true;
  else if (name == "rb_swap") out.rb_swap = true;
  else if (name == "key_shift") out.key_shift = true;
  else if (name == "short_mips") out.short_mips = true;
  else if (name == "names_stripped") out.names_stripped = true;
  else if (name == "stale_after_join") out.stale_after_join = true;
  else if (name == "gpu_copy_after_join") out.gpu_copy_after_join = true;
  else return false;
  return true;
}

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

bool BuildPacIndex(const UiIndexOptions& options, uint8_t pac_index, std::vector<UiFileEntry>& out, std::string& err, bool* from_cache) {
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
  if (cacheable) WriteCache(options.cache_dir, pac_index, size, time, out);
  return true;
}

// ---- the loader -----------------------------------------------------------------------------------------------------------------------------------------------
struct UiFileLoader::Impl {
  launcher::GameFiles files;
  UiFaults faults;
  std::map<uint8_t, std::unique_ptr<pac::Pac>> pacs;
  std::map<uint8_t, uint64_t> used;   // lane 126 (review F1.5): the tick each pac was last asked for (LRU)
  uint64_t tick = 0;

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

UiFileLoader::UiFileLoader(const std::string& game_root, const UiFaults& faults) : impl_(new Impl) {
  impl_->files.SetRoot(std::filesystem::path(game_root));
  impl_->faults = faults;
}
UiFileLoader::~UiFileLoader() = default;
void UiFileLoader::ReleasePacs() { impl_->pacs.clear(), impl_->used.clear(); }
size_t UiFileLoader::resident_pacs() const { return impl_->pacs.size(); }

bool UiFileLoader::Load(const UiFileEntry& e, uint32_t want_mips, UiLoaded& out, std::string& err) {
  out = {};
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

// ---- the decisions --------------------------------------------------------------------------------------------------------------------------------------------
bool UiCore::JoinByContent(const rdr_adapter::ImageContent& c, const uint8_t* swizzle, uint32_t& entry) {
  if (!index_) return false;
  std::vector<uint32_t> ids;
  index_->FindAll(c.width, c.height, rdr::Format(uint8_t(c.format)), c.mips, c.hash, ids);
  if (ids.empty()) return false;
  entry = ids.front();
  for (const uint32_t id : ids) {   // review F4: the first whose colours come out right through the stream slot's swizzle (ids are in index order)
    UiFileEntry fe;
    if (index_->Get(id, fe) && UiSwizzleMatches(fe.format, rdr::Format(uint8_t(c.format)), swizzle)) return entry = id, true;
  }
  return true;   // none matches: the first, and the per-draw swizzle check refuses it (counted)
}

UiCore::Verdict UiCore::Decide(uint32_t key, uint32_t stream_image, const uint8_t* swizzle, uint32_t view_format, uint32_t min_level, uint32_t max_level, UiTextureProvider& provider) {
  using rdr_adapter::KeepWhy;
  Verdict v;
  ++census_.claimed;
  if (!stream_image) return ++census_.untextured, v;
  rdr_adapter::ImageContent c;
  if (!content_ || !content_(stream_image, c)) return ++census_.gpu, v;   // no content was ever told: a surface (a GPU picture) or a texture not read yet
  if (c.hash == 0) return ++census_.movie, v;
  if (!key) return ++census_.nokey, v;
  const auto frame_key = frame_keys_.emplace(key, stream_image);
  if (!frame_key.second && frame_key.first->second != stream_image) {   // the key names two images in one frame: not 1:1, the draw and the key's join are dropped
    joins_.erase(key);
    return ++census_.mismatch_key, ++census_.mismatch, v.why = KeepWhy::Mismatch, v;
  }
  Join& j = joins_[key];
  const uint32_t index_gen = index_ ? index_->generation() : 0;
  const bool known = j.image != 0;
  const bool content_changed = known && (j.image != stream_image || j.generation != c.generation);
  if (!known || content_changed || (j.entry < 0 && j.index_gen != index_gen)) {
    const bool was_joined = known && j.entry >= 0;
    if (content_changed && was_joined) ++census_.changed_after_join;
    uint32_t e = 0;
    bool declared = false;
    bool found = false;
    if (const auto d = declared_.find(key); d != declared_.end()) e = d->second, declared = found = true;
    else found = JoinByContent(c, swizzle, e);
    j.image = stream_image, j.generation = c.generation, j.index_gen = index_gen, j.tex = nullptr;
    if (!found) {
      j.entry = -1;
      return ++census_.unjoined, v;
    }
    j.entry = e, j.declared = declared;
    if (!was_joined) {
      join_order_.push_back(key);
      ++(declared ? census_.joined_declared : census_.joined_content);
    } else if (content_changed) {
      ++census_.rejoined;
    }
  }
  if (j.entry < 0) return ++census_.unjoined, v;
  uint32_t entry = uint32_t(j.entry);
  // The join-side plants (the entry used is not the one the content named).
  if (faults_.first_texture) {
    if (first_entry_ < 0) first_entry_ = entry;
    entry = uint32_t(first_entry_);
  }
  if (faults_.key_shift && join_order_.size() > 1) {
    const auto at = std::find(join_order_.begin(), join_order_.end(), key);
    const uint32_t other = at == join_order_.end() || at == join_order_.begin() ? join_order_.back() : *(at - 1);
    if (const auto o = joins_.find(other); o != joins_.end() && o->second.entry >= 0) entry = uint32_t(o->second.entry);
  }
  if (faults_.icon_as_font && index_) {
    uint32_t sheet = 0, icons = 0;
    if (index_->FindPath(0, "p0/0/0", sheet) && index_->FindPath(0, "p0/0/1", icons) && entry == sheet) entry = icons;
  }
  if (min_level != 0) return ++census_.mismatch_mips, ++census_.mismatch, v.why = KeepWhy::Mismatch, v;   // a slot that starts above mip 0 stays the stream's (the join is kept)
  // A view of another class than the storage (a signed / biased reinterpretation of the stream's image): ours is the plain picture, so the stream's draw stays (review F3).
  if (!faults_.no_view_check && view_format != 0 && UiByteClass(rdr::Format(uint8_t(view_format))) != UiByteClass(rdr::Format(uint8_t(c.format)))) return ++census_.mismatch_view, ++census_.mismatch, v.why = KeepWhy::Mismatch, v;
  // The hot path (review C2): the entry's chain length and the provider's texture are cached per key while the provider's pointers stay valid.
  const uint32_t epoch = provider.Epoch();
  const bool cached = j.tex && j.tex_entry == entry && j.tex_epoch == epoch;
  uint32_t chain_mips = j.chain_mips;
  UiFileEntry fe;
  if (!cached) {
    if (!index_ || !index_->Get(entry, fe)) return ++census_.unjoined, v;
    chain_mips = fe.mips;
  }
  const uint32_t want = std::min<uint32_t>({chain_mips, max_level + 1, c.mips});   // the stream holds c.mips; its fetch reads up to max_level
  const UiTexture* t = nullptr;
  if (cached && j.tex_want == want) {
    t = j.tex;
  } else {
    if (fe.prefix.empty() && (!index_ || !index_->Get(entry, fe))) return ++census_.unjoined, v;
    bool pending = false;
    t = provider.Texture(entry, fe, want, pending);
    if (!t) {
      j.tex = nullptr;
      if (pending) return ++census_.pending, v.why = KeepWhy::NotReady, v;
      return ++census_.load_failed, ++census_.unjoined, v;
    }
    j.tex = t, j.tex_entry = entry, j.tex_want = want, j.chain_mips = chain_mips, j.tex_epoch = epoch;
  }
  t->last_used = frame_;
  // The per-draw checks: ours must be the stream's picture in size, format, mip count and bytes (OUR upload's hash against the stream content's), else the stream's draw
  // stays and the key's join is dropped until its content changes.
  const auto drop = [&](uint64_t& counter) -> Verdict {
    ++counter, ++census_.mismatch;
    j.entry = -1, j.tex = nullptr;
    v.why = KeepWhy::Mismatch;
    return v;
  };
  uint64_t* failed = nullptr;
  if (t->width != c.width || t->height != c.height || UiByteClass(t->format) != UiByteClass(rdr::Format(uint8_t(c.format)))) failed = &census_.mismatch_size;
  else if (!UiSwizzleMatches(t->format, rdr::Format(uint8_t(c.format)), swizzle)) failed = &census_.mismatch_swizzle;
  else if (c.mips == 0 || t->mips != want) failed = &census_.mismatch_mips;   // (ours holds exactly the mips the slot reads)
  else if (t->prefix.size() < c.mips || t->prefix[c.mips - 1] != c.hash) failed = &census_.mismatch_bytes;
  if (failed) {
    if (!faults_.no_checks) return drop(*failed);
    ++*failed, ++census_.unchecked;   // (the "unchecked_" plants: the check fired but the draw is replaced anyway, so the planted picture shows what the check protects)
  }
  v.ours = true, v.texture = t, v.entry = entry, v.declared = j.declared;
  ++census_.replaced;
  return v;
}

std::string UiCore::Report(const UiFileIndex& index) const {
  const UiCensus& c = census_;
  char line[900];
  std::snprintf(line, sizeof line,
                "replaced %llu, kept %llu (unjoined %llu, mismatch %llu, movie %llu, gpu %llu, untextured %llu, pending %llu, nokey %llu), keys joined %llu (declared %llu, by content %llu), "
                "changed after join %llu (rejoined %llu), mismatch size %llu view %llu swizzle %llu mips %llu bytes %llu key %llu, load failed %llu, unchecked %llu, %s",
                (unsigned long long)c.replaced, (unsigned long long)(c.claimed - c.replaced), (unsigned long long)c.unjoined, (unsigned long long)c.mismatch, (unsigned long long)c.movie,
                (unsigned long long)c.gpu, (unsigned long long)c.untextured, (unsigned long long)c.pending, (unsigned long long)c.nokey,
                (unsigned long long)(c.joined_declared + c.joined_content), (unsigned long long)c.joined_declared, (unsigned long long)c.joined_content,
                (unsigned long long)c.changed_after_join, (unsigned long long)c.rejoined, (unsigned long long)c.mismatch_size, (unsigned long long)c.mismatch_view, (unsigned long long)c.mismatch_swizzle, (unsigned long long)c.mismatch_mips,
                (unsigned long long)c.mismatch_bytes, (unsigned long long)c.mismatch_key, (unsigned long long)c.load_failed, (unsigned long long)c.unchecked, index.Summary().c_str());
  return line;
}

}  // namespace rdr_scene
