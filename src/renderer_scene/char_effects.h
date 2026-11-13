// Session 101, lane 13: the scene front-end's character path (docs\renderer\SCENE_FRONTEND.md "Wrestlers"). New file; front_end.cpp calls it.
//   - which yCh_* effect pair a wrestler material takes (ChooseCharEffect),
//   - the skin: a mesh is cut into chunks of at most 21 bones (the effects hold 63 vec4 = 21 bone matrices), each with its own bone stream,
//   - the lighting data the effects read from textures (the ambient cube of the four lights, slot 13) and from named constants (ResolveChar).
// Space: the character effects are written for the game's FILE space (-Y up). The viewer's loader mirrors Y once; the backend of this path
// mirrors it back at the three places a matrix enters (the bone rows, the view-projection, the eye), so every constant the effects read is
// the game's own (light directions as stored, the sky axis (0, 1, 0), the cube indexed by the file-space normal). No game memory, no D3D stream.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../assets/asset_model.h"

namespace rdr_scene {

// A material class the file names "yCh_*" (the wrestlers' effect family).
bool IsCharEffect(const std::string& effect);
// The cloth classes (yCh_cloth, _flex, _trans): their vertex stage does not skin the position. A skinned cloth piece arrives in WORLD space (the game's
// cloth simulation writes it), a rigid one adds its bone matrix to the unmoved position as an OFFSET. The front-end poses the vertices on the CPU for the
// first and hands the second its world matrix minus the identity.
bool IsClothEffect(const std::string& effect);
// Hair / translucent classes (the viewer draws them twice, front faces then back faces, blended).
bool IsHairEffect(const std::string& effect);

struct CharChoice {
  int vs = -1, ps = -1;      // indices into rdr::fx::kVariants
  std::string effect;        // "yCh_skin_flex_muscle_fragment + yCh_skin_flex_skinned_vertex" is built from the registry by the caller
  std::string why_not;
  bool Ok() const { return vs >= 0 && ps >= 0; }
};
// The simplest vertex + fragment pair of `effect` that reads only textures the front-end can supply (the ones below), has every fragment
// interpolator written by the vertex stage, and uses none of the options that need a runtime pass the files do not carry (shadow map,
// cube reflections, blood, sweat, debug). `skinned`: the FX_SKINNED vertex variant, else the rigid one.
CharChoice ChooseCharEffect(const std::string& effect, bool skinned, bool alpha_test);

// What a fragment sampler slot of a character effect holds.
enum class CharSlot : uint8_t {
  Diffuse,      // 0: the material's colour texture
  Normal,       // 1: normal xy + gloss w (the material's normal / effect map; a neutral map when it has none)
  NoiseTile,    // 3: the gear's tiled detail (`noise2`); neutral grey when absent
  Mask,         // 6: the gear's detail mask (`Mask`, z); black when absent
  LightCube,    // 13: the ambient cube of the four lights (built by BuildAmbientCube)
  RimMap,       // 15: the screen-space rim map (a runtime pass: black = no rim, like the viewer's default)
  FlatBlack,    // team-colour masks (2, 4, 7, 14), logo layers: all zero = the option does nothing
  FlatGrey,     // wrinkle map (5), hair noise (8, 9): the neutral value
  FlatBlackCube,  // 10: the reflection cube of yCh_Reflect (the stage's environment: not fed; black = no reflection layer)
};
CharSlot RoleOfSlot(int slot);

// ---- the ambient cube ----------------------------------------------------------------------------------------------------------
// The wrestlers' diffuse light, as the game's yPreLight pass fills it and the skin / gear shaders read it back (viewer_hlsl.h PreLight, port):
// for a file-space direction d, weights from the four lights' two wrap-around lobes. rgb = sum of weight x colour, a = the glow luminance.
constexpr int kCubeSize = 16;
// Writes 6 faces x size x size RGBA16F texels (Vulkan face order +X -X +Y -Y +Z -Z) into `half` (uint16 halves). Also returns the constants
// the effects pair with it: scale[0..2] = DifLghMax.xyz (this frame's light sum over the cube's), scale[3] = DifLghMaxA.x.
void BuildAmbientCube(const assets::LightSet& ls, int size, std::vector<uint16_t>& half, float scale[4]);
uint64_t LightSetKey(const assets::LightSet& ls);

// ---- skin chunks ---------------------------------------------------------------------------------------------------------------------
constexpr int kMaxPaletteBones = 21;  // WorldMatrixArray is vec4[63]: three rows per bone
struct SkinVertex { uint32_t ids[4]; float w[4]; };  // the second vertex stream: palette slots as integers (the effects declare bone ids integer and cast), weights
struct SkinChunk {
  std::vector<assets::Vertex> vertices;
  std::vector<uint32_t> indices;       // local to `vertices`
  std::vector<SkinVertex> skin;        // parallel to `vertices`
  std::vector<uint16_t> nodes;         // palette slot -> skeleton node (the index into the model's skin palette)
};
// Cuts a skinned mesh into chunks of at most kMaxPaletteBones distinct bones. Returns false when a single triangle needs more (not seen).
bool BuildSkinChunks(const assets::Mesh& mesh, std::vector<SkinChunk>& out);

// ---- constants by name ---------------------------------------------------------------------------------------------------------------
struct CharCtx {
  const assets::Material* mat = nullptr;
  const assets::LightSet* lights = nullptr;
  const float* view_proj = nullptr;   // the frame's view-projection, viewer space (16 floats, row vectors)
  const float* eye = nullptr;         // the camera, viewer space
  const assets::Matrix* bones = nullptr;  // final matrices of palette slots 0..bone_count-1 (viewer space, row vectors, world included)
  uint32_t bone_count = 0;
  float alpha = 1.f;                  // object alpha
  const float* cube_scale = nullptr;  // BuildAmbientCube's scale[4]
  bool cloth_offset = false;          // a rigid cloth piece: the bone rows are (world - identity)
  float mirror = -1.f;                // -1: the matrices are in the viewer's Y-up space (mirrored back here); +1: already the game's space (a live record)
};
// Fills `out` (vec4s * 4 floats, zeroed by the caller). False = a name this path does not know (reported by the caller).
bool ResolveChar(const std::string& name, uint32_t vec4s, const CharCtx& c, float* out);

}  // namespace rdr_scene
