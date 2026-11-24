// S106 G4-1A: moved verbatim from scene_record.h (and record_io.h for the 2D row checks); see docs\research\s106\G4.md
#pragma once
#include <cstdint>

namespace rdr_scene {


// lane 136 (SH3 slice 1, docs/renderer/SHADOW_CIRCLES.md): the round blob shadows under the characters (T_CircleShadow, sub_82812E90) as the game uploads them. Two optional sections with
// ids in the enum (16 and 17; 15 is lane 130's): 16 = one CircleShadow per character per call, 17 = the one shared disc mesh.
// Entries are stamped with the hook's guest frame and joined to the stream's frame by that stamp, never by the record's index (the +1 pairing is the crowd's, not ours).
inline constexpr uint32_t kCirclePoints = 21;      // uploaded WorldPosArray rows (c8..c28)
inline constexpr uint32_t kCircleMeshVertices = 128, kCircleMeshIndices = 192, kCircleVertexStride = 32;
struct CircleShadow {
  uint32_t frame = 0;          // the hook's guest frame (low 32 bits) when the game uploaded the constants
  uint32_t character = 0;      // the character's engine address (the word in the slot table), 0 = unknown
  uint32_t slot = 0;           // 0..8: the slot in the game's table of 9 characters
  uint32_t flags = 0;          // CircleFlags
  float shadow_param[4] = {};  // c0: x strength, y ground height, z fade height, w fade power
  float view_proj[16] = {};    // c4-c7 (the four uploaded rows)
  float points[kCirclePoints][4] = {};   // c8-c28: x, y, z, 1 of each tracked body point (world space, the game's file space)
};
enum CircleFlags : uint32_t { kCircleHaveViewProj = 1, kCircleHaveParam = 2, kCircleFromLatch = 0x100 };   // 0x100 (S103 A3): the row was read at the latch, not recorded by the walk; circle_contrib.h replaces it when the walk recorded the same (frame, slot)
static_assert(sizeof(CircleShadow) == 16 + 16 + 64 + 16 * kCirclePoints, "CircleShadow is a flat POD table");
struct CircleMesh {
  uint32_t vertex_count = 0, index_count = 0, stride = kCircleVertexStride, flags = 0;
  uint8_t vertices[kCircleMeshVertices * kCircleVertexStride] = {};   // the stream's host layout: float3 position, float blend, u8x4 point ids, float3 disc uv + strength
  uint16_t indices[kCircleMeshIndices] = {};                          // triangle list, host byte order
};
static_assert(sizeof(CircleMesh) == 16 + kCircleMeshVertices * kCircleVertexStride + 2 * kCircleMeshIndices, "CircleMesh is a flat POD table");

}  // namespace rdr_scene
