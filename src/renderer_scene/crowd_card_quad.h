// The far crowd card's quad (Session 111, lane CARDCULL; docs\research\s111\CARDCULL.md). Header-only: LiveCrowd builds the card model from it
// and the unit tools\scene\cardcull_unit.cpp puts the same four vertices through the game's chain and ours.
#pragma once
#include <cstdint>

namespace rdr_scene::live::far_card {

// The game's quad (sub_82507890, built once): x = 164 x 0.5 / 164 x -0.5, y = 0 / -250, z = 0, in this vertex order. Its index buffer 0, 1, 2, 3 is drawn by
// sub_82512F70 audience_2d_block_draw as a 2-triangle strip (primitive 6), i.e. the triangles (0, 1, 2) and (2, 1, 3) in D3D's order: kIndices.
inline constexpr float kX[4] = {+82.f, -82.f, +82.f, -82.f};
inline constexpr float kY[4] = {0.f, 0.f, -250.f, -250.f};
inline constexpr float kU[4] = {1.f, 0.f, 1.f, 0.f};   // ours: the baked picture's coordinates
inline constexpr float kV[4] = {1.f, 1.f, 0.f, 0.f};
inline constexpr uint32_t kIndexCount = 6;
inline constexpr uint32_t kIndices[kIndexCount] = {0, 1, 2, 2, 1, 3};

// The card's face cull is FrontEnd::DrawInstanced's own (cull back, front clockwise: the crowd node turns the cull on for every crowd draw and the card pass leaves it,
// so the game drops a card seen from behind); the planted twin is InstancedDraw::plant_two_sided (rig fault cards_two_sided). S111 CROWDCULLSRC.

}  // namespace rdr_scene::live::far_card
