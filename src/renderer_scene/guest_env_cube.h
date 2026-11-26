// S111 GUESTCUBE: the environment light cube (g_mEnvLPrm, pixel sampler 10: env_cube.h) of a CREATED superstar's parts (the guest model path: guest_model_draw.h BuildModel). docs\research\s111\GUESTCUBE.md.
// The game binds slot 10 from the part's material sampler, whose texture the part's file record names (the edit pacs: CUBEMAP_eyes on yCh_Reflect, CUBEMAP on yCh_trans / yCh_TransTiling). The only textures
// of those names are chCommon.pac PACH 0 entry 21's two file cubes (BC1 128x128, 8 mips, 6 faces; different texels). The guest reader sees that the bound object is a cube (fetch dimension 3: row state
// kGtEngineDefault, no bytes); S111 EYECUBE: the record now carries the part's texture NAME (sections 72 / 73) and the cube is picked by it (CubeByName); a record without it keeps the old pick by effect
// (LegacyName). Both cubes are read ONCE a process from the same chCommon directories every stock wrestler
// falls back to (guest_env_cube_load.h LoadAndPublish, from GameFilesSource::SetRoot) and published immutable; BuildModel reads Now() and copies the one it needs into the model (64 KB; the front end
// uploads it once: AddModel's shared map keys it by name / size). Not published (no game files) = no cube: slot 10 stays the black cube, as before.
// Header-only: the standard library + asset_model.h (the guest unit builds guest_model_draw.h against record_io.cpp alone). Unit: tools\scene\envcube_unit.cpp.
#pragma once
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "../assets/asset_model.h"
#include "../assets/tex_name.h"   // S111 EYECUBE: the one read-only by-name lookup

namespace rdr_scene::guest_env_cube {

inline constexpr const char* kEyesName = "CUBEMAP_eyes";   // the edit pacs' yCh_Reflect (eye) records name it
inline constexpr const char* kPlainName = "CUBEMAP";       // their yCh_trans / yCh_TransTiling records name it

struct Cubes {
  assets::Texture eyes;    // CUBEMAP_eyes
  assets::Texture plain;   // CUBEMAP
};

namespace detail {
inline std::mutex& Mu() {
  static std::mutex m;
  return m;
}
inline std::shared_ptr<const Cubes>& Held() {
  static std::shared_ptr<const Cubes> c;
  return c;
}
}  // namespace detail

// Any thread. The loader publishes once a process; null clears (the unit).
inline void Publish(std::shared_ptr<const Cubes> c) {
  std::lock_guard<std::mutex> l(detail::Mu());
  detail::Held() = std::move(c);
}
inline std::shared_ptr<const Cubes> Now() {
  std::lock_guard<std::mutex> l(detail::Mu());
  return detail::Held();
}

// S111 EYECUBE: the published cube whose name is `name` (the part record's texture name, the one sub_82BD9A60 resolves: case-insensitive, assets\tex_name.h). nullptr = nothing published, no cube of
// that name (the game's miss: key {0, 0}; ours binds the black cube), or not a six-face image.
inline const assets::Texture* CubeByName(const Cubes* c, std::string_view name) {
  if (!c) return nullptr;
  const assets::Texture* set[2] = {&c->eyes, &c->plain};
  const assets::Texture* t = assets::FindTexByName(set, 2, name);
  return t && t->faces == 6 && !t->data.empty() ? t : nullptr;
}
// The name a record WITHOUT sections 72 / 73 (written before them), or a part whose name was not read, stands for: the edit pacs' name by effect (GUESTCUBE's census: yCh_Reflect = CUBEMAP_eyes, any
// other = CUBEMAP). A fitted stand-in for those records only.
inline const char* LegacyName(const std::string& shader) { return shader == "yCh_Reflect" ? kEyesName : kPlainName; }

// The Model::textures index of `cube` in `out`: the copy this build already added (same name, six faces), else a new copy appended. -1 for nullptr.
inline int Attach(assets::Model& out, const assets::Texture* cube) {
  if (!cube) return -1;
  for (size_t i = 0; i < out.textures.size(); ++i)
    if (out.textures[i].faces == 6 && out.textures[i].name == cube->name) return int(i);
  out.textures.push_back(*cube);
  return int(out.textures.size()) - 1;
}

}  // namespace rdr_scene::guest_env_cube
