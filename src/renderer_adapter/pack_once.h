// S102 T5: the pack-once body of Binder::PackConstants, as a template so the offline test (tools/fb/fb_test.cpp) can run the SHIPPED code on fake types.
// Variant: block_bytes, constant_count, constants[] {offset, reg, count}, bools_offset. Bank: dwords. Bools: words (8 x uint32).
// Packs straight into the frame builder's block (FrameBuilder::ReserveConstants: aligned, zeroed). Every copy is clamped to the reserved size
// (review F1: a no-fragment Variant{} has block_bytes 0 -> 16 bytes reserved, bools_offset 0: an unclamped 32-byte copy wrote past the vector).
// The bytes equal the stack-block path's (that path writes the same copies into an 8 KB block and pushes the first `bytes`).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "../renderer/frame_builder.h"

namespace rdr_adapter {

template <class Variant, class Bank, class Bools>
inline uint32_t PackOnceInto(const Variant& variant, const Bank& bank, const Bools& bools, rdr::FrameBuilder& builder) {
  const uint32_t bytes = std::min<uint32_t>(std::max<uint32_t>(variant.block_bytes, 16), 8192);
  uint32_t offset = 0;
  uint8_t* block = builder.ReserveConstants(bytes, offset);
  uint32_t count = variant.constant_count;
  if (count && rdr::g_pack_once_plant.load(std::memory_order_relaxed)) --count;  // TEST ONLY planted twin: the last range is dropped
  for (uint32_t r = 0; r < count; ++r) {
    const auto& range = variant.constants[r];
    if (uint32_t(range.offset) >= bytes) continue;
    const uint32_t want = uint32_t(range.count) * 16;
    const uint32_t n = std::min<uint32_t>(want, bytes - uint32_t(range.offset));
    if constexpr (requires { bank.CopyBytes(0u, 0u, static_cast<uint8_t*>(nullptr)); })   // S102 T1b: a BankRef (block map aware)
      bank.CopyBytes(uint32_t(range.reg) * 16, n, block + range.offset);
    else
      std::memcpy(block + range.offset, bank.dwords + uint32_t(range.reg) * 4, n);
  }
#ifdef RDR_PACK_PLANT_NOCLAMP  // TEST ONLY (tools/fb/fb_test.cpp, built with AddressSanitizer): the review's bug, the unclamped bools copy
#ifndef RDR_FB_UNIT_TEST_BUILD
#error RDR_PACK_PLANT_NOCLAMP belongs to tools/fb/fb_test.cpp only
#endif
  if (variant.bools_offset != 0xFFFF) std::memcpy(block + variant.bools_offset, bools.words, 32);
#else
  if (variant.bools_offset != 0xFFFF && uint32_t(variant.bools_offset) < bytes)
    std::memcpy(block + variant.bools_offset, bools.words, std::min<uint32_t>(32, bytes - uint32_t(variant.bools_offset)));
#endif
  return offset;
}

}  // namespace rdr_adapter
