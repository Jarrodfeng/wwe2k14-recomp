// S102 lane R24-E (docs\renderer\GPU_SMALL.md): the "small" GPU passes. No Vulkan, no game here: the switch, the decision and the census, shared by post_pass.cpp (the pick), live.cpp (the per-frame read),
// fview / rplay (the flags) and tools\r24e.
//
// wwe2k14_rdr_gpu_small (PerFrame, default 0), a bit mask read where the pass is recorded, every frame:
//   1  the quarter-size depth-of-field chain of OUR post pass (yDownScale_dof + yBlur_hole_fill into q1_ / q2_, the 320x180 "post" / "other" steps of the GPU table) is not recorded when nothing reads it:
//      the combine reads q2_ in slot 2 (the NEAR blur) only inside `if (DOFParam2.x > 1 - depth)` and in slot 6 (the FAR blur) when no earlier layer made one (post_plan.h far_blur). With
//      DOFParam2.x <= 0 and a separate far blur (Bryan's entrance, the two-layer shape) the near branch can never run (depth is in [0, 1], so 1 - depth >= 0), slot 6 reads the stream's own image, and
//      no bloom / glare chain reads q2_: the two passes (about 0.9 ms drained on the UHD 620) are dead work. A REMOVAL, exact: the picture is byte-equal (the combine's slot 2 is bound to the scene colour, a
//      valid image the shader never samples in that case).
//   2  RESERVED, NOT BUILT (the rim chain skip: the census found its inputs equal to the previous frame on 0 of 1,200+ frames of Bryan / Brock / wm30; docs\renderer\GPU_SMALL.md section 3).
//   4  RESERVED, NOT BUILT (the pre-light memo: the lights' constants differ on every frame; section 4).
// The switch is clamped to the built bits (ClampGpuSmall), so a value of 2 or 4 does nothing.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "api.h"

namespace rdr {

inline constexpr int kGpuSmallNearChain = 1;
inline std::atomic<int> g_gpu_small{0};              // the mask above
inline std::atomic<int> g_gpu_small_plant{0};        // tools only: 1 = the near-blur test is not made, 2 = the far-blur test is not made, 3 = neither, 4 = never applied (the census reads 0 taken), 5 = the depth-format test is not made
inline int ClampGpuSmall(int v) { return v & kGpuSmallNearChain; }

// Census (relaxed counters in fixed slots, bumped once per Record, never per draw; the line is built at print time).
enum GpuSmallWhy : int { kGsTaken = 0, kGsOff, kGsNearLive, kGsFarShared, kGsBloomLive, kGsViewerDepth, kGsNoDofChain, kGsDepthFloat, kGsWhyCount };
inline std::atomic<uint64_t> g_gpu_small_records[kGsWhyCount]{};   // [kGsTaken] = records that skipped the two passes, the others = the reason a record with the switch on kept them
inline std::atomic<uint64_t> g_gpu_small_passes_removed{0};
inline std::atomic<uint32_t> g_gpu_small_last_x{0};
// Tools only (rplay arm kind 7 "MASK:X"): DOFParam2.x forced to X for the frame (both arms of a proof see the same combine constant), so the captures can cross x = 0.
inline std::atomic<bool> g_gpu_small_force{false};
inline std::atomic<uint32_t> g_gpu_small_force_x{0};
// The proof that the near branch can never run needs depth in [0, 1]: a depth buffer or an UNORM image holds that, a float colour image (R32F, RGBA16F ...) may not (review F1). Allow-list of rdr::Format values.
inline bool GpuSmallDepthClamped(uint32_t format) {
  switch (format) {
    case uint32_t(Format::RGBA8): case uint32_t(Format::BGRA8): case uint32_t(Format::R8): case uint32_t(Format::RG8): case uint32_t(Format::RGBA4): case uint32_t(Format::R5G6B5):
    case uint32_t(Format::A1RGB5): case uint32_t(Format::RGB10A2): case uint32_t(Format::RGBA16_UNORM): case uint32_t(Format::RG16_UNORM): case uint32_t(Format::R16_UNORM): case uint32_t(Format::Depth):
      return true;
    default:
      return false;
  }
}                // last DOFParam2.x seen (float bits)

// The decision, one function so the pass and the tools share it. `x` = DOFParam2.x; `far_separate` = slot 6 reads an image an earlier layer made; `reads_chain` = the bright pass (bloom / glare) or the
// planted dof_chain reads q2_; `viewer_depth` = the depth is the viewer's converted R32F (it may exceed 1); `has_chain` = this record has a dof / hole_fill (or t2) pass at all.
inline GpuSmallWhy GpuSmallDecide(int mask, float x, bool far_separate, bool reads_chain, bool viewer_depth, bool has_chain, bool depth_clamped = false) {
  if (!(mask & kGpuSmallNearChain)) return kGsOff;
  if (!has_chain) return kGsNoDofChain;
  const int plant = g_gpu_small_plant.load(std::memory_order_relaxed);
  if (plant == 4) return kGsOff;   // planted: the lever is never applied (the census must read 0 taken)
  if (viewer_depth) return kGsViewerDepth;
  if (!depth_clamped && plant != 5) return kGsDepthFloat;   // planted 5: the depth-format test is not made
  if (reads_chain) return kGsBloomLive;
  const bool near_dead = (plant == 1 || plant == 3) ? true : !(x > 0.f);   // planted 1 / 3: the near-blur test is not made
  if (!near_dead) return kGsNearLive;
  if (!far_separate && plant != 2 && plant != 3) return kGsFarShared;   // planted 2 / 3: the far-blur test is not made
  return kGsTaken;
}
inline void GpuSmallNote(GpuSmallWhy why, float x, uint32_t passes_removed) {
  g_gpu_small_records[why].fetch_add(1, std::memory_order_relaxed);
  if (why == kGsTaken) g_gpu_small_passes_removed.fetch_add(passes_removed, std::memory_order_relaxed);
  uint32_t bits;
  std::memcpy(&bits, &x, 4);
  g_gpu_small_last_x.store(bits, std::memory_order_relaxed);
}
inline void GpuSmallCensusLine(uint64_t frames, char* out, size_t cap) {
  uint32_t bits = g_gpu_small_last_x.load();
  float x;
  std::memcpy(&x, &bits, 4);
  std::snprintf(out, cap,
                "[rdr][gpu_small] mask %d, frames %llu, post records with the switch on: taken %llu (passes removed %llu), kept: near blur live %llu, far blur shared with the near %llu, bloom / glare reads the chain %llu, viewer depth %llu, no dof chain %llu, depth not clamped (float format) %llu; last DOFParam2.x %.4f",
                g_gpu_small.load(), static_cast<unsigned long long>(frames), static_cast<unsigned long long>(g_gpu_small_records[kGsTaken].load()), static_cast<unsigned long long>(g_gpu_small_passes_removed.load()),
                static_cast<unsigned long long>(g_gpu_small_records[kGsNearLive].load()), static_cast<unsigned long long>(g_gpu_small_records[kGsFarShared].load()),
                static_cast<unsigned long long>(g_gpu_small_records[kGsBloomLive].load()), static_cast<unsigned long long>(g_gpu_small_records[kGsViewerDepth].load()),
                static_cast<unsigned long long>(g_gpu_small_records[kGsNoDofChain].load()), static_cast<unsigned long long>(g_gpu_small_records[kGsDepthFloat].load()), double(x));
}

}  // namespace rdr
