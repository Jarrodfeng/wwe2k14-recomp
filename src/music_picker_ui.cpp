// See music_picker_ui.h. Session 101, lane 292: the game-side glue of the music picker's view (the ImGui dialog, the owner-mode texture
// uploader, the Android touch pass-through). The view itself is music_picker_imgui.h.
#include "music_picker_ui.h"

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include "music_picker_imgui.h"
#include "renderer_adapter/overlay_drawer.h"

#if defined(__ANDROID__)
#include <jni.h>
#include <rex/cvar.h>
#include <string>
#endif

namespace wwe2k14::music_picker {

std::unique_ptr<PickerView> MakeImGuiPickerView(ViewHost host) { return std::make_unique<ImGuiPickerView>(std::move(host)); }

namespace {

class Dialog final : public rex::ui::ImGuiDialog, public PickerDialog {
 public:
  Dialog(rex::ui::ImGuiDrawer* drawer, ViewHost host, std::function<bool(PickerView&, float)> frame, std::function<void()> closed)
      : rex::ui::ImGuiDialog(drawer), owned_(MakeImGuiPickerView(std::move(host))), view_(owned_.get()), frame_(std::move(frame)), closed_(std::move(closed)) {
    g_open.store(true, std::memory_order_relaxed);
    view_->OnOpen();
  }
  Dialog(rex::ui::ImGuiDrawer* drawer, PickerView* view, std::function<bool(PickerView&, float)> frame, std::function<void()> closed)
      : rex::ui::ImGuiDialog(drawer), view_(view), frame_(std::move(frame)), closed_(std::move(closed)) {
    g_open.store(true, std::memory_order_relaxed);  // (the caller's view: it calls OnOpen / OnClose itself)
  }
  void Close() override { rex::ui::ImGuiDialog::Close(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!frame_) return Close();
    if (!frame_(*view_, std::clamp(io.DeltaTime, 0.0f, 0.1f))) Close();
  }
  void OnClose() override {
    if (owned_) view_->OnClose();
    g_open.store(false, std::memory_order_relaxed);
    auto closed = std::move(closed_);
    if (closed) closed();  // (the dialog object is deleted right after this returns)
  }

 private:
  std::unique_ptr<PickerView> owned_;
  PickerView* view_ = nullptr;
  std::function<bool(PickerView&, float)> frame_;
  std::function<void()> closed_;
};

// Keeps the overlay layer's texture alive (the Drawer's Texture owns the renderer's copy by id).
struct TextureOwner {
  std::unique_ptr<rex::ui::ImmediateTexture> tex;
};

}  // namespace

PickerDialog* OpenPickerDialog(rex::ui::ImGuiDrawer* drawer, ViewHost host, std::function<bool(PickerView&, float dt)> per_frame,
                               std::function<void()> on_closed) {
  if (!drawer) return nullptr;
  return new Dialog(drawer, std::move(host), std::move(per_frame), std::move(on_closed));
}

PickerDialog* OpenPickerDialogWithView(rex::ui::ImGuiDrawer* drawer, PickerView* view, std::function<bool(PickerView&, float dt)> per_frame,
                                       std::function<void()> on_closed) {
  if (!drawer || !view) return nullptr;
  return new Dialog(drawer, view, std::move(per_frame), std::move(on_closed));
}

std::function<GpuImage(int, int, const uint8_t*)> MakeOwnerDrawerUploader() {
  if (!rdr_overlay::g_drawer) return {};
  return [](int w, int h, const uint8_t* rgba) -> GpuImage {
    if (!rdr_overlay::g_drawer || w <= 0 || h <= 0) return {};
    auto owner = std::make_shared<TextureOwner>();
    owner->tex = rdr_overlay::g_drawer->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear, false, rgba);
    if (!owner->tex) return {};
    GpuImage g;
    g.id = uint64_t(reinterpret_cast<uintptr_t>(owner->tex.get()));  // what the SDK's draw loop reads back (imgui_drawer.cpp: cmd.GetTexID())
    g.keep = owner;
    return g;
  };
}

}  // namespace wwe2k14::music_picker

#if defined(__ANDROID__)
// TouchOverlay.java asks four times a second: while the picker is open the on-screen buttons hide and touches reach the window.
extern "C" JNIEXPORT jboolean JNICALL Java_io_github_jarrodfeng_wwe2k14_TouchOverlay_nativePickerOpen(JNIEnv*, jclass) {
  return wwe2k14::music_picker::PickerIsOpen() ? JNI_TRUE : JNI_FALSE;
}
// Lane 297b: the quick (250 ms) look is only worth it when the game was started with custom music on (the flag is fixed per process).
extern "C" JNIEXPORT jboolean JNICALL Java_io_github_jarrodfeng_wwe2k14_TouchOverlay_nativePickerPossible(JNIEnv*, jclass) {
  const std::string v = rex::cvar::GetFlagByName("wwe2k14_music_mix_into_game");
  return (v == "true" || v == "1") ? JNI_TRUE : JNI_FALSE;
}
#endif
