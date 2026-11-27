// S111 LEVELSBYTE (docs\research\s111\LEVELSBYTE.md): the texture level count a character's model was built with, carried from the game's load request to our loader.
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause. Header only, std only: the contributor (wrestler_contrib.cpp KeyLink, through build_levels_note.h), the
// loader (live_assets.cpp GameFilesSource::Load, asset_load.h LoadWrestlerAt, tex_dir_order.h Add) and the offline unit (tools\scene\levelsbyte_unit.cpp) run THESE.
// The game: the character model reader (Factory_Texture sub_8271FEB8) builds the character's texture lists at byte +6 of its load request (the 72-byte record of the
// model slot, [0x83924734] + 72 x slot). A fresh request holds 4 (sub_823BB100); the match / entrance / scene-build fillers (sub_825C0EC8, sub_82311D30, sub_82314128)
// raise it to 5 only for a created superstar's key (<= 9999, sub_822C0998), the match filler sets 0 (= the whole chain) for the keys whose hundreds are 255, the
// Threads editor's preview (sub_8339D7C8) 1. A stock wrestler's key is >= 10000: its textures are built at 4 in a match, an entrance and a story scene.
// The record's entry carries a count other than 4 as the tag "/l<byte>", right after "model_key:<digits>" (change_mask.h's "/m", the composite "/c:<hex>" and
// the refusal "/r" suffix keep their places and their parsers). No tag = 4. The byte is always the game's; ours never picks a count of its own.
#pragma once
#include <cstdint>
#include <string>

namespace assets::level_tag {

inline constexpr uint8_t kFreshRequestByte = 4;                  // sub_823BB100: a fresh load request's byte +6
inline constexpr uint32_t kStockTexLevels = kFreshRequestByte;   // a stock wrestler's own textures: the count of every character load without a tag
inline constexpr uint32_t kWholeChainLevels = 32;                // above any texture's chain = the whole chain (game_levels.h GameLevels; the game's 0, sub_82CCC488)

// The entry tag of a request byte: "" for the fresh request's 4, else "/l<byte>".
inline std::string Tag(uint8_t byte) { return byte == kFreshRequestByte ? std::string() : "/l" + std::to_string(unsigned(byte)); }

// The request byte an entry names: its "/l<1-3 digits>" tag (followed by '/' or the end, at most 255), else 4.
inline uint8_t ByteOfEntry(const std::string& entry) {
  for (size_t p = entry.find("/l"); p != std::string::npos; p = entry.find("/l", p + 1)) {
    size_t q = p + 2;
    unsigned v = 0;
    while (q < entry.size() && q < p + 5 && entry[q] >= '0' && entry[q] <= '9') v = v * 10 + unsigned(entry[q] - '0'), ++q;
    if (q > p + 2 && v <= 255 && (q == entry.size() || entry[q] == '/')) return uint8_t(v);
  }
  return kFreshRequestByte;
}

// The Texture::game_levels of a request byte (0 means "not known" there, so the game's 0 = the whole chain becomes kWholeChainLevels).
inline uint32_t TexLevels(uint8_t byte) { return byte == 0 ? kWholeChainLevels : uint32_t(byte); }

// The count a character load named by `entry` gives its own texture directories (GameFilesSource::Load -> LoadWrestlerAt).
inline uint32_t TexLevelsOfEntry(const std::string& entry) { return TexLevels(ByteOfEntry(entry)); }

}  // namespace assets::level_tag
