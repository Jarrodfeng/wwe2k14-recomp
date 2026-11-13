// The scene front-end (Session 101, lane 6; docs\renderer\SCENE_FRONTEND.md): takes the scene record's tables (scene_record.h, in memory)
// and produces frames for the renderer (src\renderer\renderer.h). Assets are registered once with our loaders' host formats
// (assets::Model: floats), materials are bound BY NAME to the hand-written effects (src\renderer\shaders\fx\), their named constants
// are filled by name (fx_tables.h). No game memory, no D3D stream, no binder of game draws.
//
// Includes the renderer and the asset model only (no viewer, no SDK).
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../assets/asset_model.h"
#include "../renderer/frame_builder.h"
#include "../renderer/renderer.h"
#include "scene_record.h"
#include "char_effects.h"  // S101 lane 13: the character path
#include "post_pass.h"     // S101 lane 34: the post pass

namespace rdr_scene {

// What the effect chooser may pick from, and what it picked.
struct MaterialWants {
  bool diffuse = false, lightmap = false, normal = false, specmap = false;
  bool spec = false;       // the material asks for a highlight
  bool alpha_test = false;
  bool skinned = false;    // S101 lane 15: a skinned prop chunk (the FX_SKINNED vertex variant)
};
struct FxChoice {
  int vs = -1, ps = -1;    // indices into rdr::fx::kVariants (-1 = none)
  std::string effect;      // the effect the pair belongs to ("yBG_DefaultLM")
  std::string why_not;     // set when vs < 0: the reason, for the coverage list
  bool Ok() const { return vs >= 0 && ps >= 0; }
};
// Picks the vertex + fragment variants of an effect for a material: the simplest pair whose interpolators match and whose textures and
// vertex inputs the front-end can supply. effect is the material class name as the file stores it ("yBG_Default", "yProp_BumpMap").
FxChoice ChooseEffect(const std::string& effect, const MaterialWants& wants);

// What one effect did in a scene: the coverage list (every mesh is either bound to our effect or drawn in the debug colour).
struct EffectCoverage {
  std::string effect;       // the material class name
  std::string bound_to;     // "yBG_DefaultLM: ...": one chosen pair (the first), empty when none
  uint32_t meshes_bound = 0, meshes_unbound = 0;
  std::string reason;       // why unbound
};

struct FrontEndOptions {
  bool textures = true;     // false: every material samples a flat grey (the selftest's planted fault)
  bool debug_unbound = true;  // draw a mesh with no effect in magenta (never silently)
};

// S101 lane 15 (entrances): the light sets of one model as the entrance rig rewrote them for this frame (the viewer's GpuModel::lights: the model's sets,
// the default rig, and for a belt / carried prop the entrant's set appended LAST by the viewer) and which set each mesh uses (GpuMesh::light: the viewer re-points
// a belt's normal-mapped meshes at the entrant's set, the rest at set 0). Replaces the sets and the per-mesh choice the model was added with (a proposed
// scene-record field: SCENE_RECORD "v2 candidates").
struct ModelLights {
  uint32_t asset = 0;
  std::vector<assets::LightSet> sets;
  std::vector<int16_t> mesh_set;  // per source mesh (Mesh::src order): its index into `sets`; empty = the file's choice (the set the mesh was added with)
};

// A frame request: what to draw, from where.
struct SceneFrame {
  Camera camera;                         // view = world -> clip when proj is the identity (the viewer gives its combined matrix; see the page)
  std::vector<Object> objects;
  std::vector<ParamValue> params;        // Object::params_first / count index this (names are strings: resolved by the caller into ParamNames)
  std::vector<std::string> param_names;  // parallel to params
  // S101 lane 13 (wrestlers): the skin palettes (Object::bones_first / bones_count index this; one matrix per skeleton node, viewer space, row vectors,
  // the viewer's `Model::skin`) and the evaluated light sets of characters (Object::light_set indexes this; the in-memory form of kScLights).
  std::vector<Mat4> bones;
  std::vector<assets::LightSet> light_sets;
  // S101 lane 15 (entrances): this frame's light sets per model (the entrance rig) and the entrance rule that the game draws the ring / prop classes with
  // their light-map variant (the stage page carries the rig's pools).
  std::vector<ModelLights> model_lights;
  bool entrance = false;
  bool entrance_bg = false;  // the viewer's ent_lm_bg switch: the yBG_Default / yBG_UVScroll classes take the entrance LM variant too (off by default)
  bool viewer_space = true;  // S101 lane 13: the matrices are the viewer's (Y up, the loader's mirror); false = already the game's file space (a live record)
  double time_s = 0;
  uint32_t width = 0, height = 0;
  float clear[4] = {0.12f, 0.14f, 0.18f, 1.0f};
  // S101 lane 34: when set, the scene's colour + depth go through the post pass (post_pass.h) and the picture read back is its output (the display curve is
  // NOT applied: the caller does, as the game does at the present). The scene's size must equal post->width / height.
  const PostParams* post = nullptr;
};

class FrontEnd {
 public:
  explicit FrontEnd(rdr::Renderer& renderer, const FrontEndOptions& options = FrontEndOptions());
  ~FrontEnd();

  // Registers a model (an arena or wrestler) as an asset: uploads its textures and geometry once, remembers its materials by name.
  // Returns the asset index objects refer to. Warnings (a texture that could not be uploaded) are appended.
  int AddModel(const assets::Model& model, std::vector<std::string>& warnings);
  void ReleaseModel(int model);  // the GPU must be idle (the caller waits)
  // S101 lane 15: a live texture of a model (an RGBA8 texture the viewer rewrites: tron video frames, the stage light-map page) gets a new picture.
  bool UpdateTexture(int model, size_t texture, uint32_t w, uint32_t h, const uint8_t* rgba);
  uint64_t texture_updates() const { return texture_updates_; }

  // Draws the frame into an internal RGBA8 colour image (and depth) of frame.width x frame.height and reads it back as RGBA8.
  bool RenderToPixels(const SceneFrame& frame, std::vector<uint8_t>& rgba, std::string& error);

  // The coverage of everything drawn since the last Reset: per material class name.
  std::vector<EffectCoverage> Coverage() const;
  std::string CoverageText() const;
  void ResetCoverage();
  uint64_t validation_errors() const { return renderer_.validation_errors(); }

 private:
  struct ModelData;
  struct Bound;
  ModelData& Mod(int id);
  const Bound& BindMaterial(ModelData& m, int material, bool skinned, bool ent_lm = false);  // (S101 lane 13: skinned picks the vertex variant of a character)
  void EnsureTargets(uint32_t w, uint32_t h);
  rdr::ImageId MakeTexture(const assets::Texture& t, std::string& warning);
  uint16_t ProgramFor(int vs, int ps, bool alpha);
  void ReloadPrograms(const std::vector<int>& changed);  // S101 lane24: the live shader reload changed SPIR-V (shader_reload.h): forget every program and every material's binding
  uint16_t LayoutFor(int vs);

  rdr::Renderer& renderer_;
  FrontEndOptions options_;
  std::vector<ModelData*> models_;
  std::deque<std::string> names_;
  std::map<uint64_t, uint16_t> programs_;
  std::map<int, uint16_t> layouts_;
  std::unique_ptr<PostPass> post_pass_;                // S101 lane 34 (made on first use; destroyed with the front-end, before the renderer)
  rdr::ImageId post_out_ = rdr::kNoImage;              // S101 lane 34: the post pass's output at the scene's size
  bool depth_sampled_ = false, want_depth_sampled_ = false;  // S101 lane 34
  rdr::ImageId white_ = rdr::kNoImage, grey_ = rdr::kNoImage, color_ = rdr::kNoImage, depth_ = rdr::kNoImage;
  rdr::ImageId black_ = rdr::kNoImage, neutral_normal_ = rdr::kNoImage, black_cube_ = rdr::kNoImage;  // S101 lane 13: all-zero layers / rim map; a normal map with no bump and half gloss
  struct CubeEntry { rdr::ImageId image = rdr::kNoImage; float scale[4] = {1, 1, 1, 1}; };
  std::unordered_map<uint64_t, CubeEntry> cubes_;  // the ambient cube of a light set (char_effects.h BuildAmbientCube), by LightSetKey
  const CubeEntry& CubeFor(const assets::LightSet& ls);
  uint32_t target_w_ = 0, target_h_ = 0;
  // S101 lane 257: the stage's big screen (stage slot 800) shows the previous frame's scene colour in the match look (the viewer's rule, renderer.cpp "live screen"):
  // live_screen_ is that copy at the target's size, live_valid_ says a frame has been copied into it, in_seed_ marks the extra frame that seeds it (a still shows the screen's picture).
  rdr::ImageId live_screen_ = rdr::kNoImage;
  bool live_valid_ = false, in_seed_ = false;
  uint64_t texture_updates_ = 0;  // S101 lane 15
  std::map<std::string, EffectCoverage> coverage_;
  std::set<std::string> missing_fields_;
  rdr::FrameBuilder builder_;
  int debug_vs_ = -1, debug_ps_ = -1;
};

}  // namespace rdr_scene
