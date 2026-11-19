// S103 lane AIM: the stream-free build's overlay mailbox (docs\research\s103\AIM.md). The app's Dear ImGui dialogs (music picker, perf overlay, SDK toasts) are recorded on the UI
// thread by overlay_off.h's Drawer into an rdr::OverlayList and left HERE; the scene host drains it on its own thread just before it renders a presented frame (SceneHost, after
// fb.Finish()) into Renderer::SetOverlay / SetOverlayTexture, and RecordPresent draws it over the picture. STATIC, not a SceneHost member: the SDK makes the drawer in
// SetupPresentation, before StartForGame creates the host, and ImGui's font atlas is uploaded on the first Draw; queued textures wait here for the first frame.
// No SDK and no game includes (the host rig links it). Replaces the stream build's LiveSink::SetOverlay / ApplyOverlay (live.cpp) for the OFF build only.
// Also here (SDK-free, so the rig proves them): the toast clock and the paint-request decision (review fix 1: the SDK's achievement toast dialog is always registered, so "no dialog" must mean
// "no dialog but an idle toast").
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "../renderer/overlay.h"
#include "../renderer/renderer.h"
#include "../renderer_scene/swap_once.h"

namespace wwe2k14::scene_host::overlay_mailbox {

struct PendingTexture {
  uint32_t id = 0, width = 0, height = 0;
  std::vector<uint8_t> rgba;
};

inline std::mutex g_mu;
inline std::vector<PendingTexture> g_textures;               // new textures, in creation order (g_mu)
inline std::vector<uint32_t> g_released;                     // ids whose ImGui texture object is gone (g_mu); ids are never reused
inline std::shared_ptr<const rdr::OverlayList> g_next;       // the newest list; null = take the overlay off the screen (g_mu)
inline bool g_changed = false;                               // g_next is newer than what the renderer has (g_mu)

// UI thread.
inline void SetTexture(uint32_t id, uint32_t width, uint32_t height, std::vector<uint8_t>&& rgba) {
  std::lock_guard lock(g_mu);
  g_textures.push_back(PendingTexture{id, width, height, std::move(rgba)});
}
inline void SetList(std::shared_ptr<const rdr::OverlayList> list) {
  std::lock_guard lock(g_mu);
  g_next = std::move(list);
  g_changed = true;
}
inline void ReleaseTexture(uint32_t id) {   // the ImGui side destroyed the texture (any thread)
  std::lock_guard lock(g_mu);
  g_released.push_back(id);
}

// Host thread, once per presented frame before RenderFrame (the same body as live.cpp ApplyOverlay, plus the frees). Returns whether anything was handed over.
inline bool Drain(rdr::Renderer& renderer) {
  std::vector<PendingTexture> textures;
  std::vector<uint32_t> released;
  std::shared_ptr<const rdr::OverlayList> list;
  bool changed = false;
  {
    std::lock_guard lock(g_mu);
    textures.swap(g_textures);
    released.swap(g_released);
    if (g_changed) list = g_next, changed = true, g_changed = false;
  }
  for (const PendingTexture& t : textures) renderer.SetOverlayTexture(t.id, t.width, t.height, t.rgba.data());
  if (changed) renderer.SetOverlay(std::move(list));   // (the list goes first: a freed id is no longer named by the list that replaced the old one)
  for (uint32_t id : released) renderer.RemoveOverlayTexture(id);
  return changed || !textures.empty() || !released.empty();
}

// ---- the toast clock -----------------------------------------------------------------------------------------------------------------------------------
// The SDK's AchievementToastDialog queues events (Push, guest threads) and shows each for 4.5 s once visible; its queue is private. The app's toast (overlay_off.h) calls Push here, which keeps
// "busy until": each push adds one toast's time (4.5 s + 1 s margin) after the later of now and the previous end. Idle = past it.
inline constexpr int64_t kToastEachNs = 5'500'000'000;
inline std::atomic<int64_t> g_toast_busy_until_ns{0};
inline int64_t SteadyNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void ToastPushed(int64_t now_ns = SteadyNs()) {
  int64_t seen = g_toast_busy_until_ns.load(std::memory_order_relaxed);
  while (!g_toast_busy_until_ns.compare_exchange_weak(seen, std::max(seen, now_ns) + kToastEachNs, std::memory_order_acq_rel)) {}
}
inline bool ToastIdle(int64_t now_ns = SteadyNs()) { return now_ns >= g_toast_busy_until_ns.load(std::memory_order_acquire); }

// ---- the paint request's decision ----------------------------------------------------------------------------------------------------------------------
// `other_dialogs`: some ImGui dialog besides the toast is open; `toast_idle`: no toast queued or showing; `shown`: a list is on the screen. false = skipped (counted in `overlay requests: skipped`).
inline bool WantPaint(bool skip_idle, bool other_dialogs, bool toast_idle, bool shown) {
  if (rdr_scene::swap_once::OverlayHasWork(skip_idle, other_dialogs || !toast_idle, shown)) return true;
  rdr_scene::swap_once::g_count.overlay_skipped.fetch_add(1, std::memory_order_relaxed);
  return false;
}

}  // namespace wwe2k14::scene_host::overlay_mailbox
