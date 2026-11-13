// A small block allocator: a few large VkDeviceMemory blocks, first-fit inside them. Linear and optimal resources live in separate blocks,
// so buffer-image granularity never applies.
#pragma once
#include <mutex>
#include <vector>

#include "device.h"

namespace rdr {

struct Allocation {
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
  VkDeviceSize size = 0;
  uint8_t* mapped = nullptr;  // null unless the memory is host-visible
  int block = -1;
};

class Allocator {
 public:
  void Init(Device* device) {
    device_ = device;
    block_size_ = (device->caps().unified_memory ? 32ull : 64ull) << 20;
  }
  void Destroy();

  bool Alloc(const VkMemoryRequirements& needs, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, bool linear, Allocation& out);
  void Free(const Allocation& allocation);

  VkDeviceSize bytes_reserved() const { return bytes_reserved_; }

 private:
  struct Range {
    VkDeviceSize offset, size;
  };
  struct Block {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint8_t* mapped = nullptr;
    int type = -1;
    bool linear = false;
    std::vector<Range> free;  // sorted by offset, merged
  };

  bool CarveFrom(int block_index, const VkMemoryRequirements& needs, Allocation& out);
  bool AllocOfType(int type, const VkMemoryRequirements& needs, bool linear, Allocation& out);  // S101: one memory type; false = its heap is full

  Device* device_ = nullptr;
  VkDeviceSize block_size_ = 64ull << 20;
  std::vector<Block> blocks_;
  VkDeviceSize bytes_reserved_ = 0;
  std::mutex mutex_;
};

}  // namespace rdr
