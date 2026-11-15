// S102 R21-A: the scene record's arena object ids ("arena:slot:K" and the twin "arena:slot:K:r...") classified without a std::string or sscanf.
// ParseArenaId gives, for every id our writer makes (ASCII, an int-range number: arena_contrib.cpp is the only writer, no setlocale), exactly what the old code's sscanf(id, "arena:slot:%d%7s", &k, tail) + the prefix test gave: n = 0 (not an arena id), 1 (a slot), 2 (a slot with a tail).
// A record's string offsets are per record (SceneTables::AddString appends in the writer's order), so an id's text is not a key across frames: no cache, the byte test is the cost
// (verified in record_io.cpp AddString / Str: an offset is the position in THIS record's string blob).
#pragma once
#include <cstdint>
#include <string_view>

namespace rdr_scene {

struct ArenaId {
  int n = 0;          // 0: not "arena:slot:<int>..."; 1: a plain slot; 2: a slot with a tail after the number
  int k = 0;          // the slot number
  char tail[8] = {};  // the first 7 non-space characters after the number (sscanf %7s)
};

inline bool IsSpaceC(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// `short_prefix`: the planted fault (the prefix test stops after "arena:slo").
inline ArenaId ParseArenaId(std::string_view id, bool short_prefix = false) {
  ArenaId r;
  static constexpr char kPrefix[] = "arena:slot:";
  constexpr size_t kFull = sizeof(kPrefix) - 1;
  const size_t plen = short_prefix ? 9 : kFull;
  if (id.size() <= plen) return r;
  for (size_t i = 0; i < plen; ++i)
    if (id[i] != kPrefix[i]) return r;
  size_t p = kFull;
  while (p < id.size() && IsSpaceC(id[p])) ++p;
  bool neg = false;
  if (p < id.size() && (id[p] == '+' || id[p] == '-')) neg = id[p] == '-', ++p;
  const size_t d0 = p;
  uint32_t v = 0;
  while (p < id.size() && id[p] >= '0' && id[p] <= '9') {
    v = v * 10u + uint32_t(id[p] - '0');   // wraps mod 2^32 like the UCRT's %d
    ++p;
  }
  if (p == d0) return r;   // no digits: sscanf stopped before %d (n 0)
  r.k = int(neg ? 0u - v : v);
  r.n = 1;
  while (p < id.size() && IsSpaceC(id[p])) ++p;
  if (p >= id.size()) return r;
  int t = 0;
  while (p < id.size() && !IsSpaceC(id[p]) && t < 7) r.tail[t++] = id[p++];
  r.n = 2;
  return r;
}

}  // namespace rdr_scene
