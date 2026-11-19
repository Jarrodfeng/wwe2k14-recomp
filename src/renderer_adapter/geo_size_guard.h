// S103 IXF: the binder's static geometry cache is keyed by the geometry offset. A hit must never hand a draw a buffer SMALLER than the draw's range
// (a30_match: a 1,270-byte buffer made for a stale draw at offset 2,529,850, then drawn with 1,311 indices = 2,622 bytes: a validation error and,
// on a weak GPU, a read past the buffer; docs\research\s103\IX_index_overrun.md). Larger is safe (same offset = same content prefix).
//
// Pure: src\renderer_adapter\geo_size_guard_test.cpp is the offline proof.
#pragma once

#include <cstdint>

namespace rdr_adapter::geosize {

enum class Cache : uint8_t {
  kCreate,    // nothing cached at this offset: make the buffer (size = the draw's range)
  kHit,       // cached, at least as big as the draw's range: serve it
  kMismatch,  // cached but smaller than the draw's range: Problem("geometry_offset_size_mismatch"), serve a separate correctly-sized buffer, leave the cache
};

inline Cache Classify(bool have_buffer, uint32_t cached_size, uint32_t want_size) {
  if (!have_buffer) return Cache::kCreate;
  return cached_size >= want_size ? Cache::kHit : Cache::kMismatch;
}

}  // namespace rdr_adapter::geosize
