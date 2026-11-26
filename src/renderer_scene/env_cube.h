// S111 ENVCUBE: the soft environment light cube at pixel sampler 10 of the yCh_Reflect / yCh_trans / yCh_TransTiling fragments (g_mEnvLPrm = ych sampler row
// 0x8392AC20, name-table site sub_82BAAEE8 0x82BACFC8). docs\research\s111\ENVCUBE.md.
// The game binds slot 10 from the part's own material sampler: the file names `CUBEMAP_eyes` / `CUBEMAP`, a DXT1 128x128 cube (8 mips, 6 faces) of chCommon.pac
// (PACH 0 entry 21), the shared wrestler directory every wrestler TexSet already falls back to (asset_load.h LoadWrestlerPac). Material::env_cube (jboy.h) holds
// its Model::textures index; MakeTexture uploads it as a 6-layer cube image (BC1; DDS face order = Vulkan's). Ours bound the black cube there, so the FX_ENV_LIGHT
// term (fx_trans_ps.glsl `cubes[10]`) added nothing. One rule, used by FrontEnd::BindMaterial's CharSlot::EnvCube case (RoleOfSlot's role of slot 10): the
// material's cube when it is a six-face image that uploaded, else the black cube (a material naming none; reflections off: MakeTexture skips every cube).
// Header-only; the unit is tools\scene\envcube_unit.cpp.
#pragma once
#include <cstddef>

namespace rdr_scene::env_cube {

inline constexpr int kSlot = 10;   // g_mEnvLPrm: pixel sampler register 10 (sub_82BAAEE8 0x82BACFCC)

// The cube image of Model::textures index `i`: `images[i]` when `info[i].faces == 6` (a cube file that uploaded as a cube), else `none`. A 2D image never
// reaches the cube slot; an image that did not upload is `none` already. `images` / `info` = FrontEnd::ModelData::images / img_info (any vectors of the same shape).
template <class ImageVec, class InfoVec>
typename ImageVec::value_type CubeAt(const ImageVec& images, const InfoVec& info, int i, typename ImageVec::value_type none) {
  if (i < 0 || size_t(i) >= images.size() || size_t(i) >= info.size() || info[size_t(i)].faces != 6) return none;
  return images[size_t(i)];
}

}  // namespace rdr_scene::env_cube
