// The ONE read-only lookup of a texture BY NAME over a set of parsed textures (S111 EYECUBE, docs\research\s111\EYECUBE.md). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// The game resolves a model's texture names this way: sub_82BD9A60 hands each 16-byte name of the model's table to sub_82BA9B60, which lower-cases it and compares it with each texture's name; a miss is
// {0, 0} (nothing bound). A name is up to 16 bytes, NUL padded (a 16-character name has no NUL). Read-only: no cache, no allocation, so any thread may call it on an immutable set (TexSet::Get writes a
// per-model cache, dds.h, and is the FILE loader's). Callers: guest_env_cube.h CubeByName (a created superstar's slot-10 cube); UNIFYMAT step 5's guest texture callback is meant to call it too.
#pragma once
#include <cstddef>
#include <string_view>

#include "asset_model.h"

namespace assets {

// `n` cut at its first NUL (a name table entry is NUL padded).
inline std::string_view TexNameView(std::string_view n) {
  const size_t z = n.find('\0');
  return z == std::string_view::npos ? n : n.substr(0, z);
}
// Equal when the ASCII case is ignored (sub_82BA9B60 lower-cases the key; the file directories hold mixed case: "CUBEMAP_eyes").
inline bool SameTexName(std::string_view a, std::string_view b) {
  a = TexNameView(a), b = TexNameView(b);
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const char x = (a[i] >= 'A' && a[i] <= 'Z') ? char(a[i] + 32) : a[i], y = (b[i] >= 'A' && b[i] <= 'Z') ? char(b[i] + 32) : b[i];
    if (x != y) return false;
  }
  return true;
}
// The texture of `set` (n entries, nullptr entries skipped) named `name`; nullptr = an empty name or none of that name (the game's miss).
inline const Texture* FindTexByName(const Texture* const* set, size_t n, std::string_view name) {
  if (TexNameView(name).empty()) return nullptr;
  for (size_t i = 0; i < n; ++i)
    if (set[i] && SameTexName(set[i]->name, name)) return set[i];
  return nullptr;
}

}  // namespace assets
