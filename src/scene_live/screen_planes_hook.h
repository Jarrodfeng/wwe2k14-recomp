// The display-set flush entry for the stage screens' live pictures (S103 lane M). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Dependency-free on purpose: d3d_textures.h includes THIS (the existing flush hook of sub_827FB540 already runs at every movie flush) and the glue (screen_planes_glue.h) installs the function.
// With --wwe2k14_rdr_scene = 0 the call costs one relaxed load of the mode snapshot and does nothing else (no guest read, no copy, no lock).
#pragma once
#include <atomic>
#include <cstdint>

#include "../renderer_game/scene_frame.h"

namespace wwe2k14::scene_screens {

inline std::atomic<void (*)(uint8_t* base, uint32_t display_set)> g_on_flush{nullptr};

inline void OnDisplayFlush(uint8_t* base, uint32_t display_set) {
  if (scene_frame::CurrentMode() == scene_frame::Mode::Off) return;
  if (auto f = g_on_flush.load(std::memory_order_relaxed)) f(base, display_set);
}

}  // namespace wwe2k14::scene_screens

// S103 M (diagnosis for the key -> channel binding): the existing part-draw hook (draw_origin.h, sub_82BDED40) calls this with the draw's model object, part index and call site.
// Only the screens' pass site (0x82C0F4BC, sub_82C0CA08) is of interest; the glue logs the model's handle table once per (model, part). Off at mode 0 after one relaxed load.
namespace wwe2k14::scene_screens {
inline std::atomic<void (*)(uint8_t* base, uint32_t model, uint32_t part)> g_on_part{nullptr};
inline constexpr uint32_t kScreensPassSite = 0x82C0F4BC;
inline void OnPartDraw(uint8_t* base, uint32_t model, uint32_t part, uint32_t site) {
  if (site != kScreensPassSite) return;
  if (scene_frame::CurrentMode() == scene_frame::Mode::Off) return;
  if (auto f = g_on_part.load(std::memory_order_relaxed)) f(base, model, part);
}
}  // namespace wwe2k14::scene_screens
