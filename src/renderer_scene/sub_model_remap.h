// S105 C2LA F1: the texture-index remap a sub-model's material takes (rf_c_core.cpp SubModel). One function, so a Material texture field added later has ONE place to join
// (the ring mat's noise picture was missed once: the full model's index was read against the ~12-image sub-model). Header-only; the unit is tools\scene\submodel_remap_unit.cpp.
#pragma once
#include "../assets/asset_model.h"

namespace rdr_scene {

// `tex` maps a full-model texture index to the sub-model's (and adds the image); `skip_noise` is the unit's planted twin (the pre-fix gap).
template <class TexFn>
void RemapMaterialTextures(assets::Material& mt, const TexFn& tex, bool skip_noise = false) {
  mt.diffuse = tex(mt.diffuse), mt.normal = tex(mt.normal), mt.lightmap = tex(mt.lightmap), mt.mask = tex(mt.mask), mt.specular = tex(mt.specular), mt.detail = tex(mt.detail);
  if (!skip_noise) mt.noise = tex(mt.noise);
  for (int& ly : mt.layer) ly = tex(ly);   // S103 LAY: the UVScroll layer textures follow the same remap
}

}  // namespace rdr_scene
