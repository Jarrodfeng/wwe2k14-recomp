// Session 98 (Arena Viewer phone port, stage B): see ui_backend_sdl3_vk.h. The Win32 / D3D11 backend's rules (what the UI swallows,
// the textures of the shown models, the back-buffer grab for ui_shot) on SDL3 + Vulkan.
#include "ui_backend_sdl3_vk.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include "renderer_vk.h"
#include "shot.h"
#include "ui_backend_common.h"

using namespace vkfn;

namespace view {
namespace ui {

namespace {

class Sdl3Vk : public Backend {
 public:
  Sdl3Vk(SDL_Window* w, RendererVk* r) : win_(w), r_(r) {}
  ~Sdl3Vk() override {
    if (!inited_) return;
    vkDeviceWaitIdle(r_->Device());
    r_->SetUiTextureHooks(nullptr, nullptr);
    r_->SetSwapchainHook(nullptr);
    for (auto& kv : own_) ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)kv.first), r_->FreeUiImage(kv.second);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    vkDestroySampler(r_->Device(), sampler_, nullptr);
  }
  bool Init(std::string& err) override {
    if (!ImGui_ImplSDL3_InitForVulkan(win_)) { err = "ImGui_ImplSDL3_InitForVulkan failed"; return false; }
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, [](const char* n, void* inst) { return GetProc(n, inst); }, (void*)r_->Instance())) {
      err = "ImGui_ImplVulkan_LoadFunctions failed";
      return false;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_1;
    ii.Instance = r_->Instance(), ii.PhysicalDevice = r_->PhysicalDevice(), ii.Device = r_->Device();
    ii.QueueFamily = r_->QueueFamily(), ii.Queue = r_->Queue();
    ii.DescriptorPoolSize = 4096;  // (one set per texture shown in the Assets tab, a few hundred per arena)
    ii.MinImageCount = 2, ii.ImageCount = std::max<uint32_t>(2, r_->SwapImageCount());
    ii.PipelineInfoMain.RenderPass = r_->UiRenderPass();
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&ii)) { err = "ImGui_ImplVulkan_Init failed"; return false; }
    VkSamplerCreateInfo sc{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sc.magFilter = sc.minFilter = VK_FILTER_LINEAR, sc.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sc.addressModeU = sc.addressModeV = sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, sc.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(r_->Device(), &sc, nullptr, &sampler_);
    VkSampler smp = sampler_;
    r_->SetUiTextureHooks([smp](VkImageView v) { return (uint64_t)ImGui_ImplVulkan_AddTexture(smp, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); },
                          [](uint64_t id) { ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)id); });
    r_->SetSwapchainHook([](uint32_t n) { if (n >= 2) ImGui_ImplVulkan_SetMinImageCount(n); });  // (a remade swap chain)
    inited_ = true;
    return true;
  }
  void NewFrame() override {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
  }
  void RenderDrawData() override { ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), r_->UiCommandBuffer()); }
  float Dpi() override {
#ifdef __ANDROID__
    // Session 98 phone port (stage C): the phone launcher's UI scale (mobile_main.cpp: the display scale over what SDL already scales;
    // 3.5 on the Find X8 Ultra), so the viewer's buttons are the launcher's finger size
    int pw = 0, ph = 0, ww = 0, wh = 0;
    SDL_GetWindowSizeInPixels(win_, &pw, &ph);
    SDL_GetWindowSize(win_, &ww, &wh);
    const float ui = SDL_GetWindowDisplayScale(win_) / (ww > 0 ? float(pw) / float(ww) : 1.f);
    return ui > 0.5f && ui < 8 ? ui : 1.f;
#else
    return std::max(1.f, SDL_GetWindowDisplayScale(win_));
#endif
  }
  bool Message(void*, unsigned msg, uintptr_t wp, intptr_t, bool dragging, intptr_t& out) override {
    if (msg != kSdlEventMsg) return false;
    const SDL_Event* ev = (const SDL_Event*)wp;
    ImGui_ImplSDL3_ProcessEvent(ev);
    ImGuiIO& io = ImGui::GetIO();
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);  // (an open combo must take Esc / arrows itself)
    bool swallow = false;
    switch (ev->type) {
      case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_WHEEL:
        swallow = io.WantCaptureMouse && !dragging;
        break;
      case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT:
        swallow = io.WantCaptureKeyboard || io.WantTextInput || popup;
        break;
      default: break;
    }
    if (swallow) out = 0;
    return swallow;
  }
  ImTextureID MakeTexture(const uint8_t* px, int w, int h, bool bc3) override {
    const VkImageView v = r_->MakeUiImage(px, w, h, bc3);
    if (!v) return 0;
    const ImTextureID id = (ImTextureID)ImGui_ImplVulkan_AddTexture(sampler_, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    own_[id] = v;
    return id;
  }
  void FreeTexture(ImTextureID id) override {
    auto it = own_.find(id);
    if (it == own_.end()) return;
    vkDeviceWaitIdle(r_->Device());
    ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)id);
    r_->FreeUiImage(it->second);
    own_.erase(it);
  }
  void ListModels(std::vector<ModelMeta>& out) override { ListRendererModels(*r_, out); }
  bool LookLit() override { return r_->LookLit(); }
  bool GrabBackBuffer(std::vector<uint8_t>& bgra, int& w, int& h, std::string& err) override { return r_->GrabBackBuffer(bgra, w, h, err); }
  bool WritePng(const std::string& path, const uint8_t* bgra, int w, int h, std::string& err) override { return view::WritePng(path, bgra, w, h, err); }
  std::string SystemFont(const char* file) override {
#ifdef _WIN32
    const char* wd = std::getenv("WINDIR");
    if (!wd) return std::string();
    std::filesystem::path p = std::filesystem::path(wd) / "Fonts" / file;
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return std::string();
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
#elif defined(__ANDROID__)
    // Session 98 phone port (stage C): the phone's own faces for the PC's (the phone launcher's choice, mobile_main.cpp LoadFonts)
    const bool heavy = std::string(file) == "ariblk.ttf";
    static const char* const kText[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/NotoSans-Regular.ttf"};
    static const char* const kHeavy[] = {"/system/fonts/Roboto-Black.ttf", "/system/fonts/Roboto-Bold.ttf"};
    std::error_code ec;
    for (const char* f : heavy ? kHeavy : kText)
      if (std::filesystem::exists(f, ec)) return f;
    return std::string();
#else
    (void)file;
    return std::string();
#endif
  }

 private:
  SDL_Window* win_;
  RendererVk* r_;
  VkSampler sampler_ = VK_NULL_HANDLE;
  std::map<ImTextureID, VkImageView> own_;
  bool inited_ = false;
};

}  // namespace

std::unique_ptr<Backend> MakeSdl3VkBackend(SDL_Window* window, RendererVk* r) { return std::make_unique<Sdl3Vk>(window, r); }

}  // namespace ui
}  // namespace view
