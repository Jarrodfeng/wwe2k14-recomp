// wwe2k14 - third-party Vulkan layers that crash the game at start (Session 36; GitHub issue 1, "crashing on launch",
// a player's problem report of 2026-10-18 19:38, v0.1.5-playtest, RTX 3070, driver 610.88).
//
// Overlay programs add "implicit layers" that the Vulkan loader puts into every Vulkan program. The player had seven:
// Overwolf's VK_LAYER_OW_OVERLAY (owclient.dll) and VK_LAYER_OW_OBS_HOOK (ow-graphics-vulkan.dll), FPS Monitor's
// VK_LAYER_RENDERDOC_Capture (fpsmonvk64.dll), OBS's VK_LAYER_OBS_HOOK and VK_LAYER_WEOBS_HOOK, and NVIDIA's two. The
// game crashed in vkCreateDevice (access violation 0xC0000005, before any game code): the backtrace runs vulkan-1.dll
// -> nvoglv64.dll -> fpsmonvk64.dll -> owclient.dll -> ow-graphics-hook64.dll+0xB815, the faulting frame. The loader
// logged each layer's own off switch (an environment variable named in its manifest); setting Overwolf's two, OBS's
// and FPS Monitor's in a command prompt before starting the launcher let the game start (the player, 2026-10-19).
//
// So before anything touches Vulkan (the start of OnPreSetup, ahead of the GPU pick's own vkCreateInstance) every
// variable in --wwe2k14_vulkan_disable_layers is set to 1 in this process, unless it is already set. Default:
// Overwolf's two layers only (the crash is inside Overwolf's code). OBS capture stays on for streamers; FPS Monitor's
// switch, DISABLE_LAYER, is a generic name other layers may share, so it is not set by default either. An empty
// value turns this off. Logged: "[wwe2k14][vulkan] layer switch NAME=1 ...".
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>

#include <string>

#ifdef _WIN32
#include "platform/compat.h"  // <windows.h> on Windows (Session 63)
#endif

REXCVAR_DEFINE_STRING(wwe2k14_vulkan_disable_layers, "DISABLE_VULKAN_OW_OVERLAY_LAYER,DISABLE_VULKAN_OW_OBS_CAPTURE",
                      "wwe2k14",
                      "Session 36 (GitHub issue 1): comma-separated Vulkan layer off switches (environment variables) "
                      "set to 1 before the game starts Vulkan. Default: Overwolf's overlay layers, which crashed the "
                      "game at start. Empty = none. See src/vulkan_layers.h");

namespace wwe2k14::vulkan_layers {

inline void Apply() {
#ifdef _WIN32
  const std::string list = REXCVAR_GET(wwe2k14_vulkan_disable_layers);
  size_t pos = 0;
  while (pos < list.size()) {
    size_t comma = list.find(',', pos);
    if (comma == std::string::npos) comma = list.size();
    std::string name = list.substr(pos, comma - pos);
    pos = comma + 1;
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name.empty()) continue;
    char existing[64];
    if (GetEnvironmentVariableA(name.c_str(), existing, sizeof(existing)) > 0) {
      REXLOG_INFO("[wwe2k14][vulkan] layer switch {} already set ({}), left as it is", name, existing);
      continue;
    }
    const bool ok = SetEnvironmentVariableA(name.c_str(), "1") != 0;
    REXLOG_INFO("[wwe2k14][vulkan] layer switch {}=1 {}", name, ok ? "set (that layer stays out of the game)" : "FAILED");
  }
#endif
}

}  // namespace wwe2k14::vulkan_layers
