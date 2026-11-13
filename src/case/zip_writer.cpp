#include "zip_writer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <system_error>

namespace rdr_case {

namespace {
using Table8 = std::array<std::array<uint32_t, 256>, 8>;
const Table8& Tables() {
  static const Table8 t = [] {
    Table8 r{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      r[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; ++i)
      for (int s = 1; s < 8; ++s) r[s][i] = (r[s - 1][i] >> 8) ^ r[0][r[s - 1][i] & 0xFF];
    return r;
  }();
  return t;
}

void Le16(std::string& o, uint16_t v) { o.push_back(char(v)); o.push_back(char(v >> 8)); }
void Le32(std::string& o, uint32_t v) { for (int i = 0; i < 4; ++i) o.push_back(char(v >> (8 * i))); }
void Le64(std::string& o, uint64_t v) { for (int i = 0; i < 8; ++i) o.push_back(char(v >> (8 * i))); }
constexpr uint64_t k4G = 0xFFFFFFFFull;
}  // namespace

uint32_t Crc32(uint32_t crc, const void* data, size_t bytes) {
  const Table8& t = Tables();
  const auto* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  while (bytes >= 8) {
    uint32_t a, b;
    std::memcpy(&a, p, 4);
    std::memcpy(&b, p + 4, 4);
    a ^= crc;
    crc = t[7][a & 0xFF] ^ t[6][(a >> 8) & 0xFF] ^ t[5][(a >> 16) & 0xFF] ^ t[4][a >> 24] ^ t[3][b & 0xFF] ^ t[2][(b >> 8) & 0xFF] ^
          t[1][(b >> 16) & 0xFF] ^ t[0][b >> 24];
    p += 8;
    bytes -= 8;
  }
  while (bytes--) crc = t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

ZipWriter::~ZipWriter() { Abort(); }

void ZipWriter::Fail(const std::string& what) {
  if (error_.empty()) error_ = what;
}

bool ZipWriter::Put(const void* data, size_t bytes) {
  if (!file_) return false;
  if (bytes && std::fwrite(data, 1, bytes, file_) != bytes) {
    Fail("short write to " + part_.string());
    return false;
  }
  position_ += bytes;
  return true;
}

bool ZipWriter::Begin(const std::filesystem::path& path) {
  Abort();
  final_ = path;
  part_ = path;
  part_ += ".part";
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
#if defined(_WIN32)
  file_ = _wfopen(part_.c_str(), L"wb");
#else
  file_ = std::fopen(part_.c_str(), "wb");
#endif
  position_ = 0;
  entries_.clear();
  error_.clear();
  in_entry_ = false;
  if (!file_) {
    Fail("cannot create " + part_.string());
    return false;
  }
  return true;
}

bool ZipWriter::BeginEntry(const std::string& name, uint64_t size) {
  if (!file_ || in_entry_ || !error_.empty()) return false;
  Central c;
  c.name = name;
  c.size = size;
  c.offset = position_;
  c.zip64 = size >= k4G;
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &now);
#else
  localtime_r(&now, &tm);
#endif
  c.dos_time = uint16_t((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
  c.dos_date = uint16_t(((std::max(tm.tm_year, 80) - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
  std::string h;
  Le32(h, 0x04034b50);
  Le16(h, c.zip64 ? 45 : 20);
  Le16(h, 0x0008 | 0x0800);  // data descriptor follows; UTF-8 names
  Le16(h, 0);                // stored
  Le16(h, c.dos_time);
  Le16(h, c.dos_date);
  Le32(h, 0);  // crc: in the descriptor
  Le32(h, c.zip64 ? uint32_t(k4G) : 0);
  Le32(h, c.zip64 ? uint32_t(k4G) : 0);
  Le16(h, uint16_t(name.size()));
  Le16(h, c.zip64 ? 20 : 0);
  h += name;
  if (c.zip64) {
    Le16(h, 0x0001);
    Le16(h, 16);
    Le64(h, 0);
    Le64(h, 0);
  }
  if (!Put(h.data(), h.size())) return false;
  entries_.push_back(c);
  in_entry_ = true;
  entry_declared_ = size;
  entry_written_ = 0;
  entry_crc_ = 0;
  return true;
}

bool ZipWriter::Write(const void* data, size_t bytes) {
  if (!in_entry_) return false;
  entry_crc_ = Crc32(entry_crc_, data, bytes);
  entry_written_ += bytes;
  return Put(data, bytes);
}

bool ZipWriter::EndEntry() {
  if (!in_entry_) return false;
  in_entry_ = false;
  Central& c = entries_.back();
  if (entry_written_ != entry_declared_) {
    Fail("entry " + c.name + " declared " + std::to_string(entry_declared_) + " bytes, got " + std::to_string(entry_written_));
    return false;
  }
  c.crc = entry_crc_;
  std::string d;
  Le32(d, 0x08074b50);
  Le32(d, c.crc);
  if (c.zip64) {
    Le64(d, c.size);
    Le64(d, c.size);
  } else {
    Le32(d, uint32_t(c.size));
    Le32(d, uint32_t(c.size));
  }
  return Put(d.data(), d.size());
}

bool ZipWriter::AddBytes(const std::string& name, const void* data, size_t bytes) {
  return BeginEntry(name, bytes) && Write(data, bytes) && EndEntry();
}

bool ZipWriter::Finish() {
  if (!file_) return false;
  if (in_entry_) Fail("Finish inside an entry");
  if (!error_.empty()) {
    Abort();
    return false;
  }
  const uint64_t cd_start = position_;
  for (const Central& c : entries_) {
    const bool big_offset = c.offset >= k4G;
    const bool z = c.zip64 || big_offset;
    std::string e;
    Le32(e, 0x02014b50);
    Le16(e, 45);
    Le16(e, z ? 45 : 20);
    Le16(e, 0x0008 | 0x0800);
    Le16(e, 0);
    Le16(e, c.dos_time);
    Le16(e, c.dos_date);
    Le32(e, c.crc);
    Le32(e, c.zip64 ? uint32_t(k4G) : uint32_t(c.size));
    Le32(e, c.zip64 ? uint32_t(k4G) : uint32_t(c.size));
    Le16(e, uint16_t(c.name.size()));
    uint16_t extra = 0;
    if (z) extra = uint16_t(4 + (c.zip64 ? 16 : 0) + (big_offset ? 8 : 0));
    Le16(e, extra);
    Le16(e, 0);  // comment
    Le16(e, 0);  // disk
    Le16(e, 0);  // internal attrs
    Le32(e, 0);  // external attrs
    Le32(e, big_offset ? uint32_t(k4G) : uint32_t(c.offset));
    e += c.name;
    if (z) {
      Le16(e, 0x0001);
      Le16(e, uint16_t(extra - 4));
      if (c.zip64) { Le64(e, c.size); Le64(e, c.size); }
      if (big_offset) Le64(e, c.offset);
    }
    if (!Put(e.data(), e.size())) { Abort(); return false; }
  }
  const uint64_t cd_size = position_ - cd_start;
  const bool z64_end = entries_.size() >= 0xFFFF || cd_start >= k4G || cd_size >= k4G;
  std::string t;
  if (z64_end) {
    const uint64_t z64_pos = position_;
    Le32(t, 0x06064b50);
    Le64(t, 44);
    Le16(t, 45);
    Le16(t, 45);
    Le32(t, 0);
    Le32(t, 0);
    Le64(t, entries_.size());
    Le64(t, entries_.size());
    Le64(t, cd_size);
    Le64(t, cd_start);
    Le32(t, 0x07064b50);
    Le32(t, 0);
    Le64(t, z64_pos);
    Le32(t, 1);
  }
  Le32(t, 0x06054b50);
  Le16(t, 0);
  Le16(t, 0);
  Le16(t, z64_end ? 0xFFFF : uint16_t(entries_.size()));
  Le16(t, z64_end ? 0xFFFF : uint16_t(entries_.size()));
  Le32(t, z64_end ? uint32_t(k4G) : uint32_t(cd_size));
  Le32(t, z64_end ? uint32_t(k4G) : uint32_t(cd_start));
  Le16(t, 0);
  if (!Put(t.data(), t.size())) { Abort(); return false; }
  if (std::fclose(file_) != 0) Fail("close failed");
  file_ = nullptr;
  std::error_code ec;
  if (error_.empty()) {
    std::filesystem::remove(final_, ec);
    std::filesystem::rename(part_, final_, ec);
    if (ec) Fail("rename failed: " + ec.message());
  }
  if (!error_.empty()) {
    std::filesystem::remove(part_, ec);
    return false;
  }
  return true;
}

void ZipWriter::Abort() {
  if (file_) {
    std::fclose(file_);
    file_ = nullptr;
    std::error_code ec;
    std::filesystem::remove(part_, ec);
  }
}

}  // namespace rdr_case
