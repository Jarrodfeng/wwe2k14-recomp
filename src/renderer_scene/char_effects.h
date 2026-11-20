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
  // S103 YCB (ChooseCharEffectTable): what the table pick decided. shadow = a picked variant reads the shadow map (FX_SHADOW), wet = the picked pixel variant reads the liquid map (slot 14),
  // tech_picked = the pair is the game's own table row for (technique, pass), cube_analytic = the row's pixel variant reads the cube-specular slot 11 (the 360's bake) and the pair binds the variant with the same defines minus FX_CUBE_SPECULAR (CUB.md Route A: the term is evaluated per pixel; a LOOK computed, the bake dropped),
  // blocked_wrinkles = the row needs the wrinkle map and the WRK switch is off, so nothing was picked (the caller takes the old no-table pick, counted).
  bool shadow = false, wet = false, tech_picked = false, cube_analytic = false, blocked_wrinkles = false;
  bool logo_table = false;   // S105 A5c fix F1: the row was taken from the editor's table (edit.pac p7/0)
  bool no_table = false;   // S103 YCB (review F4): no effect table existed when the pick ran (the caller rebuilds the bind once one does); one snapshot, no second read
  bool Ok() const { return vs >= 0 && ps >= 0; }
};
// S103 YCB (docs\research\s103\YCH.md section 4; switch wwe2k14_rdr_char_table_pick): the vertex + fragment pair of the effect table's row for (technique, pass) of this wrestler material, the game's own pick
// (technique_pick.h CharPass makes the pass). A cube-specular row (pixel slot 11) binds its non-cube twin (same defines minus FX_CUBE_SPECULAR, the row's own vertex stage; counted: cube_analytic, a deliberate choice, not a fallback); every other
// reason leaves the result !Ok with why_not set, and the caller keeps the old min-define pick (ChooseCharEffect*), counted. The vertex stage accepts FX_ID_MAP / FX_WRINKLE_LEVEL (their constants stay the neutral zero:
// IdMapLvLst / WklLvLst = the wrestler's live levels (S103 WRK: CharCtx::idmap / wkl, null = 0 = no tint / no wrinkle depth)); a row with FX_WRINKLES binds slot 5 = the material's wrinkles_n (S103 WRK; blocked only with the switch off).
// S105 A5c fix F1: `logo` = the material carries a CAW logo (Material::logo7 / logo4 >= 0): the row comes from the editor's table (read_rule::LogoEffectTableNow, edit.pac p7/0), the game's own editor rows, which are all logo variants.
// No editor table or no such effect in it: the startup table's row (nothing is drawn for the logo, as before). CharChoice::logo_table says which table was used.
CharChoice ChooseCharEffectTable(const std::string& effect, bool skinned, bool alpha_test, uint32_t technique, int pass, bool logo = false);
// The simplest vertex + fragment pair of `effect` that reads only textures the front-end can supply (the ones below), has every fragment
// interpolator written by the vertex stage, and uses none of the options that need a runtime pass the files do not carry (shadow map,
// cube reflections, blood, sweat, debug). `skinned`: the FX_SKINNED vertex variant, else the rigid one.
// S103 SWF: `wet` = the liquid-map reader: only yCh_skin / yCh_skin_flex, only a fragment that has FX_BSS + FX_BLOOD_TINT + FX_SWEAT + FX_BLOOD (reads the wrestler's liquid map at slot 14, the sweat bump at slot 4;
// the constants c205.zw / c210 / c234 / c246 from the material), simplest such pair. With no map nothing about a draw changes: the caller asks for `wet` only when it has a map for the wrestler.
CharChoice ChooseCharEffect(const std::string& effect, bool skinned, bool alpha_test, bool wet = false, bool logo = false);   // S105 A5c: logo = the material carries a CAW logo: an FX_LOGO fragment is preferred
// S103 L6: the same with the shadow-map reader on (FX_SHADOW fragment + FX_SHADOW / FX_SPHERE_OCC vertex): only fragments that HAVE FX_SHADOW are considered. Used when a frame's
// maps hold a shadow map (frame.maps, MapRole::ShadowMap, not raw); the plain choice above stays the fallback.
CharChoice ChooseCharEffectShadow(const std::string& effect, bool skinned, bool alpha_test, bool wet = false, bool logo = false);

// What a fragment sampler slot of a character effect holds.
enum class CharSlot : uint8_t {
  Diffuse,      // 0: the material's colour texture
  Normal,       // 1: normal xy + gloss w (the material's normal / effect map; a neutral map when it has none)
  NoiseTile,    // 3: the gear's tiled detail (`noise2`); neutral grey when absent
  Mask,         // 6: the gear's detail mask (`Mask`, z); black when absent
  LightCube,    // 13: the ambient cube of the four lights (built by BuildAmbientCube)
  WrinkleNormal,  // 5: the face wrinkle normal map `wrinkles_n` (g_mNormalWklPrm, WRK) of a table-picked bind; the NEUTRAL NORMAL when the material has none (counted; wrk_join.h WrinkleSlotPick), the old flat grey with the switch off
  RimMap,       // 15: the screen-space rim map (a runtime pass; the live map clears to (1,0,1,1) off-body (S103 B1F); the black stand-in = no rim)
  FlatBlack,    // team-colour masks (2, 14): all zero = the option does nothing
  Logo7,        // S105 A5c, 7: the CAW logo picture (Material::logo7); black when the material has none
  Logo4,        // S105 A5c, 4: the CAW logo2 picture (Material::logo4); black when the material has none
  FlatGrey,     // wrinkle map (5), hair noise (8, 9): the neutral value
  FlatBlackCube,  // 10: the reflection cube of yCh_Reflect (the stage's environment: not fed; black = no reflection layer)
  ShadowMap,    // S103 L6, 12: the wrestlers' shadow map (lane E's MapRole::ShadowMap); a shadow-variant fragment only
  LiquidMap,    // S103 SWF, 14: the wrestler's liquid map (x blood, y sweat, z gloss; set per draw from MapRole::SweatMap); only a wet-variant fragment samples it
  SweatBump,    // S103 SWF, 4: the wet skin's bump map (the material's g_mSweatPrm); only a wet-variant fragment samples it
};
CharSlot RoleOfSlot(int slot);
// S103 SWF: the role of a slot of a `wet` fragment (slots 4 and 14 differ from RoleOfSlot's team-colour stand-ins).
CharSlot RoleOfWetSlot(int slot);

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
  std::vector<uint32_t> src;           // S103 W2: parallel to `vertices`: the mesh vertex index each came from (offline checks)
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
  // S103 L6: the shadow reader's inputs (null = the plain variant: the names stay neutral zero). matrix = MapImage::matrix (TexProjLight, VS c68-c71 in register layout), params = MapImage::params.
  const float* shadow_matrix = nullptr;
  const float* shadow_params = nullptr;
  const float* shadow_prm = nullptr;   // MapImage::prm: ShadowPrm rows 0-2 (12 floats)
  const float* wkl = nullptr;          // S103 WRK: the six live g_fWklLv0..5 (WklLvLst; null = the record carries none for this body: zeros, counted by the caller)
  const float* idmap = nullptr;        // S103 WRK: the eight live g_fIdMapLv0..7 (IdMapLvLst; null = zeros)
  bool cube_analytic = false;          // S103 YCB (review F3): the draw is a cube row bound as its non-cube twin: a lobe power of 0 is fed as 1, as the 360's bake did
  bool spec_sweat = false;            // S103 YCB: a table-picked draw (CUB.md Route A): Sweat (c210) comes from the material even when the draw is not the wet variant (the analytic slot-11 term / the gear sibling read it)
  bool wet = false;                    // S103 SWF: this draw is bound to the wet variant (a liquid map is bound at slot 14): Param0.zw, Sweat, Blood and BSSBldClr come from the material; else they stay the dry zeros
  const float* rim_params = nullptr;   // S103 L7: the frame's filtered rim map (MapImage::params: w, h, near, far; null = none bound: RimLightDepth stays (0, 1, 1, 0) and slot 15 is black)
};
// Fills `out` (vec4s * 4 floats, zeroed by the caller). False = a name this path does not know (reported by the caller).
// S103 L7: how many key-light resolves rotated a record key light (view space), and how many of those had |d.x| > 1e-3 (the right-axis sign is only testable with those)
void KeyLightCensus(uint64_t& resolves, uint64_t& with_x);
bool ResolveChar(const std::string& name, uint32_t vec4s, const CharCtx& c, float* out);

}  // namespace rdr_scene
