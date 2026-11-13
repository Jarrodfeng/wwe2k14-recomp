// The disk side of a capture (docs\renderer\CAPTURE_FORMAT.md): one background thread writes what the adapter hands it. No game or SDK includes.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rdr_adapter {

uint64_t Xxh3(const void* data, size_t bytes);

enum class Stream : uint32_t { Geometry = 0, Textures = 1 };

// All calls come from the adapter under its own lock.
class CaptureWriter {
 public:
  CaptureWriter();
  ~CaptureWriter();

  // Creates the folder and its files and starts the thread. False: nothing was started.
  bool Begin(const std::filesystem::path& folder, uint64_t budget_bytes);
  // Waits for everything queued to reach the disk, then closes the files.
  void End();
  bool active() const;

  // Appends to geometry.bin or textures.bin. The returned offset is where the bytes will be; UINT64_MAX: over the budget, nothing written.
  uint64_t Append(Stream stream, std::vector<uint8_t>&& bytes);
  // Writes a whole file under the capture folder (frames\frame_N.bin, images.bin, ...). False: over the budget.
  bool WriteFile(const std::string& relative, std::vector<uint8_t>&& bytes);

  uint64_t bytes_used() const;
  uint64_t budget() const;
  bool over_budget() const;
  // Non-empty after a write failed on the writer thread (the capture is then useless: the adapter stops it).
  std::string TakeError();

 private:
  struct Impl;
  Impl* impl_;
};

}  // namespace rdr_adapter
