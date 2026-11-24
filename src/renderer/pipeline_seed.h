// S106 PSEED: the shipped pipeline seed (header-only, no Vulkan, no SDK: the renderer's StoreInit and tools\renderer\pseed_unit.cpp include it).
// entry: rdr::pstore::ParseSeedLines, rdr::pstore::MergeSeed, rdr::pstore::SaveKeeps
//
// The seed is `R <hex>` lines in shader_list.txt (hashes, states, vertex layouts and pass formats of OUR pipelines: no SPIR-V, no game bytes: the rule-1 exception of that
// file, DECISIONS S92) and one `H rdr <version> <count>` header line. <hex> is PutRecord's bytes (kRecordBytes * 2 digits). It is device-neutral by TOKENS: PassData::formats hold VkFormat numbers, so
// the merger writes kDepthToken in the depth slot (never a literal D24 / D32S8), and the loader (ParseSeedLines) swaps it for the device's depth format and drops a record whose colour
// format the device cannot render to; no vendor / device check applies. The names are kept: a prebuilt program takes its record's name for the whole
// session (renderer.cpp store_names), a nameless seed would make every seeded program nameless in logs and colour-cut plants.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "pipeline_store.h"

namespace rdr::pstore {

// The depth slot (PassData::formats[kMaxColorTargets]) of a seed record: a token the loader replaces with the device's depth format. Outside Sane's VkFormat range (> 184), so it can
// never reach Vulkan: ParseSeedLines swaps it before a record leaves the parser, and a player's own list holding it is refused by Sane.
constexpr uint16_t kDepthToken = 0xFFFF;

inline bool HexDigit(char c, uint8_t& v) {
  if (c >= '0' && c <= '9') v = uint8_t(c - '0');
  else if (c >= 'a' && c <= 'f') v = uint8_t(c - 'a' + 10);
  else if (c >= 'A' && c <= 'F') v = uint8_t(c - 'A' + 10);
  else return false;
  return true;
}

inline std::string HexOfRecord(const Record& r) {
  Writer w;
  PutRecord(w, r);
  static const char* d = "0123456789abcdef";
  std::string s;
  for (uint8_t b : w.bytes) s += d[b >> 4], s += d[b & 15];
  return s;
}

// One `R` line's hex -> a record; "" on success, else why it is refused. The range checks catch a digit that leaves a field out of range; a flip that stays in range is caught by the
// H rdr line's checksum of the R bytes (ParseSeedLines refuses the whole seed). The depth slot must be kDepthToken (has_depth) or 0 and is left as the token: the caller swaps it.
inline std::string RecordFromHex(const std::string& hex, Record& r) {
  if (hex.size() != kRecordBytes * 2) return "wrong length " + std::to_string(hex.size());
  std::vector<uint8_t> bytes(kRecordBytes);
  for (size_t i = 0; i < kRecordBytes; ++i) {
    uint8_t hi = 0, lo = 0;
    if (!HexDigit(hex[2 * i], hi) || !HexDigit(hex[2 * i + 1], lo)) return "not hex at digit " + std::to_string(2 * i);
    bytes[i] = uint8_t(hi << 4 | lo);
  }
  Reader rd{bytes.data(), bytes.size()};
  r = Record{};
  if (!GetRecord(rd, r)) return "unreadable";
  r.name[kNameBytes - 1] = 0;
  r.seen = 0;
  r.seed = true;
  const uint16_t depth_slot = r.pass.formats[kMaxColorTargets];
  if (r.pass.has_depth ? depth_slot != kDepthToken : depth_slot != 0) return "depth slot is not the device-neutral token (a literal depth format is device-specific)";
  r.pass.formats[kMaxColorTargets] = 0;   // Sane sees the slot as an ordinary format; the token goes back after
  const bool sane = Sane(r);
  r.pass.formats[kMaxColorTargets] = depth_slot;
  if (!sane) return "failed the range checks";
  return {};
}

struct SeedParse {
  uint32_t lines = 0;        // R lines seen
  uint32_t refused = 0;      // lines that failed (bad hex / length / range check), or all of them when the header refused the seed
  uint32_t multiview = 0;    // dropped: a multiview class on a device without multiview
  uint32_t unsupported = 0;  // dropped: a colour format the device cannot render to
  std::string first_refusal;
  std::string header_refusal;  // non-empty = the whole seed was refused (no / wrong H rdr line, wrong version, count or checksum): out is empty
};

// FNV over every R line's hex text (lower case, one '\n' each): the H rdr line's 4th number. tools\renderer\pipeline_seed.py writes and checks the same value.
inline uint64_t SeedChecksum(const std::vector<std::string>& hexes) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (const std::string& hx : hexes) {
    for (char c : hx) h = (h ^ uint8_t(c >= 'A' && c <= 'F' ? c - 'A' + 'a' : c)) * 0x100000001b3ull;
    h = (h ^ uint8_t('\n')) * 0x100000001b3ull;
  }
  return h;
}

// Reads the R lines out of a shader_list.txt body; every other line type is ignored (the SDK importer ignores R and the 5-word H rdr in turn: shader_list.h Import). With R lines present the
// `H rdr <version> <count> <checksum>` line must exist and match (kListVersion, the R count, SeedChecksum), or the whole seed is refused. The depth token becomes `depth_format`
// (the device's); `color_ok` (empty = every format) says whether the device can render to a colour format: a record with one it cannot is dropped, never built.
inline SeedParse ParseSeedLines(const std::string& text, std::vector<Record>& out, bool device_multiview, uint16_t depth_format, const std::function<bool(uint16_t)>& color_ok = {}) {
  SeedParse sp;
  std::vector<std::string> hexes;
  bool have_header = false;
  std::string header_line;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t e = text.find('\n', pos);
    if (e == std::string::npos) e = text.size();
    const size_t b = pos;
    pos = e + 1;
    while (e > b && (text[e - 1] == '\r' || text[e - 1] == ' ')) --e;
    if (e - b >= 6 && text.compare(b, 6, "H rdr ") == 0) {
      if (!have_header) header_line = text.substr(b, e - b);
      have_header = true;
      continue;
    }
    if (e - b < 2 || text[b] != 'R' || text[b + 1] != ' ') continue;
    size_t h = b + 2;
    while (h < e && text[h] == ' ') ++h;
    hexes.push_back(text.substr(h, e - h));
  }
  sp.lines = uint32_t(hexes.size());
  if (hexes.empty()) return sp;
  {   // the header: version, count, checksum
    unsigned version = 0, count = 0;
    unsigned long long crc = 0;
    if (!have_header || std::sscanf(header_line.c_str(), "H rdr %u %u %llx", &version, &count, &crc) != 3) sp.header_refusal = "no readable H rdr <version> <count> <checksum> line";
    else if (depth_format == 0) sp.header_refusal = "the device's depth format is 0 (none found): a record with a depth attachment cannot be built";   // S106 PSEEDF2 N9
    else if (version != kListVersion) sp.header_refusal = "H rdr version " + std::to_string(version) + ", this build reads " + std::to_string(kListVersion);
    else if (count != hexes.size()) sp.header_refusal = "H rdr count " + std::to_string(count) + " but " + std::to_string(hexes.size()) + " R lines";
    else if (uint64_t(crc) != SeedChecksum(hexes)) sp.header_refusal = "H rdr checksum does not match the R lines (edited or merged by hand)";
    if (!sp.header_refusal.empty()) {
      sp.refused = sp.lines;
      sp.first_refusal = sp.header_refusal;
      return sp;
    }
  }
  uint32_t no = 0;
  for (const std::string& hx : hexes) {
    ++no;
    Record r;
    const std::string why = RecordFromHex(hx, r);
    if (!why.empty()) {
      if (!sp.refused++) sp.first_refusal = "line " + std::to_string(no) + ": " + why;
      continue;
    }
    if (r.pass.pad > 1 && !device_multiview) {
      ++sp.multiview;
      continue;
    }
    bool colors_ok = true;
    if (color_ok)
      for (uint32_t c = 0; c < r.pass.color_count; ++c)
        if (!color_ok(r.pass.formats[c])) colors_ok = false;
    if (!colors_ok) {
      ++sp.unsupported;
      continue;
    }
    if (r.pass.has_depth) r.pass.formats[kMaxColorTargets] = depth_format;
    out.push_back(r);
  }
  return sp;
}

struct SeedMerge {
  uint32_t added = 0, known = 0;
};

// Adds the seed records whose RecordKey the list does not hold yet; the player's own records keep their order, `seen` and names. Added records are pending and marked
// `seed`. `index` is the RecordKey -> position map of `records`.
inline SeedMerge MergeSeed(std::vector<Record>& records, std::unordered_map<uint64_t, uint32_t>& index, std::vector<uint32_t>& pending, const std::vector<Record>& seed) {
  SeedMerge m;
  for (const Record& r : seed) {
    const uint64_t key = RecordKey(r);
    if (index.count(key)) {
      ++m.known;
      continue;
    }
    index.emplace(key, uint32_t(records.size()));
    pending.push_back(uint32_t(records.size()));
    records.push_back(r);
    ++m.added;
  }
  return m;
}

// Whether StoreSave writes this record: a seed record nobody drew stays out of the player's list (the seed is re-read at each boot).
inline bool SaveKeeps(const Record& r) { return !r.dead && !(r.seed && r.seen == 0); }

}  // namespace rdr::pstore
