// Session 98: the PC backend of the Arena Viewer UI (Win32 window, the viewer's D3D11 Renderer). The only UI file that
// touches Win32 / D3D.
#pragma once
#include <windows.h>

#include <memory>

#include "ui_backend.h"

namespace view {
class RendererD3D11;
namespace ui {

// Call once before the window is created (the UI is laid out in real pixels).
void EnableDpiAwareness();
float SystemDpiScale();  // 1.0 at 96 dpi (the primary monitor's, after EnableDpiAwareness)
std::unique_ptr<Backend> MakeWin32Dx11Backend(HWND hwnd, RendererD3D11* r);

}  // namespace ui
}  // namespace view
