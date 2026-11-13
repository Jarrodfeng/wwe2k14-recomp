// The crowd's far cards, drawn by us (Session 101, lane CD = C1): the GPU half. One instanced draw per person type (at most 20 a frame, in the stream's type order) of the
// card quad, the three world rows of each card per instance, the type's picture bound as slot 0; the game's pixel stage, alpha-test twin, state, camera and light map.
// docs\renderer\CROWD_CARDS.md. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
#pragma once
#include <cstdint>
#include <string>

#include "../renderer/renderer.h"
#include "../renderer_adapter/crowd_hook.h"
#include "crowd_cards_core.h"

namespace rdr_scene {

class CardsDrawer {
 public:
  struct Options {
    // Planted faults of the drawer (the check's twins): the quad's uv halves swapped / the red-blue swap of the picture's slot undone / the pixel stage without its alpha-test twin.
    bool uv_swapped = false, copy_swap_lost = false, no_alpha_test = false;
  };
  struct Stats {
    uint64_t draws = 0;          // instanced draws recorded
    uint64_t alpha_twin = 0;     // ... whose pixel stage is the alpha-test twin AS BUILT (review F4)
    uint64_t alpha_wanted = 0;   // ... draws made while the dropped draw's state asked for the test
    uint64_t no_image = 0;       // frames refused because a picture had no renderer image
    uint64_t other_program = 0;  // lane 219: frames refused because the first card draw runs a vertex stage ours does not follow
  };
  CardsDrawer(rdr::Renderer& renderer, const Options& options);

  // Records the plan's runs into in.builder (the targets bound at this point). Returns the instances drawn; 0 = nothing was appended.
  uint32_t Draw(rdr_adapter::CrowdInject& in, const cards::Plan& plan);
  const std::string& refusal() const { return refusal_; }   // lane 219: why the last Draw drew nothing (empty after a draw)
  const Stats& stats() const { return stats_; }
  bool ready() const { return vs_ >= 0; }

 private:
  rdr::Renderer& renderer_;
  Options options_;
  int vs_ = -1;               // rdr::fx::kVariants index of yAudienceCard_cards_vertex
  int vs_lm_vertex_ = -1;      // lane 219: yAudienceCard_cards_lm_vertex_vertex
  std::string refusal_;
  uint16_t layout_ = 0xFFFF;  // quad (binding 0) + three world rows per instance (binding 1)
  Stats stats_;
};

// The vertex-program hashes of a variant (every game hash that maps to it): for the bake pass test of cards::Input.
const std::unordered_set<uint64_t>& HashesOfVariant(const char* variant_name);

}  // namespace rdr_scene
