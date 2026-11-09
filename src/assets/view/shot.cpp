// See shot.h. SDL3's own PNG writer (SDL_SavePNG, SDL 3.4+): no video subsystem needed, UTF-8 paths on every platform.
#include "shot.h"

#include <SDL3/SDL.h>

#include <filesystem>

namespace view {

bool WritePng(const std::string& path_in, const uint8_t* bgra, int w, int h, std::string& err) {
  try {
    std::filesystem::path p = std::filesystem::absolute(std::filesystem::u8path(path_in));
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
  } catch (const std::exception& e) {
    err = std::string("cannot create folder: ") + e.what();
    return false;
  }
  // B8G8R8A8 in memory = SDL's BGRA32 (byte order)
  SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_BGRA32, const_cast<uint8_t*>(bgra), w * 4);
  if (!s) { err = std::string("SDL_CreateSurfaceFrom: ") + SDL_GetError(); return false; }
  const bool ok = SDL_SavePNG(s, path_in.c_str());
  if (!ok) err = std::string("SDL_SavePNG: ") + SDL_GetError();
  SDL_DestroySurface(s);
  return ok;
}

}  // namespace view
