#pragma once
// Session 73 (Jarrod: SHARE THE LAST GAME LOG sends ONE file): a small ZIP writer for the phone launcher's report.
// Store method only (no compression: logs are a few MB and every unzip tool reads stored entries), CRC-32, a local
// header per entry, the central directory and its end record. No ZIP64: an entry or the archive over 4 GB fails.
// Each file is copied in pieces; its local header's CRC and sizes are filled in after the copy (seek back).
#include <array>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mlauncher::zip {

inline uint32_t Crc32(uint32_t crc, const void* data, size_t size) {
  static const std::array<uint32_t, 256> kTable = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  const auto* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < size; ++i) crc = kTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

class Writer {
 public:
  // Opens (truncates) the archive. ok() says whether it could.
  explicit Writer(const std::filesystem::path& path) : out_(path, std::ios::binary | std::ios::trunc) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    dos_time_ = uint16_t((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
    dos_date_ = uint16_t(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
  }
  bool ok() const { return bool(out_) && !failed_; }

  // An entry from memory.
  bool AddText(const std::string& name, const std::string& data) {
    if (!ok()) return false;
    Entry e = Begin(name);
    out_.write(data.data(), std::streamsize(data.size()));
    e.crc = Crc32(0, data.data(), data.size());
    e.size = uint64_t(data.size());
    return End(e);
  }

  // An entry from a file on disk (false and no entry when it cannot be read).
  bool AddFile(const std::string& name, const std::filesystem::path& file) {
    if (!ok()) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    Entry e = Begin(name);
    std::vector<char> buf(1 << 16);
    while (in) {
      in.read(buf.data(), std::streamsize(buf.size()));
      const std::streamsize got = in.gcount();
      if (got <= 0) break;
      out_.write(buf.data(), got);
      e.crc = Crc32(e.crc, buf.data(), size_t(got));
      e.size += uint64_t(got);
    }
    return End(e);
  }

  // Writes the central directory; the archive is complete once this returns true.
  bool Finish() {
    if (!ok()) return false;
    const uint64_t dir_at = uint64_t(out_.tellp());
    for (const Entry& e : entries_) {
      U32(0x02014b50);
      U16(20), U16(20), U16(0x0800), U16(0);  // made by 2.0, needs 2.0, UTF-8 names, stored
      U16(dos_time_), U16(dos_date_);
      U32(e.crc), U32(uint32_t(e.size)), U32(uint32_t(e.size));
      U16(uint16_t(e.name.size())), U16(0), U16(0), U16(0), U16(0);  // extra, comment, disk, internal attributes
      U32(0), U32(uint32_t(e.header_at));
      out_.write(e.name.data(), std::streamsize(e.name.size()));
    }
    const uint64_t dir_size = uint64_t(out_.tellp()) - dir_at;
    if (dir_at > 0xFFFFFFFFull || entries_.size() > 0xFFFF) failed_ = true;
    U32(0x06054b50);
    U16(0), U16(0), U16(uint16_t(entries_.size())), U16(uint16_t(entries_.size()));
    U32(uint32_t(dir_size)), U32(uint32_t(dir_at)), U16(0);
    out_.flush();
    const bool good = ok();
    out_.close();
    return good;
  }

  size_t count() const { return entries_.size(); }

 private:
  struct Entry {
    std::string name;
    uint64_t header_at = 0;
    uint32_t crc = 0;
    uint64_t size = 0;
  };

  void U16(uint16_t v) {
    const char b[2] = {char(v & 0xFF), char(v >> 8)};
    out_.write(b, 2);
  }
  void U32(uint32_t v) {
    const char b[4] = {char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char(v >> 24)};
    out_.write(b, 4);
  }

  Entry Begin(const std::string& name) {
    Entry e;
    e.name = name;
    e.header_at = uint64_t(out_.tellp());
    U32(0x04034b50);
    U16(20), U16(0x0800), U16(0);  // needs 2.0, UTF-8 names, stored
    U16(dos_time_), U16(dos_date_);
    U32(0), U32(0), U32(0);  // CRC and sizes: End fills them in
    U16(uint16_t(e.name.size())), U16(0);
    out_.write(e.name.data(), std::streamsize(e.name.size()));
    return e;
  }

  bool End(const Entry& e) {
    if (e.size > 0xFFFFFFFFull || e.header_at > 0xFFFFFFFFull) failed_ = true;
    const std::streampos end = out_.tellp();
    out_.seekp(std::streamoff(e.header_at + 14));
    U32(e.crc), U32(uint32_t(e.size)), U32(uint32_t(e.size));
    out_.seekp(end);
    if (!ok()) return false;
    entries_.push_back(e);
    return true;
  }

  std::ofstream out_;
  bool failed_ = false;
  uint16_t dos_time_ = 0, dos_date_ = 0;
  std::vector<Entry> entries_;
};

}  // namespace mlauncher::zip
