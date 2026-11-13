// The crowd's people from the files, for the scene record's crowd class (Session 101, lane 27; docs\systems\crowd.md). Copyright (c) the
// wwe2k14-recomp authors. BSD-3-Clause. Reads the player's own arena pac at run time; nothing is kept on disk.
//  - Init(game): the game folder (a second GameFiles of its own; the viewer's loader seam keeps its own).
//  - LoadBody(arena, type): the person body of `type` in `arena` as a Model, at its STORED pose (no m2.pac pose), with the arena's own crowd
//    materials, light set and stage lightmap, un-placed (the record's Object::world places it), textures pruned to what it uses. Vertices are
//    in the viewer's space (FinishModel's Y mirror, like every arena mesh): palettes come in viewer space (the reader converts a Live record once).
//  - Palettes(arena, act, frame): the game's pose of the 20 types as skin matrices (file space, row vectors, one per skeleton node), from the
//    m2.pac motions the same way the viewer's crowd poses its bodies (asset_load.h AddCrowd / PoseCrowdBody).
//  - Members(arena): the viewer's crowd members (type, file-space matrix, stands flag), the stand-in the test builds a snapshot from.
#pragma once
#include <string>
#include <vector>

#include "asset_model.h"

namespace assets::crowd_body {

struct Member {
  int type = 0;
  Matrix file_world;     // file space (-Y up), row vector, with the person's scale
  bool stands = false;   // a stands member (the game's card rule applies); false = a seated floor person
};

bool Init(const std::string& game_path);
bool LoadBody(int arena, int type, Model& out, std::string& err);
// S101 lane 35 (the crowd's first promotion, game space end to end): LoadBody with the viewer's Y mirror taken back off the vertices (FinishModel negated
// pos / normal / tangent y): the body is in the game's file space (-Y up) like the scene record's member matrices and palettes, and drawable by the
// game's own yAudience effect. Triangle order is unchanged. Thread-safe against other calls only if the caller serialises (one worker).
bool LoadBodyGameSpace(int arena, int type, Model& out, std::string& err);
// S101 lane 35 (LOD bodies): the person body of `type` at LOD group `lod` (0 = LoadBodyGameSpace; 1 / 2 = pac entry 20000 + 40 * lod + type of the audience set, the decimated bodies the game draws for
// the far stands), game space (no Y mirror), vertices at the loader's scale (the caller multiplies positions by 10 for the game's skinned blob), the diffuse `aud_NNa`, nodes in the body's own order.
bool LoadBodyLodGameSpace(int arena, int lod, int type, Model& out, std::string& err);
// Forgets an arena's loaded model (the load screen): its memory goes (S101 lane 35).
void Release(int arena);
bool Palettes(int arena, int act, int frame, std::vector<std::vector<Matrix>>& out, std::string& err);
bool Members(int arena, std::vector<Member>& out, std::string& err);

// CPU-skins a stored body with a palette (`count` VIEWER-space skin matrices (a Live record is converted once at load: S X S), one per skeleton node, 16 floats each, the Mat4 layout): out = a
// copy of `stored` with every vertex = sum(w * (v * palette[bone])) / sum(w), the normals likewise (the stored vertices and the palette are in the
// viewer's space). Same weights rule as the viewer's PoseCrowdBody; a bone index past `count` adds nothing.
void PoseBody(const Model& stored, const float* palette, size_t count, Model& out);

}  // namespace assets::crowd_body
