// Settings parity S11 (Session 100): the app's Dear ImGui dialogs (the performance overlay, the CineCam panel, the check banner, the
// inspector, the upscale notice) drawn by the new renderer when it owns the window. With presentation detached the SDK asks the app for its
// own immediate drawer (SDK 88e4077, rex_app.cpp): this one records ImGui's draw lists on the UI thread into an rdr::OverlayList and hands
// each finished list to the live sink, whose render thread draws the newest one over every picture (renderer.cpp RecordOverlay). The UI
// thread paints once per game present (RequestPaint, from the reader's OnPresent): the SDK's ImGuiDrawer::Draw runs the dialogs there, as
// the SDK presenter's UI paint did.
// S103 AIM: the drawer body is src\renderer_host\overlay_drawer_core.h, shared with the stream-free build (overlay_off.h); only the sink differs.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>

#include "../renderer/overlay.h"
#include "../renderer_host/overlay_drawer_core.h"
#include "../renderer_scene/swap_once.h"   // S102 SW1: the census counters of the paint-request skip
#include "live.h"

namespace wwe2k14::rdr_overlay {

// The stream build's sink: the live sink's mailbox (live.cpp). It frees no texture (the stream's renderer keeps its ids until replaced: unchanged).
struct LiveSinkRef {
  rdr_adapter::LiveSink& sink;
  LiveSinkRef(rdr_adapter::LiveSink& s) : sink(s) {}
  void SetTexture(uint32_t id, uint32_t width, uint32_t height, std::vector<uint8_t>&& rgba) { sink.SetOverlayTexture(id, width, height, std::move(rgba)); }
  void SetList(std::shared_ptr<const rdr::OverlayList> list) { sink.SetOverlay(std::move(list)); }
  void ReleaseTexture(uint32_t) {}
};

using Drawer = rdr_overlay_core::BasicDrawer<LiveSinkRef>;
using Context = rdr_overlay_core::Context;

inline Drawer* g_drawer = nullptr;                // set by the app's OnCreateImmediateDrawer (owner mode)
inline std::atomic<bool> g_paint_pending{false};  // one UI paint queued at a time

// S102 SW1 (wwe2k14_rdr_swap_once bit 4): whether a paint request has anything to do. The UI thread's task does exactly this much when no dialog is open: `Clear()`, which returns at once unless a list is
// still on the screen. So with no dialog and nothing on the screen the request (a std::function, a post to the UI thread, a wake-up) is not made at all, and the next present asks again. The
// dialog list is read without the UI thread's lock (ImGuiDrawer::HasDialogs is a plain `!dialogs_.empty()`: a benign formal race, two pointers compared and never dereferenced). The worst case is one
// present early or late in showing a dialog; nothing flickers, since an overlay list stays on screen until SetOverlay changes it and a closed dialog still gets its one Clear().
inline bool PaintHasWork(rex::ui::ImGuiDrawer* imgui, bool skip_idle) {
  return rdr_scene::swap_once::OverlayHasWork(skip_idle, imgui->HasDialogs(), g_drawer && g_drawer->Shown());
}

// From the game's present (any thread): the UI thread runs the dialogs once and records them (`post` = the app context's CallInUIThread).
// `skip_idle` = swap_once bit 4: no request while no dialog is open and no list is on the screen.
inline void RequestPaint(const std::function<void(std::function<void()>)>& post, rex::ui::Window* window, rex::ui::ImGuiDrawer* imgui, bool skip_idle = false) {
  if (!g_drawer || !window || !imgui) return;
  if (!PaintHasWork(imgui, skip_idle)) {
    rdr_scene::swap_once::g_count.overlay_skipped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (g_paint_pending.exchange(true)) return;
  rdr_scene::swap_once::g_count.overlay_posted.fetch_add(1, std::memory_order_relaxed);
  post([window, imgui] {
    g_paint_pending.store(false);
    if (!imgui->HasDialogs()) {
      g_drawer->Clear();
      return;
    }
    Context context(window->GetActualPhysicalWidth(), window->GetActualPhysicalHeight());
    imgui->Draw(context);
  });
}

}  // namespace wwe2k14::rdr_overlay
