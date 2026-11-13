// S101 UI contract (UI_PROMOTION_MAP.md 4, lane U0): the image-content supplier. What a replace hook that joins by picture content (the UI) needs to know about
// the stream image behind a texture slot's image record id: its size, format, mip count and a 64-bit hash of its texel bytes (all mips the record holds, as
// uploaded). Fed through the SAME Set() / Put() by fview (from the capture's image records and contents) and by the live render thread (where the reader
// writes an image's content), as lane 47's ImageInfoTable. Lookup() answers false for an id it was never told about (no info = the hook keeps the stream's
// image). The hash is fixed by the contract: ImageContentHash below (xxh3-64), the same for every feeder and every hook.
// No game or SDK includes (xxHash is header-only third-party code, from the SDK source tree or any -I that holds it).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

#define XXH_INLINE_ALL
#if __has_include("xxhash.h")
#include "xxhash.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/xxHash/xxhash.h"
#endif

namespace rdr_adapter {

// S101 contract (one hash for every feeder and every hook: lanes 66 UI, 70 ring, 68 flash): ImageContent::hash is XXH3-64 of the image's
// layer-0 mips 0..mips-1, each mip's texel bytes tightly packed (no row padding), in mip order, concatenated. A hook hashing a file texture
// for comparison hashes the same bytes the same way (its first `mips` mips). Any other function is a bug (a check would fail closed).
inline uint64_t ImageContentHash(const void* data, size_t bytes) { return XXH3_64bits(data, bytes); }

struct ImageContent {
  uint32_t width = 0, height = 0;
  uint32_t format = 0;   // an rdr::Format (the TextureBlobHeader's / ImageRecord's), so file and stream compare in the same terms
  uint32_t mips = 0;
  uint64_t hash = 0;     // ImageContentHash (above) of the layer-0 mips 0..mips-1, tightly packed, in order
  // Table-wide and monotonic (lane 66 review F2): a new number every time any id's content is written, never reused after an Erase / Clear, so a
  // hook that remembers (id, generation) always sees a rewrite as "changed after join".
  uint64_t generation = 0;
};

class ImageContentTable {
 public:
  void Put(uint32_t id, const ImageContent& c) {
    std::lock_guard lock(mu_);
    ImageContent& slot = map_[id];
    slot = c;
    slot.generation = ++next_generation_;
  }
  void Erase(uint32_t id) {
    std::lock_guard lock(mu_);
    map_.erase(id);
  }
  void Clear() {
    std::lock_guard lock(mu_);
    map_.clear();
  }
  bool Lookup(uint32_t id, ImageContent& out) const {
    std::lock_guard lock(mu_);
    const auto it = map_.find(id);
    if (it == map_.end()) return false;
    out = it->second;
    return true;
  }
  // The function handed to ReplaceFactoryArgs::image_content. The table must outlive it.
  std::function<bool(uint32_t, ImageContent&)> Supplier() const {
    return [this](uint32_t id, ImageContent& out) { return Lookup(id, out); };
  }

 private:
  mutable std::mutex mu_;
  std::unordered_map<uint32_t, ImageContent> map_;
  uint64_t next_generation_ = 0;  // survives Erase / Clear
};

}  // namespace rdr_adapter
