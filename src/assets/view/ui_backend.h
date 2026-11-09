// Session 98: the seam between the Arena Viewer UI (ui.cpp: pure Dear ImGui + std, no Win32 / D3D) and the platform.
// PC: ui_backend_win32_dx11.cpp (imgui_impl_win32 + imgui_impl_dx11, the viewer's D3D11 Renderer). The phone port writes a
// Vulkan + SDL3 one. Textures cross this seam only as opaque ImTextureID values the backend makes and frees.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"

namespace view {
namespace ui {

// One uploaded texture of a shown model (read-only view of the renderer's GPU textures).
struct TexMeta {
  std::string name;
  uint32_t width = 0, height = 0, mips = 1;
  std::string format;   // "BC3 (DXT5)", ...
  ImTextureID id = 0;   // 0 = the upload failed; owned by the renderer (never freed by the UI)
};
struct ModelMeta {
  std::string label;  // "wrestler 107 attire 0", "arena 5"
  std::vector<TexMeta> textures;
};

// Session 98 phone port: on SDL platforms Message gets msg = kSdlEventMsg and wp = the const SDL_Event* (hwnd null, lp 0).
constexpr unsigned kSdlEventMsg = 0xFFFF5D10u;

class Backend {
 public:
  virtual ~Backend() {}
  virtual bool Init(std::string& err) = 0;  // after ImGui::CreateContext
  virtual void NewFrame() = 0;              // platform + renderer NewFrame
  virtual void RenderDrawData() = 0;        // ImGui::GetDrawData() into the back buffer
  virtual float Dpi() = 0;                  // 1.0 = 96 dpi
  // Messages (Win32): true = the UI used it, the viewer must not act on it. Other platforms: return false.
  virtual bool Message(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp, bool camera_dragging, intptr_t& out) = 0;
  // Own textures (font sheet, PNG frames). bc3 = true: `px` is the DXT5 blocks of a w x h sheet, else straight RGBA8.
  virtual ImTextureID MakeTexture(const uint8_t* px, int w, int h, bool bc3) = 0;
  virtual void FreeTexture(ImTextureID id) = 0;
  // The shown models and their textures (the scene's own GPU textures).
  virtual void ListModels(std::vector<ModelMeta>& out) = 0;
  virtual bool LookLit() = 0;
  // The finished back buffer (UI included) as BGRA, alpha 255; then written as PNG.
  virtual bool GrabBackBuffer(std::vector<uint8_t>& bgra, int& w, int& h, std::string& err) = 0;
  virtual bool WritePng(const std::string& path, const uint8_t* bgra, int w, int h, std::string& err) = 0;
  // Full path of a system font file ("segoeui.ttf", "ariblk.ttf"), "" when there is none.
  virtual std::string SystemFont(const char* file) = 0;
};

}  // namespace ui
}  // namespace view
