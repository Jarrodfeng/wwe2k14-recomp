// Pixels -> PNG for shots, UI shots and offline renders. Session 98 phone port: portable (SDL3's SDL_SavePNG, shot.cpp) instead of
// Windows' WIC; the decoded pixels are the same, the PNG bytes differ (another encoder).
#pragma once
#include <cstdint>
#include <string>

namespace view {

// `bgra` is w*h*4 bytes, row pitch w*4 (B8G8R8A8). The alpha channel is written as given. Creates the file's folder.
bool WritePng(const std::string& path, const uint8_t* bgra, int w, int h, std::string& err);

}  // namespace view
