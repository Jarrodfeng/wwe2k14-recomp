// jboy_parts: a READ-ONLY view of a JBOY model blob's mesh records, for tools that must reason about a model's PARTS without the loader's
// conversion (Session 101, lane 42; docs\renderer\WRESTLER_PROMOTION_MAP.md lane W0). Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
// assets::jboy.h's LoadJboy flattens a pac entry into viewer-space meshes (a Y mirror, a strip -> list conversion that drops degenerates, bones
// merged by name across pieces); a stream draw is paired with the file's own mesh RECORD, one to one, so this header exposes exactly that:
//   per mesh record (index = the game's part index): vertex count, the bone palette (1-based node numbers as stored), the strip index groups
//   as stored (BE u16), the four vertex streams as pointers into the blob, the effect name; and the node table's names.
// It never copies vertex data and never converts anything. Layout facts are those of jboy.h's header comment (verified there on ch107 / bg05).
// The blob must outlive the Parts. No SDK, no game includes; pac.h's Be32/Be16 helpers only.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "pac.h"

namespace assets::jboy_parts {

struct Part {
  uint32_t mesh_index = 0;          // the record's index in the mesh table = the game's part index (claim to verify against the stream)
  uint32_t vertices = 0;
  uint32_t palette_count = 0;       // record +0x08, clamped to 20
  uint32_t palette[20] = {};        // record +0x0C..: 1-based node numbers, as stored (slot k of the palette -> node palette[k] - 1)
  uint32_t index_count = 0;         // total over the groups
  uint32_t index_groups = 0;
  const uint8_t* index_data = nullptr;  // group 0's BE u16 indices (all groups' counts sum to index_count; see GroupIndices)
  struct Group { uint32_t count = 0; const uint8_t* data = nullptr; };
  std::vector<Group> groups;
  uint32_t stride[4] = {28, 0, 0, 0};   // bytes per vertex of S0..S3
  const uint8_t* s[4] = {};             // S0..S3 first vertex (S0 = the stored offset + 4)
  bool tiles = false;               // the four streams tile exactly (the loader skips a record that does not)
  bool ok = false;                  // vertex count sane, streams and index block inside the body
  std::string effect;               // material class name (+0x7C)
};

struct Parts {
  std::vector<Part> parts;
  uint32_t node_count = 0;
  std::vector<std::string> node_names;  // the node table's names (16 bytes each), in file order
};

inline std::string CStr(const uint8_t* p, size_t n) {
  size_t l = 0;
  while (l < n && p[l]) ++l;
  return std::string(reinterpret_cast<const char*>(p), l);
}

// Parses the mesh and node tables of a decoded JBOY entry. False + err on a structural failure; a record the loader would skip is still
// listed (ok / tiles say what is wrong with it) so part indices stay equal to record indices.
inline bool Parse(const Bytes& blob, Parts& out, std::string& err) {
  out = {};
  if (blob.size() < 0x48 || std::memcmp(blob.data(), "JBOY", 4) != 0) return err = "not a JBOY", false;
  const uint32_t body_len = Be32(blob.data() + 4);
  if (body_len < 0x40 || size_t(body_len) + 8 > blob.size()) return err = "JBOY size past the entry", false;
  const uint8_t* b = blob.data() + 8;
  const size_t n = body_len;
  auto ok = [&](uint64_t off, uint64_t len) { return off <= n && len <= n - off; };
  auto u32 = [&](uint64_t off) { return ok(off, 4) ? Be32(b + off) : 0u; };
  const uint32_t nmesh = u32(0x10), mesh_tab = u32(0x14), nnode = u32(0x18), node_tab = u32(0x20);
  if (nmesh > 512 || !ok(mesh_tab, uint64_t(nmesh) * 0xB8)) return err = "mesh table out of range", false;
  if (nnode > 4096 || !ok(node_tab, uint64_t(nnode) * 80)) return err = "node table out of range", false;
  out.node_count = nnode;
  for (uint32_t i = 0; i < nnode; ++i) out.node_names.push_back(CStr(b + node_tab + 80ull * i, 16));
  for (uint32_t mi = 0; mi < nmesh; ++mi) {
    Part p;
    const uint32_t rec = mesh_tab + mi * 0xB8;
    p.mesh_index = mi;
    p.vertices = u32(rec);
    p.palette_count = std::min<uint32_t>(u32(rec + 8), 20);
    for (uint32_t k = 0; k < 20; ++k) p.palette[k] = u32(rec + 0x0C + 4 * k);
    p.effect = CStr(b + rec + 0x7C, 16);
    const uint32_t N = p.vertices, s0 = u32(rec + 0x68), s1 = u32(rec + 0x6C), s2 = u32(rec + 0x70), s3 = u32(rec + 0x74), ib = u32(rec + 0x9C);
    const uint64_t s0d = uint64_t(s0) + 4;
    bool tiles = N > 0 && N <= 400000 && s0d + uint64_t(N) * 28 == s1;
    const uint64_t d1 = s2 > s1 ? s2 - s1 : 0, d2 = s3 > s2 ? s3 - s2 : 0, d3 = ib > s3 ? ib - s3 : 0;
    if (N) {
      p.stride[1] = uint32_t(d1 / N), p.stride[2] = uint32_t(d2 / N), p.stride[3] = uint32_t(d3 / N);
      if (d1 % N || d2 % N || d3 % N) tiles = false;
      if (p.stride[1] != 8 && p.stride[1] != 16 && p.stride[1] != 24 && p.stride[1] != 32) tiles = false;
      if (p.stride[2] != 8) tiles = false;
      if (p.stride[3] != 0 && p.stride[3] != 12) tiles = false;
    }
    p.tiles = tiles;
    bool good = N > 0 && N <= 400000 && ok(s0d, uint64_t(N) * 28) && ok(s1, d1) && ok(s2, d2) && ok(s3, d3) && ok(ib, 12);
    if (good) {
      p.s[0] = b + s0d, p.s[1] = b + s1, p.s[2] = b + s2, p.s[3] = b + s3;
      const uint32_t first_ptr = u32(ib + 8);
      if (first_ptr < ib + 12 || (first_ptr - ib) % 12 != 0 || (first_ptr - ib) / 12 > 64 || !ok(ib, first_ptr - ib)) good = false;
      else {
        const uint32_t ng = (first_ptr - ib) / 12;
        for (uint32_t g = 0; g < ng && good; ++g) {
          const uint32_t cnt = u32(ib + 12 * g + 4), ptr = u32(ib + 12 * g + 8);
          if (!ok(ptr, uint64_t(cnt) * 2)) { good = false; break; }
          p.groups.push_back({cnt, b + ptr});
          p.index_count += cnt;
        }
        p.index_groups = ng;
        if (good && !p.groups.empty()) p.index_data = p.groups[0].data;
      }
    }
    p.ok = good;
    out.parts.push_back(std::move(p));
  }
  return true;
}

// The part's indices, all groups concatenated in order (host order). Empty when the record is not ok.
inline std::vector<uint16_t> AllIndices(const Part& p) {
  std::vector<uint16_t> r;
  for (const auto& g : p.groups)
    for (uint32_t i = 0; i < g.count; ++i) r.push_back(Be16(g.data + 2ull * i));
  return r;
}

}  // namespace assets::jboy_parts
