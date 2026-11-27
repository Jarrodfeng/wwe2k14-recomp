// Session 96 (the asset runtime, Jarrod 2026-11-08: "From scratch", "Census speed", "PC only", "part of the launcher ...
// a button in advanced"): the CPU-side form of a wrestler or an arena, read from the player's own game files at run time
// (CLAUDE.md rules 1 and 2: nothing from the game is shipped or committed). This header is the CONTRACT between the
// loaders (src\assets\asset_load.h: pac -> JBOY -> these structs) and the viewer (src\assets\view\: D3D11, cameras,
// shots, the command pipe; the program wwe2k14_assets.exe, started by the launcher's Advanced button). Formats: docs\systems\assets.md (from out\runtime\research_*.md, Session 95-96).
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace assets {

// One vertex as the viewer draws it (little-endian, host floats). Filled from the JBOY's planar streams:
// S0 position + normal + colour, S1 skin (bone slot, weight) pairs, S2 UV (the diffuse UV of every model, S108 BAN3), S3 tangent.
struct Vertex {
  float pos[3];
  float normal[3];
  float uv0[2];       // the first texture coordinate set
  float uv1[2];       // static arena meshes: a copy of uv0 (S2) for the dev viewer's own light-map read; 0 otherwise. No game shader reads it (only particles map inUv2)
  float tangent[3];   // 0 when the mesh has none
  uint32_t color;     // RGBA8 as stored (0xFFFFFFFF when none)
  // skeleton NODE indices (the mesh's palette slot already mapped through its palette); 0 when unused. Session 99 (scene objects): 16
  // bits - Fandango's DLC stage set 2591 is a 659-node rig whose lamps hang from nodes past 255 (8 bits clamped them all to node 255).
  uint16_t bones[4];
  float weights[4];   // sum 1 for skinned vertices; {1,0,0,0} for rigid ones
};

// A DDS texture as stored (linear, little-endian): the viewer uploads `data` (all mips) in `format`.
enum class TexFormat : uint8_t { kUnknown, kBC1, kBC2, kBC3, kB4G4R4A4, kB8G8R8A8, kR8G8B8A8 };
struct Texture {
  std::string name;  // the name in its texture directory (e.g. "color", "rin_mat00")
  TexFormat format = TexFormat::kUnknown;
  uint32_t width = 0, height = 0, mips = 1;
  uint32_t faces = 1;         // S103 RFL: 6 = a cube map file (DDS caps2 0xFE00): `data` = face 0..5 (+X -X +Y -Y +Z -Z), each face's `mips` chain tightly packed; a 2D texture: 1
  std::vector<uint8_t> data;  // mip 0 first, tightly packed (a cube: face 0's chain comes first, so a reader that ignores `faces` still sees face 0 as a 2D texture)
  bool live = false;          // Session 98 (tron video): the viewer rewrites this texture every video frame (a stage slot)
  uint32_t game_levels = 0;   // S111 MIPLEVELS: the level count the game's loader builds this texture with (0 = not known: the file's own chain is uploaded). Set per directory (TexSet::AddDirLevels); renderer_scene\game_levels.h applies it
};

// What a mesh's material needs for a plain look: texture slots are indices into Model::textures, -1 = none.
// S103 WRK (switch wwe2k14_rdr_wrk, stream-free, default ON): live face wrinkles + id-map tint + the +616 pass mode. The loader reads it: OFF = the wrinkle normal map is not decoded or uploaded.
inline std::atomic<bool> g_wrk_live{true};   // BOTH-BUILDS: a LOOK, ON in both builds; the cvar exists only in the OFF build (start_settings.cpp), the stream build keeps the default true

struct Material {
  std::string shader;  // the material class name as stored ("yCh_skin", "yBG_Default", "yProp_BumpMap", ...)
  int diffuse = -1, normal = -1, lightmap = -1, mask = -1, specular = -1;
  // S103 RFL (docs\research\s103\RFF.md section A [C]): the reflection layer of the Reflect effects: sampCubeRefPrm = the cube texture (a file DDS, 6 faces; Texture::faces),
  // sampRefMapPrm = the 2D reflection-amount map (pixel slot 9), g_fReflectAlpha = PS c208.x. -1 / 0 = the record names none (the front end then binds the black cube and ReflectAlpha 0).
  int cube = -1, refmap = -1;
  std::string cube_name;   // the cube sampler's texture name as stored (kept when no texture directory holds it: census / the unfed-cube report)
  float reflect_alpha = 0.f;
  int layer[3] = {-1, -1, -1};  // S103 LAY: sampDiffuse1Prm / 2Prm / 3Prm = the UVScroll layers 2..4 (texture indices; the technique word 0/2/4/6 says how many are drawn; docs/formats/uvscroll_params.md "Layers")
  // S105 C2L (docs/research/s105/C2L.md 3b [C]): the yProp_PSM grain: sampNoisePrm = the noise picture (fetch slot 6, `mat_noize`), and the game's name entries (sub_82BAAEE8 0x82BADB60 / 0x82BADB98 /
  // 0x82BADD20: register 207, components 3 / 4 / 5 = x / y / z) pack g_iNoiseTileX / g_iNoiseTileY / g_fNoiseLvl into c207 = (tile x, tile y, level, -): `Noise` (PS) and `NoiseParam` (VS).
  int noise = -1;
  float noise_lvl = 0, noise_tile[2] = {0, 0};
  std::string diffuse_name;  // Session 97: the diffuse sampler's texture name as stored (kept when the name is in no texture directory: stage movies)
  // S111 CACMATCHTEX (belt_plate.h): the diffuse / normal / specular sampler's name-table index = the live model's texture KEY index ([model+260] + 8 t; the game's vslot +60 sub_82BD4F50 stores key t), -1 = none.
  // Kept whether or not a texture directory holds the name: a slot the file cannot fill (a nameplate, a created belt's plates) is bound to the live key at this index.
  int key_diffuse = -1, key_normal = -1, key_specular = -1;
  float ambient[4] = {1, 1, 1, 1};  // g_f4MatAmbCol when present
  float diffuse_color[4] = {1, 1, 1, 1};  // g_f4MatDifCol when present
  // Session 97 (look spec out/runtime/s97/look.md): the rest of the lighting recipe's per-material parameters.
  float light_limit[4] = {10, 10, 10, 1};  // g_fvLightLimit (cap on the summed light; the CTAB default)
  float spec_col[4] = {1, 1, 1, 1};        // g_f4SpecularCol (arena) / g_f4SpecCol (wrestler)
  float spec_lev = 0;                      // g_fSpecularLev / g_fSpecLev (0 = highlight off)
  float spec_pow = 300;                    // g_iSpecularPow / g_fSpecPow
  bool ych_table = false;                  // S111 BODYSPEC (material_floats.h BeginRecord): the record's effect goes through the game's yCh name table (lobe names g_fSpecLev / g_fSpecPow / g_f4SpecCol, no power = 1)
  // Session 98 (skin fix): the wrestler parameters the game packs into the skin draw's constants [C: value match, probe run 6816 /
  // ch161 JBOY]: c142 = (g_fSpecLev, g_fSpecPow, g_fSpecLvBase, g_fSpecPwBase); c204 = (g_f4Ref_COL.rgb, g_f4Ref_DOT);
  // c205.xy = (g_fFairy, g_fFairyEdge); c155 = g_f3SSSSinCol, c158 = g_f3SSSSinCol2 (the skin glow; only the yCh_skin family has it).
  float spec_lv_base = 0, spec_pw_base = 1;
  float ref_col[4] = {0, 0, 0, 1};
  float fairy = 0, fairy_edge = 1;
  // S103 B3F: the hair highlight [C: sub_82BAAEE8 0x82BAD42C-0x82BAD5A0]: c249 = (g_fPrimaryShift, g_fSecondShift, g_SpecExp1, g_SpecExp2), c250 = (g_f4SpeCol1.rgb, g_fHairTileU),
  // c251 = (g_f4SpeCol2.rgb, g_fHairTileV). hair_tile defaults (10, 1): seen live in the game's draws (run 29) when the file has no g_fHairTileU/V [U: the default's source, the CTAB].
  float hair_shift[2] = {0, 0}, hair_exp[2] = {0, 0}, hair_col1[3] = {0, 0, 0}, hair_col2[3] = {0, 0, 0}, hair_tile[2] = {10, 1};
  // S103 B3F: c174 = (g_fRimAtnDsMin, g_fRimAtnDsMax, g_fRimAtnRtMin, g_fRimAtnRtMax) [C: sub_82BAAEE8 0x82BAC0C8-0x82BAC170, ids 3..6 = x..w]; the defaults are the values ours hard-coded before
  // (unconfirmed for a file without them). g_fRimAdjSpcMas has no string reference in the exe (q str), so no constant is known to take it [U].
  float rim_atn[4] = {500.f, 2500.f, 1.f, 0.4f};
  // S111 MATFEED (docs\research\s111\MATFEED.md): PS c156 = (g_fHdLigLvl, g_fHdLigSt) [rows 0x8392A320 / 0x8392A340, lanes x / y]; c249 = g_f4MuscleParam (whole vec4); c250 = g_f4MuscleUV_x..w
  // (lanes x..w); hair c243 = (g_fSSLevel, g_fSSStart, g_fSSEnd) (lanes x..z), c244 = g_f4SSColor (whole vec4). Defaults = the values ours fed before (no blow-out, the muscle map off, no glow):
  // a material that names none gets no write in the game (the register keeps the last draw's value: a 360 habit, not reproduced).
  float hd_lig[2] = {0.f, 1.f};
  bool has_hd_lig_lvl = false;   // S111 MATFEED review: the material names g_fHdLigLvl; only such a part takes the body's live value (the walk looks up the part's OWN parameter names: sub_82C04DF0 0x82C07984)
  float muscle_prm[4] = {1.f, 0.f, 0.f, 0.f}, muscle_uv[4] = {0.f, 0.f, 1.f, 1.f};
  float ss[3] = {0.f, 0.f, 0.f}, ss_col[4] = {0.f, 0.f, 0.f, 0.f};
  // EYA: c247 = (g_fFurLevel, -, g_fDifAmbLevel, -) [C: sub_82BAAEE8 0x82BAD37C / 0x82BAD3B4, name entries id 3 and id 5, ids 3..6 = x..w as for c174]; y / w have no table entry [U].
  // yCh_Reflect (the eyeball) scales its sky / ground ambient by c247.z. [C: no default; the game writes c247.z only when the material names g_fDifAmbLevel (sub_82BDB460 0x82BDC898, sub_82C04DF0 0x82C05354), otherwise the register keeps the last draw's value; 0 is our choice].
  float fur_level = 0, dif_amb_level = 0;
  float sss_col[4] = {0, 0, 0, 1}, sss_col2[4] = {0, 0, 0, 1};
  bool has_sss = false;
  // S111 UNIFY5 (UNREADNAMES.md): PS c155.w = the material's g_fSSSSinLvl and PS c201.w = its g_f4TexBis_Dif (the diffuse LOD bias), fed by ResolveChar SSS / Level. Defaults = what ours fed before
  // (0 / -1; every file record naming them holds 1.0 with its g_f3SSSSinCol.w 1.0 / -1.0): a material naming neither gets no write in the game (the register keeps the last draw's value: a 360 habit).
  float sss_sin_lvl = 0.f, tex_bis_dif = -1.f;
  // Session 98 (wrestler fix 3): the gear shader's tiled detail (probe run 6816, gear draw 1405: c206 = (50, g_f4NoiseTBump, g_f4NoiseTDiff,
  // g_f4NoiseTSpec) = (50, .08, .08, .5), c208 = (0, ..TBumpF, ..TDiffF, ..TSpecF) = (0, .3, .3, .8), vertex c188 = (g_fNoiseTNear,
  // g_fNoiseTFar) = (250, 1000): the two sets are mixed by the clip depth). `detail` = g_mNoiseTilePrm (texture `noise2`, chCommon.pac).
  int detail = -1;
  int logo7 = -1, logo4 = -1;   // S105 A5c: the CAW logo pictures (fetch slots 7 / 4, bound by the model draw's logo callback; see the A5c note); -1 = none (black: the layer does nothing)
  int change_mask[4] = {-1, -1, -1, -1};   // S108 THR2 (change_mask.h): the Superstar Threads editor's Mask1..Mask4 (PACH entries 13..16, kept only for a `/m` load), bound at pixel slots 7, 4, 14, 2 on a change-colour draw; -1 = none (black: that mask does nothing)
  bool diffuse_t0 = false;      // S105 A5c: the game sets pixel bool 150 for this part (its diffuse sampler's texture index is 0): an FX_LOGO fragment then reads the layer at slot 7, else slot 4
  int wrinkle_normal = -1;   // S103 WRK: g_mNormalWklPrm (the face wrinkle normal map `wrinkles_n`, fetch slot 5; WRK.md 1a [C])
  float wrink_uv[4] = {0, 0, 1, 1};   // S103 WRK: g_f4WrinkUV_x/y/z/w = PS c230 (the wrinkle rectangle, a file value); (0,0,1,1) only when has_wrink_uv is false
  float idmap_col[4] = {0, 0, 0, 0};  // S103 WRK: g_f4IdMapCol = PS c145 (file value)
  bool has_wrink_uv = false, has_idmap_col = false;
  bool spec_pow_prm = false;   // S103 YCB: the record names g_mSpecPowPrm with a texture index > 0 (the game's part entry E+16 non-null: the per-material spec-power cube, technique_pick.h CharPass; the player's files name it on every yCh material but Aniso_trans / 4 cloth / gear 14 / 1191 [YCH.md])
  int sweat_bump = -1;   // S103 SWF: g_mSweatPrm (the wet skin's bump map, fetch slot 4; SWB S2-3: texture index 7 or 8 of the record)
  int muscle = -1;       // S111 MUSCLEMAP: g_mMusclePrm (the flex effects' muscle normal map, pixel slot 2: sub_82BAAEE8 0x82BACDEC; a texture of the character's own model, mf01 .. mf14); -1 = none (black)
  int hair_noise_map = -1, hair_shift_map = -1;   // S111 HAIRMAPS: g_mHairNoisePrm / g_mHairShiftPrm (the hair highlights' noise and per-strand shift maps, pixel slots 8 / 9: rows 0x8392ABC0 / 0x8392ABE0); -1 = none (grey)
  int env_cube = -1;     // S111 ENVCUBE: g_mEnvLPrm (the soft environment light cube, pixel slot 10: sub_82BAAEE8 0x82BACFC8; CUBEMAP_eyes / CUBEMAP, a 6-face DXT1 file cube of chCommon.pac); -1 = none (the black cube)
  float noise_near[3] = {0, 0, 0}, noise_far[3] = {0, 0, 0};  // bump (normal gain), diffuse gain, light (specular) gain
  float noise_dist[2] = {250, 1000};                          // g_fNoiseTNear / g_fNoiseTFar
  // S103 SWF (docs\systems\liquid_map.md; SWB S2-3 [C: sub_82BAAEE8 "ych" rows, scratch parse + CH324 / CH343 records, 49 of 49]): the skin's wet constants, read by name from the material record. They are fed only to a draw
  // bound to the wet variant (the dry draw keeps Param0.zw = 0 and no Blood / Sweat / BSSBldClr, byte for byte). Defaults = the values every file record carries.
  float bss_swt[2] = {0.4f, 0.15f};                           // g_fBSSSwtMax, g_fBSSSwtMin -> PS c205.zw (the gloss of wet skin: min = dry, max = fully wet)
  float sweat_prm[4] = {0.8f, 100.f, 24.f, 0.f};              // g_f4SweatSpeLv, SpePw, BpCnt, Level -> PS c210 (the level is the Swet component's runtime word: 0 in every capture)
  float blood_prm[2] = {3.0f, 200.f};                         // g_f2BloodSpecLv, g_f2BloodSpecPw -> PS c234.xy
  float bss_bld_clr[4] = {0.66f, 0.f, 0.f, 1.f};              // g_f4BSSBldClr -> PS c246 (what blood multiplies the skin colour by)
  bool has_wet = false;                                       // the record named at least one of the above (census only)
  bool has_lm_slot = false;                // the mesh carries a sampLightMapPrm slot (= the game's "LM" shader family),
                                           // even when no texture is set: then the stage-wide lightmap is used
  // S103 L13b: the sampLightMapPrm slot names one of the stage's own page textures (`fade_lightmap00` / `fade_lightmap01` / `lightmap00`): a stand-in for the runtime stage page,
  // not a light map of the mesh's own. [C arena 4: the 27 yBG_Default meshes with such a slot (slots 0-21 ...) all have record word +0x90 = 1, and run 28's match stream (frame 622,
  // draws 385 / 386 / 390 / 393 = slots 7 / 10 / 18 / 19) draws them `yBG_Default_vertex_lm_vertex` + `_base_fragment` with slots (0, 16): the vertex-LM class, not the LM pixel pair]
  bool lm_stage_page = false;
  bool alpha_blend = false;  // translucent classes (yCh_trans, yCh_Aniso_trans, ...)
  bool translucent_draw = false;   // S111 LAYERS review: a character part the game draws in its TRANSLUCENT node (assets\char_draw_layer.h Translucent; the name rule where no draw split is known). The caster passes key on it (renderer_scene\wrestler_caster.h), never on alpha_blend
  // S111 UNIFY5 review: the part is drawn by a CHARACTER's draw nodes (a stock wrestler's body or piece: jboy.h JboyOptions::char_part from LoadWrestlerPac, never LoadProp; every created superstar part:
  // guest_model_draw.h BuildModel). The caster passes key on it (renderer_scene\wrestler_caster.h Casts), never on the effect name: the game's caster walks the character nodes and tests only the
  // part tag (a created superstar's yDefault part casts like its yCh_skin parts).
  bool char_part = false;
  uint8_t cutout_blend = 0;  // S103 F: a cut-out drawn as blended by the loader: 1 = a prop slot's cut-out texture (jboy.h: keeps depth write in Game order), 2 = an a / s slot (asset_load.h: stays far-to-near in Game order)
  bool alpha_test = false;   // cut-outs (hair cards, crowd cards)
  float diffuse_alpha_clear = 0.f;  // S103 B2: share (0..1) of the diffuse texture's top-mip texels with alpha < 255 (jboy.h AlphaClearShare); set for arena / prop-slot materials, 0 = alpha-free
  uint8_t texel_cutout = 0;   // S103 TQF: for the yProp_BumpMap / Default / Reflect class the old texel rule's verdict is KEPT here instead of in alpha_test / alpha_blend: bit 0 = the arena / prop-entry rule (was alpha_test), bit 1 = the prop-slot rule (was alpha_blend + cutout_blend = 1). The front end uses it only when the effect table is missing (GameTestsAlpha) and to keep a prop slot's sorted cut-out when the picked variant shows the diffuse alpha (AddModelImpl); the viewer reads it as the old flags
  bool vertex_alpha_lt255 = false;   // S103 TQF: an arena / prop-entry mesh with a vertex colour alpha < 255 on any vertex (the file's S0 colour byte 0; Wheel_Chair_b 65-210, 2753_01 176-255): the game's yProp draw blends at the vertex alpha (fx_prop.glsl), so such a part is tested + blended (front_end.cpp GameTestsAlpha) [L: PAS.md]
  bool alpha_mask = false;   // S103 P1: an arena material whose diffuse alpha is a HIGHLIGHT MASK, not coverage: the game draws it with the FX_ALPHA_SPEC / FX_ALPHA_GLOSS pixel shader and no discard (the barricade padding ar_fence / ar_fence_b, ar_sak0, ar_dm). Rule [C for arena 4: 265 of 265 meshes the stream shows; L elsewhere]: yBG_Default / yProp_Default, no specular or normal map, BC2 / BC3 diffuse with < 2 % exactly-zero alpha texels and a mean alpha >= 0.10 (jboy.h AlphaStats; docs\formats\arena_slot_flags.md)
  bool no_highlight = false;  // S103 L5: an arena yBG_Default / yProp_Default material the game draws with the plain `*_base` / `*_lightmap` pixel shader whatever its g_fSpecularLev (stairs, tier walls, exit frame: level 0.5, DXT1, no specular map): no highlight lobe. [C arena 4: 243 of 244 meshes the stream shows; the one exception ar_sak2 is [U]; L elsewhere] (jboy.h; docs\formats\arena_slot_flags.md)
  float spe_lod[2] = {0.f, 1e30f};   // S103 D1: g_fSpeLodNear / g_fSpeLodFar (the FX_SPEC_FADE highlight fade range, VS SpeLodParam.xy); absent = no fade [U: the effect's own default]
  uint32_t technique = 0;    // S103 D1: the mesh record word +0x90 = the technique index the colour pass hands the effect (sub_82C04DF0 0x82C05AF8; PLAN.md skeleton R0)
  bool vertex_lm = false;    // S103 L10: an arena yBG_* material whose mesh record word +0x90 has bit 0 set and bit 3 clear: the game draws it with the `*_vertex_lm_vertex` program (the stage light-map page sampled in the VERTEX stage at slot 16; its pixel shader still reads the page at slot 13). [C arena 4: 264 of 264 meshes the stream shows (yProp_* excluded by effect); L elsewhere] (jboy.h; docs/formats/arena_lightmap.md "S103 L10")
  bool cull_back = false;   // the game draws this material with back faces culled. Stage slots 0..399 of an arena: the mesh record's word +0x8C bit 0x2 (SCU, jboy.h; sub_82C04DF0 0x82C0A880);
                            // the movie screens 800-809 (Session 98, capture 6818; rf_c_core.cpp / stage_media.h) and the crowd (live_crowd.cpp) keep their own rules
  bool cull_from_record = false;   // SCU / S105 C3: cull_back came from the mesh record's bit 0x2 (EVERY JBOY family now, jboy.h; the front end's switches turn exactly these off: wwe2k14_rdr_stage_cull for a stage slot, wwe2k14_rdr_family_cull for the rest; the viewer ignores it)
  bool stage_slot = false;         // S105 C3: an arena stage slot 0..399 (kind 1): only these meshes' mirrored `r` copy takes the swapped front face (sub_82C04DF0 0x82C0B920-0x82C0B968; a non-arena object with a mirrored world gets no swap, CUL_ADV D)
  uint32_t cull_word = 0;          // SCU: the mesh record's word +0x8C as stored (values 4 / 6 / 0x114 / 0x116 in the arenas; d2 dump + census only)
  bool blend_inline = false;  // Session 98 (spot / beard pass): alpha-blended in the opaque pass's order with depth write on (the stage movie
                              // surfaces: capture 6818 draws them SrcAlpha / InvSrcAlpha with depth write, before the later layers)
  bool additive = false;     // S97: drawn added on top (arena slots flagged `a` in entry 50001: glows, beams, fog)
  // S105 SZW (docs/research/harness/SZW_contract.md): the game's per-draw state of a stage slot, by its entry-50001 tokens (the slot record byte 8: `a` 0x40, `s` 0x20, `z` 0x08; arena_slot_flags.md).
  // bit 1 = an `s` / `a` slot (group 1 / 2: blended, alpha-tested GEQUAL 1/255, WRITES depth unless `z`), bit 2 = the `z` token (z write off for the slot's draw; alone it draws in the opaque group, blended,
  // tested, no depth write), bit 4 = a movie-key piece (800-809) with a vertex alpha below 255 (blend_inline, depth write: the test must discard what it hides, A54). Read by FrontEnd::Record behind the
  // switch wwe2k14_rdr_stage_zwrite; the loader's own alpha_test / alpha_blend / cutout_blend fields are unchanged (switch off = today's draw).
  uint8_t stage_zw = 0;
  // S103 E6: the UV-scroll speeds of the scrolling classes (yBG_UVScroll / yProp_UVScroll and their LM twins), stored in the material record as eight scalars
  // g_fScrollU, g_fScrollV, g_fScrollU2, g_fScrollV2, ..U3, ..V3, ..U4, ..V4 (layer 0..3); UVScroll0 = (U, V, U2, V2), UVScroll1 = (U3, V3, U4, V4) [hints in
  // docs/renderer/CONSTANTS.md]. scroll_have = at least one was found in the record (else the speeds stay 0 and the consumer counts the absence).
  float scroll[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  bool scroll_have = false;
  float sl_scale = 0.f;   // S103 TQ2 (UVS.md): g_fSL_Scale = VS SL_Scale.x of the SearchLight (planar-uv) techniques 8..11 (0.004 on every searchlight read)
  bool crowd = false;        // S97: an audience member (the game's crowd vertex shader lights with the UNNORMALISED world normal: its
                             // length is the member's scale, 0.84-1.01, so the diffuse sum and the hemisphere blend scale with it)
};

struct Mesh {
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;  // TRIANGLE LIST (the file's strips are converted; degenerate triangles dropped)
  int material = -1;              // index into Model::materials
  std::string part;               // which JBOY / slot it came from (e.g. "body", "hair", "slot 12"), for the census report
  int placement = -1;             // Session 97: index into Model::placements; -1 = drawn once as stored
  bool skinned = false;           // Session 97: vertices follow Model::skin (wrestler pieces); false = drawn as stored
  int rigid_node = -1;            // S103 C4: an ARENA mesh (the ring set): the skeleton node its palette slot 0 names (palette[0] - 1, the raw node number: arena models keep no merged skeleton); -1 = none. A mesh with no skin stream is drawn rigid on it
  std::vector<uint16_t> palette_nodes;  // S103 C4: slot k of the mesh's palette -> node (palette[k] - 1), the file's own map (sub_82C04DF0 0x82C0A038-0x82C0A04C reads matrix palette[k] - 1 of the call's array); arena meshes only
  int light_set = -1;             // Session 97: index into Model::light_sets (arenas); -1 = the viewer's default rig
  // Session 98 (step 16, click to inspect): where the mesh came from, for the viewer's INSPECT panel and the friendly names.
  std::string node;     // the first node name of the piece's model ("ar_fence01_a"); empty when the piece has none
  std::string palette;  // the skin-palette bone names joined by ';' (wrestler pieces)
  int src_kind = 0;     // 1 arena (stage slot, ring set, stage screen), 2 wrestler piece, 3 crowd body, 4 prop (bgEtc leaf)
  std::string src_pac;  // "bg05.pac" / "ch107.pac" / "bgEtc.pac"
  int src_key = 0;      // the pac entry: slot / key; wrestler entry id (0 body, 10000+ hair ...); 20000+N crowd body; prop key
  // S101 lane 40 (our arena): the file's own record for this mesh, for the join with a stream draw. src_record = the mesh record's index in its JBOY (the game's draw part; a
  // record the loader skipped leaves a gap), strip_indices = the sum of the index groups' counts as stored (triangle strips), index_groups = how many groups.
  int src_record = -1;
  uint32_t strip_indices = 0, index_groups = 0;
  // S101 lane 49 (the arena's bone-id props): arena meshes read with skin streams (S1 = pairs of LE u32 palette slot + BE f32 weight) keep the file's raw pairs, 4 per vertex (slot 255 =
  // none), and how many pairs the stream stores per vertex. Empty for every other mesh.
  std::vector<uint8_t> src_skin_slots;
  std::vector<float> src_skin_weights;
  uint32_t src_skin_pairs = 0;
};

// Session 97: one arena light set (pac entry 30002/N): four directions TO the light, four colours, sky / ground ambient.
// Layout (784-byte kind): dirs vec4 at +0x10, colours at +0x50, sky at +0x100, ground at +0x110 (out/runtime/s97/look.md
// section 5). The 896-byte kind: dirs +0x10, colours +0x60, no ambient read (unconfirmed where it sits: left 0).
struct LightSet {
  int id = 0;  // the set number as stored (the `l(N)` of entry 50001)
  int edit_id = -2;  // Session 98 (lighting editor): the id whose edits apply (-2 = id); -1 = a fallback copy (the mesh's own set was missing / black): never edited
  float dir[4][3] = {};
  float col[4][3] = {};
  float sky[3] = {};
  float ground[3] = {};
  // Session 98 (skin fix; probe run 6816, out\s98\skin_fix\_model.py): the w of each stored colour = that light's weight in the wrestlers' entrance
  // specular lobes (c0..c3.w of the skin draw; arena 5 set 10 stores 0.6275 / 0.3529 / 1.451 / 0.2745 = probe run 6816 exactly).
  float wt[4] = {};
  // The wrestlers' highlight light (the skin draw's c4 colour / c12 direction): run 6816's entrant takes set 10's light 4 here.
  // It is NOT a diffuse light (the ambient cube is built from lights 0-3 only); it only feeds the edge highlight.
  float hl_dir[3] = {};
  float hl_col[3] = {};
  bool hl_view = false;   // S103 L7: hl_dir is the record's VIEW-space key light (camera-attached: the character path rotates it by the frame camera); false = already a world / file-space direction
  // Session 98 (replay S5): the colours the ambient cube is built from = the previous frame's (the game's yPreLight pass is fed the
  // last frame's c0..c3; renderer.cpp plCol). Set by the entrance rig; without it the cube uses `col`.
  bool has_pcol = false;
  float pcol[4][3] = {};
  // Session 98 (match oracle, out\s98\match_oracle\notes.md): each light's cube falloff = the yPreLight pass's c16..c19.x (side_prelight.frag:
  // keep_k = 1 - share x (1 - fall_k)). Stored at +0x140 of the 896-byte kind (arenas 0 / 5: sets 10 / 15 / 16 / 200 / 201 hold (0, 0, 0.5, 0),
  // the others 0) [C: probe run 6816's match prelight draw (frame 15841) c16..c19.x = (0, 0, 0.5, 0) = set 10's +0x140; run 071's match
  // cube (capture match_a) fits the PreLight of set 10 with it to rms 0.010 (ratio 0.992) vs 0.045 (ratio 0.955) without]. The entrance rig's
  // cubes are built with 0 [C: every entrance prelight draw of runs 6816 / 6824 / 071], so an evaluated rig set clears it.
  float fall[4] = {};
  // S101 lane33: the per-light "camera-attached" flag of the 784-byte kind (u32 BE at +0x120 + 4 i): the game evaluates a flagged light's
  // stored / keyed direction as view space (world = V^T d) in the MATCH too, the others stay world-fixed [C: captures wm30 (arena 29,
  // sets 30 / 31 / 32) and the Triple H / Brock match (arena 15): per light the game's c8..c11 = V^T d for the flagged lights (0.0 deg) and
  // = d for the others, set 31 flags (1, 0, 0, 1), set 32 (1, 0, 0, 0), set 30 none; docs\renderer\LIGHT_MATCH.md]. The 896 kind: [U] the flag
  // is probably at +0x184 + 4 i (set 10 (1,1,1,1) there, matching 'all five lights camera-attached'; sets 15 / 16 / 135 / 200 / 201 (1,1,1,1); 50 / 100 /
  // 124 / 500 none; not wired, false here). Arenas 78 / 80 store set 10 as a 784 record with (0,1,1,1): conflicts with the wrestler rule [U].
  bool follow[4] = {};
  // S103 RL: the scene front end turns the follow[] lights of THIS set by the frame camera when it binds DifLghVec (world = V^T d, the same axes as
  // char_effects.cpp KeyLightWorld). Set only by rf_c_core.cpp LightsFor (the record's sets, match look); false everywhere else, so the viewer (which turns
  // its own copies in ArenaLightsFollowCamera) is never turned twice.
  bool follow_view = false;
};
// Our default rig for models with no light set of their own (the wrestlers): the numbers of arena 5 set 121 (the
// commonest set of the arenas), read with out/runtime/s97/lightset_dump.py. Numbers only.
inline LightSet DefaultRig() {
  LightSet s;
  s.id = 121;
  const float d[4][3] = {{-0.02149f, 0.6153f, -0.788f}, {-0.7456f, 0.5618f, 0.3584f}, {0.6938f, 0.6247f, 0.3584f}, {-0.03294f, 0.6285f, -0.7771f}};
  for (int i = 0; i < 4; ++i) {
    for (int k = 0; k < 3; ++k) s.dir[i][k] = d[i][k];
    s.col[i][0] = s.col[i][1] = 0.451f, s.col[i][2] = 0.4784f;
  }
  s.sky[0] = s.sky[1] = s.sky[2] = s.ground[0] = s.ground[1] = s.ground[2] = 0.1961f;
  return s;
}

// Session 97: a 4x4 matrix, row-major, ROW vectors (p' = p * M, translation in elements 12..14): the game's own CPU
// convention (src\nf\palette_transpose.h, src\native_skin.h).
using Matrix = std::array<float, 16>;
inline Matrix Identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }
inline Matrix Mul(const Matrix& a, const Matrix& b) {  // a then b (row vectors: p * a * b)
  Matrix r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
  return r;
}

// Local transform from a rotation quaternion (x y z w, column-vector meaning) and a translation, in row-vector form.
inline Matrix FromQuatT(const float q[4], const float t[3]) {
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  // column-form rotation R; row form stores R^T (row i = column i of R)
  const float r00 = 1 - 2 * (y * y + z * z), r01 = 2 * (x * y - z * w), r02 = 2 * (x * z + y * w);
  const float r10 = 2 * (x * y + z * w), r11 = 1 - 2 * (x * x + z * z), r12 = 2 * (y * z - x * w);
  const float r20 = 2 * (x * z - y * w), r21 = 2 * (y * z + x * w), r22 = 1 - 2 * (x * x + y * y);
  return {r00, r10, r20, 0, r01, r11, r21, 0, r02, r12, r22, 0, t[0], t[1], t[2], 1};
}
// Quaternion of R = Rz(z) * Ry(y) * Rx(x) (column vectors, right-handed, radians): the node record's euler angles
// (Session 97 skeleton reader: this order beat the other 23 variants on Kane, 4.2 vs 53.1 mean joint distance).
inline void EulerXYZToQuat(const float e[3], float q[4]) {
  const float cx = std::cos(e[0] * 0.5f), sx = std::sin(e[0] * 0.5f), cy = std::cos(e[1] * 0.5f), sy = std::sin(e[1] * 0.5f),
              cz = std::cos(e[2] * 0.5f), sz = std::sin(e[2] * 0.5f);
  // qz * qy * qx
  q[3] = cz * cy * cx + sz * sy * sx;
  q[0] = cz * cy * sx - sz * sy * cx;
  q[1] = cz * sy * cx + sz * cy * sx;
  q[2] = sz * cy * cx - cz * sy * sx;
}
// Inverse of a rigid (rotation + translation) row-vector matrix.
inline Matrix InverseRigid(const Matrix& m) {
  Matrix r = Identity();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r[i * 4 + j] = m[j * 4 + i];
  for (int j = 0; j < 3; ++j) r[12 + j] = -(m[12] * r[j] + m[13] * r[4 + j] + m[14] * r[8 + j]);
  return r;
}
// The files are -Y up with the other handedness; the viewer negates Y (S = diag(1,-1,1)). A file-space transform M
// becomes S * M * S in viewer space: flip the signs of the elements that mix Y with X / Z.
inline Matrix MirrorY(Matrix m) {
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      if ((i == 1) != (j == 1)) m[i * 4 + j] = -m[i * 4 + j];
  return m;
}

struct Bone {
  std::string name;
  int parent = -1;
  // FILE space (Session 97: no longer Y-negated), parent-relative, as in the node record.
  float local_translation[3] = {0, 0, 0};
  float local_rotation[4] = {0, 0, 0, 1};  // quaternion x y z w from the record's euler angles
  Matrix bind_world = Identity();          // the bind pose in model space, VIEWER space (MirrorY applied)
  Matrix inv_bind = Identity();            // its inverse, viewer space
  // S103 W2B: a node a wrestler PIECE added to the body's skeleton: the piece's pac entry id and the node's index in that piece's own node table (-1 for the body's own nodes). The game's SUBM component forces the
  // map word -1 for `kami_b*` / `spring*` piece nodes (sub_8259E720) and the spring-hair simulation writes their rows every frame; the live record carries those rows tagged with exactly this (id, node) pair (docs\agents\s103\W2.md).
  int piece_id = -1;
  int piece_node = -1;
  uint32_t piece_nodes = 0;              // S103 W2B (review G): the node count of the piece entry this bone came from (0 for the body)
  bool has_corr = false;                 // S103 W2B (review F): the piece's bind chain differs from the body's here (corr non-identity): the game's row P_j must be premultiplied by corr_inv = inverse(corr_j) (apply corr_inv first, then the row)
  Matrix corr_inv = Identity();
};

// Session 97: extra copies of some meshes (the crowd's people, repeated ring pieces): each mesh whose `placement` names
// an entry here is drawn once per transform (model space -> model space), instead of once as stored.
struct Placement {
  std::string label;  // e.g. "crowd body 20003", "ring post"
  std::vector<Matrix> transforms;
};

// S103 V1: the visibility box of one stage slot (pac entry 50020, docs\formats\stage_visibility.md), in the VIEWER's space; only slots whose tree node was confirmed by the slot's own meshes.
struct StageBox {
  int slot = -1;
  bool twin = false;   // the slot is also drawn mirrored (flag r): its mirrored copy has its own test
  float lo[3] = {}, hi[3] = {};
};

// A loaded wrestler (all JBOY pieces of one attire) or an arena (all stage slots of one bg pac).
struct Model {
  std::string label;  // "wrestler 107 attire 0", "arena 5"
  std::vector<Mesh> meshes;
  std::vector<Material> materials;
  std::vector<Texture> textures;
  std::vector<Bone> bones;
  uint32_t main_bones = 0;   // S103 W2B: the body entry's own node count (bones[main_bones..] were added by pieces: Bone::piece_id / piece_node); 0 = not a wrestler model
  std::vector<Placement> placements;
  // Session 97: the skinning palette, one matrix per bone (pose_world * inv_bind); empty = every bone identity (the
  // stored bind pose). The viewer can replace it per frame (motions).
  std::vector<Matrix> skin;
  // Session 97: the light sets the meshes use (Mesh::light_set indexes this) and the stage-wide top-down lightmap
  // (arena texture `fade_lightmap00`; index into textures, -1 = none).
  std::vector<LightSet> light_sets;
  int stage_lightmap = -1;
  // Session 97: g_f4LightMapParam of the stage: xy = top-down scale of the stage lightmap (uv = world.xz * xy * 0.5 + 0.5),
  // gain = c184.z. The CTAB default is (1/3500, 1/3500, 1); the live value of arena 5 is (1/5600, 1/5750, 2.5).
  // The loader default is what a NON-arena model keeps (m2.pac weapons, evtobj.pac props). [C, S111 LMPROPS, technique_census --lmprops]: no draw of such a model reads c184 today: the 10 meshes of m2 ids 29 / 31 / 32 / 34
  // that name sampLightMapPrm have no light-map texture of their own, so the front end gives them the plain variant (BindMaterial: w.lightmap needs an image; a prop has no stage page), and the loader sees no other m2 / evtobj
  // mesh that names it (the rest of the names a raw scan finds are not light-map slots to our loader [L: skin-stream meshes, material_samplers.h]). The game's own pick for these draws is unread [U]: when one is found to draw light-mapped, it must take the arena's value
  // (arena_lm::Compute / Held, the LMPREV rule), never this default (the census check then fails: technique_census --lmprops --plant-no-page shows the draws it would be).
  float lm_scale[2] = {1.f / 3500.f, 1.f / 3500.f};
  float lm_gain = 2.5f;
  // S103 B3: lm_scale / lm_gain = c184 of the arena, from the game's own table at load (arena_lm.h, T3b): x y = 1 / span, z = the gain. lm_card: a card-technique model (the crowd card quad):
  // the game draws it with z x the card factor ([0x8376383C] * 0.001, applied only when > 0; the front end reads it from the record row kSlCardFactor).
  bool lm_card = false;
  float bounds_min[3] = {0, 0, 0}, bounds_max[3] = {0, 0, 0};
  std::string custom_key;   // S103 CAR: a created arena (ids 200-249): the part selection (car::Sel::Key) this model was built from; empty for every other arena
  bool cycle_groups = false;  // S103 V1: the arena has slot-cycle entries (60010-60025); the pick is the game's tick, read at the latch (S103 CAR, stage_slot_vis.h CycleSkips)
  std::vector<std::vector<int>> cycle_slot_groups;  // S103 CAR: per group (key order) its slot list as the text gives it; the loader-only fallback shows the first slot, hides the rest
  std::vector<StageBox> stage_boxes;  // S103 V1: arenas only (assets::stage_tree::Fill), sorted by slot; a slot without a box is never tested
  // S103 D1 (the read rule, tools\harvest\choice_predict.py rule_slot_record): per stage slot, bytes +6 / +7 / +8 / +9 / +10 of the game's 12-byte slot record as the parser
  // sub_824E8628 builds them from the slot's entry-50001 tokens [R7 byte +10 bits / R8 slot record / R10 class3 p tokens]. Arenas only; a slot with no row is absent (all zero).
  struct SlotRecord { uint8_t b6 = 0, b7 = 0, b8 = 0, b9 = 0, b10 = 0; bool cycled = false; };  // cycled (S103 UVF): a slot of a 60010-60025 cycle group (not a record byte)
  std::vector<std::pair<int, SlotRecord>> slot_records;   // sorted by slot
  int arena = -1;   // S103 D1: the arena id of an arena model (LoadArena; sub-models copy it): [stage+1216] for the read rule (R10 class4 [L])
  // Anything odd the loader saw (a missing texture name, a stream that did not tile, NaN positions, ...). The census
  // reports these per item; an empty list means the loader found nothing wrong.
  std::vector<std::string> warnings;
  // S108 AL1: what the loader could NOT give of what the game draws (each also a warning): `kind` = "model_part" (a part / slot / piece / crowd body whose JBOY failed to load: dropped) or
  // "texture_pac" (a pac holding this model's shared textures did not open: every name it holds stays unbound); `part` = the thing a person looks for; `why` = the loader's reason.
  // Read once per adopted model by the live renderer (renderer_scene\model_load_report.h) and said through rdr::DrawShortfall. Written only through NoteDropped below.
  struct Shortfall { const char* kind; std::string part, why; };
  std::vector<Shortfall> shortfalls;
  // Time spent reading + decoding (ms), for the census.
  double load_ms = 0;
};

// S108 AL1: the loader's one way to say it dropped something the game draws: the warning (text unchanged: "<part> failed: <why>" for a part) and the shortfall the renderer reports.
inline void NoteDropped(Model& m, const std::string& part, const std::string& why, const char* kind = "model_part") {
  m.warnings.push_back(std::string(kind) == "model_part" ? part + " failed: " + why : part + ": " + why);
  m.shortfalls.push_back({kind, part, why});
}

}  // namespace assets
