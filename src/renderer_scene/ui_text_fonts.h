// The three menu fonts' glyph tables for our UI text (Session 101, lane 89 = UT), read from the player's own pac\menu\fontHD.pac. Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The same rules as UiAssets::Font (ui_assets.cpp: records, kerning pair list, per-font trim) without the sheet's pixels and without the 2D pass's dependencies: ui_assets.cpp
// is in no game / fview build list (its stb_image declarations need an external symbol) and this hook has to be in all of them, so this file carries its own STATIC stb_image
// like ui_ours_core.cpp. A change to the .yfb rules goes in both.
#pragma once
#include <string>

#include "ui_assets.h"

namespace rdr_scene {

// fonts[0..2] = font000 / font001 / font002 (glyph records, `kerns`, `advance_trim` 2 / 2 / 7, `cell`, sheet.width / height; sheet.data stays empty). false + err: loud.
bool UiTextLoadFonts(const std::string& game_root, UiFont fonts[3], std::string& err);

}  // namespace rdr_scene
