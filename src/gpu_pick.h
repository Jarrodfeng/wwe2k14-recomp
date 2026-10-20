// Session 31 (a playtester's laptop started the game on its Intel integrated GPU): which graphics card the game uses.
//
// The SDK's `vulkan_device` (an index into vkEnumeratePhysicalDevices' list; -1 = auto) auto-picks by four feature
// bits only and keeps the driver's order on a tie, so an integrated GPU listed first wins
// (thirdparty\rexglue-sdk-src\src\ui\vulkan\vulkan_provider.cpp:78-139; a forced index that fails falls back to
// auto). Before the GPU starts (OnPreSetup), when `vulkan_device` is left at its default, Choose() lists the Vulkan
// devices itself (the same loader, so the same order; Session 37: src\vk_devices.h) and sets `vulkan_device` to:
//   --wwe2k14_gpu=NAME   the first device whose name contains NAME (any case; the launcher's chooser), else
//   the discrete GPU with the most device-local memory, else leaves auto.
// Every device and the choice go in the log ("[wwe2k14][gpu]"). The SDK's own list follows at WARN ("Available
// Vulkan physical devices"), so the index can be checked against it.
// Session 37: also what the chosen device is (g_chosen: integrated? interlock for the accurate render path?), for the
// light renderer (src\light_renderer.h). With no discrete GPU and no name, the SDK still picks; g_chosen is then the
// first device (the only one on most such PCs).

#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "vk_devices.h"

REXCVAR_DEFINE_STRING(wwe2k14_gpu, "", "wwe2k14",
                      "Graphics card: part of its name (e.g. 'RTX 4090'). Empty = the dedicated GPU with the most memory "
                      "(Session 31: the SDK's auto choice can take an integrated GPU). Ignored when --vulkan_device is set");

namespace wwe2k14::gpu_pick {

inline bool g_known = false;  // g_chosen is filled in
inline vkdev::Device g_chosen;

inline void Choose() {
#ifdef _WIN32
  const std::vector<vkdev::Device> devs = vkdev::List();
  if (devs.empty()) {
    REXLOG_WARN("[wwe2k14][gpu] no Vulkan devices listed: the SDK picks the device");
    return;
  }
  for (size_t i = 0; i < devs.size(); ++i)
    REXLOG_INFO("[wwe2k14][gpu] device {}: {} ({}, {} MB device-local, interlock {}, vendor 0x{:04X})", i, devs[i].name,
                devs[i].type_text(), devs[i].local_mb, devs[i].interlock ? "yes" : "no", devs[i].vendor);
  if (rex::cvar::GetFlagSource("vulkan_device") != rex::cvar::Source::kDefault) {
    const int forced = std::atoi(rex::cvar::GetFlagByName("vulkan_device").c_str());
    if (forced >= 0 && forced < int(devs.size())) g_chosen = devs[size_t(forced)], g_known = true;
    REXLOG_INFO("[wwe2k14][gpu] vulkan_device was set: left as it is");
    return;
  }
  const std::string want = REXCVAR_GET(wwe2k14_gpu);
  const int pick = vkdev::Pick(devs, want);
  g_chosen = devs[size_t(pick)], g_known = true;
  const bool by_name = !want.empty() && vkdev::Lower(devs[size_t(pick)].name).find(vkdev::Lower(want)) != std::string::npos;
  if (!want.empty() && !by_name)
    REXLOG_WARN("[wwe2k14][gpu] no device name contains '{}': using the automatic choice", want);
  if (!by_name && !devs[size_t(pick)].discrete()) {
    REXLOG_INFO("[wwe2k14][gpu] no discrete GPU: the SDK picks the device");
    return;
  }
  rex::cvar::SetFlagByName("vulkan_device", std::to_string(pick));
  REXLOG_INFO("[wwe2k14][gpu] vulkan_device = {} ({})", pick, by_name ? "--wwe2k14_gpu" : "the discrete GPU with the most memory");
#endif
}

}  // namespace wwe2k14::gpu_pick
