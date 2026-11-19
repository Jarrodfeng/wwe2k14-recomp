// The crowd's prop meshes (signs, foam hands and the like) from the arena pac, in the stream's own host vertex, for drawing them inside the game's frame (S101 lane 133 = N4;
// docs\renderer\CROWD_PROMOTION.md "N4", docs\systems\crowd.md "Sign / foam-hand draws"). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// Reads the player's own pac\bg\bgNN.pac at run time; nothing is kept on disk.
//
// Where they live [C: byte joins on run 7079 / Check I / Bryan, tools\crowd\crowd_props_join.py]: bgNN.pac, PACH 0, entry 20000 (the audience set) holds, besides the 20 bodies (20000-20019
// and the lower LODs), single-mesh models in the key range 40000-42999. The two the stream draws (T_Audience_LightMap at 0x82518D60: 34 vertices / 77 indices; T_Audience at 0x825192D0:
// 140 / 223) are keys 40000 / 40100 and 41016. Which key a given draw is, is NOT assumed: PropSet carries every single-mesh model of the range and the hook joins by counts + bytes.
//
// The game's vertex for these (stride 28, VK_FORMAT 106 position, 106 normal, 83 uv) from the file's mesh record [C: bit-equal on position and uv, normal within 1 ulp on a few
// vertices]: position = float32(file * 10) (the game's x10 for crowd models), normal = the file's, uv = the S2 stream's two floats, each truncated to half (prop_host_vertex.h).
// Indices: the file's big-endian u16 strip as plain u16.
// Threads: Load on a worker (it takes the loader's process-wide mutex and shares its one-slot pac cache); everything else is plain data.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace assets::crowd_prop_gs {

inline constexpr uint32_t kVertexBytes = 28;   // position f32 x3, normal f32 x3, uv f16 x2

struct PropMesh {
  uint32_t key = 0;                 // the entry id inside the audience set (40000, 41016, ...)
  uint32_t vertex_count = 0;
  std::vector<uint8_t> vertices;    // vertex_count * kVertexBytes
  std::vector<uint16_t> indices;    // a triangle strip, plain order
};

// S103 FHB: a texture entry of the same range (the foam hand's print is entry 42000 + h - 1, a file image): mip 0 of its first DDS, linear DXT blocks as the file stores them (little-endian, untiled).
struct PropPicture {
  uint32_t key = 0;                 // the entry id inside the audience set (42016, ...)
  uint32_t width = 0, height = 0;
  uint32_t format = 0;              // the game's format code: 18 = DXT1 (BC1), 19 = DXT3 (BC2), 20 = DXT5 (BC3)
  std::vector<uint8_t> bytes;       // ((width + 3) / 4) * ((height + 3) / 4) blocks
};

struct PropSet {
  int arena = -1;
  std::vector<PropMesh> meshes;     // in key order
  std::vector<PropPicture> pictures;   // S103 FHB: the texture entries of the range, in key order
  std::vector<PropPicture> sign_pictures;   // S103 CSF: the crowd signs' pictures from pac\audience.pac PACH 0 entry 0 (key = the entry id), the same for every arena; empty when the pac is missing (signs then keep the guest read)
  size_t Bytes() const {
    size_t n = 0;
    for (const PropMesh& m : meshes) n += m.vertices.size() + m.indices.size() * 2;
    return n;
  }
};

// Pure: one JBOY entry (the whole entry, 'JBOY' tag included) -> the host vertex / index of its single mesh. False + err for anything else (more than one mesh, a stride other than
// 28, a missing uv stream, an index past the vertices). Used by Load; its output is checked against an independent Python conversion (tools\crowd\crowd_props_join.py, byte joins on real draws), not by a C++ unit test.
bool ConvertJboy(const uint8_t* data, size_t size, PropMesh& out, std::string& err);
// S103 FHB, pure: one non-model entry of the range -> mip 0 of its first DDS (found in the first 4 KB: a bare DDS or a texture directory's first blob). False + err for anything else (no DDS, a format other than DXT1 / 3 / 5, a short mip).
bool ConvertDds(const uint8_t* data, size_t size, PropPicture& out, std::string& err);

bool Init(const std::string& game_path);
// Loads every single-mesh model of keys 40000-42999 of the audience set (entry 20000 of PACH 0) of pac\bg\bgNN.pac. Serialised with every other loader (asset_load.h's LoadMutex).
bool Load(int arena, PropSet& out, std::string& err);
// S103 CSF (H-CS1): every sign picture of `pac\audience.pac` PACH 0 (entry 0 is a nested PACH of 525 leaves, all 64x64 DXT1, scanned 2026-11-19), read ONCE per boot and kept (about 525 x 2 KB = 1.05 MB decoded); keys in order.
// Not through the loader's one-slot pac cache (it would evict the arena pac); the file is 1 MB. False + err when the pac is missing / unreadable (callers fall back to the guest read).
bool LoadSignPictures(std::vector<PropPicture>& out, std::string& err);

}  // namespace assets::crowd_prop_gs
