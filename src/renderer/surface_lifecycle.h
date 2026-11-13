// Lane 235: the window-surface lifecycle of the renderer's swapchain, as pure logic (no Vulkan), so a host test can drive it.
//
// Why: on Android the ANativeWindow is destroyed when the app goes to the background and a new one is made on return. The SDK fixed
// this for its own presenter (f5d75cd, Session 71); in owner mode (S100 stage B) our swapchain is the one on the window, so it must do
// the same: stop presenting and release the VkSurfaceKHR in the background, make a new surface + swapchain from the NEW window pointer
// on return, and treat VK_ERROR_SURFACE_LOST_KHR as "make a new one", never as a stall.
//
// Threads: OnBackground / OnForeground may come from any thread (SDL's event watcher); they only set atomics. Decide / Created / Lost
// are the present thread's (the Swapchain's owner). Nothing here blocks.
#pragma once
#include <atomic>
#include <cstdint>

namespace rdr {

// The app's background / foreground events: any thread, atomics only.
class SurfaceEvents {
 public:
  // SDL_EVENT_WILL_ENTER_BACKGROUND / DID_ENTER_BACKGROUND (repeats are harmless).
  void OnBackground() {
    background_.store(true, std::memory_order_release);
    epoch_.fetch_add(1, std::memory_order_acq_rel);
  }
  // SDL_EVENT_DID_ENTER_FOREGROUND: SDL sends it once the new window surface is ready. Forces one re-create even when the pointer value
  // is the same as before (Android may hand the new ANativeWindow the old one's address).
  void OnForeground() {
    background_.store(false, std::memory_order_release);
    epoch_.fetch_add(1, std::memory_order_acq_rel);
  }
  bool background() const { return background_.load(std::memory_order_acquire); }
  uint32_t epoch() const { return epoch_.load(std::memory_order_acquire); }

 private:
  std::atomic<bool> background_{false};
  std::atomic<uint32_t> epoch_{0};
};

// The process's one window: the app's event watcher feeds this, the swapchain reads it.
inline SurfaceEvents& WindowEvents() {
  static SurfaceEvents events;
  return events;
}

class SurfaceLifecycle {
 public:
  explicit SurfaceLifecycle(SurfaceEvents* events = nullptr) : events_(events ? events : &own_) {}

  enum class Action {
    kPresent,  // the surface is good: acquire and present
    kSkip,     // nothing to draw to right now (background, no window yet, waiting to retry a failed create): no acquire
    kRelease,  // destroy the swapchain and surface; nothing to create from yet
    kCreate,   // destroy any surface/swapchain and make new ones from `current_window`
  };

  // Android (a window source exists): a loss on an unchanged pointer waits for an app-state event.
  void SetWaitForEventOnLoss(bool wait) { wait_for_event_on_loss_ = wait; }
  void OnBackground() { events_->OnBackground(); }
  void OnForeground() { events_->OnForeground(); }

  // Present thread, once per frame before acquiring. current_window: the window's native handle right now (null = none).
  Action Decide(void* current_window) {
    // Epoch first, then the flag: OnBackground stores the flag before it bumps the epoch, so a new epoch always comes with its flag.
    const uint32_t epoch = events_->epoch();
    const bool background = events_->background();
    if (epoch != seen_epoch_) {
      seen_epoch_ = epoch;
      force_ = true;
      retry_wait_ = 0;
    }
    // Phone run 051 (SIGSEGV right after "window surface released"): in the background we touch NO Vulkan object. Releasing the
    // swapchain / surface at WILL_ENTER_BACKGROUND ran (vkDeviceWaitIdle, destroy) while Android was tearing the window down
    // (trim level 20 followed 27 ms later) and the process died. The old surface is kept (it also keeps the old window alive); it is
    // destroyed only by the re-create on the foreground event, long after the window is gone, and nothing is acquired or presented
    // meanwhile.
#ifdef WWE2K14_LIFE_PLANT_RELEASE_IN_BACKGROUND
    if (background || !current_window) return has_surface_ ? ReleaseNow() : Action::kSkip;  // plant: the run-051 behaviour
#endif
    if (background || !current_window) return Action::kSkip;
    if (has_surface_ && !force_ && !lost_ && current_window == bound_) return Action::kPresent;
    // Lane 235 review SF1 (Android): SDL releases the ANativeWindow when the surface is destroyed but leaves its pointer property
    // alone, and our background event follows later. A loss on the SAME pointer with no event since is therefore not a reason to
    // create: our VkSurface is the reference keeping that window alive, and creating from a freed window is a use-after-free. Wait
    // for the event (or a different pointer). Without a window source (PC) a loss re-creates at once.
    if (has_surface_ && lost_ && !force_ && current_window == bound_ && wait_for_event_on_loss_ &&
        !PlantRecreateOnLoss()) return Action::kSkip;
    if (retry_wait_ > 0) {  // a failed create: not every frame
      --retry_wait_;
      return Action::kSkip;
    }
#ifdef WWE2K14_LIFE_PLANT_NO_RECREATE
    if (has_surface_) return Action::kPresent;  // plant: the old surface kept after a new window / a loss
#endif
    return Action::kCreate;
  }

  // Present thread: result of a kCreate (ok = surface, swapchain format check and swapchain all made).
  void Created(void* window, bool ok) {
    force_ = false;
    lost_ = false;
    has_surface_ = ok;
    bound_ = ok ? window : nullptr;
    retry_wait_ = ok ? 0 : kRetryFrames;
  }
  // Present thread: kRelease done.
  void Released() {
    has_surface_ = false;
    bound_ = nullptr;
    lost_ = false;
    force_ = false;
  }
  // Present thread: acquire or present returned VK_ERROR_SURFACE_LOST_KHR.
  void Lost() { lost_ = true; }

  bool has_surface() const { return has_surface_; }
  void* bound() const { return bound_; }

  static constexpr int kRetryFrames = 30;

 private:
  Action ReleaseNow() { return Action::kRelease; }
  static constexpr bool PlantRecreateOnLoss() {
#ifdef WWE2K14_LIFE_PLANT_RECREATE_ON_LOSS
    return true;  // plant: SF1 guard removed
#else
    return false;
#endif
  }

  SurfaceEvents own_;
  SurfaceEvents* events_;
  bool wait_for_event_on_loss_ = false;
  uint32_t seen_epoch_ = 0;
  bool force_ = false;
  bool lost_ = false;
  bool has_surface_ = false;
  void* bound_ = nullptr;
  int retry_wait_ = 0;
};

}  // namespace rdr
