#pragma once
// Session 26: the game files as the launcher sees them, from a folder or an Xbox 360 disc image (.iso). The game
// itself mounts either (ReXGlue's DiscImageDevice); the launcher only needs a few reads: default.xex for the version
// check, Bink headers of the original trons, the titantron folder's file names.
//
// Disc image layout (as DiscImageDevice reads it, from Xenia; tools\iso\make_xiso.py writes the same): 2048-byte
// sectors; at sector 32 of the game partition "MICROSOFT*XBOX*MEDIA", the root table's sector and size; a table is a
// binary tree of entries (left / right links in 4-byte units, sector, size, attributes, name length, name), all
// little-endian. The game partition starts at one of a few known offsets (0 for a trimmed image).

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace launcher {

namespace fs = std::filesystem;

class DiscImage {
 public:
  struct Entry {
    std::string name;
    bool dir = false;
    uint64_t offset = 0, size = 0;  // file data in the image
    std::vector<std::unique_ptr<Entry>> children;
  };

  // False with `error` set (plain English) if this isn't a readable disc image.
  bool Open(const fs::path& path, std::string& error) {
    f_.open(path, std::ios::binary);
    std::error_code ec;
    size_ = fs::file_size(path, ec);
    if (!f_ || ec) return error = "can't open the file", false;
    static constexpr uint64_t kOffsets[] = {0x00000000, 0x0000FB20, 0x00020600, 0x02080000, 0x0FD90000};
    uint8_t head[28];
    bool found = false;
    for (uint64_t off : kOffsets) {
      if (Read(off + 32 * kSector, head, sizeof(head)) && std::memcmp(head, "MICROSOFT*XBOX*MEDIA", 20) == 0) {
        base_ = off, found = true;
        break;
      }
    }
    if (!found) return error = "not an Xbox 360 disc image", false;
    const uint32_t root_sector = Le32(head + 20), root_size = Le32(head + 24);
    root_.dir = true;
    if (!ReadTable(root_sector, root_size, root_, 0)) return error = "the disc image is damaged", false;
    if (past_end_) {
      error = "the disc image is incomplete (" + std::to_string(past_end_) + " of " + std::to_string(files_) +
              " files are missing from its end): copy it again";
      return false;
    }
    return true;
  }

  // Path inside the image, '/' or '\' separated, any case; nullptr if missing.
  const Entry* Find(const std::string& path) const {
    const Entry* e = &root_;
    size_t i = 0;
    while (i < path.size()) {
      size_t j = path.find_first_of("/\\", i);
      if (j == std::string::npos) j = path.size();
      if (j > i) {
        const std::string part = path.substr(i, j - i);
        const Entry* next = nullptr;
        for (const auto& c : e->children)
          if (EqualNoCase(c->name, part)) next = c.get();
        if (!next) return nullptr;
        e = next;
      }
      i = j + 1;
    }
    return e;
  }

  bool ReadFile(const Entry& e, uint64_t offset, void* out, size_t n) {
    if (e.dir || offset + n > e.size) return false;
    return Read(e.offset + offset, out, n);
  }

  uint64_t game_offset() const { return base_; }

 private:
  static constexpr uint64_t kSector = 2048;

  static uint32_t Le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }
  static uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
  static bool EqualNoCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
             return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
  }

  bool Read(uint64_t pos, void* out, size_t n) {
    if (pos + n > size_) return false;
    f_.clear();
    f_.seekg(std::streamoff(pos));
    return bool(f_.read(static_cast<char*>(out), std::streamsize(n)));
  }

  // A damaged image is refused, never looped over: every table is read once, every entry visited once, the depth
  // (tree links + folder nesting together) stays under 1024 and all tables together under 64 MiB. Real trees are
  // balanced and shallow (the same rules as ReXGlue's DiscImageDevice, Session 26 review).
  bool ReadTable(uint32_t sector, uint32_t size, Entry& parent, int depth) {
    if (!size) return true;
    const uint64_t pos = base_ + uint64_t(sector) * kSector;
    table_bytes_ += size;
    if (size > (32u << 20) || table_bytes_ > (64u << 20) || depth > kMaxDepth) return false;
    if (std::find(tables_.begin(), tables_.end(), pos) != tables_.end()) return false;  // a table used twice
    tables_.push_back(pos);
    std::vector<uint8_t> t(size);
    if (!Read(pos, t.data(), t.size())) return false;
    if (size >= 4 && Le32(t.data()) == 0xFFFFFFFFu) return true;  // an empty folder stored as padding
    std::vector<uint8_t> visited(size / 4 + 1, 0);
    return ReadEntry(t, visited, 0, parent, depth);
  }

  bool ReadEntry(const std::vector<uint8_t>& t, std::vector<uint8_t>& visited, uint32_t ordinal, Entry& parent,
                 int depth) {
    const size_t p = size_t(ordinal) * 4;
    if (depth > kMaxDepth || p + 14 > t.size() || p + 14 + t[p + 13] > t.size() || visited[ordinal]) return false;
    visited[ordinal] = 1;
    const uint16_t left = Le16(&t[p]), right = Le16(&t[p + 2]);
    const uint32_t sector = Le32(&t[p + 4]), length = Le32(&t[p + 8]);
    const uint8_t attr = t[p + 12], name_len = t[p + 13];
    if (left && !ReadEntry(t, visited, left, parent, depth + 1)) return false;
    auto e = std::make_unique<Entry>();
    e->name.assign(reinterpret_cast<const char*>(&t[p + 14]), name_len);
    e->dir = (attr & 0x10) != 0;
    if (e->dir) {
      if (!ReadTable(sector, length, *e, depth + 1)) return false;
    } else {
      e->offset = base_ + uint64_t(sector) * kSector, e->size = length;
      ++files_;
      if (length && e->offset + e->size > size_) ++past_end_;  // a zero-length file needs no data
    }
    parent.children.push_back(std::move(e));
    return !right || ReadEntry(t, visited, right, parent, depth + 1);
  }

  static constexpr int kMaxDepth = 1024;
  std::ifstream f_;
  uint64_t size_ = 0, base_ = 0;
  uint64_t files_ = 0, past_end_ = 0, table_bytes_ = 0;
  std::vector<uint64_t> tables_;
  Entry root_;
};

// The game files: a folder (the usual copy) or a disc image file. Paths are relative ("movies\\titantron").
class GameFiles {
 public:
  // Re-opens only when the path changes.
  void SetRoot(const fs::path& root) {
    if (root == root_ && opened_) return;
    root_ = root, opened_ = true, image_.reset(), error_.clear();
    std::error_code ec;
    if (!root.empty() && fs::is_regular_file(root, ec)) {
      image_ = std::make_unique<DiscImage>();
      if (!image_->Open(root, error_)) image_.reset();
    }
  }
  // Opens the same path again (the file may have been replaced since).
  void Reopen() {
    const fs::path root = root_;
    opened_ = false;
    SetRoot(root);
  }
  const fs::path& root() const { return root_; }
  bool is_image() const {
    std::error_code ec;
    return !root_.empty() && fs::is_regular_file(root_, ec);
  }
  // Set when the root is a file that can't be read as a disc image.
  const std::string& image_error() const { return error_; }

  bool Exists(const std::string& rel) const {
    if (is_image()) return image_ && image_->Find(rel);
    std::error_code ec;
    return !root_.empty() && fs::exists(root_ / rel, ec);
  }

  // Up to n bytes from the start; empty if missing.
  std::vector<uint8_t> ReadHead(const std::string& rel, size_t n) {
    std::vector<uint8_t> out;
    if (is_image()) {
      const DiscImage::Entry* e = image_ ? image_->Find(rel) : nullptr;
      if (!e || e->dir) return out;
      out.resize(size_t(std::min<uint64_t>(n, e->size)));
      if (!image_->ReadFile(*e, 0, out.data(), out.size())) out.clear();
      return out;
    }
    std::ifstream f(root_ / rel, std::ios::binary);
    out.resize(n);
    f.read(reinterpret_cast<char*>(out.data()), std::streamsize(n));
    out.resize(size_t(std::max<std::streamsize>(0, f.gcount())));
    return out;
  }

  // Calls `sink` with the whole file in chunks; false if missing or a read failed.
  bool Stream(const std::string& rel, const std::function<void(const uint8_t*, size_t)>& sink) {
    std::vector<uint8_t> buf(1 << 20);
    if (is_image()) {
      const DiscImage::Entry* e = image_ ? image_->Find(rel) : nullptr;
      if (!e || e->dir) return false;
      for (uint64_t pos = 0; pos < e->size;) {
        const size_t n = size_t(std::min<uint64_t>(buf.size(), e->size - pos));
        if (!image_->ReadFile(*e, pos, buf.data(), n)) return false;
        sink(buf.data(), n);
        pos += n;
      }
      return true;
    }
    std::ifstream f(root_ / rel, std::ios::binary);
    if (!f) return false;
    while (f) {
      f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
      if (f.gcount() > 0) sink(buf.data(), size_t(f.gcount()));
    }
    return !f.bad();  // end of file sets only eof / fail; a read error sets bad
  }

  // File names (not folders) in a folder of the game files.
  std::vector<std::string> List(const std::string& rel_dir) const {
    std::vector<std::string> names;
    if (is_image()) {
      const DiscImage::Entry* d = image_ ? image_->Find(rel_dir) : nullptr;
      if (d && d->dir)
        for (const auto& c : d->children)
          if (!c->dir) names.push_back(c->name);
      return names;
    }
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root_ / rel_dir, ec))
      if (e.is_regular_file(ec)) names.push_back(e.path().filename().string());
    return names;
  }

 private:
  fs::path root_;
  bool opened_ = false;
  std::unique_ptr<DiscImage> image_;
  std::string error_;
};

}  // namespace launcher
