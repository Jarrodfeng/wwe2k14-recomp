// The stage screens from the files, in the GAME's space (S101 lane 115 = SO; docs\renderer\UNCLAIMED_MODEL_PARTS.md section 3.3; as built: docs\renderer\SCREENS_OURS.md).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Reads the player's own arena pac (bgNN.pac) at run time; nothing is kept on disk.
//
// The stage screens are the arena's pac keys 800-809 (the stream draws 801-807 at pass site 0x82C0F4BC, sub_82C0CA08): the big wall behind the stage, the mini-tron, the ribbon band.
// The file names their material yBG_Default; the game draws them with yMovieDefault (the video planes are run-time pictures), so the file's effect is NOT a class check here: ScreensCore
// requires the stream's program to be a yMovieDefault one (a per-key exception, measured). Geometry only: no texture is read, every slot is the stream's.
// The meshes are the ring's mesh record (ring_game_space.h: `entry` = the pac key) and the vertex / index writer is the ring's (WriteVertices / BuildIndices).
// Threads: Load on a worker (it takes the loader's process-wide mutex); everything else is plain data.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ring_game_space.h"

namespace assets::screen_gs {

inline constexpr int kFirstKey = 800, kLastKey = 809;

struct ScreenSet {
  int arena = -1;
  std::vector<int> keys;   // the keys that exist in this arena's pac and have a mesh
  std::vector<ring_gs::RingMesh> meshes;
  size_t Bytes() const {
    size_t n = 0;
    for (const ring_gs::RingMesh& m : meshes) n += m.blob ? m.blob->size() : 0;   // (an entry's meshes share one blob: an upper bound)
    return n;
  }
  const ring_gs::RingMesh* Find(int key, int part) const {
    for (const ring_gs::RingMesh& m : meshes)
      if (m.entry == key && m.part == part) return &m;
    return nullptr;
  }
};

bool Init(const std::string& game_path);
// Loads arena `arena`'s keys 800-809. False + err when the pac cannot be read or holds none.
bool Load(int arena, ScreenSet& out, std::string& err);

}  // namespace assets::screen_gs
