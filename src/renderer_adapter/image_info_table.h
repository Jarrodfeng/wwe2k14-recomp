// S101 lane 47: the live image-info supplier. A thread-safe copy of the image records (width / height behind a frame texture slot's image id) that a
// replace hook's size check reads (ReplaceFactoryArgs::image_info). The live render thread feeds it from the records it already syncs into the binder
// (live.cpp, where `source.records` is replaced); fview and the unit test feed it from the capture's images.bin through the SAME Set(). Lookup() answers exactly
// what CaptureReader::FindImage answers (id 0 / past the table / a record whose id is not its slot: no info), so a capture and the live path agree.
// No info = the hook keeps the stream's image (arena_ours_core.cpp). No game or SDK includes.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "adapter_frame.h"

namespace rdr_adapter {

class ImageInfoTable {
 public:
  // Replaces the whole table (the live game resends all of it when it grows or a record changes).
  void Set(const ImageRecord* records, size_t count) {
    std::vector<ImageRecord> copy(records, records + count);
    std::lock_guard lock(mu_);
    records_.swap(copy);
  }
  size_t size() const {
    std::lock_guard lock(mu_);
    return records_.size();
  }
  // The same rule as CaptureReader::FindImage.
  bool Lookup(uint32_t id, uint32_t& w, uint32_t& h) const {
    std::lock_guard lock(mu_);
    if (id == 0 || id > records_.size()) return false;
    const ImageRecord& r = records_[id - 1];
    if (r.id != id) return false;
    w = r.width, h = r.height;
    return true;
  }
  // The function handed to ReplaceFactoryArgs::image_info. The table must outlive it.
  std::function<bool(uint32_t, uint32_t&, uint32_t&)> Supplier() const {
    return [this](uint32_t id, uint32_t& w, uint32_t& h) { return Lookup(id, w, h); };
  }

 private:
  mutable std::mutex mu_;
  std::vector<ImageRecord> records_;
};

}  // namespace rdr_adapter
