#include "memory.h"

#include <algorithm>

#include "problem.h"

namespace rdr {
bool Allocator::CarveFrom(int block_index, const VkMemoryRequirements& needs, Allocation& out) {
  Block& block = blocks_[size_t(block_index)];
  for (size_t i = 0; i < block.free.size(); ++i) {
    Range range = block.free[i];
    const VkDeviceSize aligned = (range.offset + needs.alignment - 1) & ~(needs.alignment - 1);
    const VkDeviceSize padding = aligned - range.offset;
    if (range.size < padding + needs.size) continue;
    const VkDeviceSize tail = range.size - padding - needs.size;
    block.free.erase(block.free.begin() + std::ptrdiff_t(i));
    if (tail) block.free.insert(block.free.begin() + std::ptrdiff_t(i), Range{aligned + needs.size, tail});
    if (padding) block.free.insert(block.free.begin() + std::ptrdiff_t(i), Range{range.offset, padding});
    out.memory = block.memory;
    out.offset = aligned;
    out.size = needs.size;
    out.mapped = block.mapped ? block.mapped + aligned : nullptr;
    out.block = block_index;
    return true;
  }
  return false;
}

bool Allocator::Alloc(const VkMemoryRequirements& needs, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred, bool linear, Allocation& out) {
  // S101: a preferred memory type whose heap is full (e.g. the 256 MB host-visible device-local window without resizable BAR, shared
  // with other processes) is a preference, not a requirement: try the next type that has the required flags instead of failing
  // ("Not enough graphics memory for the frame ring" on a busy GPU, the gate's recurring flake; small-VRAM devices the same).
  uint32_t type_bits = needs.memoryTypeBits;
  for (;;) {
    const int type = device_->FindMemoryType(type_bits, required, preferred);
    if (type < 0) return false;
    if (AllocOfType(type, needs, linear, out)) return true;
    type_bits &= ~(1u << uint32_t(type));
  }
}

bool Allocator::AllocOfType(int type, const VkMemoryRequirements& needs, bool linear, Allocation& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < int(blocks_.size()); ++i) {
    if (blocks_[size_t(i)].type == type && blocks_[size_t(i)].linear == linear && CarveFrom(i, needs, out)) return true;
  }
  VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  info.allocationSize = std::max(block_size_, needs.size);
  info.memoryTypeIndex = uint32_t(type);
  Block block;
  if (vk::vkAllocateMemory(device_->device(), &info, nullptr, &block.memory) != VK_SUCCESS) {
    // A block-sized request can fail where the allocation itself would fit: try the exact size once before giving up on this type.
    if (info.allocationSize == needs.size) return false;
    info.allocationSize = needs.size;
    if (vk::vkAllocateMemory(device_->device(), &info, nullptr, &block.memory) != VK_SUCCESS) return false;
  }
  block.size = info.allocationSize;
  block.type = type;
  block.linear = linear;
  block.free.push_back({0, block.size});
  if (device_->memory().memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
    void* mapped = nullptr;
    if (vk::vkMapMemory(device_->device(), block.memory, 0, VK_WHOLE_SIZE, 0, &mapped) == VK_SUCCESS) block.mapped = static_cast<uint8_t*>(mapped);
  }
  blocks_.push_back(std::move(block));
  bytes_reserved_ += info.allocationSize;
  return CarveFrom(int(blocks_.size()) - 1, needs, out);
}

void Allocator::Free(const Allocation& allocation) {
  if (allocation.block < 0) return;
  std::lock_guard<std::mutex> lock(mutex_);
  Block& block = blocks_[size_t(allocation.block)];
  const Range freed{allocation.offset, allocation.size};
  auto next = std::lower_bound(block.free.begin(), block.free.end(), freed, [](const Range& a, const Range& b) { return a.offset < b.offset; });
  next = block.free.insert(next, freed);
  if (next + 1 != block.free.end() && next->offset + next->size == (next + 1)->offset) {
    next->size += (next + 1)->size;
    block.free.erase(next + 1);
  }
  if (next != block.free.begin() && (next - 1)->offset + (next - 1)->size == next->offset) {
    (next - 1)->size += next->size;
    block.free.erase(next);
  }
}

void Allocator::Destroy() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Block& block : blocks_) {
    if (block.memory) vk::vkFreeMemory(device_->device(), block.memory, nullptr);
  }
  blocks_.clear();
  bytes_reserved_ = 0;
}

}  // namespace rdr
