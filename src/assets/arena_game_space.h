// The arena's stage slots from the files, in the GAME's space, for drawing them inside the game's own frame (Session 101, lane 40; the design:
// docs\renderer\ARENA_PROMOTION_MAP.md sections 2-4). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Reads the player's own arena pac at run
// time; nothing is kept on disk.
//
// Like assets::crowd_body (lane 35): the arena is loaded with the viewer's loader (assets::LoadArena) and then the viewer's Y mirror (FinishModel) is taken
// back off the vertices, so the meshes are in the space the game's draws are in (file space, -Y up, the stream's own vertex floats). The triangle order is the
// file's own strip order (the loader's strip-to-list), so the winding is the stream's. Only the stage slots (pac keys 0-399) are returned: the ring (951+),
// the screens (800-809) and the props stay the stream's.
//
// What each mesh carries for the join with a stream draw (ARENA map section 2): the game's draw "part" (src_record, the mesh record's index in its slot),
// the vertex count, the INDEX COUNT the stream draw has (the file's strip indices; with the index-group rule: a mesh stored as 2 groups is drawn with one
// more index, the restart between them), the material class (the effect name) and the file textures by role.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "asset_model.h"

namespace assets::arena_gs {

struct Vertex {                 // 36 bytes; what our vertex buffer holds
  float pos[3];                 // game space (file space)
  float normal[3];
  uint8_t color[4];             // as the loader stores the file's colour word: R, G, B, A (arenas: file bytes A R G B -> R G B A)
  float uv[2];                  // the first set (the diffuse's)
};
static_assert(sizeof(Vertex) == 36, "arena vertex layout");

// S101 lane 49 (the placed props, slots 390-397: `yProp_DefaultLM_skinned_*_vertex` read a weight word and a bone-id word): the vertex of a mesh stored with skin streams, 44 bytes.
// [C: the stream's converted vertex on Check B 7051: element 1 = R8G8B8A8_UNORM weights, element 2 = R8G8B8A8_UINT bone ids, both after the position; the file's S1 pair (LE u32 palette
// slot, BE f32 weight 1.0) becomes ids (slot, 255, 255, 255) and weights (0, 0, 0, 255), byte for byte]. Only one-influence-per-vertex meshes are built (StageMesh::skinned).
struct SkinVertex {
  Vertex v;
  uint8_t weights[4];
  uint8_t ids[4];
};
static_assert(sizeof(SkinVertex) == 44, "arena skin vertex layout");

struct StageMesh {
  int slot = -1;                // the pac key (0-399)
  int part = -1;                // the mesh record's index within the slot's JBOY (= the game's DrawOrigin::part, measured by tools\arena\arena_ours_test)
  std::string effect;           // the material class as stored ("yBG_Default", "yBG_BumpMapLM", "yProp_DefaultLM", ...)
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;  // a triangle list in the stream's winding
  std::vector<float> uv1;       // the second (light-map) set, 2 floats a vertex: not drawn, kept for the test's diagnosis
  uint32_t strip_indices = 0;   // the file's strip indices (sum over the groups)
  uint32_t index_groups = 0;
  int diffuse = -1, normal_map = -1, specular = -1;  // indices into StageArena::textures; -1 = none
  uint64_t material_hash = 0;   // FNV over the material parameters the draw reads (colours, light limit, specular, flags): "interchangeable" needs it equal
  bool lm = false;              // the mesh carries a light-map slot (the "LM" family)
  // S101 lane 49: true when the file stores one skin pair per vertex (slot < 255, weight exactly 1.0) for EVERY vertex: then skin_ids / skin_weights hold the stream's two words (4 bytes a vertex).
  bool skinned = false;
  std::vector<uint8_t> skin_ids, skin_weights;
  bool alpha_test = false, alpha_blend = false, additive = false;
  // The index count the game's draw of this mesh has: the strip indices, one more per extra group (the restart between groups). false ignores the group rule.
  uint32_t StreamIndexCount(bool group_rule = true) const { return strip_indices + (group_rule && index_groups > 1 ? index_groups - 1 : 0); }
};

struct StageArena {
  int arena = -1;
  std::vector<StageMesh> meshes;
  std::vector<Texture> textures;   // only what the stage meshes use
  int stage_lightmap = -1;         // the file's own stage light map (fade_lightmap00), an index into textures: only for the file_lightmap fault
  size_t Bytes() const;
};

bool Init(const std::string& game_path);
// Loads arena `arena`'s stage slots. Thread-safe against other calls only if the caller serialises (one worker).
// viewer_space: keep the viewer's Y mirror (the planted fault "viewer_space").
bool Load(int arena, StageArena& out, std::string& err, bool viewer_space = false);

}  // namespace assets::arena_gs
