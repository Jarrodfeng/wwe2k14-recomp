// S103 lane AIM (docs\research\s103\AIM.md): the stream-free (WWE2K14_KEEP_STREAM=OFF) build's ImGui drawer. The drawer body is overlay_drawer_core.h (shared with the stream build's
// src\renderer_adapter\overlay_drawer.h); its sink is the static overlay mailbox (overlay_mailbox.h), which the scene host drains onto the renderer (scene_host.cpp). The paint cadence
// (RequestPaint, once per game present, skipped while idle) is called from the host's present hook (host_start.h g_on_present). The SDK asks the app for this drawer in owner mode.
//
// Review fix 1: the SDK registers an achievement toast dialog at LaunchModule that stays in the drawer's dialog list for the whole run, so `HasDialogs()` is always true. The app returns OUR toast
// (OffToast: the SDK's AchievementToastDialog, same look and queue, plus a clock of when its queue is empty); RequestPaint counts the drawer's dialogs, discounts the toast, and skips while no other
// dialog is open, the toast is idle and no list is on the screen.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/overlay/achievement_toast.h>
#include <rex/ui/window.h>

#include "overlay_drawer_core.h"
#include "overlay_mailbox.h"

namespace wwe2k14::rdr_overlay_off {
namespace mb = wwe2k14::scene_host::overlay_mailbox;

struct MailboxSink {
  void SetTexture(uint32_t id, uint32_t width, uint32_t height, std::vector<uint8_t>&& rgba) { mb::SetTexture(id, width, height, std::move(rgba)); }
  void SetList(std::shared_ptr<const rdr::OverlayList> list) { mb::SetList(std::move(list)); }
  void ReleaseTexture(uint32_t id) { mb::ReleaseTexture(id); }
};

using Drawer = rdr_overlay_core::BasicDrawer<MailboxSink>;
using Context = rdr_overlay_core::Context;

inline Drawer* g_drawer = nullptr;                // set by the app's OnCreateImmediateDrawer (owner mode)
inline std::atomic<bool> g_paint_pending{false};  // one UI paint queued at a time
inline std::atomic<bool> g_toast_registered{false};

// Our achievement toast: the SDK's own dialog (queue, look, icons), with the clock the paint request reads. Push runs on guest threads.
class OffToast final : public rex::ui::AchievementToastDialog {
 public:
  OffToast(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate_drawer, rex::Runtime* runtime) : AchievementToastDialog(drawer, immediate_drawer, runtime) {
    g_toast_registered.store(true, std::memory_order_release);
  }
  ~OffToast() override { g_toast_registered.store(false, std::memory_order_release); }
  void Push(const rex::system::AchievementEvent& event) override {
    mb::ToastPushed();   // before the event is queued: the next present's request already sees "busy"
    AchievementToastDialog::Push(event);
  }
};

// The number of dialogs the drawer holds. `dialogs_` is private and has no count; it is read through an explicit instantiation (the standard way past `private`: compile-checked, no #define), without the
// UI thread's lock (the same benign formal race as ImGuiDrawer::HasDialogs: a size read, nothing dereferenced).
struct DialogsTag { using type = std::vector<rex::ui::ImGuiDialog*> rex::ui::ImGuiDrawer::*; friend type DialogsOf(DialogsTag); };
template <class Tag, typename Tag::type M> struct Rob { friend typename Tag::type DialogsOf(Tag) { return M; } };
template struct Rob<DialogsTag, &rex::ui::ImGuiDrawer::dialogs_>;
inline bool OtherDialogsOpen(rex::ui::ImGuiDrawer* imgui) {
  const size_t n = (*imgui.*DialogsOf(DialogsTag{})).size();
  return n > (g_toast_registered.load(std::memory_order_acquire) ? 1u : 0u);
}

// From the game's present (any thread): the UI thread runs the dialogs once and records them (`post` = the app context's CallInUIThread).
inline void RequestPaint(const std::function<void(std::function<void()>)>& post, rex::ui::Window* window, rex::ui::ImGuiDrawer* imgui) {
  if (!g_drawer || !window || !imgui) return;
  if (!mb::WantPaint(true, OtherDialogsOpen(imgui), mb::ToastIdle(), g_drawer->Shown())) return;
  if (g_paint_pending.exchange(true)) return;
  rdr_scene::swap_once::g_count.overlay_posted.fetch_add(1, std::memory_order_relaxed);
  post([window, imgui] {
    g_paint_pending.store(false);
    if (!OtherDialogsOpen(imgui) && mb::ToastIdle()) {   // nothing to show: the last list (a finished toast, a closed dialog) comes off the screen, once
      g_drawer->Clear();
      return;
    }
    Context context(window->GetActualPhysicalWidth(), window->GetActualPhysicalHeight());
    imgui->Draw(context);
  });
}

}  // namespace wwe2k14::rdr_overlay_off
