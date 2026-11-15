// The renderer. Hand it images, buffers, programs and whole frames; it draws them.
// CreateImage / UpdateImage / CreateBuffer / UpdateBuffer / Destroy* may be called from any thread. RenderFrame belongs to one thread.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api.h"
#include "counts.h"  // S102 M1
#include "outside.h"  // S102 G5
#include "overlay.h"

namespace rdr {

struct RendererOptions {
  int validation = 0;             // 0 off; 1 core + synchronisation; 2 also best practices
  int gpu_index = -1;
  bool instrument_stats = false;  // S102 G10 (review F11): rplay --draw-census 2 only: the device enables pipelineStatisticsQuery; the game never sets it
  NativeWindow window;            // window.window null: headless
  bool vsync = true;
  float render_scale = 1.0f;      // applied to images marked kUsageScaled
  // Settings parity S10b: DLSS. -1 off; 0 DLAA, 1 max performance, 2 balanced, 3 max quality, 4 ultra performance, 5 ultra quality (an
  // output no bigger than the render size is always DLAA). Needs streamline_dir (NVIDIA's runtime) and a device that runs DLSS.
  int dlss_mode = -1;
  std::string streamline_dir;
  bool present_sgsr = false;      // settings parity S10a: an upscaling present through SGSR 1 (rdr_present_sgsr.frag), else bilinear
  float sgsr_sharpness = 2.0f;    // SGSR's edge sharpness (the reference default)
  bool light_cube_compute = false;    // S102 L1b review F2: the start-time value of wwe2k14_rdr_prelight_compute: the light cubes' compute pipelines are made at startup (compute_supported() is true only then)
  bool multiview = false;             // S102 C1 review F2: the start-time value of wwe2k14_rdr_prelight_layered (DeviceOptions::multiview)
  bool robust_buffers = false;  // S102 D1 (--wwe2k14_rdr_robust_buffers, StartOnly): the device enables robustBufferAccess (DeviceOptions::robust_buffers)
  bool exclusive_fullscreen = false;  // S102 G2b (--wwe2k14_rdr_exclusive_fullscreen, StartOnly, Windows): the window swapchain asks for VK_EXT_full_screen_exclusive at the panel's own mode (exclusive_policy.h)
  bool present_noclear = false;   // S102 G1 lever 2 (--wwe2k14_rdr_low_present_noclear under LOW): no full-panel clear, only the bars around the picture
  uint32_t present_offscreen_w = 0, present_offscreen_h = 0;  // S102 G2 (measurement, rplay --present-size WxH): no window, the present draws into images of this size (swapchain.h CreateOffscreen)
  bool present_fill = false;     // settings parity S5 (ultrawide / taller screens): the frame stretched to the window (anamorphic: the
                                  // reader squeezed the scene by the same factor), else fitted with bars
  uint32_t frames_in_flight = 2;
  // S100 phone 120 fix 2 (run s100ph4): the rate the display-timed presents are spaced at (the decoupled 120 sets 120). 0 = the swapchain's
  // reported refresh. Jarrod's phone reports 16.58 ms (60 Hz) even while it runs at 120 (the old renderer read the same while it presented
  // 120 a second in matches), so spacing by that reading held the 120 mode at 60.
  uint32_t present_hz = 0;
  // S101 lane22 (the replay farm): two timestamps around each frame's command buffer, read back with LastGpuNs(). Off by default (the game exe shares this file).
  bool gpu_timestamps = false;
  // S102 M1: per-frame, per-pass counts of the calls the renderer records (counts.h), read with LastFrameCounts(). Integer increments only; off in the game unless
  // --wwe2k14_rdr_counts, always on in rplay (--counts).
  bool counts = false;
  bool occlusion_precise = false;  // S102 G11 (F1): the device enables occlusionQueryPrecise and the frame trace's per-draw queries are precise (rplay --frame-trace only)
  // Lane 269: a test plant for the sanity check: 1 = the wrong tick period (x1000), 2 = results read straight after the submit. 0 = off.
  uint32_t gpu_timestamp_plant = 0;
  // S102 S0-3 (test only, needs gpu_timestamps): 3 = an EMPTY pass planted in the plane class, 4 = a SLOW pass (copies of a scratch buffer) planted in the rim class. 0 = off.
  uint32_t gpu_stamp_plant = 0;  // (S102 G5: 6 = an EXTRA queue submit of its own small buffer (a slow copy loop) before each frame's buffer: GPU work outside the stamped buffer, which the outside meter must show)
  // S101 lane 200 (dev only): pretend format features are missing so the fallbacks run on a full-feature GPU (format_caps.h bits). 0 = the device as it is.
  uint32_t format_caps_mask = 0;
  // S102 lane U1 (docs\renderer\TITLE_CARD_MALI.md): block-compressed textures (BC1-BC5) on a device that cannot sample them (Mali, Xclipse, Adreno 6xx) are made in
  // RGBA8 / R8 / RG8 and decoded on the CPU at upload (bc_decode.h). 0 = off (the old refusal by name), 1 = auto (only where the device lacks BC: nothing changes
  // on a device that has it), 2 = force (the offline proof on a PC).
  uint32_t bc_decode = 1;
  // S102 U1 (dev builds only, tests): staging_cap_mb shrinks the 256 MB staged-bytes cap; staging_wait_plant brings back the pre-fix wait on the render thread (the planted fault).
  uint32_t staging_cap_mb = 0;
  bool staging_wait_plant = false;
  std::string gpu_name;  // S102 U1: part of the graphics card's name the player chose in the launcher (--wwe2k14_gpu); empty = the best card (discrete first)
  // Lane 276 (SKIP_GAME_WORK_MAP 19.10): the pipeline store. A folder (UTF-8) for pipelines.bin (the list of pipelines built, keys only) and pipeline_cache.bin (the
  // driver's VkPipelineCache, header-checked); empty = off (the default: the cache starts empty and dies with the process, as before).
  std::string pipeline_store_dir;
  uint32_t pipeline_prebuild_slice_us = 2000;  // the listed pipelines are built in slices of about this long at the start of each RenderFrame (at least one a slice); 0 = only PrebuildPipelines() builds them
  uint32_t pipeline_store_plant = 0;           // test plants: 1 = the list's checksum is not checked, 2 = the cache's device/driver/checksum is not checked
  uint32_t pipeline_save_interval_ms = 30000;  // how often new pipelines are saved while the game runs (the save runs on one worker; tests use a short one)
};

// True for a VkFormat whose vertex input arrives as an integer (UINT / SINT): the shader must declare that location as an integer type.
bool VertexFormatIsInteger(uint16_t vk_format);

// Full: everything. PersistentOnly: a frame that will not be shown still does its work that carries state. DisplayOnly: an in-between redraw, no state work.
// S102 lane KO: NoPasses (test only, wwe2k14_rdr_ko_draw_none): no pass and no copy is recorded, uploads and the present still run.
enum class RenderMode : uint8_t { Full, PersistentOnly, DisplayOnly, NoPasses };

class Renderer {
 public:
  static std::unique_ptr<Renderer> Create(const RendererOptions& options, std::string& error);
  ~Renderer();

  ImageId CreateImage(const ImageDesc& desc);
  // `data` is tightly packed for that mip and layer, in the image's own format.
  void UpdateImage(ImageId image, uint32_t mip, uint32_t layer, const void* data, size_t bytes);
  void DestroyImage(ImageId image);

  BufferId CreateBuffer(size_t bytes, const void* initial);
  void UpdateBuffer(BufferId buffer, size_t offset, const void* data, size_t bytes);
  void DestroyBuffer(BufferId buffer);

  uint16_t AddProgram(const ProgramDesc& desc);
  uint16_t AddVertexLayout(const VertexLayoutDesc& desc);

  void RenderFrame(const Frame& frame, RenderMode mode = RenderMode::Full);

  // S102 R21-B (docs\renderer\STAGE_SPLIT.md): the two-stage render thread. Any thread. SealUploads closes the uploads made so far (UpdateImage /
  // UpdateBuffer / new images) into a batch and returns its id: a frame whose Frame::upload_seal is that id applies exactly the batches up to it (a frame
  // with seal 0 applies everything, as before). SetProducerThread names the calling thread as the one that hands frames to Render: it never waits for
  // staging room (only Render frees staging; U1's rule, extended to the bind stage).
  uint64_t SealUploads();
  void SetProducerThread();
  // S102 R21-B (review F1): the calling thread is the one that calls RenderFrame now (rdr_record names itself before its hooks run, so a staging wait
  // there is never a wait on itself after a one-thread frame).
  void SetRenderThread();
  // S102 R21-B (review F1): SetOverlayTexture in two halves for the two-stage thread. MakeOverlayTexture (the bind stage, before the hand-over: its
  // upload is sealed with that frame) makes and uploads the image; SetOverlayImage (the thread that renders) puts it under the id (the old one is destroyed).
  ImageId MakeOverlayTexture(uint32_t width, uint32_t height, const uint8_t* rgba);
  void SetOverlayImage(uint32_t id, ImageId image);

  // 256 RGBA8 entries: the display curve the final draw applies by value. The default is the identity.
  void SetDisplayCurve(const uint8_t* rgba256);

  // Settings parity S11: the app's overlay (overlay.h), drawn over the picture at every present until replaced; null = none. Textures by the
  // ids the lists name (RGBA8, tightly packed); id 0 is plain white. Render thread (the one calling RenderFrame).
  void SetOverlayTexture(uint32_t id, uint32_t width, uint32_t height, const uint8_t* rgba);
  void SetOverlay(std::shared_ptr<const OverlayList> list);
  // Settings parity S10b: DLSS's last output (the picture before the HUD, at the shown size), for checks; kNoImage = none.
  ImageId dlss_output() const;
  // lane 201: true when the LAST present drew DLSS's output (so dlss_output() is the shown picture before the HUD, not an older frame's).
  bool dlss_in_last_present() const;
  // S102 C1 (layered_targets.h): the device can render one pass into several layers (VK_KHR_multiview, Vulkan 1.1 core; the feature is enabled when the device has it). False under rdr::g_force_no_multiview.
  bool multiview_supported() const;
  // S102 C1 (review of G14): the format can be rendered to AND sampled on this device (a light cube is given target usage only then: the strip it replaces was a target of this format).
  bool ColourTargetable(Format format) const;
  // S102 L1b (light_cubes.h): the frame's queue can run a compute dispatch (false under rdr::g_force_no_compute), and the format can be written by a compute shader AND sampled (8-bit RGBA only).
  bool compute_supported() const;
  bool StorageTargetable(Format format) const;
  // S101 lane22: the GPU time of the last rendered frame's command buffer in ns (top to bottom of pipe), waiting for it if need be; 0 when
  // gpu_timestamps is off, the queue has no valid timestamp bits, or no frame was rendered.
  uint64_t LastGpuNs();
  // S102 M1: the counts of the last rendered frame (counts.h); empty (frame 0, no passes) when RendererOptions::counts is off or no frame was rendered. Render thread.
  const FrameCounts& LastFrameCounts() const;
  // S102 M1 test plant (rplay --counts-plant PASS): one extra draw is recorded (a repeat of the pass's last draw) in render pass number PASS of every frame, so the budget check has a fault to catch. -1 = off.
  void SetCountsPlant(int pass);
  // Lane 269: logs the GPU time window so far (median / p95 / max); the renderer logs one every 600 frames and at shutdown by itself.
  void LogGpuSummary();
  // Lane 269: whole-run counts of results dropped as implausible (not in 0..time since the submit) and of results not ready; a healthy run has both 0.
  void GpuTotals(uint64_t& implausible, uint64_t& not_ready) const;
  // S102 S0-3: names the image's GPU class (api.h GpuClass) for the per-role GPU line. Unnamed images are kGpuOther. Cheap; call after CreateImage.
  void SetImageClass(ImageId image, uint8_t gpu_class);
  // S102 S0-3: the whole run's sums for tests and the summary: ms a frame per class, the wait, the work (classes summed) and the old bracket, over `frames` GPU samples.
  struct GpuRoleTotals {
    double cls_ms[kGpuClassCount] = {};
    double blit_ms = 0;  // S102 G2: the output cap's blit (inside cls_ms[kGpuOutput])
    double gap_ms = 0, work_ms = 0, total_ms = 0;  // gap = the start gap [0] -> [1]; NOT the display wait (docs\renderer\GPU_STAMPS.md)
    double cls_passes_a_frame[kGpuClassCount] = {};
    double acquire_ms = 0, present_call_ms = 0;     // per Render, over the whole run
    uint64_t frames = 0, no_pass = 0, no_present = 0, overflow = 0;
    double stamps_a_frame = 0;
  };
  GpuRoleTotals GpuRoles() const;
  // S102 G5 (docs\renderer\GPU_OUTSIDE.md): the whole run's sums of the points outside our command buffers (outside.h); the CPU side always counts, the GPU side needs rdr::g_gpu_outside.
  const OutsideSums& OutsideRun() const;

  // Blocks until the GPU is idle, then copies mip 0 / layer 0 to tightly packed RGBA8. Test and screenshot use only.
  bool ReadImage(ImageRef image, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
  // S102 G2 (rplay --present-size, test use): the last picture the present drew into the offscreen output, as RGBA8. False with a window or before any present.
  bool ReadPresent(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
  // Lane 232 (3d a2): the read-back without a drain. QueueReadback asks for a level to be copied (persistent host-visible staging, recorded into the command buffer of the NEXT RenderFrame, after
  // all its steps); false: not readable / already queued or in flight. PollReadbacks (render thread, never waits) appends each copy whose frame's fence has signalled, as RGBA8 like ReadImage.
  struct ReadbackResult {
    uint64_t tag = 0;
    ImageRef image;
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    bool cancelled = false;  // the request was dropped (image gone / unreadable / its source copy never ran): no bytes, forget the tag
  };
  // `source`: when set, the copy is recorded right after the copy step that writes this level from that image, and cancelled when no such copy ran in the frame.
  bool QueueReadback(ImageRef image, uint64_t tag, ImageId source = kNoImage);
  void PollReadbacks(std::vector<ReadbackResult>& out);
  void WaitIdle();
  // S101 fview (nothing else calls these): one texel's raw bytes (the image's own format; compare these, not the display copy) and a display RGBA8 of it;
  // AddProgram with the shader-module results checked; whether the validation layer is really loaded.
  bool ReadTexelRaw(ImageRef image, int x, int y, uint8_t raw[8], uint32_t& raw_bytes, uint8_t rgba[4]);
  // S101 lane 20 (the judge): many texels in one submit (one wait, one staging buffer). Each read's `ok` says whether it was readable (same rules as ReadTexelRaw).
  struct TexelRead {
    ImageRef image;
    int x = 0, y = 0;
    uint8_t raw[8] = {};
    uint32_t raw_bytes = 0;
    uint8_t rgba[4] = {};
    bool ok = false;
  };
  bool ReadTexelsRaw(std::vector<TexelRead>& reads);
  bool AddProgramChecked(const ProgramDesc& desc, uint16_t& id);

  // Lane 276 (SKIP_GAME_WORK_MAP 19.10): the pipeline store. `blobs` are the SPIR-V arrays of the shaders programs can be made from (static, must outlive the
  // renderer); a stored pipeline whose program is not made yet is made from them by hash. Cheap without a store (does nothing).
  struct ShaderBlobDesc {
    const char* name;
    const uint32_t* words;
    size_t bytes;
  };
  void RegisterShaderCatalog(const ShaderBlobDesc* blobs, size_t count);
  bool pipeline_store_active() const;
  // Lane 276: false while a scene is live (match / entrance / victory): the listed pipelines are then not built in slices (what is left stays on demand). The app sets it each frame.
  void SetPipelinePrebuildAllowed(bool allowed);
  // Lane 276: the live game hard-exits (~Renderer never runs). Render thread, from the quit flush: saves what is dirty, waits for a save in flight, prints the summary ("at quit").
  void FlushPipelineStore();
  // Builds every listed pipeline that can be built now (render thread); `max_ms` 0 = all. Returns how many are left (unresolved ones included).
  size_t PrebuildPipelines(uint32_t max_ms = 0);
  // Writes the list and the driver cache now (also done at shutdown and every 30 s while pipelines were built).
  void SavePipelineStore();
  struct PipelineStoreCounts {
    uint64_t on_demand = 0, prebuilt = 0, prebuilt_used = 0, on_demand_ns = 0, prebuilt_ns = 0, list_records = 0, list_loaded = 0;
  };
  PipelineStoreCounts pipeline_store_counts() const;
  bool validation_active() const;

  uint64_t validation_errors() const;
  const char* device_name() const;

 private:
  Renderer();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rdr
