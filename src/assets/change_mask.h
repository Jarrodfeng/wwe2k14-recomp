// S108 lane THR2 (docs\research\s108\THR_plan.md section 6 A3; src\scene_live\change_colour.h): the Superstar Threads editor's four masks as model textures.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, no game or SDK includes (the loader, the contributor and the offline unit compile it).
//
// The game [C, listings; our words]: the editor's draw callback (sub_8339F0E0) binds the attire slot descriptor's textures named Mask1..Mask4 at pixel samplers 7, 4, 14, 2
// for every draw of the edited body; the same four pictures the build step composes from (attire_compose.h: PACH entries 13..16, k = entry - 13, a 16-bit DDS whose header
// names the R, G, B, A bit masks). The shader takes the first non-zero channel of mask 1 r, g, b, a, mask 2 ... and replaces the colour with ChangeColor[i] x that value.
// Ours: a body the record names with the `/m` entry tag (wrestler_contrib.cpp Collect, only while the game recolours it) is loaded with those four entries kept as model
// textures, and every material's change_mask[k] names texture k (the callback binds the same four for every draw of the model). A masked load caches apart from the plain one
// (the tag is part of the entry string = the model cache key), so a match never pays for masks. A PACH with no mask entries keeps none: the body is drawn without change colour
// (the game's own rule: sub_8339F5D0 clears its switch word [E+80] when the descriptor has no Mask1).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "asset_model.h"
#include "attire_compose.h"   // FindDds / detail::View: the one DDS header reader of the masks

namespace change_mask {

constexpr int kMasks = attire_compose::kMasks;                       // 4: Mask1..Mask4
constexpr uint32_t kFirstEntry = attire_compose::kFirstMaskEntry;    // 13

// The entry tag. Placed right after "model_key:<digits>" so the composite tag ("/c:<hex>") and the refusal tag ("/r", a SUFFIX test) keep their places:
// "model_key:2401/m/c:...". sscanf("model_key:%d") still reads the key.
inline bool HasTag(const std::string& entry) {
  for (size_t p = entry.find("/m"); p != std::string::npos; p = entry.find("/m", p + 1))
    if (p + 2 == entry.size() || entry[p + 2] == '/') return true;
  return false;
}
inline std::string WithTag(const std::string& entry) {
  if (HasTag(entry)) return entry;
  size_t at = 0;
  if (entry.rfind("model_key:", 0) == 0) {
    at = 10;
    while (at < entry.size() && entry[at] >= '0' && entry[at] <= '9') ++at;
  } else {
    at = entry.size();
  }
  return entry.substr(0, at) + "/m" + entry.substr(at);
}

// One mask entry (a 16-bit DDS blob, as attire_compose reads it) -> a texture whose r, g, b, a are the mask's R, G, B, A channels scaled to 0..255 (level 0 only: the
// shader tests "non-zero", the composite uses level 0 too). The common header (R 0x0F00, G 0x00F0, B 0x000F, A 0xF000) is kept as stored (kB4G4R4A4: the front end widens
// exactly those nibbles at the upload, front_end.cpp MakeTexture); any other 16-bit layout is decoded by its header's masks to RGBA8.
inline bool DecodeMask(const uint8_t* blob, size_t n, const std::string& name, assets::Texture& out, std::string& why) {
  attire_compose::detail::DdsView v;
  if (!attire_compose::detail::View(blob, n, v, why)) return false;
  if (v.bits != 16 || v.fourcc != 0) return why = "not a 16-bit uncompressed DDS", false;
  const size_t texels = size_t(v.w) * v.h;
  if (v.n < texels * 2) return why = "shorter than its first level", false;
  out = assets::Texture();
  out.name = name, out.width = v.w, out.height = v.h, out.mips = 1;
  if (v.cm[0] == 0x0F00u && v.cm[1] == 0x00F0u && v.cm[2] == 0x000Fu && v.cm[3] == 0xF000u) {
    out.format = assets::TexFormat::kB4G4R4A4;
    out.data.assign(v.data, v.data + texels * 2);
    return true;
  }
  uint32_t shift[4], top[4];
  for (int c = 0; c < 4; ++c) {
    shift[c] = v.cm[c] ? uint32_t(__builtin_ctz(v.cm[c])) : 0;
    top[c] = v.cm[c] ? (v.cm[c] >> shift[c]) : 1;
  }
  out.format = assets::TexFormat::kR8G8B8A8;
  out.data.assign(texels * 4, 0);
  for (size_t i = 0; i < texels; ++i) {
    const uint32_t px = uint32_t(v.data[2 * i]) | (uint32_t(v.data[2 * i + 1]) << 8);
    for (int c = 0; c < 4; ++c) out.data[4 * i + size_t(c)] = v.cm[c] ? uint8_t((((px & v.cm[c]) >> shift[c]) * 255u + top[c] / 2) / top[c]) : 0;
  }
  return true;
}

// The loader's step (live_assets.cpp GameFilesSource::Load, under `/m`): `entry_blob(k)` returns the decoded bytes of PACH entry 13 + k (null = absent). Each present mask
// becomes a texture of `m` and every material's change_mask[k] names it; an unreadable mask is left out (its slot binds black = that mask does nothing), counted in `why`.
// Returns the number of masks kept (0 = the body draws without change colour).
template <class EntryBlobFn>
inline int KeepMasks(assets::Model& m, EntryBlobFn entry_blob, std::string& why) {
  int kept = 0;
  int index[kMasks] = {-1, -1, -1, -1};
  for (int k = 0; k < kMasks; ++k) {
    size_t n = 0;
    const uint8_t* b = entry_blob(k, n);
    if (!b) continue;
    assets::Texture t;
    std::string w;
    if (!DecodeMask(b, n, "Mask" + std::to_string(k + 1), t, w)) { why += (why.empty() ? "" : "; ") + std::string("mask ") + std::to_string(kFirstEntry + uint32_t(k)) + ": " + w; continue; }
    index[k] = int(m.textures.size());
    m.textures.push_back(std::move(t));
    ++kept;
  }
  for (assets::Material& mat : m.materials)
    for (int k = 0; k < kMasks; ++k) mat.change_mask[k] = index[k];
  return kept;
}

// S108 THB (docs\research\s108\THB_threads_gear.md): an entrance PROP's mask. The prop's evtobj.pac "/SPMT/<id>" PACH holds one picture, `Mask1` (asset_load.h LoadPropMask), whose four channels are the masks of the Threads set's 4 gear
// colours; the recolour shader reads it at pixel slot 12. It rides the model as one more texture and every material's change_mask[0] names it (the model's own materials sample texture 0 as diffuse, never this one). The loader (live_assets.cpp
// ApplyPropMask) calls it for every entry of a prop id that has an SPMT row; the draw asks for the cc_mask row only while the record carries colours for that model.
inline void KeepPropMask(assets::Model& m, assets::Texture&& t) {
  const int index = int(m.textures.size());
  m.textures.push_back(std::move(t));
  for (assets::Material& mat : m.materials) mat.change_mask[0] = index;
}

}  // namespace change_mask
