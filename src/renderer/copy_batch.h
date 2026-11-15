// S102 lane G12 (docs\renderer\COPIES_LEFT.md): consecutive copies recorded as ONE vkCmdCopyImage with several regions and ONE barrier flush. No Vulkan, no game, no SDK here:
// this header holds the switch, the counters, the planted faults and the pure rule (Compatible) the renderer's step loop and tools\g12\copy_batch_test.cpp share.
//
// Why: the 360 resolves one strip texture into six cube faces with six back-to-back copies (the pre-light sss cubes: 5 slots x 6 copies a frame in Bryan's entrance, 30 of
// the 97 copy steps); each copy costs a pipeline barrier (a layout change of ITS cube layer) and a copy command, and on a shared-memory GPU the barrier's drain is most of
// the cost of a 64x64 copy. The pixels moved are the same; the work around them is one barrier and one command instead of six.
//
// The rule (Compatible, all must hold between the batch's first copy and the next one): the same source image, mip and layer; the same destination image; a destination
// subresource (mip, layer) not yet in the batch (so no two regions write the same texels and the single command has no write-write overlap); both copies take the plain
// vkCmdCopyImage path (same texel block, same extent: the renderer's own test, not the blit path); a batch is at most kMaxBatch regions. Everything between two copies of
// the batch is nothing (they are adjacent steps), so no draw or other copy can sit between them: the destination is written exactly as before, region by region.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr {

// --wwe2k14_rdr_copy_batch (default OFF, PerFrame): read every frame at the top of Render. rplay / fview: --copy-batch 1.
inline std::atomic<bool> g_copy_batch{false};
// Tests only (fview / rplay --copy-batch-plant N), 0 = off: 1 the batch's LAST region is not copied (a stale cube face: the picture must change); 2 the batch's barrier flush is
// left out (a layout error only the validation layers see); 3 the control (the batch as shipped).
inline std::atomic<int> g_copy_batch_plant{0};
// Counters (whole run, for the report): batches recorded (more than one copy in one command), copies they replaced, the most regions in one batch.
inline std::atomic<uint64_t> g_copy_batches{0}, g_copy_batched{0}, g_copy_batch_max{0};

inline constexpr uint32_t kMaxBatch = 16;

// What the rule needs to know about one copy that takes the plain path. sub = mip * layers + layer of the destination.
struct BatchKey {
  uint32_t source_image = 0, source_mip = 0, source_layer = 0;
  uint32_t dest_image = 0, dest_sub = 0;
  bool plain = false;   // vkCmdCopyImage path (not a blit, not unsupported)
};

// `batch` = the copies already in the batch (batch[0] fixes the source and destination), `next` = the candidate.
inline bool Compatible(const BatchKey* batch, uint32_t count, const BatchKey& next) {
  if (!count || count >= kMaxBatch || !next.plain) return false;
  const BatchKey& first = batch[0];
  if (!first.plain) return false;
  if (next.source_image != first.source_image || next.source_mip != first.source_mip || next.source_layer != first.source_layer || next.dest_image != first.dest_image) return false;
  for (uint32_t i = 0; i < count; ++i)
    if (batch[i].dest_sub == next.dest_sub) return false;
  return true;
}

// S102 G12 review F5: the step loop's lookahead as a pure function. views[0] is the first copy (already known to be recorded); views[1..count) are the steps after it, in order, up to kMaxBatch of them:
// is_copy false (a pass or the Present), records false (the render mode would not record the step: PersistentOnly / DisplayOnly), or a key Compatible refuses each END the batch. Returns the number
// of copies to record together (1 = no batch).
struct StepView {
  bool is_copy = false, records = false;
  BatchKey key;
};
inline uint32_t BatchSpan(const StepView* views, uint32_t count) {
  if (count < 2 || !views[0].is_copy || !views[0].records || !views[0].key.plain) return 1;
  BatchKey batch[kMaxBatch];
  batch[0] = views[0].key;
  uint32_t n = 1;
  for (uint32_t j = 1; j < count && n < kMaxBatch; ++j) {
    if (!views[j].is_copy || !views[j].records || !Compatible(batch, n, views[j].key)) break;
    batch[n++] = views[j].key;
  }
  return n;
}

inline void NoteBatch(uint32_t regions) {
  g_copy_batches.fetch_add(1, std::memory_order_relaxed);
  g_copy_batched.fetch_add(regions, std::memory_order_relaxed);
  uint64_t seen = g_copy_batch_max.load(std::memory_order_relaxed);
  while (regions > seen && !g_copy_batch_max.compare_exchange_weak(seen, regions, std::memory_order_relaxed)) {}
}

}  // namespace rdr
