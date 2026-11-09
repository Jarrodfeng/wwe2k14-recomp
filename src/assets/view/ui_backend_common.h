// Session 98 (phone port): what every ui::Backend does the same way - the shown models and their textures for the Assets tab.
#pragma once
#include <vector>

#include "renderer.h"
#include "ui_backend.h"

namespace view {
namespace ui {

inline const char* TexFormatName(assets::TexFormat f) {
  switch (f) {
    case assets::TexFormat::kBC1: return "BC1 (DXT1)";
    case assets::TexFormat::kBC2: return "BC2 (DXT3)";
    case assets::TexFormat::kBC3: return "BC3 (DXT5)";
    case assets::TexFormat::kB4G4R4A4: return "B4G4R4A4";
    case assets::TexFormat::kB8G8R8A8: return "B8G8R8A8";
    case assets::TexFormat::kR8G8B8A8: return "R8G8B8A8";
    default: return "unknown";
  }
}

inline void ListRendererModels(Renderer& r, std::vector<ModelMeta>& out) {
  out.clear();
  const auto& models = r.Models();
  for (size_t mi = 0; mi < models.size(); mi++) {
    const GpuModel& m = models[mi];
    ModelMeta mm;
    mm.label = m.label;
    for (size_t i = 0; i < m.tex_info.size(); i++) {
      TexMeta t;
      t.name = m.tex_info[i].name;
      t.width = m.tex_info[i].width, t.height = m.tex_info[i].height, t.mips = m.tex_info[i].mips;
      t.format = TexFormatName(m.tex_info[i].format);
      t.id = (ImTextureID)r.UiTexture(mi, i);
      mm.textures.push_back(std::move(t));
    }
    out.push_back(std::move(mm));
  }
}

}  // namespace ui
}  // namespace view
