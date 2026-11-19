// S103 L7: the state of the maps' filter chains (live_map_chain.cpp). Not an interface for other lanes.
#pragma once
#include <cstdint>

#include "../renderer/api.h"
#include "../renderer/frame_builder.h"

namespace rdr_scene {

// The game's chains behind the two wrestler maps, as OUR passes over the raw maps lane E / E2 make (docs\renderer\LIVE_MAPS.md "Lane L7"):
//   shadow: raw (d, d, 1, 1) -> yBlur min_pair horizontal -> yBlur min_rgba vertical -> (blurred mean, min, blurred mean of the larger layer, its min)
//   rim:    raw (0, fade, 0, 1) -> yBlur t2 (13 taps, sigma 1) -> the blurred rim map
struct ChainState {
  bool ready = false;
  int vs_quad = -1, fs_pair = -1, fs_rgba = -1, fs_t2 = -1;
  uint16_t prog_pair = 0, prog_rgba = 0, prog_t2 = 0, layout = 0;
  rdr::ImageId sh_mid = rdr::kNoImage, sh_out = rdr::kNoImage;
  uint32_t sh_w = 0, sh_h = 0;
  rdr::ImageId rim_out = rdr::kNoImage;
  uint32_t rim_w = 0, rim_h = 0;
  uint64_t shadow_made = 0, rim_made = 0;
  uint32_t shadow_passes = 0, rim_passes = 0;   // this frame
  const rdr::FrameBuilder* last_fb = nullptr;   // the graph the last chain was recorded into (one chain per graph)
  uint32_t last_draws = 0;
  const char* skip = "";                         // why a chain was not run this frame ("" = ok)
};

}  // namespace rdr_scene
