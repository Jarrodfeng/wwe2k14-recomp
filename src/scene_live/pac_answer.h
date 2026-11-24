// S110 PACANS (docs/research/s110/SEATDRAW.md): the parsers of the game's own pac-list answer ("which pac file holds this model").
// Our renderer used to name a wrestler's model file by its own rule (`pac\ch\ch<model key / 100>.pac`). The game decides it by its pac-list lookup
// (sub_82C51D20: the asked name `/EMD/000NNNnn` -> the first node of the list holding that name). When the node's file is not the id rule's
// (a seated character, a title-update node at the head of the list, another pack order) the two disagree and our model has the wrong skeleton.
// S110 SEATREL: this header holds only the two parsers and the Answer type. WHICH answer names a body is decided by model_file_note.h: the answer of
// the read that BUILT the game's model (per slot descriptor), never the last lookup of a name (the menus' probe lookups also ask sub_82C51D20).
// Header-only, std only (the hook and the unit tests both include it).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace rdr_scene::live::pac_answer {

struct Answer {
  uint32_t file_id = 0;   // the character id of the file the game took (CH<id>...)
  uint32_t file_key = 0;  // file_id x 100 + key % 100: the key the file's own directory rows carry
  std::string lib_rel;    // S110 LIBSIZE: a library node (SSLIB:\<KEY>\...): `<KEY>\pac\ch\ch497.pac` under the one "lib" overlay; empty = not a library node
};

namespace detail {
inline bool Digit(char c) { return c >= '0' && c <= '9'; }
inline std::string_view LastComponent(std::string_view p) {
  const size_t at = p.find_last_of("\\/");
  return at == std::string_view::npos ? p : p.substr(at + 1);
}
}  // namespace detail

// `/EMD/000NNNnn` (the last component: exactly 8 decimal digits) -> the model key (leading zeros dropped). False for anything else.
inline bool ParseAsked(std::string_view asked, uint32_t& key, std::string& name8) {
  const std::string_view c = detail::LastComponent(asked);
  if (c.size() != 8) return false;
  const std::string_view dir = asked.substr(0, asked.size() - c.size());   // "/EMD/" (or "EMD/"): only the model directory's names are model keys
  if (dir != "/EMD/" && dir != "EMD/") return false;
  uint32_t k = 0;
  for (char ch : c) {
    if (!detail::Digit(ch)) return false;
    k = k * 10 + uint32_t(ch - '0');
  }
  if (k < 100) return false;
  key = k;
  name8 = std::string(c);
  return true;
}

// A node path `...\CH<id>_DLC<pp>.PAC` or the disc form `...\CH<id>.PAC` (any case) -> the id. The WHOLE file name must have that form:
// the game links a node into the list before it copies the node's path (sub_82C51B98 then sub_82C51B20; the disc pass sub_82C52668 +E0..+138)
// and the lookup takes no lock, so a lookup on another thread can see a prefix of the path (review S110 PACANS: `...\CH39` must not answer 39).
inline bool ParseNodeFileId(std::string_view node_path, uint32_t& id) {
  size_t n = 0;
  while (n < node_path.size() && node_path[n] != '\0') ++n;   // a 48-byte field: ends at the first NUL
  const std::string_view c = detail::LastComponent(node_path.substr(0, n));
  auto up = [](char ch) { return ch >= 'a' && ch <= 'z' ? char(ch - 32) : ch; };
  auto lit = [&](size_t at, const char* s) {   // c[at..] starts with the upper-case literal s
    for (size_t k = 0; s[k]; ++k)
      if (at + k >= c.size() || up(c[at + k]) != s[k]) return false;
    return true;
  };
  if (!lit(0, "CH") || c.size() < 3 || !detail::Digit(c[2])) return false;
  uint32_t v = 0;
  size_t i = 2;
  for (; i < c.size() && detail::Digit(c[i]); ++i) {
    v = v * 10 + uint32_t(c[i] - '0');
    if (v > 99999) return false;
  }
  if (lit(i, "_DLC")) {   // the pack form: _DLC<digits>
    const size_t d = i + 4;
    for (i = d; i < c.size() && detail::Digit(c[i]); ++i) {}
    if (i == d) return false;
  }
  if (c.size() != i + 4 || !lit(i, ".PAC")) return false;
  id = v;
  return true;
}

// S110 LIBSIZE (docs/research/s110/LIBSIZE.md): every library file is CH497.PAC, so the folder comes from the node path: `SSLIB:\<KEY>\rest` -> `<KEY>\rest`,
// lower-cased BELOW the KEY folder (the phone's file system is case-sensitive; the KEY folder keeps its case). Empty for a node that is not under SSLIB:\,
// and for a path with no KEY folder (`SSLIB:\CH497.PAC`: nothing to resolve under the library root) or nothing after it.
inline std::string LibRel(std::string_view node_path) {
  size_t n = 0;
  while (n < node_path.size() && node_path[n] != '\0') ++n;
  const std::string_view p = node_path.substr(0, n);
  constexpr std::string_view kPre = "SSLIB:\\";
  if (p.size() <= kPre.size()) return {};
  for (size_t i = 0; i < kPre.size(); ++i) {
    const char c = p[i] >= 'a' && p[i] <= 'z' ? char(p[i] - 32) : p[i];
    if (c != kPre[i]) return {};
  }
  const std::string_view rest = p.substr(kPre.size());
  const size_t cut = rest.find('\\');
  if (cut == 0 || cut == std::string_view::npos || cut + 1 >= rest.size()) return {};
  std::string out(rest);
  for (size_t i = cut + 1; i < out.size(); ++i)
    if (out[i] >= 'A' && out[i] <= 'Z') out[i] = char(out[i] + 32);
  return out;
}

}  // namespace rdr_scene::live::pac_answer
