// S102 lane G2b: the decisions of wwe2k14_rdr_exclusive_fullscreen (VK_EXT_full_screen_exclusive at the panel's own mode), kept free of Vulkan and Windows so
// tools\g2b\exclusive_test.cpp runs them with planted twins and no window. swapchain.cpp holds the only Vulkan / Win32 calls and asks this class what to do.
// docs\renderer\OUTPUT_PASS.md "Lane G2b".
//
// States (the word the log line prints):
//   off          the switch is off (or this is not Windows): nothing exclusive is created, ever.
//   unsupported  the switch is on and the driver does not list VK_EXT_full_screen_exclusive: the normal path, said once.
//   on           the switch is on, the extension is there, and vkAcquireFullScreenExclusiveModeEXT succeeded.
//   lost         the switch is on and we do not hold exclusivity: the acquire was refused, or Present / Acquire returned
//                VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT (alt-tab, Win+D, another full-screen app). From then on the swapchain is rebuilt on the
//                normal path (no exclusive structs) and stays there until the process ends: one line, no crash, no hang, no re-acquire storm.
#pragma once
#include <cstdint>
#include <cstdio>

namespace rdr::excl {

// The Vulkan values this policy needs, spelled here so the header includes nothing (VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT = -1000255000).
constexpr int kResultSuccess = 0;
constexpr int kResultSuboptimal = 1000001003;  // VK_SUBOPTIMAL_KHR
constexpr int kResultExclusiveLost = -1000255000;

enum class State : uint8_t { Off, Unsupported, On, Lost };

inline const char* Name(State s) {
  switch (s) {
    case State::Off: return "off";
    case State::Unsupported: return "unsupported";
    case State::On: return "on";
    case State::Lost: return "lost";
  }
  return "off";
}

class Policy {
 public:
  // Once, before the first swapchain: the switch, and whether the device enabled the extension.
  void Configure(bool wanted, bool extension_present) {
    wanted_ = wanted;
    state_ = !wanted ? State::Off : !extension_present ? State::Unsupported : State::Off;
    armed_ = wanted && extension_present;
  }
  bool wanted() const { return wanted_; }
  State state() const { return state_; }
  // True when the swapchain must be created with the exclusive structs chained. False for every path that is not "asked, supported, not lost":
  // this is the only gate in front of the structs, so a switch-off run builds none.
  bool ChainStructs() const { return armed_ && state_ != State::Lost; }
  // F3: the surface says fullScreenExclusive is not supported for this monitor (vkGetPhysicalDeviceSurfaceCapabilities2KHR): the normal path, "unsupported".
  void NoteSurfaceUnsupported() {
    if (!armed_) return;
    armed_ = false;
    held_ = false;
    state_ = State::Unsupported;
  }
  // F1: vkCreateSwapchainKHR refused the swapchain made with the structs. Returns true when the state changed (log a line). ChainStructs() is off from now on:
  // the caller retries once without the structs.
  bool NoteCreateFailed() {
    if (!armed_ || state_ == State::Lost) return false;
    state_ = State::Lost;
    held_ = false;
    return true;
  }
  // A swapchain was just created with the structs: the result of vkAcquireFullScreenExclusiveModeEXT. Returns true when the state changed (log a line).
  bool NoteAcquire(int result) {
    if (!armed_) return false;
    const State was = state_;
    state_ = result == kResultSuccess ? State::On : State::Lost;
    held_ = result == kResultSuccess;
    return state_ != was;
  }
  // Review (F4 corrected): exclusive access lasts until it is released or the swapchain is destroyed; it does NOT carry over through oldSwapchain.
  // So a rebuild releases the old swapchain BEFORE vkCreateSwapchainKHR (NoteReleased) and every create with the structs chained is followed by an acquire.
  void NoteReleased() { held_ = false; }
  bool AcquireAfterCreate(bool chained) const { return chained; }
  // F2: every LOST result needs the swapchain rebuilt on the normal path, whether or not it is the first (a repeat must not leave the old swapchain in use).
  bool NeedsRebuild(int result) const { return armed_ && result == kResultExclusiveLost; }
  // Every vkQueuePresentKHR / vkAcquireNextImageKHR result. Returns true on the transition to lost: the one log line.
  bool NoteResult(int result) {
    if (result != kResultExclusiveLost || !armed_ || state_ == State::Lost) return false;
    state_ = State::Lost;
    held_ = false;
    return true;
  }
  // Whether to call vkReleaseFullScreenExclusiveModeEXT before the swapchain goes.
  // True only while the current swapchain holds exclusive access (not after a release, a loss, a refused acquire or a failed create).
  bool ShouldRelease() const { return armed_ && held_; }

  // The one line (docs say: `[gfx][present] exclusive <on|off|lost|unsupported> mode WxH@Hz`); hz 0 = the panel's rate could not be read.
  static int Line(char* out, size_t size, State s, uint32_t w, uint32_t h, uint32_t hz) {
    return std::snprintf(out, size, "[gfx][present] exclusive %s mode %ux%u@%u", Name(s), unsigned(w), unsigned(h), unsigned(hz));
  }

 private:
  bool wanted_ = false;
  bool armed_ = false;  // wanted and the extension exists
  bool held_ = false;   // the current swapchain holds exclusive access
  State state_ = State::Off;
};

}  // namespace rdr::excl
