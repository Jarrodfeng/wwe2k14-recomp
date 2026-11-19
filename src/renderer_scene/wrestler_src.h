// S103 DLW: the live loader's file reader sees the packs the game was offered (dlc_overlays.h), and says where a wrestler's pac came from.
// ApplyOverlays gives a launcher::GameFiles the published packs as read-only overlays (the same GameFiles overlay rule the viewer uses,
// assets\dlc_files.h Configure: the disc first, then each pack in pack-id order, each tried with the literal name and with its "_dlcPP" name).
// Where(): "disc" | "dlc" | "mod" (the imported-superstars pack) | "none": the census word (`[wwe2k14][wrestler_src]`, live_assets.cpp).
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../dlc_overlays.h"
#include "../launcher/disc_image.h"

namespace rdr_scene::wrestler_src {

// One opened STFS package per file, shared by every reader (the render thread's and the loader's GameFilesSource): the walk happens once.
inline std::shared_ptr<launcher::GameFiles::PackageSource> OpenPackage(const std::filesystem::path& p) {
  static std::mutex mu;
  static std::map<std::filesystem::path, std::shared_ptr<launcher::GameFiles::PackageSource>> cache;   // S103 DLF: keyed on the path itself (generic_string() throws on MSVC for unmappable characters)
  std::lock_guard<std::mutex> l(mu);
  auto& e = cache[p];
  if (!e) e = std::make_shared<launcher::GameFiles::PackageSource>(p);
  return e;
}

// Re-applies when the published set changed since `*gen` (a reader keeps its own generation). Returns true when it changed the overlays.
inline bool ApplyOverlays(launcher::GameFiles& files, uint64_t* gen) {
  const uint64_t g = wwe2k14::dlc_overlays::Generation();
  if (gen && *gen == g) return false;
  if (gen) *gen = g;
  std::vector<launcher::GameFiles::Overlay> ov;
  for (const auto& it : wwe2k14::dlc_overlays::Get()) ov.push_back({it.root, it.suffix, it.kind, it.package ? OpenPackage(it.root) : nullptr});
  files.SetOverlays(std::move(ov));
  return true;
}

inline const char* Where(const launcher::GameFiles& files, const std::string& rel) {
  if (files.InRoot(rel)) return "disc";
  const launcher::GameFiles::Overlay* o = files.OverlayOf(rel);
  if (!o) return "none";
  return o->kind == "mod" ? "mod" : "dlc";
}

}  // namespace rdr_scene::wrestler_src
