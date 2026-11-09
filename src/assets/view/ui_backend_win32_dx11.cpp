// Session 98: see ui_backend_win32_dx11.h.
#include "ui_backend_win32_dx11.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>

#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
#include "renderer_d3d11.h"
#include "shot.h"
#include "ui_backend_common.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace view {
namespace ui {

void EnableDpiAwareness() { SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); }
float SystemDpiScale() { return std::max(1.f, (float)GetDpiForSystem() / 96.f); }

namespace {

class Win32Dx11 : public Backend {
 public:
  Win32Dx11(HWND h, RendererD3D11* r) : hwnd_(h), r_(r) {}
  ~Win32Dx11() override {
    if (inited_) {
      ImGui_ImplDX11_Shutdown();
      ImGui_ImplWin32_Shutdown();
    }
  }
  bool Init(std::string& err) override {
    if (!ImGui_ImplWin32_Init(hwnd_)) { err = "ImGui_ImplWin32_Init failed"; return false; }
    if (!ImGui_ImplDX11_Init(r_->Device(), r_->Context())) { err = "ImGui_ImplDX11_Init failed"; return false; }
    inited_ = true;
    return true;
  }
  void NewFrame() override {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
  }
  void RenderDrawData() override { ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData()); }
  float Dpi() override { return std::max(1.f, (float)GetDpiForWindow(hwnd_) / 96.f); }

  bool Message(void* hw, unsigned msg, uintptr_t wp, intptr_t lp, bool dragging, intptr_t& out) override {
    ImGui_ImplWin32_WndProcHandler((HWND)hw, msg, wp, lp);
    if (msg == WM_DPICHANGED) {  // moved to a monitor with another DPI: take the suggested window rect (the UI rescales itself in Draw)
      const RECT* rc = (const RECT*)lp;
      SetWindowPos((HWND)hw, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
      out = 0;
      return true;
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);  // (an open combo must take Esc / arrows itself)
    bool swallow = false;
    switch (msg) {
      case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
      case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEMOVE: case WM_MOUSEWHEEL:
        swallow = io.WantCaptureMouse && !dragging;
        break;
      case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        swallow = io.WantCaptureKeyboard || io.WantTextInput || popup;
        break;
    }
    if (swallow) out = 0;
    return swallow;
  }

  ImTextureID MakeTexture(const uint8_t* px, int w, int h, bool bc3) override {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w; d.Height = (UINT)h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = bc3 ? DXGI_FORMAT_BC3_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init{px, (UINT)(bc3 ? (w + 3) / 4 * 16 : w * 4), 0};
    Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(r_->Device()->CreateTexture2D(&d, &init, &tex))) return 0;
    if (FAILED(r_->Device()->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return 0;
    return (ImTextureID)(uintptr_t)srv;
  }
  void FreeTexture(ImTextureID id) override {
    if (auto* s = (ID3D11ShaderResourceView*)(uintptr_t)id) s->Release();
  }

  void ListModels(std::vector<ModelMeta>& out) override { ListRendererModels(*r_, out); }
  bool LookLit() override { return r_->LookLit(); }
  bool GrabBackBuffer(std::vector<uint8_t>& bgra, int& w, int& h, std::string& err) override {
    return r_->GrabBackBuffer(bgra, w, h, err);
  }
  bool WritePng(const std::string& path, const uint8_t* bgra, int w, int h, std::string& err) override {
    return view::WritePng(path, bgra, w, h, err);
  }
  std::string SystemFont(const char* file) override {
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::filesystem::path p = std::filesystem::path(windir) / "Fonts" / file;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return std::string();
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
  }

 private:
  HWND hwnd_;
  RendererD3D11* r_;
  bool inited_ = false;
};

}  // namespace

std::unique_ptr<Backend> MakeWin32Dx11Backend(HWND hwnd, RendererD3D11* r) { return std::make_unique<Win32Dx11>(hwnd, r); }

}  // namespace ui
}  // namespace view
