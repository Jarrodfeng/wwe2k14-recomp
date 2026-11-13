// Settings parity S9b (Session 100, Jarrod 2026-11-11 08:1x: "120fps absolutely stays as long as a decouplement is there properly"): a picture
// between two game ticks. The game keeps its 60 updates; the live sink draws, for each tick, the picture halfway between the previous tick and
// this one, then this tick's own. Draws are found again in the previous tick by the binder's blend key (its shaders, its geometry and how many
// such draws came before it, binder.h BlendRanges); the vertex constants the shader build marked interpolable (transforms, camera and light
// positions, bones) are mixed, everything else is this tick's. A draw new this tick, or whose ranges differ, shows this tick's values.
// The same mixing as FramePump::Interpolated (frame_pump.cpp), with the previous tick kept as copies (the live sink's builder is reused).
#pragma once
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "api.h"

namespace rdr {

class TickBlender {
 public:
  // Keeps what the next tick's blend needs from this tick (its draws' keys and constants).
  void Keep(const Frame& frame) {
    draws_.assign(frame.draws.data, frame.draws.data + frame.draws.count);
    ranges_.assign(frame.blend_ranges.data, frame.blend_ranges.data + frame.blend_ranges.count);
    constants_.assign(frame.constants.data, frame.constants.data + frame.constants.count);
    by_key_.clear();
    for (uint32_t i = 0; i < uint32_t(draws_.size()); ++i) {
      const DrawOp& draw = draws_[i];
      const uint64_t key = uint64_t(draw.blend_key_high) << 32 | draw.blend_key_low;
      if (key && draw.blend_count) by_key_.emplace(key, i);
    }
    valid_ = true;
  }
  void Forget() { valid_ = false; }
  bool valid() const { return valid_; }

  // `frame` with its interpolable vertex constants mixed `alpha` of the way from the kept tick to it (0 = the kept tick, 1 = `frame`).
  // The result's spans point into `frame`'s storage and this object: valid until either changes. `matched` counts the draws mixed.
  const Frame& Blend(const Frame& frame, float alpha, uint32_t& matched) {
    matched = 0;
    blended_ = frame;
    if (!valid_ || frame.blend_ranges.count == 0) return blended_;
    mixed_.assign(frame.constants.data, frame.constants.data + frame.constants.count);
    for (uint32_t i = 0; i < frame.draws.count; ++i) {
      const DrawOp& draw = frame.draws[i];
      const uint64_t key = uint64_t(draw.blend_key_high) << 32 | draw.blend_key_low;
      if (!key || !draw.blend_count) continue;
      const auto found = by_key_.find(key);
      if (found == by_key_.end()) continue;
      const DrawOp& before = draws_[found->second];
      if (before.blend_count != draw.blend_count) continue;
      bool any = false;
      for (uint32_t r = 0; r < draw.blend_count; ++r) {
        const BlendRange& range = frame.blend_ranges[draw.blend_first + r];
        const BlendRange& range_before = ranges_[before.blend_first + r];
        if (range.bytes != range_before.bytes || range.offset != range_before.offset) continue;
        if (size_t(before.vs_constants) + range_before.offset + range_before.bytes > constants_.size()) continue;
        if (size_t(draw.vs_constants) + range.offset + range.bytes > mixed_.size()) continue;
        const auto* from = reinterpret_cast<const float*>(constants_.data() + before.vs_constants + range_before.offset);
        const auto* to = reinterpret_cast<const float*>(frame.constants.data + draw.vs_constants + range.offset);
        auto* out = reinterpret_cast<float*>(mixed_.data() + draw.vs_constants + range.offset);
        for (uint32_t c = 0; c < range.bytes / 4; ++c) out[c] = from[c] + (to[c] - from[c]) * alpha;
        any = true;
      }
      matched += any;
    }
    blended_.constants = Span<uint8_t>{mixed_.data(), uint32_t(mixed_.size())};
    return blended_;
  }

 private:
  bool valid_ = false;
  std::vector<DrawOp> draws_;
  std::vector<BlendRange> ranges_;
  std::vector<uint8_t> constants_;
  std::unordered_map<uint64_t, uint32_t> by_key_;
  std::vector<uint8_t> mixed_;
  Frame blended_;
};

}  // namespace rdr
