// The crowd camera flash's model from the arena pac, in the GAME's space, for drawing it inside the game's own frame (Session 101, lane 68; the design:
// docs\renderer\PARTICLES_PROMOTION_MAP.md sections 2-3 lane F, docs\formats\crowd_flash.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Reads the player's own arena pac at run time; nothing is kept on disk.
//
// Like assets::arena_gs (lane 40): the viewer's loader (jboy.h) reads pac entry 31000 and the viewer's Y mirror is taken back off, so the vertices are the stream's own
// floats. Unlike LoadArena, only the one entry (and the texture directories 400-402) is read, not the whole arena.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "arena_game_space.h"   // arena_gs::Vertex (36 bytes: pos, normal, colour, uv)
#include "asset_model.h"

namespace assets::flash_gs {

struct FlashMesh {
  int arena = -1;
  std::vector<arena_gs::Vertex> vertices;
  std::vector<uint32_t> indices;     // a triangle list in the stream's winding
  uint32_t strip_indices = 0, index_groups = 0;
  std::string effect;
  bool has_diffuse = false;
  Texture diffuse;                   // the model's own texture (flash01)
  // The index count the game's draw of this mesh has: the strip indices, one more per extra group (the restart between groups; ARENA map section 2).
  uint32_t StreamIndexCount() const { return strip_indices + (index_groups > 1 ? index_groups - 1 : 0); }
  size_t Bytes() const { return vertices.size() * sizeof(arena_gs::Vertex) + indices.size() * 4 + diffuse.data.size(); }
};

bool Init(const std::string& game_path);
// Loads entry 31000 of pac\bg\bgNN.pac (NN = arena). Serialised with every other loader (asset_load.h's LoadMutex).
bool Load(int arena, FlashMesh& out, std::string& err);

}  // namespace assets::flash_gs
