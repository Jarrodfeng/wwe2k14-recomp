// S108 AL1 (docs\research\s108\AL1_plan.md row 5): what the front end refuses to upload, as pure functions, and its report through the shared draw_shortfall (kinds texture_upload / mesh_upload).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no renderer: FrontEnd::MakeTexture / AddModelImpl (front_end.cpp) and the offline unit (tools\weapons\al1_sites_unit.cpp) run THESE.
// A refused texture binds white (or the black stand-in), a refused mesh is not drawn. Every file texture is DXT1 / DXT3 / DXT5 / A4R4G4B4 / A8R8G8B8, 2D or cube, all accepted by ParseDds
// (docs\formats\drawables_atlas.md, 57873 textures): a refusal is bad data or a run-time texture we built wrong, never the game's choice.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

#include "../assets/asset_model.h"
#include "../renderer/problem.h"

namespace rdr_scene::upload {

inline size_t UploadMipBytes(assets::TexFormat f, uint32_t w, uint32_t h) {
  switch (f) {
    case assets::TexFormat::kBC1: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 8;
    case assets::TexFormat::kBC2:
    case assets::TexFormat::kBC3: return size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * 16;
    case assets::TexFormat::kB4G4R4A4: return size_t(w) * h * 2;
    default: return size_t(w) * h * 4;
  }
}

// What MakeTexture uploads of `t`: the mips whose data is whole (`fit`), the bytes of one face's chain (`total`), a cube (six faces).
struct TexturePlan {
  uint32_t mips = 0, fit = 0;
  size_t total = 0;
  bool cube = false;
};
// nullptr = uploadable (`p` filled); else MakeTexture's refusal, in its order (the RFL switch-off cube is the caller's own silent case, before this).
inline const char* TextureRefusal(const assets::Texture& t, TexturePlan& p) {
  if (t.width == 0 || t.height == 0 || t.format == assets::TexFormat::kUnknown) return "empty or unknown format";
  const bool bc = t.format == assets::TexFormat::kBC1 || t.format == assets::TexFormat::kBC2 || t.format == assets::TexFormat::kBC3;
  if (bc && ((t.width % 4) || (t.height % 4))) return "block-compressed size not a multiple of 4";
  if (t.width > 16384 || t.height > 16384) return "too large";
  uint32_t mips = std::max(1u, t.mips), mx = std::max(t.width, t.height), maxmips = 1;
  while (mx > 1) mx >>= 1, ++maxmips;
  p.mips = std::min(mips, maxmips);
  p.total = 0, p.fit = 0;
  for (uint32_t i = 0; i < p.mips; ++i) {
    const size_t sz = UploadMipBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
    if (p.total + sz > t.data.size()) break;
    p.total += sz, ++p.fit;
  }
  if (p.fit == 0) return "data shorter than mip 0";
  p.cube = t.faces == 6 && t.width == t.height;   // S103 RFL: a cube file (assets::Texture::faces): six faces, each `total` bytes (its whole chain)
  if (p.cube && (p.fit != p.mips || t.data.size() < 6 * p.total)) return "cube data shorter than six whole faces";
  return nullptr;
}

// The report: `kind` texture_upload / mesh_upload, `label` = the model's label, `what` = "texture 'name'" / "mesh 'part'". Once per (kind, label + what) a run.
inline void ReportUploadRefusal(const char* kind, const std::string& label, const std::string& what, const std::string& why) {
  const std::string object = (label.empty() ? std::string("(unlabelled model)") : label) + ": " + what;
  rdr::DrawShortfall(kind, object.c_str(), 1, 0, "%s", why.c_str());
}

}  // namespace rdr_scene::upload
