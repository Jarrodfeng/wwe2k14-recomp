// S111 GUESTCUBE: reads guest_env_cube.h's two cubes from the player's chCommon.pac (the shared wrestler texture directories: asset_load.h LoadCommonTexDirs, read once a process) and publishes them.
// Called by GameFilesSource::SetRoot (live_assets.cpp, under load_detail::LoadMutex: CommonTexDirs is shared) and envcube_unit's `real` mode. docs\research\s111\GUESTCUBE.md.
// Needs stb_image.h declared before it, as asset_load.h does.
#pragma once
#include <memory>
#include <string>

#include "../assets/asset_load.h"
#include "guest_env_cube.h"

namespace rdr_scene::guest_env_cube {

// CUBEMAP_eyes and CUBEMAP parsed from chCommon's texture directories (the TexSet lookup the stock path uses: case-insensitive, first directory wins). False + why when either is missing or not a cube file.
inline bool LoadCubes(launcher::GameFiles& files, Cubes& out, std::string& why) {
  assets::load_detail::LoadCommonTexDirs(files);
  if (assets::load_detail::CommonTexDirs().empty()) return why = "chCommon.pac gave no texture directory (" + assets::load_detail::CommonTexError() + ")", false;
  assets::TexSet set;
  for (const assets::Bytes& d : assets::load_detail::CommonTexDirs()) set.AddDir(assets::Bytes(d));
  std::string w;
  if (!set.Peek(kEyesName, out.eyes, w)) return why = std::string(kEyesName) + ": " + w, false;
  if (!set.Peek(kPlainName, out.plain, w)) return why = std::string(kPlainName) + ": " + w, false;
  if (out.eyes.faces != 6 || out.plain.faces != 6) return why = "chCommon's CUBEMAP_eyes / CUBEMAP is not a six-face cube file", false;
  return true;
}

// Loads and publishes once a process (a published set stands: the same game's chCommon). False + why when the cubes could not be read (nothing published: slot 10 stays black).
inline bool LoadAndPublish(launcher::GameFiles& files, std::string& why) {
  if (Now()) return true;
  auto c = std::make_shared<Cubes>();
  if (!LoadCubes(files, *c, why)) return false;
  Publish(std::move(c));
  return true;
}

}  // namespace rdr_scene::guest_env_cube
