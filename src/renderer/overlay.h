// Settings parity S11 (Session 100): the app's overlay - its Dear ImGui dialogs (the performance overlay, the CineCam panel, prompts) - drawn by
// the new renderer over the picture at each present. The game side records ImGui's draw lists into an OverlayList on the UI thread
// (renderer_adapter/overlay_drawer.h); the live sink hands the newest one to the renderer (Renderer::SetOverlay), which draws it in the present
// pass at the window's full size (renderer.cpp RecordPresent). No SDK or game includes.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

namespace rdr {

// Pictures the new renderer has handed to the screen (each present; the decoupled 120's in-betweens included). The performance overlay
// shows their rate beside the game's own (Jarrod's phone test: the graph, the game's frames, never showed the in-betweens).
inline std::atomic<uint64_t> g_presented{0};

// Lane 269: the GPU time of every submitted frame (two timestamps around its command buffer, read a few frames late without stalling).
// Total ns and count since the start, and the newest frame's time in microseconds. Zero count = no timestamps (off, or the queue has none).
inline std::atomic<uint64_t> g_gpu_ns_total{0};
inline std::atomic<uint64_t> g_gpu_frames{0};
inline std::atomic<uint32_t> g_gpu_last_us{0};

struct OverlayVertex {  // the layout of Dear ImGui's ImDrawVert and the SDK's ImmediateVertex
  float x, y;           // in the list's coordinate space (0..width, 0..height: the window, top-left origin)
  float u, v;
  uint32_t color;       // RGBA8 (R in the low byte)
};

struct OverlayDraw {
  uint32_t texture = 0;     // an id given to Renderer::SetOverlayTexture; 0 = plain white
  uint32_t first_index = 0, count = 0;
  int32_t base_vertex = 0;
  bool clipped = false;
  float clip[4] = {0, 0, 0, 0};  // left, top, right, bottom in the list's coordinate space
};

struct OverlayList {
  float width = 0, height = 0;  // the coordinate space (the window in ImGui's logical units)
  std::vector<OverlayVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<OverlayDraw> draws;
};

}  // namespace rdr
