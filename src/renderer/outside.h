// S102 lane G5 (docs\renderer\GPU_OUTSIDE.md): the load OUTSIDE our command buffers. R1 finding 2: on the Surface the frame's GPU time is larger than what the stamps inside the
// frame's one command buffer add up to. This is the table of those points, the meter that times each one (CPU side, steady clock, no Vulkan call added), and the line it prints.
// No game data; no Vulkan; no SDK. Renderer::Impl owns one OutsideMeter (renderer.cpp, search `S102 G5`).
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rdr {

// --wwe2k14_rdr_gpu_outside (default OFF, PerFrame): the meter. On, a frame's submit is followed at once by a wait on its fence (an INSTRUMENT: it serialises the CPU and the GPU for the
// frame, like the readback path does), so "the GPU's wall time for this submit" is known and the part the stamps do not cover can be named. The CPU-side timers need no switch (a clock
// read each); the line is printed with the switch on only. rplay: --gpu-outside 1. The live sink sets it from the per-frame switch table (rdr_sw::kGpuOutside).
inline std::atomic<bool> g_gpu_outside{false};
// --wwe2k14_rdr_upload_batch (default OFF, PerFrame): the frame's texture uploads in three groups (all layout changes to "copy destination" in ONE barrier, all the copies, all the
// changes back in ONE barrier) instead of two barriers per upload. The picture is byte-equal. Read every frame in Render (rplay: --upload-batch 1).
inline std::atomic<bool> g_upload_batch{false};
// S102 G5 counters (whole run, for fview's / rplay's report): frames that took the batched path, frames that WANTED it (switch on, more than one image upload) but kept the old path because one
// subresource was uploaded twice (the duplicate fallback), and frames with more than one image upload while the switch was on.
inline std::atomic<uint64_t> g_upload_batched_frames{0}, g_upload_dup_fallbacks{0}, g_upload_multi_frames{0};
// Tests only (fview --upload-batch-plant N): 1 = the batched groups' last barrier flush is dropped, 2 = a duplicate upload is injected and the duplicate fallback is OFF, 3 = the same duplicate
// with the fallback on (the control). 0 = off.
inline std::atomic<int> g_upload_batch_plant{0};

enum OutsidePoint : uint8_t {
  kOutFenceWait = 0,   //  1 the slot's fence wait (CPU blocks until the GPU finished this slot's last frame)
  kOutDrain,           //  2 DrainRetired: vkDestroy* + the memory allocator's frees of the staging a finished frame used
  kOutDestroys,        //  3 ProcessDestroys: images / buffers the game released
  kOutPools,           //  4 vkResetCommandPool + vkResetDescriptorPool
  kOutCollect,         //  5 CollectGpuTime: vkGetQueryPoolResults of the finished frame
  kOutPipeStore,       //  6 the pipeline store's slice + save (menus / loading only)
  kOutRingCopy,        //  7 the memcpy of constants / geometry / previous constants into the mapped ring (host-visible, write-combined memory)
  kOutBegin,           //  8 vkBeginCommandBuffer + the query reset + the first stamps
  kOutUploads,         //  9 ApplyUploads on the CPU: staging bookkeeping, barriers, the copies' recording, scaled-upload temp images
  kOutCompile,         // 10 Compile: the frame's passes and steps
  kOutTextureSets,     // 11 vkAllocateDescriptorSets + vkUpdateDescriptorSets for the frame's texture sets
  kOutFramebuffers,    // 12 vkCreateFramebuffer for targets new to the cache
  kOutPipelines,       // 13 vkCreateGraphicsPipelines on demand (a shader the frame needs and the cache lacks)
  kOutRecord,          // 14 recording the steps (less the sets / framebuffers / pipelines above)
  kOutEnd,             // 15 vkEndCommandBuffer
  kOutSubmit,          // 16 the vkQueueSubmit call
  kOutAcquire,         // 17 vkAcquireNextImageKHR (CPU wait for a free swapchain image; windowed only)
  kOutPresent,         // 18 the vkQueuePresentKHR call (windowed only)
  kOutPointCount
};

inline const char* OutsideName(uint32_t p) {
  static const char* k[kOutPointCount] = {"fence wait", "drain retired", "destroys", "pool resets", "gpu results read", "pipeline store", "ring copy", "begin + reset", "uploads (cpu)",
                                          "compile", "texture sets", "framebuffers", "pipelines built", "record", "end buffer", "submit call", "acquire", "present call"};
  return p < kOutPointCount ? k[p] : "?";
}

struct OutsideSums {
  uint64_t frames = 0;
  uint64_t ns[kOutPointCount] = {};
  // GPU side (needs the meter on): the wall time from the submit call's return to the fence's signal, the stamped work inside the buffer (tail - [0]) and what is left over.
  uint64_t gpu_wall_ns = 0, gpu_stamped_ns = 0, gpu_outside_ns = 0, gpu_samples = 0;
  uint64_t upload_gpu_ns = 0, upload_gpu_samples = 0;  // the GPU time of ApplyUploads' commands (a stamp after them; stamps on only)
  uint64_t uploads = 0, upload_bytes = 0, scaled_uploads = 0, upload_barriers = 0, pipelines_built = 0, upload_batched_frames = 0;
};

inline std::string OutsideLine(const char* tag, const OutsideSums& s) {
  if (!s.frames) return std::string();
  char buf[1536];
  const double f = double(s.frames) * 1e6;
  int n = std::snprintf(buf, sizeof(buf), "%s %llu frames, CPU ms a frame outside the buffers:", tag, static_cast<unsigned long long>(s.frames));
  for (uint32_t p = 0; p < kOutPointCount && n > 0 && n < int(sizeof(buf)) - 64; ++p) n += std::snprintf(buf + n, sizeof(buf) - size_t(n), " %s %.3f,", OutsideName(p), double(s.ns[p]) / f);
  const double g = s.gpu_samples ? double(s.gpu_samples) * 1e6 : 1.0;
  const double u = s.upload_gpu_samples ? double(s.upload_gpu_samples) * 1e6 : 1.0;
  if (n > 0 && n < int(sizeof(buf)) - 256)
    n += std::snprintf(buf + n, sizeof(buf) - size_t(n),
                       " | GPU outside the stamps: submit-to-signal wall %.3f - stamped %.3f = outside %.3f ms a frame (%llu samples); uploads on the GPU %.3f ms a frame (%llu samples);"
                       " a frame: %.1f uploads (%.1f scaled, %.0f KB), %.1f upload barriers, %.2f pipelines built, batched on %.0f%% of frames",
                       double(s.gpu_wall_ns) / g, double(s.gpu_stamped_ns) / g, double(s.gpu_outside_ns) / g, static_cast<unsigned long long>(s.gpu_samples), double(s.upload_gpu_ns) / u,
                       static_cast<unsigned long long>(s.upload_gpu_samples), double(s.uploads) / double(s.frames), double(s.scaled_uploads) / double(s.frames),
                       double(s.upload_bytes) / double(s.frames) / 1024.0, double(s.upload_barriers) / double(s.frames), double(s.pipelines_built) / double(s.frames),
                       100.0 * double(s.upload_batched_frames) / double(s.frames));
  return std::string(buf);
}

}  // namespace rdr
