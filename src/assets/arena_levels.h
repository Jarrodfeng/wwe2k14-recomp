// S111 ARENALEVELS (docs\research\s111\ARENALEVELS.md): the mip level count the game's stage and crowd builders give each arena texture (assets::Texture::game_levels;
// renderer_scene\game_levels.h Relevel uploads it). Header only; pure over bytes (asset_load.h LoadArena / AddCrowd and crowd_body.cpp call it; the unit
// tools\scene\arena_levels_unit.cpp builds its own bytes). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// The game [C]:
//   * The stage's TextureSetup (sub_824E7AE8, CStage vtable slot 19) sets the loader's level-count word to 3, builds the key-400 directory texture by texture (sub_824E7EA8),
//     appends 401 and 402 (sub_82BA9D68, no per-name step) and restores the word. A created arena's TextureSetup_CAR (sub_827648C8) does the same for its three parts' 400
//     directories (sub_82764200 each), one part's 401 and one part's 402. Ours' merged created-arena pac names those 400s 400 / 403 / 404 (car_arena.h MergeParts).
//   * One key-400 texture: the count is 3, unless the arena's table names it: pac entry 50010, an XML "MipmapData" list that sub_827FDEB0 reads into 32-byte rows (the name,
//     byte 31 = MipmapLevel) kept at [stage + 23152] (sub_824E9710; the other two writers of that word store 0); sub_827FDB78 takes the FIRST row whose name equals the
//     directory record's bytes up to their NUL. Then, when the arena record's byte 24 is set (sub_822C38B8 / sub_824E7668: pac\misc01_Start.pac p0/2, ids 200..249 read
//     row 200), the names in kWholeChainNames get 0, and on a created arena (sub_82764200 only) the names in kDesignNames get 1. The loader's count 0 is the size's whole chain
//     (sub_82CCC488).
//   * The crowd builder (sub_8250C1D0) builds key 21000 of the crowd set (nested PACH 20000) at 3.
// The XML reader (sub_82C51718 -> sub_82C50F78 / sub_82C51110): a token is a tag '<' .. '>' or a text run up to the next '<'; the bytes 1..32 after any token are skipped,
// so white space between tags is never a text token; a text token keeps the last tag's name, start or end.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "arena_props_table.h"   // arena_props::RecordKey (ids 200..249 -> 200)
#include "asset_model.h"
#include "dds.h"                 // TexSet
#include "level_tag.h"           // level_tag::kWholeChainLevels (the game's count 0 = the whole chain, as a Texture::game_levels)
#include "pac.h"

namespace assets::arena_levels {

inline constexpr uint32_t kStageListLevels = 3;   // sub_824E7AE8 +0x24C, sub_827648C8 +0x348: the stage's lists
inline constexpr uint32_t kCrowdLevels = 3;       // sub_8250C1D0 +0x130: the crowd's key-21000 list
inline constexpr uint32_t kMipTableEntry = 50010; // the arena's per-name table (sub_824E9710 +0x24)
inline constexpr size_t kRecordByte = 24;         // sub_822C38B8: the arena record byte that turns the name rules on

// sub_824E7EA8 +0xCC..+0xFC (sub_82764200 +0x110..+0x140): 11 slots, 9 names (slo_02 / slo_02s are listed twice).
inline constexpr const char* kWholeChainNames[] = {"ar_dm", "slo_02", "slo_02s", "raw_slope1", "fleet_sh2", "ar_fence", "ar_fence_b", "ji_top", "raw_titan_l"};
// sub_82764200 +0x9C..+0x1C8: a created arena's 6 names at count 1.
inline constexpr const char* kDesignNames[] = {"design_01", "design_02", "design_03", "design_04", "design_05", "design"};

// One row of the table (sub_827FDEB0: the name in bytes 0..30, MipmapLevel in byte 31; rows past the last attribute stay zero = an empty name, count 0).
struct Row {
  std::string name;
  uint8_t level = 0;
};

namespace detail {
enum : int { kOther = 0, kStart = 6, kEnd = 7, kText = 14 };   // sub_82C51110's token kinds

// One token at p[i]; `i` moves past it and the bytes 1..32 after it. A tag sets `name` (a start tag's name ends at a space or a closing "/"; `tag` = the bytes between
// '<' and '>'); a text token sets `text` and keeps `name`. false at the end.
inline bool Next(const uint8_t* p, size_t n, size_t& i, int& type, std::string& name, std::string& text, std::string& tag) {
  if (i >= n) return false;
  const size_t s = i;
  if (p[i] == '<') {
    size_t j = i + 1;
    while (j < n && p[j] != '>') ++j;
    tag.assign(reinterpret_cast<const char*>(p + s + 1), j - s - 1);
    i = j < n ? j + 1 : n;
    if (!tag.empty() && tag[0] == '/') {
      type = kEnd, name = tag.substr(1);
    } else if (!tag.empty() && (tag[0] == '?' || tag[0] == '!')) {
      type = kOther;
    } else {
      size_t k = 0;
      while (k < tag.size() && tag[k] != ' ' && !(tag[k] == '/' && k + 1 == tag.size())) ++k;
      type = kStart, name = tag.substr(0, k);
    }
  } else {
    size_t j = i;
    while (j < n && p[j] != '<') ++j;
    text.assign(reinterpret_cast<const char*>(p + s), j - s);
    type = kText, i = j;
  }
  while (i < n && p[i] != 0 && p[i] <= 32) ++i;
  return true;
}

// The NumOfData attribute of a start tag's bytes (0 when absent or unusable; sub_827FDEB0 +0x1AC..+0x1D8 allocates that many zeroed rows).
inline size_t NumOfData(const std::string& tag) {
  const size_t a = tag.find("NumOfData");
  if (a == std::string::npos) return 0;
  size_t k = a + 9;
  while (k < tag.size() && (tag[k] == '=' || tag[k] == '"' || tag[k] == '\'' || tag[k] == ' ')) ++k;
  const long v = std::strtol(tag.c_str() + k, nullptr, 10);
  return v > 0 && v <= 65536 ? size_t(v) : 0;   // (the game takes up to 0x7FFFFFF; a table above 65536 rows is not a file this reader trusts)
}
}  // namespace detail

// The MipmapData rows of a 50010 text, as sub_827FDEB0 fills them: the start tag MipmapData allocates NumOfData rows; each other start tag met at the top level opens a row,
// closed by its own end tag (the next row); inside it the text after TextureName is the row's name (at most 30 bytes) and the text after MipmapLevel its level (strtol, low byte).
// A MipmapBiasData block's rows (a shape / texture LOD bias) are not read here.
inline std::vector<Row> ParseMipmapData(const uint8_t* p, size_t n) {
  std::vector<Row> rows;
  size_t i = (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) ? 3 : 0;   // sub_82C51718: a UTF-8 byte order mark
  int type = detail::kOther, mode = 2;   // sub_827FDEB0's r25: 0 = MipmapData rows, 1 = MipmapBiasData, 2 = neither yet
  size_t row = 0;
  std::string name, text, tag;
  while (detail::Next(p, n, i, type, name, text, tag)) {
    if (type != detail::kStart) continue;
    if (name == "MipmapData") {
      rows.assign(detail::NumOfData(tag), Row{});
      mode = 0, row = 0;
      continue;
    }
    if (name == "MipmapBiasData") {
      mode = 1, row = 0;
      continue;
    }
    const std::string wrapper = name;
    bool closed = false;
    while (detail::Next(p, n, i, type, name, text, tag)) {
      if (type == detail::kEnd && name == wrapper) {
        ++row, closed = true;
        break;
      }
      if (type != detail::kText || mode != 0 || row >= rows.size()) continue;
      if (name == "TextureName") rows[row].name = text.substr(0, 30);
      else if (name == "MipmapLevel") rows[row].level = uint8_t(std::strtol(text.c_str(), nullptr, 10));
    }
    if (!closed) break;
  }
  return rows;
}

// Byte `off` of the arena's 108-byte record in misc01_Start p0/2 (12-byte head, LE u32 count at +4; the first row whose byte 4 is the key): 0 when no record matches
// (sub_822C38B8 returns 0), -1 when the leaf is not the table.
inline int RecordByte(const std::vector<uint8_t>& leaf, int arena, size_t off) {
  if (leaf.size() < 12 || off >= 108) return -1;
  uint32_t n = 0;
  std::memcpy(&n, leaf.data() + 4, 4);
  const int key = arena_props::RecordKey(arena);
  for (uint32_t i = 0; i < n; ++i) {
    const size_t r = 12 + 108 * size_t(i);
    if (r + 108 > leaf.size()) return -1;
    if (leaf[r + 4] == key) return leaf[r + off];
  }
  return 0;
}

// The count rule of one arena's own key-400 textures.
struct Rule {
  std::vector<Row> rows;   // the 50010 table (empty: none, every name keeps `base`)
  uint32_t base = kStageListLevels;   // S111 PROPLEVELS: the count of a name the table does not hold (the stage's 3; prop_levels.h TableRule sets the prop builders' 3)
  bool byte24 = false;     // the arena record's byte 24
  bool created = false;    // a Create-an-Arena venue (200..249): sub_82764200's design names
  int record = 0;          // RecordByte's answer (-1 = the record leaf was not the table)

  // Texture::game_levels of the key-400 texture whose directory record reads `raw` (its bytes up to the NUL).
  uint32_t Of(const std::string& raw) const {
    int c = int(base);
    for (const Row& w : rows)
      if (!w.name.empty() && w.name == raw) {
        c = int(int8_t(w.level));   // sub_827FDB78: the byte, sign-extended
        break;
      }
    if (byte24) {
      for (const char* nm : kWholeChainNames)
        if (raw == nm) c = 0;
      if (created)
        for (const char* nm : kDesignNames)
          if (raw == nm) c = 1;
    }
    return c <= 0 ? level_tag::kWholeChainLevels : uint32_t(c);   // 0 = the whole chain (a negative byte, not in the files, is read as the chain too)
  }
  TexSet::LevelsOf Fn() const {
    return [this](const std::string& raw) { return Of(raw); };
  }
};

// The rule of arena `arena` from its record leaf (misc01_Start p0/2) and its PACH `h` (bytes `buf`); `created` = a Create-an-Arena venue (car::IsCustomId).
inline Rule MakeRule(const std::vector<uint8_t>& record_leaf, const pac::Pach& h, const Bytes& buf, int arena, bool created) {
  Rule r;
  r.created = created;
  r.record = RecordByte(record_leaf, arena, kRecordByte);
  r.byte24 = r.record > 0;
  if (const pac::Ent* e = pac::Find(h, kMipTableEntry)) {
    const Bytes t = pac::Decode(buf.data() + e->off, e->size);
    r.rows = ParseMipmapData(t.data(), t.size());
  }
  return r;
}

// Adds the arena's stage texture directories to `tex` (and, for a created arena, each to its part's set in `part`: CASD, CAEN, CARG) in the loader's order, each with its count:
// the own-400 directories per name (`r`), 401 / 402 at kStageListLevels. Returns how many were added to `tex`.
inline int AddStageDirs(TexSet& tex, TexSet* part, const pac::Pach& h, const Bytes& buf, bool created, const Rule& r) {
  const TexSet::LevelsOf per_name = r.Fn();
  // ours' created-arena merge (car_arena.h): CASD 400 -> 400, CAEN 400 -> 403 (+ CAEN 401), CARG 400 -> 404 (+ CARG 402); a bg pac: 400, 401, 402
  static constexpr std::pair<uint32_t, int> kCar[] = {{400u, 0}, {403u, 1}, {401u, 1}, {404u, 2}, {402u, 2}};
  static constexpr std::pair<uint32_t, int> kBg[] = {{400u, -1}, {401u, -1}, {402u, -1}};
  const std::pair<uint32_t, int>* keys = created ? kCar : kBg;
  const size_t nk = created ? 5 : 3;
  int added = 0;
  for (size_t k = 0; k < nk; ++k) {
    const pac::Ent* e = pac::Find(h, keys[k].first);
    if (!e) continue;
    const bool own400 = keys[k].first == 400u || keys[k].first == 403u || keys[k].first == 404u;
    auto add = [&](TexSet& s, Bytes&& d) { return own400 ? s.AddDirNamed(std::move(d), per_name) : s.AddDirLevels(std::move(d), kStageListLevels); };
    const Bytes dir = pac::Decode(buf.data() + e->off, e->size);
    if (add(tex, Bytes(dir))) ++added;
    if (created && part && keys[k].second >= 0) add(part[keys[k].second], Bytes(dir));
  }
  return added;
}

}  // namespace assets::arena_levels
