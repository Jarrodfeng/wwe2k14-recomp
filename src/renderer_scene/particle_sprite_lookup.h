// A particle batch's texture name -> the decoded sprite -> a sampled image (Session 108, lane PRT; split out of frame_hooks.cpp so the offline unit drives the SAME resolve and upload). BSD-3-Clause.
// Two sources, chosen by the name the namer gave (particle_namer.h):
//   pac\prtcls.pac + leaf      -> the file sprite decoded at boot (live_assets.h ParticleSpritesNow: the namer's index build);
//   guest:prtcls + content key -> the picture read from the game's own texture object (scene_live\particle_guest_sprites.h), when the file index does not hold it (a DLC pack's / mod's particle file).
// Render thread: no loading, no guest read; only lookups and the image upload.
#pragma once
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>

#include "../assets/asset_model.h"
#include "../renderer/renderer.h"
#include "../scene_live/particle_guest_sprites.h"

namespace rdr_scene {

inline size_t SpriteMipBytes(assets::TexFormat f, uint32_t w, uint32_t h) {
  const size_t blocks = size_t(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4);
  return f == assets::TexFormat::kBC1 ? blocks * 8 : blocks * 16;
}
// A particle sprite (BC1 / BC2 / BC3, as prtcls.pac stores them and as the game binds them) as a sampled image with the mips it holds. kNoImage for anything else.
inline rdr::ImageId MakeSpriteImage(rdr::Renderer& r, const assets::Texture& t) {
  const bool bc = t.format == assets::TexFormat::kBC1 || t.format == assets::TexFormat::kBC2 || t.format == assets::TexFormat::kBC3;
  if (!bc || t.width == 0 || t.height == 0 || (t.width % 4) || (t.height % 4) || t.width > 4096 || t.height > 4096) return rdr::kNoImage;
  uint32_t mx = std::max(t.width, t.height), maxmips = 1;
  while (mx > 1) mx >>= 1, ++maxmips;
  const uint32_t want = std::min(std::max(1u, t.mips), maxmips);
  size_t total = 0;
  uint32_t fit = 0;
  for (uint32_t i = 0; i < want; ++i) {
    const size_t sz = SpriteMipBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
    if (total + sz > t.data.size()) break;
    total += sz, ++fit;
  }
  if (!fit) return rdr::kNoImage;
  rdr::ImageDesc d;
  d.width = uint16_t(t.width), d.height = uint16_t(t.height), d.mips = uint8_t(fit), d.usage = rdr::kUsageSampled;
  d.format = t.format == assets::TexFormat::kBC1 ? rdr::Format::BC1 : t.format == assets::TexFormat::kBC2 ? rdr::Format::BC2 : rdr::Format::BC3;
  const rdr::ImageId id = r.CreateImage(d);
  if (id == rdr::kNoImage) return id;
  size_t at = 0;
  for (uint32_t i = 0; i < fit; ++i) {
    const size_t sz = SpriteMipBytes(t.format, std::max(1u, t.width >> i), std::max(1u, t.height >> i));
    r.UpdateImage(id, i, 0, t.data.data() + at, sz);
    at += sz;
  }
  return id;
}

// The sprite a batch names, or null. `guest_name` = the name is a guest picture's (its bytes exist only in the live process that read them: a record replayed offline cannot have them).
// `files` = the published file sprites (null until built). The returned pointer keeps its source alive.
inline std::shared_ptr<const assets::Texture> ParticleSpriteFor(const std::string& pac, const std::string& entry, const std::shared_ptr<const std::unordered_map<std::string, assets::Texture>>& files,
                                                                const live::particles::GuestSprites& guest, bool& guest_name) {
  guest_name = pac == live::particles::kGuestSpritePac;
  if (guest_name) {
    uint64_t key = 0;
    return live::particles::GuestSpriteKey(entry, key) ? guest.Find(key) : nullptr;
  }
  if (!files) return nullptr;
  const auto it = files->find(entry);
  return it == files->end() ? nullptr : std::shared_ptr<const assets::Texture>(files, &it->second);
}

}  // namespace rdr_scene
