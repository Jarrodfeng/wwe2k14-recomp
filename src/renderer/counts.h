// S102 lane M1: machine-independent counts of what one rendered frame asks of the GPU and the driver. Integers only, no clocks: the same capture
// and the same exe give the same numbers on any machine, so tools\budget\budget_check.py can refuse a change that raises one.
// Filled by Renderer::Impl::Render / RecordPass / RecordCopy when RendererOptions::counts is on (rplay always; the game behind
// --wwe2k14_rdr_counts, default off); read with Renderer::LastFrameCounts(). Names and numbers only (hard rule 1).
#pragma once
#include <cstdint>
#include <vector>

namespace rdr {

struct CallCounts {
  uint32_t draws = 0;             // vkCmdDraw
  uint32_t indexed_draws = 0;     // vkCmdDrawIndexed
  uint32_t dispatches = 0;        // vkCmdDispatch (S102 L1b: the light cubes' one dispatch)
  uint32_t copies = 0;            // image copies / blits recorded for a CopyOp, and the buffer / image copies of an upload
  uint32_t clears = 0;            // vkCmdClearAttachments and the load-op clears of a pass's attachments
  uint32_t barriers = 0;          // vkCmdPipelineBarrier commands
  uint32_t barrier_images = 0;    // image barriers inside them
  uint32_t pipeline_binds = 0;    // vkCmdBindPipeline
  uint32_t descriptor_binds = 0;  // vkCmdBindDescriptorSets
  uint32_t push_constants = 0;    // vkCmdPushConstants
  uint32_t dynamic_sets = 0;      // dynamic-state blocks issued (viewport + scissor + bias + blend + stencil + push constants)
  uint32_t vertex_binds = 0;      // vkCmdBindVertexBuffers
  uint32_t index_binds = 0;       // vkCmdBindIndexBuffer
  uint64_t vertices = 0;          // sum of count * instances over the non-indexed draws
  uint64_t indices = 0;           // sum of count * instances over the indexed draws
  uint64_t area_px = 0;           // upper bound of target pixels written: per draw, viewport x scissor x pass rectangle, summed (overdraw included)
  void Add(const CallCounts& o) {
    draws += o.draws, indexed_draws += o.indexed_draws, dispatches += o.dispatches, copies += o.copies, clears += o.clears, barriers += o.barriers;
    barrier_images += o.barrier_images, pipeline_binds += o.pipeline_binds, descriptor_binds += o.descriptor_binds, push_constants += o.push_constants;
    dynamic_sets += o.dynamic_sets, vertex_binds += o.vertex_binds, index_binds += o.index_binds, vertices += o.vertices, indices += o.indices, area_px += o.area_px;
  }
};

struct PassCounts {
  uint32_t index = 0;              // the pass's number in this frame's plan
  uint32_t width = 0, height = 0;  // the render area
  uint32_t color_count = 0;
  uint32_t first_op = 0;           // its first op in the frame (names the pass in a failure message)
  // The pass's identity, stable when other passes come and go: the first target (first colour, else depth) and its format. budget_check.py keys a pass by
  // target image + mip + layer + size + colour count + the occurrence of that key in the frame. `index` is a label only: it shifts when a pass comes or goes.
  uint32_t target_image = 0, target_mip = 0, target_layer = 0, target_format = 0;
  CallCounts calls;
};

struct FrameCounts {
  uint32_t frame = 0;         // the renderer's own frame number (serial)
  uint32_t passes = 0;        // render passes recorded
  uint32_t steps = 0;
  uint32_t texture_sets = 0;  // texture descriptor sets built
  uint32_t ring_constants = 0, ring_geometry = 0, ring_prev = 0;  // bytes written into the frame ring
  uint64_t upload_bytes = 0;  // image + buffer upload bytes asked for this frame (the requested sizes, not the driver's allocation sizes)
  uint32_t dropped = 0;       // 1 when the frame was dropped before recording (frame_ring_full): its other counts are 0
  // Everything recorded into the frame's command buffer by Render: passes (draws, binds, load-op and in-pass clears), copies and blits of CopyOps and uploads,
  // barriers (FlushBarriers and the raw ones in uploads and the in-frame read-back), image clears of uploads. NOT counted: the present pass, DLSS, the overlay
  // (windowed only; rplay has none), and ReadImage / ReadPixels style calls that record their own command buffer outside any frame.
  CallCounts total;
  std::vector<PassCounts> pass;
  void Reset(uint32_t serial) {
    frame = serial, passes = steps = texture_sets = ring_constants = ring_geometry = ring_prev = dropped = 0, upload_bytes = 0;
    total = CallCounts{};
    pass.clear();
  }
};

// One per attachment whose load op is CLEAR: the colour attachments in `color_clear_mask` (bit c) plus the depth/stencil attachment when its depth or stencil load is CLEAR.
inline uint32_t LoadClearCount(uint32_t color_clear_mask, bool depth_stencil_clear) {
  uint32_t n = depth_stencil_clear ? 1u : 0u;
  for (; color_clear_mask; color_clear_mask &= color_clear_mask - 1) ++n;
  return n;
}

}  // namespace rdr
