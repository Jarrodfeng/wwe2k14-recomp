#pragma once
// Session 80 (SCENE_SHADERS_PLAN.md; Jarrod: "Our own scene shaders", after "I'm not a fan of the bandaid fixes"): our
// own shaders for the scene's main materials in place of the game's translated ones, on NATIVE's draws - the same
// mechanism as the post effects (src\post_swap.h: the SDK swaps a guest shader's code for ours and keeps its pass,
// textures and constants; it needs gpu_native_shader_swap, which NATIVE's list turns on). wwe2k14_scene_shaders (read
// at start) is off by itself and on under the arc switch (src\native_frame.h, on by default since v0.2.15). The shaders:
// src\scene\*.frag, compiled by tools\post\build_shaders.ps1. Since 2026-11-02 every pixel shader of the match and the
// entrances (the crowd, the wrestlers, the arena, the passes around the scene, the post chain, the sprites).

#include <cstdint>
#include <iterator>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "half2_hooks.h"  // wwe2k14::half2::Cp()
#include "scene/spirv/crowd_entrance.h"
#include "scene/spirv/crowd_match.h"
#include "scene/spirv/own_crowd_card_mv_vert.h"  // Session 95 (F5): the view-1-camera variants
#include "scene/spirv/own_crowd_card_mv_vi_vert.h"  // Session 95 (P7): the attribute forms (gpu_native_crowd_input)
#include "scene/spirv/own_crowd_card_vert.h"
#include "scene/spirv/own_crowd_card_vi_vert.h"
#include "scene/spirv/own_crowd_entrance.h"
#include "scene/spirv/own_crowd_entrance_mv_vert.h"
#include "scene/spirv/own_crowd_entrance_mv_vi_vert.h"
#include "scene/spirv/own_crowd_entrance_vert.h"
#include "scene/spirv/own_crowd_entrance_vi_vert.h"
#include "scene/spirv/own_crowd_match.h"
#include "scene/spirv/own_crowd_match_mv_vert.h"
#include "scene/spirv/own_crowd_match_mv_vi_vert.h"
#include "scene/spirv/own_crowd_match_vert.h"
#include "scene/spirv/own_crowd_match_vi_vert.h"
#include "scene/spirv/probe_flat.h"
#include "scene/spirv/wrestler_body.h"
#include "scene/spirv/wrestler_body_entrance.h"
#include "scene/spirv/wrestler_skin.h"
#include "scene/spirv/wrestler_skin_entrance.h"
#include "scene/spirv/wrestler_head.h"
#include "scene/spirv/wrestler_body_lean.h"
#include "scene/spirv/wrestler_hair_lean.h"
#include "scene/spirv/wrestler_head_lean.h"
#include "scene/spirv/wrestler_skin_decal_lean.h"
#include "scene/spirv/wrestler_skin_lean.h"
#include "scene/spirv/wrestler_skin_plain_lean.h"
#include "scene/spirv/wrestler_head_entrance.h"
#include "scene/spirv/wrestler_hair.h"
#include "scene/spirv/wrestler_skin_decal.h"
#include "scene/spirv/wrestler_skin_decal_entrance.h"
#include "scene/spirv/wrestler_body_entrance_pow.h"
#include "scene/spirv/wrestler_skin_entrance_pow.h"
#include "scene/spirv/wrestler_head_entrance_pow.h"
#include "scene/spirv/wrestler_skin_decal_entrance_pow.h"
#include "scene/spirv/wrestler_skin_plain_entrance_pow.h"
#include "scene/spirv/wrestler_skin_plain.h"
#include "scene/spirv/wrestler_skin_plain_entrance.h"
#include "scene/spirv/arena_bg.h"
#include "scene/spirv/arena_bg_bump_lm_mp.h"
#include "scene/spirv/arena_bg_shadow_lm_mp.h"
#include "scene/spirv/arena_bg_shadow_spec_mask_lm_mp.h"
#include "scene/spirv/arena_bg_spec_mask_lm_mp.h"
#include "scene/spirv/arena_prop_bump_mp.h"
#include "scene/spirv/arena_prop_bump_reflect_mp.h"
#include "scene/spirv/arena_prop_bump_rope2_mp.h"
#include "scene/spirv/arena_prop_bump_rope_mp.h"
#include "scene/spirv/arena_prop_spec_mask_lm_mp.h"
#include "scene/spirv/arena_bg_lm.h"
#include "scene/spirv/arena_bg_shadow.h"
#include "scene/spirv/arena_bg_shadow_plain.h"
#include "scene/spirv/arena_prop_psm.h"
#include "scene/spirv/arena_prop_bump.h"
#include "scene/spirv/arena_prop_bump_reflect.h"
#include "scene/spirv/arena_prop_bump_rope.h"
#include "scene/spirv/arena_prop_bump_rope2.h"
#include "scene/spirv/arena_bg_bump_lm.h"
#include "scene/spirv/arena_bg_scroll_blend_lm.h"
#include "scene/spirv/arena_bg_shadow_lm.h"
#include "scene/spirv/arena_bg_spec_mask_lm.h"
#include "scene/spirv/arena_prop_psm_lm.h"
#include "scene/spirv/arena_prop_spec_mask_lm.h"
#include "pixel_multiview.h"  // Session 95
#include "scene/spirv/audience_copy.h"
#include "scene/spirv/circle_shadow.h"
#include "scene/spirv/liquid_map_copy.h"
#include "scene/spirv/rim_light_map.h"
#include "scene/spirv/shadow_map.h"
#include "scene/spirv/side_blur10.h"
#include "scene/spirv/side_blur13.h"
#include "scene/spirv/side_blur21.h"
#include "scene/spirv/side_blur4_mask.h"
#include "scene/spirv/side_blur4_mask_inv.h"
#include "scene/spirv/side_blur5.h"
#include "scene/spirv/side_downscale4.h"
#include "scene/spirv/side_liquid_layer.h"
#include "scene/spirv/side_prelight.h"
#include "scene/spirv/crowd_2d.h"
#include "scene/spirv/debug_primitive.h"
#include "scene/spirv/default_textured.h"
#include "scene/spirv/movie_yuv.h"
#include "scene/spirv/arena_bg_spec_plain.h"
#include "scene/spirv/side_blur15.h"
#include "scene/spirv/side_bloom_bright4.h"
#include "scene/spirv/side_dof_blur.h"
#include "scene/spirv/side_dof_downsample.h"
#include "scene/spirv/wrestler_reflect.h"
#include "scene/spirv/wrestler_reflect_entrance.h"
#include "scene/spirv/wrestler_skin2.h"
#include "scene/spirv/wrestler_skin2_entrance.h"
#include "scene/spirv/wrestler_trans.h"
#include "scene/spirv/wrestler_trans_lobes.h"
#include "scene/spirv/arena_bg_shadow_spec_mask_lm.h"
#include "scene/spirv/side_glare_blur8.h"
#include "scene/spirv/side_glare_combine.h"
#include "scene/spirv/sprite_modes.h"
#include "scene/spirv/sprite_movie_yuv_alpha.h"
#include "scene/spirv/arena_bg_scroll.h"
#include "scene/spirv/arena_bg_scroll_lm.h"
#include "scene/spirv/arena_bg_spec.h"
#include "scene/spirv/arena_particle.h"
#include "scene/spirv/arena_prop.h"
#include "scene/spirv/arena_prop_lm.h"
#include "scene/spirv/arena_prop_spec.h"
#include "scene/spirv/arena_prop_spec_mask.h"

REXCVAR_DECLARE(bool, wwe2k14_shader_mediump);  // (src\post_swap.h)
REXCVAR_DEFINE_BOOL(wwe2k14_scene_shaders, false, "wwe2k14",
                    "Session 80 (with gpu_native_shader_swap, on in NATIVE's list; read at start): our own shaders for "
                    "every pixel shader of the match and the entrances (OWN_RENDERER_PLAN.md) in place of the game's "
                    "translated ones; false = the game's own. Off by itself; on under wwe2k14_native_frame");
REXCVAR_DEFINE_BOOL(gpu_native_wrestler_lean, false, "GPU/Native",
                    "Session 95 (read at start; needs wwe2k14_scene_shaders): the materials of the wrestlers (gear, body skin, skin plain / decal, "
                    "head, hair; the hair also in the entrances) from their LEAN compiles (src/scene/wrestler_lean.glsl): exact early-outs - no shadow fetch at strength 0, no shadow pows "
                    "where the first filter pair is lit, no highlight / lighting-map fetch at rim strength 0, no reflection cube fetch at zero weights, "
                    "no area / detail / second-normal fetch where its mask or weight is 0. Same picture (exact in the offline proof except where the plain code makes a NaN); off = the plain ones");
REXCVAR_DEFINE_BOOL(gpu_native_wrestler_pow, false, "GPU/Native",
                    "Session 95 (r3gpu, read at start; needs wwe2k14_scene_shaders): the entrance materials of the wrestlers (gear, skin, skin plain / decal, head) from their POWLEAN compiles "
                    "(src/scene/wrestler_pow.glsl): the powers whose exponent is 1, 2, 4 or 20 (uniform branch) as multiplies in place of log2 + exp2. Same picture to float rounding; off = the plain ones");
REXCVAR_DEFINE_INT32(wwe2k14_scene_shaders_probe, 0, "wwe2k14",
                     "Session 80 (diagnostic, pictures wrong; needs gpu_native_shader_swap; read at start): 1 = a flat "
                     "grey in place of the wrestlers' body material; 2 = in place of every wrestler pixel shader (src\\scene\\probe_flat.frag): the GPU time it saves is "
                     "the most our own shader for it could save on its pixel side; 3 = in place of the side passes' "
                     "shaders (blurs, lighting cubes, sweat / rim light / shadow maps, the crowd atlas copy)");

namespace wwe2k14::scene_swap {

// Session 95 (gpu_native_wrestler_lean): the plain module or its LEAN compile.
struct SpvPick {
  const uint32_t* spv;
  size_t words;
};
template <size_t N, size_t M>
inline SpvPick PickSpv(bool lean, const uint32_t (&plain)[N], const uint32_t (&lean_spv)[M]) {
  return lean ? SpvPick{lean_spv, M} : SpvPick{plain, N};
}

// Registration flag (SDK vulkan/native/shader_swap.h kFlagOwnEpilogue): the replacement does the colour output step
// itself (src\scene\xenos_ps.glsl), so it takes the draws with alpha test, exponent bias or gamma too.
inline constexpr uint32_t kOwnEpilogue = 1;
// (SDK kFlagAnyConstants: a probe that reads no float constants takes any guest shader of its hash.)
inline constexpr uint32_t kAnyConstants = 2;
// Session 80 (OWN_RENDERER_PLAN.md step W0): every wrestler pixel shader of Bryan's entrance and the match (Surface
// passes_ram frames 5831 / 10860), for the flat probe 2 - the most our own wrestler materials could save on their
// pixel side.
inline constexpr uint64_t kWrestlerPixelShaders[] = {
    0xB1F58C0AA0E3C83Aull, 0x1D20CB5D36B02E5Bull, 0xAFB4E9E073971D7Aull, 0x58782AADE08EE063ull, 0x2B8A869CF01395DBull,
    0x91BF021CA7C0C1A5ull, 0xB73643DDFFC89BACull, 0x072B4BDE9B14E56Full, 0xF75AE782D8A42A61ull, 0x9F8F910305279A11ull,
    0x1B66FF87A6433D53ull, 0xFEE45804EDD0EC05ull, 0xB08B5E734D48F5D0ull, 0xE86BD728B5D916BAull, 0x4C9D02186B6E5E9Dull,
    0xC61723668154F870ull};
// Session 80 (OWN_RENDERER_PLAN.md: the arena, step 3): the pixel shaders of the passes around the scene - the blurs and
// downscales (yBlur, yDownScale), the entrance's lighting cubes (yPreLight), the sweat maps (yLiquidMap), the rim light
// map, the shadow maps and circle shadows, the crowd atlas copy (yAudience) - for the flat probe 3: what our own
// versions of them could save on their pixel side (Surface run 028: ~0.4-0.7 ms for many of these small passes).
inline constexpr uint64_t kSidePassPixelShaders[] = {
    0xE1AD4BB4E0563253ull, 0x7C650AA1C792DFF6ull, 0x694A6D148DC33CFFull, 0x632189BAFAE233F0ull, 0x901118D1EF082BECull,
    0x0DC910FA641514C0ull, 0x9C85DA5F0879DA76ull, 0x8479D8DA0BB01D6Bull, 0xF235532B75570EAFull, 0x022F374B74F15579ull,
    0xD03E7D4C719B0EDBull, 0xDA494C9383E18961ull, 0xD05BFEA12F4350ADull, 0x7ABDFEFBCADB4BF9ull};

// The guest pixel shaders replaced (hashes from the shader census, Surface smoke log 2026-11-01; microcode read locally
// from out\s60 / out\s70\shader_dump), and the float constant registers each uses, in order.
// The match crowd (vertex shaders 183CC64A4C4E96AC, 0E2A3DF5BAA2CA18): colour texture x lit colour. No constants.
inline constexpr uint64_t kCrowdMatch = 0xED94EC2F6D52E14Full;
// The entrance crowd (vertex shaders C1028A64EFCF8229, CB5B16DA1312E5AB): the same x the lighting map x c184.z.
inline constexpr uint64_t kCrowdEntrance = 0x803205E3BEA932C8ull;
inline constexpr uint8_t kCrowdEntranceFloats[] = {184};
// The wrestlers' body material (vertex shader F709D14F30FCC95C; 11-16% of the scene passes' vertices), match and
// entrance variants: 11 texture reads (3 cube in the match's), one colour output, no kill.
inline constexpr uint64_t kWrestlerBodyMatch = 0x1B66FF87A6433D53ull;
inline constexpr uint8_t kWrestlerBodyMatchFloats[] = {4,   12,  52,  53,  54,  142, 144, 161, 162, 166, 168,
                                                       169, 170, 180, 181, 182, 187, 189, 190, 195, 197, 204,
                                                       205, 206, 208, 218, 234, 246, 252, 253, 254, 255};
inline constexpr uint64_t kWrestlerBodyEntrance = 0xB1F58C0AA0E3C83Aull;
// The wrestlers' body skin (vertex shader 3F91EC05EDABDF7B, yCh_skin), match and entrance (OWN_RENDERER_PLAN.md W2).
inline constexpr uint64_t kWrestlerSkinMatch = 0xB08B5E734D48F5D0ull;
inline constexpr uint8_t kWrestlerSkinMatchFloats[] = {4,   12,  52,  53,  54,  142, 144, 155, 156, 158, 161, 162,
                                                       166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197,
                                                       199, 201, 204, 205, 210, 218, 234, 246, 253, 254, 255};
inline constexpr uint64_t kWrestlerSkinEntrance = 0x91BF021CA7C0C1A5ull;
// The wrestlers' heads (vertex shader 0E92FB108A675C39, yCh_skin heads): the body skin's constants plus c230 (a second
// normal map's rectangle). The console's 0 x inf = 0 is kept in the material itself (the Intel black eyes).
// The wrestlers' hair / anisotropic material (vertex shader 56DD2E94E20F2826, matches and entrances).
inline constexpr uint64_t kWrestlerHair = 0x58782AADE08EE063ull;
inline constexpr uint8_t kWrestlerHairFloats[] = {4,   12,  161, 162, 180, 181, 182, 187, 189, 190, 195,
                                                  197, 201, 204, 205, 218, 249, 250, 251, 253, 254, 255};
// The body skin's variants: a decal layer (tf2 in the c250 window, c249), and without the paint parts.
inline constexpr uint64_t kWrestlerSkinDecal = 0xE86BD728B5D916BAull;
inline constexpr uint8_t kWrestlerSkinDecalFloats[] = {4, 12, 52, 53, 54, 142, 144, 155, 156, 158, 161, 162, 166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197, 199, 201, 204, 205, 210, 218, 234, 246, 249, 250, 253, 254, 255};
inline constexpr uint64_t kWrestlerSkinDecalEntrance = 0xF75AE782D8A42A61ull;
inline constexpr uint8_t kWrestlerSkinDecalEntranceFloats[] = {0, 1, 2, 3, 4, 8, 9, 10, 11, 12, 142, 144, 155, 156, 158, 161, 162, 166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197, 199, 201, 204, 205, 210, 218, 234, 246, 249, 250, 253, 254, 255};
inline constexpr uint64_t kWrestlerSkinPlain = 0x4C9D02186B6E5E9Dull;
inline constexpr uint8_t kWrestlerSkinPlainFloats[] = {4, 12, 52, 53, 54, 142, 144, 155, 156, 158, 161, 162, 166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197, 199, 201, 204, 205, 210, 218, 254, 255};
inline constexpr uint64_t kWrestlerSkinPlainEntrance = 0x9F8F910305279A11ull;
inline constexpr uint8_t kWrestlerSkinPlainEntranceFloats[] = {0, 1, 2, 3, 4, 8, 9, 10, 11, 12, 142, 144, 155, 156, 158, 161, 162, 166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197, 199, 201, 204, 205, 210, 218, 254, 255};
inline constexpr uint64_t kWrestlerHeadMatch = 0xFEE45804EDD0EC05ull;
inline constexpr uint8_t kWrestlerHeadMatchFloats[] = {4,   12,  52,  53,  54,  142, 144, 155, 156, 158, 161, 162,
                                                       166, 168, 169, 170, 180, 181, 182, 187, 189, 190, 195, 197,
                                                       199, 201, 204, 205, 210, 218, 230, 234, 246, 253, 254, 255};
inline constexpr uint64_t kWrestlerHeadEntrance = 0x1D20CB5D36B02E5Bull;
inline constexpr uint8_t kWrestlerHeadEntranceFloats[] = {0,   1,   2,   3,   4,   8,   9,   10,  11,  12,  142,
                                                          144, 155, 156, 158, 161, 162, 166, 168, 169, 170, 180,
                                                          181, 182, 187, 189, 190, 195, 197, 199, 201, 204, 205,
                                                          210, 218, 230, 234, 246, 253, 254, 255};
inline constexpr uint8_t kWrestlerSkinEntranceFloats[] = {0,   1,   2,   3,   4,   8,   9,   10,  11,  12,
                                                          142, 144, 155, 156, 158, 161, 162, 166, 168, 169,
                                                          170, 180, 181, 182, 187, 189, 190, 195, 197, 199,
                                                          201, 204, 205, 210, 218, 234, 246, 253, 254, 255};
inline constexpr uint8_t kWrestlerBodyEntranceFloats[] = {0,   1,   2,   3,   4,   8,   9,   10,  11,  12,
                                                          142, 144, 161, 162, 166, 168, 169, 170, 180, 181,
                                                          182, 187, 189, 190, 195, 197, 204, 205, 206, 208,
                                                          210, 218, 234, 246, 252, 253, 254, 255};

// Session 80 (OWN_RENDERER_PLAN.md: the arena ours): the arena's materials - the game's yBG_* (background) and yProp_*
// (prop) effects in matches and on the entrance stage, and its particles (effect names from the pacs' shader index;
// shader census of runs 5503 / Surface 028). Each with the float registers its guest shader uses, in order.
struct ArenaMaterial {
  uint64_t hash;
  const uint32_t* spv;
  size_t words;
  const uint8_t* floats;
  size_t float_count;
  const char* name;
};
inline constexpr uint8_t kArenaLodFloats[] = {248};
inline constexpr uint8_t kArenaLmFloats[] = {184, 248};
inline constexpr uint8_t kArenaScrollLmFloats[] = {184};
inline constexpr uint8_t kArenaSpecFloats[] = {8, 200, 201, 202, 248, 255};
inline constexpr uint8_t kArenaScrollFloats[] = {255};
inline constexpr uint8_t kArenaShadowFloats[] = {8, 168, 169, 170, 200, 201, 202, 248, 255};
inline constexpr uint8_t kArenaShadowPlainFloats[] = {168, 169, 170, 248, 255};
inline constexpr uint8_t kArenaPsmFloats[] = {207, 254, 255};
inline constexpr uint8_t kArenaBumpFloats[] = {0, 1, 2, 3, 8, 9, 10, 11, 74, 180, 181, 182, 189, 190, 200, 201, 202, 248,
                                               255};
inline constexpr uint8_t kArenaBumpReflectFloats[] = {0,   1,   2,   3,   8,   9,   10,  11,  74,  180, 181,
                                                      182, 189, 190, 200, 201, 202, 208, 248, 253, 254, 255};
inline constexpr uint8_t kArenaScrollBlendLmFloats[] = {184, 255};
inline constexpr uint8_t kArenaPsmLmFloats[] = {184, 207, 254, 255};
inline constexpr uint8_t kArenaSpecLmFloats[] = {8, 184, 200, 201, 202, 248, 255};
inline constexpr uint8_t kArenaShadowLmFloats[] = {168, 169, 170, 184, 248, 255};
inline constexpr uint8_t kArenaShadowSpecLmFloats[] = {8, 168, 169, 170, 184, 200, 201, 202, 248, 255};
inline constexpr uint8_t kArenaBumpLmFloats[] = {0,   1,   2,   3,   8,   9,   10,  11,  74,  166, 168, 169, 170,
                                                 180, 181, 182, 184, 189, 190, 200, 201, 202, 248, 254, 255};
inline const ArenaMaterial kArenaMaterials[] = {
    {0xF335375CE1CC39E4ull, arena_bg_spv, std::size(arena_bg_spv), kArenaLodFloats, std::size(kArenaLodFloats),
     "arena background (yBG_Default)"},
    {0x468C06E826088769ull, arena_prop_spv, std::size(arena_prop_spv), kArenaLodFloats, std::size(kArenaLodFloats),
     "arena prop (yProp_Default)"},
    {0x34B29856453FE506ull, arena_particle_spv, std::size(arena_particle_spv), nullptr, 0, "arena particle (yParticle)"},
    {0x2BC8853B576D423Eull, arena_bg_lm_spv, std::size(arena_bg_lm_spv), kArenaLmFloats, std::size(kArenaLmFloats),
     "arena background lit (yBG_DefaultLM)"},
    {0x047BBF321B125654ull, arena_prop_lm_spv, std::size(arena_prop_lm_spv), kArenaLmFloats, std::size(kArenaLmFloats),
     "arena prop lit (yProp_DefaultLM)"},
    {0x12280371DFCC7F44ull, arena_bg_scroll_lm_spv, std::size(arena_bg_scroll_lm_spv), kArenaScrollLmFloats,
     std::size(kArenaScrollLmFloats), "arena background scroll lit (yBG_UVScrollLM)"},
    {0xB73643DDFFC89BACull, arena_prop_spec_spv, std::size(arena_prop_spec_spv), kArenaSpecFloats,
     std::size(kArenaSpecFloats), "arena prop highlight (yProp_Default)"},
    {0x072B4BDE9B14E56Full, arena_prop_spec_mask_spv, std::size(arena_prop_spec_mask_spv), kArenaSpecFloats,
     std::size(kArenaSpecFloats), "arena prop masked highlight (yProp_Default)"},
    {0x775AF960824D6ADCull, arena_bg_spec_spv, std::size(arena_bg_spec_spv), kArenaSpecFloats,
     std::size(kArenaSpecFloats), "arena background highlight (yBG_Default)"},
    {0xDB603A91AD911046ull, arena_bg_scroll_spv, std::size(arena_bg_scroll_spv), kArenaScrollFloats,
     std::size(kArenaScrollFloats), "arena background two-layer scroll (yBG_UVScroll)"},
    // The floor's shadow-receiving materials and the ring mat (drafted on the arithmetic harness, worst 2e-5).
    {0xE98D0C3605046302ull, arena_bg_shadow_spv, std::size(arena_bg_shadow_spv), kArenaShadowFloats,
     std::size(kArenaShadowFloats), "arena floor shadowed highlight (yBG_Default)"},
    {0xB63CD5FCA34C147Dull, arena_bg_shadow_plain_spv, std::size(arena_bg_shadow_plain_spv), kArenaShadowPlainFloats,
     std::size(kArenaShadowPlainFloats), "arena floor shadowed (yBG_Default)"},
    {0xB87AA657F15D8EC5ull, arena_prop_psm_spv, std::size(arena_prop_psm_spv), kArenaPsmFloats,
     std::size(kArenaPsmFloats), "arena ring mat plane shadow (yProp_PSM)"},
    // The normal-mapped props and the ropes (four directional lights + sky / ground ambient; harness worst 1e-5,
    // the reflecting one 1.6e-4 - cancellation noise).
    {0x2B8A869CF01395DBull, arena_prop_bump_spv, std::size(arena_prop_bump_spv), kArenaBumpFloats,
     std::size(kArenaBumpFloats), "arena prop normal-mapped (yProp_BumpMap)"},
    {0xAFB4E9E073971D7Aull, arena_prop_bump_rope_spv, std::size(arena_prop_bump_rope_spv), kArenaBumpFloats,
     std::size(kArenaBumpFloats), "arena ropes (yProp_BumpMap)"},
    {0xC61723668154F870ull, arena_prop_bump_rope2_spv, std::size(arena_prop_bump_rope2_spv), kArenaBumpFloats,
     std::size(kArenaBumpFloats), "arena ropes, mask in the normal map (yProp_BumpMap)"},
    {0x741439F55F5B15D4ull, arena_prop_bump_reflect_spv, std::size(arena_prop_bump_reflect_spv),
     kArenaBumpReflectFloats, std::size(kArenaBumpReflectFloats), "arena prop normal-mapped, reflecting (yProp_BumpMap)"},
    // The entrance stage's lightmapped variants (harness worst ~1e-5 absolute).
    {0xDDBEB05B1F79ABF4ull, arena_bg_scroll_blend_lm_spv, std::size(arena_bg_scroll_blend_lm_spv),
     kArenaScrollBlendLmFloats, std::size(kArenaScrollBlendLmFloats),
     "arena background two-layer scroll lit (yBG_UVScrollLM)"},
    {0x4E1A94BC95B4C2E9ull, arena_prop_psm_lm_spv, std::size(arena_prop_psm_lm_spv), kArenaPsmLmFloats,
     std::size(kArenaPsmLmFloats), "arena prop plane shadow lit (yProp_PSMLM)"},
    {0xF7FA89EF70876EFFull, arena_prop_spec_mask_lm_spv, std::size(arena_prop_spec_mask_lm_spv), kArenaSpecLmFloats,
     std::size(kArenaSpecLmFloats), "arena prop masked highlight lit (yProp_DefaultLM)"},
    {0xAA33C0D66574EE32ull, arena_bg_spec_mask_lm_spv, std::size(arena_bg_spec_mask_lm_spv), kArenaSpecLmFloats,
     std::size(kArenaSpecLmFloats), "arena background highlight lit (yBG_DefaultLM)"},
    {0xED3793930D3A2C1Dull, arena_bg_shadow_lm_spv, std::size(arena_bg_shadow_lm_spv), kArenaShadowLmFloats,
     std::size(kArenaShadowLmFloats), "arena floor shadowed lit (yBG_DefaultLM)"},
    {0xA0A9844B8A45CC6Bull, arena_bg_bump_lm_spv, std::size(arena_bg_bump_lm_spv), kArenaBumpLmFloats,
     std::size(kArenaBumpLmFloats), "arena background normal-mapped lit (yBG_BumpMapLM)"},
    {0x748BF8B0868777D3ull, arena_bg_spec_plain_spv, std::size(arena_bg_spec_plain_spv), kArenaSpecFloats,
     std::size(kArenaSpecFloats), "arena background highlight, unmasked (yBG_Default)"},
    {0x9B46FDF960C4CC1Full, arena_bg_shadow_spec_mask_lm_spv, std::size(arena_bg_shadow_spec_mask_lm_spv),
     kArenaShadowSpecLmFloats, std::size(kArenaShadowSpecLmFloats),
     "arena floor shadowed masked highlight lit (yBG_DefaultLM)"},
};

// Session 80 (OWN_RENDERER_PLAN.md: the passes around the scene ours): the side passes' shaders (Surface run 029: the
// flat probe 3 on them saved 14.9% of the match frame, 8.9% of Bryan's): five small ones written by hand, the blurs,
// downscale, sweat layer and lighting cubes drafted on the arithmetic harness (bit-identical but the 21-tap blur, 3e-6).
// Several keep the fetch constant's LOD bias (they walk mip chains); the 16-bit shadow maps get the translation's
// fixed-point scaling through xenos_ps.glsl's kXeColorQuantize (the SDK sets it per translation).
inline constexpr uint8_t kShadowMapFloats[] = {168};
inline constexpr uint8_t kCircleShadowFloats[] = {255};
inline constexpr uint8_t kBlur21Floats[] = {34, 253, 254, 255};
inline constexpr uint8_t kPrelightFloats[] = {0,  1,  2,   3,   8,   9,   10,  11,  16,  17,  18,
                                              19, 198, 199, 204, 205, 206, 207, 208, 209, 254, 255};
inline constexpr uint8_t kDownscale4Floats[] = {0, 1, 2, 3, 255};
inline constexpr uint8_t kBlur4MaskFloats[] = {0, 1, 255};
inline constexpr uint8_t kLiquidLayerFloats[] = {203, 204, 255};
inline constexpr uint8_t kBlur5Floats[] = {0, 1, 16, 17};
inline constexpr uint8_t kBlur10Floats[] = {0, 1, 2, 3, 16, 17};
inline constexpr uint8_t kMovieFloats[] = {253, 254, 255};
inline constexpr uint8_t kBloomBrightFloats[] = {0, 1, 2, 3, 16, 255};
inline constexpr uint8_t kGlareBlurFloats[] = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23};
inline constexpr uint8_t kGlareCombineFloats[] = {16, 17, 18, 19};
inline constexpr uint8_t kSpriteModesFloats[] = {254, 255};
inline constexpr uint8_t kDofDownsampleFloats[] = {0, 1, 2, 3, 21, 255};
inline constexpr uint8_t kBlur15Floats[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14,
                                            16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30};
inline constexpr uint8_t kDofBlurFloats[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 16,
                                             17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 255};
inline constexpr uint8_t kBlur13Floats[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
                                            16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28};
inline const ArenaMaterial kSidePasses[] = {
    {0xD03E7D4C719B0EDBull, rim_light_map_spv, std::size(rim_light_map_spv), nullptr, 0, "rim light map (yRimLightMap)"},
    {0xDA494C9383E18961ull, shadow_map_spv, std::size(shadow_map_spv), kShadowMapFloats, std::size(kShadowMapFloats),
     "shadow map depth (yShadowMap)"},
    {0xF235532B75570EAFull, liquid_map_copy_spv, std::size(liquid_map_copy_spv), nullptr, 0,
     "sweat map copy (yLiquidMap)"},
    {0x7ABDFEFBCADB4BF9ull, audience_copy_spv, std::size(audience_copy_spv), nullptr, 0, "crowd atlas copy (yAudience)"},
    {0xD05BFEA12F4350ADull, circle_shadow_spv, std::size(circle_shadow_spv), kCircleShadowFloats,
     std::size(kCircleShadowFloats), "circle shadow (yCircleShadow)"},
    {0xE1AD4BB4E0563253ull, side_blur21_spv, std::size(side_blur21_spv), kBlur21Floats, std::size(kBlur21Floats),
     "plane shadow 21-tap blur (yBlur)"},
    {0x8479D8DA0BB01D6Bull, side_prelight_spv, std::size(side_prelight_spv), kPrelightFloats,
     std::size(kPrelightFloats), "lighting cubes (yPreLight)"},
    {0x9C85DA5F0879DA76ull, side_downscale4_spv, std::size(side_downscale4_spv), kDownscale4Floats,
     std::size(kDownscale4Floats), "4-tap downscale (yDownScale)"},
    {0x0DC910FA641514C0ull, side_blur4_mask_spv, std::size(side_blur4_mask_spv), kBlur4MaskFloats,
     std::size(kBlur4MaskFloats), "4-tap mask blur (yBlur)"},
    {0x901118D1EF082BECull, side_blur4_mask_inv_spv, std::size(side_blur4_mask_inv_spv), kBlur4MaskFloats,
     std::size(kBlur4MaskFloats), "4-tap inverted mask blur (yBlur)"},
    {0x022F374B74F15579ull, side_liquid_layer_spv, std::size(side_liquid_layer_spv), kLiquidLayerFloats,
     std::size(kLiquidLayerFloats), "sweat map layer (yLiquidMap)"},
    {0x694A6D148DC33CFFull, side_blur5_spv, std::size(side_blur5_spv), kBlur5Floats, std::size(kBlur5Floats),
     "shadow 5-tap blur (yBlur)"},
    {0x7C650AA1C792DFF6ull, side_blur10_spv, std::size(side_blur10_spv), kBlur10Floats, std::size(kBlur10Floats),
     "shadow 10-tap blur (yBlur)"},
    {0x632189BAFAE233F0ull, side_blur13_spv, std::size(side_blur13_spv), kBlur13Floats, std::size(kBlur13Floats),
     "13-tap blur (yBlur)"},
    // The small ones left in the match and Bryan's frames (census of run 5503): the flat-colour primitive (the clears and
    // the plane shadow's mask), the overlays' plain textured material, the 2D crowd sprites, the titantron's colour
    // conversion.
    {0xA4A965C189287B99ull, debug_primitive_spv, std::size(debug_primitive_spv), nullptr, 0,
     "flat-colour primitive (yDebugPrimitive)"},
    {0x9A39D20F274CF7D9ull, default_textured_spv, std::size(default_textured_spv), nullptr, 0, "textured (yDefault)"},
    {0xEFC4EFCB5FC51945ull, crowd_2d_spv, std::size(crowd_2d_spv), kArenaScrollLmFloats, std::size(kArenaScrollLmFloats),
     "2D crowd sprites (yAudience)"},
    {0xB5853824AE01D2D8ull, movie_yuv_spv, std::size(movie_yuv_spv), kMovieFloats, std::size(kMovieFloats),
     "titantron video colour conversion (yMovieDefault)"},
    // The post chain's last ones: the match's depth-of-field downsample and blur, the entrance bloom's bright pass and
    // blur (drafted on the harness, bit-identical; they keep the fetch constant's LOD bias).
    {0x156FA9A105B45F9Bull, side_bloom_bright4_spv, std::size(side_bloom_bright4_spv), kBloomBrightFloats,
     std::size(kBloomBrightFloats), "bloom bright pass (yDownScale)"},
    {0xA7AD68BE21C46E7Aull, side_dof_downsample_spv, std::size(side_dof_downsample_spv), kDofDownsampleFloats,
     std::size(kDofDownsampleFloats), "depth of field downsample (yDownScale)"},
    {0x755AC86C4301F2BBull, side_blur15_spv, std::size(side_blur15_spv), kBlur15Floats, std::size(kBlur15Floats),
     "15-tap blur (yBlur)"},
    {0xA5C9AC54E4868DC1ull, side_dof_blur_spv, std::size(side_dof_blur_spv), kDofBlurFloats, std::size(kDofBlurFloats),
     "depth of field blur (yBlur)"},
    // Brock's entrance (census 5531): the star glare's directional blur and streak combine, and the two sprite shaders
    // (ySprite: the video-style sprite with an alpha plane, and the moded sprite - also the menus' and overlays').
    {0x75641266549A40C0ull, side_glare_blur8_spv, std::size(side_glare_blur8_spv), kGlareBlurFloats,
     std::size(kGlareBlurFloats), "glare 8-tap blur (yBlur)"},
    {0x9C84E33D39058FB0ull, side_glare_combine_spv, std::size(side_glare_combine_spv), kGlareCombineFloats,
     std::size(kGlareCombineFloats), "glare streak combine (yBlur)"},
    {0xB57EADC321958E8Full, sprite_movie_yuv_alpha_spv, std::size(sprite_movie_yuv_alpha_spv), kMovieFloats,
     std::size(kMovieFloats), "sprite, video with alpha plane (ySprite)"},
    {0x7754C61815DA6614ull, sprite_modes_spv, std::size(sprite_modes_spv), kSpriteModesFloats,
     std::size(kSpriteModesFloats), "sprite, moded (ySprite)"},
};

// Session 80 (OWN_RENDERER_PLAN.md: every shader of the frame ours): the wrestlers' last materials - yCh_trans (the
// hair family, alpha from the normal map; match: a reflection cube, entrance: four light lobes), yCh_Reflect (the body /
// gear family with a second cube), a second yCh_skin (the plain skin without its detail part). Drafted on the
// arithmetic harness (default constants: worst 1e-5 to 7e-4, cancellation).
inline constexpr uint8_t kTransFloats[] = {4,   12,  52,  53,  54,  142, 144, 161, 162, 180, 181, 182,
                                           187, 189, 190, 195, 197, 201, 204, 205, 218, 253, 254, 255};
inline constexpr uint8_t kTransLobesFloats[] = {0,   1,   2,   3,   4,   8,   9,   10,  11,  12,  142, 144, 161, 162,
                                                180, 181, 182, 187, 189, 190, 195, 197, 201, 204, 205, 218, 254, 255};
inline constexpr uint8_t kReflectFloats[] = {4,   12,  52,  53,  54,  142, 144, 161, 162, 166, 168, 169, 170, 180,
                                             181, 182, 187, 189, 190, 195, 197, 204, 205, 218, 247, 254, 255};
inline constexpr uint8_t kReflectEntranceFloats[] = {0,   1,   2,   3,   4,   8,   9,   10,  11,  12,  142,
                                                     144, 161, 162, 166, 168, 169, 170, 180, 181, 182, 187,
                                                     189, 190, 195, 197, 204, 205, 218, 247, 253, 254, 255};
inline constexpr uint8_t kSkin2Floats[] = {4,   12,  52,  53,  54,  142, 144, 155, 156, 158, 161,
                                           162, 166, 168, 169, 170, 180, 181, 182, 187, 189, 190,
                                           195, 197, 199, 201, 204, 205, 218, 254, 255};
inline constexpr uint8_t kSkin2EntranceFloats[] = {0,   1,   2,   3,   4,   8,   9,   10,  11,  12,  142, 144,
                                                   155, 156, 158, 161, 162, 166, 168, 169, 170, 180, 181, 182,
                                                   187, 189, 190, 195, 197, 199, 201, 204, 205, 218, 254, 255};
// Session 93 (idea 8, --wwe2k14_shader_mediump): the arena materials that have a second compile with hand-placed mediump
// colour maths (tools\postuild_shaders.ps1 $MediumpShaders; out\s938_mediump_notes.md), by the guest hash.
struct MediumpVariant {
  uint64_t hash;
  const uint32_t* spv;
  size_t words;
};
inline const MediumpVariant kMediumpVariants[] = {
    {0x2B8A869CF01395DBull, arena_prop_bump_mp_spv, std::size(arena_prop_bump_mp_spv)},
    {0xAFB4E9E073971D7Aull, arena_prop_bump_rope_mp_spv, std::size(arena_prop_bump_rope_mp_spv)},
    {0xC61723668154F870ull, arena_prop_bump_rope2_mp_spv, std::size(arena_prop_bump_rope2_mp_spv)},
    {0x741439F55F5B15D4ull, arena_prop_bump_reflect_mp_spv, std::size(arena_prop_bump_reflect_mp_spv)},
    {0xF7FA89EF70876EFFull, arena_prop_spec_mask_lm_mp_spv, std::size(arena_prop_spec_mask_lm_mp_spv)},
    {0xAA33C0D66574EE32ull, arena_bg_spec_mask_lm_mp_spv, std::size(arena_bg_spec_mask_lm_mp_spv)},
    {0xED3793930D3A2C1Dull, arena_bg_shadow_lm_mp_spv, std::size(arena_bg_shadow_lm_mp_spv)},
    {0xA0A9844B8A45CC6Bull, arena_bg_bump_lm_mp_spv, std::size(arena_bg_bump_lm_mp_spv)},
    {0x9B46FDF960C4CC1Full, arena_bg_shadow_spec_mask_lm_mp_spv, std::size(arena_bg_shadow_spec_mask_lm_mp_spv)},
};

inline const ArenaMaterial kWrestlerMore[] = {
    {0xCA3E51C5BA993041ull, wrestler_trans_spv, std::size(wrestler_trans_spv), kTransFloats, std::size(kTransFloats),
     "wrestler trans (yCh_trans)"},
    {0x9864A5F6683152D7ull, wrestler_trans_lobes_spv, std::size(wrestler_trans_lobes_spv), kTransLobesFloats,
     std::size(kTransLobesFloats), "wrestler trans, light lobes (yCh_trans)"},
    {0x3C06B3329BE81996ull, wrestler_reflect_spv, std::size(wrestler_reflect_spv), kReflectFloats,
     std::size(kReflectFloats), "wrestler reflect (yCh_Reflect)"},
    {0xBBB2E028DCF36E8Dull, wrestler_reflect_entrance_spv, std::size(wrestler_reflect_entrance_spv),
     kReflectEntranceFloats, std::size(kReflectEntranceFloats), "wrestler reflect (entrance, yCh_Reflect)"},
    {0x9C2FD0E90E71F4A3ull, wrestler_skin2_spv, std::size(wrestler_skin2_spv), kSkin2Floats, std::size(kSkin2Floats),
     "wrestler skin 2 (yCh_skin)"},
    {0x925085B612A44F10ull, wrestler_skin2_entrance_spv, std::size(wrestler_skin2_entrance_spv), kSkin2EntranceFloats,
     std::size(kSkin2EntranceFloats), "wrestler skin 2 (entrance, yCh_skin)"},
};

// Session 80 (OWN_RENDERER_PLAN.md: the crowd fully ours): our own crowd programs (src\scene\own_crowd_*), drawn by the
// SDK's own crowd renderer in place of the game's crowd draws when --gpu_native_own_crowd is on (live; off by
// default). By the run's guest vertex shader: the match's lit crowd, the entrances' (lighting map per pixel).
inline constexpr uint64_t kCrowdMatchVs = 0x183CC64A4C4E96ACull;
inline constexpr uint64_t kCrowdEntranceVs = 0xC1028A64EFCF8229ull;
// Session 87 (step 1 piece 2): the crowd's flat cards (T_Audience, pixel shader EFC4EFCB5FC51945; 4-dword card
// vertices), flags 1 (the lighting map read per pixel, as the entrances') + 2 (a card mesh). Its pixel shader is the
// entrances' own: sprite x vertex colour (white) x lighting map x c184.z.
inline constexpr uint64_t kCrowdCardVs = 0xD5E2FCC9F76BC40Cull;

inline void InstallOwnCrowd(rex::graphics::CommandProcessor* cp) {
  int ok = 0;
  // Session 95 (gpu_d3d_decouple F5): with the views on, the vertex shaders that read view 1's camera (CrowdScene vec4 24-27, filled by the SDK with the blended camera);
  // with it off, exactly the plain ones as before.
  const bool mv = pixel_multiview::On();
  ok += cp->RegisterNativeOwnCrowd(kCrowdMatchVs, mv ? own_crowd_match_mv_vert_spv : own_crowd_match_vert_spv,
                                   mv ? std::size(own_crowd_match_mv_vert_spv) : std::size(own_crowd_match_vert_spv),
                                   own_crowd_match_spv, std::size(own_crowd_match_spv), 0);
  ok += cp->RegisterNativeOwnCrowd(kCrowdEntranceVs, mv ? own_crowd_entrance_mv_vert_spv : own_crowd_entrance_vert_spv,
                                   mv ? std::size(own_crowd_entrance_mv_vert_spv) : std::size(own_crowd_entrance_vert_spv), own_crowd_entrance_spv,
                                   std::size(own_crowd_entrance_spv), 1);
  ok += cp->RegisterNativeOwnCrowd(kCrowdCardVs, mv ? own_crowd_card_mv_vert_spv : own_crowd_card_vert_spv,
                                   mv ? std::size(own_crowd_card_mv_vert_spv) : std::size(own_crowd_card_vert_spv),
                                   own_crowd_entrance_spv, std::size(own_crowd_entrance_spv), 1 | 2);
  // Session 95 (P7, gpu_native_crowd_input): each program's attribute form (flags bit 28, as RegisterNativeVertexSwap's), used per run only when the SDK's switch and the run's plan allow it.
  constexpr uint32_t kAttributeForm = 0x10000000u;
  cp->RegisterNativeOwnCrowd(kCrowdMatchVs, mv ? own_crowd_match_mv_vi_vert_spv : own_crowd_match_vi_vert_spv, mv ? std::size(own_crowd_match_mv_vi_vert_spv) : std::size(own_crowd_match_vi_vert_spv),
                             own_crowd_match_spv, std::size(own_crowd_match_spv), kAttributeForm);
  cp->RegisterNativeOwnCrowd(kCrowdEntranceVs, mv ? own_crowd_entrance_mv_vi_vert_spv : own_crowd_entrance_vi_vert_spv, mv ? std::size(own_crowd_entrance_mv_vi_vert_spv) : std::size(own_crowd_entrance_vi_vert_spv),
                             own_crowd_entrance_spv, std::size(own_crowd_entrance_spv), kAttributeForm);
  cp->RegisterNativeOwnCrowd(kCrowdCardVs, mv ? own_crowd_card_mv_vi_vert_spv : own_crowd_card_vi_vert_spv, mv ? std::size(own_crowd_card_mv_vi_vert_spv) : std::size(own_crowd_card_vi_vert_spv),
                             own_crowd_entrance_spv, std::size(own_crowd_entrance_spv), kAttributeForm);
  REXLOG_INFO("[wwe2k14][scene_swap] own crowd programs registered: {} (gpu_native_own_crowd = {})", ok,
              rex::cvar::GetFlagByName("gpu_native_own_crowd"));
}

// Once, after the graphics system exists (OnPostSetup).
inline void Install() {
  if (rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp()) InstallOwnCrowd(cp);
  const int32_t probe = REXCVAR_GET(wwe2k14_scene_shaders_probe);
  if (!REXCVAR_GET(wwe2k14_scene_shaders) && !probe) return;
  rex::graphics::CommandProcessor* cp = wwe2k14::half2::Cp();
  if (!cp) {
    REXLOG_WARN("[wwe2k14][scene_swap] no command processor yet: no scene shaders registered");
    return;
  }
  int ok = 0;
  // Session 95 (B2, gpu_native_shader_ablate): the flat module the SDK swaps in for the families named by the live cvar (kAblationHash, SDK shader_swap.h).
  if (REXCVAR_GET(wwe2k14_scene_shaders)) {
    ok += cp->RegisterNativeShaderSwap(0xAB1A7E0000000001ull, probe_flat_spv, std::size(probe_flat_spv), nullptr, 0,
                                       "ablation: flat grey (gpu_native_shader_ablate)", kOwnEpilogue | kAnyConstants);
  }
  if (probe == 2) {
    for (const uint64_t hash : kWrestlerPixelShaders) {
      ok += cp->RegisterNativeShaderSwap(hash, probe_flat_spv, std::size(probe_flat_spv), nullptr, 0,
                                         "probe: flat wrestler", kOwnEpilogue | kAnyConstants);
    }
  }
  if (probe == 3) {
    for (const uint64_t hash : kSidePassPixelShaders) {
      ok += cp->RegisterNativeShaderSwap(hash, probe_flat_spv, std::size(probe_flat_spv), nullptr, 0,
                                         "probe: flat side pass", kOwnEpilogue | kAnyConstants);
    }
  }
  if (probe == 1) {
    ok += cp->RegisterNativeShaderSwap(kWrestlerBodyMatch, probe_flat_spv, std::size(probe_flat_spv),
                                       kWrestlerBodyMatchFloats, std::size(kWrestlerBodyMatchFloats),
                                       "probe: flat wrestler body (match)", kOwnEpilogue);
    ok += cp->RegisterNativeShaderSwap(kWrestlerBodyEntrance, probe_flat_spv, std::size(probe_flat_spv),
                                       kWrestlerBodyEntranceFloats, std::size(kWrestlerBodyEntranceFloats),
                                       "probe: flat wrestler body (entrance)", kOwnEpilogue);
  }
  if (!REXCVAR_GET(wwe2k14_scene_shaders)) {
    REXLOG_INFO("[wwe2k14][scene_swap] probe {} registered: {}", probe, ok);
    return;
  }
  // Session 80 (OWN_RENDERER_PLAN.md W2): the wrestlers' gear material in matches (src\scene\wrestler_body.frag; its
  // arithmetic checked against the translation on 8,192 random inputs, ~1e-6), with its own colour output step.
  const bool wlean = REXCVAR_GET(gpu_native_wrestler_lean);
  if (wlean) {
    REXLOG_INFO("[wwe2k14][scene_swap] [native] gpu_native_wrestler_lean: LEAN compiles for the wrestlers' match gear / skin / skin plain / skin decal / head / hair");
  }
  const bool wpow = REXCVAR_GET(gpu_native_wrestler_pow);  // (Session 95, r3gpu)
  if (wpow) REXLOG_INFO("[wwe2k14][scene_swap] [native] gpu_native_wrestler_pow: POWLEAN compiles for the wrestlers' entrance gear / skin / skin plain / skin decal / head");
  const SpvPick pk_e_body = PickSpv(wpow, wrestler_body_entrance_spv, wrestler_body_entrance_pow_spv);
  const SpvPick pk_e_skin = PickSpv(wpow, wrestler_skin_entrance_spv, wrestler_skin_entrance_pow_spv);
  const SpvPick pk_e_head = PickSpv(wpow, wrestler_head_entrance_spv, wrestler_head_entrance_pow_spv);
  const SpvPick pk_e_decal = PickSpv(wpow, wrestler_skin_decal_entrance_spv, wrestler_skin_decal_entrance_pow_spv);
  const SpvPick pk_e_plain = PickSpv(wpow, wrestler_skin_plain_entrance_spv, wrestler_skin_plain_entrance_pow_spv);
  const SpvPick pk_body = PickSpv(wlean, wrestler_body_spv, wrestler_body_lean_spv);
  const SpvPick pk_skin = PickSpv(wlean, wrestler_skin_spv, wrestler_skin_lean_spv);
  const SpvPick pk_head = PickSpv(wlean, wrestler_head_spv, wrestler_head_lean_spv);
  const SpvPick pk_hair = PickSpv(wlean, wrestler_hair_spv, wrestler_hair_lean_spv);
  const SpvPick pk_decal = PickSpv(wlean, wrestler_skin_decal_spv, wrestler_skin_decal_lean_spv);
  const SpvPick pk_plain = PickSpv(wlean, wrestler_skin_plain_spv, wrestler_skin_plain_lean_spv);
  ok += cp->RegisterNativeShaderSwap(kWrestlerBodyMatch, pk_body.spv, pk_body.words,
                                     kWrestlerBodyMatchFloats, std::size(kWrestlerBodyMatchFloats),
                                     "wrestler gear (match)", kOwnEpilogue);
  // The entrances' gear (src\scene\wrestler_body_entrance.frag: four directional lights in place of the reflection
  // cube; arithmetic checked the same way, ~3e-6).
  ok += cp->RegisterNativeShaderSwap(kWrestlerBodyEntrance, pk_e_body.spv, pk_e_body.words, kWrestlerBodyEntranceFloats,
                                     std::size(kWrestlerBodyEntranceFloats), "wrestler gear (entrance)", kOwnEpilogue);
  // The body skin (src\scene\wrestler_skin*.frag; arithmetic checked against the translation, worst ~5e-5).
  ok += cp->RegisterNativeShaderSwap(kWrestlerSkinMatch, pk_skin.spv, pk_skin.words,
                                     kWrestlerSkinMatchFloats, std::size(kWrestlerSkinMatchFloats),
                                     "wrestler skin (match)", kOwnEpilogue);
  ok += cp->RegisterNativeShaderSwap(kWrestlerSkinEntrance, pk_e_skin.spv, pk_e_skin.words, kWrestlerSkinEntranceFloats,
                                     std::size(kWrestlerSkinEntranceFloats), "wrestler skin (entrance)", kOwnEpilogue);
  // The heads (src\scene\wrestler_head*.frag; arithmetic worst ~2e-4).
  ok += cp->RegisterNativeShaderSwap(kWrestlerHeadMatch, pk_head.spv, pk_head.words,
                                     kWrestlerHeadMatchFloats, std::size(kWrestlerHeadMatchFloats),
                                     "wrestler head (match)", kOwnEpilogue);
  ok += cp->RegisterNativeShaderSwap(kWrestlerHeadEntrance, pk_e_head.spv, pk_e_head.words, kWrestlerHeadEntranceFloats,
                                     std::size(kWrestlerHeadEntranceFloats), "wrestler head (entrance)", kOwnEpilogue);
  // The hair (src\scene\wrestler_hair.frag; arithmetic worst ~3e-4).
  ok += cp->RegisterNativeShaderSwap(kWrestlerHair, pk_hair.spv, pk_hair.words, kWrestlerHairFloats,
                                     std::size(kWrestlerHairFloats), "wrestler hair", kOwnEpilogue);
  // The skin variants (src\scene\wrestler_skin_{decal,plain}*.frag; arithmetic worst ~6e-5).
  ok += cp->RegisterNativeShaderSwap(kWrestlerSkinDecal, pk_decal.spv, pk_decal.words, kWrestlerSkinDecalFloats, std::size(kWrestlerSkinDecalFloats),
                                     "wrestler skin decal (match)", kOwnEpilogue);
  ok += cp->RegisterNativeShaderSwap(kWrestlerSkinDecalEntrance, pk_e_decal.spv, pk_e_decal.words, kWrestlerSkinDecalEntranceFloats, std::size(kWrestlerSkinDecalEntranceFloats),
                                     "wrestler skin decal (entrance)", kOwnEpilogue);
  ok += cp->RegisterNativeShaderSwap(kWrestlerSkinPlain, pk_plain.spv, pk_plain.words, kWrestlerSkinPlainFloats, std::size(kWrestlerSkinPlainFloats),
                                     "wrestler skin plain (match)", kOwnEpilogue);
  ok += cp->RegisterNativeShaderSwap(kWrestlerSkinPlainEntrance, pk_e_plain.spv, pk_e_plain.words, kWrestlerSkinPlainEntranceFloats, std::size(kWrestlerSkinPlainEntranceFloats),
                                     "wrestler skin plain (entrance)", kOwnEpilogue);
  const bool mediump = REXCVAR_GET(wwe2k14_shader_mediump);
  int mediump_registered = 0;
  for (const ArenaMaterial& m : kArenaMaterials) {
    const uint32_t* spv = m.spv;
    size_t words = m.words;
    if (mediump) {
      for (const MediumpVariant& v : kMediumpVariants) {
        if (v.hash == m.hash) {
          spv = v.spv;
          words = v.words;
          ++mediump_registered;
          break;
        }
      }
    }
    ok += cp->RegisterNativeShaderSwap(m.hash, spv, words, m.floats, m.float_count, m.name, kOwnEpilogue);
  }
  if (mediump) {
    REXLOG_INFO("[wwe2k14][scene_swap] mediump variants registered {} (arena materials; the wrestlers, crowd and side passes stay highp)",
                mediump_registered);
  }
  for (const ArenaMaterial& m : kWrestlerMore) {
    ok += cp->RegisterNativeShaderSwap(m.hash, m.spv, m.words, m.floats, m.float_count, m.name, kOwnEpilogue);
  }
  // (Probe 3 registers the flat probe on these hashes first; with it, the probe stays.)
  if (probe != 3) {
    for (const ArenaMaterial& m : kSidePasses) {
      ok += wwe2k14::pixel_multiview::Register(cp, m.hash, m.spv, m.words, m.floats, m.float_count, m.name, kOwnEpilogue);
    }
  }
  // (Session 87 stage 4: with its own epilogue - crowd_match.frag ends in xenos_ps.glsl's XeOutputColor - so the draws whose
  // colour epilogue is not idle (alpha test / exponent bias / gamma) no longer keep the translation.)
  ok += cp->RegisterNativeShaderSwap(kCrowdMatch, crowd_match_spv, std::size(crowd_match_spv), nullptr, 0,
                                     "crowd (match)", kOwnEpilogue);
  ok += cp->RegisterNativeShaderSwap(kCrowdEntrance, crowd_entrance_spv, std::size(crowd_entrance_spv),
                                     kCrowdEntranceFloats, std::size(kCrowdEntranceFloats), "crowd (entrance)",
                                     kOwnEpilogue);  // (Session 87 stage 4: crowd_entrance.frag ends in XeOutputColor)
  REXLOG_INFO("[wwe2k14][scene_swap] scene shaders registered: {} (gpu_native_shader_swap = {})", ok,
              rex::cvar::GetFlagByName("gpu_native_shader_swap"));
}

}  // namespace wwe2k14::scene_swap
