// The renderer. Hand it images, buffers, programs and whole frames; it draws them.
// CreateImage / UpdateImage / CreateBuffer / UpdateBuffer / Destroy* may be called from any thread. RenderFrame belongs to one thread.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api.h"
#include "overlay.h"

namespace rdr {

struct RendererOptions {
  int validation = 0;             // 0 off; 1 core + synchronisation; 2 also best practices
  int gpu_index = -1;
  NativeWindow window;            // window.window null: headless
  bool vsync = true;
  float render_scale = 1.0f;      // applied to images marked kUsageScaled
  // Settings parity S10b: DLSS. -1 off; 0 DLAA, 1 max performance, 2 balanced, 3 max quality, 4 ultra performance, 5 ultra quality (an
  // output no bigger than the render size is always DLAA). Needs streamline_dir (NVIDIA's runtime) and a device that runs DLSS.
  int dlss_mode = -1;
  std::string streamline_dir;
  bool present_sgsr = false;      // settings parity S10a: an upscaling present through SGSR 1 (rdr_present_sgsr.frag), else bilinear
  float sgsr_sharpness = 2.0f;    // SGSR's edge sharpness (the reference default)
  bool present_fill = false;      // settings parity S5 (ultrawide / taller screens): the frame stretched to the window (anamorphic: the
                                  // reader squeezed the scene by the same factor), else fitted with bars
  uint32_t frames_in_flight = 2;
  // S100 phone 120 fix 2 (run s100ph4): the rate the display-timed presents are spaced at (the decoupled 120 sets 120). 0 = the swapchain's
  // reported refresh. Jarrod's phone reports 16.58 ms (60 Hz) even while it runs at 120 (the old renderer read the same while it presented
  // 120 a second in matches), so spacing by that reading held the 120 mode at 60.
  uint32_t present_hz = 0;
  // S101 lane22 (the replay farm): two timestamps around each frame's command buffer, read back with LastGpuNs(). Off by default (the game exe shares this file).
  bool gpu_timestamps = false;
  // Lane 269: a test plant for the sanity check: 1 = the wrong tick period (x1000), 2 = results read straight after the submit. 0 = off.
  uint32_t gpu_timestamp_plant = 0;
  // S101 lane 200 (dev only): pretend format features are missing so the fallbacks run on a full-feature GPU (format_caps.h bits). 0 = the device as it is.
  uint32_t format_caps_mask = 0;
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
enum class RenderMode : uint8_t { Full, PersistentOnly, DisplayOnly };

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
  // S101 lane22: the GPU time of the last rendered frame's command buffer in ns (top to bottom of pipe), waiting for it if need be; 0 when
  // gpu_timestamps is off, the queue has no valid timestamp bits, or no frame was rendered.
  uint64_t LastGpuNs();
  // Lane 269: logs the GPU time window so far (median / p95 / max); the renderer logs one every 600 frames and at shutdown by itself.
  void LogGpuSummary();
  // Lane 269: whole-run counts of results dropped as implausible (not in 0..time since the submit) and of results not ready; a healthy run has both 0.
  void GpuTotals(uint64_t& implausible, uint64_t& not_ready) const;

  // Blocks until the GPU is idle, then copies mip 0 / layer 0 to tightly packed RGBA8. Test and screenshot use only.
  bool ReadImage(ImageRef image, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height);
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
