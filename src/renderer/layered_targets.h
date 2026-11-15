// S102 lane C1 (docs\renderer\PRELIGHT_DIRECT.md section 7): a render pass into several layers of one image at once (VK_KHR_multiview, core in Vulkan 1.1).
// A TargetsOp whose colour[0].pad (ImageRef::pad, zero in every capture and every older frame) is N >= 2 renders into layers [layer, layer + N) of the image through one
// 2D-array view: ONE render pass, the draws run once per layer with gl_ViewIndex = 0..N-1 (view mask (1 << N) - 1). Every attachment of the pass has the same N.
// Offline instruments only: g_force_no_multiview makes the renderer say it has no multiview (the fallback arm and its planted twin).
#pragma once
#include <atomic>
#include <cstdint>

#include "api.h"

namespace rdr {

inline constexpr uint32_t kMaxViews = 6;   // the cube's six faces; Vulkan guarantees maxMultiviewViewCount >= 6 when multiview is there at all
inline std::atomic<bool> g_force_no_multiview{false};   // rplay / fview --no-multiview
// Offline plants (rplay / fview --layered-plant N, --layered-clear-sentinel): 1 = the render pass's view mask is (1 << N) - 2 (view 0 is never drawn), 2 = the layered colour transition covers ONE layer only
// (validation must catch it). The sentinel makes the layered pass's CLEAR value magenta instead of 0: a pass that really covers every texel gives the same cube either way (C1 review F3).
inline std::atomic<int> g_layered_plant{0};
inline std::atomic<bool> g_layered_clear_sentinel{false};

// How many layers the target renders into: 1 for every ordinary target.
inline uint32_t ViewCount(const ImageRef& ref) { return ref.pad >= 2 && ref.pad <= kMaxViews ? uint32_t(ref.pad) : 1u; }
inline ImageRef Layered(ImageRef ref, uint32_t views) {
  ref.pad = uint16_t(views >= 2 && views <= kMaxViews ? views : 0);
  return ref;
}

}  // namespace rdr
