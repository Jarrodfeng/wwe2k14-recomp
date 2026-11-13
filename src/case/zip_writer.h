// A store-only zip writer (Session 101, lane 5): no compression, CRC-32, zip64 for entries or archives past 4 GB. Our own code, no game or SDK includes.
// Each entry's size is declared up front; its bytes arrive in pieces; the CRC and sizes follow in a data descriptor (one pass, no seeking back).
// The archive is written to "<path>.part" and renamed on Finish, so a watcher never sees half a zip.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace rdr_case {

uint32_t Crc32(uint32_t crc, const void* data, size_t bytes);  // slicing-by-8; crc = 0 to start

class ZipWriter {
 public:
  ZipWriter() = default;
  ~ZipWriter();
  ZipWriter(const ZipWriter&) = delete;
  ZipWriter& operator=(const ZipWriter&) = delete;

  bool Begin(const std::filesystem::path& path);
  // Starts an entry of exactly `size` bytes (a different total in Write calls makes EndEntry fail). `name` uses '/'.
  bool BeginEntry(const std::string& name, uint64_t size);
  bool Write(const void* data, size_t bytes);
  bool EndEntry();
  // Convenience: a whole entry from memory.
  bool AddBytes(const std::string& name, const void* data, size_t bytes);
  // Writes the central directory and renames the file into place. False: any earlier call failed (the .part file is deleted).
  bool Finish();
  void Abort();
  const std::string& error() const { return error_; }
  uint64_t bytes_written() const { return position_; }

 private:
  struct Central {
    std::string name;
    uint64_t size = 0, offset = 0;
    uint32_t crc = 0;
    bool zip64 = false;
    uint16_t dos_time = 0, dos_date = 0;
  };
  bool Put(const void* data, size_t bytes);
  void Fail(const std::string& what);

  std::filesystem::path final_, part_;
  std::FILE* file_ = nullptr;
  uint64_t position_ = 0;
  std::vector<Central> entries_;
  bool in_entry_ = false;
  uint64_t entry_declared_ = 0, entry_written_ = 0;
  uint32_t entry_crc_ = 0;
  std::string error_;
};

}  // namespace rdr_case
