// Session 98 (plan step 16, "click to inspect"): the data the Arena Viewer keeps about every uploaded mesh so a click can say what it
// hit, plus the part-info record the INSPECT panel and the `pick` / `inspect` commands show. Plain C++ (no D3D, no ImGui): the
// renderer fills PartMeta / PlacementMeta at upload time, viewer_main.cpp turns a PickHit into a PartInfo, ui_inspect.cpp draws it.
//
//   PickHit   = which mesh of which model (the renderer's indices) and which instance of it (crowd member, placed copy).
//   PartMeta  = per GpuMesh: where it came from (pac / slot or key / piece), node and palette names, textures, light set.
//   PartInfo  = everything the panel shows (friendly name from assets::FriendlyPartName, the literal asset name, source, ...).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace view {

struct PickHit {
  int model = -1;     // index into Renderer::Models()
  int mesh = -1;      // index into GpuModel::meshes
  int instance = 0;   // instance of a placed / instanced mesh (0 for a mesh drawn once)
  bool valid() const { return model >= 0 && mesh >= 0; }
  bool operator==(const PickHit& o) const { return model == o.model && mesh == o.mesh && instance == o.instance; }
  bool operator!=(const PickHit& o) const { return !(*this == o); }
};

// Per uploaded mesh (parallel to GpuModel::meshes).
struct PartMeta {
  int kind = 0;               // assets::Mesh::src_kind: 1 arena, 2 wrestler piece, 3 crowd body, 4 prop
  std::string pac;            // "bg05.pac"
  int key = 0;                // slot / key / entry
  std::string part;           // "slot 392", "piece 10000", "crowd 3", "ring steps 32"
  std::string node;           // first node name of the piece
  std::string palette;        // skin-palette bone names, ';' separated
  std::string shader;         // material class ("yCh_skin", "yBG_Default")
  std::string diffuse_name;   // the diffuse sampler's name as stored
  int light_id = -1;          // the arena light set number (-1 = the viewer's default rig)
  bool additive = false, skinned = false, blended = false;
  int placement = -1;         // index into GpuModel::pmeta; -1 = drawn once
  std::vector<std::pair<std::string, int>> tex;  // sampler role ("Diffuse", "Normal", ...) and the slot in GpuModel::textures (-1 = no such texture)
};

// Per placement list: the row-vector 4x4 of every instance (16 floats each, model space), as uploaded.
struct PlacementMeta {
  std::string label;
  std::vector<float> mats;
  size_t count() const { return mats.size() / 16; }
};

struct PartTexRef {
  std::string role, name;
  int model = -1, slot = -1;  // the ASSETS tab entry (-1 = not an uploaded texture)
  uint32_t w = 0, h = 0;
};

struct PartInfo {
  bool valid = false;
  PickHit hit;
  std::string friendly, literal, category, source, kind, shader, node, palette, light, motion, model_label;
  std::string mesh_note;                // "mesh 2 of 4 in this piece"
  std::string placement;                // "ring post, copy 2 of 4" / "crowd aud_03a, member 118 of 1500" / "drawn once"
  float pos[3] = {0, 0, 0};             // world position of the instance (the mesh's centre for a mesh drawn once)
  float turn_deg = 0;                   // the instance's / the model's turn about Y
  int instances = 1;
  std::vector<PartTexRef> textures;
  std::vector<std::string> movies;
};

std::string PartInfoJson(const PartInfo& p);

}  // namespace view
