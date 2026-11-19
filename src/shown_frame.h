// S103 lane SHOT: ONE way to take the picture the player is looking at, for the input-script `screenshot` command, photo mode, clip capture, the bookmark grabber and the diag shots.
// With --wwe2k14_rdr_owner the SDK presenter is detached (it never saw our picture: CaptureGuestOutput answered "no guest output"), so the picture is the one OUR renderer last presented:
// the owner (the scene host, src\renderer_host\scene_host.cpp, or the stream's LiveSink in a stream build) reads it back ON REQUEST ONLY (a pending flag, one copy after a present, handed to the
// waiting thread with a timeout), so nothing is paid while nobody asks. Not in owner mode: the SDK presenter, exactly as before.
// RawImage layout is the SDK capture's: R8 G8 B8 X8, stride = width * 4 (X8 = 255).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/presenter.h>

REXCVAR_DECLARE(bool, wwe2k14_rdr_owner);  // (src\renderer_adapter\reader.h / src\renderer_host\host_start.cpp)

namespace wwe2k14 {

// Set once at start by whoever owns the window (wwe2k14_app.h), before any capture thread runs. Takes `image` (data moved in is reused), fills it from the last presented picture; false = none yet / timed out.
inline std::function<bool(rex::ui::RawImage&)> g_shown_frame_owner;

// RGBA8 rows (tightly packed, w * h * 4, alpha already 255 or whatever the renderer wrote) -> a RawImage in the SDK capture's layout. The owners and the offline test share it.
inline void FillRawImage(rex::ui::RawImage& image, const uint8_t* rgba, uint32_t width, uint32_t height) {
  image.width = width, image.height = height, image.stride = size_t(width) * 4;
  image.data.assign(rgba, rgba + size_t(width) * height * 4);
  for (size_t i = 3; i < image.data.size(); i += 4) image.data[i] = 255;  // X8
}

// S103 FLAT: the guest frame number of the picture the LAST CaptureShownFrame on THIS thread returned (set by the owner, read right after the call on the same thread; ~0 = unknown: not owner mode, or the owner does not say).
// A screenshot is "the one last presented", frames behind the guest line that asked: this is how a log line states which frame it really was.
// S103 RBC: the game's GPU read-backs (replay-clip thumbnail, Create an Arena screenshot) want the 3D SCENE of the newest OURS frame: `out`, raw RGBA8 (rw x rh tight), no 2D rows, no HUD, and no display curve (the game's
// own pipeline applies its ramp after the CPU reads the pixels). Set by the stream-free host (host_start.cpp; null in the stream build). Request: any thread, never blocks; the answer callback runs ONCE on the host thread
// (keep it short). Capture: blocks up to `timeout_ms`; false = no scene (a menu, a loading screen), a failed read or a timeout.
using SceneAnswer = std::function<void(bool ok, std::vector<uint8_t>&& rgba, uint32_t w, uint32_t h, uint64_t guest_frame)>;
inline std::function<bool(SceneAnswer)> g_scene_request_owner;
inline std::function<bool(std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h, uint32_t timeout_ms, uint64_t* guest_frame)> g_scene_capture_owner;

inline thread_local uint64_t t_shown_guest_frame = ~0ull;
inline constexpr uint64_t kShownFrameUnknown = ~0ull;

inline bool CaptureShownFrame(rex::ui::RawImage& image) {
  t_shown_guest_frame = kShownFrameUnknown;
  if (REXCVAR_GET(wwe2k14_rdr_owner)) return g_shown_frame_owner && g_shown_frame_owner(image);  // (no owner yet / none in this build: false, never the idle SDK presenter)
  auto* state = REX_KERNEL_STATE();
  auto* runtime = state ? state->emulator() : nullptr;
  auto* graphics = runtime ? runtime->graphics_system() : nullptr;
  auto* presenter = graphics ? graphics->presenter() : nullptr;
  return presenter && presenter->CaptureGuestOutput(image);
}

}  // namespace wwe2k14
