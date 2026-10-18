// wwe2k14 - matte passes (Session 16, item 7). Default off.
//
// --wwe2k14_matte=depth           whenever a screenshot is taken (input script `screenshot` / `screenshot_every`),
//                                 also dump the frame's depth: arms the SDK's gpu_depth_dump (patch 0012: every depth
//                                 resolve until the next guest swap, read back synchronously into
//                                 captures/NNN_LABEL_depth_K.bin + .json, guest memory left as it was) and writes
//                                 captures/NNN_LABEL_matte.json with the camera's near / far planes (render view
//                                 +500 / +504, read from the view the depth-of-field converter sub_827F14C0 last used:
//                                 the same planes the game turns into depth-buffer values). Then
//                                 `py tools\matte\depth.py out\build\win-amd64-release\captures` writes
//                                 NNN_LABEL_depth.png (16-bit, linear: white = near plane, black = far plane) + .json.
//                                 The depth comes from the first frame rendered after the screenshot request (the
//                                 screenshot captures the frame before): about one frame apart (unconfirmed exactly).
// "mask" (wrestler mask) is not implemented: see PLAYBOOK Session 16 item 7 for the approach.
#pragma once

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "frame_stats.h"
#include "input_script.h"

REXCVAR_DEFINE_STRING(wwe2k14_matte, "", "wwe2k14",
                      "Matte passes with every screenshot (Session 16): 'depth' = the frame's depth buffer (dump + "
                      "near / far; tools/matte/depth.py makes the 16-bit PNG). Empty = off. See src/matte.h");

namespace wwe2k14::matte {

// The render view the depth-of-field converter last ran for (cinecam.h hooks sub_827F14C0 and records it).
inline std::atomic<uint32_t> g_last_dof_view{0};

inline float GuestF32(uint32_t address) {
  const uint8_t* p = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(address);
  uint32_t v = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// Game thread, at a screenshot step (before the capture thread starts).
inline void OnScreenshot(const std::string& label) {
  const std::string spec = REXCVAR_GET(wwe2k14_matte);
  if (spec.find("depth") == std::string::npos) return;
  const auto dir = std::filesystem::current_path() / "captures";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  char prefix[16];
  std::snprintf(prefix, sizeof(prefix), "%03d_", std::max(CurrentLogNumber(), 0));
  const std::filesystem::path base = dir / (prefix + label);
  const uint32_t view = g_last_dof_view.load(std::memory_order_relaxed);
  float zn = 0, zf = 0;
  if (view) zn = GuestF32(view + 500), zf = GuestF32(view + 504);
  if (std::FILE* f = std::fopen((base.string() + "_matte.json").c_str(), "w")) {
    std::fprintf(f, "{\"label\": \"%s\", \"frame\": %llu, \"render_view\": %u, \"near\": %.6g, \"far\": %.6g}\n",
                 label.c_str(), (unsigned long long)g_guest_frame.load(), view, zn, zf);
    std::fclose(f);
  }
  rex::cvar::SetFlagByName("gpu_depth_dump", base.string() + "_depth");
  REXLOG_INFO("[wwe2k14][matte] depth requested for '{}' (near {}, far {}, render view {:08X}) -> {}_depth_K.bin",
              label, zn, zf, view, base.string());
}

inline void Install() {
  const std::string spec = REXCVAR_GET(wwe2k14_matte);
  if (spec.empty()) return;
  g_screenshot_hook = &OnScreenshot;
  REXLOG_INFO("[wwe2k14][matte] mattes with every screenshot: {}{}", spec,
              spec.find("mask") != std::string::npos ? " (mask: not implemented, ignored)" : "");
}

}  // namespace wwe2k14::matte
