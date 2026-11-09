// Session 98 (Arena Viewer phone port, stage B): the Vulkan 1.1 backend of the viewer renderer - every pass of the D3D11 one (shadow
// maps, scene with skinning / instancing, sprites, the post chain, crowd flashes, the pick ID pass and the highlight), from the same
// HLSL (viewer_hlsl.h -> spirv\viewer_spv.h) and the same shared frame logic (renderer.h / render_shared.h).
// Structure (simple on purpose): one command buffer per frame / shot, submitted and waited for at once (nothing in flight between
// frames, so the CPU may rewrite any buffer between them); per-frame constants and bone palettes in a host-visible ring; one
// descriptor set per pass (set 0) and per draw (set 1) from a pool reset each frame; render passes / pipelines / framebuffers made on
// demand and cached (framebuffers per frame); D3D's clip space by a negative-height viewport.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "renderer.h"
#include "vk_loader.h"

namespace view {

class RendererVk : public Renderer {
 public:
  RendererVk();
  ~RendererVk() override;
  const char* BackendName() const override { return "vk"; }
  bool Init(const Surface& s, int w, int h, std::string& err) override;
  void Resize(int w, int h) override;
  void DrawFrame(const Camera& cam) override;
  bool ShotPixels(const Camera& cam, int w, int h, std::vector<uint8_t>& px, std::string& err) override;
  bool GrabBackBuffer(std::vector<uint8_t>& bgra, int& w, int& h, std::string& err) override;
  int AddSpriteTexture(const assets::Texture& t) override;
  std::string Timing(const Camera& cam, int w, int h, int frames, std::string& err, bool with_pick = false) override;
  bool DeviceLost() const override { return lost_; }
  uint64_t UiTexture(size_t model, size_t tex) override;

  // ---- for the UI backend (ui_backend_sdl3_vk.cpp: imgui_impl_vulkan) ----
  VkInstance Instance() const;
  VkPhysicalDevice PhysicalDevice() const;
  VkDevice Device() const;
  uint32_t QueueFamily() const;
  VkQueue Queue() const;
  VkRenderPass UiRenderPass();      // the render pass the overlay draws in (the window's picture, load)
  VkCommandBuffer UiCommandBuffer() const;  // valid inside the overlay callback
  uint32_t SwapImageCount() const;
  // A new swap chain's minimum image count (imgui_impl_vulkan's SetMinImageCount); called on every change.
  void SetSwapchainHook(std::function<void(uint32_t)> f);
  // Android: the window's surface goes away in the background (SDL_EVENT_WILL_ENTER_BACKGROUND -> SurfaceGone: swap chain and
  // surface dropped, frames skipped) and comes back in the foreground (SDL_EVENT_DID_ENTER_FOREGROUND -> SurfaceBack: a new
  // VkSurfaceKHR from Surface::vk_create_surface and a new swap chain). A VK_ERROR_SURFACE_LOST_KHR does the same by itself.
  void SurfaceGone();
  bool SurfaceBack();
  // The UI's handles of the scene's textures: make one for an image view / drop one (set by the UI backend after its init).
  void SetUiTextureHooks(std::function<uint64_t(VkImageView)> add, std::function<void(uint64_t)> remove);
  // A texture of the UI itself (font sheet, PNG frames): RGBA8, or BC3 blocks when bc3. Returns its view (0 on failure).
  VkImageView MakeUiImage(const uint8_t* px, int w, int h, bool bc3);
  void FreeUiImage(VkImageView view);

 protected:
  std::unique_ptr<GpuModelData> NewModelData() override;
  bool UploadTexture(GpuModelData& d, const assets::Texture& t, std::vector<std::string>& warnings) override;
  bool UploadPlacement(GpuModelData& d, const std::vector<assets::Matrix>& transforms, std::string& err) override;
  bool UploadMesh(GpuModelData& d, const assets::Mesh& m, int& buf, std::string& err) override;
  bool FinishModel(GpuModelData& d, std::string& err) override;
  bool UploadBones(GpuModel& g, const std::vector<assets::Matrix>& full, std::string& err) override;
  void DropBones(GpuModel& g) override;
  bool UpdateTexture(GpuModel& g, size_t slot, int w, int h, const uint8_t* rgba) override;
  void DropSpriteTextures() override;
  void DropPendingReadbacks() override;
  void BeforeModelsDrop() override;
  PickHit PickRead(const Camera& cam, int w, int h, int x, int y, std::string& err) override;
  bool LiveScreenReady() const override;

 public:
  struct Impl;  // (opaque; public so renderer_vk.cpp's helpers can name it)

 private:
  std::unique_ptr<Impl> p_;
  bool lost_ = false;
  friend struct Impl;
};

}  // namespace view
