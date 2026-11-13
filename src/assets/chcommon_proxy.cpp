// See chcommon_proxy.h. S101 lane 99.
// stb_image is compiled INTO this translation unit, static (no stbi_* symbol leaves it), exactly like prop_game_space.cpp.
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include "chcommon_proxy.h"

#include <filesystem>

#include "asset_load.h"

namespace assets::chcommon_proxy {

std::shared_ptr<const ProxySet> Load(const std::string& game_root, std::string& err) {
  launcher::GameFiles files;
  files.SetRoot(std::filesystem::u8path(game_root));
  pac::Pac pc;
  if (!pac::Open(files, "pac\\ch\\chCommon.pac", pc, err)) {
    if (err.empty()) err = "cannot open pac\\ch\\chCommon.pac";
    return nullptr;
  }
  const pac::Ent* e = nullptr;
  auto set = std::make_shared<ProxySet>();
  for (const pac::Pach& h : pc.pachs)
    if ((e = pac::Find(h, 0)) != nullptr) {
      set->blob = pac::Decode(pc.bytes.data() + e->off, e->size);
      break;
    }
  if (!e) return err = "chCommon.pac has no entry 0", nullptr;
  if (!jboy_parts::Parse(set->blob, set->parts, err)) {
    if (err.empty()) err = "chCommon.pac entry 0 is not a model";
    return nullptr;
  }
  return set;
}

}  // namespace assets::chcommon_proxy
