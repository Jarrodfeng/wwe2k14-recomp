// wwe2k14 launcher - the DLC wrestlers' names, read from the player's own DLC files (Session 32, Jarrod: "Add them
// now", DLC wrestlers in Quick Match).
//
// A character's name is the game's string text id 60000 + its character id (docs\systems\dlc.md "Names";
// tools\ids\names_from_strings.py does the same in Python). A pack's string file (pac\string_dlcPP.pac, in a folder or
// inside a package) is a run of zlib streams, each one string table: u32 0, u32 COUNT, COUNT x (u32 offset, u32 length,
// u32 text id), then UTF-8 strings. The first table with a name list is the first language set (the names are the
// same in the others as far as checked). Nothing is written or kept on disk.
//
// Needs stb_image.h (stbi_zlib_decode_malloc_guesssize_headerflag) included before this header, like game_font.h.
#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../dlc_catalog.h"

namespace wwe2k14::dlc {

constexpr uint32_t kNameTextBase = 60000;
constexpr size_t kMaxStringPackBytes = 64u << 20;  // the four known ones are 6.6 MB each

inline uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// {character id: the game's name} from a string file's bytes; empty if there is no name list.
inline std::map<int, std::string> NamesFromStringPack(const std::vector<uint8_t>& data) {
  for (size_t s = 0; s + 2 < data.size(); ++s) {
    if (data[s] != 0x78 || (data[s + 1] != 0x01 && data[s + 1] != 0x5E && data[s + 1] != 0x9C && data[s + 1] != 0xDA))
      continue;
    int out_len = 0;
    const int in_len = int(std::min<size_t>(data.size() - s, size_t(32) << 20));
    char* out = stbi_zlib_decode_malloc_guesssize_headerflag(reinterpret_cast<const char*>(&data[s]), in_len, 1 << 16,
                                                             &out_len, 1);
    if (!out) continue;
    std::map<int, std::string> names;
    const auto* o = reinterpret_cast<const uint8_t*>(out);
    const size_t n = size_t(out_len);
    if (n >= 8 && Le32(o) == 0) {
      const size_t count = Le32(o + 4);
      if (count < 200000 && 8 + 12 * count <= n) {
        for (size_t i = 0; i < count; ++i) {
          const uint8_t* r = o + 8 + 12 * i;
          const uint32_t off = Le32(r), len = Le32(r + 4), tid = Le32(r + 8);
          if (tid < kNameTextBase + 100 || tid >= kNameTextBase + 1000 || off > n || len > n - off) continue;
          const char* str = reinterpret_cast<const char*>(o + off);
          names[int(tid - kNameTextBase)] = std::string(str, strnlen(str, len));
        }
      }
    }
    STBI_FREE(out);
    if (names.size() >= 50) return names;  // a real name list (other tables hold a stray id or two in the range)
  }
  return {};
}

// The pack's own string file, from its folder or from inside its package.
inline std::map<int, std::string> NamesFromPack(const Pack& p) {
  if (p.string_pack.empty()) return {};
  std::vector<uint8_t> data;
  if (p.package) {
    StfsPackage pkg;
    std::string why;
    const StfsPackage::File* f = nullptr;
    if (!pkg.Open(p.root, why) || !(f = pkg.Find(p.string_pack)) || !pkg.Read(*f, data, kMaxStringPackBytes)) return {};
  } else {
    std::string rel = p.string_pack;
    std::replace(rel.begin(), rel.end(), '\\', '/');
    if (!ReadSmallFile(p.root / fs::path(std::u8string(rel.begin(), rel.end())), data, kMaxStringPackBytes)) return {};
  }
  return NamesFromStringPack(data);
}

// "BIG E LANGSTON" -> "Big E Langston"; words that already have lower-case letters ("(n.W.o.)", "(Outsiders)") stay.
inline std::string ReadableName(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t j = s.find(' ', i);
    if (j == std::string::npos) j = s.size();
    std::string w = s.substr(i, j - i);
    const bool has_lower = std::any_of(w.begin(), w.end(), [](char c) { return c >= 'a' && c <= 'z'; });
    if (!has_lower) {
      bool first = true;
      for (char& c : w) {
        if (c >= 'A' && c <= 'Z' && !first) c = char(c - 'A' + 'a');
        if (std::isalpha(static_cast<unsigned char>(c))) first = false;
      }
    }
    out += (out.empty() ? "" : " ") + w;
    i = j + 1;
  }
  return out;
}

}  // namespace wwe2k14::dlc
