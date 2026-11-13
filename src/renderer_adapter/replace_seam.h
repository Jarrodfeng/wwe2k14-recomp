// S101 lane 41: pure helpers of the in-place replace seam (draw_replace_hook.h), kept apart from the binder so an offline test can run them without a
// GPU: the vertex-constant row override. No game or SDK includes.
#pragma once
#include <cstdint>
#include <cstring>

namespace rdr_adapter {

// ReplaceOut::first_register / rows / row_data onto a packed vertex-constant block (Binder::PackBlock's layout). `ranges` are the variant's
// ConstantRange table (fields reg, count, offset: rdr::fx::ConstantRange; a template so this header needs no registry): each range maps registers
// [reg, reg + count) to block bytes [offset, offset + 16 * count). Registers of the override that no range maps are not read by the program and are
// skipped; the rest are copied row for row. Returns the rows written. A range that would write past `block_bytes` is skipped whole (never writes
// outside the block). The stream's bytes are never touched: the caller passes its own COPY of the block.
template <class Range>
inline uint32_t ApplyRowOverride(const Range* ranges, uint32_t range_count, uint8_t* block, uint32_t block_bytes, uint32_t first_register, uint32_t rows,
                                 const float* row_data) {
  if (!rows || !row_data) return 0;
  uint32_t written = 0;
  for (uint32_t r = 0; r < range_count; ++r) {
    const uint32_t reg = ranges[r].reg, count = ranges[r].count;
    const uint32_t lo = first_register > reg ? first_register : reg;
    const uint32_t end_a = first_register + rows, end_b = reg + count;
    const uint32_t hi = end_a < end_b ? end_a : end_b;
    if (lo >= hi) continue;
    const uint32_t at = uint32_t(ranges[r].offset) + (lo - reg) * 16u, bytes = (hi - lo) * 16u;
    if (at + bytes > block_bytes) continue;
    std::memcpy(block + at, row_data + size_t(lo - first_register) * 4, bytes);
    written += hi - lo;
  }
  return written;
}

}  // namespace rdr_adapter
