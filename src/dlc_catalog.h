// wwe2k14 - DLC packs on disk: find them and read their catalogs (Session 32). Shared by the game (src\dlc.h) and the
// launcher; standard C++ only.
//
// A pack is either a folder holding info\catalog.dlc (magic DLCC: extracted DLC) or an Xbox 360 content package
// (STFS, magic CON / LIVE / PIRS: the file as it sits on the console's drive, e.g. copied with FATXplorer; Session 32,
// Jarrod: "possible other people wouldn't have extracted yet"). The game mounts each pack it is given as dlcN: and reads
// dlcN:\info/catalog.dlc (docs\systems\dlc.md). Scan() walks the DLC folder (any depth, so a copied tree works as is)
// and returns every pack with its status. Nothing here writes to the player's files, and a damaged or unknown file is
// reported, never thrown: every read is bounds-checked and every filesystem call takes an error_code.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace wwe2k14::dlc {

namespace fs = std::filesystem;

struct CatalogEntry {
  uint16_t id = 0, type = 0, pack = 0, licence_bit = 0;
  int32_t char_id = -1;
  std::string name;  // UTF-8 (the file holds Windows-1252)
};

struct Catalog {
  uint16_t version = 0, pack_id = 0;
  std::vector<CatalogEntry> entries;
  std::vector<std::string> files;  // relative to the pack folder, '\' separators as in the file
};

enum class Status { kReady, kIncomplete, kDamaged, kDuplicate, kOtherContent };

inline const char* StatusName(Status s) {
  switch (s) {
    case Status::kReady: return "ready";
    case Status::kIncomplete: return "incomplete";
    case Status::kDamaged: return "damaged";
    case Status::kDuplicate: return "duplicate";
    case Status::kOtherContent: return "not WWE 2K14 DLC";
  }
  return "?";
}

constexpr uint32_t kTitleId = 0x545408B2;          // WWE 2K14
constexpr uint32_t kMarketplaceContentType = 2;    // XContentType::kMarketplaceContent

struct Pack {
  fs::path root;       // the pack's folder, or the package file (package = true)
  std::string folder;  // relative to the DLC folder, UTF-8 ("." for the folder itself)
  bool package = false;      // an Xbox 360 content package (STFS), read in place
  std::string package_name;  // its own display name (English), UTF-8
  Status status = Status::kDamaged;
  std::string reason;  // why it is not ready
  int pack_id = -1;
  std::vector<CatalogEntry> entries;
  size_t files_listed = 0;
  std::vector<std::string> missing;
  uint64_t bytes = 0;

  // The pack's own names: its bundle entries (type 21), else every entry; UTF-8.
  std::string DisplayName() const {
    std::string out;
    for (int pass = 0; pass < 2 && out.empty(); ++pass)
      for (const auto& e : entries)
        if ((pass == 1 || e.type == 21) && !e.name.empty()) out += (out.empty() ? "" : ", ") + e.name;
    return out.empty() ? Fallback() : out;
  }
  // One name for a list row: a package's own display name, else the first bundle entry, else the first entry.
  std::string ShortName() const {
    if (!package_name.empty()) return package_name;
    for (const auto& e : entries)
      if (e.type == 21 && !e.name.empty()) return e.name;
    return entries.empty() || entries[0].name.empty() ? Fallback() : entries[0].name;
  }
  // No names to show (a damaged catalog, a package): the pack id, else the folder or file name.
  std::string Fallback() const {
    if (pack_id >= 0) return "Pack " + std::to_string(pack_id);
    const size_t slash = folder.find_last_of("\\/");
    return slash == std::string::npos ? folder : folder.substr(slash + 1);
  }
  // Every entry's name.
  std::string AllNames() const {
    std::string out;
    for (const auto& e : entries) out += (out.empty() ? "" : ", ") + e.name;
    return out;
  }
};

constexpr size_t kHeaderSize = 28, kEntrySize = 64;
constexpr size_t kMaxCatalogBytes = 64u << 10;  // the four known catalogs are 288 - 1632 bytes
constexpr size_t kMaxPacks = 64;               // the game enumerates 128 items (sub_8281BE28)

inline uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

// Windows-1252 text (the catalog names) -> UTF-8. The 1252 quotes and dashes the names use (0x91-0x97) become plain
// ' " - (the launcher's and the game's fonts have those), the rest of 0x80-0x9F '?'.
inline std::string Cp1252ToUtf8(const uint8_t* s, size_t n) {
  static const char k91[7] = {'\'', '\'', '"', '"', '?', '-', '-'};
  std::string out;
  for (size_t i = 0; i < n && s[i]; ++i) {
    uint32_t c = s[i];
    if (c >= 0x91 && c <= 0x97) c = uint8_t(k91[c - 0x91]);
    else if (c >= 0x80 && c < 0xA0) c = '?';
    if (c < 0x80) out += char(c < 0x20 ? '?' : c);
    else out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));  // U+00A0-U+00FF
  }
  return out;
}

// The catalog ends with `count` records: u16 record size, s16, s16, u8 name length, name (size = 7 + length). The
// tables between the entries and the list are not decoded: find the start where `count` records chain and only zero
// padding follows (the same rule as tools\dlc\scan.py). Review (Session 32): a chain is checked without allocating and
// must reach the last non-zero byte, and only kMaxListStarts start offsets are tried (the four real catalogs' tables
// are under 300 bytes), so a crafted catalog cannot make this take long.
constexpr size_t kMaxListStarts = 4096;
inline bool FindFileList(const std::vector<uint8_t>& d, size_t start, size_t count, std::vector<std::string>& out) {
  out.clear();
  if (count == 0) return true;
  size_t end_nz = d.size();  // one past the last non-zero byte
  while (end_nz > 0 && d[end_nz - 1] == 0) --end_nz;
  auto chain_end = [&](size_t p) -> size_t {  // where `count` records starting at p end, or 0
    size_t q = p;
    for (size_t k = 0; k < count; ++k) {
      if (q + 7 > d.size()) return 0;
      const size_t size = U16(&d[q]), n = d[q + 6];
      if (size != 7 + n || n == 0 || q + size > d.size()) return 0;
      for (size_t i = 0; i < n; ++i)
        if (d[q + 7 + i] < 32 || d[q + 7 + i] >= 127) return 0;
      q += size;
    }
    return q;
  };
  for (size_t p = start, tries = 0; p + 7 <= d.size() && tries < kMaxListStarts; ++p, ++tries) {
    const size_t q = chain_end(p);
    if (q == 0 || q < end_nz) continue;  // not a chain, or non-zero bytes after it
    for (size_t k = 0, r = p; k < count; ++k) {
      const size_t n = d[r + 6];
      out.emplace_back(reinterpret_cast<const char*>(&d[r + 7]), n);
      r += 7 + n;
    }
    return true;
  }
  return false;
}

inline bool ParseCatalog(const std::vector<uint8_t>& d, Catalog& c, std::string& why) {
  if (d.size() < kHeaderSize || std::memcmp(d.data(), "DLCC", 4) != 0) return why = "not a DLC catalog", false;
  c.version = U16(&d[4]);
  if (U16(&d[6]) != kHeaderSize) return why = "unknown catalog layout", false;
  c.pack_id = U16(&d[8]);
  const size_t n_entries = U16(&d[12]), n_files = U16(&d[18]);
  if (kHeaderSize + kEntrySize * n_entries > d.size()) return why = "catalog cut short", false;
  c.entries.clear();
  for (size_t i = 0; i < n_entries; ++i) {
    const uint8_t* e = &d[kHeaderSize + kEntrySize * i];
    if (U16(e) != kEntrySize) return why = "catalog entry damaged", false;
    CatalogEntry ce;
    ce.id = U16(e + 2), ce.type = U16(e + 4), ce.pack = U16(e + 6), ce.licence_bit = U16(e + 8);
    ce.char_id = int32_t(uint32_t(e[16]) | (uint32_t(e[17]) << 8) | (uint32_t(e[18]) << 16) | (uint32_t(e[19]) << 24));
    ce.name = Cp1252ToUtf8(e + 32, 32);
    c.entries.push_back(std::move(ce));
  }
  if (!FindFileList(d, kHeaderSize + kEntrySize * n_entries, n_files, c.files)) return why = "file list damaged", false;
  return true;
}

// Never throws (review: u8string() throws on a folder name with an unpaired UTF-16 surrogate, which NTFS allows).
inline std::string PathUtf8(const fs::path& p) {
  try {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
  } catch (...) {
    return "(a name that can't be shown)";
  }
}

inline bool ReadSmallFile(const fs::path& p, std::vector<uint8_t>& out, size_t max_bytes) {
  std::error_code ec;
  const auto size = fs::file_size(p, ec);
  if (ec || size > max_bytes) return false;
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.resize(size_t(size));
  f.read(reinterpret_cast<char*>(out.data()), std::streamsize(size));
  return bool(f) || size == 0;
}

// "CON " / "LIVE" / "PIRS": an Xbox 360 content package that has not been extracted.
inline bool IsStfsPackage(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  char m[4] = {};
  if (!f.read(m, 4)) return false;
  return !std::memcmp(m, "CON ", 4) || !std::memcmp(m, "LIVE", 4) || !std::memcmp(m, "PIRS", 4);
}

inline uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
inline uint32_t Le24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }

// An Xbox 360 STFS content package, read in place (Session 32). A port of ReXGlue's StfsContainerDevice
// (thirdparty\rexglue-sdk-src\src\filesystem\devices\stfs_container_device.cpp: ReadHeaderAndVerify, ReadSTFS,
// BlockToOffsetSTFS, BlockToHashBlockNumberSTFS, GetBlockHash; the layout in stfs_xbox.h), with every read
// bounds-checked. Open() walks the whole directory and every file's block chain exactly as ReXGlue's device will when
// the game mounts the package, so a package that passes here cannot send that device past the end of the file or out
// of its directory list (review: ReXGlue dereferences a failed hash-table read and indexes the directory unchecked).
// SVOD (multi-file) packages are refused: DLC is STFS.
class StfsPackage {
 public:
  struct File {
    std::string path;  // "pac\ch\x.pac" (backslashes, as the game asks)
    bool dir = false;
    uint32_t length = 0;
    std::vector<uint64_t> block_offsets;  // file offset of each 4 KB data block, in order
  };

  uint32_t content_type = 0, title_id = 0;
  std::string display_name;  // English, UTF-8
  std::vector<File> files;

  bool svod = false;  // a multi-file package (not supported)

  // The header only: magic, title, content type, display name, the volume descriptor. Cheap: Scan checks the title
  // and content type before Walk() reads anything else (review: other games' packages are not walked).
  bool OpenHeader(const fs::path& path, std::string& why) {
    std::error_code ec;
    size_ = fs::file_size(path, ec);
    if (ec) return why = "unreadable", false;
    in_.open(path, std::ios::binary);
    if (!in_) return why = "unreadable", false;
    constexpr size_t kHeader = 0x971A;  // sizeof(StfsHeader)
    std::vector<uint8_t> h(kHeader);
    if (size_ < kHeader || !ReadAt(0, h.data(), kHeader)) return why = "too small for a package", false;
    if (std::memcmp(h.data(), "CON ", 4) && std::memcmp(h.data(), "LIVE", 4) && std::memcmp(h.data(), "PIRS", 4))
      return why = "not a package", false;
    const uint32_t header_size = Be32(&h[0x340]);
    content_type = Be32(&h[0x344]);
    title_id = Be32(&h[0x360]);
    for (size_t i = 0; i < 128; ++i) {  // display name, English slot (UTF-16BE at 0x411)
      uint32_t c = uint32_t(h[0x411 + 2 * i]) << 8 | h[0x412 + 2 * i];
      if (!c) break;
      if (c >= 0xD800 && c < 0xE000) c = '?';  // a surrogate (review: not valid on its own in UTF-8)
      if (c < 0x80) display_name += char(c < 0x20 ? '?' : c);
      else if (c < 0x800) display_name += char(0xC0 | (c >> 6)), display_name += char(0x80 | (c & 0x3F));
      else display_name += char(0xE0 | (c >> 12)), display_name += char(0x80 | ((c >> 6) & 0x3F)), display_name += char(0x80 | (c & 0x3F));
    }
    svod = Be32(&h[0x3A9]) != 0 || Be32(&h[0x39D]) > 1;
    const uint8_t* vd = &h[0x379];  // StfsVolumeDescriptor
    read_only_ = vd[2] & 1, root_active_ = (vd[2] & 2) != 0;
    ft_count_ = uint32_t(vd[3]) | uint32_t(vd[4]) << 8;
    first_table_block_ = Le24(&vd[5]);
    total_blocks_ = Be32(&vd[0x1C]);
    bpht_ = read_only_ ? 1 : 2;
    step0_ = kL0 + bpht_;
    step1_ = kL1 + (kL0 + 1) * bpht_;
    base_ = (uint64_t(header_size) + 0xFFF) & ~uint64_t(0xFFF);
    // header_size is capped (review): ReXGlue rounds it in 32 bits, so a huge value would wrap there and not here.
    if (!svod && (vd[0] != 0x24 || header_size < kHeader || header_size > 0x100000 || base_ >= size_ ||
                  ft_count_ == 0 || ft_count_ > 4096 || total_blocks_ > kL2))
      return why = "damaged package header", false;
    return true;
  }

  // The directory and every file's block chain, as ReXGlue's device will read them when it mounts the package.
  // Review hardening: each block may be visited once in the whole package (a looping or shared chain is refused at
  // once), a parent must be a directory, at most 16 levels and 260-character paths.
  bool Walk(std::string& why) {
    if (svod) return why = "a multi-file (SVOD) package: not supported", false;
    std::vector<bool> visited(total_blocks_, false);
    auto visit = [&](uint32_t block) {
      if (block >= total_blocks_ || visited[block]) return false;
      visited[block] = true;
      return true;
    };
    std::vector<uint8_t> depth;  // per entry
    uint32_t table_block = first_table_block_;
    std::array<uint8_t, kBlock> dir{};
    for (uint32_t n = 0; n < ft_count_; ++n) {
      if (!visit(table_block) || !ReadAt(BlockOffset(table_block), dir.data(), kBlock))
        return why = "damaged package directory", false;
      for (size_t m = 0; m < kBlock / 0x40; ++m) {
        const uint8_t* e = &dir[m * 0x40];
        if (e[0] == 0) break;
        const size_t name_len = e[0x28] & 0x3F;
        const uint32_t parent = uint32_t(e[0x32]) << 8 | e[0x33];
        if (name_len == 0 || name_len > 40 || files.size() >= 65535 ||
            (parent != 0xFFFF && (parent >= files.size() || !files[parent].dir || depth[parent] >= 16)))
          return why = "damaged package directory", false;
        File f;
        f.dir = (e[0x28] & 0x80) != 0;
        f.path = (parent == 0xFFFF ? std::string() : files[parent].path + "\\") +
                 std::string(reinterpret_cast<const char*>(e), name_len);
        if (f.path.size() > 260) return why = "damaged package directory", false;
        if (!f.dir) {
          f.length = Be32(&e[0x34]);
          uint32_t block = Le24(&e[0x2F]);
          size_t remaining = f.length;
          while (remaining && block != kEnd) {  // ReXGlue's chain walk
            const uint64_t off = BlockOffset(block);
            const size_t n_bytes = std::min<size_t>(kBlock, remaining);
            if (!visit(block) || off + n_bytes > size_) return why = "damaged package data", false;
            f.block_offsets.push_back(off);
            remaining -= n_bytes;
            if (!NextBlock(block, block)) return why = "damaged package hash tables", false;
          }
          if (remaining) return why = "damaged package data", false;
        }
        depth.push_back(uint8_t(parent == 0xFFFF ? 0 : depth[parent] + 1));
        files.push_back(std::move(f));
      }
      if (!NextBlock(table_block, table_block)) return why = "damaged package hash tables", false;
      if (table_block == kEnd) break;
    }
    return true;
  }

  bool Open(const fs::path& path, std::string& why) { return OpenHeader(path, why) && Walk(why); }

  const File* Find(std::string path) const {
    std::replace(path.begin(), path.end(), '/', '\\');
    for (const auto& f : files)
      if (!f.dir && f.path.size() == path.size() &&
          std::equal(f.path.begin(), f.path.end(), path.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
          }))
        return &f;
    return nullptr;
  }

  bool Read(const File& f, std::vector<uint8_t>& out, size_t max_bytes) {
    if (f.length > max_bytes) return false;
    out.resize(f.length);
    size_t done = 0;
    for (uint64_t off : f.block_offsets) {
      const size_t n = std::min<size_t>(kBlock, f.length - done);
      if (!ReadAt(off, out.data() + done, n)) return false;
      done += n;
    }
    return done == f.length;
  }

 private:
  static constexpr uint32_t kBlock = 0x1000, kEnd = 0xFFFFFF, kL0 = 170, kL1 = 28900, kL2 = 4913000;

  bool ReadAt(uint64_t off, void* buf, size_t n) {
    if (off > size_ || n > size_ - off) return false;
    in_.clear();
    in_.seekg(std::streamoff(off));
    return bool(in_.read(static_cast<char*>(buf), std::streamsize(n)));
  }
  uint64_t BlockOffset(uint64_t block_index) const {
    uint64_t base = kL0, block = block_index;
    for (int i = 0; i < 3; ++i) {
      block += ((block_index + base) / base) * bpht_;
      if (block_index < base) break;
      base *= kL0;
    }
    return base_ + (block << 12);
  }
  uint32_t HashBlockNumber(uint32_t block_index, int level) const {
    if (level == 0) {
      if (block_index < kL0) return 0;
      uint32_t block = (block_index / kL0) * step0_;
      block += ((block_index / kL1) + 1) * bpht_;
      return block_index < kL1 ? block : block + bpht_;
    }
    if (level == 1) return block_index < kL1 ? step0_ : (block_index / kL1) * step1_ + bpht_;
    return step1_;
  }
  const uint8_t* Table(uint64_t key, uint64_t read_at) {  // ReXGlue caches by the primary offset
    auto it = tables_.find(key);
    if (it != tables_.end()) return it->second.data();
    if (tables_.size() > 4096) return nullptr;
    std::array<uint8_t, kBlock> t{};
    if (!ReadAt(read_at, t.data(), kBlock)) return nullptr;
    return tables_.emplace(key, t).first->second.data();
  }
  static bool Active(const uint8_t* table, uint32_t record) { return (Be32(&table[record * 0x18 + 0x14]) & 0x40000000) != 0; }
  // ReXGlue's GetBlockHash, then level0_next_block(); false where ReXGlue's read would fail.
  bool NextBlock(uint32_t block_index, uint32_t& next) {
    uint64_t secondary = root_active_ ? kBlock : 0;
    const uint64_t off0 = base_ + (uint64_t(HashBlockNumber(block_index, 0)) << 12);
    if (!tables_.count(off0)) {
      if (read_only_) {
        secondary = 0;
      } else if (total_blocks_ > kL0) {
        const uint64_t off1 = base_ + (uint64_t(HashBlockNumber(block_index, 1)) << 12);
        if (!tables_.count(off1)) {
          if (total_blocks_ > kL1) {
            const uint64_t off2 = base_ + (uint64_t(HashBlockNumber(block_index, 2)) << 12);
            const uint8_t* t2 = Table(off2, off2 + secondary);
            if (!t2) return false;
            secondary = Active(t2, (block_index / kL1) % kL0) ? kBlock : 0;
          }
          if (!Table(off1, off1 + secondary)) return false;
        }
        secondary = Active(tables_[off1].data(), (block_index / kL0) % kL0) ? kBlock : 0;
      }
      if (!Table(off0, off0 + secondary)) return false;
    }
    next = Be32(&tables_[off0][(block_index % kL0) * 0x18 + 0x14]) & 0xFFFFFF;
    return true;
  }

  std::ifstream in_;
  uint64_t size_ = 0, base_ = 0;
  uint32_t bpht_ = 1, step0_ = 0, step1_ = 0, total_blocks_ = 0, ft_count_ = 0, first_table_block_ = 0;
  bool read_only_ = true, root_active_ = false;
  std::map<uint64_t, std::array<uint8_t, kBlock>> tables_;
};

inline bool IEquals(const std::string& a, const char* b);
inline bool LooksLikePackageName(const fs::path& p) {
  if (!p.has_extension()) return true;
  const std::string ext = PathUtf8(p.extension());
  for (const char* e : {".bin", ".stfs", ".con", ".live", ".pirs", ".pkg"})
    if (IEquals(ext, e)) return true;
  return false;
}

inline bool IEquals(const std::string& a, const char* b) {
  const size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// Every pack under `dir`, sorted by pack id (then folder). check_files: test that every file the catalog lists is
// there (a few hundred file checks); bytes: add up each pack's size.
inline std::vector<Pack> Scan(const fs::path& dir, bool check_files = true, bool bytes = true) {
  std::vector<Pack> packs;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return packs;
  std::vector<fs::path> catalogs, packages;
  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
  for (size_t visited = 0; !ec && it != end && visited < 20000; it.increment(ec), ++visited) {  // a pack: ~45 files
    if (it.depth() > 12) {
      it.disable_recursion_pending();
      continue;
    }
    std::error_code fe;
    if (!it->is_regular_file(fe)) continue;
    const fs::path& p = it->path();
    if (IEquals(PathUtf8(p.filename()), "catalog.dlc") && IEquals(PathUtf8(p.parent_path().filename()), "info"))
      catalogs.push_back(p);
    // Packages are probed only when they have no extension, as the console and FATXplorer name them (a long hex id),
    // or a package-ish one: a wrong folder full of big game files then costs no file opens (review: the launcher
    // scans on its UI thread).
    else if (packages.size() < kMaxPacks && LooksLikePackageName(p) && it->file_size(fe) >= 0xA000 && !fe &&
             IsStfsPackage(p))
      packages.push_back(p);
  }
  for (const auto& cat : catalogs) {
    if (packs.size() >= kMaxPacks) break;
    Pack pk;
    pk.root = cat.parent_path().parent_path();
    std::error_code re;
    const fs::path rel = fs::relative(pk.root, dir, re);
    pk.folder = re || rel.empty() ? PathUtf8(pk.root) : PathUtf8(rel);
    std::vector<uint8_t> data;
    Catalog c;
    if (!ReadSmallFile(cat, data, kMaxCatalogBytes)) {
      pk.status = Status::kDamaged, pk.reason = "catalog unreadable";
    } else if (!ParseCatalog(data, c, pk.reason)) {
      pk.status = Status::kDamaged;
    } else {
      pk.pack_id = c.pack_id;
      pk.entries = std::move(c.entries);
      pk.files_listed = c.files.size();
      if (check_files) {
        for (std::string f : c.files) {
          std::replace(f.begin(), f.end(), '\\', '/');
          // A listed path must stay inside the pack (review: "C:/..." or "//server/..." would replace the root).
          if (f.find(':') != std::string::npos || f[0] == '/' || f.find("..") != std::string::npos) {
            pk.missing.push_back(f);
            continue;
          }
          std::error_code fe;
          const std::u8string u(f.begin(), f.end());
          if (!fs::is_regular_file(pk.root / fs::path(u), fe)) pk.missing.push_back(f);
        }
      }
      pk.status = pk.missing.empty() ? Status::kReady : Status::kIncomplete;
      if (!pk.missing.empty()) pk.reason = std::to_string(pk.missing.size()) + " file(s) missing";
    }
    if (bytes) {
      std::error_code be;
      size_t seen = 0;  // the same caps as the first walk (review)
      for (fs::recursive_directory_iterator f(pk.root, fs::directory_options::skip_permission_denied, be);
           !be && f != fs::recursive_directory_iterator() && seen < 20000; f.increment(be), ++seen) {
        if (f.depth() > 12) {
          f.disable_recursion_pending();
          continue;
        }
        std::error_code se;
        if (f->is_regular_file(se)) pk.bytes += f->file_size(se);
      }
    }
    packs.push_back(std::move(pk));
  }
  for (const auto& p : packages) {
    if (packs.size() >= kMaxPacks) break;
    Pack pk;
    pk.root = p;
    pk.package = true;
    std::error_code re;
    const fs::path rel = fs::relative(p, dir, re);
    pk.folder = re ? PathUtf8(p) : PathUtf8(rel);
    std::error_code se;
    pk.bytes = fs::file_size(p, se);
    StfsPackage pkg;
    std::vector<uint8_t> data;
    Catalog c;
    const StfsPackage::File* cat = nullptr;
    if (!pkg.OpenHeader(p, pk.reason)) {
      pk.status = Status::kDamaged;
    } else if (pkg.title_id != kTitleId || pkg.content_type != kMarketplaceContentType) {  // before any walk (review)
      pk.package_name = pkg.display_name;
      pk.status = Status::kOtherContent;
      char buf[96];
      if (pkg.title_id != kTitleId) std::snprintf(buf, sizeof(buf), "for another game (title %08X)", pkg.title_id);
      else std::snprintf(buf, sizeof(buf), "WWE 2K14 content, but not DLC (content type %X)", pkg.content_type);
      pk.reason = buf;
    } else if (!pkg.Walk(pk.reason)) {
      pk.package_name = pkg.display_name;
      pk.status = Status::kDamaged;
    } else if (!(cat = pkg.Find("info\\catalog.dlc")) || !pkg.Read(*cat, data, kMaxCatalogBytes)) {
      pk.package_name = pkg.display_name;
      pk.status = Status::kDamaged, pk.reason = "no catalog inside the package";
    } else if (!ParseCatalog(data, c, pk.reason)) {
      pk.package_name = pkg.display_name;
      pk.status = Status::kDamaged;
    } else {
      pk.package_name = pkg.display_name;
      pk.pack_id = c.pack_id;
      pk.entries = std::move(c.entries);
      pk.files_listed = c.files.size();
      if (check_files)
        for (const auto& f : c.files)
          if (!pkg.Find(f)) pk.missing.push_back(f);
      pk.status = pk.missing.empty() ? Status::kReady : Status::kIncomplete;
      if (!pk.missing.empty()) pk.reason = std::to_string(pk.missing.size()) + " file(s) missing";
    }
    packs.push_back(std::move(pk));
  }
  std::stable_sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) {
    const int ka = a.pack_id < 0 ? 1 << 20 : a.pack_id, kb = b.pack_id < 0 ? 1 << 20 : b.pack_id;
    return ka != kb ? ka < kb : a.folder < b.folder;
  });
  for (size_t i = 1; i < packs.size(); ++i)
    for (size_t j = 0; j < i; ++j)
      if (packs[i].pack_id >= 0 && packs[i].pack_id == packs[j].pack_id && packs[j].status != Status::kDuplicate &&
          packs[i].status != Status::kDuplicate) {
        packs[i].status = Status::kDuplicate, packs[i].reason = "same pack as " + packs[j].folder;
        break;
      }
  return packs;
}

// The DLC folder when none is set: `dlc` next to the game data folder (game_data\dlc in the dev layout; for a disc image
// X.iso, beside it), else `dlc` next to the game exe / launcher (a release folder), whichever exists first; when
// neither exists, the first. The game (src\dlc.h) and the launcher use this one rule.
inline fs::path DefaultDir(const fs::path& game_data_root, const fs::path& exe_dir) {
  std::vector<fs::path> c;
  if (!game_data_root.empty()) c.push_back(game_data_root.parent_path() / "dlc");
  if (!exe_dir.empty()) c.push_back(exe_dir / "dlc");
  for (const auto& p : c) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return p;
  }
  return c.empty() ? fs::path() : c.front();
}

// "3,5" -> {3, 5}: the pack ids the player switched off.
inline std::vector<int> ParseIdList(const std::string& s) {
  std::vector<int> ids;
  int v = 0;
  bool any = false;
  for (char ch : s + ",") {
    if (ch >= '0' && ch <= '9') {
      v = v * 10 + (ch - '0'), any = true;
      if (v > 65535) v = 65535;
    } else {
      if (any) ids.push_back(v);
      v = 0, any = false;
    }
  }
  return ids;
}

}  // namespace wwe2k14::dlc
