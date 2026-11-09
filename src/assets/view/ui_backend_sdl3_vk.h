// Session 98 (Arena Viewer phone port, stage B): the Arena Viewer UI's backend for an SDL3 window + the Vulkan renderer
// (imgui_impl_sdl3 + imgui_impl_vulkan). The PC's --backend vk and, in stage C, the phone.
#pragma once
#include <memory>

#include "ui_backend.h"

struct SDL_Window;

namespace view {
class RendererVk;
namespace ui {

std::unique_ptr<Backend> MakeSdl3VkBackend(SDL_Window* window, RendererVk* r);

}  // namespace ui
}  // namespace view
