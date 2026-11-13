#pragma once
// wwe2k14 - Session 101 lane 241 (docs\renderer\LEAVE_THE_COMMAND_STREAM.md 3b wiring): the exe's one flag for "the SDK runs guest_gpu_only" (SDK patch 113,
// RuntimeConfig::guest_gpu_only: no graphics provider, no command processor, no presenter; the MMIO range and the VSync thread only). Set once in OnPreSetup
// (src\wwe2k14_app.h) when --wwe2k14_owner_guest_gpu_only is on and its two preconditions hold; false for the whole run otherwise. Every game-side user of the
// SDK's graphics objects that would act on an absent one reads this instead of asking the runtime each call (half2::Cp() is on the draw path).
#include <atomic>

namespace wwe2k14 {
inline std::atomic<bool> g_guest_gpu_only{false};
inline bool GuestGpuOnly() { return g_guest_gpu_only.load(std::memory_order_relaxed); }
}  // namespace wwe2k14
